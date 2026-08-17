// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetTypeActions_Base.h"

class UGaussianSplatTrainingDataset;

/**
 * Asset type actions for Training Dataset assets.
 * Provides content browser integration: custom color, category, and context menu actions.
 */
class FAssetTypeActions_GaussianSplatTrainingDataset : public FAssetTypeActions_Base
{
public:
	//~ Begin IAssetTypeActions Interface
	virtual FText GetName() const override;
	virtual FColor GetTypeColor() const override;
	virtual UClass* GetSupportedClass() const override;
	virtual uint32 GetCategories() override;
	virtual bool HasActions(const TArray<UObject*>& InObjects) const override { return true; }
	virtual void GetActions(const TArray<UObject*>& InObjects, struct FToolMenuSection& Section) override;
	//~ End IAssetTypeActions Interface

private:
	/** 扫描选中数据集 */
	void ExecuteScan(TArray<TWeakObjectPtr<UGaussianSplatTrainingDataset>> Objects);

	/** 在浏览器中打开外部目录 */
	void ExecuteOpenDirectory(TArray<TWeakObjectPtr<UGaussianSplatTrainingDataset>> Objects);
};
