// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/IToolkitHost.h"
#include "Misc/NotifyHook.h"

class UGaussianSplatAsset;
class IDetailsView;
class SDockTab;
class SBorder;
class SGaussianSplatAssetViewport;
class S4DTransportBar;

/**
 * 4D 高斯资产的专用"播放器式"编辑器（区别于 3D 编辑器）：
 *
 *   ┌─────────────────────────┬──────────┐
 *   │                         │          │
 *   │   3D 预览视口            │ Details  │
 *   │                         │          │
 *   ├─────────────────────────┴──────────┤
 *   │ |◀ [▶播放] ■停止 ▶|  ●─────────  t=…│  ← S4DTransportBar
 *   └────────────────────────────────────┘
 *
 * 播放控制直接驱动预览组件（Play4D/SetPlaybackTime 等），渲染管线零改动。
 * 3D 资产仍走 FGaussianSplatAssetEditor（见 AssetTypeActions 分流）。
 */
class FGaussianSplat4DEditor : public FAssetEditorToolkit, public FNotifyHook
{
public:
	FGaussianSplat4DEditor();
	virtual ~FGaussianSplat4DEditor();

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

	/** Initialize the editor with the given 4D asset */
	void InitGaussianSplat4DEditor(const EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& InitToolkitHost, UGaussianSplatAsset* InAsset);

private:
	/** Tab identifiers */
	static const FName ViewportTabId;
	static const FName DetailsTabId;

	/** Spawn the viewport tab (top, contains transport bar at bottom) */
	TSharedRef<SDockTab> SpawnTab_Viewport(const FSpawnTabArgs& Args);

	/** Spawn the details tab (right side) */
	TSharedRef<SDockTab> SpawnTab_Details(const FSpawnTabArgs& Args);

	/** The 4D asset being played */
	TStrongObjectPtr<UGaussianSplatAsset> SplatAsset;

	/** Details view for properties */
	TSharedPtr<IDetailsView> DetailsView;

	/** Viewport widget */
	TSharedPtr<SGaussianSplatAssetViewport> ViewportWidget;

	/** Transport bar widget (player controls at the bottom) */
	TSharedPtr<S4DTransportBar> TransportBar;
};
