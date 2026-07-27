// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/IToolkitHost.h"
#include "Misc/NotifyHook.h"
#include "GaussianSplatEditorData.h"

class UGaussianSplatAsset;
class UGaussianSplatEditorData;
class IDetailsView;
class SDockTab;
class SBorder;
class SGaussianSplatAssetViewport;

/**
 * Custom asset editor for OpenSplat assets.
 *
 * Layout:
 *   - 顶部工具栏：编辑工具按钮（框选 / 球选 / 笔刷 / 全选 / 反选 / 删除 / 隐藏 / 隔离 / 全显）
 *   - 左侧：3D 预览视口（支持鼠标交互选择 splat）
 *   - 右侧：Details 面板（显示资产属性 + 选中 splat 信息）
 *
 * 编辑会话状态（选中/隐藏 flags）保存在 EditorData 中，不污染资产本身。
 * EditorData 是 Transient，关闭编辑器时自动销毁。
 */
class FGaussianSplatAssetEditor : public FAssetEditorToolkit, public FNotifyHook
{
public:
	FGaussianSplatAssetEditor();
	virtual ~FGaussianSplatAssetEditor();

	//~ Begin FNotifyHook Interface
	virtual void NotifyPostChange(const FPropertyChangedEvent& PropertyChangedEvent, FProperty* PropertyThatChanged) override;
	//~ End FNotifyHook Interface

	//~ Begin IToolkit Interface
	virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& TabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& TabManager) override;
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FText GetToolkitName() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;
	virtual FString GetDocumentationLink() const override;
	//~ End IToolkit Interface

	/** Initialize the editor with the given asset */
	void InitGaussianSplatAssetEditor(const EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& InitToolkitHost, UGaussianSplatAsset* InAsset);

	/** Get the editor data (selection / hide state). Viewport uses this to query flags. */
	UGaussianSplatEditorData* GetEditorData() const { return EditorData.Get(); }

	/** Get the viewport widget (for tool-driven mouse interaction). */
	TSharedPtr<SGaussianSplatAssetViewport> GetViewportWidget() const;

private:
	/** Tab identifiers */
	static const FName ViewportTabId;
	static const FName DetailsTabId;

	/** Spawn the viewport tab (left side) */
	TSharedRef<SDockTab> SpawnTab_Viewport(const FSpawnTabArgs& Args);

	/** Spawn the details tab (right side) */
	TSharedRef<SDockTab> SpawnTab_Details(const FSpawnTabArgs& Args);

	// ------------------------------------------------------------------------
	// 工具栏构建
	// ------------------------------------------------------------------------

	/** 构建顶部工具栏（编辑工具按钮） */
	void BuildToolbar();

	/** 工具栏按钮回调 */
	void OnToolButtonClicked(EGaussianEditTool InTool);
	void OnSelectAllClicked();
	void OnDeselectAllClicked();
	void OnInvertSelectionClicked();
	void OnDeleteSelectedClicked();
	void OnHideSelectedClicked();
	void OnIsolateSelectedClicked();
	void OnShowAllClicked();

	/** 刷新 Details 面板（编辑操作后调用，让选中数量等属性更新） */
	void RefreshDetails();

	/** EditorData 变化回调（触发 viewport 刷新） */
	void OnEditorDataChanged();

	// ------------------------------------------------------------------------
	// 数据成员
	// ------------------------------------------------------------------------

	/** The asset being edited */
	TStrongObjectPtr<UGaussianSplatAsset> SplatAsset;

	/** 编辑会话数据（选中/隐藏 flags，Transient，不序列化）。
	 *  用 TStrongObjectPtr 而非 UPROPERTY 是因为 EditorData 是 Transient UObject，
	 *  不需要 GC 反射追踪 —— TStrongObjectPtr 自己持有强引用防止 GC 回收。 */
	TStrongObjectPtr<UGaussianSplatEditorData> EditorData;

	/** Details view for properties */
	TSharedPtr<IDetailsView> DetailsView;

	/** Viewport widget container */
	TSharedPtr<SBorder> ViewportContainer;

	/** 持有的 viewport widget 引用（用于工具栏触发鼠标交互模式切换） */
	TSharedPtr<SGaussianSplatAssetViewport> ViewportWidget;
};
