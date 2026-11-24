// Copyright OpenSplat4D 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
// UE 5.9 起 UHT 对「仅前置声明、未见定义」的 UPROPERTY 类型会猜错跨模块 API 宏
// （UGaussianSplatComponent 被猜成不存在的 GAUSSIANSPLAT_API），必须 include 定义头。
// 本模块 Build.cs 已依赖 NanoGS，include 无额外代价。
#include "NanoGSGaussianSplatComponent.h"
#include "GaussianSplatFractureActor.generated.h"

class UGaussianSplatFractureAsset;

/**
 * 可拖入场景的「高斯泼溅破裂」Actor —— 破裂资产的运行时门面。
 *
 * 与 Chaos 资产拖进场景得到的 Actor 等价：拥有一个普通的
 * UGeometryCollectionComponent，蓝图里可以直接对它用 Chaos 自带的破裂节点
 * （ApplyBreakingLinearVelocity / CrumbleActiveClusters / RemoveAllAnchors ...）；
 * 额外挂一个 UGaussianSplatComponent 渲染源 splat 点云，并按骨头 Delta 跟随
 * 碎块飞散。
 *
 * 两个组件同挂在 Root 下、相对变换都是单位阵，因此「几何集合组件空间」与
 * 「splat 组件的本地空间」重合 —— 骨头 Delta 才能直接作用在资产本地坐标的
 * splat 上。
 *
 * 相位说明：本模块是 Default（见 OpenSplat4DFracture.Build.cs），所以这里能
 * 硬依赖 GeometryCollectionEngine；NanoGS / OpenSplat4DRuntime 是
 * PostConfigInit，只能走反射，两者互不影响。
 */
UCLASS(Blueprintable, ClassGroup = (Rendering), meta = (DisplayName = "高斯泼溅破裂 Actor"))
class OPENSPLAT4DFRACTURE_API AGaussianSplatFractureActor : public AActor
{
	GENERATED_BODY()

public:
	AGaussianSplatFractureActor();

	//~ Begin AActor Interface
	virtual void OnConstruction(const FTransform& Transform) override;
	// 关卡重新加载（编辑器关掉再开 / 打包后进游戏）不会走 OnConstruction 或
	// PostInitializeComponents（二者只有 SpawnActor 时才会触发），但一定会走
	// 组件注册完成回调。骨头来源与 tick 开关都不进存档，必须在这里重连。
	virtual void PostRegisterAllComponents() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	//~ End AActor Interface

	/** 指定破裂资产：把几何集合装进组件、把源 splat 装进 splat 组件，并接上骨头绑定。
	 *  传 nullptr 退回普通（无破裂）状态。 */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|破裂", meta = (DisplayName = "设置破裂资产"))
	void SetFractureAsset(UGaussianSplatFractureAsset* NewAsset);

	/** 当前破裂资产 */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|破裂", meta = (DisplayName = "获取破裂资产"))
	UGaussianSplatFractureAsset* GetFractureAsset() const;

	/** 复位破裂：碎块回到未破碎状态、物理状态重置。 */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|破裂", meta = (DisplayName = "复位破裂"))
	void ResetFracture();

	/** 幂等重连：破裂资产 -> 几何集合组件 / splat 组件 / 骨头绑定。
	 *  改了破裂资产，或想在运行时手动重连时调用。 */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|破裂", meta = (DisplayName = "同步破裂绑定"))
	void SyncFractureBinding();

	/** 破裂的物理与渲染载体；蓝图里直接对它用 Chaos 的破裂节点。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "高斯泼溅|破裂",
		meta = (DisplayName = "破裂几何集合组件"))
	TObjectPtr<UGeometryCollectionComponent> GeometryCollectionComponent;

	/** 渲染源 splat 点云、并跟随碎块运动的组件。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "高斯泼溅|破裂",
		meta = (DisplayName = "高斯泼溅组件"))
	TObjectPtr<UGaussianSplatComponent> GaussianSplatComponent;
};
