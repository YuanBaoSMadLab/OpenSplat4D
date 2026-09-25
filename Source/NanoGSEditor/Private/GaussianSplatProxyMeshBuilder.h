// Copyright Epic Games, Inc. All Rights Reserved.
// Copyright OpenSplat4D 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MeshDescription.h"

class UGaussianSplatAsset;
class UGaussianSplatFractureAsset;
class UStaticMesh;

/**
 * Tunables for the blocky voxel proxy mesh.
 *
 * The proxy mesh is the fracture source of a Gaussian Splat asset: a closed,
 * manifold, axis-aligned block hull that a user cuts in the Chaos Fracture
 * Mode to author a UGeometryCollection. It is deliberately coarse -- its only
 * job is to give Chaos a watertight solid to cut.
 *
 * All coordinates are asset-local centimetres, i.e. exactly the space the
 * splat positions live in, so the mesh lines up with the splat actor with no
 * extra transform and can later be bound to the collection bones directly.
 */
struct FGaussianSplatProxyMeshParams
{
	/** Edge length of one voxel (cm). <= 0 => auto = max(trimmed diagonal / 192, 1 cm). */
	float VoxelSize = 0.0f;

	/** Position offset (cm, asset local space) applied on top of the estimation
	 *  grid's automatically derived minimum corner.
	 *
	 *  The grid is what decides which splats belong to the body: a splat that
	 *  falls outside it neither fills a voxel nor (through the shell bounds)
	 *  ends up in the fracture. That makes moving the grid the way the fracture
	 *  editor excludes parts of a capture -- floor, background, stray floaters --
	 *  without touching the source asset. Zero keeps the grid on the trimmed
	 *  cloud bounds. */
	FVector GridOriginOffset = FVector::ZeroVector;

	/** Per-axis trimming quantile (0.01 => drop the lowest/highest 1%) so stray
	 *  floaters cannot inflate the grid and destroy the effective resolution. */
	float TrimPercentile = 0.01f;

	/** A voxel becomes solid only when it holds at least this many splats. This
	 *  is the second half of the floater defence: a sparse outlier shell is
	 *  rejected even when it falls inside the trimmed bounds. */
	int32 MinSplatsPerVoxel = 3;

	/** One 6-neighbour dilation pass. Cells that touch only along an edge or a
	 *  corner produce non-manifold edges, which PlanarCut cannot handle. */
	bool bDilate = true;

	/** Guardrails. The voxel size is doubled (up to a fixed attempt count) until
	 *  the grid fits; exceeding MaxTriangles is a hard failure. */
	int32 MaxCellsPerAxis = 256;
	int64 MaxOccupiedVoxels = 4ll * 1024 * 1024;
	int64 MaxTriangles = 4ll * 1024 * 1024;
};

/** Outcome of a proxy mesh build (also used to fill the result dialog). */
struct FGaussianSplatProxyMeshResult
{
	bool bSuccess = false;
	FString Error;
	FString Summary;

	float EffectiveVoxelSize = 0.0f;
	FVector GridOrigin = FVector::ZeroVector;
	FIntVector GridSize = FIntVector::ZeroValue;
	int64 OccupiedVoxels = 0;
	/** Splats that fell outside the estimation grid and were therefore not
	 *  allowed to contribute to the shell (see GridOriginOffset). */
	int64 ClippedSplats = 0;
	int32 QuadCount = 0;
	int32 TriangleCount = 0;
	/** Edges referenced by a number of faces other than 2 (should be 0). */
	int32 NonManifoldEdgeCount = 0;
};

/**
 * Builds a closed blocky proxy mesh out of the splat positions of an asset.
 *
 * Pipeline: trimmed bounds (quantile) -> occupancy grid (density threshold) ->
 * dilation -> face extraction with greedy rectangle merge -> welded manifold
 * mesh in a FMeshDescription. Nothing here touches the render path.
 */
class FGaussianSplatProxyMeshBuilder
{
public:
	/**
	 * Fill OutMesh with the proxy mesh. OutMesh is emptied first.
	 * On failure OutMesh must be discarded by the caller (it is left partial).
	 * Only reads positions through the zero-copy LockPositionDataReadOnly().
	 */
	static FGaussianSplatProxyMeshResult BuildMeshDescription(
		const UGaussianSplatAsset* Asset,
		const FGaussianSplatProxyMeshParams& Params,
		FMeshDescription& OutMesh);

	// [废弃管线 · 已停用] 旧做法是把代理网格存成独立 UStaticMesh 资产（"<Asset>_Proxy"）。
	// 现行管线：体素壳直接内嵌进 UGaussianSplatFractureAsset（见 BuildVoxelShell），
	// 不再产生任何 StaticMesh 资产。此入口已无调用点，仅注释保留以防误用。
	// static UStaticMesh* CreateProxyMeshAsset(
	// 	const UGaussianSplatAsset* Asset,
	// 	const FGaussianSplatProxyMeshParams& Params,
	// 	FString& OutError,
	// 	FGaussianSplatProxyMeshResult* OutResult = nullptr);

	/**
	 * Extract a raw triangle soup out of a built proxy mesh: welded vertex
	 * positions and three int32 indices per triangle. Quad faces are fan
	 * triangulated. Returns false when the mesh has no usable polygon.
	 */
	static bool ExtractTriangleSoup(
		const FMeshDescription& Mesh,
		TArray<FVector3f>& OutVertices,
		TArray<int32>& OutIndices);

	/**
	 * Build the proxy mesh and store it in FractureAsset as the collision voxel
	 * shell (the fracture asset owns the shell itself, so no StaticMesh asset
	 * is created). Rebuilds any previous shell; on failure the previous shell
	 * is left untouched.
	 * @return True when the shell was written (OutError holds the reason).
	 */
	static bool BuildVoxelShell(
		const UGaussianSplatAsset* Asset,
		UGaussianSplatFractureAsset* FractureAsset,
		const FGaussianSplatProxyMeshParams& Params,
		FString& OutError,
		FGaussianSplatProxyMeshResult* OutResult = nullptr);
};