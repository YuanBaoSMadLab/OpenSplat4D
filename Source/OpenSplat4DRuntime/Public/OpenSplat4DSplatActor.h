#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "OpenSplat4DPointCloud.h"
#include "OpenSplat4DSplatActor.generated.h"

class UBoxComponent;

/**
 * 自研渲染管线主 Actor：基于 UInstancedStaticMeshComponent 一次性渲染所有 splat。
 *
 * v12 + Selection Editor: Masked Circular Billboard + WPO + Per-Point Radius + Distance LOD
 *   - WPO billboard 面向相机，圆形硬边遮罩
 *   - 每 splat 大小来自点云 3DGS scale（MaxSigma * 3.0）× PerPointSizeScale
 *   - Tick 驱动距离 LOD：远处放大保屏幕尺寸 + dither 降密度
 *   - 编辑模式：可视化Box选区，可在Details面板调整选区粒子大小
 */
UCLASS(CollapseCategories, ConversionRoot)
class OPENSPLAT4DRUNTIME_API AOpenSplat4DSplatActor : public AActor
{
	GENERATED_BODY()
public:
	AOpenSplat4DSplatActor(const FObjectInitializer& ObjectInitializer);

	/** 渲染的点云资产 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D")
	TObjectPtr<UOpenSplat4DPointCloud> PointCloud;

	/** Splat 尺寸缩放因子。默认 1.0（标准 3DGS 尺度）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D",
		meta = (ClampMin = "0.001", ClampMax = "100.0"))
	float SplatScale = 1.0f;

	/** 实例渲染的根组件 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "OpenSplat4D")
	TObjectPtr<UInstancedStaticMeshComponent> SplatComponent;

	// ---- 选区编辑工具 ----

	/** 开启粒子编辑模式：显示选区Box，可调整框内粒子大小 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D|Selection Editor")
	bool bEditMode = false;

	/** 选区内粒子的预览大小缩放（滑动实时预览）。应用后才写入点云资产。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D|Selection Editor",
		meta = (ClampMin = "0.01", ClampMax = "20.0", UIMin = "0.1", UIMax = "10.0", EditCondition = "bEditMode"))
	float SelectionPreviewScale = 1.0f;

	/** 当前Box选区内的粒子数量（只读） */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "OpenSplat4D|Selection Editor",
		meta = (EditCondition = "bEditMode"))
	int32 SelectedPointCount = 0;

	/** 将当前预览缩放应用到选区内的所有粒子（写入点云资产PerPointSizeScale） */
	UFUNCTION(CallInEditor, Category = "OpenSplat4D|Selection Editor")
	void ApplyScaleToSelection();

	/** 重置预览缩放为1.0（不修改点云数据） */
	UFUNCTION(CallInEditor, Category = "OpenSplat4D|Selection Editor")
	void ResetSelectionPreview();

	/** 选中所有粒子（将Box设为包围整个点云） */
	UFUNCTION(CallInEditor, Category = "OpenSplat4D|Selection Editor")
	void SelectAllPoints();

	/** 重置所有粒子大小为1.0（清除所有PerPointSizeScale编辑） */
	UFUNCTION(CallInEditor, Category = "OpenSplat4D|Selection Editor")
	void ResetAllPointSizes();

	virtual void Tick(float DeltaTime) override;
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostActorCreated() override;
	virtual void PostLoad() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	void SetPointCloud(UOpenSplat4DPointCloud* InCloud);

	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	UOpenSplat4DPointCloud* GetPointCloud() const { return PointCloud; }

	UInstancedStaticMeshComponent* GetSplatComponent() const { return SplatComponent; }

protected:
	void RebuildInstances();
	void UpdateSelectionBoxVisibility();
	void UpdateSelectedCount();

	UPROPERTY(VisibleAnywhere, Category = "OpenSplat4D|Selection Editor")
	TObjectPtr<UBoxComponent> SelectionBox;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> DynMaterial;

	float CurrentLODScale;
	float CurrentDitherFrac;
	TArray<bool> CachedSelected;
	bool bSelectionDirty;
	uint8 bNeedsRebuild : 1 = 0;

	FVector LastSelectionBoxPos = FVector::ZeroVector;
	FQuat LastSelectionBoxRot = FQuat::Identity;
	FVector LastSelectionBoxExtent = FVector::ZeroVector;

	void OnPointCloudChanged();
	FDelegateHandle OnPointsChangedHandle;
};
