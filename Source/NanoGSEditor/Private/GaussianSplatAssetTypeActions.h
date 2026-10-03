// Copyright OpenSplat4D 2026 All Rights Reserved.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetTypeActions_Base.h"

class UGaussianSplatAsset;

/**
 * Asset type actions for Gaussian Splat assets
 * Provides content browser editor integration like custom color, category, and actions
 */
class FAssetTypeActions_GaussianSplatAsset : public FAssetTypeActions_Base
{
public:
	//~ Begin IAssetTypeActions Interface
	virtual FText GetName() const override;
	virtual FColor GetTypeColor() const override;
	virtual UClass* GetSupportedClass() const override;
	virtual uint32 GetCategories() override;
	virtual bool HasActions(const TArray<UObject*>& InObjects) const override { return true; }
	virtual void GetActions(const TArray<UObject*>& InObjects, struct FToolMenuSection& Section) override;
	virtual void OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<class IToolkitHost> EditWithinLevelEditor = TSharedPtr<IToolkitHost>()) override;
	//~ End IAssetTypeActions Interface

private:
	/** Handler for reimporting assets */
	void ExecuteReimport(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects);

	/** Handler for showing asset info */
	void ExecuteShowInfo(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects);

	/** Handler for enabling Nanite */
	void ExecuteEnableNanite(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects);

	/** Handler for disabling Nanite */
	void ExecuteDisableNanite(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects);

	/** Handler for importing a multi-file PLY sequence as a keyframe 4D asset */
	void ExecuteImportPLYSequence(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects);

	/** Handler for generating the blocky voxel proxy mesh used as a fracture source */
	void ExecuteGenerateProxyMesh(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects);

	/** Handler for creating the independent fracture asset that references this splat asset */
	void ExecuteGenerateFractureAsset(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects);

	// ------------------------------------------------------------------------
	// 已废弃（v2 重构）：旧的「绑定破裂骨头」把骨头绑定写回 UGaussianSplatAsset。
	// 新流程里绑定数据归 UGaussianSplatFractureAsset，绑定改由破裂编辑器在
	// 解算时调用 FGaussianSplatBoneBinder 完成，Content Browser 不再提供该入口。
	// 代码保留不删，仅停用；如需恢复请先确认绑定目标已改回。
	// ------------------------------------------------------------------------
	// void ExecuteBindFractureBones(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects);

	/** Check if all selected assets have Nanite enabled */
	bool AreAllNaniteEnabled(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects) const;

	/** Check if all selected assets have Nanite disabled */
	bool AreAllNaniteDisabled(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects) const;
};
