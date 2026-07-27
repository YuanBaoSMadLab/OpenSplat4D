// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "GaussianSplatEditorData.generated.h"

class UGaussianSplatAsset;

/**
 * 编辑模式工具类型。对应工具栏上的按钮。
 * 与 Splatshop 的 actions/ 目录对应：
 *   - RectSelect    → actions/RectSelectAction.h
 *   - SphereSelect  → actions/SphereSelectAction.h
 *   - BrushSelect   → actions/BrushSelectAction.h（基于 SphereSelect 累积）
 *   - MoveTool      → actions/GizmoAction.h（后续阶段实现）
 */
UENUM(BlueprintType)
enum class EGaussianEditTool : uint8
{
	/** 无活动工具（仅轨道相机） */
	None        UMETA(DisplayName = "无"),
	/** 框选：左键拖拽矩形选区 */
	RectSelect  UMETA(DisplayName = "框选"),
	/** 球选：左键点击，选中点击点周围球形范围内的 splat */
	SphereSelect UMETA(DisplayName = "球选"),
	/** 笔刷：左键按住拖动，连续球选累积 */
	BrushSelect UMETA(DisplayName = "笔刷"),
};

/**
 * 编辑会话数据：持有选中 / 隐藏 flags，提供选择 / 删除 / 隔离 API。
 *
 * 这个对象是 Transient 的 —— 不参与资产序列化，只在编辑器打开资产期间存在。
 * 选中状态保存在这里，避免污染 UGaussianSplatAsset 本身。
 *
 * 线程安全：所有 API 必须在 GameThread 调用（因为会修改 BulkData / 触发
 * MarkRenderStateDirty）。选择算法在内部用 ParallelFor 加速只读遍历，
 * 但写 SelectedFlags 时回到主线程串行提交。
 */
UCLASS(Transient, BlueprintType)
class UGaussianSplatEditorData : public UObject
{
	GENERATED_BODY()

public:
	UGaussianSplatEditorData();

	/** 初始化：绑定到目标资产，分配 flags 数组到 SplatCount 大小 */
	void Initialize(UGaussianSplatAsset* InAsset);

	// ------------------------------------------------------------------------
	// 选择 API
	// ------------------------------------------------------------------------

	/** 选中所有 splat */
	void SelectAll();

	/** 取消所有选择 */
	void DeselectAll();

	/** 反选 */
	void InvertSelection();

	/** 按索引列表设置选中（替换模式） */
	void SetSelectionByIndices(const TArray<int32>& Indices, bool bAddToExisting = false);

	/** 按索引列表添加到当前选择 */
	void AddToSelection(const TArray<int32>& Indices) { SetSelectionByIndices(Indices, true); }

	/** 获取选中数量 */
	int32 GetSelectedCount() const;

	/** 获取选中索引列表（用于 Details 面板显示 / 调试） */
	TArray<int32> GetSelectedIndices() const;

	/** 判断某个 splat 是否被选中 */
	FORCEINLINE bool IsSelected(int32 SplatIndex) const
	{
		return SelectedFlags.IsValidIndex(SplatIndex) && SelectedFlags[SplatIndex];
	}

	// ------------------------------------------------------------------------
	// 隐藏 / 隔离 API
	// ------------------------------------------------------------------------

	/** 隐藏当前选中的 splat */
	void HideSelected();

	/** 隔离当前选中的 splat（其他全部隐藏） */
	void IsolateSelected();

	/** 显示所有 splat（清除隐藏状态） */
	void ShowAll();

	/** 判断某个 splat 是否被隐藏 */
	FORCEINLINE bool IsHidden(int32 SplatIndex) const
	{
		return HiddenFlags.IsValidIndex(SplatIndex) && HiddenFlags[SplatIndex];
	}

	// ------------------------------------------------------------------------
	// 删除 API
	// ------------------------------------------------------------------------

	/**
	 * 删除当前选中的 splat。
	 *
	 * 这是不可逆操作（除非用户撤销），会修改资产的 BulkData。
	 * 实现策略：
	 *   1. 从 PositionBulkData / OtherBulkData / SHBulkData 中移除选中项
	 *   2. 更新 SplatCount
	 *   3. 重建 BoundingBox
	 *   4. 删除 ColorTextureBulkData 中对应行（按 splat 索引取模）
	 *   5. 清空 SelectedFlags / HiddenFlags 并重新分配大小
	 *   6. 触发资产 MarkRenderStateDirty（通过 OnAssetModified 委托）
	 *
	 * 返回实际删除的 splat 数量。如果资产未初始化或没有选中，返回 0。
	 */
	int32 DeleteSelected();

	// ------------------------------------------------------------------------
	// 状态查询
	// ------------------------------------------------------------------------

	/** 当前激活的编辑工具 */
	EGaussianEditTool GetActiveTool() const { return ActiveTool; }

	/** 设置当前激活的编辑工具 */
	void SetActiveTool(EGaussianEditTool NewTool) { ActiveTool = NewTool; }

	/** 笔刷半径（世界单位，cm），用于 BrushSelect / SphereSelect */
	float GetBrushRadius() const { return BrushRadius; }
	void SetBrushRadius(float InRadius) { BrushRadius = FMath::Max(1.0f, InRadius); }

	/** 绑定的资产（弱引用，防止悬挂） */
	UGaussianSplatAsset* GetAsset() const { return Asset.Get(); }

	/** 资产是否被修改（用于触发 viewport 刷新） */
	bool IsDirty() const { return bDirty; }

	/** 清除 dirty 标志（viewport 刷新后调用） */
	void ClearDirty() { bDirty = false; }

	// ------------------------------------------------------------------------
	// 选择算法（静态工具函数）
	// ------------------------------------------------------------------------
	//
	// 这些函数把选择几何（射线 / 球 / 屏幕矩形）转换为 splat 索引列表。
	// 它们不修改 EditorData 状态，只返回索引；调用者用 SetSelectionByIndices
	// 应用结果。这样保持算法无状态，便于测试和复用。
	//
	// 性能：对于 100 万 splat，单线程遍历约 10ms，对编辑器交互可接受。
	// 如果未来需要支持更大模型，可以改用 ParallelFor + BVH 加速。
	// ------------------------------------------------------------------------

	/**
	 * 球形选择：返回世界空间中距离 SphereCenter 在 SphereRadius 内的所有 splat 索引。
	 * 用于 SphereSelect / BrushSelect 工具。
	 *
	 * @param  Asset         目标资产（必须已初始化）
	 * @param  SphereCenter  球心（世界空间）
	 * @param  SphereRadius  球半径（世界空间，cm）
	 * @param  OutIndices    [out] 选中的索引列表
	 * @return 实际选中的数量
	 */
	static int32 SphereSelect(
		UGaussianSplatAsset* Asset,
		const FVector& SphereCenter,
		float SphereRadius,
		TArray<int32>& OutIndices);

	/**
	 * 屏幕空间矩形选择：把每个 splat 投影到屏幕，返回落在矩形内的所有 splat 索引。
	 * 用于 RectSelect 工具。
	 *
	 * @param  Asset           目标资产
	 * @param  ViewOrigin      相机位置（世界空间）
	 * @param  ViewDir         相机方向（世界空间，归一化）
	 * @param  ViewRight       相机右方向（世界空间，归一化）
	 * @param  ViewUp          相机上方向（世界空间，归一化）
	 * @param  FOVRadians      垂直 FOV（弧度）
	 * @param  AspectRatio     宽高比
	 * @param  ScreenRectMin   矩形左上角（屏幕空间，[-1,1]）
	 * @param  ScreenRectMax   矩形右下角（屏幕空间，[-1,1]）
	 * @param  OutIndices      [out] 选中的索引列表
	 * @return 实际选中的数量
	 */
	static int32 RectSelect(
		UGaussianSplatAsset* Asset,
		const FVector& ViewOrigin,
		const FVector& ViewDir,
		const FVector& ViewRight,
		const FVector& ViewUp,
		float FOVRadians,
		float AspectRatio,
		const FVector2D& ScreenRectMin,
		const FVector2D& ScreenRectMax,
		TArray<int32>& OutIndices);

	// ------------------------------------------------------------------------
	// 委托
	// ------------------------------------------------------------------------

	/** 当选择 / 隐藏 / 删除发生变化时广播，让 viewport 刷新 */
	DECLARE_MULTICAST_DELEGATE(FOnEditorDataChanged);
	FOnEditorDataChanged OnChanged;

private:
	/** 标记 dirty 并广播 */
	void MarkDirty();

	/** 持有的资产（弱引用） */
	UPROPERTY(Transient)
	TWeakObjectPtr<UGaussianSplatAsset> Asset;

	/** 选中标志位（与资产 SplatCount 同长度） */
	TBitArray<> SelectedFlags;

	/** 隐藏标志位（与资产 SplatCount 同长度） */
	TBitArray<> HiddenFlags;

	/** 当前激活的工具 */
	UPROPERTY(Transient)
	EGaussianEditTool ActiveTool = EGaussianEditTool::None;

	/** 笔刷半径（cm） */
	UPROPERTY(Transient)
	float BrushRadius = 50.0f;

	/** dirty 标志（选择 / 隐藏 / 删除变化时设为 true） */
	bool bDirty = false;
};
