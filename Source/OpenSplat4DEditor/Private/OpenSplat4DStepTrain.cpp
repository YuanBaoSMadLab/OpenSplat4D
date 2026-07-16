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
	Train(true, [this]() { TaskProgressPercent = 0.7f; Reload(); TaskProgressPercent = 1.f; OnTaskFinished.Broadcast(); });
}

void UOpenSplat4DStep_GaussianSplatting::Train(bool bAsync, TFunction<void()> FinishedCallback)
{
	UpdateParams();
	const UOpenSplat4DSettings* Settings = GetDefault<UOpenSplat4DSettings>();
	const FString Python = Settings->GetPythonExecutablePath();
	const FString Colmap = Settings->GetColmapExecutablePath();
	if (!OpenSplat4DIsExecutableResolvable(Python))
	{
		UE_LOG(LogOpenSplat4DStep, Error, TEXT("Python 可执行文件未找到: \"%s\"。请在【设置】→ OpenSplat4D → PythonExecutablePath 填写 python 的完整路径（如 C:/Python311/python.exe），或把 python 加入系统 PATH 后重启编辑器。"), *Python);
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

void UOpenSplat4DStep_GaussianSplatting::Export()
{
	FSaveAssetDialogConfig Cfg;
	Cfg.DefaultPath = LastSavePath;
	Cfg.DefaultAssetName = "PointCloud";
	Cfg.AssetClassNames.Add(UOpenSplat4DPointCloud::StaticClass()->GetClassPathName());
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
	UOpenSplat4DPointCloud* NewAsset = DuplicateObject<UOpenSplat4DPointCloud>(Result, NewPackage, *AssetName);
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

UOpenSplat4DPointCloud* UOpenSplat4DStep_GaussianSplatting::LoadPly(UObject* Outer, FName AssetName)
{
	UOpenSplat4DPointCloud* Output = nullptr;
	FString PlyPath = FString::Printf(TEXT("%s/output/point_cloud/iteration_%d/point_cloud.ply"), *WorkDir, Iterations);
	if (bClippingByMask) PlyPath = Clip(PlyPath);
	Output = UOpenSplat4DEditorLibrary::LoadSplatFile(PlyPath, Outer, AssetName);
	if (Output) Output->SetCompressionMethod(CompressionMethod);
	return Output;
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
