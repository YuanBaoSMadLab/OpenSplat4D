#include "OpenSplat4DHLODBuilder.h"
#include "OpenSplat4DPointCloud.h"
#include "OpenSplat4DPointCloudActor.h"
#include "OpenSplat4DEditorLibrary.h"
#include "NiagaraComponent.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Components/StaticMeshComponent.h"
#include "NiagaraComponent.h"
#include "LandscapeComponent.h"

#include "WorldPartition/HLOD/HLODHashBuilder.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/SkyLight.h"
#include "Components/SkyLightComponent.h"
#include "Engine/Texture.h"
#include "AssetCompilingManager.h"
#include "ShaderCompiler.h"

UOpenSplat4DHLODBuilderSettings::UOpenSplat4DHLODBuilderSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		CaptureSettings = CreateDefaultSubobject<UOpenSplat4DStep_Capture>(TEXT("CaptureSettings"));
		SparseReconstructionSettings = CreateDefaultSubobject<UOpenSplat4DStep_SparseReconstruction>(TEXT("SparseReconstructionSettings"));
		GaussianSplattingSettings = CreateDefaultSubobject<UOpenSplat4DStep_GaussianSplatting>(TEXT("GaussianSplattingSettings"));
	}
}

void UOpenSplat4DHLODBuilderSettings::ComputeHLODHash(FHLODHashBuilder& InHashBuilder) const
{
	// Hash the three pipeline step settings so editing capture / reconstruction / training
	// parameters triggers an HLOD rebuild.
	CaptureSettings->SerializeScriptProperties(InHashBuilder);
	SparseReconstructionSettings->SerializeScriptProperties(InHashBuilder);
	GaussianSplattingSettings->SerializeScriptProperties(InHashBuilder);

	// The 3DGS vs 4DGS mode produces a different asset, so it must affect the hash.
	InHashBuilder.HashField(bTrain4D, GET_MEMBER_NAME_CHECKED(UOpenSplat4DHLODBuilderSettings, bTrain4D));
}

TSubclassOf<UHLODBuilderSettings> UOpenSplat4DHLODBuilder::GetSettingsClass() const
{
	return UOpenSplat4DHLODBuilderSettings::StaticClass();
}

TArray<UActorComponent*> UOpenSplat4DHLODBuilder::Build(const FHLODBuildContext& InHLODBuildContext, const TArray<UActorComponent*>& InSourceComponents) const
{
	const UOpenSplat4DHLODBuilderSettings* Settings = Cast<UOpenSplat4DHLODBuilderSettings>(HLODBuilderSettings);
	UWorld* World = InHLODBuildContext.TargetWorld;
	if (!Settings || !World)
	{
		return {};
	}

	// Skip runtime / distant-landscape proxies ("_DL" cells) — nothing to capture there.
	if (InHLODBuildContext.AssetsBaseName.Contains(TEXT("_DL")))
	{
		return {};
	}

	// --- Pre-build stabilization (mirrors the reference GaussianSplatting HLOD builder) ---
	// World Partition builds HLOD cells in isolation; make sure the scene is fully streamed,
	// assets/textures/shaders are ready, and memory is reclaimed before we capture, otherwise
	// captures can be empty or non-deterministic.
	const int32 GarbageCollectionFrequency = 10;
	static int32 BuildCounter = 0;
	World->FlushLevelStreaming(EFlushLevelStreamingType::Visibility);

	if (FApp::CanEverRender())
	{
		FAssetCompilingManager::Get().FinishAllCompilation();
		FAssetCompilingManager::Get().ProcessAsyncTasks();
		UTexture::ForceUpdateTextureStreaming();
		IStreamingManager::Get().StreamAllResources();
		if (GShaderCompilingManager && GShaderCompilingManager->GetNumRemainingJobs() > 0)
		{
			GShaderCompilingManager->FinishAllCompilation();
		}
	}

	if (!GarbageCollectionFrequency || BuildCounter++ % GarbageCollectionFrequency == 0)
	{
		UE_LOG(LogOpenSplat4DStep, Warning, TEXT("OpenSplat4D HLOD: pre-build CollectGarbage"));
		GEngine->ForceGarbageCollection();
	}

	// Recapture the sky once so environment lighting is baked into the captured frames.
	static bool bNeedRecaptureSky = true;
	if (bNeedRecaptureSky)
	{
		if (ASkyLight* SkyLight = Cast<ASkyLight>(UGameplayStatics::GetActorOfClass(World, ASkyLight::StaticClass())))
		{
			if (USkyLightComponent* SkyLightComp = SkyLight->GetLightComponent())
			{
				SkyLightComp->MarkRenderStateDirty();
				SkyLightComp->RecaptureSky();
			}
		}
		bNeedRecaptureSky = false;
	}

	const FString WorkDir = FPaths::Combine(FPaths::GetPath(InHLODBuildContext.AssetsBaseName), TEXT("OpenSplat4DWork"));
	const FString PlyPath = FString::Printf(TEXT("%s/output/point_cloud/iteration_%d/point_cloud.ply"),
		*WorkDir, Settings->GaussianSplattingSettings->Iterations);
	const bool bUseCache = FParse::Param(FCommandLine::Get(), TEXT("UseCache"));

	Settings->CaptureSettings->SetWorld(World);
	Settings->SparseReconstructionSettings->SetWorld(World);
	Settings->GaussianSplattingSettings->SetWorld(World);

	Settings->CaptureSettings->Activate();
	Settings->CaptureSettings->SetWorkDir(WorkDir);
	Settings->SparseReconstructionSettings->SetWorkDir(WorkDir);
	Settings->GaussianSplattingSettings->SetWorkDir(WorkDir);

	Settings->CaptureSettings->SourceMode = EOpenSplat4DCaptureSourceMode::Select;
	Settings->CaptureSettings->SetSelectionByComponents(InSourceComponents);

	if (!(FPaths::FileExists(PlyPath) && bUseCache))
	{
		Settings->CaptureSettings->Capture();
		Settings->SparseReconstructionSettings->ReconstructionSparse(false);
		// Honor the dual-mode switch: train a dynamic 4DGS when requested.
		Settings->GaussianSplattingSettings->bTrain4D = Settings->bTrain4D;
		Settings->GaussianSplattingSettings->Train(false);
	}
	else
	{
		UE_LOG(LogOpenSplat4DStep, Warning, TEXT("Use Cache Ply : %s"), *PlyPath);
	}

	// Collect the source asset names for the meta json (mirrors reference).
	TSet<FString> SourceAssets;
	for (UActorComponent* SourceComponent : InSourceComponents)
	{
		if (UStaticMeshComponent* StaticMeshComp = Cast<UStaticMeshComponent>(SourceComponent))
		{
			if (UStaticMesh* StaticMesh = StaticMeshComp->GetStaticMesh())
			{
				SourceAssets.Add(StaticMesh->GetName());
			}
		}
		else if (ULandscapeComponent* LandscapeComp = Cast<ULandscapeComponent>(SourceComponent))
		{
			SourceAssets.Add(LandscapeComp->GetOwner()->GetActorLabel());
		}
		else
		{
			SourceAssets.Add(SourceComponent->GetName());
		}
	}

	const FBoxSphereBounds Bounds = Settings->CaptureSettings->CurrentBounds;
	const FString InfoPath = FString::Printf(TEXT("%s/output/point_cloud/iteration_%d/point_cloud_meta.json"),
		*WorkDir, Settings->GaussianSplattingSettings->Iterations);
	TSharedRef<FJsonObject> WriteJsonObject = MakeShared<FJsonObject>();
	WriteJsonObject->SetStringField(TEXT("Location"), Bounds.Origin.ToString());
	WriteJsonObject->SetStringField(TEXT("BoxExtent"), Bounds.BoxExtent.ToString());
	WriteJsonObject->SetBoolField(TEXT("bTrain4D"), Settings->bTrain4D);
	TArray<TSharedPtr<FJsonValue>> SourceArr;
	for (const FString& S : SourceAssets)
	{
		SourceArr.Add(MakeShared<FJsonValueString>(S));
	}
	WriteJsonObject->SetArrayField(TEXT("SourceAssets"), SourceArr);
	FString JsonString;
	TSharedRef<TJsonWriter<>> JsonWriter = TJsonWriterFactory<>::Create(&JsonString);
	if (FJsonSerializer::Serialize(WriteJsonObject, JsonWriter) && JsonWriter->Close())
	{
		FFileHelper::SaveStringToFile(JsonString, *InfoPath);
	}

	// Optionally clip by mask, then load the trained ply into a point-cloud asset.
	FString LoadPlyPath = PlyPath;
	if (Settings->GaussianSplattingSettings->bClippingByMask)
	{
		LoadPlyPath = Settings->GaussianSplattingSettings->Clip(PlyPath);
	}
	UOpenSplat4DPointCloud* PointCloud = UOpenSplat4DEditorLibrary::LoadSplatFile(
		LoadPlyPath, InHLODBuildContext.AssetsOuter, FName(*InHLODBuildContext.AssetsBaseName));
	if (PointCloud != nullptr)
	{
		AOpenSplat4DPointCloudActor* Actor = InHLODBuildContext.AssetsOuter->GetWorld()->SpawnActor<AOpenSplat4DPointCloudActor>(
			AOpenSplat4DPointCloudActor::StaticClass(), Bounds.Origin, FRotator::ZeroRotator);
		Actor->SetPointCloud(PointCloud);
		Actor->bAutoPlay = false;
		TArray<UActorComponent*> Components;
		if (UActorComponent* NiagaraComp = Actor->FindComponentByClass<UNiagaraComponent>())
		{
			Components.Add(NiagaraComp);
		}
		return Components;
	}
	return {};
}
