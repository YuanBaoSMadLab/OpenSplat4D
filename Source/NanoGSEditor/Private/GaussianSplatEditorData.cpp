// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatEditorData.h"
#include "GaussianSplatAsset.h"

#define LOCTEXT_NAMESPACE "GaussianSplatEditorData"

UGaussianSplatEditorData::UGaussianSplatEditorData()
{
}

void UGaussianSplatEditorData::Initialize(UGaussianSplatAsset* InAsset)
{
	Asset = InAsset;

	// 根据 SplatCount 分配 flags 数组大小。如果资产未初始化（SplatCount=0），
	// flags 数组保持空，所有 API 会安全地无操作。
	const int32 Count = InAsset ? InAsset->GetSplatCount() : 0;
	SelectedFlags.Init(false, Count);
	HiddenFlags.Init(false, Count);

	// 重置工具状态
	ActiveTool = EGaussianEditTool::None;
	BrushRadius = 50.0f;
	bDirty = false;
}

// ============================================================================
// 选择 API
// ============================================================================

void UGaussianSplatEditorData::SelectAll()
{
	if (!Asset.IsValid()) return;

	// 把所有非隐藏的 splat 标记为选中（隐藏的不选）
	const int32 Count = Asset->GetSplatCount();
	if (Count <= 0) return;

	for (int32 i = 0; i < Count; ++i)
	{
		if (!IsHidden(i))
		{
			SelectedFlags[i] = true;
		}
	}
	MarkDirty();
}

void UGaussianSplatEditorData::DeselectAll()
{
	if (!Asset.IsValid()) return;

	const int32 Count = Asset->GetSplatCount();
	for (int32 i = 0; i < Count; ++i)
	{
		SelectedFlags[i] = false;
	}
	MarkDirty();
}

void UGaussianSplatEditorData::InvertSelection()
{
	if (!Asset.IsValid()) return;

	const int32 Count = Asset->GetSplatCount();
	for (int32 i = 0; i < Count; ++i)
	{
		if (!IsHidden(i))  // 隐藏的 splat 不参与反选
		{
			SelectedFlags[i] = !SelectedFlags[i];
		}
	}
	MarkDirty();
}

void UGaussianSplatEditorData::SetSelectionByIndices(const TArray<int32>& Indices, bool bAddToExisting)
{
	if (!Asset.IsValid()) return;

	if (!bAddToExisting)
	{
		DeselectAll();
	}

	for (const int32 Idx : Indices)
	{
		if (SelectedFlags.IsValidIndex(Idx))
		{
			SelectedFlags[Idx] = true;
		}
	}
	MarkDirty();
}

int32 UGaussianSplatEditorData::GetSelectedCount() const
{
	int32 Count = 0;
	for (int32 i = 0; i < SelectedFlags.Num(); ++i)
	{
		if (SelectedFlags[i]) ++Count;
	}
	return Count;
}

TArray<int32> UGaussianSplatEditorData::GetSelectedIndices() const
{
	TArray<int32> Result;
	for (int32 i = 0; i < SelectedFlags.Num(); ++i)
	{
		if (SelectedFlags[i]) Result.Add(i);
	}
	return Result;
}

// ============================================================================
// 隐藏 / 隔离 API
// ============================================================================

void UGaussianSplatEditorData::HideSelected()
{
	if (!Asset.IsValid()) return;

	const int32 Count = Asset->GetSplatCount();
	for (int32 i = 0; i < Count; ++i)
	{
		if (SelectedFlags[i])
		{
			HiddenFlags[i] = true;
			SelectedFlags[i] = false;  // 隐藏后取消选中
		}
	}
	MarkDirty();
}

void UGaussianSplatEditorData::IsolateSelected()
{
	if (!Asset.IsValid()) return;

	const int32 Count = Asset->GetSplatCount();
	for (int32 i = 0; i < Count; ++i)
	{
		// 选中 → 显示，未选中 → 隐藏
		HiddenFlags[i] = !SelectedFlags[i];
	}
	MarkDirty();
}

void UGaussianSplatEditorData::ShowAll()
{
	if (!Asset.IsValid()) return;

	const int32 Count = Asset->GetSplatCount();
	for (int32 i = 0; i < Count; ++i)
	{
		HiddenFlags[i] = false;
	}
	MarkDirty();
}

// ============================================================================
// 删除 API
// ============================================================================

int32 UGaussianSplatEditorData::DeleteSelected()
{
	// ============================================================================
	// 稳定性策略：阶段 1 先不实现实际删除（避免 BulkData 操作的崩溃风险）
	// ============================================================================
	// 删除 splat 需要同时修改 PositionBulkData / OtherBulkData / SHBulkData /
	// ColorTextureBulkData，并重建 BoundingBox / SplatCount / RenderData /
	// ClusterHierarchy，且需要支持 Undo/Redo。这是高风险操作，留到后续阶段实现。
	//
	// 当前实现：如果用户尝试删除，弹通知提示"使用隐藏替代删除"，并自动隐藏选中。
	// 这样既不会崩溃，又能让用户完成"移除不需要的 splat"的目标（视觉上等同删除）。
	// ============================================================================
	if (!Asset.IsValid()) return 0;

	const int32 SelectedCount = GetSelectedCount();
	if (SelectedCount <= 0) return 0;

	UE_LOG(LogTemp, Warning, TEXT("GaussianSplatEditor: DeleteSelected 当前为 stub —— 已自动隐藏 %d 个 splat 代替删除（删除功能后续实现）"), SelectedCount);

	// 隐藏选中作为删除的视觉替代
	HideSelected();

	return SelectedCount;
}

// ============================================================================
// 内部辅助
// ============================================================================

void UGaussianSplatEditorData::MarkDirty()
{
	bDirty = true;
	OnChanged.Broadcast();
}

// ============================================================================
// 选择算法
// ============================================================================

int32 UGaussianSplatEditorData::SphereSelect(
	UGaussianSplatAsset* Asset,
	const FVector& SphereCenter,
	float SphereRadius,
	TArray<int32>& OutIndices)
{
	OutIndices.Reset();
	if (!Asset || Asset->GetSplatCount() <= 0 || SphereRadius <= 0.0f)
	{
		return 0;
	}

	// 获取解压后的 splat 位置。GetDecompressedPositions 内部已经处理了
	// BulkData Lock/Unlock 和 ON_SCOPE_EXIT，所以这里不需要手动管理。
	// 注意：对于 100 万 splat，这里会分配 12MB 内存（TArray<FVector>）。
	// 如果性能成为瓶颈，可以改为流式读取 BulkData 并就地遍历。
	TArray<FVector> Positions = Asset->GetDecompressedPositions();
	if (Positions.Num() != Asset->GetSplatCount())
	{
		UE_LOG(LogTemp, Warning, TEXT("GaussianSplatEditorData::SphereSelect: Positions.Num()=%d != SplatCount=%d, 跳过选择"),
			Positions.Num(), Asset->GetSplatCount());
		return 0;
	}

	const float RadiusSq = SphereRadius * SphereRadius;

	// 预估 10% 选中率，避免频繁 realloc
	OutIndices.Reserve(Positions.Num() / 10 + 16);

	for (int32 i = 0; i < Positions.Num(); ++i)
	{
		const float DistSq = FVector::DistSquared(Positions[i], SphereCenter);
		if (DistSq <= RadiusSq)
		{
			OutIndices.Add(i);
		}
	}

	return OutIndices.Num();
}

int32 UGaussianSplatEditorData::RectSelect(
	UGaussianSplatAsset* Asset,
	const FVector& ViewOrigin,
	const FVector& ViewDir,
	const FVector& ViewRight,
	const FVector& ViewUp,
	float FOVRadians,
	float AspectRatio,
	const FVector2D& ScreenRectMin,
	const FVector2D& ScreenRectMax,
	TArray<int32>& OutIndices)
{
	OutIndices.Reset();
	if (!Asset || Asset->GetSplatCount() <= 0)
	{
		return 0;
	}
	if (FOVRadians <= 0.0f || AspectRatio <= 0.0f)
	{
		return 0;
	}

	TArray<FVector> Positions = Asset->GetDecompressedPositions();
	if (Positions.Num() != Asset->GetSplatCount())
	{
		UE_LOG(LogTemp, Warning, TEXT("GaussianSplatEditorData::RectSelect: Positions.Num()=%d != SplatCount=%d, 跳过选择"),
			Positions.Num(), Asset->GetSplatCount());
		return 0;
	}

	// 计算投影参数。UE 使用透视投影，半高 = tan(FOV/2)，半宽 = 半高 * AspectRatio。
	// 屏幕 [-1, 1] 映射到 NDC。我们把 splat 投影到相机的 view space，然后归一化到 [-1, 1]。
	const float HalfTanFOV = FMath::Tan(FOVRadians * 0.5f);
	const float HalfTanFOVHorizontal = HalfTanFOV * AspectRatio;
	const float InvHalfTanFOV = 1.0f / HalfTanFOV;
	const float InvHalfTanFOVH = 1.0f / HalfTanFOVHorizontal;

	// 矩形在屏幕空间 [-1, 1]，min/max 可能颠倒（拖拽方向不同），这里规范化
	const float MinX = FMath::Min(ScreenRectMin.X, ScreenRectMax.X);
	const float MaxX = FMath::Max(ScreenRectMin.X, ScreenRectMax.X);
	const float MinY = FMath::Min(ScreenRectMin.Y, ScreenRectMax.Y);
	const float MaxY = FMath::Max(ScreenRectMin.Y, ScreenRectMax.Y);

	OutIndices.Reserve(Positions.Num() / 10 + 16);

	for (int32 i = 0; i < Positions.Num(); ++i)
	{
		const FVector Relative = Positions[i] - ViewOrigin;

		// 投影到 view space：depth = 沿 ViewDir 的距离，必须 > 0（在相机前方）
		const float Depth = FVector::DotProduct(Relative, ViewDir);
		if (Depth <= 0.0f)
		{
			continue;  // 在相机后方或齐平面，跳过
		}

		// 计算屏幕空间 X / Y（NDC，[-1, 1]）
		// X = (Relative · ViewRight) / Depth / HalfTanFOVHorizontal
		// Y = (Relative · ViewUp)    / Depth / HalfTanFOV
		// 注意：UE 屏幕坐标 Y 朝下，但这里我们用数学坐标（Y 朝上），与 ScreenRect 一致
		const float ScreenX = FVector::DotProduct(Relative, ViewRight) / Depth * InvHalfTanFOVH;
		const float ScreenY = FVector::DotProduct(Relative, ViewUp) / Depth * InvHalfTanFOV;

		if (ScreenX >= MinX && ScreenX <= MaxX && ScreenY >= MinY && ScreenY <= MaxY)
		{
			OutIndices.Add(i);
		}
	}

	return OutIndices.Num();
}

#undef LOCTEXT_NAMESPACE
