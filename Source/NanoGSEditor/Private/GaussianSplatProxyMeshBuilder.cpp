// Copyright Epic Games, Inc. All Rights Reserved.
// Copyright OpenSplat4D 2026 All Rights Reserved.

#include "GaussianSplatProxyMeshBuilder.h"

#include "GaussianSplatAsset.h"

#include "Containers/BitArray.h"
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "Misc/ScopeExit.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	/** Bins per axis used for the quantile trim. 1024 is a few KB and resolves
	 *  the 1%/99% cut far finer than any voxel size we would use. */
	constexpr int32 ProxyHistogramBins = 1024;

	/** Hard allocation guard for the occupancy grid (1 byte per cell). */
	constexpr int64 ProxyMaxGridCells = 64ll * 1024 * 1024;

	/** Voxel-size doubling attempts before giving up. */
	constexpr int32 ProxyMaxGrowAttempts = 16;

	/** Splats processed per streaming block, so scratch memory stays bounded
	 *  regardless of the asset size. */
	constexpr int32 ProxySplatBlock = 1 << 20;

	/** Positions are always stored as float32 xyz. */
	constexpr int32 ProxyPositionStride = 12;

	/** (axis, u, v) cycle chosen so that cross(u, v) == +axis, which lets the
	 *  same quad winding code emit a correct outward normal on every axis. */
	constexpr int32 AxisU[3] = { 1, 2, 0 };
	constexpr int32 AxisV[3] = { 2, 0, 1 };

	/** A maximal axis-aligned rectangle of one voxel slab, in (u, v) cell indices. */
	struct FQuadRect
	{
		int32 U0 = 0;
		int32 U1 = 0;
		int32 V0 = 0;
		int32 V1 = 0;
	};

	struct FVoxelGrid
	{
		/** Asset-local cm of lattice corner (0,0,0). */
		FVector Origin = FVector::ZeroVector;
		/** Cell counts per axis (lattice corners are Size+1). */
		FIntVector Size = FIntVector::ZeroValue;
		float CellSize = 1.0f;
		TBitArray<> Occupied;

		FORCEINLINE int64 NumCells() const
		{
			return (int64)Size.X * (int64)Size.Y * (int64)Size.Z;
		}

		FORCEINLINE int32 Index(int32 X, int32 Y, int32 Z) const
		{
			return (Z * Size.Y + Y) * Size.X + X;
		}
	};

	/** True when all three components are finite (rejects NaN and infinities
	 *  without relying on engine helpers, so the check is version agnostic). */
	FORCEINLINE bool IsFinitePosition(const float* P)
	{
		const float Limit = 3.0e38f;
		return FMath::Abs(P[0]) < Limit && FMath::Abs(P[1]) < Limit && FMath::Abs(P[2]) < Limit;
	}

	FORCEINLINE void SetAxis(FIntVector& Vector, int32 Axis, int32 Value)
	{
		switch (Axis)
		{
		case 0: Vector.X = Value; break;
		case 1: Vector.Y = Value; break;
		default: Vector.Z = Value; break;
		}
	}

	FORCEINLINE void SetAxis(FVector3f& Vector, int32 Axis, float Value)
	{
		switch (Axis)
		{
		case 0: Vector.X = Value; break;
		case 1: Vector.Y = Value; break;
		default: Vector.Z = Value; break;
		}
	}

	/** Greedy rectangle merge of a binary slab mask: scans v ascending, extends
	 *  each new run as far as possible in u, then downward in v. Deterministic. */
	void MergeSlabRects(const TArray<uint8>& Mask, int32 W, int32 H, TArray<uint8>& ScratchVisited, TArray<FQuadRect>& OutRects)
	{
		ScratchVisited.Reset();
		ScratchVisited.SetNumZeroed(W * H);
		OutRects.Reset();

		for (int32 V0 = 0; V0 < H; ++V0)
		{
			const int32 RowBase = V0 * W;
			for (int32 U0 = 0; U0 < W; ++U0)
			{
				if (Mask[RowBase + U0] == 0 || ScratchVisited[RowBase + U0] != 0)
				{
					continue;
				}

				int32 U1 = U0;
				while (U1 + 1 < W && Mask[RowBase + U1 + 1] != 0 && ScratchVisited[RowBase + U1 + 1] == 0)
				{
					++U1;
				}

				int32 V1 = V0;
				for (;;)
				{
					if (V1 + 1 >= H)
					{
						break;
					}
					const int32 NextBase = (V1 + 1) * W;
					bool bRowFree = true;
					for (int32 U = U0; U <= U1; ++U)
					{
						if (Mask[NextBase + U] == 0 || ScratchVisited[NextBase + U] != 0)
						{
							bRowFree = false;
							break;
						}
					}
					if (!bRowFree)
					{
						break;
					}
					++V1;
				}

				for (int32 V = V0; V <= V1; ++V)
				{
					const int32 Base = V * W;
					for (int32 U = U0; U <= U1; ++U)
					{
						ScratchVisited[Base + U] = 1;
					}
				}

				FQuadRect Rect;
				Rect.U0 = U0;
				Rect.U1 = U1;
				Rect.V0 = V0;
				Rect.V1 = V1;
				OutRects.Add(Rect);
			}
		}
	}

	/** One 6-neighbour dilation, which removes cells that only touch along an
	 *  edge or a corner (those are exactly the non-manifold cases). */
	void DilateOccupied(FVoxelGrid& Grid)
	{
		const TBitArray<> Source = Grid.Occupied;
		Grid.Occupied.Init(false, Source.Num());

		const FIntVector& S = Grid.Size;
		for (int32 Z = 0; Z < S.Z; ++Z)
		{
			for (int32 Y = 0; Y < S.Y; ++Y)
			{
				for (int32 X = 0; X < S.X; ++X)
				{
					if (!Source[Grid.Index(X, Y, Z)])
					{
						continue;
					}
					Grid.Occupied[Grid.Index(X, Y, Z)] = true;
					if (X > 0) { Grid.Occupied[Grid.Index(X - 1, Y, Z)] = true; }
					if (X + 1 < S.X) { Grid.Occupied[Grid.Index(X + 1, Y, Z)] = true; }
					if (Y > 0) { Grid.Occupied[Grid.Index(X, Y - 1, Z)] = true; }
					if (Y + 1 < S.Y) { Grid.Occupied[Grid.Index(X, Y + 1, Z)] = true; }
					if (Z > 0) { Grid.Occupied[Grid.Index(X, Y, Z - 1)] = true; }
					if (Z + 1 < S.Z) { Grid.Occupied[Grid.Index(X, Y, Z + 1)] = true; }
				}
			}
		}
	}

	int64 CountOccupied(const TBitArray<>& Bits)
	{
		int64 Count = 0;
		for (int32 Index = 0; Index < Bits.Num(); ++Index)
		{
			if (Bits[Index])
			{
				++Count;
			}
		}
		return Count;
	}
}

FGaussianSplatProxyMeshResult FGaussianSplatProxyMeshBuilder::BuildMeshDescription(
	const UGaussianSplatAsset* Asset,
	const FGaussianSplatProxyMeshParams& Params,
	FMeshDescription& OutMesh)
{
	FGaussianSplatProxyMeshResult Result;

	if (!Asset)
	{
		Result.Error = TEXT("资产为空。");
		return Result;
	}
	// v1 fracture only covers static 3D splats.
	if (Asset->Is4D())
	{
		Result.Error = TEXT("该资产含 4D 数据，暂不支持生成破裂代理网格（当前仅支持 3D 静态 splat）。");
		return Result;
	}

	const int32 SplatCount = Asset->GetSplatCount();
	if (SplatCount <= 0)
	{
		Result.Error = TEXT("资产内没有 splat。");
		return Result;
	}
	if (Asset->PositionFormat != EGaussianPositionFormat::Float32)
	{
		Result.Error = FString::Printf(TEXT("位置格式为 %s，代理网格生成只支持 Float32（当前需重新导入该资产）。"),
			*UEnum::GetValueAsString(Asset->PositionFormat));
		return Result;
	}

	// Zero-copy streaming read: never materialise a TArray<FVector> of positions
	// (a 100M splat asset would need 2.4 GB).
	int64 DataSize = 0;
	const uint8* Data = static_cast<const uint8*>(Asset->LockPositionDataReadOnly(&DataSize));
	if (!Data)
	{
		Result.Error = TEXT("无法锁定位置数据（bulk data 为空或已被占用）。");
		return Result;
	}
	ON_SCOPE_EXIT
	{
		Asset->UnlockPositionData();
	};

	const int64 ExpectedSize = (int64)SplatCount * ProxyPositionStride;
	if (DataSize < ExpectedSize)
	{
		Result.Error = FString::Printf(TEXT("位置数据长度异常：实际 %s 字节，期望至少 %s 字节。"),
			*LexToString(DataSize), *LexToString(ExpectedSize));
		return Result;
	}

	const float* PositionData = reinterpret_cast<const float*>(Data);

	// ------------------------------------------------------------------------
	// Pass 1: full bounds, skipping non-finite samples.
	// ------------------------------------------------------------------------
	FVector FullMin(TNumericLimits<double>::Max(), TNumericLimits<double>::Max(), TNumericLimits<double>::Max());
	FVector FullMax(TNumericLimits<double>::Lowest(), TNumericLimits<double>::Lowest(), TNumericLimits<double>::Lowest());
	int64 ValidSplats = 0;

	for (int32 BlockStart = 0; BlockStart < SplatCount; BlockStart += ProxySplatBlock)
	{
		const int32 BlockEnd = FMath::Min(BlockStart + ProxySplatBlock, SplatCount);
		const float* BlockPtr = PositionData + (int64)BlockStart * 3;
		for (int32 Index = BlockStart; Index < BlockEnd; ++Index)
		{
			const float* P = BlockPtr + (int64)(Index - BlockStart) * 3;
			if (!IsFinitePosition(P))
			{
				continue;
			}
			FullMin.X = FMath::Min<double>(FullMin.X, P[0]);
			FullMin.Y = FMath::Min<double>(FullMin.Y, P[1]);
			FullMin.Z = FMath::Min<double>(FullMin.Z, P[2]);
			FullMax.X = FMath::Max<double>(FullMax.X, P[0]);
			FullMax.Y = FMath::Max<double>(FullMax.Y, P[1]);
			FullMax.Z = FMath::Max<double>(FullMax.Z, P[2]);
			++ValidSplats;
		}
	}

	if (ValidSplats == 0)
	{
		Result.Error = TEXT("位置数据中没有有效（有限）的 splat。");
		return Result;
	}

	// ------------------------------------------------------------------------
	// Pass 2: quantile trim. A handful of floating outliers must not decide the
	// grid resolution, otherwise the proxy collapses into a few huge blocks.
	// ------------------------------------------------------------------------
	const float Trim = FMath::Clamp(Params.TrimPercentile, 0.0f, 0.25f);
	FVector TrimMin = FullMin;
	FVector TrimMax = FullMax;

	if (Trim > 0.0f)
	{
		const FVector Range = FullMax - FullMin;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (Range[Axis] <= 0.0)
			{
				continue;
			}

			TArray<uint32> Histogram;
			Histogram.SetNumZeroed(ProxyHistogramBins);
			const double BinScale = (double)(ProxyHistogramBins - 1) / Range[Axis];
			const double Lo = FullMin[Axis];

			for (int32 BlockStart = 0; BlockStart < SplatCount; BlockStart += ProxySplatBlock)
			{
				const int32 BlockEnd = FMath::Min(BlockStart + ProxySplatBlock, SplatCount);
				const float* BlockPtr = PositionData + (int64)BlockStart * 3;
				for (int32 Index = BlockStart; Index < BlockEnd; ++Index)
				{
					const float* P = BlockPtr + (int64)(Index - BlockStart) * 3;
					if (!IsFinitePosition(P))
					{
						continue;
					}
					int32 Bin = (int32)FMath::Clamp<double>((double)(P[Axis] - Lo) * BinScale, 0.0, (double)(ProxyHistogramBins - 1));
					++Histogram[Bin];
				}
			}

			const int64 SampleCount = ValidSplats;
			const int64 LowTarget = (int64)FMath::FloorToDouble((double)SampleCount * Trim);
			const int64 HighTarget = SampleCount - LowTarget;

			int64 Accumulated = 0;
			int32 LowBin = 0;
			int32 HighBin = ProxyHistogramBins - 1;
			bool bLowFound = false;
			for (int32 Bin = 0; Bin < ProxyHistogramBins; ++Bin)
			{
				Accumulated += Histogram[Bin];
				if (!bLowFound && Accumulated >= LowTarget)
				{
					LowBin = Bin;
					bLowFound = true;
				}
				if (Accumulated >= HighTarget)
				{
					HighBin = Bin;
					break;
				}
			}

			const double BinWidth = Range[Axis] / (double)(ProxyHistogramBins - 1);
			TrimMin[Axis] = Lo + (double)LowBin * BinWidth;
			TrimMax[Axis] = Lo + (double)(HighBin + 1) * BinWidth;
		}
	}

	FVector Extent = TrimMax - TrimMin;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (!(Extent[Axis] > 0.0))
		{
			// Degenerate axis (flat capture): keep a single usable slab.
			Extent[Axis] = FMath::Max(1.0, (double)FullMax[Axis] - (double)FullMin[Axis]);
		}
	}

	// ------------------------------------------------------------------------
	// Voxel size: user/auto choice, then axis guardrail.
	// ------------------------------------------------------------------------
	float VoxelSize = Params.VoxelSize;
	if (!(VoxelSize > 0.0f))
	{
		VoxelSize = (float)FMath::Max(Extent.Size() / 192.0, 1.0);
	}
	const int32 MaxCellsPerAxis = FMath::Clamp(Params.MaxCellsPerAxis, 8, 512);
	const double ExtentMax = FMath::Max3(Extent.X, Extent.Y, Extent.Z);
	// Two cells are kept as padding (one per side) so the dilation pass never
	// has to reallocate the grid.
	const double MinVoxelForAxis = ExtentMax / (double)FMath::Max(MaxCellsPerAxis - 2, 1);
	VoxelSize = (float)FMath::Max((double)VoxelSize, MinVoxelForAxis);

	const int64 MaxOccupied = FMath::Max<int64>(Params.MaxOccupiedVoxels, 1024);
	const int64 MaxTriangles = FMath::Max<int64>(Params.MaxTriangles, 1024);
	const int32 MinSplatsPerVoxel = FMath::Max(Params.MinSplatsPerVoxel, 1);

	// ------------------------------------------------------------------------
	// Occupancy grid (grown until it fits the guardrails).
	// ------------------------------------------------------------------------
	FVoxelGrid Grid;
	bool bGridReady = false;
	int32 GrowAttempts = 0;
	FString GrowNote;

	for (int32 Attempt = 0; Attempt < ProxyMaxGrowAttempts; ++Attempt)
	{
		FVoxelGrid Candidate;
		Candidate.CellSize = VoxelSize;
		Candidate.Origin = TrimMin - FVector(VoxelSize);
		Candidate.Size.X = FMath::Clamp((int32)FMath::CeilToDouble(Extent.X / VoxelSize) + 2, 3, MaxCellsPerAxis);
		Candidate.Size.Y = FMath::Clamp((int32)FMath::CeilToDouble(Extent.Y / VoxelSize) + 2, 3, MaxCellsPerAxis);
		Candidate.Size.Z = FMath::Clamp((int32)FMath::CeilToDouble(Extent.Z / VoxelSize) + 2, 3, MaxCellsPerAxis);

		const int64 NumCells = Candidate.NumCells();
		if (NumCells > ProxyMaxGridCells)
		{
			VoxelSize *= 2.0f;
			++GrowAttempts;
			continue;
		}

		TArray<uint8> Counts;
		Counts.SetNumZeroed((int32)NumCells);

		const FVector Origin = Candidate.Origin;
		const FIntVector Size = Candidate.Size;
		const double InvCellSize = 1.0 / (double)VoxelSize;
		const int32 MaxX = Size.X - 1;
		const int32 MaxY = Size.Y - 1;
		const int32 MaxZ = Size.Z - 1;

		for (int32 BlockStart = 0; BlockStart < SplatCount; BlockStart += ProxySplatBlock)
		{
			const int32 BlockEnd = FMath::Min(BlockStart + ProxySplatBlock, SplatCount);
			const float* BlockPtr = PositionData + (int64)BlockStart * 3;
			for (int32 Index = BlockStart; Index < BlockEnd; ++Index)
			{
				const float* P = BlockPtr + (int64)(Index - BlockStart) * 3;
				if (!IsFinitePosition(P))
				{
					continue;
				}
				const double FX = ((double)P[0] - Origin.X) * InvCellSize;
				const double FY = ((double)P[1] - Origin.Y) * InvCellSize;
				const double FZ = ((double)P[2] - Origin.Z) * InvCellSize;
				const int32 CX = (int32)FMath::Clamp<double>(FMath::FloorToDouble(FX), 0.0, (double)MaxX);
				const int32 CY = (int32)FMath::Clamp<double>(FMath::FloorToDouble(FY), 0.0, (double)MaxY);
				const int32 CZ = (int32)FMath::Clamp<double>(FMath::FloorToDouble(FZ), 0.0, (double)MaxZ);
				uint8& CellCount = Counts[(CZ * Size.Y + CY) * Size.X + CX];
				if (CellCount < 255)
				{
					++CellCount;
				}
			}
		}

		Candidate.Occupied.Init(false, (int32)NumCells);
		for (int64 Cell = 0; Cell < NumCells; ++Cell)
		{
			if (Counts[(int32)Cell] >= (uint8)MinSplatsPerVoxel)
			{
				Candidate.Occupied[(int32)Cell] = true;
			}
		}
		Counts.Empty();

		if (Params.bDilate)
		{
			DilateOccupied(Candidate);
		}

		const int64 Occupied = CountOccupied(Candidate.Occupied);
		if (Occupied > MaxOccupied)
		{
			// Too dense at this resolution: coarsen and retry.
			VoxelSize *= 2.0f;
			++GrowAttempts;
			continue;
		}
		if (Occupied == 0)
		{
			Result.Error = FString::Printf(
				TEXT("体素全为空：在 %.2f cm 体素、每格至少 %d 个 splat 的条件下没有实心体素。\n请减小体素尺寸或降低密度阈值后重试。"),
				VoxelSize, MinSplatsPerVoxel);
			return Result;
		}

		Grid = MoveTemp(Candidate);
		bGridReady = true;
		break;
	}

	if (!bGridReady)
	{
		Result.Error = FString::Printf(TEXT("体素网格在 %d 次自动放大后仍超出护栏（占用体素上限 %s）。请手动指定更大的体素尺寸。"),
			ProxyMaxGrowAttempts, *LexToString(MaxOccupied));
		return Result;
	}

	if (GrowAttempts > 0)
	{
		GrowNote = FString::Printf(TEXT("（已自动放大体素 %d 次以满足护栏）"), GrowAttempts);
		UE_LOG(LogTemp, Log, TEXT("GaussianSplatProxyMesh: voxel size auto-grown %d time(s) to %f"), GrowAttempts, VoxelSize);
	}

	// ------------------------------------------------------------------------
	// Face extraction. For each axis and each direction we emit the faces that
	// separate an occupied cell from an empty one (outside counts as empty),
	// which is exactly the closed block surface, with no interior faces.
	// Rectangle merging then collapses coplanar patches into single quads.
	// ------------------------------------------------------------------------
	OutMesh.Empty();
	FStaticMeshAttributes Attributes(OutMesh);
	Attributes.Register();

	TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> VertexNormals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> VertexTangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> VertexBinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector2f> VertexUVs = Attributes.GetVertexInstanceUVs();
	// At least one UV channel is required or the static mesh build warns.
	VertexUVs.SetNumChannels(1);

	const FPolygonGroupID PolygonGroup = OutMesh.CreatePolygonGroup();

	// Welding keyed on the integer lattice corner: every vertex of a voxel
	// surface sits on the lattice, so this makes the mesh watertight and lets
	// us detect non-manifold edges at all.
	TMap<FIntVector, FVertexID> VertexByLattice;

	TArray<uint8> SlabMask;
	TArray<uint8> SlabVisited;
	TArray<FQuadRect> Rectangles;

	const FIntVector GridSize = Grid.Size;
	int32 QuadCount = 0;

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const int32 UAxis = AxisU[Axis];
		const int32 VAxis = AxisV[Axis];
		const int32 W = GridSize[UAxis];
		const int32 H = GridSize[VAxis];
		const int32 SliceCount = GridSize[Axis];

		SlabMask.SetNumZeroed(W * H);

		for (int32 Direction = -1; Direction <= 1; Direction += 2)
		{
			for (int32 Slice = 0; Slice < SliceCount; ++Slice)
			{
				const int32 NeighbourSlice = Slice + Direction;
				const bool bNeighbourInside = NeighbourSlice >= 0 && NeighbourSlice < SliceCount;

				for (int32 V = 0; V < H; ++V)
				{
					for (int32 U = 0; U < W; ++U)
					{
						FIntVector Cell = FIntVector::ZeroValue;
						SetAxis(Cell, Axis, Slice);
						SetAxis(Cell, UAxis, U);
						SetAxis(Cell, VAxis, V);
						const bool bFilled = Grid.Occupied[Grid.Index(Cell.X, Cell.Y, Cell.Z)];

						bool bNeighbourFilled = false;
						if (bFilled && bNeighbourInside)
						{
							FIntVector Neighbour = Cell;
							SetAxis(Neighbour, Axis, NeighbourSlice);
							bNeighbourFilled = Grid.Occupied[Grid.Index(Neighbour.X, Neighbour.Y, Neighbour.Z)];
						}
						SlabMask[V * W + U] = (bFilled && !bNeighbourFilled) ? 1 : 0;
					}
				}

				MergeSlabRects(SlabMask, W, H, SlabVisited, Rectangles);
				if (Rectangles.Num() == 0)
				{
					continue;
				}

				if (((int64)QuadCount + Rectangles.Num()) * 2 > MaxTriangles)
				{
					Result.Error = FString::Printf(
						TEXT("代理网格三角形数将超过上限 %s（体素过细）。请使用更大的体素尺寸，或提高护栏后重试。"),
						*LexToString(MaxTriangles));
					return Result;
				}

				// Plane of the emitted face, in lattice units.
				const int32 PlaneIndex = Slice + (Direction > 0 ? 1 : 0);

				FVector3f FaceNormal = FVector3f::ZeroVector;
				SetAxis(FaceNormal, Axis, (Direction > 0) ? 1.0f : -1.0f);
				FVector3f FaceTangent = FVector3f::ZeroVector;
				SetAxis(FaceTangent, UAxis, 1.0f);

				TArray<FVertexInstanceID> CornerInstances;
				CornerInstances.SetNumUninitialized(4);

				for (const FQuadRect& Rect : Rectangles)
				{
					// Lattice corners of the rectangle (inclusive cell range, so
					// the far edge is at index + 1).
					const int32 CornerU[4] = { Rect.U0, Rect.U1 + 1, Rect.U1 + 1, Rect.U0 };
					const int32 CornerV[4] = { Rect.V0, Rect.V0, Rect.V1 + 1, Rect.V1 + 1 };

					for (int32 Corner = 0; Corner < 4; ++Corner)
					{
						// Direction -1 emits the same quad with reversed winding so
						// that the cross product still points along the normal.
						const int32 Wound = (Direction > 0) ? Corner : (3 - Corner);

						FIntVector Lattice = FIntVector::ZeroValue;
						SetAxis(Lattice, Axis, PlaneIndex);
						SetAxis(Lattice, UAxis, CornerU[Wound]);
						SetAxis(Lattice, VAxis, CornerV[Wound]);

						FVertexID* ExistingVertex = VertexByLattice.Find(Lattice);
						if (!ExistingVertex)
						{
							const FVector CornerPoint(
								Grid.Origin.X + (double)Lattice.X * Grid.CellSize,
								Grid.Origin.Y + (double)Lattice.Y * Grid.CellSize,
								Grid.Origin.Z + (double)Lattice.Z * Grid.CellSize);
							const FVertexID NewVertex = OutMesh.CreateVertex();
							VertexPositions[NewVertex] = FVector3f((float)CornerPoint.X, (float)CornerPoint.Y, (float)CornerPoint.Z);
							ExistingVertex = &VertexByLattice.Add(Lattice, NewVertex);
						}
						const FVertexID VertexID = *ExistingVertex;

						const FVertexInstanceID InstanceID = OutMesh.CreateVertexInstance(VertexID);
						VertexNormals[InstanceID] = FaceNormal;
						VertexTangents[InstanceID] = FaceTangent;
						VertexBinormalSigns[InstanceID] = 1.0f;
						VertexUVs[InstanceID] = FVector2f(
							(float)((double)CornerU[Wound] * Grid.CellSize * 0.01),
							(float)((double)CornerV[Wound] * Grid.CellSize * 0.01));
						CornerInstances[Corner] = InstanceID;
					}

					OutMesh.CreatePolygon(PolygonGroup, MakeArrayView(CornerInstances.GetData(), 4));
					++QuadCount;
				}
			}
		}
	}

	if (QuadCount == 0)
	{
		Result.Error = TEXT("占用体素没有形成任何表面，无法生成代理网格。");
		return Result;
	}

	// ------------------------------------------------------------------------
	// Manifold self-check: every undirected edge of a closed surface must be
	// shared by exactly two faces. Anything else breaks PlanarCut.
	// ------------------------------------------------------------------------
	TMap<uint64, int32> EdgeUseCount;
	EdgeUseCount.Reserve(QuadCount * 2);
	TArray<FVertexID> PolygonVertices;
	const int32 NumPolygons = OutMesh.Polygons().Num();
	for (int32 PolygonIndex = 0; PolygonIndex < NumPolygons; ++PolygonIndex)
	{
		PolygonVertices.Reset();
		OutMesh.GetPolygonVertices(FPolygonID(PolygonIndex), PolygonVertices);
		const int32 Count = PolygonVertices.Num();
		for (int32 Corner = 0; Corner < Count; ++Corner)
		{
			const uint32 A = (uint32)PolygonVertices[Corner].GetValue();
			const uint32 B = (uint32)PolygonVertices[(Corner + 1) % Count].GetValue();
			const uint64 Key = ((uint64)FMath::Min(A, B) << 32) | (uint64)FMath::Max(A, B);
			++EdgeUseCount.FindOrAdd(Key);
		}
	}

	int32 NonManifoldEdges = 0;
	for (const TPair<uint64, int32>& Pair : EdgeUseCount)
	{
		if (Pair.Value != 2)
		{
			++NonManifoldEdges;
		}
	}
	if (NonManifoldEdges > 0)
	{
		// Not fatal, but PlanarCut may produce artefacts: report it loudly.
		UE_LOG(LogTemp, Warning,
			TEXT("GaussianSplatProxyMesh: %d non-manifold edge(s) out of %d (dilation was %s). PlanarCut may misbehave."),
			NonManifoldEdges, EdgeUseCount.Num(), Params.bDilate ? TEXT("on") : TEXT("off"));
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("GaussianSplatProxyMesh: mesh is closed and manifold (%d edges checked)."), EdgeUseCount.Num());
	}

	Result.OccupiedVoxels = CountOccupied(Grid.Occupied);
	Result.QuadCount = QuadCount;
	Result.TriangleCount = QuadCount * 2;
	Result.EffectiveVoxelSize = Grid.CellSize;
	Result.GridOrigin = Grid.Origin;
	Result.GridSize = Grid.Size;
	Result.NonManifoldEdgeCount = NonManifoldEdges;
	Result.bSuccess = true;
	Result.Summary = FString::Printf(
		TEXT("体素尺寸: %.2f cm %s\n")
		TEXT("网格尺寸: %d x %d x %d\n")
		TEXT("占用体素: %s\n")
		TEXT("四边形: %d（三角形 %d）\n")
		TEXT("非流形边: %d"),
		Grid.CellSize, *GrowNote,
		Grid.Size.X, Grid.Size.Y, Grid.Size.Z,
		*LexToString(Result.OccupiedVoxels),
		QuadCount, QuadCount * 2,
		NonManifoldEdges);

	UE_LOG(LogTemp, Log, TEXT("GaussianSplatProxyMesh: built %d quads / %d verts, voxel %.2f cm, occupied %s"),
		QuadCount, VertexByLattice.Num(), Grid.CellSize, *LexToString(Result.OccupiedVoxels));

	return Result;
}

UStaticMesh* FGaussianSplatProxyMeshBuilder::CreateProxyMeshAsset(
	const UGaussianSplatAsset* Asset,
	const FGaussianSplatProxyMeshParams& Params,
	FString& OutError,
	FGaussianSplatProxyMeshResult* OutResult)
{
	FGaussianSplatProxyMeshResult LocalResult;
	FMeshDescription BuiltMesh;
	LocalResult = BuildMeshDescription(Asset, Params, BuiltMesh);
	if (OutResult)
	{
		*OutResult = LocalResult;
	}
	if (!LocalResult.bSuccess)
	{
		OutError = LocalResult.Error;
		return nullptr;
	}

	// Save next to the source asset, named "<Asset>_Proxy".
	FString PackagePath = TEXT("/Game");
	FString BaseName = Asset ? Asset->GetName() : TEXT("Splat");
	if (Asset && Asset->GetOutermost())
	{
		const FString AssetPackageName = Asset->GetOutermost()->GetName();
		const int32 SlashIndex = AssetPackageName.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (SlashIndex != INDEX_NONE)
		{
			PackagePath = AssetPackageName.Left(SlashIndex);
		}
	}
	BaseName = ObjectTools::SanitizeObjectName(BaseName);
	BaseName += TEXT("_Proxy");

	FString OutPackageName;
	FString OutAssetName;
	FAssetToolsModule::GetModule().Get().CreateUniqueAssetName(PackagePath / BaseName, TEXT(""), OutPackageName, OutAssetName);

	UPackage* Package = CreatePackage(*OutPackageName);
	UStaticMesh* Mesh = NewObject<UStaticMesh>(Package, UStaticMesh::StaticClass(), FName(*OutAssetName), RF_Public | RF_Standalone);
	if (!Mesh)
	{
		OutError = TEXT("无法创建 StaticMesh 资产对象。");
		return nullptr;
	}

	FMeshDescription* MeshDescription = Mesh->CreateMeshDescription(0);
	if (!MeshDescription)
	{
		OutError = TEXT("无法为该 StaticMesh 创建网格描述。");
		return nullptr;
	}
	*MeshDescription = MoveTemp(BuiltMesh);

	Mesh->CommitMeshDescription(0);

	// The proxy must stay watertight: automatic degenerate removal would open
	// holes, and the normals/tangents are already exact for axis-aligned faces.
	FMeshBuildSettings& BuildSettings = Mesh->GetSourceModel(0).BuildSettings;
	BuildSettings.bRecomputeNormals = false;
	BuildSettings.bRecomputeTangents = false;
	BuildSettings.bUseMikkTSpace = false;
	BuildSettings.bGenerateLightmapUVs = false;
	BuildSettings.bRemoveDegenerates = false;

	Mesh->Build();
	Mesh->MarkPackageDirty();

	FAssetRegistryModule::AssetCreated(Mesh);

	FPackagePath PkgPath = FPackagePath::FromPackageNameChecked(Package->GetName());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	SaveArgs.bWarnOfLongFilename = false;
	if (!UPackage::SavePackage(Package, Mesh, *PkgPath.GetLocalFullPath(), SaveArgs))
	{
		OutError = FString::Printf(TEXT("代理网格已生成但保存失败：%s"), *PkgPath.GetLocalFullPath());
		return nullptr;
	}

	UE_LOG(LogTemp, Log, TEXT("GaussianSplatProxyMesh: created proxy mesh asset '%s'"), *OutPackageName);
	OutError.Reset();
	return Mesh;
}