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

/**
 * Custom asset editor for OpenSplat assets.
 * Layout: Left = 3D preview viewport, Right = details panel (Chinese localized).
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

private:
	/** Tab identifiers */
	static const FName ViewportTabId;
	static const FName DetailsTabId;

	/** Spawn the viewport tab (left side) */
	TSharedRef<SDockTab> SpawnTab_Viewport(const FSpawnTabArgs& Args);

	/** Spawn the details tab (right side) */
	TSharedRef<SDockTab> SpawnTab_Details(const FSpawnTabArgs& Args);

	/** The asset being edited */
	TStrongObjectPtr<UGaussianSplatAsset> SplatAsset;

	/** Details view for properties */
	TSharedPtr<IDetailsView> DetailsView;

	/** Viewport widget container */
	TSharedPtr<SBorder> ViewportContainer;
};
