// Copyright OpenSplat4D 2026 All Rights Reserved.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "Interfaces/Interface_CollisionDataProvider.h"
#include "GaussianDataTypes.h"
#include "NanoGSGaussianSplatComponent.generated.h"

class UGaussianSplatAsset;
class UGaussianSplatFractureAsset;
class FGaussianSplatSceneProxy;
class UBodySetup;

/**
 * Collision generation method for Gaussian Splat point clouds
 *
 * NOTE on quality:
 *   - BoundingBox / Voxel / None: implemented and production-ready.
 *   - ConvexHull: a real convex hull. GenerateConvexHull() samples the point
 *     cloud down to CollisionMaxFaces * 4 vertices and hands them to
 *     BodySetup->AggGeom.ConvexElems; the engine's Chaos cooking
 *     (FConvexBuilder::Build, the same path a Static Mesh's auto-generated
 *     convex collision uses) computes the actual hull. The result is the
 *     convex hull of the *sampled* subset, i.e. a slightly-inside-the-real-hull
 *     approximation -- correct behaviour, just lower resolution.
 *   - ConvexDecomposition: a real multi-hull split (GenerateSimplifiedCollision).
 *     The cloud is recursively cut in half along its longest axis and every
 *     piece becomes its own convex element, so a concave asset (statue, room
 *     interior) is covered by several hulls instead of one block that fills
 *     every dent. The hull count is capped by MaxConvexHulls. Unlike V-HACD no
 *     concavity metric drives the split: the pieces are a uniform subdivision
 *     rather than minimal-error ones, so shape coverage is good but the element
 *     count is higher than V-HACD would need for the same result.
 */
UENUM(BlueprintType)
enum class EGaussianCollisionMethod : uint8
{
	/** No collision */
	None UMETA(DisplayName = "无碰撞"),
	/** Convex hull of the sampled points. Real hull, computed by Chaos cooking,
	 *  but only from CollisionMaxFaces * 4 sampled vertices (approximation). */
	ConvexHull UMETA(DisplayName = "凸包"),
	/** Multi-hull split: the cloud is cut into up to MaxConvexHulls pieces and each
	 *  piece becomes its own convex element, so concave assets are covered without
	 *  filling in their dents and holes. */
	ConvexDecomposition UMETA(DisplayName = "凸分解(多凸包)"),
	/** Bounding box collision */
	BoundingBox UMETA(DisplayName = "包围盒"),
	/** 已废弃：旧「体素」方法曾把点云量化到占用格、每格一个盒子。格子分辨率不足
	 *  时形状会方块化，整格覆盖还会带来半格偏位。现在改为逐 splat 几何，与
	 *  SplatSpheres 完全等价（枚举值保留以兼容旧存档）。 */
	Voxel UMETA(DisplayName = "体素(已废弃 → 逐Splat)"),
	/** 已废弃：旧「体素壳(复杂碰撞)」方法曾把破裂资产的体素壳当三角网格碰撞。
	 *  该壳只贴合到体素粒度（方块化 + 偏位），且会随破裂解算反复变化。现在改为
	 *  逐 splat 几何，与 SplatSpheres 完全等价（枚举值保留以兼容旧存档）。
	 *  体素壳本身仍然保留，用于破裂解算 / 骨头绑定 / 网格外剔除。 */
	ProxyMesh UMETA(DisplayName = "体素壳(已废弃 → 逐Splat)"),
	/** 逐 splat 几何（「原生碰撞」，推荐）：每个 splat 一个球体，球心就是 splat
	 *  自己的位置，半径由 splat 自身的旋转/尺度推得（见 SplatSphereRadius，0 =
	 *  自动 = 与该 splat 的可见椭球等体积）。位置与大小都逐一对应，没有量化带来
	 *  的方块化与偏位；球体是解析形状，不像 ConvexHull / ConvexDecomposition
	 *  那样需要 Chaos cooking，编辑器与运行时表现一致。
	 *  消耗由 MaxSplatSpheres 限制：超过上限就按等步长抽稀，半径同步放大到
	 *  cbrt(步长) 倍，保持覆盖范围不变。
	 *  枚举值保留以兼容旧存档；Voxel / ProxyMesh 现在也走同一条路径。 */
	SplatSpheres UMETA(DisplayName = "逐Splat球体(原生)")
};

/**
 * Component for rendering Gaussian Splatting assets in the scene
 */
UCLASS(ClassGroup = (Rendering), meta = (BlueprintSpawnableComponent), hidecategories = (Navigation))
class NANOGS_API UGaussianSplatComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UGaussianSplatComponent(const FObjectInitializer& ObjectInitializer);

	//~ Begin UObject Interface
	virtual void PostLoad() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	//~ End UObject Interface

	//~ Begin UActorComponent Interface
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	//~ End UActorComponent Interface

	//~ Begin UPrimitiveComponent Interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;

	/** Whether to cast shadows */
	virtual bool CastShadow() const { return bCastShadow; }

	/** Get the body setup for collision */
	virtual UBodySetup* GetBodySetup() override;

	/** Collision test */
	virtual bool LineTraceComponent(struct FHitResult& OutHit, const FVector TraceStart, const FVector TraceEnd, const FCollisionQueryParams& TraceParams) override;
	//~ End UPrimitiveComponent Interface

	/** Set the Gaussian Splat asset to render */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅")
	void SetSplatAsset(UGaussianSplatAsset* NewAsset);

	/** Get the currently assigned asset */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅")
	UGaussianSplatAsset* GetSplatAsset() const { return SplatAsset; }

	/** Get the number of splats being rendered */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅")
	int32 GetSplatCount() const;

	/** Rebuild collision based on current settings */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|碰撞")
	void RebuildCollision();

public:
	/** The Gaussian Splat asset to render */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅", meta = (DisplayName = "泼溅资产"))
	TObjectPtr<UGaussianSplatAsset> SplatAsset;

	/** Spherical Harmonic order to use for rendering (0-3). 0=DC only (flat color),
	 *  1-3=progressively more view-dependent color detail.  Higher values produce
	 *  more realistic highlights but increase GPU cost.  Match this to the SH band
	 *  count in your source PLY file.  Tooltip: 调节球谐函数的计算阶数（0=仅基础色，3=完整视图相关颜色）。阶数越高颜色越真实，但 GPU 开销越大。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|质量", meta = (ClampMin = "0", ClampMax = "3", DisplayName = "SH 阶数"))
	int32 SHOrder = 3;

	/** Sort splats every N frames. 1 = every frame (best quality, higher GPU cost).
	 *  Higher values reduce GPU cost but may cause popping during fast camera
	 *  movement.  Increasing this is the #1 performance optimization for large
	 *  splat counts.  Tooltip: 每 N 帧排序一次。1=每帧排序（最佳质量），增大可降低 GPU 开销但可能在快速移动时出现闪烁。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|性能", meta = (ClampMin = "1", ClampMax = "10", DisplayName = "排序间隔（帧）"))
	int32 SortEveryNthFrame = 1;

	/** Global opacity multiplier. 1.0 = as-trained.  Values > 1.0 make all splats
	 *  more opaque (can reduce floaters but may oversaturate).  0.0 = fully transparent.
	 *  Tooltip: 全局不透明度缩放。1.0=训练原始值，>1.0 更不透明可减少浮游物，<1.0 更透明。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|渲染", meta = (ClampMin = "0.0", ClampMax = "2.0", DisplayName = "不透明度缩放"))
	float OpacityScale = 1.0f;

	/** Scale multiplier for splat sizes in world-space.  1.0 = as-trained.
	 *  Higher values make splats larger (softer look), lower values make them
	 *  smaller (sharper but may show gaps).  Tooltip: Splat 尺寸缩放倍数。1.0=训练原始大小，>1.0 更大更模糊，<1.0 更小更锐利但可能出现空隙。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|渲染", meta = (ClampMin = "0.1", ClampMax = "10.0", DisplayName = "Splat 尺寸缩放"))
	float SplatScale = 1.0f;

	/** Gaussian falloff sharpness exponent.  Higher values create tighter, sharper
	 *  splats (less blending between neighbors).  Lower values create softer,
	 *  more blended splats.  Standard 3DGS uses 4.0.  Tooltip: 高斯衰减锐度指数。值越大 splat 边缘越锐利（减少模糊拖影），越小越柔和。标准值为 4.0。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|渲染", meta = (ClampMin = "1.0", ClampMax = "10.0", DisplayName = "高斯锐度"))
	float GaussianSharpness = 4.0f;

	/** Enable frustum culling for better performance */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|性能", DisplayName = "视锥剔除")
	bool bEnableFrustumCulling = true;

	/** Projected error threshold for LOD selection (resolution-independent, like Nanite).
	 *  Lower values = more conservative (keep detail longer, less LOD savings)
	 *  Higher values = more aggressive (switch to LOD sooner, better performance)
	 *  Uses projection-space units. ~0.03 ≈ 32 pixels at 1080p with 90° FOV. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|性能", meta = (ClampMin = "0.001", ClampMax = "1.0", DisplayName = "Nanite 精度（LOD 误差阈值）"))
	float LODErrorThreshold = 0.03f;

	/** 可视范围上限（厘米）。超过该距离的 splat 会被剔除（0 = 不限制）。
	 *  大场景性能优化：配合 Nanite 精度使用，远处内容自动消失。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|性能", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "100000.0", DisplayName = "最大可视距离"))
	float MaxDrawDistance = 0.0f;

	/** 开始淡出的距离（厘米）。0 = 到达最大可视距离时直接硬切。
	 *  设为小于最大可视距离的值可获得平滑的远处淡出效果（如 0.8 × 最大距离）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|性能", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "100000.0", DisplayName = "淡出起始距离", EditCondition = "MaxDrawDistance > 0"))
	float FadeOutStartDistance = 0.0f;

	// ---- 性能参数蓝图接口（便于蓝图/运行时快速调用） ----

	/** 设置 Nanite 精度（LOD 误差阈值）。调大 = 更激进降 LOD = 帧率更高。立即生效。 */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|性能", meta = (DisplayName = "设置 Nanite 精度"))
	void SetNanitePrecision(float InThreshold);

	/** 设置最大可视距离（cm）。超过该距离的 splat 被剔除。0 = 不限制。立即生效。 */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|性能", meta = (DisplayName = "设置最大可视距离"))
	void SetVisibilityRange(float InDistance);

	/** 设置淡出起始距离（cm）。0 = 到达最大可视距离时硬切。立即生效。 */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|性能", meta = (DisplayName = "设置淡出起始距离"))
	void SetFadeOutStart(float InDistance);

	/** 设置深度排序间隔（帧）。相机移动时每 N 帧才完整排序一次。大场景建议 2~4。立即生效。 */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|性能", meta = (DisplayName = "设置排序间隔"))
	void SetSortInterval(int32 InFrames);

	/** 批量应用性能参数（编辑器预览用）：同时设置精度/可视距离/淡出，一次重建。 */
	void ApplyPerformanceSettings(float InNanitePrecision, float InMaxDrawDistance, float InFadeOutStart)
	{
		LODErrorThreshold = FMath::Clamp(InNanitePrecision, 0.001f, 1.0f);
		MaxDrawDistance = FMath::Max(InMaxDrawDistance, 0.0f);
		FadeOutStartDistance = FMath::Max(InFadeOutStart, 0.0f);
		MarkRenderStateDirty();
	}

	/** 阴影投射 - 是否投射阴影到周围场景。
	 *  阴影几何按逐 splat 生成：每个被采样的 splat 一个按其自身四元数定向、按其
	 *  自身三轴尺度缩放的立方体（见 MaxShadowSplatSamples）。位置就是 splat 自己
	 *  的位置，所以投影贴合点云形状，不再有占用格量化造成的方块化与偏位。
	 *  几何只在阴影深度 pass 提交，不写入自身深度/颜色，因此不会压暗模型本身。
	 *  关闭时零开销：几何缓存为空，代理不进入阴影分支。
	 *  注意：阴影几何是静态 GPU 缓冲（一次性建好），所以不随破裂骨头动画。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|阴影", meta = (DisplayName = "投射阴影"))
	bool bCastShadow = false;

	/** 阴影逐 splat 采样的上限。每采样一个 splat 产出 12 个三角形，所以三角形
	 *  总数 = 12 × 采样数，这是一个硬上界。超过上限时按等步长抽稀，几何同步放大
	 *  到 cbrt(步长) 倍，保持轮廓覆盖率。默认 32768 ≈ 39 万三角形。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|阴影", meta = (DisplayName = "阴影采样上限", ClampMin = "256", ClampMax = "1048576", EditCondition = "bCastShadow"))
	int32 MaxShadowSplatSamples = 32768;

	// 已废弃：阴影代理类型（旧的包围盒路径已移除）
	// UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|阴影", meta = (ClampMin = "0", ClampMax = "2", EditCondition = "bCastShadow"))
	// uint8 ShadowProxyDetail = 1;

	// 已废弃：阴影强度缩放（阴影深度 pass 不解算材质，无法按材质调深浅）
	// UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|阴影", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bCastShadow"))
	// float ShadowIntensity = 1.0f;

	/** 阴影几何（源资产局部空间）是否可用。供代理查询，无锁只读。 */
	bool HasShadowGeometry() const { return ShadowShellTriangles.Num() >= 3; }

	/** 取阴影几何（源资产局部空间的三角形汤）。 */
	void GetShadowGeometry(TArray<FVector3f>& OutVertices, TArray<int32>& OutTriangles) const
	{
		OutVertices = ShadowShellVertices;
		OutTriangles = ShadowShellTriangles;
	}

	// ------------------------------------------------------------------
	// 破裂（Chaos GeometryCollection 驱动）
	//
	// 该资产必须在编辑器里执行过"绑定破裂骨头"。绑定后，每帧从下面的
	// 几何集合组件读取各骨头的当前变换，算出 Delta_b = Cur_b · Rest_b⁻¹
	// 推给渲染代理，splat 就会跟随各自所属的碎块一起飞散。
	//
	// 注意：这里刻意用 UObject* 而不是 UGeometryCollectionComponent*。
	// NanoGS 模块在 PostConfigInit 相位加载，早于 GeometryCollectionEngine，
	// 因此运行时只能靠反射（FindFunction + ProcessEvent）读取变换。
	// ------------------------------------------------------------------

	/** 驱动本组件的几何集合组件（必须已在该破裂资产上解算过破裂）。
	 *  留空则表示不做破裂跟随。 */
	UPROPERTY(EditInstanceOnly, BlueprintReadWrite, Category = "高斯泼溅|破裂",
		meta = (DisplayName = "破裂几何集合组件",
			AllowedClasses = "/Script/GeometryCollectionEngine.GeometryCollectionComponent"))
	TObjectPtr<UObject> FractureGeometryCollection = nullptr;

	/** 破裂资产（UGaussianSplatFractureAsset）。指定后：
	 *   - 骨头绑定与断面 splat 从该资产读取（源资产自 v11 起不再持有破裂数据）；
	 *   - 渲染数据取该资产（源点云 + 绑定 + 断面）而不是裸源资产。
	 *  留空 = 按普通 splat 资产渲染，无破裂。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|破裂",
		meta = (DisplayName = "破裂资产",
			AllowedClasses = "/Script/NanoGS.GaussianSplatFractureAsset"))
	TObjectPtr<UGaussianSplatFractureAsset> FractureAsset = nullptr;

	/** 设置破裂资产。会自动把源 splat 资产同步为本组件的渲染资产（若为空）。 */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|破裂", meta = (DisplayName = "设置破裂资产"))
	void SetFractureAsset(UGaussianSplatFractureAsset* NewAsset);

	/** 当前是否有生效的破裂跟随（已指定组件 + 破裂资产已绑定骨头） */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|破裂", meta = (DisplayName = "破裂跟随是否生效"))
	bool IsFractureBindingActive() const;

#if WITH_EDITOR
	/** 编辑器「破碎预览」专用：绕过几何集合，直接把一组骨头 delta 推给渲染代理
	 *  （数组长度必须等于破裂骨头数，索引与绑定的骨头一一对应）。传空数组
	 *  = 结束预览，骨头路径关闭、碎片回到 rest。
	 *  预览期间 TickFractureBoneDeltas 完全让位，不再读写几何集合。 */
	void SetPreviewBoneDeltas(const TArray<FMatrix44f>& InDeltas);

	/** 是否正在编辑器破碎预览中（由 SetPreviewBoneDeltas 控制） */
	bool IsPreviewBoneDeltasActive() const { return bPreviewBoneDeltasActive; }

	/** 编辑器「真实 Chaos 破碎预览」专用：把某个几何集合组件指定为破裂跟随目标
	 *  （预览场景里现搭的那个），并同步组件的 tick 开关 —— TickFractureBoneDeltas
	 *  是靠组件 tick 驱动的，只改 FractureGeometryCollection 属性不会打开它。
	 *  传 nullptr 解除跟随。 */
	void SetFractureTargetForPreview(UObject* InCollectionComponent);
#endif

	/** 碰撞生成方法 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|碰撞", meta = (DisplayName = "碰撞方法"))
	EGaussianCollisionMethod CollisionMethod = EGaussianCollisionMethod::None;

	/** 碰撞体面数 - 用于简化凸包的面数限制 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|碰撞", meta = (ClampMin = "4", ClampMax = "255", EditCondition = "CollisionMethod != EGaussianCollisionMethod::None"))
	int32 CollisionMaxFaces = 32;

	/** 忽略系数 - 稀疏区域的点被忽略的概率 (0=保留所有点, 1=忽略大部分点) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|碰撞", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "CollisionMethod != EGaussianCollisionMethod::None"))
	float IgnoreFactor = 0.5f;

	/** 凸分解输出的最大凸包数（凸分解方法用）。算法每轮挑「包围盒体积最大」的那
	 *  一块，沿它最长的一条轴把它切成两半，直到块数达到该值。
	 *  切面是一个平面，所以同一块的点全落在同一侧 ⇒ 凸包之间不会重叠，凹形区域
	 *  保持中空而不是被填平。块数越多贴合越好，Chaos 里的碰撞体也越多。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|碰撞", meta = (DisplayName = "最多凸包数", ClampMin = "1", ClampMax = "64", EditCondition = "CollisionMethod == EGaussianCollisionMethod::ConvexDecomposition"))
	int32 MaxConvexHulls = 8;

	/** 逐 splat 球体碰撞的球体数量上限（Voxel / ProxyMesh / SplatSpheres 三者共用，
	 *  现已统一为逐 splat 几何）。超过该值时按等步长抽稀，并同步放大球体半径，
	 *  保证覆盖范围不变而刚体开销可控。球体是解析形状，不参与 Chaos cooking，
	 *  所以数量只影响模拟/查询开销。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|碰撞", meta = (DisplayName = "最多球体数", ClampMin = "16", ClampMax = "65536", EditCondition = "CollisionMethod == EGaussianCollisionMethod::Voxel || CollisionMethod == EGaussianCollisionMethod::ProxyMesh || CollisionMethod == EGaussianCollisionMethod::SplatSpheres"))
	int32 MaxSplatSpheres = 2048;

	/** 逐 splat 球体碰撞的球体半径（Voxel / ProxyMesh / SplatSpheres 三者共用，
	 *  现已统一为逐 splat 几何）。0 = 自动，取该 splat 自身的三轴尺度推出与其
	 *  可见椭球等体积的球半径（位置与大小都逐一对应，无量化方块化与偏位）；
	 *  >0 则所有球体统一用该半径覆盖。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|碰撞", meta = (DisplayName = "球体半径(0=自动)", ClampMin = "0.0", EditCondition = "CollisionMethod == EGaussianCollisionMethod::Voxel || CollisionMethod == EGaussianCollisionMethod::ProxyMesh || CollisionMethod == EGaussianCollisionMethod::SplatSpheres"))
	float SplatSphereRadius = 0.0f;

	// ------------------------------------------------------------------
	// 4D playback (only active when the asset has temporal data: Is4D()).
	// Advances CurrentTime on Tick and pushes it to the render proxy,
	// where CalcViewData applies temporal marginalization w(t).
	// For static 3D assets all of this is inert (tick disabled).
	// ------------------------------------------------------------------

	/** Automatically start playback when a 4D asset is registered */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|4D 播放", meta = (DisplayName = "自动播放"))
	bool bAutoPlay = true;

	/** Loop playback between the asset's TimeStart and TimeEnd */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|4D 播放", meta = (DisplayName = "循环播放"))
	bool bLooping = true;

	/** Playback speed in asset time units per second (1.0 = real time of the trained time axis) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|4D 播放", meta = (ClampMin = "0.0", ClampMax = "10.0", DisplayName = "播放速度"))
	float PlayRate = 1.0f;

	/** Current playback time in asset time units (read-only; use SetPlaybackTime to scrub) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "高斯泼溅|4D 播放", meta = (DisplayName = "当前时间"))
	float CurrentTime = 0.f;

	/** Whether playback is currently advancing */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "高斯泼溅|4D 播放", meta = (DisplayName = "播放中"))
	bool bPlaying = false;

	/** Start playback from the current time */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|4D 播放", meta = (DisplayName = "播放"))
	void Play4D();

	/** Pause playback (keeps the current time) */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|4D 播放", meta = (DisplayName = "暂停"))
	void Pause4D();

	/** Stop playback and reset the time to the asset's TimeStart */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|4D 播放", meta = (DisplayName = "停止"))
	void Stop4D();

	/** Scrub to a specific time (asset time units, clamped to the asset's time range) */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|4D 播放", meta = (DisplayName = "设置播放时间"))
	void SetPlaybackTime(float InTime);

	/** Whether this component's asset supports 4D playback */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|4D 播放", meta = (DisplayName = "支持 4D 播放"))
	bool Supports4DPlayback() const;

protected:
	/** Called when the asset changes */
	void OnAssetChanged();

	/** Mark the render state as dirty */
	void MarkRenderStateDirty();

	/** Called when the asset's data changes (e.g., Nanite enabled/disabled) */
	void OnAssetDataChanged(class UGaussianSplatAsset* ChangedAsset);

	/** Subscribe to asset change notifications */
	void SubscribeToAssetChanges();

	/** Unsubscribe from asset change notifications */
	void UnsubscribeFromAssetChanges();

	/** Called when the fracture asset's render data (binding / caps) changes */
	void OnFractureAssetDataChanged(UGaussianSplatFractureAsset* ChangedAsset);

	/** Subscribe to fracture asset change notifications */
	void SubscribeToFractureAssetChanges();

	/** Unsubscribe from fracture asset change notifications */
	void UnsubscribeFromFractureAssetChanges();

	/** Build collision body setup from point cloud data */
	void BuildCollisionBodySetup();

	/**
	 * 当前配置是否真的会产生碰撞几何。仅当 CollisionMethod == None 时为 false。
	 * 保留这一层抽象是为了让「配置切换」与「是否建物理状态」解耦：改方法时先刷
	 * 几何再刷 profile，避免引擎为一个空 BodySetup 建出物理状态。
	 */
	bool HasEffectiveCollision() const;

	/**
	 * 按 CollisionMethod 把碰撞配置切到 BlockAll / NoCollision。
	 *
	 * 必须在几何（BodySetup）就绪之后调用，且刻意与 RebuildCollision() 分离：
	 * RebuildCollision() 会在 OnCreatePhysicsState() → GetBodySetup() 路径里被懒调用，
	 * 若在其中改 profile 会重入物理状态创建。启动阶段的调用点在 OnRegister()。
	 */
	void ApplyCollisionProfile();

	/** Generate convex hull from point cloud positions */
	bool GenerateConvexHull(const TArray<FVector>& Points, TArray<FVector>& OutVertices, TArray<int32>& OutIndices);

	/** Generate simplified collision mesh：真正的多凸包分解（见枚举注释） */
	bool GenerateSimplifiedCollision(const TArray<FVector>& Points, TArray<FVector>& OutVertices, TArray<int32>& OutIndices);

	/** Generate one sphere element per (thinned) splat: 逐 splat 原生碰撞。
	 *  球心 = splat 自己的位置；半径取该 splat 自身的三轴尺度（0 = 自动），
	 *  因此位置与大小都逐一对应，没有体素量化的方块化与偏位。
	 *  Voxel / ProxyMesh / SplatSpheres 三个方法现在都走这条路径。 */
	bool GenerateSplatSpheres(const TArray<FVector>& Points, TArray<FVector>& OutVertices, TArray<int32>& OutIndices);

	/**
	 * 重建阴影几何：逐 splat 生成定向立方体（见 MaxShadowSplatSamples）。
	 *
	 * bCastShadow 关闭时清空缓存，代理侧随即关闭阴影分支（零开销）。
	 * 资产 / 开关 / 采样上限变化时调用。
	 */
	void RebuildShadowGeometry();

	/**
	 * 阴影几何：按步长直接读 bulk 数据，为每个被采样的 splat 输出一个按其自身
	 * 四元数定向、按其自身三轴尺度缩放的立方体（8 顶点 / 12 三角）。
	 *
	 * 位置就是 splat 自己的位置，所以投影贴合点云形状，不再有占用格量化造成的
	 * 方块化与偏位。直接按步长读 PositionBulkData / OtherBulkData，不整份解压
	 * 点云（亿级 splat 也不会因为重建吃掉几个 GB）；采样数由 MaxShadowSplatSamples
	 * 限制，因此三角形数量有上界（12 × 采样数）。
	 *
	 * 输出在源资产局部空间（代理的 local-to-world 一致）。
	 */
	void BuildShadowSplatGeometry();

private:
	/** Enable/disable component tick based on 4D asset + playing state */
	void Update4DTickEnabled();

	/** Push CurrentTime to the scene proxy (no-op if no proxy) */
	void PushCurrentTimeToProxy();

	/**
	 * 破裂跟随：读取几何集合组件的骨头当前变换，算出
	 * Delta_b = Cur_b · Rest_b⁻¹ 并推给渲染代理。
	 * 全身静止（或未生效）时推空数组，渲染路径完全关闭，零每帧开销。
	 */
	void TickFractureBoneDeltas();

	/** Load (once) the asset's K rest bone matrices. False if unavailable. */
	bool EnsureFractureRestMatrices();

	/** Cached rest bone matrices (asset local space, K x FMatrix44f) */
	TArray<FMatrix44f> FractureRestMatrices;

	/** Reused scratch for the per-frame deltas pushed to the proxy */
	TArray<FMatrix44f> FractureBoneDeltas;

	/** Deltas most recently pushed (used to skip uploads once the debris settles) */
	TArray<FMatrix44f> FractureLastPushedDeltas;

	/** 上一帧推给代理的状态：true = 正在跟随破裂 */
	bool bFractureWasActive = false;

	/** 上次推送目标的场景代理。代理重建（如重新绑定骨头）后必须重推一次。 */
	const void* LastFractureProxy = nullptr;

#if WITH_EDITOR
	/** 编辑器破碎预览是否接管了骨头路径（见 SetPreviewBoneDeltas）。
	 *  为真时 TickFractureBoneDeltas 直接返回，不去读写几何集合。 */
	bool bPreviewBoneDeltasActive = false;
#endif

	/** Cached bounds */
	mutable FBoxSphereBounds CachedBounds;
	mutable bool bBoundsCached = false;

	/** Delegate handle for asset change subscription */
	FDelegateHandle AssetChangedDelegateHandle;

	/** Delegate handle for fracture asset change subscription */
	FDelegateHandle FractureAssetChangedDelegateHandle;

	/** Collision body setup */
	UPROPERTY(Transient)
	TObjectPtr<UBodySetup> BodySetup;

	/** Whether collision needs rebuild */
	bool bCollisionDirty = true;

	// ------------------------------------------------------------------
	// Shadow geometry: per-splat oriented boxes as a raw triangle soup
	// (source asset local space). Only filled while bCastShadow is on.
	// ------------------------------------------------------------------

	/** Shadow shell vertex positions */
	TArray<FVector3f> ShadowShellVertices;

	/** Shadow shell triangles (3 indices per triangle) */
	TArray<int32> ShadowShellTriangles;
};