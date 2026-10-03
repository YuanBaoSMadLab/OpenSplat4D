// Copyright OpenSplat4D 2026 All Rights Reserved.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UGaussianSplatAsset;
class UGaussianSplatFractureAsset;
class UGeometryCollection;

/**
 * Binds every splat of a Gaussian splat asset to one fracture bone of a
 * previously cut UGeometryCollection.
 *
 * The proxy mesh produced by FGaussianSplatProxyMeshBuilder lives in the very
 * same space as the splat positions (asset local centimetres), so cutting that
 * mesh in Fracture Mode yields a collection whose component space is identical
 * to the asset local space, provided the user cuts the mesh at the origin with
 * an identity transform. Under that assumption each splat can be assigned to
 * the bone of the piece that contains it.
 *
 * Assignment criterion is the signed distance to each piece's oriented bounding
 * box (OBB), built from the piece's own vertices in bone local space. The piece
 * with the smallest signed distance wins, i.e. the piece the splat is deepest
 * inside; ties are broken by the lower bone index so the result is fully
 * deterministic.
 */
struct FGaussianSplatBoneBindParams
{
	/** Distance tolerance (cm) used for deterministic tie breaking. */
	float EpsilonCm = 0.001f;

	/** Minimum OBB half extent per axis (cm); keeps razor thin pieces usable. */
	float MinExtentCm = 0.05f;

	/** Coarse lookup rings around a splat's cell before falling back to the
	 *  per cell nearest piece. 2 is enough for every splat that lives inside
	 *  the proxy body, which is the overwhelming majority. */
	int32 MaxCoarseRings = 2;

	/** Upper bound on the uniform lookup grid allocation. */
	int64 GridCellBudget = 4ll * 1024 * 1024;

	/** Splats processed per streaming block while reading the position block. */
	int32 SplatBlockSize = 1 << 20;

	/** Flatness of a cap splat along its surface normal (0.01 .. 1). 0.25 turns
	 *  the cap into a flat disc instead of a ball. */
	float CapFlatness = 0.25f;

	/** Cap density relative to the piece's own splat surface density. 1.0 makes
	 *  the cut surface as dense as the piece's original surface. */
	float CapDensityScale = 1.0f;

	/** Minimum number of caps on a piece that exposes a cut surface. */
	int32 MinCapsPerCutPiece = 8;

	/** Upper bound on a single piece's cap count. */
	int32 MaxCapsPerPiece = 8192;

	/** Global cap budget; generation stops once it is reached. 0 disables caps. */
	int32 MaxCapSplatCount = 2000000;

	/** Deterministic sampling seed, so repeated binds produce identical caps. */
	int32 CapRandomSeed = 20260924;
};

struct FGaussianSplatBoneBindResult
{
	bool bSuccess = false;
	FString Error;
	FString Summary;

	/** Bone count written into the asset (all transforms of the collection). */
	int32 BoneCount = 0;
	/** Leaf bones that actually own geometry, i.e. the selectable pieces. */
	int32 PieceCount = 0;
	int32 BoundSplatCount = 0;
	/** Splats resolved through the per cell fallback (outside the proxy body). */
	int32 CoarseSplatCount = 0;
	/** Splats flagged as dropped because they lie outside the fracture's
	 *  estimation grid; they are not rendered as part of the fracture. */
	int32 ClippedSplatCount = 0;
	/** Pieces no splat was assigned to. */
	int32 EmptyBoneCount = 0;
	int32 LargestPieceCount = 0;
	FIntVector GridSize = FIntVector::ZeroValue;
	float CellSizeCm = 0.0f;

	/** Pieces that expose a cut surface and received cap splats. */
	int32 CutPieceCount = 0;
	/** Cut surface (cap) splats written into the asset. */
	int32 CapSplatCount = 0;
};

class FGaussianSplatBoneBinder
{
public:
	/**
	 * Assign every splat of Asset to a bone of Collection.
	 *
	 * @param Asset     Source splat asset: read only (positions, format, 4D flag).
	 * @param Fracture  Destination fracture asset: the bone binding and the cut
	 *                  surface caps are written here (v11; the splat asset no
	 *                  longer carries a binding).
	 * @return Result with bSuccess false and Error set when the binding could
	 *         not be produced; neither asset is touched in that case.
	 */
	static FGaussianSplatBoneBindResult BindAssetToCollection(
		UGaussianSplatAsset* Asset,
		UGaussianSplatFractureAsset* Fracture,
		const UGeometryCollection* Collection,
		const FGaussianSplatBoneBindParams& Params = FGaussianSplatBoneBindParams());
};
