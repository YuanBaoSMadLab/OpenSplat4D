// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatTrainingDatasetTypeActions.h"
#include "GaussianSplatTrainingDataset.h"
#include "ToolMenuSection.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformProcess.h"

#define LOCTEXT_NAMESPACE "AssetTypeActions_GaussianSplatTrainingDataset"

FText FAssetTypeActions_GaussianSplatTrainingDataset::GetName() const
{
	return LOCTEXT("AssetName", "训练数据集");
}

FColor FAssetTypeActions_GaussianSplatTrainingDataset::GetTypeColor() const
{
	return FColor(255, 165, 0);  // Orange
}

UClass* FAssetTypeActions_GaussianSplatTrainingDataset::GetSupportedClass() const
{
	return UGaussianSplatTrainingDataset::StaticClass();
}

uint32 FAssetTypeActions_GaussianSplatTrainingDataset::GetCategories()
{
	return EAssetTypeCategories::Misc;
}

void FAssetTypeActions_GaussianSplatTrainingDataset::GetActions(const TArray<UObject*>& InObjects, FToolMenuSection& Section)
{
	TArray<TWeakObjectPtr<UGaussianSplatTrainingDataset>> Datasets;
	for (UObject* Object : InObjects)
	{
		if (UGaussianSplatTrainingDataset* Dataset = Cast<UGaussianSplatTrainingDataset>(Object))
		{
			Datasets.Add(Dataset);
		}
	}

	Section.AddMenuEntry(
		"GaussianSplatTrainingDataset_Scan",
		LOCTEXT("ScanLabel", "扫描目录"),
		LOCTEXT("ScanTooltip", "重新扫描外部目录，更新图片列表和相机参数"),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateSP(this, &FAssetTypeActions_GaussianSplatTrainingDataset::ExecuteScan, Datasets),
			FCanExecuteAction()
		)
	);

	Section.AddMenuEntry(
		"GaussianSplatTrainingDataset_OpenDir",
		LOCTEXT("OpenDirLabel", "打开外部目录"),
		LOCTEXT("OpenDirTooltip", "在文件浏览器中打开外部图片目录"),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateSP(this, &FAssetTypeActions_GaussianSplatTrainingDataset::ExecuteOpenDirectory, Datasets),
			FCanExecuteAction::CreateLambda([Datasets]()
			{
				for (const auto& Ptr : Datasets)
				{
					if (Ptr.IsValid() && Ptr->bUseExternalPath && !Ptr->ExternalDirectory.Path.IsEmpty())
						return true;
				}
				return false;
			})
		)
	);
}

void FAssetTypeActions_GaussianSplatTrainingDataset::ExecuteScan(TArray<TWeakObjectPtr<UGaussianSplatTrainingDataset>> Objects)
{
	for (const TWeakObjectPtr<UGaussianSplatTrainingDataset>& Ptr : Objects)
	{
		if (UGaussianSplatTrainingDataset* Dataset = Ptr.Get())
		{
			Dataset->ScanDirectory();
		}
	}
}

void FAssetTypeActions_GaussianSplatTrainingDataset::ExecuteOpenDirectory(TArray<TWeakObjectPtr<UGaussianSplatTrainingDataset>> Objects)
{
	for (const TWeakObjectPtr<UGaussianSplatTrainingDataset>& Ptr : Objects)
	{
		if (UGaussianSplatTrainingDataset* Dataset = Ptr.Get())
		{
			if (Dataset->bUseExternalPath && !Dataset->ExternalDirectory.Path.IsEmpty())
			{
				FPlatformProcess::ExploreFolder(*Dataset->ExternalDirectory.Path);
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
