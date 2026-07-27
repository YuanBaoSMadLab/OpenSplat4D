#include "OpenSplat4DStep.h"
#include "OpenSplat4DSettings.h"
#include "OpenSplat4DEditorLibrary.h"
#include "OpenSplat4DLocalization.h"
#include "HAL/PlatformProcess.h"

#include "UObject/SavePackage.h"

#include "IContentBrowserSingleton.h"
#include "ContentBrowserModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"

#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Misc/PackagePath.h"
#include "GaussianSplatAsset.h"
#include "OpenSplat4DPointCloudActor.h"
#include "GaussianSplatComponent.h"
#include "PLYFileReader.h"

// ----------------------------------------------------------------------------
// Sparse reconstruction (colmap)
// ----------------------------------------------------------------------------
void UOpenSplat4DStep_SparseReconstruction::Activate() { UpdateParams(); }
void UOpenSplat4DStep_SparseReconstruction::Deactivate() {}

void UOpenSplat4DStep_SparseReconstruction::Reconstruction()
{
	if (OnRequestTaskStart.IsBound() && !OnRequestTaskStart.Execute()) return;
	TaskProgressPercent = 0.5f;
	ReconstructionSparse(true, [this]() { TaskProgressPercent = 1.f; OnTaskFinished.Broadcast(); });
}

void UOpenSplat4DStep_SparseReconstruction::ColmapEdit()
{
	const FString Colmap = GetDefault<UOpenSplat4DSettings>()->GetColmapExecutablePath();
	if (!OpenSplat4DIsExecutableResolvable(Colmap))
	{
		UE_LOG(LogOpenSplat4DStep, Error, TEXT("COLMAP 可执行文件未找到: \"%s\"。请在【设置】→ OpenSplat4D → ColmapExecutablePath 填写 colmap 的完整路径（如 C:/Program Files/Colmap/colmap.exe），或把 colmap 加入系统 PATH 后重启编辑器。"), *Colmap);
		return;
	}
	const FString Command = FString::Printf(TEXT("%s %s --colmap %s --edit"),
		*OpenSplat4DQuoteArg(GetDefault<UOpenSplat4DSettings>()->GetHelperScriptPath()),
		*OpenSplat4DQuoteArg(WorkDir), *OpenSplat4DQuoteArg(Colmap));
	ExecuteCommand(OpenSplat4DQuoteArg(GetDefault<UOpenSplat4DSettings>()->GetPythonExecutablePath()), Command, true);
}

void UOpenSplat4DStep_SparseReconstruction::ColmapView()
{
	const FString Colmap = GetDefault<UOpenSplat4DSettings>()->GetColmapExecutablePath();
	if (!OpenSplat4DIsExecutableResolvable(Colmap))
	{
		UE_LOG(LogOpenSplat4DStep, Error, TEXT("COLMAP 可执行文件未找到: \"%s\"。请在【设置】→ OpenSplat4D → ColmapExecutablePath 填写 colmap 的完整路径（如 C:/Program Files/Colmap/colmap.exe），或把 colmap 加入系统 PATH 后重启编辑器。"), *Colmap);
		return;
	}
	const FString Command = FString::Printf(TEXT("%s %s --colmap %s --view"),
		*OpenSplat4DQuoteArg(GetDefault<UOpenSplat4DSettings>()->GetHelperScriptPath()),
		*OpenSplat4DQuoteArg(WorkDir), *OpenSplat4DQuoteArg(Colmap));
	ExecuteCommand(OpenSplat4DQuoteArg(GetDefault<UOpenSplat4DSettings>()->GetPythonExecutablePath()), Command, true);
}

void UOpenSplat4DStep_SparseReconstruction::ReconstructionSparse(bool bAsync, TFunction<void()> FinishedCallback)
{
	UpdateParams();
	// Honor the explicitly-selected target capture set (index asset) so we
	// always reconstruct the right images / cameras.
	if (TargetCaptureSet)
	{
		WorkDir = TargetCaptureSet->WorkDirectory;
	}
	const FString Colmap = GetDefault<UOpenSplat4DSettings>()->GetColmapExecutablePath();
	if (!OpenSplat4DIsExecutableResolvable(Colmap))
	{
		UE_LOG(LogOpenSplat4DStep, Error, TEXT("COLMAP 可执行文件未找到: \"%s\"。请在【设置】→ OpenSplat4D → ColmapExecutablePath 填写 colmap 的完整路径（如 C:/Program Files/Colmap/colmap.exe），或把 colmap 加入系统 PATH 后重启编辑器。"), *Colmap);
		if (FinishedCallback) FinishedCallback();
		return;
	}
	const FString Command = FString::Printf(TEXT("%s %s --colmap %s --sparse --extractor=\"%s\" --matcher=\"%s\" --mapper=\"%s\" --aligner=\"%s\" "),
		*OpenSplat4DQuoteArg(GetDefault<UOpenSplat4DSettings>()->GetHelperScriptPath()),
		*OpenSplat4DQuoteArg(WorkDir), *OpenSplat4DQuoteArg(GetDefault<UOpenSplat4DSettings>()->GetColmapExecutablePath()),
		*FeatureExtractorParams, *ExhaustiveMatcherParams, *MapperParams, *ModelAlignerParams);
	ExecuteCommand(OpenSplat4DQuoteArg(GetDefault<UOpenSplat4DSettings>()->GetPythonExecutablePath()), Command, bAsync, FinishedCallback);
}

void UOpenSplat4DStep_SparseReconstruction::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	const FName PN = PropertyChangedEvent.GetMemberPropertyName();
	if (PN == GET_MEMBER_NAME_CHECKED(UOpenSplat4DStep_SparseReconstruction, MaxNumFeatures) || PN.ToString().EndsWith("Custom"))
	{
		UpdateParams();
	}
}

void UOpenSplat4DStep_SparseReconstruction::UpdateParams()
{
	FeatureExtractorParams = FString::Printf(TEXT("%s --SiftExtraction.max_num_features %d"), *FeatureExtractorParamsCustom, MaxNumFeatures);
	ExhaustiveMatcherParams = ExhaustiveMatcherParamsCustom;
	MapperParams = FString::Printf(TEXT("%s --Mapper.abs_pose_min_num_inliers %d"), *MapperParamsCustom, AbsPoseMinNumInliers);
	ModelAlignerParams = ModelAlignerParamsCustom;
}

// ----------------------------------------------------------------------------
// Gaussian splatting training
// ----------------------------------------------------------------------------
void UOpenSplat4DStep_GaussianSplatting::Activate() { UpdateParams(); }
void UOpenSplat4DStep_GaussianSplatting::Deactivate() {}

void UOpenSplat4DStep_GaussianSplatting::Train()
{
	if (OnRequestTaskStart.IsBound() && !OnRequestTaskStart.Execute()) return;
	TaskProgressPercent = 0.3f;
		Train(true, [this]()
		{
			TaskProgressPercent = 0.7f;
			Reload();
			UGaussianSplatAsset* Saved = nullptr;
			if (bAutoImportToContent) Saved = SaveToContent();
			if (Saved && bPlaceInLevel) PlaceInLevel(Saved);
			TaskProgressPercent = 1.f;
			OnTaskFinished.Broadcast();
		});
}

void UOpenSplat4DStep_GaussianSplatting::Train(bool bAsync, TFunction<void()> FinishedCallback)
{
	UpdateParams();
	// Honor the explicitly-selected target capture set (index asset) so training
	// fits the intended reconstruction.
	if (TargetCaptureSet)
	{
		WorkDir = TargetCaptureSet->WorkDirectory;
	}
	const UOpenSplat4DSettings* Settings = GetDefault<UOpenSplat4DSettings>();
	const FString Python = Settings->GetPythonExecutablePath();
	const FString Colmap = Settings->GetColmapExecutablePath();
	if (!OpenSplat4DIsExecutableResolvable(Python))
	{
		UE_LOG(LogOpenSplat4DStep, Error, TEXT("Python 可执行文件未找到: \"%s\"。插件默认使用自带的 ThirdParty/Python/python.exe，请先运行 ThirdParty/Python/install_env.bat 打包训练环境；或在【设置】→ OpenSplat4D → PythonExecutablePath 填写一个可用的 python 完整路径（如 C:/Python310/python.exe，需含 torch+cu128 及已编译的 CUDA 扩展）。"), *Python);
		if (FinishedCallback) FinishedCallback();
		return;
	}
	if (!OpenSplat4DIsExecutableResolvable(Colmap))
	{
		UE_LOG(LogOpenSplat4DStep, Error, TEXT("COLMAP 可执行文件未找到: \"%s\"。请在【设置】→ OpenSplat4D → ColmapExecutablePath 填写 colmap 的完整路径（如 C:/Program Files/Colmap/colmap.exe），或把 colmap 加入系统 PATH 后重启编辑器。"), *Colmap);
		if (FinishedCallback) FinishedCallback();
		return;
	}
	const FString Repo = bTrain4D ? Settings->Get4DGSRepoDir() : Settings->Get3DGSRepoDir();
	const FString Command = FString::Printf(TEXT("%s %s --colmap %s --gaussian %s --train=\"%s\" %s"),
		*OpenSplat4DQuoteArg(GetDefault<UOpenSplat4DSettings>()->GetHelperScriptPath()),
		*OpenSplat4DQuoteArg(WorkDir), *OpenSplat4DQuoteArg(GetDefault<UOpenSplat4DSettings>()->GetColmapExecutablePath()),
		*OpenSplat4DQuoteArg(Repo), *GaussianSplattingTrainParams,
		bTrain4D ? TEXT("--4d") : TEXT(""));
	ExecuteCommand(OpenSplat4DQuoteArg(GetDefault<UOpenSplat4DSettings>()->GetPythonExecutablePath()), Command, bAsync, FinishedCallback);
}

FString UOpenSplat4DStep_GaussianSplatting::Clip(FString PlyPath)
{
	const FString Command = FString::Printf(TEXT("%s %s --clip --ply \"%s\" --mask_dilation %d --clip_threshold %f"),
		*OpenSplat4DQuoteArg(GetDefault<UOpenSplat4DSettings>()->GetHelperScriptPath()), *OpenSplat4DQuoteArg(WorkDir), *PlyPath, MaskDilation, ClipThreshold);
	ExecuteCommand(OpenSplat4DQuoteArg(GetDefault<UOpenSplat4DSettings>()->GetPythonExecutablePath()), Command, false);
	FString Directory, Filename, Extension;
	FPaths::Split(PlyPath, Directory, Filename, Extension);
	return FPaths::Combine(Directory, Filename + "_clipped." + Extension);
}

void UOpenSplat4DStep_GaussianSplatting::Reload()
{
	if (!LocalPackage) LocalPackage = CreatePackage(TEXT("/OpenSplat4D/Editor"));
	Result = LoadPly(LocalPackage, NAME_None);
	OnPlyLoadFinished.ExecuteIfBound();
}

UGaussianSplatAsset* UOpenSplat4DStep_GaussianSplatting::SaveToContent()
{
	// Make sure we have the freshly trained cloud in memory.
	if (!Result) { Reload(); }
	if (!Result)
	{
		UE_LOG(LogOpenSplat4DStep, Error, TEXT("OpenSplat4D: 自动导入失败，训练结果未加载。"));
		return nullptr;
	}

	// Name the asset by timestamp so repeated runs never collide.
	if (Result->GetSplatCount() == 0)
	{
		UE_LOG(LogOpenSplat4DStep, Error, TEXT("OpenSplat4D: 自动导入跳过——训练结果点云为空（0 个点）。WorkDir：%s Iter：%d"), *WorkDir, Iterations);
		return nullptr;
	}
	const FString TimeStr = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	const FString AssetName = FString::Printf(TEXT("Gaussian_%s"), *TimeStr);
	const FString PackagePath = FString::Printf(TEXT("/Game/OpenSplat4D/Models/%s"), *AssetName);

	UPackage* NewPackage = CreatePackage(*PackagePath);
	NewPackage->MarkPackageDirty();
	UGaussianSplatAsset* NewAsset = DuplicateObject<UGaussianSplatAsset>(Result, NewPackage, *AssetName);
	if (!NewAsset)
	{
		UE_LOG(LogOpenSplat4DStep, Error, TEXT("OpenSplat4D: 自动导入失败，无法创建资产 %s。"), *PackagePath);
		return nullptr;
	}
	NewAsset->SetFlags(RF_Public | RF_Standalone);

	// Stamp the source ply + the 3 data paths + mode onto the new asset.
	NewAsset->SourceFilePath = FPaths::Combine(
		WorkDir, TEXT("output"), TEXT("point_cloud"),
		FString::Printf(TEXT("iteration_%d"), Iterations), TEXT("point_cloud.ply"));
	FillAssetPaths();

	FAssetRegistryModule::AssetCreated(NewAsset);

	FString LocalPath;
	if (!FPackageName::TryConvertLongPackageNameToFilename(PackagePath, LocalPath, FPackageName::GetAssetPackageExtension()))
	{
		UE_LOG(LogOpenSplat4DStep, Error, TEXT("OpenSplat4D: 自动导入失败，无法解析文件路径 %s。"), *PackagePath);
		return nullptr;
	}
	// Ensure parent directory exists
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(LocalPath), true);

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	SaveArgs.bWarnOfLongFilename = false;
	if (!UPackage::SavePackage(NewPackage, NewAsset, *LocalPath, SaveArgs))
	{
		UE_LOG(LogOpenSplat4DStep, Error, TEXT("OpenSplat4D: 自动导入失败，无法保存 %s。"), *LocalPath);
		return nullptr;
	}
	Result = NewAsset;

	// Confirm to the user + reveal the new asset in the Content Browser.
	const FString Msg = FString::Printf(
		TEXT("训练完成，已自动导入高斯模型：\n/Game/OpenSplat4D/Models/%s"), *AssetName);
	FNotificationInfo NotifyInfo(FText::FromString(Msg));
	NotifyInfo.ExpireDuration = 6.0f;
	NotifyInfo.bUseSuccessFailIcons = true;
	if (TSharedPtr<SNotificationItem> N = FSlateNotificationManager::Get().AddNotification(NotifyInfo))
	{
		N->SetCompletionState(SNotificationItem::CS_Success);
	}

	if (GEditor)
	{
		TArray<UObject*> ObjectsToSync;
		ObjectsToSync.Add(NewAsset);
		GEditor->SyncBrowserToObjects(ObjectsToSync);
	}
	return NewAsset;
}

	void UOpenSplat4DStep_GaussianSplatting::PlaceInLevel(UGaussianSplatAsset* SplatAsset)
	{
		if (!SplatAsset || !GEditor)
		{
			return;
		}
		UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
		if (!EditorWorld)
		{
			return;
		}
		FActorSpawnParameters SpawnParams;
		SpawnParams.bNoFail = true;
		AOpenSplat4DPointCloudActor* Actor = EditorWorld->SpawnActor<AOpenSplat4DPointCloudActor>(
			AOpenSplat4DPointCloudActor::StaticClass(), SpawnParams);
		if (Actor)
		{
			Actor->GaussianSplatComponent->SetSplatAsset(SplatAsset);
			GEditor->SelectActor(Actor, true, true);
			UE_LOG(LogOpenSplat4DStep, Log, TEXT("OpenSplat4D: 已将训练结果放入场景 — %s"), *Actor->GetName());
		}
	}

void UOpenSplat4DStep_GaussianSplatting::FillAssetPaths()
{
	// Mode is stored on the capture set / point cloud asset, set separately.
	// Here we only fill the capture-set directory paths.

	if (!TargetCaptureSet)
	{
		return;
	}

	// (a) 拍摄的图片集位置
	TargetCaptureSet->ImagesDir.Path = TargetCaptureSet->GetImagesDir();

	// (b) 初次生成模型的位置（稀疏重建输出）
	TargetCaptureSet->InitialModelDir.Path = TargetCaptureSet->GetInitialModelDir();

	// (c) 训练模型生成后的位置
	TargetCaptureSet->TrainedModelDir.Path = FPaths::Combine(
		WorkDir, TEXT("output"), TEXT("point_cloud"),
		FString::Printf(TEXT("iteration_%d"), Iterations));
}

void UOpenSplat4DStep_GaussianSplatting::Export()
{
	FSaveAssetDialogConfig Cfg;
	Cfg.DefaultPath = LastSavePath;
	Cfg.DefaultAssetName = "PointCloud";
	Cfg.AssetClassNames.Add(UGaussianSplatAsset::StaticClass()->GetClassPathName());
	Cfg.ExistingAssetPolicy = ESaveAssetDialogExistingAssetPolicy::AllowButWarn;
	Cfg.DialogTitleOverride = OS4D_TEXT("Save As");

	const FContentBrowserModule& CBM = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	const FString SaveObjectPath = CBM.Get().CreateModalSaveAssetDialog(Cfg);
	if (SaveObjectPath.IsEmpty()) return;
	const FString PackagePath = FPackageName::ObjectPathToPackageName(SaveObjectPath);
	const FString AssetName = FPaths::GetBaseFilename(PackagePath, true);
	LastSavePath = PackagePath;
	if (AssetName.IsEmpty()) return;
	UPackage* NewPackage = CreatePackage(*PackagePath);
	UGaussianSplatAsset* NewAsset = DuplicateObject<UGaussianSplatAsset>(Result, NewPackage, *AssetName);
	NewAsset->SetFlags(RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(NewAsset);
	FPackagePath NewPackagePath = FPackagePath::FromPackageNameChecked(NewPackage->GetName());
	FString LocalPath = NewPackagePath.GetLocalFullPath();
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	SaveArgs.bWarnOfLongFilename = false;
	UPackage::SavePackage(NewPackage, NewAsset, *LocalPath, SaveArgs);
	Result = NewAsset;
}

UGaussianSplatAsset* UOpenSplat4DStep_GaussianSplatting::LoadPly(UObject* Outer, FName AssetName)
{
	FString PlyPath = FString::Printf(TEXT("%s/output/point_cloud/iteration_%d/point_cloud.ply"), *WorkDir, Iterations);
	if (bClippingByMask) PlyPath = Clip(PlyPath);

	if (!FPaths::FileExists(PlyPath))
	{
		UE_LOG(LogOpenSplat4DStep, Error, TEXT("PLY file not found: %s"), *PlyPath);
		return nullptr;
	}

	TArray<FGaussianSplatData> SplatData;
	FString ErrorMessage;
	int32 DetectedSHBands = 0;

	if (!FPLYFileReader::ReadPLYFile(PlyPath, SplatData, ErrorMessage, &DetectedSHBands))
	{
		UE_LOG(LogOpenSplat4DStep, Error, TEXT("Failed to read PLY: %s"), *ErrorMessage);
		return nullptr;
	}

	UGaussianSplatAsset* Asset = NewObject<UGaussianSplatAsset>(Outer, UGaussianSplatAsset::StaticClass(), AssetName, RF_Transient);
	if (!Asset)
	{
		return nullptr;
	}

	Asset->SourceFilePath = PlyPath;
	Asset->SHBands = DetectedSHBands;
	Asset->InitializeFromSplatData(SplatData, ImportQuality);
	UE_LOG(LogOpenSplat4DStep, Log, TEXT("Loaded %d splats (SH bands: %d) from %s"), SplatData.Num(), DetectedSHBands, *PlyPath);

	return Asset;
}

void UOpenSplat4DStep_GaussianSplatting::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	UpdateParams();
}

void UOpenSplat4DStep_GaussianSplatting::UpdateParams()
{
	GaussianSplattingTrainParams = FString::Printf(TEXT("--resolution %d --iterations %d --save_iterations %d --feature_lr %f --opacity_lr %f --scaling_lr %f --rotation_lr %f --position_lr_max_steps %d --position_lr_init %f --position_lr_final %f --position_lr_delay_mult %f --densify_from_iter %d --densify_until_iter %d --densify_grad_threshold %f --densification_interval %d --opacity_reset_interval %d --depth_l1_weight_init %f --depth_l1_weight_final %f --lambda_dssim %f --percent_dense %f"),
		Resolution, Iterations, Iterations, Feature_LR, Opacity_LR, Scaling_LR, Rotation_LR,
		Position_LR_MaxSteps, Position_LR_Init, Position_LR_Final, Position_LR_DelayMult,
		DensifyFromIter, DensifyUntilIter, DensifyGradThreshold, DensificationInterval, OpacityResetInterval,
		Depth_L1_WeightInit, Depth_L1_WeightFinal, LambdaDssim, PercentDense);
}
