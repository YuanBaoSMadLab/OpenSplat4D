// Copyright Epic Games, Inc. All Rights Reserved.
// Copyright OpenSplat4D 2026 All Rights Reserved.

#include "GaussianSplatAssetViewport.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatFractureAsset.h"
#include "NanoGSGaussianSplatActor.h"
#include "NanoGSGaussianSplatComponent.h"
#include "AdvancedPreviewScene.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "PrimitiveDrawInterface.h"
#include "SceneView.h"
// 真实 Chaos 破碎预览：预览场景里现搭几何集合（Editor-only 依赖）。
#include "GeometryCollection/GeometryCollectionActor.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "GeometryCollection/GeometryCollectionObject.h"

#define LOCTEXT_NAMESPACE "GaussianSplatAssetViewport"

//////////////////////////////////////////////////////////////////////////
// FGaussianSplatAssetViewportClient
//////////////////////////////////////////////////////////////////////////

FGaussianSplatAssetViewportClient::FGaussianSplatAssetViewportClient(FPreviewScene* InPreviewScene)
	: FEditorViewportClient(nullptr, nullptr, nullptr)
{
	// Create an advanced preview scene for proper lighting and environment
	FAdvancedPreviewScene::ConstructionValues ConstructionValues;
	ConstructionValues.LightBrightness = 3.0f;
	ConstructionValues.SkyBrightness = 1.0f;

	PreviewScene = MakeShareable(new FAdvancedPreviewScene(ConstructionValues));

	// Connect preview scene to the viewport client (protected member of FEditorViewportClient)
	// This ensures the viewport renders the preview scene, not the main world
	FEditorViewportClient::PreviewScene = PreviewScene.Get();

	// Set default camera position
	SetViewLocation(FVector(0, 0, 100));
	SetViewRotation(FRotator(-15.0f, 0.0f, 0.0f));
}

FGaussianSplatAssetViewportClient::~FGaussianSplatAssetViewportClient()
{
	DestroyChaosPreviewCollection();

	if (PreviewActor.IsValid())
	{
		UWorld* PreviewWorld = PreviewScene.IsValid() ? PreviewScene->GetWorld() : nullptr;
		if (PreviewWorld)
		{
			PreviewWorld->DestroyActor(PreviewActor.Get(), false, false);
		}
		PreviewActor.Reset();
	}
}

void FGaussianSplatAssetViewportClient::Tick(float DeltaSeconds)
{
	FEditorViewportClient::Tick(DeltaSeconds);

	// Tick the preview scene's world
	if (PreviewScene.IsValid())
	{
		UWorld* PreviewWorld = PreviewScene->GetWorld();
		if (PreviewWorld)
		{
			PreviewWorld->Tick(LEVELTICK_All, DeltaSeconds);
		}
	}

	// The fracture previews are driven from here (not from a component tick): the
	// lightweight one pushes deltas straight to the scene proxy, the Chaos one
	// owns a geometry collection it spawned into the preview world.
	if (bFracturePreviewActive)
	{
		if (bFracturePreviewChaos)
		{
			UpdateChaosFracturePreview(DeltaSeconds);
		}
		else
		{
			UpdateFracturePreview(DeltaSeconds);
		}
	}
}

void FGaussianSplatAssetViewportClient::SetSplatAsset(UGaussianSplatAsset* InAsset, bool bFrameAsset)
{
	CurrentAsset = InAsset;

	if (!PreviewScene.IsValid())
	{
		return;
	}

	UWorld* PreviewWorld = PreviewScene->GetWorld();
	if (!PreviewWorld)
	{
		return;
	}

	// Destroy existing preview actor
	if (PreviewActor.IsValid())
	{
		PreviewWorld->DestroyActor(PreviewActor.Get(), false, false);
		PreviewActor.Reset();
	}

	// The Chaos preview's geometry collection only exists to drive the preview
	// component, which is gone now.
	DestroyChaosPreviewCollection();

	// The actor is gone, so the deltas pushed to its proxy are gone with it. The
	// caller (fracture editor) restarts the preview after the rebuild.
	bFracturePreviewActive = false;

	if (!InAsset || InAsset->GetSplatCount() <= 0)
	{
		return;
	}

	// Spawn a preview actor in the preview world
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.bNoFail = true;

	AGaussianSplatActor* NewActor = PreviewWorld->SpawnActor<AGaussianSplatActor>(
		AGaussianSplatActor::StaticClass(),
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		SpawnParams);

	if (NewActor && NewActor->GaussianSplatComponent)
	{
		NewActor->GaussianSplatComponent->SetSplatAsset(InAsset);

		// Apply the asset's preview performance settings to the preview component
		// so users can tune Nanite precision / visibility range in the asset editor.
		NewActor->GaussianSplatComponent->LODErrorThreshold = InAsset->PreviewNanitePrecision;
		NewActor->GaussianSplatComponent->MaxDrawDistance = InAsset->PreviewMaxDrawDistance;
		NewActor->GaussianSplatComponent->FadeOutStartDistance = InAsset->PreviewFadeOutStartDistance;

		// 4D playback preview: start/pause according to the asset's preview
		// settings (only takes effect on assets that contain temporal data).
		NewActor->GaussianSplatComponent->bAutoPlay = InAsset->PreviewAutoPlay;
		NewActor->GaussianSplatComponent->PlayRate = InAsset->PreviewPlayRate;
		if (InAsset->PreviewAutoPlay)
		{
			NewActor->GaussianSplatComponent->Play4D();
		}
		else
		{
			NewActor->GaussianSplatComponent->Pause4D();
		}

		PreviewActor = NewActor;

		// Adjust camera to frame the asset (skipped on refresh rebuilds to keep the user's view)
		if (bFrameAsset)
		{
			FBox Bounds = InAsset->GetBounds();
			if (Bounds.IsValid)
			{
				FVector Center = Bounds.GetCenter();
				float Extent = Bounds.GetExtent().Length();
				if (Extent > 0.0f)
				{
					// Position camera to view the asset
					FVector CameraOffset(Extent * 1.5f, Extent * 1.5f, Extent * 0.8f);
					SetViewLocation(Center + CameraOffset);
					SetViewRotation((Center - (Center + CameraOffset)).Rotation());
				}
			}
		}

		UE_LOG(LogTemp, Log, TEXT("GaussianSplatAssetViewport: Preview actor created for asset '%s' (%d splats)"),
			*InAsset->GetName(), InAsset->GetSplatCount());
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("GaussianSplatAssetViewport: Failed to create preview actor"));
	}
}

void FGaussianSplatAssetViewportClient::SetCuttingPreview(const TArray<FVector>& InSegments, const TArray<FVector>& InSites)
{
	CuttingPreviewSegments = InSegments;
	CuttingPreviewSites = InSites;
	Invalidate();
}

void FGaussianSplatAssetViewportClient::ClearCuttingPreview()
{
	if (CuttingPreviewSegments.Num() == 0 && CuttingPreviewSites.Num() == 0)
	{
		return;
	}

	CuttingPreviewSegments.Reset();
	CuttingPreviewSites.Reset();
	Invalidate();
}

void FGaussianSplatAssetViewportClient::SetFractureAsset(UGaussianSplatFractureAsset* InFractureAsset)
{
	CurrentFractureAsset = InFractureAsset;

	if (AGaussianSplatActor* Actor = PreviewActor.Get())
	{
		if (UGaussianSplatComponent* Component = Actor->GaussianSplatComponent)
		{
			// Switches the render data from the bare source asset to
			// source + bone binding + caps (see the component's SetFractureAsset).
			Component->SetFractureAsset(InFractureAsset);
		}
	}
}

void FGaussianSplatAssetViewportClient::StartFracturePreview(const TArray<FMatrix44f>& InRestBoneMatrices,
	const FVector& InBodyCenter, bool bInGravity, float InExplosionSpeed, const FVector& InCenterOffset)
{
	// Switching from the Chaos form (or restarting) must release the collection first.
	StopChaosFracturePreview();

	FracturePreviewRestBones = InRestBoneMatrices;
	FracturePreviewBodyCenter = InBodyCenter;
	bFracturePreviewGravity = bInGravity;
	FracturePreviewSpeed = FMath::Max(InExplosionSpeed, 0.0f);
	FracturePreviewCenterOffset = InCenterOffset;
	FracturePreviewTime = 0.0f;

	bFracturePreviewActive = FracturePreviewRestBones.Num() > 1;

	if (!bFracturePreviewActive)
	{
		// Nothing to fly apart (needs at least one piece beyond the root bone).
		StopFracturePreview();
		return;
	}

	UpdateFracturePreview(0.0f);
}

void FGaussianSplatAssetViewportClient::StopFracturePreview()
{
	bFracturePreviewActive = false;
	FracturePreviewTime = 0.0f;
	FracturePreviewRestBones.Reset();
	FracturePreviewDeltas.Reset();

	StopChaosFracturePreview();

	if (AGaussianSplatActor* Actor = PreviewActor.Get())
	{
		if (UGaussianSplatComponent* Component = Actor->GaussianSplatComponent)
		{
			// Empty array = bone path off, pieces back at rest.
			Component->SetPreviewBoneDeltas(TArray<FMatrix44f>());
		}
	}

	Invalidate();
}

void FGaussianSplatAssetViewportClient::UpdateFracturePreview(float DeltaSeconds)
{
	AGaussianSplatActor* Actor = PreviewActor.Get();
	UGaussianSplatComponent* Component = Actor ? Actor->GaussianSplatComponent : nullptr;
	if (!Component)
	{
		bFracturePreviewActive = false;
		return;
	}

	FracturePreviewTime += DeltaSeconds;

	const FVector BlastCenter = FracturePreviewBodyCenter + FracturePreviewCenterOffset;

	// Farther pieces fly faster (falls off towards the blast center), so the body
	// opens up from the middle instead of translating as a whole. The farthest
	// piece moves at exactly FracturePreviewSpeed.
	float MaxDistance = 0.0f;
	for (int32 BoneIndex = 1; BoneIndex < FracturePreviewRestBones.Num(); ++BoneIndex)
	{
		MaxDistance = FMath::Max(MaxDistance,
			(FVector(FracturePreviewRestBones[BoneIndex].GetOrigin()) - BlastCenter).Size());
	}
	const float InvMaxDistance = MaxDistance > KINDA_SMALL_NUMBER ? 1.0f / MaxDistance : 0.0f;

	FracturePreviewDeltas.SetNumUninitialized(FracturePreviewRestBones.Num());

	for (int32 BoneIndex = 0; BoneIndex < FracturePreviewRestBones.Num(); ++BoneIndex)
	{
		if (BoneIndex == 0)
		{
			// Bone 0 is the collection root: moving it would translate the whole
			// body, which is not what "explode at the object center" means.
			FracturePreviewDeltas[BoneIndex] = FMatrix44f::Identity;
			continue;
		}

		const FVector RestOrigin(FracturePreviewRestBones[BoneIndex].GetOrigin());
		FVector Direction = RestOrigin - BlastCenter;
		const float Distance = Direction.Size();
		if (Distance <= KINDA_SMALL_NUMBER)
		{
			// A piece sitting exactly on the blast center has no direction of its
			// own; pushing it up keeps it moving instead of staying frozen.
			Direction = FVector::UpVector;
		}
		else
		{
			Direction /= Distance;
		}

		const float Speed = FracturePreviewSpeed * FMath::Clamp(Distance * InvMaxDistance, 0.1f, 1.0f);
		FVector Offset = Direction * (Speed * FracturePreviewTime);

		if (bFracturePreviewGravity)
		{
			// UE's default gravity (cm/s^2), same value the Chaos preview would use.
			constexpr float GravityZ = -980.0f;
			Offset.Z += 0.5f * GravityZ * FracturePreviewTime * FracturePreviewTime;
		}

		// Row-vector convention: the translation lives in row 3, which is exactly
		// how the runtime builds Delta_b = Cur_b * Rest_b^-1 and how the shader
		// applies it (mul(float4(pos,1), BoneDelta)).
		FMatrix44f Delta = FMatrix44f::Identity;
		Delta.M[3][0] = (float)Offset.X;
		Delta.M[3][1] = (float)Offset.Y;
		Delta.M[3][2] = (float)Offset.Z;
		FracturePreviewDeltas[BoneIndex] = Delta;
	}

	Component->SetPreviewBoneDeltas(FracturePreviewDeltas);

	// The viewport is not necessarily realtime, so ask for a redraw explicitly.
	Invalidate();
}

// ---------------------------------------------------------------------------
// 真实 Chaos 破碎预览
// ---------------------------------------------------------------------------

void FGaussianSplatAssetViewportClient::StartChaosFracturePreview(const TArray<FMatrix44f>& InRestBoneMatrices,
	const FVector& InBodyCenter, bool bInGravity, float InStrength, const FVector& InCenterOffset)
{
	// Whatever was running before (either preview form) must be released first.
	StopFracturePreview();

	AGaussianSplatActor* Actor = PreviewActor.Get();
	UGaussianSplatComponent* Component = Actor ? Actor->GaussianSplatComponent : nullptr;
	UGaussianSplatFractureAsset* Fracture = CurrentFractureAsset.Get();
	UGeometryCollection* Collection = Fracture ? Cast<UGeometryCollection>(Fracture->GeometryCollection) : nullptr;
	UWorld* PreviewWorld = PreviewScene.IsValid() ? PreviewScene->GetWorld() : nullptr;

	if (!PreviewWorld || !Component || !Collection || InRestBoneMatrices.Num() < 2)
	{
		// Nothing to simulate: no collection, no binding, or no scene to spawn into.
		return;
	}

	// 逐块初速度：方向 = 块中心 - 爆炸中心，越远的块越快（与轻量预览同一套规则）。
	// 骨头索引就是几何集合的 transform 索引（见 FGaussianSplatBoneBinder），
	// 所以这些速度可以直接按骨头索引交给 ApplyLinearVelocity。
	const FVector BlastCenter = InBodyCenter + InCenterOffset;
	float MaxDistance = 0.0f;
	for (int32 BoneIndex = 1; BoneIndex < InRestBoneMatrices.Num(); ++BoneIndex)
	{
		MaxDistance = FMath::Max(MaxDistance,
			(FVector(InRestBoneMatrices[BoneIndex].GetOrigin()) - BlastCenter).Size());
	}
	const float InvMaxDistance = MaxDistance > KINDA_SMALL_NUMBER ? 1.0f / MaxDistance : 0.0f;

	FracturePreviewBlastVelocities.Empty(InRestBoneMatrices.Num());
	FracturePreviewBlastVelocities.SetNumZeroed(InRestBoneMatrices.Num());
	for (int32 BoneIndex = 1; BoneIndex < InRestBoneMatrices.Num(); ++BoneIndex)
	{
		FVector Direction = FVector(InRestBoneMatrices[BoneIndex].GetOrigin()) - BlastCenter;
		const float Distance = Direction.Size();
		if (Distance <= KINDA_SMALL_NUMBER)
		{
			Direction = FVector::UpVector;
		}
		else
		{
			Direction /= Distance;
		}

		FracturePreviewBlastVelocities[BoneIndex] =
			Direction * (FMath::Max(InStrength, 0.0f) * FMath::Clamp(Distance * InvMaxDistance, 0.1f, 1.0f));
	}

	// 现搭一个几何集合 Actor。它和预览 Actor 都摆在原点且不旋转，所以几何集合
	// 的组件空间就是源资产局部空间，骨头 rest 矩阵可以直接用。
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.bNoFail = true;

	AGeometryCollectionActor* CollectionActor = PreviewWorld->SpawnActor<AGeometryCollectionActor>(
		AGeometryCollectionActor::StaticClass(),
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		SpawnParams);

	UGeometryCollectionComponent* CollectionComponent =
		CollectionActor ? CollectionActor->GetGeometryCollectionComponent() : nullptr;

	if (!CollectionComponent)
	{
		if (CollectionActor)
		{
			PreviewWorld->DestroyActor(CollectionActor, false, false);
		}
		FracturePreviewBlastVelocities.Reset();
		return;
	}

	// 只引用破裂资产的几何集合（bApplyAssetDefaults=false：模拟参数一律由这里定）。
	CollectionComponent->SetRestCollection(Collection, /*bApplyAssetDefaults=*/false);
	// 先开模拟：这一步才会建立并注册物理代理，后面设重力/施力才有落点。
	CollectionComponent->SetSimulatePhysics(true);
	CollectionComponent->SetEnableGravity(bInGravity);
	// 预览只让 splat 表现碎片，Chaos 网格自己不出图，免得和点云重叠。
	CollectionComponent->SetVisibility(false, /*bPropagateToChildren=*/false);
	// 解算出来的集合是一个整体簇，不拆开的话每块都不会单独受力。
	CollectionComponent->CrumbleActiveClusters();

	// 预览组件按骨头跟随这个集合（GetCurrentTransforms），这是两边唯一的耦合。
	Component->SetFractureAsset(Fracture);
	Component->SetFractureTargetForPreview(CollectionComponent);

	FracturePreviewCollectionActor = CollectionActor;
	FracturePreviewCollection = CollectionComponent;
	// 拆簇要过一个物理步才生效，冲量等一小会儿再给；给的时候连推几帧，
	// 保证每块都真的被推走（预览，不是物理实验）。
	FracturePreviewBlastDelay = 0.1f;
	FracturePreviewBlastPushTime = 0.25f;
	bFracturePreviewChaos = true;
	bFracturePreviewActive = true;

	Invalidate();
}

void FGaussianSplatAssetViewportClient::UpdateChaosFracturePreview(float DeltaSeconds)
{
	AGaussianSplatActor* Actor = PreviewActor.Get();
	UGaussianSplatComponent* Component = Actor ? Actor->GaussianSplatComponent : nullptr;
	UGeometryCollectionComponent* CollectionComponent = FracturePreviewCollection.Get();

	if (!Component || !CollectionComponent)
	{
		// The collection is gone (asset swapped / world torn down): drop the preview.
		bFracturePreviewActive = false;
		DestroyChaosPreviewCollection();
		return;
	}

	if (FracturePreviewBlastDelay > 0.0f)
	{
		FracturePreviewBlastDelay -= DeltaSeconds;
	}
	else if (FracturePreviewBlastPushTime > 0.0f)
	{
		FracturePreviewBlastPushTime -= DeltaSeconds;

		// 索引 0 是集合的根（整体位移用），爆炸不推它，交给 chaos 自己解算。
		for (int32 BoneIndex = 1; BoneIndex < FracturePreviewBlastVelocities.Num(); ++BoneIndex)
		{
			CollectionComponent->ApplyLinearVelocity(BoneIndex, FracturePreviewBlastVelocities[BoneIndex]);
		}
	}

	// 视口不一定实时，主动请求重绘。
	Invalidate();
}

void FGaussianSplatAssetViewportClient::StopChaosFracturePreview()
{
	if (AGaussianSplatActor* Actor = PreviewActor.Get())
	{
		if (UGaussianSplatComponent* Component = Actor->GaussianSplatComponent)
		{
			// 先停骨头路径（碎片回到 rest），再解除跟随 —— 否则最后一帧的位移
			// 会留在 GPU 上，碎片看起来卡在半空。
			Component->SetPreviewBoneDeltas(TArray<FMatrix44f>());
			Component->SetFractureTargetForPreview(nullptr);
		}
	}

	DestroyChaosPreviewCollection();
}

void FGaussianSplatAssetViewportClient::DestroyChaosPreviewCollection()
{
	if (AActor* CollectionActor = FracturePreviewCollectionActor.Get())
	{
		UWorld* PreviewWorld = PreviewScene.IsValid() ? PreviewScene->GetWorld() : nullptr;
		if (PreviewWorld)
		{
			PreviewWorld->DestroyActor(CollectionActor, false, false);
		}
	}

	FracturePreviewCollectionActor.Reset();
	FracturePreviewCollection.Reset();
	FracturePreviewBlastVelocities.Reset();
	FracturePreviewBlastDelay = 0.0f;
	FracturePreviewBlastPushTime = 0.0f;
	bFracturePreviewChaos = false;
}

void FGaussianSplatAssetViewportClient::Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	// 基类负责拖拽工具 / gizmo / DrawHelper / active modes，必须先调用
	FEditorViewportClient::Draw(View, PDI);

	if (!PDI)
	{
		return;
	}

	const int32 SegmentCount = CuttingPreviewSegments.Num() / 2;
	for (int32 SegmentIndex = 0; SegmentIndex < SegmentCount; ++SegmentIndex)
	{
		PDI->DrawLine(
			CuttingPreviewSegments[SegmentIndex * 2],
			CuttingPreviewSegments[SegmentIndex * 2 + 1],
			FLinearColor(0.95f, 0.55f, 0.20f),
			SDPG_Foreground,
			1.0f);
	}

	for (const FVector& Site : CuttingPreviewSites)
	{
		PDI->DrawPoint(Site, FLinearColor(0.62f, 0.94f, 0.84f), 4.0f, SDPG_Foreground);
	}
}

//////////////////////////////////////////////////////////////////////////
// SGaussianSplatAssetViewport
//////////////////////////////////////////////////////////////////////////

void SGaussianSplatAssetViewport::Construct(const FArguments& InArgs, TSharedPtr<FGaussianSplatAssetViewportClient> InClient)
{
	ViewportClient = InClient;

	SEditorViewport::Construct(
		SEditorViewport::FArguments()
		.IsEnabled(true)
		.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("GaussianSplatAssetEditor.Viewport")))
	);
}

TSharedRef<FEditorViewportClient> SGaussianSplatAssetViewport::MakeEditorViewportClient()
{
	if (ViewportClient.IsValid())
	{
		return ViewportClient.ToSharedRef();
	}

	// Create a default client if none provided
	ViewportClient = MakeShareable(new FGaussianSplatAssetViewportClient());
	return ViewportClient.ToSharedRef();
}

void SGaussianSplatAssetViewport::SetSplatAsset(UGaussianSplatAsset* InAsset, bool bFrameAsset)
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->SetSplatAsset(InAsset, bFrameAsset);
	}
}

AGaussianSplatActor* SGaussianSplatAssetViewport::GetPreviewActor() const
{
	return ViewportClient.IsValid() ? ViewportClient->GetPreviewActor() : nullptr;
}

void SGaussianSplatAssetViewport::SetCuttingPreview(const TArray<FVector>& InSegments, const TArray<FVector>& InSites)
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->SetCuttingPreview(InSegments, InSites);
	}
}

void SGaussianSplatAssetViewport::ClearCuttingPreview()
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->ClearCuttingPreview();
	}
}

void SGaussianSplatAssetViewport::SetFractureAsset(UGaussianSplatFractureAsset* InFractureAsset)
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->SetFractureAsset(InFractureAsset);
	}
}

void SGaussianSplatAssetViewport::StartFracturePreview(const TArray<FMatrix44f>& InRestBoneMatrices,
	const FVector& InBodyCenter, bool bInGravity, float InExplosionSpeed, const FVector& InCenterOffset)
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->StartFracturePreview(InRestBoneMatrices, InBodyCenter, bInGravity, InExplosionSpeed, InCenterOffset);
	}
}

void SGaussianSplatAssetViewport::StopFracturePreview()
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->StopFracturePreview();
	}
}

void SGaussianSplatAssetViewport::StartChaosFracturePreview(const TArray<FMatrix44f>& InRestBoneMatrices,
	const FVector& InBodyCenter, bool bInGravity, float InStrength, const FVector& InCenterOffset)
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->StartChaosFracturePreview(InRestBoneMatrices, InBodyCenter, bInGravity, InStrength, InCenterOffset);
	}
}

bool SGaussianSplatAssetViewport::IsFracturePreviewActive() const
{
	return ViewportClient.IsValid() && ViewportClient->IsFracturePreviewActive();
}

#undef LOCTEXT_NAMESPACE
