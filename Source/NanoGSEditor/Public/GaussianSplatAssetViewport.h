// Copyright Epic Games, Inc. All Rights Reserved.
// Copyright OpenSplat4D 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorViewportClient.h"
#include "SEditorViewport.h"

class UGaussianSplatAsset;
class UGaussianSplatFractureAsset;
class UGeometryCollectionComponent;
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

	/**
	 * 让预览组件渲染某个破裂资产（源点云 + 骨头绑定 + 断面），而不是裸源资产。
	 * 传 nullptr 回到裸源资产渲染。破裂编辑器每次换资产都会调它。
	 */
	void SetFractureAsset(UGaussianSplatFractureAsset* InFractureAsset);

	/**
	 * 开始轻量破碎预览：每帧让每块碎片沿「爆炸中心 → 该块」的方向飞散，
	 * 可选叠加重力。走的是渲染器已有的「破裂骨头 delta」通道，不跑 Chaos
	 * 求解，因此开销极小；几何集合与破裂资产都不会被修改。
	 *
	 * @param InRestBoneMatrices 破裂资产的 K 个 rest 骨头矩阵（资产局部空间，
	 *                           索引 0 是几何集合的根，预览里保持不动）
	 * @param InBodyCenter       物体中心（资产局部空间），爆炸中心的基准
	 * @param bInGravity         true = 叠加重力，false = 匀速直线飞散（无重力）
	 * @param InExplosionSpeed   爆炸初速度（cm/s）
	 * @param InCenterOffset     爆炸中心相对物体中心的偏移（cm）
	 */
	void StartFracturePreview(const TArray<FMatrix44f>& InRestBoneMatrices, const FVector& InBodyCenter,
		bool bInGravity, float InExplosionSpeed, const FVector& InCenterOffset = FVector::ZeroVector);

	/** 停止破碎预览，碎片回到 rest（骨头路径关闭） */
	void StopFracturePreview();

	/**
	 * 开始真实 Chaos 破碎预览：在预览场景里现搭一个几何集合组件做刚体模拟，
	 * 碎片真的会互相碰撞、落地。破裂资产的几何集合会被引用（只读），
	 * 解算结果一个字节都不改；预览结束就把现搭的 Actor 销毁。
	 *
	 * 与轻量预览的区别：轻量只推骨头 delta（碎片不会互撞、不会落地停住），
	 * 真实预览是 Chaos 在算，但需要已解算（有骨头绑定）且资产里有几何集合。
	 *
	 * @param InRestBoneMatrices 破裂资产的 K 个 rest 骨头矩阵（资产局部空间），
	 *                           索引 0 是几何集合的根，不参与爆炸
	 * @param InBodyCenter       物体中心（资产局部空间），爆炸中心的基准
	 * @param bInGravity         true = 碎片受重力（会落地停下）
	 * @param InStrength         爆炸强度（cm/s，越远的块越快）
	 * @param InCenterOffset     爆炸中心相对物体中心的偏移（cm）
	 */
	void StartChaosFracturePreview(const TArray<FMatrix44f>& InRestBoneMatrices, const FVector& InBodyCenter,
		bool bInGravity, float InStrength, const FVector& InCenterOffset = FVector::ZeroVector);

	/** 停止真实 Chaos 预览：销毁现搭的几何集合 Actor，碎片回到 rest */
	void StopChaosFracturePreview();

	/** 是否正在破碎预览 */
	bool IsFracturePreviewActive() const { return bFracturePreviewActive; }

	/** 当前预览是不是真实 Chaos 形态（false = 轻量运动学） */
	bool IsFracturePreviewChaos() const { return bFracturePreviewChaos; }

	/** Get the preview actor */
	AGaussianSplatActor* GetPreviewActor() const { return PreviewActor.Get(); }

private:
	/** 推进破碎预览一帧：算 delta、推给预览组件、请求重绘 */
	void UpdateFracturePreview(float DeltaSeconds);

	/** 推进真实 Chaos 预览一帧：等几何集合拆开成独立刚体后施加爆炸冲量 */
	void UpdateChaosFracturePreview(float DeltaSeconds);

	/** 销毁真实 Chaos 预览现搭的几何集合 Actor（幂等） */
	void DestroyChaosPreviewCollection();

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

	/** 预览组件当前渲染的破裂资产（nullptr = 裸源资产） */
	TWeakObjectPtr<UGaussianSplatFractureAsset> CurrentFractureAsset;

	// --- 轻量破碎预览状态（见 StartFracturePreview）---

	/** 预览是否在跑 */
	bool bFracturePreviewActive = false;

	/** 已跑的预览时间（秒），驱动位移与重力项 */
	float FracturePreviewTime = 0.0f;

	/** 是否叠加重力（false = 匀速直线飞散） */
	bool bFracturePreviewGravity = true;

	/** 爆炸初速度（cm/s） */
	float FracturePreviewSpeed = 120.0f;

	/** 物体中心（资产局部空间） */
	FVector FracturePreviewBodyCenter = FVector::ZeroVector;

	/** 爆炸中心相对物体中心的偏移（cm） */
	FVector FracturePreviewCenterOffset = FVector::ZeroVector;

	/** rest 骨头矩阵（资产局部空间，K 个） */
	TArray<FMatrix44f> FracturePreviewRestBones;

	/** 每帧的 delta 暂存（避免每帧分配） */
	TArray<FMatrix44f> FracturePreviewDeltas;

	// --- 真实 Chaos 破碎预览状态（见 StartChaosFracturePreview）---

	/** 当前预览是不是真实 Chaos 形态（false = 轻量运动学） */
	bool bFracturePreviewChaos = false;

	/** 现搭的几何集合 Actor（预览结束即销毁） */
	TWeakObjectPtr<AActor> FracturePreviewCollectionActor;

	/** 现搭的几何集合组件：预览组件的骨头跟随目标 */
	TWeakObjectPtr<UGeometryCollectionComponent> FracturePreviewCollection;

	/** 逐块爆炸初速度（资产局部空间，索引与骨头一致；施加窗口用完后清空） */
	TArray<FVector> FracturePreviewBlastVelocities;

	/** 延迟施加爆炸：先让 CrumbleActiveClusters 把碎片拆成独立刚体（秒） */
	float FracturePreviewBlastDelay = 0.0f;

	/** 爆炸冲量的施加窗口（秒）：窗口内每帧重置一次初速度，保证一定推得动 */
	float FracturePreviewBlastPushTime = 0.0f;
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

	/** 让预览组件渲染某个破裂资产（nullptr = 裸源资产），见客户端同名接口 */
	void SetFractureAsset(UGaussianSplatFractureAsset* InFractureAsset);

	/** 开始轻量破碎预览，见客户端同名接口 */
	void StartFracturePreview(const TArray<FMatrix44f>& InRestBoneMatrices, const FVector& InBodyCenter,
		bool bInGravity, float InExplosionSpeed, const FVector& InCenterOffset = FVector::ZeroVector);

	/** 停止轻量破碎预览 */
	void StopFracturePreview();

	/** 开始真实 Chaos 破碎预览，见客户端同名接口 */
	void StartChaosFracturePreview(const TArray<FMatrix44f>& InRestBoneMatrices, const FVector& InBodyCenter,
		bool bInGravity, float InStrength, const FVector& InCenterOffset = FVector::ZeroVector);

	/** 是否正在破碎预览 */
	bool IsFracturePreviewActive() const;

private:
	/** The viewport client */
	TSharedPtr<FGaussianSplatAssetViewportClient> ViewportClient;
};
