// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorViewportClient.h"
#include "SEditorViewport.h"

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
	FGaussianSplatAssetViewportClient(FPreviewScene* InPreviewScene = nullptr);
	virtual ~FGaussianSplatAssetViewportClient();

	//~ Begin FEditorViewportClient Interface
	virtual bool ShouldOrbitCamera() const override { return true; }
	virtual void Tick(float DeltaSeconds) override;
	//~ End FEditorViewportClient Interface

	/** Set the asset to preview. bFrameAsset=false rebuilds the preview actor without moving the camera. */
	void SetSplatAsset(UGaussianSplatAsset* InAsset, bool bFrameAsset = true);

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
	//~ End SEditorViewport Interface

	/** Set the asset to preview. bFrameAsset=false rebuilds without moving the camera. */
	void SetSplatAsset(UGaussianSplatAsset* InAsset, bool bFrameAsset = true);

	/** Get the current preview actor (may be null) */
	AGaussianSplatActor* GetPreviewActor() const;

private:
	/** The viewport client */
	TSharedPtr<FGaussianSplatAssetViewportClient> ViewportClient;
};
