// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorViewportClient.h"
#include "SAdvancedPreviewDetailsTab.h"
#include "SEditorViewport.h"
#include "GaussianSplatAssetViewport.generated.h"

class UGaussianSplatAsset;
class AGaussianSplatActor;
class FAdvancedPreviewScene;

/**
 * Viewport client for previewing OpenSplat assets.
 * Creates a preview scene with a Gaussian Splat actor for interactive preview.
 */
class FGaussianSplatAssetViewportClient : public FEditorViewportClient
{
public:
	FGaussianSplatAssetViewportClient(FEditorViewportClient* InParentClient = nullptr);
	virtual ~FGaussianSplatAssetViewportClient();

	//~ Begin FViewportClient Interface
	virtual void Draw(FViewport* Viewport, FCanvas* Canvas) override;
	virtual FLinearColor GetBackgroundColor() const override;
	//~ End FViewportClient Interface

	//~ Begin FEditorViewportClient Interface
	virtual bool ShouldOrbitCamera() const override { return true; }
	virtual bool CanSetWidgetMode(UE::Widget::EWidgetMode NewMode) const override { return false; }
	virtual void Tick(float DeltaSeconds) override;
	//~ End FEditorViewportClient Interface

	/** Set the asset to preview */
	void SetSplatAsset(UGaussianSplatAsset* InAsset);

	/** Get the preview actor */
	AGaussianSplatActor* GetPreviewActor() const { return PreviewActor.Get(); }

private:
	/** The preview scene */
	TSharedPtr<FAdvancedPreviewScene> PreviewScene;

	/** The preview actor */
	TWeakObjectPtr<AGaussianSplatActor> PreviewActor;

	/** Current asset */
	TWeakObjectPtr<UGaussianSplatAsset> CurrentAsset;
};

/**
 * Slate viewport widget for OpenSplat asset preview.
 */
class SGaussianSplatAssetViewport : public SEditorViewport
{
public:
	SLATE_BEGIN_ARGS(SGaussianSplatAssetViewport) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, TSharedPtr<FGaussianSplatAssetViewportClient> InClient);

	//~ Begin SEditorViewport Interface
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;
	virtual TSharedPtr<SWidget> MakeViewportToolbar() override;
	//~ End SEditorViewport Interface

	/** Set the asset to preview */
	void SetSplatAsset(UGaussianSplatAsset* InAsset);

private:
	/** The viewport client */
	TSharedPtr<FGaussianSplatAssetViewportClient> ViewportClient;
};
