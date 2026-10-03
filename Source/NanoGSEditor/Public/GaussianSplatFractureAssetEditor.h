// Copyright OpenSplat4D 2026 All Rights Reserved.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Input/Reply.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/IToolkitHost.h"
#include "Misc/NotifyHook.h"

class UGaussianSplatFractureAsset;
class IDetailsView;
class SDockTab;
class STextBlock;
class SGaussianSplatAssetViewport;

/**
 * 破裂资产专用编辑器（双击 UGaussianSplatFractureAsset 打开）。
 *
 * 布局：
 *   - 左侧：3D 预览视口，显示该破裂资产所引用的源 OpenSplat 资产，
 *     并按「破裂设置」叠加 PlanarCut 生成的切割面线框与 Voronoi 站点
 *   - 右侧：详情面板，顶部是「解算」按钮与解算状态，下面是破裂资产
 *     自身的属性（源资产引用、破裂设置、几何集合、绑定统计）
 *
 * 解算 = 把体素壳喂给 Chaos 的 PlanarCut 切割，产出 FGeometryCollection
 * （内嵌为破裂资产的子对象），再用 FGaussianSplatBoneBinder 把每个 splat
 * 归属到破裂块并把断面 splat 写回破裂资产。
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

	/** 「解算」按钮的点击处理 */
	FReply OnSolveClicked();

	/** 「重新生成体素壳」按钮的点击处理 */
	FReply OnRegenerateShellClicked();

	/** 「破碎预览 / 停止预览」按钮的点击处理 */
	FReply OnFracturePreviewClicked();

	/**
	 * 按破裂资产当前的预览设置启动轻量破碎预览（碎片从爆炸中心向外飞散，
	 * 可选重力）。需要已解算（有骨头绑定）才行，否则只写状态文本。
	 */
	void StartFracturePreview();

	/** 停止轻量破碎预览，碎片回到 rest */
	void StopFracturePreview();

	/**
	 * 资产/绑定变化后让预览与当前状态一致：预览开着就按新的骨头绑定重开一次
	 * （旧的骨头索引已经失效），没开就什么也不做。
	 */
	void SyncFracturePreview();

	/**
	 * 按破裂资产当前的「估算网格位置偏移」重新生成碰撞体素壳。
	 *
	 * 生成只写破裂资产自己的体素壳块，源 splat 资产不被修改；网格外的 splat
	 * 既不进入壳的形状，之后解算时也不会被绑定。已有的壳在生成失败时保持不变。
	 */
	void RegenerateVoxelShell();

	/**
	 * 解算主流程：体素壳 → FGeometryCollection → PlanarCut 切割 →
	 * 内嵌 UGeometryCollection → 绑定回写（骨头归属 + 断面 splat）。
	 */
	void SolveFracture();

	/** 更新详情面板顶部的解算状态文本 */
	void SetSolveStatus(const FString& Message, bool bIsError);

	/** The fracture asset being edited */
	TStrongObjectPtr<UGaussianSplatFractureAsset> FractureAsset;

	/** Details view for fracture asset properties */
	TSharedPtr<IDetailsView> DetailsView;

	/** Viewport widget (previews the source splat asset) */
	TSharedPtr<SGaussianSplatAssetViewport> ViewportWidget;

	/** 详情面板顶部的解算状态文本 */
	TSharedPtr<STextBlock> SolveStatusText;

	/** 轻量破碎预览是否开着（决定按钮文案与重开语义） */
	bool bFracturePreviewActive = false;
};
