// Copyright Epic Games, Inc. All Rights Reserved.
// Copyright OpenSplat4D 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorViewportClient.h"
#include "SEditorViewport.h"

class UGaussianSplatAsset;
class AGaussianSplatActor;
class FAdvancedPreviewScene;
class FSceneView;
class FPrimitiveDrawInterface;

/**
 * Viewport client for previewing OpenSplat assets.
 * Creates a preview scene with a Gaussian Splat actor for interactive preview.
 *
 * 破裂编辑器还会用它在源点云上叠加绘制 PlanarCut 生成的切割面线框与
 * Voronoi 站点（见 SetCuttingPreview）。
 */
class FGaussianSplatAssetViewportClient : public FEditorViewportClient
{
public:
	FGaussianSplatAssetViewportClient(FPreviewScene* InPreviewScene = nullptr);
	virtual ~FGaussianSplatAssetViewportClient();

	//~ Begin FEditorViewportClient Interface
	virtual bool ShouldOrbitCamera() const override { return true; }
	virtual void Tick(float DeltaSeconds) override;
	/** 叠加绘制破裂切割面预览（基类负责拖拽工具/gizmo，必须先调用） */
	virtual void Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI) override;
	//~ End FEditorViewportClient Interface

	/** Set the asset to preview. bFrameAsset=false rebuilds the preview actor without moving the camera. */
	void SetSplatAsset(UGaussianSplatAsset* InAsset, bool bFrameAsset = true);

	/**
	 * 设置破裂切割面预览的叠加线框。
	 * @param InSegments 线段端点扁平数组：每 2 个元素一条线段（起点、终点）
	 * @param InSites    Voronoi 站点位置，用点标出（可为空）
	 * 位置一律是源资产局部空间（预览 actor 摆在世界原点，两者等价）。
	 */
	void SetCuttingPreview(const TArray<FVector>& InSegments, const TArray<FVector>& InSites);

	/** 关闭切割面预览叠加 */
	void ClearCuttingPreview();

	/** Get the preview actor */
	AGaussianSplatActor* GetPreviewActor() const { return PreviewActor.Get(); }

private:
	/** The preview scene */
	TSharedPtr<FAdvancedPreviewScene> PreviewScene;

	/** The preview actor */
	TWeakObjectPtr<AGaussianSplatActor> PreviewActor;

	/** Current asset */
	TWeakObjectPtr<UGaussianSplatAsset> CurrentAsset;

	/** 切割面预览线段（每 2 个 FVector 一条线段）；空 = 不绘制 */
	TArray<FVector> CuttingPreviewSegments;

	/** 切割面预览的 Voronoi 站点 */
	TArray<FVector> CuttingPreviewSites;
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

	/** 转发切割面预览叠加（见 FGaussianSplatAssetViewportClient::SetCuttingPreview） */
	void SetCuttingPreview(const TArray<FVector>& InSegments, const TArray<FVector>& InSites);

	/** 关闭切割面预览叠加 */
	void ClearCuttingPreview();

private:
	/** The viewport client */
	TSharedPtr<FGaussianSplatAssetViewportClient> ViewportClient;
};
