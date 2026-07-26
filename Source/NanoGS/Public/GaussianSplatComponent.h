// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "GaussianDataTypes.h"
#include "GaussianSplatComponent.generated.h"

class UGaussianSplatAsset;
class FGaussianSplatSceneProxy;
class UBodySetup;

/**
 * Collision generation method for Gaussian Splat point clouds
 */
UENUM(BlueprintType)
enum class EGaussianCollisionMethod : uint8
{
	/** No collision */
	None UMETA(DisplayName = "无碰撞"),
	/** Convex hull of all points */
	ConvexHull UMETA(DisplayName = "凸包"),
	/** Simplified convex decomposition (multiple convex hulls) */
	ConvexDecomposition UMETA(DisplayName = "凸分解"),
	/** Bounding box collision */
	BoundingBox UMETA(DisplayName = "包围盒"),
	/** Voxel-based collision (approximate shape) */
	Voxel UMETA(DisplayName = "体素")
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
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
	void SetSplatAsset(UGaussianSplatAsset* NewAsset);

	/** Get the currently assigned asset */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
	UGaussianSplatAsset* GetSplatAsset() const { return SplatAsset; }

	/** Get the number of splats being rendered */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting")
	int32 GetSplatCount() const;

	/** Rebuild collision based on current settings */
	UFUNCTION(BlueprintCallable, Category = "Gaussian Splatting|Collision")
	void RebuildCollision();

public:
	/** The Gaussian Splat asset to render */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting")
	TObjectPtr<UGaussianSplatAsset> SplatAsset;

	/** Spherical Harmonic order to use for rendering (0-3). Higher = more color detail but slower. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Quality", meta = (ClampMin = "0", ClampMax = "3"))
	int32 SHOrder = 3;

	/** Sort splats every N frames. 1 = every frame. Higher values reduce GPU cost but may cause artifacts during fast camera movement. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Performance", meta = (ClampMin = "1", ClampMax = "10"))
	int32 SortEveryNthFrame = 1;

	/** Global opacity multiplier */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Rendering", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float OpacityScale = 1.0f;

	/** Scale multiplier for splat sizes */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Rendering", meta = (ClampMin = "0.1", ClampMax = "10.0"))
	float SplatScale = 1.0f;

	/** Enable frustum culling for better performance */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Performance")
	bool bEnableFrustumCulling = true;

	/** Projected error threshold for LOD selection (resolution-independent, like Nanite).
	 *  Lower values = more conservative (keep detail longer, less LOD savings)
	 *  Higher values = more aggressive (switch to LOD sooner, better performance)
	 *  Uses projection-space units. ~0.03 ≈ 32 pixels at 1080p with 90° FOV. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|Performance", meta = (ClampMin = "0.001", ClampMax = "1.0"))
	float LODErrorThreshold = 0.03f;

	/** 阴影投射 - 是否投射阴影到周围场景 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|阴影", meta = (DisplayName = "投射阴影"))
	bool bCastShadow = false;

	/** 阴影代理类型 - 控制阴影的精度和性能 (0=包围盒, 1=凸包, 2=完整) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|阴影", meta = (ClampMin = "0", ClampMax = "2", EditCondition = "bCastShadow"))
	uint8 ShadowProxyDetail = 1;

	/** 阴影强度缩放 - 控制阴影的深浅 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|阴影", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "bCastShadow"))
	float ShadowIntensity = 1.0f;

	/** 碰撞生成方法 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|碰撞", meta = (DisplayName = "碰撞方法"))
	EGaussianCollisionMethod CollisionMethod = EGaussianCollisionMethod::None;

	/** 碰撞体面数 - 用于简化凸包的面数限制 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|碰撞", meta = (ClampMin = "4", ClampMax = "255", EditCondition = "CollisionMethod != EGaussianCollisionMethod::None"))
	int32 CollisionMaxFaces = 32;

	/** 忽略系数 - 稀疏区域的点被忽略的概率 (0=保留所有点, 1=忽略大部分点) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|碰撞", meta = (ClampMin = "0.0", ClampMax = "1.0", EditCondition = "CollisionMethod != EGaussianCollisionMethod::None"))
	float IgnoreFactor = 0.5f;

	/** 体素大小 - 用于体素碰撞方法 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaussian Splatting|碰撞", meta = (ClampMin = "1.0", ClampMax = "100.0", EditCondition = "CollisionMethod == EGaussianCollisionMethod::Voxel"))
	float VoxelSize = 10.0f;

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

	/** Generate convex hull from point cloud positions */
	bool GenerateConvexHull(const TArray<FVector>& Points, TArray<FVector>& OutVertices, TArray<int32>& OutIndices);

	/** Generate simplified collision mesh */
	bool GenerateSimplifiedCollision(const TArray<FVector>& Points, TArray<FVector>& OutVertices, TArray<int32>& OutIndices);

	/** Generate voxel-based collision */
	bool GenerateVoxelCollision(const TArray<FVector>& Points, TArray<FVector>& OutVertices, TArray<int32>& OutIndices);

private:
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
};
