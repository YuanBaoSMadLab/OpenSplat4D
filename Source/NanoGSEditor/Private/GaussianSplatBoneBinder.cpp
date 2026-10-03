// Copyright OpenSplat4D 2026 All Rights Reserved.
// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatBoneBinder.h"

#include "GaussianSplatAsset.h"
#include "GaussianSplatFractureAsset.h"
#include "GaussianDataTypes.h"

#include "GeometryCollection/GeometryCollection.h"
#include "GeometryCollection/GeometryCollectionAlgo.h"
#include "GeometryCollection/GeometryCollectionObject.h"

#include "Math/RandomStream.h"
#include "Misc/ScopeExit.h"

namespace
{
	/** 3 floats per splat position, same packing the renderer uploads. */
	constexpr int32 BindPositionStride = 12;

	/** 7 floats per splat in the rotation/scale block (quat XYZW + scale XYZ). */
	constexpr int32 BindOtherStride = 28;
	constexpr int32 BindOtherFloats = BindOtherStride / (int32)sizeof(float);

	/** Hard cap on the uniform lookup grid per axis. */
	constexpr int32 BindMaxCellsPerAxis = 512;

	/** Caps must not be more transparent than this, otherwise the freshly
	 *  exposed section reads as a hole again. */
	constexpr float BindMinCapOpacity = 0.5f;

	/** The attribute the engine writes for every fracturable bone; it is a
	 *  private literal in GeometryCollectionPhysicsProxy.cpp, so it has to be
	 *  repeated here. Missing on hand built collections, hence optional. */
	const FName MassToLocalAttributeName(TEXT("MassToLocal"));

	/** Set by the planar cut tools on every triangle they create, so a piece's
	 *  exposed section is exactly its "Internal" face set. Also a private
	 *  literal in the engine, see GeometryCollectionSelectionNodes.cpp. */
	const FName InternalAttributeName(TEXT("Internal"));

	FORCEINLINE bool IsFinitePosition(const float* P)
	{
		return FMath::IsFinite(P[0]) && FMath::IsFinite(P[1]) && FMath::IsFinite(P[2]);
	}

	/** FMatrix::TransformPosition returns a homogeneous FVector4; drop w. */
	FORCEINLINE FVector MatrixTransformPoint(const FMatrix& Matrix, const FVector& Point)
	{
		const FVector4d Result = Matrix.TransformPosition(Point);
		return FVector(Result.X, Result.Y, Result.Z);
	}

	/**
	 * One selectable fracture piece: a leaf transform of the collection plus the
	 * oriented box of the geometry it owns. Everything is expressed in
	 * collection component space, which the proxy mesh workflow makes identical
	 * to the asset local space (see the header).
	 */
	struct FPieceObb
	{
		/** Index into the collection transform group. */
		int32 BoneIndex = INDEX_NONE;

		/** OBB centre in component space. */
		FVector Center = FVector::ZeroVector;

		/** OBB half extents, already clamped to a usable minimum. */
		FVector Extent = FVector::ZeroVector;

		/** Component space -> OBB local space. */
		FMatrix ComponentToObb = FMatrix::Identity;

		/** Axis aligned component space bounds of the OBB (grid rasteriser). */
		FBox ComponentAabb = FBox(ForceInit);
	};

	/**
	 * Ranking key, ascending, smaller is better:
	 *   inside a piece      -> [0, 1]: the largest normalised axis coordinate,
	 *                          so 0 is the dead centre and 1 the surface. The
	 *                          piece the splat sits deepest inside wins.
	 *   outside every piece -> (1, inf]: 1 + euclidean distance to the box, so
	 *                          an inside candidate always beats an outside one.
	 * Normalising by the extents (rather than a signed distance) keeps a thin
	 * slab from swallowing half the cloud through its smallest axis.
	 */
	FORCEINLINE float PieceScore(const FPieceObb& Piece, const FVector& P)
	{
		const FVector Q = MatrixTransformPoint(Piece.ComponentToObb, P).GetAbs();

		const double MaxNorm = FMath::Max3(Q.X / Piece.Extent.X, Q.Y / Piece.Extent.Y, Q.Z / Piece.Extent.Z);
		if (MaxNorm <= 1.0)
		{
			return (float)MaxNorm;
		}

		const FVector Outside(Q.X - Piece.Extent.X, Q.Y - Piece.Extent.Y, Q.Z - Piece.Extent.Z);
		return (float)(1.0 + Outside.ComponentMax(FVector::ZeroVector).Size());
	}

	/** Uniform cell grid over the piece OBBs, stored as CSR for cache friendly
	 *  lookup without per cell TArray overhead. */
	struct FLookupGrid
	{
		FIntVector Size = FIntVector::ZeroValue;
		FVector Origin = FVector::ZeroVector;
		float CellSize = 1.0f;
		TArray<int32> CellStart;
		TArray<int32> CellItems;

		FORCEINLINE int32 CellIndex(int32 X, int32 Y, int32 Z) const
		{
			return (Z * Size.Y + Y) * Size.X + X;
		}

		FORCEINLINE FIntVector PointToCell(const FVector& P) const
		{
			return FIntVector(
				FMath::Clamp((int32)FMath::FloorToDouble((P.X - Origin.X) / CellSize), 0, Size.X - 1),
				FMath::Clamp((int32)FMath::FloorToDouble((P.Y - Origin.Y) / CellSize), 0, Size.Y - 1),
				FMath::Clamp((int32)FMath::FloorToDouble((P.Z - Origin.Z) / CellSize), 0, Size.Z - 1));
		}
	};

	/**
	 * Rasterise every piece AABB into a uniform grid whose cell size targets
	 * roughly one piece per cell. The cell is doubled until the grid fits both
	 * the per axis cap and the cell budget.
	 * @return false when the piece set has no usable bounds.
	 */
	bool BuildLookupGrid(const TArray<FPieceObb>& Pieces, const FGaussianSplatBoneBindParams& Params,
		FLookupGrid& OutGrid, FBox& OutBounds, FString& OutError)
	{
		FBox Bounds(ForceInit);
		for (const FPieceObb& Piece : Pieces)
		{
			Bounds += Piece.ComponentAabb;
		}
		if (!Bounds.IsValid)
		{
			OutError = TEXT("破裂碎片没有有效的包围盒，几何数据可能为空。");
			return false;
		}
		OutBounds = Bounds;

		const FVector Extent = Bounds.GetSize();
		const double Volume = FMath::Max(Bounds.GetVolume(), 1.0);
		double CellSize = FMath::Pow(Volume / (double)FMath::Max(1, Pieces.Num()), 1.0 / 3.0);
		CellSize = FMath::Max(CellSize, (double)FMath::Max(Params.MinExtentCm * 4.0f, 1.0f));

		FIntVector GridSize = FIntVector::ZeroValue;
		int64 TotalCells = 0;
		bool bFits = false;
		for (int32 Attempt = 0; Attempt < 64; ++Attempt)
		{
			GridSize = FIntVector(
				FMath::Clamp((int32)FMath::CeilToDouble(Extent.X / CellSize), 1, BindMaxCellsPerAxis),
				FMath::Clamp((int32)FMath::CeilToDouble(Extent.Y / CellSize), 1, BindMaxCellsPerAxis),
				FMath::Clamp((int32)FMath::CeilToDouble(Extent.Z / CellSize), 1, BindMaxCellsPerAxis));
			TotalCells = (int64)GridSize.X * GridSize.Y * GridSize.Z;
			if (TotalCells <= Params.GridCellBudget)
			{
				bFits = true;
				break;
			}
			CellSize *= 2.0;
		}
		if (!bFits)
		{
			OutError = TEXT("无法为破裂碎片建立查找网格（碎片数量与包围盒不匹配）。");
			return false;
		}

		OutGrid.Size = GridSize;
		OutGrid.Origin = Bounds.Min;
		OutGrid.CellSize = (float)CellSize;

		// Pass 1: per cell piece counts.
		TArray<int32> Counts;
		Counts.SetNumZeroed((int32)TotalCells);
		for (const FPieceObb& Piece : Pieces)
		{
			const FIntVector Min = OutGrid.PointToCell(Piece.ComponentAabb.Min);
			const FIntVector Max = OutGrid.PointToCell(Piece.ComponentAabb.Max);
			for (int32 Z = Min.Z; Z <= Max.Z; ++Z)
			{
				for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
				{
					for (int32 X = Min.X; X <= Max.X; ++X)
					{
						++Counts[OutGrid.CellIndex(X, Y, Z)];
					}
				}
			}
		}

		// Pass 2: prefix sum, then scatter.
		OutGrid.CellStart.SetNumUninitialized((int32)TotalCells + 1);
		int32 Running = 0;
		for (int32 CellIdx = 0; CellIdx < (int32)TotalCells; ++CellIdx)
		{
			OutGrid.CellStart[CellIdx] = Running;
			Running += Counts[CellIdx];
		}
		OutGrid.CellStart[(int32)TotalCells] = Running;

		TArray<int32> Cursor;
		Cursor.SetNumUninitialized((int32)TotalCells);
		FMemory::Memcpy(Cursor.GetData(), OutGrid.CellStart.GetData(), (SIZE_T)TotalCells * sizeof(int32));

		OutGrid.CellItems.SetNumUninitialized(Running);
		for (int32 PieceIdx = 0; PieceIdx < Pieces.Num(); ++PieceIdx)
		{
			const FPieceObb& Piece = Pieces[PieceIdx];
			const FIntVector Min = OutGrid.PointToCell(Piece.ComponentAabb.Min);
			const FIntVector Max = OutGrid.PointToCell(Piece.ComponentAabb.Max);
			for (int32 Z = Min.Z; Z <= Max.Z; ++Z)
			{
				for (int32 Y = Min.Y; Y <= Max.Y; ++Y)
				{
					for (int32 X = Min.X; X <= Max.X; ++X)
					{
						OutGrid.CellItems[Cursor[OutGrid.CellIndex(X, Y, Z)]++] = PieceIdx;
					}
				}
			}
		}

		return true;
	}

	/**
	 * Cut surface census of one piece, built from the "Internal" faces of the
	 * geometry it owns. Faces are already the triangle soup the renderer needs,
	 * so they are kept as an area weighted CDF to sample caps from.
	 */
	struct FPieceCapSource
	{
		/** Component space area of the piece's visible cut faces. */
		double InternalArea = 0.0;

		/** Component space area of the piece's visible external faces. */
		double ExternalArea = 0.0;

		/** Cut face indices, ascending, one CDF step per entry. */
		TArray<int32> InternalFaces;

		/** Area weighted CDF over InternalFaces, ascending in (0, 1]. */
		TArray<double> InternalCdf;
	};
}

FGaussianSplatBoneBindResult FGaussianSplatBoneBinder::BindAssetToCollection(
	UGaussianSplatAsset* Asset,
	UGaussianSplatFractureAsset* Fracture,
	const UGeometryCollection* Collection,
	const FGaussianSplatBoneBindParams& Params)
{
	FGaussianSplatBoneBindResult Result;

	if (!Asset)
	{
		Result.Error = TEXT("资产为空。");
		return Result;
	}
	// v11: the binding is written into the fracture asset, so it is required.
	if (!Fracture)
	{
		Result.Error = TEXT("破裂资产为空（绑定数据写入破裂资产，需先生成破裂资产）。");
		return Result;
	}
	if (!Collection)
	{
		Result.Error = TEXT("没有指定 GeometryCollection。");
		return Result;
	}
	// v1 fracture only covers static 3D splats.
	if (Asset->Is4D())
	{
		Result.Error = TEXT("该资产含 4D 数据，暂不支持绑定破裂骨头（当前仅支持 3D 静态 splat）。");
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
		Result.Error = FString::Printf(TEXT("位置格式为 %s，绑定破裂骨头只支持 Float32（当前需重新导入该资产）。"),
			*UEnum::GetValueAsString(Asset->PositionFormat));
		return Result;
	}

	const TSharedPtr<FGeometryCollection, ESPMode::ThreadSafe> GeometryCollection = Collection->GetGeometryCollection();
	if (!GeometryCollection.IsValid())
	{
		Result.Error = TEXT("该 GeometryCollection 没有内部几何数据（可能尚未在破裂模式中切割）。");
		return Result;
	}
	FGeometryCollection& GC = *GeometryCollection;

	const int32 TransformCount = GC.NumElements(FGeometryCollection::TransformGroup);
	const int32 GeometryCount = GC.NumElements(FGeometryCollection::GeometryGroup);
	if (TransformCount <= 0 || GeometryCount <= 0)
	{
		Result.Error = TEXT("该 GeometryCollection 内没有骨头或几何碎片。");
		return Result;
	}
	if (TransformCount > 65535)
	{
		Result.Error = FString::Printf(TEXT("骨头数量 %d 超过上限 65535，无法绑定（每 splat 的骨头索引只有 16 位）。"),
			TransformCount);
		return Result;
	}

	// Component space global matrices of the rest pose. Exactly the call the
	// engine makes in GeometryCollectionPhysicsProxy before setting up the
	// simulation, so the hierarchy convention cannot drift.
	TArray<FTransform> ComponentSpaceTransforms;
	GeometryCollectionAlgo::GlobalMatrices(GC.Transform, GC.Parent, ComponentSpaceTransforms);
	if (ComponentSpaceTransforms.Num() != TransformCount)
	{
		Result.Error = TEXT("GeometryCollection 的变换层级无效（GlobalMatrices 结果长度不匹配）。");
		return Result;
	}

	// Mass offset per bone. Optional: hand built collections do not have it, and
	// UGeometryCollectionComponent::GetCurrentTransforms falls back to identity
	// in that case, which we mirror here.
	const TManagedArray<FTransform>* MassToLocal =
		GC.FindAttribute<FTransform>(MassToLocalAttributeName, FGeometryCollection::TransformGroup);

	// ------------------------------------------------------------------------
	// Rest bone matrices, in the same space GetCurrentTransforms reports, so
	// that a bone delta of Cur * Rest^-1 is the identity while nothing moves.
	// ------------------------------------------------------------------------
	TArray<FMatrix44f> RestBoneMatrices;
	RestBoneMatrices.SetNumUninitialized(TransformCount);
	for (int32 BoneIdx = 0; BoneIdx < TransformCount; ++BoneIdx)
	{
		const FTransform MassOffset = (MassToLocal && MassToLocal->IsValidIndex(BoneIdx))
			? (*MassToLocal)[BoneIdx]
			: FTransform::Identity;
		RestBoneMatrices[BoneIdx] = FMatrix44f((MassOffset * ComponentSpaceTransforms[BoneIdx]).ToMatrixWithScale());
	}

	// ------------------------------------------------------------------------
	// Build one OBB per piece out of the geometry bounding boxes. The boxes are
	// stored in bone local space (FGeometryCollection::UpdateBoundingBox) and
	// are placed with the *uncorrected* component space transform, which is what
	// actually moves geometry in component space.
	// ------------------------------------------------------------------------
	TArray<FPieceObb> Pieces;
	Pieces.Reserve(GeometryCount);

	/** Geometry index -> piece index, INDEX_NONE for skipped geometries. */
	TArray<int32> GeometryToPiece;
	GeometryToPiece.Init(INDEX_NONE, GeometryCount);

	const float MinHalfExtent = FMath::Max(Params.MinExtentCm, 0.0f);
	for (int32 GeomIdx = 0; GeomIdx < GeometryCount; ++GeomIdx)
	{
		const int32 BoneIdx = GC.TransformIndex[GeomIdx];
		if (BoneIdx == INDEX_NONE || !ComponentSpaceTransforms.IsValidIndex(BoneIdx))
		{
			continue;
		}

		const FBox& LocalBox = GC.BoundingBox[GeomIdx];
		if (!LocalBox.IsValid)
		{
			continue;
		}

		const FTransform& BoneXf = ComponentSpaceTransforms[BoneIdx];
		const FMatrix BoneToComponent = BoneXf.ToMatrixWithScale();

		FPieceObb Piece;
		Piece.BoneIndex = BoneIdx;
		Piece.Center = MatrixTransformPoint(BoneToComponent, LocalBox.GetCenter());

		const FVector LocalExtent = LocalBox.GetExtent();
		Piece.Extent = FVector(
			FMath::Max((float)LocalExtent.X, MinHalfExtent),
			FMath::Max((float)LocalExtent.Y, MinHalfExtent),
			FMath::Max((float)LocalExtent.Z, MinHalfExtent));

		FMatrix ObbToComponent = BoneToComponent;
		ObbToComponent.SetOrigin(Piece.Center);
		Piece.ComponentToObb = ObbToComponent.Inverse();

		// Exact AABB of the OBB: per axis, the sum of the absolute projections
		// of the three scaled OBB axes.
		FVector AabbExtent = FVector::ZeroVector;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			AabbExtent[Axis] =
				FMath::Abs(BoneToComponent.M[0][Axis]) * Piece.Extent.X +
				FMath::Abs(BoneToComponent.M[1][Axis]) * Piece.Extent.Y +
				FMath::Abs(BoneToComponent.M[2][Axis]) * Piece.Extent.Z;
		}
		FBox Aabb(Piece.Center - AabbExtent, Piece.Center + AabbExtent);
		Piece.ComponentAabb = Aabb;

		GeometryToPiece[GeomIdx] = Pieces.Num();
		Pieces.Add(Piece);
	}

	if (Pieces.Num() == 0)
	{
		Result.Error = TEXT("该 GeometryCollection 的碎片没有几何包围盒，无法绑定（请确认已在破裂模式中切割代理网格）。");
		return Result;
	}

	FLookupGrid Grid;
	FBox ComponentBounds(ForceInit);
	{
		FString GridError;
		if (!BuildLookupGrid(Pieces, Params, Grid, ComponentBounds, GridError))
		{
			Result.Error = GridError;
			return Result;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("GaussianSplatBoneBind: %d bones / %d pieces, grid %dx%dx%d @ %.2f cm"),
		TransformCount, Pieces.Num(), Grid.Size.X, Grid.Size.Y, Grid.Size.Z, Grid.CellSize);

	// ------------------------------------------------------------------------
	// Cut surface census. Every triangle the planar cut tools create is flagged
	// "Internal", so the exposed section of a piece is exactly its internal face
	// set. Areas are measured in component space so the splat density below has
	// the same units as the piece's own surface.
	// ------------------------------------------------------------------------
	const TManagedArray<bool>* InternalFaceAttr =
		GC.FindAttribute<bool>(InternalAttributeName, FGeometryCollection::FacesGroup);
	const bool bHasInternalFaceAttr = (InternalFaceAttr != nullptr);

	// Vertex positions live in bone local space (see GeometryCollection.h); the
	// BoneMap indirection maps them to the transform that places them in
	// component space, i.e. the same space the piece OBBs use above.
	auto ComponentSpaceVertex = [&GC, &ComponentSpaceTransforms](int32 VertexIdx) -> FVector
	{
		const FVector3f& Local = GC.Vertex[VertexIdx];
		const int32 VertexBone = GC.BoneMap.IsValidIndex(VertexIdx) ? GC.BoneMap[VertexIdx] : INDEX_NONE;
		if (!ComponentSpaceTransforms.IsValidIndex(VertexBone))
		{
			return FVector(Local.X, Local.Y, Local.Z);
		}
		return ComponentSpaceTransforms[VertexBone].TransformPosition(FVector(Local.X, Local.Y, Local.Z));
	};

	TArray<FPieceCapSource> CapSources;
	CapSources.SetNum(Pieces.Num());

	int32 InternalFaceTotal = 0;
	for (int32 GeomIdx = 0; GeomIdx < GeometryCount; ++GeomIdx)
	{
		const int32 PieceIdx = GeometryToPiece[GeomIdx];
		if (PieceIdx == INDEX_NONE || GC.FaceCount[GeomIdx] <= 0)
		{
			continue;
		}

		const int32 FaceBegin = GC.FaceStart[GeomIdx];
		const int32 FaceEnd = FaceBegin + GC.FaceCount[GeomIdx];
		FPieceCapSource& Source = CapSources[PieceIdx];

		for (int32 FaceIdx = FMath::Max(FaceBegin, 0); FaceIdx < FaceEnd; ++FaceIdx)
		{
			// Hidden faces (cut away by a previous boolean step) cannot show a
			// section, so they neither carry area nor contribute samples.
			if (!GC.Indices.IsValidIndex(FaceIdx) || !GC.Visible[FaceIdx])
			{
				continue;
			}

			// "Internal" is authoritative; the odd MaterialID encoding is the
			// legacy convention and is only consulted when the attribute is
			// absent (see GeometryCollectionEngineConversion.cpp).
			const bool bInternalFace = bHasInternalFaceAttr
				? (*InternalFaceAttr)[FaceIdx]
				: ((GC.MaterialID[FaceIdx] & 1) != 0);

			const FIntVector& Tri = GC.Indices[FaceIdx];
			if (!GC.Vertex.IsValidIndex(Tri.X) || !GC.Vertex.IsValidIndex(Tri.Y) || !GC.Vertex.IsValidIndex(Tri.Z))
			{
				continue;
			}

			const FVector V0 = ComponentSpaceVertex(Tri.X);
			const FVector V1 = ComponentSpaceVertex(Tri.Y);
			const FVector V2 = ComponentSpaceVertex(Tri.Z);
			const double Area = FVector::CrossProduct(V1 - V0, V2 - V0).Size() * 0.5;

			if (bInternalFace)
			{
				Source.InternalArea += Area;
				Source.InternalFaces.Add(FaceIdx);
			}
			else
			{
				Source.ExternalArea += Area;
			}
		}

		if (Source.InternalFaces.Num() == 0)
		{
			continue;
		}

		// Area weighted sampling CDF, normalised to (0, 1].
		Source.InternalCdf.SetNumUninitialized(Source.InternalFaces.Num());
		double Running = 0.0;
		for (int32 FaceStep = 0; FaceStep < Source.InternalFaces.Num(); ++FaceStep)
		{
			const FIntVector& Tri = GC.Indices[Source.InternalFaces[FaceStep]];
			const FVector V0 = ComponentSpaceVertex(Tri.X);
			const FVector V1 = ComponentSpaceVertex(Tri.Y);
			const FVector V2 = ComponentSpaceVertex(Tri.Z);
			Running += FVector::CrossProduct(V1 - V0, V2 - V0).Size() * 0.5;
			Source.InternalCdf[FaceStep] = Running;
		}

		if (Running > UE_DOUBLE_SMALL_NUMBER)
		{
			Source.InternalArea = Running;
			for (double& Cdf : Source.InternalCdf)
			{
				Cdf /= Running;
			}
		}
		else
		{
			// Sliver cut faces with no measurable area: sample them uniformly so
			// the piece still gets its minimum number of caps.
			Source.InternalArea = 0.0;
			for (int32 FaceStep = 0; FaceStep < Source.InternalCdf.Num(); ++FaceStep)
			{
				Source.InternalCdf[FaceStep] = (double)(FaceStep + 1) / (double)Source.InternalCdf.Num();
			}
		}

		InternalFaceTotal += Source.InternalFaces.Num();
	}

	// ------------------------------------------------------------------------
	// Per splat assignment. Zero copy streaming read of the position block: a
	// 100M splat asset must never materialise a TArray<FVector>.
	// ------------------------------------------------------------------------
	int64 DataSize = 0;
	const uint8* Data = static_cast<const uint8*>(Asset->LockPositionDataReadOnly(&DataSize));
	// Rotation/scale and colour are only read to give the cut surface splats a
	// plausible look; a missing or truncated block only means caps fall back to
	// defaults, so neither lock is fatal.
	int64 OtherDataSize = 0;
	const uint8* OtherData = static_cast<const uint8*>(Asset->LockOtherDataReadOnly(&OtherDataSize));
	int64 ColorDataSize = 0;
	const uint8* ColorData = static_cast<const uint8*>(Asset->LockColorTextureDataReadOnly(&ColorDataSize));
	ON_SCOPE_EXIT
	{
		Asset->UnlockPositionData();
		Asset->UnlockOtherData();
		Asset->UnlockColorTextureData();
	};

	if (!Data)
	{
		Result.Error = TEXT("无法锁定位置数据（bulk data 为空或已被占用）。");
		return Result;
	}

	const int64 ExpectedSize = (int64)SplatCount * BindPositionStride;
	if (DataSize < ExpectedSize)
	{
		Result.Error = FString::Printf(TEXT("位置数据长度异常：实际 %s 字节，期望至少 %s 字节。"),
			*LexToString(DataSize), *LexToString(ExpectedSize));
		return Result;
	}

	const float* PositionData = reinterpret_cast<const float*>(Data);

	const float* OtherFloats = (OtherData && OtherDataSize >= (int64)SplatCount * BindOtherStride)
		? reinterpret_cast<const float*>(OtherData)
		: nullptr;

	const int32 ColorTexWidth = Asset->ColorTextureWidth;
	const int32 ColorTexHeight = Asset->ColorTextureHeight;
	const FFloat16Color* ColorPixels = (ColorData && ColorDataSize > 0 && ColorTexWidth > 0 && ColorTexHeight > 0)
		? reinterpret_cast<const FFloat16Color*>(ColorData)
		: nullptr;

	auto ResolveBone = [&Grid, &Pieces, &Params](const FVector& P, bool& bOutOutsideBody) -> int32
	{
		bOutOutsideBody = false;

		const FIntVector Cell = Grid.PointToCell(P);
		float BestScore = TNumericLimits<float>::Max();
		int32 BestBone = INDEX_NONE;
		bool bContained = false;

		for (int32 Ring = 0; Ring <= Params.MaxCoarseRings && !bContained; ++Ring)
		{
			for (int32 DZ = -Ring; DZ <= Ring; ++DZ)
			{
				for (int32 DY = -Ring; DY <= Ring; ++DY)
				{
					for (int32 DX = -Ring; DX <= Ring; ++DX)
					{
						// Only walk the shell at this radius.
						if (Ring > 0 && FMath::Abs(DX) != Ring && FMath::Abs(DY) != Ring && FMath::Abs(DZ) != Ring)
						{
							continue;
						}

						const int32 X = Cell.X + DX;
						const int32 Y = Cell.Y + DY;
						const int32 Z = Cell.Z + DZ;
						if (X < 0 || Y < 0 || Z < 0 || X >= Grid.Size.X || Y >= Grid.Size.Y || Z >= Grid.Size.Z)
						{
							continue;
						}

						const int32 CellIdx = Grid.CellIndex(X, Y, Z);
						for (int32 ItemIdx = Grid.CellStart[CellIdx]; ItemIdx < Grid.CellStart[CellIdx + 1]; ++ItemIdx)
						{
							const FPieceObb& Piece = Pieces[Grid.CellItems[ItemIdx]];
							const float Score = PieceScore(Piece, P);
							if (Score < BestScore - Params.EpsilonCm ||
								(Score <= BestScore + Params.EpsilonCm && (BestBone == INDEX_NONE || Piece.BoneIndex < BestBone)))
							{
								BestScore = Score;
								BestBone = Piece.BoneIndex;
							}
							if (Score <= 1.0f)
							{
								bContained = true;
							}
						}
					}
				}
			}
			// A piece that contains the splat always lists the splat's own cell,
			// so ring 0 already found it and deeper rings cannot improve.
		}

		if (BestBone == INDEX_NONE)
		{
			// Splat far outside every piece: exact scan, still deterministic.
			for (int32 PieceIdx = 0; PieceIdx < Pieces.Num(); ++PieceIdx)
			{
				const FPieceObb& Piece = Pieces[PieceIdx];
				const float Score = PieceScore(Piece, P);
				if (Score < BestScore - Params.EpsilonCm ||
					(Score <= BestScore + Params.EpsilonCm && (BestBone == INDEX_NONE || Piece.BoneIndex < BestBone)))
				{
					BestScore = Score;
					BestBone = Piece.BoneIndex;
				}
			}
		}

		bOutOutsideBody = (BestScore > 1.0f);
		return BestBone;
	};

	TArray<uint32> SplatBoneIndices;
	SplatBoneIndices.SetNumUninitialized(SplatCount);

	TArray<int32> PieceUseCount;
	PieceUseCount.SetNumZeroed(Pieces.Num());
	TArray<int32> BoneToPiece;
	BoneToPiece.Init(INDEX_NONE, TransformCount);
	for (int32 PieceIdx = 0; PieceIdx < Pieces.Num(); ++PieceIdx)
	{
		BoneToPiece[Pieces[PieceIdx].BoneIndex] = PieceIdx;
	}

	// Cap look-up tables. A cap splat has to look like the surface it was cut
	// from, so the average colour, opacity and splat size of every piece are
	// accumulated while its splats stream past anyway.
	TArray<FVector> PieceColorSum;
	TArray<double> PieceOpacitySum;
	TArray<double> PieceScaleSum;
	PieceColorSum.SetNumZeroed(Pieces.Num());
	PieceOpacitySum.SetNumZeroed(Pieces.Num());
	PieceScaleSum.SetNumZeroed(Pieces.Num());

	FVector GlobalColorSum = FVector::ZeroVector;
	double GlobalOpacitySum = 0.0;
	double GlobalScaleSum = 0.0;
	int64 ColorSampleCount = 0;
	int64 ScaleSampleCount = 0;

	int32 BoundSplatCount = 0;
	int32 CoarseSplatCount = 0;
	int32 ClippedSplatCount = 0;
	int32 NonFiniteCount = 0;

	// Estimation-grid clipping. The voxel shell bounds are the body the user
	// authored: if they moved the estimation grid (Fracture->GridOriginOffset)
	// to push the floor, the background or a stray cluster off it, those splats
	// must not follow a fracture piece either. They are flagged as dropped in
	// the index block, which the render shader uses to skip them, so the source
	// asset itself is never modified.
	const FBox ClipBounds = Fracture->VoxelShellBounds;
	const bool bClipToShell = ClipBounds.IsValid != 0;

	const int32 BlockSize = FMath::Max(Params.SplatBlockSize, 1);
	for (int32 BlockStart = 0; BlockStart < SplatCount; BlockStart += BlockSize)
	{
		const int32 BlockEnd = FMath::Min(BlockStart + BlockSize, SplatCount);
		const float* BlockPtr = PositionData + (int64)BlockStart * 3;

		for (int32 Index = BlockStart; Index < BlockEnd; ++Index)
		{
			const float* P = BlockPtr + (int64)(Index - BlockStart) * 3;

			if (!IsFinitePosition(P))
			{
				// Still needs an index (the block covers every splat), park it on
				// bone 0 and report it.
				SplatBoneIndices[Index] = 0;
				++NonFiniteCount;
				continue;
			}

			const FVector SplatPosition(P[0], P[1], P[2]);

			// Outside the estimation grid => dropped, not bound. See bClipToShell.
			if (bClipToShell && !ClipBounds.IsInsideOrOn(SplatPosition))
			{
				SplatBoneIndices[Index] = GAUSSIAN_SPLAT_BONE_INDEX_DROPPED;
				++ClippedSplatCount;
				continue;
			}

			bool bOutsideBody = false;
			const int32 BoneIdx = ResolveBone(SplatPosition, bOutsideBody);
			if (BoneIdx == INDEX_NONE)
			{
				SplatBoneIndices[Index] = 0;
				continue;
			}

			SplatBoneIndices[Index] = (uint32)BoneIdx;
			++BoundSplatCount;
			if (bOutsideBody)
			{
				++CoarseSplatCount;
			}
			const int32 PieceIdx = BoneToPiece[BoneIdx];
			if (PieceIdx != INDEX_NONE)
			{
				++PieceUseCount[PieceIdx];

				if (OtherFloats)
				{
					const float* SplatScale = OtherFloats + (int64)Index * BindOtherFloats + 4;
					const double ScaleAvg =
						((double)SplatScale[0] + (double)SplatScale[1] + (double)SplatScale[2]) / 3.0;
					if (FMath::IsFinite(ScaleAvg) && ScaleAvg > 0.0)
					{
						PieceScaleSum[PieceIdx] += ScaleAvg;
						GlobalScaleSum += ScaleAvg;
						++ScaleSampleCount;
					}
				}

				if (ColorPixels)
				{
					int32 TexX = 0;
					int32 TexY = 0;
					GaussianSplattingUtils::SplatIndexToTextureCoord(Index, ColorTexWidth, TexX, TexY);
					if (TexY < ColorTexHeight)
					{
						const FFloat16Color& Pixel = ColorPixels[TexY * ColorTexWidth + TexX];
						const FVector Rgb(
							FMath::Clamp(Pixel.R.GetFloat(), 0.0f, 1.0f),
							FMath::Clamp(Pixel.G.GetFloat(), 0.0f, 1.0f),
							FMath::Clamp(Pixel.B.GetFloat(), 0.0f, 1.0f));
						const double Opacity = FMath::Clamp(Pixel.A.GetFloat(), 0.0f, 1.0f);

						PieceColorSum[PieceIdx] += Rgb;
						PieceOpacitySum[PieceIdx] += Opacity;
						GlobalColorSum += Rgb;
						GlobalOpacitySum += Opacity;
						++ColorSampleCount;
					}
				}
			}
		}
	}

	if (ClippedSplatCount > 0 && BoundSplatCount == 0)
	{
		Result.Error = FString::Printf(
			TEXT("全部 %d 个 splat 都落在估算网格（体素壳包围盒 %s）之外，破裂里将没有任何 splat。\n")
			TEXT("请把「估算网格位置偏移」改回接近 (0,0,0) 并重新生成体素壳。"),
			ClippedSplatCount, *ClipBounds.ToString());
		return Result;
	}

	int32 EmptyPieceCount = 0;
	int32 LargestPieceCount = 0;
	for (int32 PieceIdx = 0; PieceIdx < Pieces.Num(); ++PieceIdx)
	{
		if (PieceUseCount[PieceIdx] == 0)
		{
			++EmptyPieceCount;
		}
		LargestPieceCount = FMath::Max(LargestPieceCount, PieceUseCount[PieceIdx]);
	}

	const FVector3f BoundsMin((float)ComponentBounds.Min.X, (float)ComponentBounds.Min.Y, (float)ComponentBounds.Min.Z);
	const FVector3f BoundsMax((float)ComponentBounds.Max.X, (float)ComponentBounds.Max.Y, (float)ComponentBounds.Max.Z);

	// ------------------------------------------------------------------------
	// Cut surface (cap) splats. The triangles a cut creates carry no splat of
	// their own, which is exactly why the section reads as a hole. The piece's
	// own splat count against its external surface area gives the density the
	// section has to match, so a section ends up about as dense as the surface
	// it was cut from. Samples are area weighted across the piece's cut faces.
	// ------------------------------------------------------------------------
	TArray<FGaussianSplatCap> Caps;
	TArray<uint32> CapBoneIndices;
	int32 CutPieceCount = 0;

	const float CapFlatness = FMath::Clamp(Params.CapFlatness, 0.01f, 1.0f);
	const float CapDensityScale = FMath::Max(Params.CapDensityScale, 0.0f);
	const int32 MinCapsPerCutPiece = FMath::Max(Params.MinCapsPerCutPiece, 0);
	const int32 MaxCapsPerPiece = FMath::Max(Params.MaxCapsPerPiece, MinCapsPerCutPiece);
	const int64 MaxCaps = FMath::Max((int64)Params.MaxCapSplatCount, 0ll);

	if (MaxCaps > 0 && InternalFaceTotal > 0)
	{
		// Density fallback for pieces that own no splat at all, plus the look
		// fallbacks for assets whose colour or rotation block is missing.
		double TotalExternalArea = 0.0;
		for (const FPieceCapSource& Source : CapSources)
		{
			TotalExternalArea += Source.ExternalArea;
		}
		const double GlobalDensity = (TotalExternalArea > UE_DOUBLE_SMALL_NUMBER && BoundSplatCount > 0)
			? (double)BoundSplatCount / TotalExternalArea
			: 0.0;
		const FVector GlobalColor = (ColorSampleCount > 0)
			? GlobalColorSum / (double)ColorSampleCount
			: FVector(1.0, 1.0, 1.0);
		const double GlobalOpacity = (ColorSampleCount > 0)
			? GlobalOpacitySum / (double)ColorSampleCount
			: 1.0;
		double GlobalScale = (ScaleSampleCount > 0) ? GlobalScaleSum / (double)ScaleSampleCount : 0.0;
		if (!(GlobalScale > 0.0))
		{
			GlobalScale = (double)Params.MinExtentCm;
		}

		for (int32 PieceIdx = 0; PieceIdx < CapSources.Num(); ++PieceIdx)
		{
			const FPieceCapSource& Source = CapSources[PieceIdx];
			const int32 FaceStepCount = Source.InternalFaces.Num();
			if (FaceStepCount == 0)
			{
				continue;
			}
			if ((int64)Caps.Num() >= MaxCaps)
			{
				break;
			}

			const int32 PieceUse = PieceUseCount[PieceIdx];
			const double Density = (PieceUse > 0 && Source.ExternalArea > UE_DOUBLE_SMALL_NUMBER)
				? (double)PieceUse / Source.ExternalArea
				: GlobalDensity;

			const int64 Wanted = (int64)FMath::RoundToDouble(Source.InternalArea * Density * (double)CapDensityScale);
			const int64 Remaining = MaxCaps - (int64)Caps.Num();
			int32 NumCaps = (int32)FMath::Min<int64>(
				FMath::Clamp<int64>(Wanted, (int64)MinCapsPerCutPiece, (int64)MaxCapsPerPiece),
				Remaining);
			if (NumCaps <= 0)
			{
				continue;
			}

			const FVector CapRgb = (ColorPixels && PieceUse > 0)
				? PieceColorSum[PieceIdx] / (double)PieceUse
				: GlobalColor;
			// A section must not read as a hole, so it never goes fully soft.
			const double CapOpacity = FMath::Max(
				(ColorPixels && PieceUse > 0) ? PieceOpacitySum[PieceIdx] / (double)PieceUse : GlobalOpacity,
				(double)BindMinCapOpacity);
			double CapScale = (OtherFloats && PieceUse > 0)
				? PieceScaleSum[PieceIdx] / (double)PieceUse
				: GlobalScale;
			if (!(CapScale > 0.0))
			{
				CapScale = GlobalScale;
			}

			const uint32 CapBoneIndex = (uint32)Pieces[PieceIdx].BoneIndex;
			const int32 FirstCapIdx = Caps.Num();
			FRandomStream Stream(Params.CapRandomSeed + PieceIdx);

			for (int32 CapIdx = 0; CapIdx < NumCaps; ++CapIdx)
			{
				// Area weighted triangle pick over the ascending CDF.
				const double Pick = Stream.FRand();
				int32 Low = 0;
				int32 High = FaceStepCount - 1;
				while (Low < High)
				{
					const int32 Mid = Low + (High - Low) / 2;
					if (Source.InternalCdf[Mid] < Pick)
					{
						Low = Mid + 1;
					}
					else
					{
						High = Mid;
					}
				}

				const FIntVector& Tri = GC.Indices[Source.InternalFaces[Low]];
				const FVector V0 = ComponentSpaceVertex(Tri.X);
				const FVector Edge1 = ComponentSpaceVertex(Tri.Y) - V0;
				const FVector Edge2 = ComponentSpaceVertex(Tri.Z) - V0;

				const FVector Normal = FVector::CrossProduct(Edge1, Edge2).GetSafeNormal();
				if (Normal.IsNearlyZero())
				{
					continue;
				}

				// Uniform barycentric sample; folding the outer half back in
				// keeps the distribution uniform without a rejection loop.
				double U = Stream.FRand();
				double V = Stream.FRand();
				if (U + V > 1.0)
				{
					U = 1.0 - U;
					V = 1.0 - V;
				}
				const FVector Position = V0 + Edge1 * U + Edge2 * V;

				// A cap is a flat disc: its local +Z is the section normal and
				// its Z extent is squeezed by the flatness factor.
				FGaussianSplatCap Cap;
				Cap.Position = FVector3f((float)Position.X, (float)Position.Y, (float)Position.Z);
				Cap.Rotation = FQuat4f::FindBetweenNormals(
					FVector3f(0.0f, 0.0f, 1.0f),
					FVector3f((float)Normal.X, (float)Normal.Y, (float)Normal.Z));
				Cap.Scale = FVector3f((float)CapScale, (float)CapScale, (float)(CapScale * (double)CapFlatness));
				Cap.Color = FLinearColor((float)CapRgb.X, (float)CapRgb.Y, (float)CapRgb.Z, (float)CapOpacity);

				Caps.Add(Cap);
				CapBoneIndices.Add(CapBoneIndex);
			}

			if (Caps.Num() > FirstCapIdx)
			{
				++CutPieceCount;
			}
		}
	}

	// The binding validates its cap indices against the cap block, so the caps
	// have to land first. Clearing is only worth a dirty mark when the fracture
	// asset actually carried caps before.
	if (Caps.Num() > 0 || Fracture->GetCapSplatCount() > 0)
	{
		Fracture->SetFractureCaps(Caps);
	}

	if (!Fracture->SetBoneBinding(RestBoneMatrices, SplatBoneIndices, BoundsMin, BoundsMax, CapBoneIndices))
	{
		Result.Error = TEXT("写入破裂骨头绑定失败（破裂资产拒绝该绑定数据，详见日志）。");
		return Result;
	}

	Result.bSuccess = true;
	Result.BoneCount = TransformCount;
	Result.PieceCount = Pieces.Num();
	Result.BoundSplatCount = BoundSplatCount;
	Result.CoarseSplatCount = CoarseSplatCount;
	Result.ClippedSplatCount = ClippedSplatCount;
	Result.EmptyBoneCount = EmptyPieceCount;
	Result.LargestPieceCount = LargestPieceCount;
	Result.GridSize = Grid.Size;
	Result.CellSizeCm = Grid.CellSize;
	Result.CutPieceCount = CutPieceCount;
	Result.CapSplatCount = Caps.Num();

	Result.Summary = FString::Printf(
		TEXT("骨头总数: %d\n")
		TEXT("碎片数: %d\n")
		TEXT("已绑定 splat: %s\n")
		TEXT("网格外剔除: %s\n")
		TEXT("碎片体外回退: %s\n")
		TEXT("没有 splat 的碎片: %d\n")
		TEXT("最大碎片 splat 数: %d\n")
		TEXT("有断面的碎片: %d\n")
		TEXT("断面填充 splat: %d\n")
		TEXT("查找网格: %d x %d x %d @ %.2f cm\n")
		TEXT("位置数据异常: %s"),
		TransformCount,
		Pieces.Num(),
		*LexToString((int64)BoundSplatCount),
		*LexToString((int64)ClippedSplatCount),
		*LexToString((int64)CoarseSplatCount),
		EmptyPieceCount,
		LargestPieceCount,
		CutPieceCount,
		Caps.Num(),
		Grid.Size.X, Grid.Size.Y, Grid.Size.Z, Grid.CellSize,
		*LexToString((int64)NonFiniteCount));

	if (NonFiniteCount > 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("GaussianSplatBoneBind: %d splats have non finite positions and were parked on bone 0."),
			NonFiniteCount);
	}

	UE_LOG(LogTemp, Log, TEXT("GaussianSplatBoneBind: bound %d / %d splats to %d pieces (%d outside the proxy body)."),
		BoundSplatCount, SplatCount, Pieces.Num(), CoarseSplatCount);

	return Result;
}