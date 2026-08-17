// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatTrainingDatasetFactory.h"
#include "GaussianSplatTrainingDataset.h"

UGaussianSplatTrainingDatasetFactory::UGaussianSplatTrainingDatasetFactory()
{
	bCreateNew = true;
	bEditAfterNew = true;

	SupportedClass = UGaussianSplatTrainingDataset::StaticClass();
}

UObject* UGaussianSplatTrainingDatasetFactory::FactoryCreateNew(
	UClass* InClass,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	UObject* Context,
	FFeedbackContext* Warn)
{
	return NewObject<UGaussianSplatTrainingDataset>(InParent, InClass, InName, Flags);
}

FText UGaussianSplatTrainingDatasetFactory::GetDisplayName() const
{
	return NSLOCTEXT("GaussianSplatTrainingDataset", "DisplayName", "训练数据集");
}

FText UGaussianSplatTrainingDatasetFactory::GetToolTip() const
{
	return NSLOCTEXT("GaussianSplatTrainingDataset", "Tooltip", "高斯泼溅训练数据集索引资产。勾选外部路径后选择图片文件夹即可自动解析。");
}
