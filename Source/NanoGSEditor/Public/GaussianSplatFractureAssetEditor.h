// Copyright Epic Games, Inc. All Rights Reserved.
// Copyright OpenSplat4D 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/IToolkitHost.h"
#include "Misc/NotifyHook.h"

class UGaussianSplatFractureAsset;
class IDetailsView;
class SDockTab;
class SGaussianSplatAssetViewport;

/**
 * 破裂资产专用编辑器（双击 UGaussianSplatFractureAsset 打开）。
 *
 * 布局：
 *   - 左侧：3D 预览视口，显示该破裂资产所引用的源 OpenSplat 资产，
 *     并按「破裂设置」叠加 PlanarCut 生成的切割面线框与 Voronoi 站点
 *   - 右侧：Details 面板，编辑破裂资产自身的属性（源资产引用、破裂设置、
 *     几何集合、绑定统计）
 *
 * 「解算」按钮在后续阶段接入。
 * 渲染侧不依赖本编辑器，破裂资产离开编辑器即可被蓝图/关卡使用。
 */
class FGaussianSplatFractureAssetEditor : public FAssetEditorToolkit, public FNotifyHook
{
public:
	FGaussianSplatFractureAssetEditor();
	virtual ~FGaussianSplatFractureAssetEditor();

	//~ Begin FNotifyHook Interface
	virtual void NotifyPostChange(const FPropertyChangedEvent& PropertyChangedEvent, FProperty* PropertyThatChanged) override;
	//~ End FNotifyHook Interface

	//~ Begin IToolkit Interface
	virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FText GetToolkitName() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;
	virtual FString GetDocumentationLink() const override;
	//~ End IToolkit Interface

	/** 用给定的破裂资产初始化编辑器 */
	void InitGaussianSplatFractureAssetEditor(
		const EToolkitMode::Type Mode,
		const TSharedPtr<IToolkitHost>& InitToolkitHost,
		UGaussianSplatFractureAsset* InAsset);

private:
	/** Tab identifiers */
	static const FName ViewportTabId;
	static const FName DetailsTabId;

	/** Spawn the viewport tab (left side) */
	TSharedRef<SDockTab> SpawnTab_Viewport(const FSpawnTabArgs& Args);

	/** Spawn the details tab (right side) */
	TSharedRef<SDockTab> SpawnTab_Details(const FSpawnTabArgs& Args);

	/** 把破裂资产引用的源 splat 资产推给预览视口（源资产为空时清空预览） */
	void RefreshPreview();

	/** 按「破裂设置」重算 PlanarCut 切割面并推给视口叠加显示 */
	void RefreshCuttingPreview();

	/** The fracture asset being edited */
	TStrongObjectPtr<UGaussianSplatFractureAsset> FractureAsset;

	/** Details view for fracture asset properties */
	TSharedPtr<IDetailsView> DetailsView;

	/** Viewport widget (previews the source splat asset) */
	TSharedPtr<SGaussianSplatAssetViewport> ViewportWidget;
};
