// Copyright Epic Games, Inc. All Rights Reserved.
// Copyright OpenSplat4D 2026 All Rights Reserved.

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
 *   - ConvexDecomposition: NOT a decomposition. It currently falls through to
 *     the single convex hull above; there is no V-HACD split, so a concave
 *     asset (statue, room interior) is not approximated well. Prefer Voxel for
 *     those until real decomposition is integrated.
 */
UENUM(BlueprintType)
enum class EGaussianCollisionMethod : uint8
{
	/** No collision */
	None UMETA(DisplayName = "无碰撞"),
	/** Convex hull of the sampled points. Real hull, computed by Chaos cooking,
	 *  but only from CollisionMaxFaces * 4 sampled vertices (approximation). */
	ConvexHull UMETA(DisplayName = "凸包"),
	/** Single convex hull, NOT a real convex decomposition (no V-HACD split). */
	ConvexDecomposition UMETA(DisplayName = "凸分解(单凸包)"),
	/** Bounding box collision */
	BoundingBox UMETA(DisplayName = "包围盒"),
	/** Voxel-based collision (approximate shape) */
	Voxel UMETA(DisplayName = "体素"),
	/** The fracture asset's collision voxel shell, cooked as a real triangle
	 *  mesh ("complex as simple").  Unlike Voxel / ConvexHull this keeps the
	 *  concave silhouette and hollow interiors, because nothing is filled in.
	 *  Requires a fracture asset that owns a voxel shell whose
	 *  bUseVoxelShellAsCollision is on; otherwise the component ends up with
	 *  no collision at all ("仅用于破裂"). */
	ProxyMesh UMETA(DisplayName = "体素壳(复杂碰撞)")
};

/**
 * Component for rendering Gaussian Splatting assets in the scene
 *
 * Implements IInterface_CollisionDataProvider so UBodySetup, whose outer is
 * this component, can pull a triangle mesh out of it and cook it — the same
 * route USplineMeshComponent uses.  Only the ProxyMesh collision method feeds
 * it (see GetPhysicsTriMeshData).
 */
UCLASS(ClassGroup = (Rendering), meta = (BlueprintSpawnableComponent), hidecategories = (Navigation))
class NANOGS_API UGaussianSplatComponent : public UPrimitiveComponent, public IInterface_CollisionDataProvider
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

	//~ Begin IInterface_CollisionDataProvider Interface
	/** Hand the cached voxel shell to the physics cooker. Fails (false) unless
	 *  the ProxyMesh method is active and a shell is loaded, so a BodySetup that
	 *  somehow cooks while another method is selected still comes out empty. */
	virtual bool GetPhysicsTriMeshData(struct FTriMeshCollisionData* CollisionData, bool bInUseAllTriData) override;

	virtual bool GetTriMeshSizeEstimates(struct FTriMeshCollisionDataEstimates& OutTriMeshEstimates, bool bInUseAllTriData) const override;

	virtual bool ContainsPhysicsTriMeshData(bool bInUseAllTriData) const override;

	/** DDC key component: the shell's content hash, so re-solving the fracture
	 *  cannot be mistaken for a cache hit. */
	virtual void GetMeshId(FString& OutMeshId) override;
	//~ End IInterface_CollisionDataProvider Interface

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
	 *  开启后使用破裂资产的体素壳（封闭流形块状外壳）作为阴影几何，
	 *  形状贴合点云本体，只在阴影深度 pass 提交，不写入自身深度/颜色，
	 *  因此不会压暗模型本身。需要指定 FractureAsset 且该资产已有体素壳。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|阴影", meta = (DisplayName = "投射阴影"))
	bool bCastShadow = false;

	// 已废弃：阴影代理类型（包围盒路径已移除，只用体素壳）
	// UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|阴影", meta = (ClampMin = "0", ClampMax = "2", EditCondition = "bCastShadow"))
	// uint8 ShadowProxyDetail = 1;

	// 已废弃：阴影强度缩放（阴影深度 pass 不解算材质，无法按材质调深浅）
	// UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|阴影", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bCastShadow"))
	// float ShadowIntensity = 1.0f;

	/** 体素壳阴影几何（源资产局部空间）是否可用。供代理查询，无锁只读。 */
	bool HasShadowGeometry() const { return ShadowShellTriangles.Num() >= 3; }

	/** 取体素壳阴影几何（源资产局部空间的三角形汤）。 */
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

	/** 碰撞生成方法 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|碰撞", meta = (DisplayName = "碰撞方法"))
	EGaussianCollisionMethod CollisionMethod = EGaussianCollisionMethod::None;

	/** 碰撞体面数 - 用于简化凸包的面数限制 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|碰撞", meta = (ClampMin = "4", ClampMax = "255", EditCondition = "CollisionMethod != EGaussianCollisionMethod::None"))
	int32 CollisionMaxFaces = 32;

	/** 忽略系数 - 稀疏区域的点被忽略的概率 (0=保留所有点, 1=忽略大部分点) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|碰撞", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "CollisionMethod != EGaussianCollisionMethod::None"))
	float IgnoreFactor = 0.5f;

	/** 体素大小 - 用于体素碰撞方法 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|碰撞", meta = (ClampMin = "1.0", ClampMax = "100.0", EditCondition = "CollisionMethod == EGaussianCollisionMethod::Voxel"))
	float VoxelSize = 10.0f;

	/** 体素碰撞生成的盒子数量上限。占用的格子超过该值时，会以 VoxelSize 为起点
	 *  成倍放大格子尺寸重算，直到占用格数落进预算，从而保证盒子覆盖整个点云
	 *  而不是只覆盖一小块。实际使用的格子尺寸见 EffectiveVoxelSize。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "高斯泼溅|碰撞", meta = (ClampMin = "16", ClampMax = "65536", EditCondition = "CollisionMethod == EGaussianCollisionMethod::Voxel"))
	int32 MaxCollisionElements = 2048;

	/** 上一次体素碰撞实际使用的格子尺寸（只读）。若因触及 MaxCollisionElements
	 *  或网格内存上限被自动放大，该值会大于 VoxelSize。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "高斯泼溅|碰撞")
	float EffectiveVoxelSize = 0.0f;

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

	/** Build collision body setup from point cloud data */
	void BuildCollisionBodySetup();

	/**
	 * 当前配置是否真的会产生碰撞几何。
	 *
	 * 与 "CollisionMethod != None" 的区别只在 ProxyMesh：该方法的几何来自破裂
	 * 资产的体素壳，壳不存在、或壳被标为"仅用于破裂"（bUseVoxelShellAsCollision
	 * 关闭）时组件必须完全没有碰撞，profile 也得留在 NoCollision，
	 * 否则引擎会为一个空 BodySetup 建出物理状态。
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

	/** Generate simplified collision mesh */
	bool GenerateSimplifiedCollision(const TArray<FVector>& Points, TArray<FVector>& OutVertices, TArray<int32>& OutIndices);

	/** Generate voxel-based collision */
	bool GenerateVoxelCollision(const TArray<FVector>& Points, TArray<FVector>& OutVertices, TArray<int32>& OutIndices);

	/**
	 * 重建阴影几何：从破裂资产的体素壳取三角形汤并缓存。
	 *
	 * bCastShadow 关闭、未指定破裂资产、或壳读不到时清空缓存，
	 * 代理侧随即关闭阴影分支（零开销）。资产/开关变化时调用。
	 */
	void RebuildShadowGeometry();

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

	/** Cached bounds */
	mutable FBoxSphereBounds CachedBounds;
	mutable bool bBoundsCached = false;

	/** Delegate handle for asset change subscription */
	FDelegateHandle AssetChangedDelegateHandle;

	/** Collision body setup */
	UPROPERTY(Transient)
	TObjectPtr<UBodySetup> BodySetup;

	/** Whether collision needs rebuild */
	bool bCollisionDirty = true;

	// ------------------------------------------------------------------
	// ProxyMesh collision source: the fracture asset's voxel shell, cached
	// as a triangle soup so the cooker can read it without re-locking the
	// bulk data. Positions are in source asset local space (the same space
	// the other collision methods build in). Empty for every other method.
	// ------------------------------------------------------------------

	/** Shell vertex positions (source asset local space) */
	TArray<FVector3f> VoxelShellVertices;

	/** Shell triangles */
	TArray<FTriIndices> VoxelShellIndices;

	// ------------------------------------------------------------------
	// Shadow geometry: the fracture asset's voxel shell as a raw triangle
	// soup (source asset local space). Only filled while bCastShadow is on.
	// ------------------------------------------------------------------

	/** Shadow shell vertex positions */
	TArray<FVector3f> ShadowShellVertices;

	/** Shadow shell triangles (3 indices per triangle) */
	TArray<int32> ShadowShellTriangles;
};