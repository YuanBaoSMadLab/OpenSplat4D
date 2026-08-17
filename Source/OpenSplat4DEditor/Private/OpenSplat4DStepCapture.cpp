#include "OpenSplat4DStep.h"
#include "OpenSplat4DSettings.h"
#include "OpenSplat4DEditorLibrary.h"
#include "OpenSplat4DPointCloud.h"
#include "OpenSplat4DPointCloudActor.h"

#include "Kismet/GameplayStatics.h"
#include "Components/SceneCaptureComponent2D.h"
#include "LevelEditor.h"
#include "Selection.h"
#include "ImageUtils.h"
#include "Engine/StaticMeshActor.h"
#include "Kismet/KismetMathLibrary.h"
#include "Components/SphereComponent.h"
#include "Misc/EngineVersionComparison.h"
#include "SLevelViewport.h"
#include "Components/SkyLightComponent.h"
#include "LandscapeComponent.h"
#include "LandscapeProxy.h"
#include "ImageCore.h"
// FPackageName (used below via LongPackageNameToFilename) is transitively
// available through Misc/PackagePath.h (FPackagePath depends on FPackageName),
// so we keep this single include rather than adding a path that the toolchain
// cannot resolve.
#include "Misc/PackagePath.h"
#include "OpenSplat4DLocalization.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

namespace
{
	UStaticMesh* GetCameraMesh()
	{
		return LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/EditorMeshes/MatineeCam_SM.MatineeCam_SM"));
	}
}

void UOpenSplat4DStep_Capture::Activate()
{
	if (!RenderTarget)
	{
		RenderTarget = NewObject<UTextureRenderTarget2D>();
		RenderTarget->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA16f;
		RenderTarget->InitAutoFormat(RenderTargetResolution, RenderTargetResolution);
		RenderTarget->UpdateResourceImmediate(true);
	}
	if (!SceneCapture)
	{
		TArray<AActor*> CaptureActors;
		UGameplayStatics::GetAllActorsOfClass(World, ASceneCapture2D::StaticClass(), CaptureActors);
		for (AActor* Actor : CaptureActors)
		{
			if (Actor->Tags.Contains("OpenSplat4DCapture"))
			{
				SceneCapture = Cast<ASceneCapture2D>(Actor);
				break;
			}
		}
		if (!SceneCapture)
		{
			SceneCapture = World->SpawnActor<ASceneCapture2D>();
			SceneCapture->SetActorLabel(TEXT("OpenSplat4DCapture"));
			SceneCapture->SetFlags(RF_Transient);
			SceneCapture->Tags.Add("OpenSplat4DCapture");
		}
		USceneCaptureComponent2D* SCC = SceneCapture->GetCaptureComponent2D();
		SCC->TextureTarget = RenderTarget;
		SCC->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		SCC->CaptureSource = bCaptureFinalColor ? ESceneCaptureSource::SCS_FinalColorHDR : ESceneCaptureSource::SCS_SceneColorHDR;

		if (ShowFlagSettings.IsEmpty())
		{
			ShowFlagSettings.Add(FEngineShowFlagsSetting{ TEXT("Atmosphere"), false });
			ShowFlagSettings.Add(FEngineShowFlagsSetting{ TEXT("Fog"), false });
			ShowFlagSettings.Add(FEngineShowFlagsSetting{ TEXT("VolumetricFog"), false });
			ShowFlagSettings.Add(FEngineShowFlagsSetting{ TEXT("Decals"), false });
			// Captures are shadow-free by default. View-dependent dynamic shadows
			// cannot be represented by a 3DGS/4DGS model and only introduce
			// artefacts in the training images, so leaving them on degrades the
			// scan quality. Re-enable via the step's ShowFlagSettings if needed.
			ShowFlagSettings.Add(FEngineShowFlagsSetting{ TEXT("DynamicShadows"), false });
			ShowFlagSettings.Add(FEngineShowFlagsSetting{ TEXT("Lighting"), true });
			ShowFlagSettings.Add(FEngineShowFlagsSetting{ TEXT("PostProcessing"), true });
			ShowFlagSettings.Add(FEngineShowFlagsSetting{ TEXT("Translucency"), true });
		}
#if UE_VERSION_NEWER_THAN(5, 5, 0)
		SCC->SetShowFlagSettings(ShowFlagSettings);
#else
		SCC->ShowFlagSettings = ShowFlagSettings;
		{
			FPropertyChangedEvent ShowFlagEvent(USceneCaptureComponent2D::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(USceneCaptureComponent2D, ShowFlagSettings)));
			SCC->PostEditChangeProperty(ShowFlagEvent);
		}
#endif
		SCC->PostProcessSettings = PostProcessSettings;
		{
			FPropertyChangedEvent PostProcEvent(USceneCaptureComponent2D::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(USceneCaptureComponent2D, PostProcessSettings)));
			SCC->PostEditChangeProperty(PostProcEvent);
		}
	}

	// Apply unlit material-only mode: disable Lighting show flag directly on SCC.
	// This overrides whatever ShowFlagSettings says, ensuring directional/sky lights
	// have zero influence on the captured images.
	if (SceneCapture && bUnlitMaterialOnly)
	{
		USceneCaptureComponent2D* SCC_Unlit = SceneCapture->GetCaptureComponent2D();
		SCC_Unlit->ShowFlags.SetLighting(false);
	}

	if (World->WorldType == EWorldType::Editor && FSlateApplication::IsInitialized())
	{
		FLevelEditorModule& LevelEditor = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
		LevelEditor.OnActorSelectionChanged().AddUObject(this, &UOpenSplat4DStep_Capture::OnActorSelectionChanged);
		TArray<UObject*> Objects;
		GEditor->GetSelectedActors()->GetSelectedObjects(Objects);
		OnActorSelectionChanged(Objects, true);
		GEngine->OnComponentTransformChanged().AddUObject(this, &UOpenSplat4DStep_Capture::OnComponentTransformChanged);
	}
	UpdateCameraMatrix();
}

void UOpenSplat4DStep_Capture::Deactivate()
{
	if (SceneCapture) { SceneCapture->K2_DestroyActor(); SceneCapture = nullptr; }
	if (LocateActor) { LocateActor->K2_DestroyActor(); LocateActor = nullptr; }
	for (AActor* Actor : CameraActors) Actor->Destroy();
	CameraActors.Reset();
	if (FModuleManager::Get().IsModuleLoaded("LevelEditor"))
	{
		FLevelEditorModule& LevelEditor = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
		LevelEditor.OnActorSelectionChanged().RemoveAll(this);
	}
	GEngine->OnComponentTransformChanged().RemoveAll(this);
}

void UOpenSplat4DStep_Capture::Capture()
{
	if (OnRequestTaskStart.IsBound() && !OnRequestTaskStart.Execute()) return;

	const float Radius = FMath::Max<FVector::FReal>(CurrentBounds.BoxExtent.Size(), 10.f);
	USceneCaptureComponent2D* SCC = SceneCapture->GetCaptureComponent2D();
	RenderTarget = SCC->TextureTarget;
	if (SourceMode == EOpenSplat4DCaptureSourceMode::Select)
	{
		TArray<AActor*> ShowOnlyActors;
		UGameplayStatics::GetAllActorsOfClass(World, ASkyLight::StaticClass(), ShowOnlyActors);
		ShowOnlyActors.Append(SelectionActors);
		SCC->ShowOnlyActors = ShowOnlyActors;
	}

	const bool bCache = SCC->bCaptureEveryFrame;
	SCC->bCaptureEveryFrame = false;
	SCC->bAlwaysPersistRenderingState = true;

	const double HalfFOV = FMath::DegreesToRadians(SCC->FOVAngle / 2.0);

	// Resolve the final workDir BEFORE capturing any images.  If no capture-set
	// asset exists yet, generate a timestamped name now so the images land in
	// the same directory that the asset's WorkDirectory will point to.
	// (Previously this happened AFTER the capture loop, so the asset pointed
	// to an empty directory while the images sat in the old default WorkDir.)
	if (!CaptureSetAsset)
	{
		const FString SetName = OpenSplat4DBuildCaptureSetName();
		const FString SetWorkDir = GetDefault<UOpenSplat4DSettings>()->GetWorkDir(SetName);
		WorkDir = SetWorkDir;
	}

	const FString ImgDir = WorkDir / "images";
	const FString MaskDir = WorkDir / "masks";
	const FString DepthDir = WorkDir / "depths";
	const FString CamFile = WorkDir / "cameras.txt";

	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	for (const FString& D : { ImgDir, MaskDir, DepthDir }) if (PF.DirectoryExists(*D)) PF.DeleteDirectoryRecursively(*D);

	FakeEngineTick(World, 0.03f, 6);
	TaskProgressPercent = 0.f;

	// ============================================================================
	// 深度归一化全局参考（自适应场景大小）
	// ============================================================================
	// 深度远裁面应根据实际场景尺寸自适应：
	//   - 有包围盒：相机距离 + 物体半径 × 1.5（覆盖物体在相机对侧的最远点）
	//   - 无包围盒：相机距离 × 3（保守估计，含足够的边界）
	// 这样小物件（如头像）得到高精度深度，大场景得到足够覆盖范围。
	const float CamDist = CameraActors.Num() > 0
		? FVector::Dist(CurrentBounds.Origin, CameraActors[0]->GetActorLocation())
		: 200.0f;
	const float BoundRadius = CurrentBounds.SphereRadius;
	float GlobalMaxDistCm;
	if (BoundRadius > KINDA_SMALL_NUMBER)
	{
		GlobalMaxDistCm = FMath::Max(CamDist + BoundRadius * 1.5f, BoundRadius * 2.5f);
	}
	else
	{
		GlobalMaxDistCm = FMath::Max(CamDist * 3.0f, 100.0f);
	}
	UE_LOG(LogOpenSplat4DStep, Log, TEXT("Depth normalization: CamDist=%.1f BoundRadius=%.1f -> GlobalMaxDist=%.1f cm"),
		CamDist, BoundRadius, GlobalMaxDistCm);

	UE_LOG(LogOpenSplat4DStep, Log, TEXT("Depth mode: %s, denom=%.1f cm"),
		bAutoDepthRange ? TEXT("auto") : TEXT("manual"),
		bAutoDepthRange ? GlobalMaxDistCm : DepthMaxDistanceCm);

	FString CamContent;
	int32 CapturedCount = 0;
	for (int i = 0; i < CameraActors.Num(); i++)
	{
		const FString ImgName = FString::Printf(TEXT("image%04d.png"), i + 1);
		SCC->SetWorldTransform(CameraActors[i]->GetTransform());
		FTextureRenderTargetResource* RTRes = RenderTarget->GameThread_GetRenderTargetResource();
		FReadSurfaceDataFlags ColorFlags(RCM_MinMax);
		const FIntRect Region(0, 0, RenderTarget->SizeX, RenderTarget->SizeY);

		if (bCaptureDepth)
		{
			FakeEngineTick(World);
			SCC->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
			SCC->CaptureScene();
			TArray<FLinearColor> DepthColors;
			// 与 teachers/GaussianSplattingForUnrealEngine 完全一致的深度管线：
			// 使用 RCM_MinMax、FFloat16::MaxF16Float 阈值、SegmentationThreshold=0.5
			RTRes->ReadLinearColorPixels(DepthColors, ColorFlags, Region);
			float Min = FFloat16::MaxF16Float;
			float Max = 0.f;
			for (const FLinearColor& C : DepthColors)
			{
				Min = FMath::Min(Min, C.R);
				if (C.R < FFloat16::MaxF16Float)
				{
					Max = FMath::Max(Max, C.R);
				}
			}
			constexpr float SegmentationThreshold = 0.5f;
			const float SegmentationRange = 1.0f - SegmentationThreshold;
			int32 NumValid = 0, NumInvalid = 0;
			for (FLinearColor& C : DepthColors)
			{
				if (C.R < FFloat16::MaxF16Float)
				{
					C.R = 1.0f - SegmentationRange * (C.R - Min) / FMath::Max(Max - Min, 0.0001f);
					NumValid++;
				}
				else
				{
					C.R = 0.f;
					NumInvalid++;
				}
			}
			UE_LOG(LogOpenSplat4DStep, Log, TEXT("  depth %s: valid=%d invalid=%d, raw=[%.4f, %.4f]"),
				*ImgName, NumValid, NumInvalid, Min, Max);
			// FImageView 不指定格式（与 teachers 一致）
			FImageView DepthView(DepthColors.GetData(), RenderTarget->SizeX, RenderTarget->SizeY);
			FImage GrayDepth; GrayDepth.Init(DepthView.SizeX, DepthView.SizeY, ERawImageFormat::G16, EGammaSpace::Linear);
			FImageCore::CopyImage(DepthView, GrayDepth);
			FImageUtils::SaveImageByExtension(*(DepthDir / ImgName), GrayDepth);
		}

		TArray<FLinearColor> Raw, Final, Mask;
		FakeEngineTick(World);
		SCC->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;
		SCC->CaptureScene();
		RTRes->ReadLinearColorPixels(Raw, ColorFlags, Region);
		Final = bCaptureFinalColor ? Raw : Raw;
		if (bCaptureFinalColor)
		{
			FakeEngineTick(World);
			SCC->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
			SCC->CaptureScene();
			RTRes->ReadLinearColorPixels(Final, ColorFlags, Region);
		}
		Mask.SetNum(Raw.Num());
		// ============================================================================
		// Mask 软化：用连续 alpha 替代硬二值化
		// ============================================================================
		// 旧实现 `M = A > 0 ? 1 : 0` 在 mask 边界形成硬切，3DGS Densification
		// 在硬边界处产生极大梯度 → 高斯点无限分裂 → 边界"杂云"。
		//
		// 修复：直接用 alpha 作为 soft mask（场景渲染 alpha 已是 [0, 1] 连续值）。
		// 对完全透明的像素（天空）仍置 0，对完全不透明像素置 1，对半透明边缘像素
		// （抗锯齿后的物体轮廓）保留中间值，让 L1 loss 在边缘自动降权。
		// 同时做一次轻量高斯模糊（3x3）进一步平滑亚像素锯齿。
		for (int j = 0; j < Raw.Num(); j++)
		{
			FLinearColor& F = Final[j];
			F = LinearToSRGB(F);
			// Raw[j].A 是场景不透明度（post-tonsmap），1=不透明，0=背景。
			// 我们要把 0/1 之间的抗锯齿过渡保留下来，所以直接 1-A 当 mask。
			const float A = 1.f - Raw[j].A;
			Mask[j] = FLinearColor(A, A, A, 1.f);
			F.A = A;
		}
		// 3x3 高斯模糊软化边缘（kernel = [1 2 1; 2 4 2; 1 2 1] / 16）。
		// 在图像边缘做 clamp 处理。这一步是可选的——如果 SoftMask 已经够用，
		// 后续训练可以靠 alpha 直接做 L1 加权，不需要再模糊。
		const int32 W = RenderTarget->SizeX;
		const int32 H = RenderTarget->SizeY;
		TArray<FLinearColor> BlurredMask;
		BlurredMask.SetNum(Mask.Num());
		for (int y = 0; y < H; y++)
		{
			for (int x = 0; x < W; x++)
			{
				const int idx = y * W + x;
				const int xm = FMath::Max(x - 1, 0);
				const int xp = FMath::Min(x + 1, W - 1);
				const int ym = FMath::Max(y - 1, 0);
				const int yp = FMath::Min(y + 1, H - 1);
				const float sum =
					Mask[ym * W + xm].R * 1.f + Mask[ym * W + x].R * 2.f + Mask[ym * W + xp].R * 1.f +
					Mask[y  * W + xm].R * 2.f + Mask[y  * W + x].R * 4.f + Mask[y  * W + xp].R * 2.f +
					Mask[yp * W + xm].R * 1.f + Mask[yp * W + x].R * 2.f + Mask[yp * W + xp].R * 1.f;
				BlurredMask[idx] = FLinearColor(sum / 16.f, sum / 16.f, sum / 16.f, 1.f);
			}
		}
		Mask = MoveTemp(BlurredMask);
		ReceiveMessage(FString::Printf(TEXT("Capturing %s"), *ImgName));
		if (bRequestCancelTask) { bRequestCancelTask = false; break; }

		FImageView IV(Final.GetData(), RenderTarget->SizeX, RenderTarget->SizeY);
		FImageUtils::SaveImageByExtension(*(ImgDir / ImgName), IV);
		FImageView MV(Mask.GetData(), RenderTarget->SizeX, RenderTarget->SizeY);
		FImageUtils::SaveImageByExtension(*(MaskDir / ImgName), MV);
		++CapturedCount;

		const FVector CP = (CameraActors[i]->GetActorLocation() - CurrentBounds.Origin) / 100.0;
		CamContent += FString::Printf(TEXT("%s %lf %lf %lf\n"), *ImgName, CP.X, -CP.Z, -CP.Y);
		TaskProgressPercent = static_cast<float>(i) / static_cast<float>(CameraActors.Num());
		FakeEngineTick(World);
	}
	FFileHelper::SaveStringToFile(CamContent, *CamFile);
	TaskProgressPercent = 0.f;

	// Persist this capture session as a native UE DataAsset (a real .uasset in
	// /Game/OpenSplat4D/Captures) instead of a loose JSON file, so the group is
	// draggable, Blueprint-referenceable and survives editor restarts. If the
	// panel already assigned an existing capture-set asset, update it; otherwise
	// create a new one keyed by the working-directory leaf.
	if (CaptureSetAsset)
	{
		OpenSplat4DUpdateCaptureSet(CaptureSetAsset, WorkDir, CapturedCount);
	}
	else
	{
		// SetName & SetWorkDir were already resolved before the capture
		// loop (WorkDir == SetWorkDir at this point).
		const FString SetName = FPaths::GetCleanFilename(WorkDir);
		CaptureSetAsset = OpenSplat4DCreateCaptureSet(SetName, WorkDir);
		if (CaptureSetAsset)
		{
			OpenSplat4DUpdateCaptureSet(CaptureSetAsset, WorkDir, CapturedCount);
		}
	}

	// Tell the user exactly where the captured data landed. The downstream
	// colmap / training steps read from WorkDir, so a missing-images report
	// almost always means "looked in the wrong folder", not "capture failed".
	UE_LOG(LogOpenSplat4DStep, Log, TEXT("Capture Finished: %d frames saved"), CameraActors.Num());
	UE_LOG(LogOpenSplat4DStep, Log, TEXT("  images : %s"), *ImgDir);
	UE_LOG(LogOpenSplat4DStep, Log, TEXT("  masks  : %s"), *MaskDir);
	UE_LOG(LogOpenSplat4DStep, Log, TEXT("  depths : %s"), *DepthDir);
	UE_LOG(LogOpenSplat4DStep, Log, TEXT("  cameras: %s"), *CamFile);
	UE_LOG(LogOpenSplat4DStep, Log, TEXT("These files are inside the plugin working directory. Click the folder icon at the top of the OpenSplat4D panel to open it, or run '稀疏重建' (Sparse) next."));

	ReceiveMessage(FString::Printf(TEXT("Capture Finished: %d images saved to %s"), CameraActors.Num(), *ImgDir));
	SCC->bCaptureEveryFrame = bCache;
	OnTaskFinished.Broadcast();
}

void UOpenSplat4DStep_Capture::PrevCamera() { SetCurrentCameraIndex(CurrentCameraIndex - 1); SCC_ApplyCamera(); }

void UOpenSplat4DStep_Capture::ScanToPointCloud()
{
	if (!World)
	{
		return;
	}
	TArray<AActor*> Actors = SelectionActors;
	if (Actors.Num() == 0 && GEditor)
	{
        USelection* SelectedActors = GEditor->GetSelectedActors();
        for (int32 SelIdx = 0; SelIdx < SelectedActors->Num(); ++SelIdx)
        {
            if (AActor* A = Cast<AActor>(SelectedActors->GetSelectedObject(SelIdx)))
            {
                Actors.AddUnique(A);
            }
        }
	}

	if (Actors.Num() == 0)
	{
		FNotificationInfo Info(OS4D_TEXT("ScanSelectFirst"));
		Info.ExpireDuration = 6.f;
		Info.bUseSuccessFailIcons = true;
		if (TSharedPtr<SNotificationItem> N = FSlateNotificationManager::Get().AddNotification(Info))
		{
			N->SetCompletionState(SNotificationItem::CS_Fail);
		}
		return;
	}

	UOpenSplat4DPointCloud* Cloud = UOpenSplat4DEditorLibrary::CreatePointCloudFromActors(
		World, Actors, ScanMode, ScanDensity, ScanPointScale, ScanSaveDir);
	if (!Cloud)
	{
		FNotificationInfo Info(OS4D_TEXT("ScanNoPoints"));
		Info.ExpireDuration = 8.f;
		Info.bUseSuccessFailIcons = true;
		if (TSharedPtr<SNotificationItem> N = FSlateNotificationManager::Get().AddNotification(Info))
		{
			N->SetCompletionState(SNotificationItem::CS_Fail);
		}
		return;
	}

	ReceiveMessage(FString::Printf(TEXT("Scanned %d points -> %s"), Cloud->GetPointCount(), *Cloud->GetName()));
	if (GEditor)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.bNoFail = true;
		if (AOpenSplat4DPointCloudActor* Actor = World->SpawnActor<AOpenSplat4DPointCloudActor>(
			AOpenSplat4DPointCloudActor::StaticClass(), SpawnParams))
		{
			Actor->SetPointCloud(Cloud);
			GEditor->SelectActor(Actor, true, true);
			TArray<UObject*> ObjectsToSync;
			ObjectsToSync.Add(Cloud);
			GEditor->SyncBrowserToObjects(ObjectsToSync);
		}
	}

	// Resolve the content-path save dir to an absolute filesystem path so the
	// user can actually find the saved .uasset (it lives in
	// <Project>/Content/OpenSplat4D/, NOT in the capture WorkDir). Note: ScanSaveDir
	// is a *directory* (e.g. "/Game/OpenSplat4D"), not a package, so we must use
	// FPackageName::LongPackageNameToFilename — using FPackagePath::GetLocalFullPath
	// here wrongly treats the folder as a package and emits a "path does not exist /
	// unspecified header extension" warning (the dangling-asset red herring).
	const FString AbsFile = FPackageName::LongPackageNameToFilename(
		ScanSaveDir / Cloud->GetName(), FPackageName::GetAssetPackageExtension());
	const FText Msg = FText::Format(
		OpenSplat4DLocalization::GetText(TEXT("ScanDone {0} {1}")),
		FText::AsNumber(Cloud->GetPointCount()),
		FText::FromString(AbsFile));
	FNotificationInfo Info(Msg);
	Info.ExpireDuration = 8.f;
	Info.bUseSuccessFailIcons = true;
	if (TSharedPtr<SNotificationItem> N = FSlateNotificationManager::Get().AddNotification(Info))
	{
		N->SetCompletionState(SNotificationItem::CS_Success);
	}
}
void UOpenSplat4DStep_Capture::NextCamera() { SetCurrentCameraIndex(CurrentCameraIndex + 1); SCC_ApplyCamera(); }

void UOpenSplat4DStep_Capture::SCC_ApplyCamera()
{
	if (SceneCapture && CameraActors.IsValidIndex(CurrentCameraIndex))
	{
		USceneCaptureComponent2D* SCC = SceneCapture->GetCaptureComponent2D();
		SCC->SetWorldTransform(CameraActors[CurrentCameraIndex]->GetActorTransform());

		// Re-apply unlit material-only mode in case it was toggled after Activate().
		if (bUnlitMaterialOnly)
		{
			SCC->ShowFlags.SetLighting(false);
		}

		// Force an immediate preview capture so the user sees what this camera
		// sees without having to run the full capture pipeline first.
		// bCaptureEveryFrame is normally off to avoid wasting GPU during idle.
		if (!SCC->bCaptureEveryFrame)
		{
			SCC->CaptureScene();
		}

		UE_LOG(LogOpenSplat4DStep, Log, TEXT("Preview camera %d/%d: %s"),
			CurrentCameraIndex + 1, CameraActors.Num(),
			*CameraActors[CurrentCameraIndex]->GetActorLabel());
	}
}

void UOpenSplat4DStep_Capture::SetSelectionByComponents(const TArray<UActorComponent*>& InSourceComponents)
{
	if (!SceneCapture || SourceMode != EOpenSplat4DCaptureSourceMode::Select) return;
	USceneCaptureComponent2D* SCC = SceneCapture->GetCaptureComponent2D();
	FBoxSphereBounds Bounds; Bounds.SphereRadius = 0;
	TArray<TWeakObjectPtr<UPrimitiveComponent>> Sel;
	for (UActorComponent* SC : InSourceComponents)
	{
		if (UPrimitiveComponent* PC = Cast<UPrimitiveComponent>(SC))
		{
			Bounds = (Bounds.SphereRadius <= 0) ? PC->Bounds : (Bounds + PC->Bounds);
			Sel.Add(PC);
		}
	}
	if (Bounds.SphereRadius == 0) return;
	CurrentBounds = Bounds;
	SCC->ShowOnlyComponents = Sel;
	UpdateCameraMatrix();
}

void UOpenSplat4DStep_Capture::OnActorSelectionChanged(const TArray<UObject*>& NewSelection, bool bForceRefresh)
{
	if (!SceneCapture) return;
	USceneCaptureComponent2D* SCC = SceneCapture->GetCaptureComponent2D();
	TArray<TObjectPtr<AActor>> NewActors;
	for (UObject* Item : NewSelection)
	{
		if (AActor* Actor = Cast<AActor>(Item))
		{
			if (Actor->HasAnyFlags(RF_Transient)) continue;
			bool bHasPrim = false;
			for (UActorComponent* C : Actor->GetComponents()) if (Cast<UPrimitiveComponent>(C)) { bHasPrim = true; break; }
			if (bHasPrim) NewActors.AddUnique(Actor);
		}
	}
	FBoxSphereBounds Bounds; Bounds.SphereRadius = 0;
	for (AActor* Actor : NewActors)
	{
		for (UActorComponent* C : Actor->GetComponents())
			if (UPrimitiveComponent* PC = Cast<UPrimitiveComponent>(C))
				Bounds = (Bounds.SphereRadius <= 0) ? PC->Bounds : (Bounds + PC->Bounds);
	}
	if (NewSelection.Num() == 1)
	{
		if (AActor* A = Cast<AActor>(NewSelection[0]); A && A->Tags.Contains("OpenSplat4DCaptureCamera"))
		{
			CurrentCameraIndex = CameraActors.IndexOfByKey(A);
			SCC_ApplyCamera();
		}
	}
	if (Bounds.SphereRadius == 0 || SourceMode != EOpenSplat4DCaptureSourceMode::Select) return;
	CurrentBounds = Bounds;
	SelectionActors = NewActors;
	TArray<AActor*> ShowOnly;
	UGameplayStatics::GetAllActorsOfClass(World, ASkyLight::StaticClass(), ShowOnly);
	ShowOnly.Append(SelectionActors);
	SCC->ShowOnlyActors = ShowOnly;
	UpdateCameraMatrix();
}

void UOpenSplat4DStep_Capture::OnComponentTransformChanged(USceneComponent* Component, ETeleportType TeleportType)
{
	if (!Component) return;
	if (Component->GetOwner() == LocateActor && LocateActor)
	{
		FBoxSphereBounds B; B.Origin = LocateActor->GetActorLocation();
		USphereComponent* S = CastChecked<USphereComponent>(LocateActor->GetRootComponent());
		B.SphereRadius = S->GetScaledSphereRadius();
		B.BoxExtent = FVector(B.SphereRadius / FMath::Sqrt(3.f));
		CurrentBounds = B; UpdateCameraMatrix();
	}
	else if (SelectionActors.Contains(Component->GetOwner())) UpdateCameraMatrix();
}

void UOpenSplat4DStep_Capture::UpdateCameraMatrix()
{
	if (!SceneCapture || !SceneCapture->IsValidLowLevel()) return;
	USceneCaptureComponent2D* SCC = SceneCapture->GetCaptureComponent2D();

	if (SourceMode == EOpenSplat4DCaptureSourceMode::Locate && !LocateActor)
	{
		LocateActor = World->SpawnActor<ATriggerSphere>();
		LocateActor->SetFlags(RF_Transient);
		LocateActor->SetActorLabel("OpenSplat4DCaptureLocate");
		USphereComponent* S = CastChecked<USphereComponent>(LocateActor->GetRootComponent());
		S->SetSphereRadius(10000);
		FBoxSphereBounds B; B.Origin = LocateActor->GetActorLocation();
		B.SphereRadius = S->GetScaledSphereRadius();
		B.BoxExtent = FVector(B.SphereRadius / FMath::Sqrt(3.f));
		CurrentBounds = B;
		if (World->WorldType == EWorldType::Editor && FSlateApplication::IsInitialized())
		{
			GEditor->SelectNone(false, true, true);
			GEditor->SelectActor(LocateActor, true, false, true);
			GEditor->MoveViewportCamerasToActor({ LocateActor }, true);
			GEditor->NoteSelectionChange();
		}
		SCC->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	}
	else if (SourceMode == EOpenSplat4DCaptureSourceMode::Select && LocateActor)
	{
		LocateActor->Destroy(); LocateActor = nullptr;
	}

	if (SourceMode == EOpenSplat4DCaptureSourceMode::Custom)
	{
		for (int i = 0; i < CameraActors.Num(); i++)
		{
			if (CameraActors[i] == nullptr)
			{
				AStaticMeshActor* M = World->SpawnActor<AStaticMeshActor>();
				M->SetMobility(EComponentMobility::Movable);
				M->AttachToActor(SceneCapture, FAttachmentTransformRules::KeepWorldTransform);
				M->SetFlags(RF_Transient);
				M->Tags.Add("OpenSplat4DCaptureCamera");
				M->GetStaticMeshComponent()->SetStaticMesh(GetCameraMesh());
				// The camera gizmo must never cast shadows into the scene (it is
				// only an editor aid placed at each capture pose, and its shadow
				// in the main viewport reads as "the camera is projecting").
				M->GetStaticMeshComponent()->SetCastShadow(false);
				CameraActors[i] = M;
			}
		}
	}
	else
	{
		const int Desired = FrameXY * FrameXY;
		if (CameraActors.Num() < Desired)
		{
			for (int i = CameraActors.Num(); i < Desired; i++)
			{
				AStaticMeshActor* M = World->SpawnActor<AStaticMeshActor>();
				M->SetMobility(EComponentMobility::Movable);
				M->AttachToActor(SceneCapture, FAttachmentTransformRules::KeepWorldTransform);
				M->SetFlags(RF_Transient);
				M->Tags.Add("OpenSplat4DCaptureCamera");
				M->GetStaticMeshComponent()->SetStaticMesh(GetCameraMesh());
				// The camera gizmo must never cast shadows into the scene (it is
				// only an editor aid placed at each capture pose, and its shadow
				// in the main viewport reads as "the camera is projecting").
				M->GetStaticMeshComponent()->SetCastShadow(false);
				CameraActors.Add(M);
			}
		}
		else if (CameraActors.Num() > Desired)
		{
			for (int i = Desired; i < CameraActors.Num(); i++) CameraActors[i]->Destroy();
			CameraActors.SetNum(Desired);
		}
		// Fall back to a sane radius when nothing is selected yet, so adjusting
		// FrameXY / CaptureDistanceScale always visibly repositions the rig
		// (otherwise Dist is 0 and every camera sits on the world origin).
		const float Radius = (CurrentBounds.SphereRadius > KINDA_SMALL_NUMBER)
			? CurrentBounds.SphereRadius
			: 200.f;
		const float Dist = Radius * CaptureDistanceScale;
		for (int i = 0; i < FrameXY; i++)
		{
			for (int j = 0; j < FrameXY; j++)
			{
				const int Idx = i * FrameXY + j;
				AActor* M = CameraActors[Idx];
				FVector Dir = (CameraMode == EOpenSplat4DCaptureCameraMode::Hemisphere)
					? UVtoPyramid(FVector2D(i / (double)(FrameXY - 1), j / (double)(FrameXY - 1)))
					: UVtoOctahedron(FVector2D(i / (double)(FrameXY - 1), j / (double)(FrameXY - 1)));
				Dir.Normalize();
				const FVector Pos = CurrentBounds.Origin + Dir * Dist;
				M->SetActorLocationAndRotation(Pos, UKismetMathLibrary::FindLookAtRotation(Pos, CurrentBounds.Origin));
				M->SetActorLabel(FString::Printf(TEXT("OpenSplat4DCamera%d[%d_%d]"), Idx + 1, i, j));
			}
		}
	}
	if (!CameraActors.IsEmpty())
	{
		SCC_ApplyCamera();
	}
	if (SourceMode == EOpenSplat4DCaptureSourceMode::Locate || SourceMode == EOpenSplat4DCaptureSourceMode::Custom)
	{
		SCC->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_LegacySceneCapture;
		TArray<AActor*> Hidden = HiddenActors; Hidden.Append(CameraActors);
		SCC->ShowOnlyActors.Reset(); SCC->HiddenActors = Hidden;
	}
	else if (SourceMode == EOpenSplat4DCaptureSourceMode::Select)
	{
		SCC->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	}
}

void UOpenSplat4DStep_Capture::SetCurrentCameraIndex(int InIndex)
{
	CurrentCameraIndex = InIndex;
	if (CameraActors.Num() > 0)
	{
		if (CurrentCameraIndex < 0) CurrentCameraIndex = CameraActors.Num() - 1;
		else if (CurrentCameraIndex >= CameraActors.Num()) CurrentCameraIndex = (CurrentCameraIndex + 1) % CameraActors.Num();
		SCC_ApplyCamera();
	}
}

void UOpenSplat4DStep_Capture::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	const FName PN = PropertyChangedEvent.GetMemberPropertyName();
	if (PN == GET_MEMBER_NAME_CHECKED(UOpenSplat4DStep_Capture, FrameXY)
		|| PN == GET_MEMBER_NAME_CHECKED(UOpenSplat4DStep_Capture, SourceMode)
		|| PN == GET_MEMBER_NAME_CHECKED(UOpenSplat4DStep_Capture, CaptureDistanceScale)
		|| PN == GET_MEMBER_NAME_CHECKED(UOpenSplat4DStep_Capture, CameraMode)
		|| PN == GET_MEMBER_NAME_CHECKED(UOpenSplat4DStep_Capture, HiddenActors))
	{
		UpdateCameraMatrix();
	}
	else if (SceneCapture && PN == GET_MEMBER_NAME_CHECKED(UOpenSplat4DStep_Capture, ShowFlagSettings))
	{
		SceneCapture->GetCaptureComponent2D()->SetShowFlagSettings(ShowFlagSettings);
	}
	else if (SceneCapture && (PN == GET_MEMBER_NAME_CHECKED(UOpenSplat4DStep_Capture, PostProcessSettings) || PN == GET_MEMBER_NAME_CHECKED(UOpenSplat4DStep_Capture, bCaptureFinalColor)))
	{
		USceneCaptureComponent2D* SCC = SceneCapture->GetCaptureComponent2D();
		SCC->PostProcessSettings = PostProcessSettings;
		SCC->CaptureSource = bCaptureFinalColor ? ESceneCaptureSource::SCS_FinalColorHDR : ESceneCaptureSource::SCS_SceneColorHDR;
	}
	else if (PN == GET_MEMBER_NAME_CHECKED(UOpenSplat4DStep_Capture, RenderTargetResolution) && RenderTarget)
	{
		RenderTarget->ResizeTarget(RenderTargetResolution, RenderTargetResolution);
	}
	else if (SceneCapture && PN == GET_MEMBER_NAME_CHECKED(UOpenSplat4DStep_Capture, bUnlitMaterialOnly))
	{
		// Toggle lighting show flag directly on the capture component.
		// bUnlitMaterialOnly=true → Lighting off (pure material)
		// bUnlitMaterialOnly=false → restore from ShowFlagSettings
		USceneCaptureComponent2D* SCC = SceneCapture->GetCaptureComponent2D();
		if (bUnlitMaterialOnly)
		{
			SCC->ShowFlags.SetLighting(false);
		}
		else
		{
			// Re-apply ShowFlagSettings to restore Lighting from config
			SCC->SetShowFlagSettings(ShowFlagSettings);
		}
		SCC_ApplyCamera();
	}
}
