// Copyright Epic Games, Inc. All Rights Reserved.
// Copyright OpenSplat4D 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "GaussianDataTypes.h"
#include "GaussianClusterTypes.h"
#include "RenderResource.h"
#include "RHI.h"
#include "RHIResources.h"
#include <atomic>

class UGaussianSplatComponent;
class UGaussianSplatAsset;
class UGaussianSplatFractureAsset;
class FGaussianSplatRenderData;
class UBodySetup;
class FLocalVertexFactory;
class FDynamicMeshIndexBuffer32;
struct FStaticMeshVertexBuffers;


/**
 * GPU resources for Gaussian Splatting rendering
 * FBufferRHIRef: Raw GPU Memory
 * ShaderResourceViewRHIRef(SRV): interpret the memory into corresponding type, shader read only
 * UnorderedAccessViewRHIRef(UAV): interpret the memory into corresponding type, shader read/write
 */
class FGaussianSplatGPUResources : public FRenderResource
{
public:
	FGaussianSplatGPUResources();
	virtual ~FGaussianSplatGPUResources();

	/** Initialize resources from asset data. When Fracture is given, the shared
	 *  render data comes from the fracture asset (source splats + bone binding +
	 *  cap splats); otherwise from the plain splat asset. */
	void Initialize(UGaussianSplatAsset* Asset, UGaussianSplatFractureAsset* Fracture = nullptr);

	/** Check if resources are valid */
	bool IsValid() const { return bInitialized && SplatCount > 0 && (PackedSplatBufferSRV.IsValid() || ColorTextureSRV.IsValid()); }

	/** Get number of splats */
	int32 GetSplatCount() const { return SplatCount; }

	//~ Begin FRenderResource Interface
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual void ReleaseRHI() override;
	//~ End FRenderResource Interface

public:
	/** Packed splat data buffer (16 bytes/splat: RGBA + float16 pos + octahedral quat + log scale) */
	FBufferRHIRef PackedSplatBuffer;
	FShaderResourceViewRHIRef PackedSplatBufferSRV;

	/** Position data buffer (legacy, unused when packed format is active) */
	FBufferRHIRef PositionBuffer;
	FShaderResourceViewRHIRef PositionBufferSRV;

	/** Rotation + Scale data buffer (legacy, unused when packed format is active) */
	FBufferRHIRef OtherDataBuffer;
	FShaderResourceViewRHIRef OtherDataBufferSRV;

	/** Spherical harmonics buffer */
	FBufferRHIRef SHBuffer;
	FShaderResourceViewRHIRef SHBufferSRV;

	/** Chunk info buffer */
	FBufferRHIRef ChunkBuffer;
	FShaderResourceViewRHIRef ChunkBufferSRV;

	/** View data buffer (computed per-frame) */
	FBufferRHIRef ViewDataBuffer;
	FUnorderedAccessViewRHIRef ViewDataBufferUAV;
	FShaderResourceViewRHIRef ViewDataBufferSRV;

	/** Sort distance buffer */
	FBufferRHIRef SortDistanceBuffer;
	FUnorderedAccessViewRHIRef SortDistanceBufferUAV;
	FShaderResourceViewRHIRef SortDistanceBufferSRV;

	/** Sort keys buffer (double buffered) */
	FBufferRHIRef SortKeysBuffer;
	FUnorderedAccessViewRHIRef SortKeysBufferUAV;
	FShaderResourceViewRHIRef SortKeysBufferSRV;

	FBufferRHIRef SortKeysBufferAlt;
	FUnorderedAccessViewRHIRef SortKeysBufferAltUAV;
	FShaderResourceViewRHIRef SortKeysBufferAltSRV;

	/** Sort distance buffer alt (for radix sort ping-pong) */
	FBufferRHIRef SortDistanceBufferAlt;
	FUnorderedAccessViewRHIRef SortDistanceBufferAltUAV;

	/** Radix sort histogram buffer */
	FBufferRHIRef RadixHistogramBuffer;
	FUnorderedAccessViewRHIRef RadixHistogramBufferUAV;

	/** Radix sort digit offset buffer */
	FBufferRHIRef RadixDigitOffsetBuffer;
	FUnorderedAccessViewRHIRef RadixDigitOffsetBufferUAV;

	/** Sort indirect dispatch args buffer (for indirect radix sort)
	 * Format: uint3 (NumTiles, 1, 1) - used by CountCS and ScatterCS
	 * Written by PrepareIndirectArgs, read by DispatchIndirectComputeShader
	 */
	FBufferRHIRef SortIndirectArgsBuffer;
	FUnorderedAccessViewRHIRef SortIndirectArgsBufferUAV;

	/** Sort parameters buffer (for indirect radix sort)
	 * Format: uint2 (SortCount, NumTiles)
	 * Written by PrepareIndirectArgs, read by radix sort shaders
	 */
	FBufferRHIRef SortParamsBuffer;
	FUnorderedAccessViewRHIRef SortParamsBufferUAV;
	FShaderResourceViewRHIRef SortParamsBufferSRV;

	/** Index buffer for quad rendering */
	FBufferRHIRef IndexBuffer;

	/** Color texture reference */
	FTextureRHIRef ColorTexture;
	FShaderResourceViewRHIRef ColorTextureSRV;

	/** Dummy white texture for fallback when ColorTexture isn't available */
	FTextureRHIRef DummyWhiteTexture;
	FShaderResourceViewRHIRef DummyWhiteTextureSRV;

	/** Whether the asset has 4D temporal data */
	bool bIs4D = false;

	/** Keyframe 4D data buffer (static, from asset; dummy when not keyframe 4D) */
	FBufferRHIRef KeyframeBuffer;
	FShaderResourceViewRHIRef KeyframeBufferSRV;

	/** Whether the asset uses keyframe 4D playback (overrides temporal marginalization when set) */
	bool bIsKeyframe4D = false;

	/** Number of keyframes in KeyframeBuffer */
	int32 KeyframeFrameCount = 0;

	/** Splats per frame covered by KeyframeBuffer */
	int32 KeyframeSplatCount = 0;

	/** Native 4D data buffer (static, from asset; dummy when not native 4D) */
	FBufferRHIRef Native4DBuffer;
	FShaderResourceViewRHIRef Native4DBufferSRV;

	/** Whether the asset uses native 4D rendering */
	bool bIsNative4D = false;

	/** Number of splats covered by Native4DBuffer */
	int32 Native4DSplatCount = 0;

	/** Animation period (TimeEnd - TimeStart) for the temporal SH bands */
	float TimeDuration4D = 0.f;

	/** Fracture bone binding: static per-splat bone index SRV (from shared render data) */
	FBufferRHIRef SplatBoneIndexBuffer;
	FShaderResourceViewRHIRef SplatBoneIndexBufferSRV;

	/** Whether this proxy's asset carries a fracture bone binding */
	bool bHasBoneBinding = false;

	/** Number of bones in the binding (0 when unbound) */
	int32 BoneCount = 0;

	/** Splats covered by SplatBoneIndexBuffer (0 when unbound) */
	int32 BoneBindingSplatCount = 0;

	/**
	 * Number of fracture cap splats appended after the LOD splats. They are
	 * part of SplatCount and occupy [SplatCount - CapSplatCount, SplatCount).
	 * 0 when the asset has no caps.
	 */
	int32 CapSplatCount = 0;

	/**
	 * Per-instance bone delta buffer (64 bytes/bone, dynamic). Kept per proxy
	 * rather than in the shared render data because two instances of the same
	 * asset can be fractured by different GeometryCollections.
	 */
	FBufferRHIRef BoneDeltaBuffer;
	FShaderResourceViewRHIRef BoneDeltaBufferSRV;

	/** Render-thread staging for per-frame bone delta uploads */
	TArray<FMatrix44f> BoneDeltaUploadData;

	/** Set when BoneDeltaUploadData differs from the GPU copy; cleared on upload */
	bool bBoneDeltasDirty = false;

	/**
	 * 1 while the bound fracture is actually displaced. Gates UseFractureBones so
	 * an un-fractured (or unbound) proxy keeps the untouched shader path and pays
	 * zero per-frame cost.
	 */
	bool bFractureBonesActive = false;

	/** Current 4D playback time (asset time units). Written by render commands from the owning component. */
	float CurrentTime = 0.f;

	/** Get a valid ColorTexture SRV (returns dummy if real one not available) */
	FShaderResourceViewRHIRef GetColorTextureSRVOrDummy() const
	{
		return ColorTextureSRV.IsValid() ? ColorTextureSRV : DummyWhiteTextureSRV;
	}

	/** Position format used by this asset (Float32, Norm16, etc.) */
	EGaussianPositionFormat PositionFormat = EGaussianPositionFormat::Float32;

	//----------------------------------------------------------------------
	// Cluster culling resources (Nanite-style optimization)
	//----------------------------------------------------------------------

	/** Cluster data buffer (static, loaded from asset) */
	FBufferRHIRef ClusterBuffer;
	FShaderResourceViewRHIRef ClusterBufferSRV;

	/** 4D temporal data buffer (static, from asset; dummy when not 4D) */
	FBufferRHIRef TemporalBuffer;
	FShaderResourceViewRHIRef TemporalBufferSRV;

	/** Visible cluster indices buffer (written by culling shader) */
	FBufferRHIRef VisibleClusterBuffer;
	FUnorderedAccessViewRHIRef VisibleClusterBufferUAV;
	FShaderResourceViewRHIRef VisibleClusterBufferSRV;

	/** Packed scalar counters written by the cluster culling shaders:
	 *  [0] visible cluster count, [1] LOD cluster count,
	 *  [2] LOD splat total, [3] LOD splat output count.
	 *  Kept in a single buffer because SM5 allows only 8 UAVs per shader. */
	FBufferRHIRef ClusterCountersBuffer;
	FUnorderedAccessViewRHIRef ClusterCountersBufferUAV;

	/** Number of clusters */
	int32 ClusterCount = 0;

	/** Number of leaf clusters (for rendering) */
	int32 LeafClusterCount = 0;

	/** Whether cluster culling is available */
	bool bHasClusterData = false;

	/** Whether Nanite is enabled for this asset (per-asset setting) */
	bool bEnableNanite = false;

	//----------------------------------------------------------------------
	// LOD splat resources (for parent cluster rendering)
	//----------------------------------------------------------------------

	/** LOD splat data buffer (static, loaded from asset) */
	FBufferRHIRef LODSplatBuffer;
	FShaderResourceViewRHIRef LODSplatBufferSRV;

	/** Number of LOD splats */
	int32 LODSplatCount = 0;

	/** Whether LOD splats are available */
	bool bHasLODSplats = false;

	//----------------------------------------------------------------------
	// Indirect draw resources (GPU-driven rendering)
	//----------------------------------------------------------------------

	/** Indirect draw argument buffer for GPU-driven rendering
	 * Structure: IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation, StartInstanceLocation
	 * Written by cluster culling, read by DrawIndexedPrimitiveIndirect
	 */
	FBufferRHIRef IndirectDrawArgsBuffer;
	FUnorderedAccessViewRHIRef IndirectDrawArgsBufferUAV;

	/** Whether indirect draw is available */
	bool bSupportsIndirectDraw = false;

	//----------------------------------------------------------------------
	// Cluster visibility integration resources
	//----------------------------------------------------------------------

	/** Maps each splat index to its cluster index
	 * Used by CalcViewData to check if splat's cluster is visible
	 */
	FBufferRHIRef SplatClusterIndexBuffer;
	FShaderResourceViewRHIRef SplatClusterIndexBufferSRV;

	/** Bitmap for cluster visibility (1 bit per cluster)
	 * Written by cluster culling, read by CalcViewData
	 * Size: ceil(ClusterCount / 32) uint32s
	 */
	FBufferRHIRef ClusterVisibilityBitmap;
	FUnorderedAccessViewRHIRef ClusterVisibilityBitmapUAV;
	FShaderResourceViewRHIRef ClusterVisibilityBitmapSRV;

	/** Selected cluster ID buffer for Nanite-style debug visualization
	 * Maps each leaf cluster to its selected cluster ID (may be parent if LOD is used)
	 * Written by cluster culling, read by CalcViewData
	 * Size: LeafClusterCount uint32s
	 */
	FBufferRHIRef SelectedClusterBuffer;
	FUnorderedAccessViewRHIRef SelectedClusterBufferUAV;
	FShaderResourceViewRHIRef SelectedClusterBufferSRV;

	//----------------------------------------------------------------------
	// LOD cluster selection output (for LOD rendering)
	//----------------------------------------------------------------------

	/** Buffer of unique parent cluster indices that need their LOD splats rendered
	 * Written by cluster culling, read by LOD rendering pass
	 * Size: ClusterCount uint32s (worst case all clusters could be selected)
	 */
	FBufferRHIRef LODClusterBuffer;
	FUnorderedAccessViewRHIRef LODClusterBufferUAV;
	FShaderResourceViewRHIRef LODClusterBufferSRV;

	/** Bitmap to track which parent clusters have been claimed (1 bit per cluster)
	 * Used during cluster culling to ensure each parent is only added once
	 * Also read by GPU-driven LOD shader to check if cluster is selected
	 */
	FBufferRHIRef LODClusterSelectedBitmap;
	FUnorderedAccessViewRHIRef LODClusterSelectedBitmapUAV;
	FShaderResourceViewRHIRef LODClusterSelectedBitmapSRV;

	//----------------------------------------------------------------------
	// GPU-driven LOD rendering resources
	//----------------------------------------------------------------------

	/** Maps each LOD splat index to its owning cluster ID
	 * Used by GPU-driven LOD shader to check if LOD splat's cluster is selected
	 * Size: TotalLODSplats * sizeof(uint32)
	 */
	FBufferRHIRef LODSplatClusterIndexBuffer;
	FShaderResourceViewRHIRef LODSplatClusterIndexBufferSRV;

	/** Total splat count for buffer allocation (SplatCount + TotalLODSplats) */
	int32 TotalSplatCount = 0;

	//----------------------------------------------------------------------
	// Splat compaction resources (GPU-driven work reduction)
	//----------------------------------------------------------------------

	/** Compacted list of visible splat indices
	 * Written by CompactSplats, read by CalcViewData (when using compaction)
	 * Size: TotalSplatCount * sizeof(uint32) (worst case all visible)
	 */
	FBufferRHIRef CompactedSplatIndicesBuffer;
	FUnorderedAccessViewRHIRef CompactedSplatIndicesBufferUAV;
	FShaderResourceViewRHIRef CompactedSplatIndicesBufferSRV;

	/** Atomic counter for visible splat count
	 * Written by CompactSplats, read by PrepareIndirectArgs
	 * Size: 4 bytes (single uint32)
	 */
	FBufferRHIRef VisibleSplatCountBuffer;
	FUnorderedAccessViewRHIRef VisibleSplatCountBufferUAV;
	FShaderResourceViewRHIRef VisibleSplatCountBufferSRV;

	/** Indirect dispatch arguments for compute shaders (CalcViewData, CalcDistances)
	 * Written by PrepareIndirectArgs, read by DispatchComputeIndirect
	 * Format: uint3 (numGroupsX, numGroupsY, numGroupsZ)
	 * Size: 12 bytes (3 uint32)
	 */
	FBufferRHIRef IndirectDispatchArgsBuffer;
	FUnorderedAccessViewRHIRef IndirectDispatchArgsBufferUAV;

	/** Whether splat compaction is enabled and buffers are available */
	bool bSupportsCompaction = false;

	/** Get position format as uint for shader */
	uint32 GetPositionFormatUint() const { return static_cast<uint32>(PositionFormat); }

	/** Get number of SH bands stored in SHBuffer (0 = no SH data) */
	int32 GetSHBands() const { return SHBands; }

	/** Asset name of the underlying shared render data (for diagnostics) */
	FString GetAssetName() const;

private:
	/** Create dummy white texture for fallback */
	void CreateDummyWhiteTexture(FRHICommandListBase& RHICmdList);

	/** Create per-instance cluster/compaction/sort buffers */
	void CreatePerInstanceBuffers(FRHICommandListBase& RHICmdList);

private:
	/** Shared render data (holds shared GPU buffers, ref-counted) */
	TSharedPtr<FGaussianSplatRenderData> SharedData;

	int32 SplatCount = 0;
	int32 SHBands = 0;  // Number of SH bands stored in SHBuffer (0 = no SH data)
	bool bInitialized = false;

public:
	/** Cached state for camera-static sort skipping */
	FMatrix CachedViewProjectionMatrix = FMatrix::Identity;
	FMatrix CachedLocalToWorld = FMatrix::Identity;
	float CachedOpacityScale = -1.0f;
	float CachedSplatScale = -1.0f;
	float CachedErrorThreshold = -1.0f;
	float CachedCurrentTime = -1.0f;
	int32 CachedDebugMode = -1;
	int32 CachedDebugForceLODLevel = -1;
	bool bHasCachedSortData = false;
};

/**
 * Scene proxy for rendering Gaussian Splatting
 */
class FGaussianSplatSceneProxy : public FPrimitiveSceneProxy
{
public:
	FGaussianSplatSceneProxy(const UGaussianSplatComponent* InComponent, const UBodySetup* InCollisionBodySetup);
	virtual ~FGaussianSplatSceneProxy();

	//~ Begin FPrimitiveSceneProxy Interface
	virtual SIZE_T GetTypeHash() const override;
	virtual uint32 GetMemoryFootprint() const override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;

	virtual void GetDynamicMeshElements(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override;

	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;
	virtual void DestroyRenderThreadResources() override;
#if WITH_EDITOR
	virtual HHitProxy* CreateHitProxies(UPrimitiveComponent* Component, TArray<TRefCountPtr<HHitProxy>>& OutHitProxies) override;
#endif
	//~ End FPrimitiveSceneProxy Interface

	/** Get GPU resources (may return nullptr if pending destruction) */
	FGaussianSplatGPUResources* GetGPUResources() const { return GPUResources; }

	/** Update the 4D playback time. Game-thread safe (enqueues a render command).
	 *  Render-thread FIFO ordering guarantees this runs before any subsequently
	 *  enqueued proxy destruction command. */
	void SetCurrentTime(float InTime)
	{
		FGaussianSplatGPUResources* Res = GPUResources;
		ENQUEUE_RENDER_COMMAND(GaussianSplatSetCurrentTime)(
			[Res, InTime](FRHICommandListImmediate&)
			{
				if (Res)
				{
					Res->CurrentTime = InTime;
				}
			});
	}

	/**
	 * Push this frame's fracture bone deltas (Delta_b = Cur_b * Rest_b^-1).
	 * Game-thread safe (enqueues a render command). Render-thread FIFO ordering
	 * guarantees this runs before any subsequently enqueued proxy destruction
	 * command. An empty array marks the fracture as at rest, which disables the
	 * bone path again.
	 */
	void SetBoneDeltas(const TArray<FMatrix44f>& InDeltas)
	{
		FGaussianSplatGPUResources* Res = GPUResources;
		if (!Res)
		{
			return;
		}

		ENQUEUE_RENDER_COMMAND(GaussianSplatSetBoneDeltas)(
			[Res, Deltas = InDeltas](FRHICommandListImmediate&)
			{
				if (!Res->bHasBoneBinding)
				{
					return;
				}

				if (Deltas.Num() != Res->BoneCount)
				{
					// At rest (or bounds changed): keep the GPU copy but stop using it
					Res->bFractureBonesActive = false;
					return;
				}

				Res->BoneDeltaUploadData = Deltas;
				Res->bBoneDeltasDirty = true;
				Res->bFractureBonesActive = true;
			});
	}

	/** Try to initialize color texture SRV if not already done */
	void TryInitializeColorTexture(FRHICommandListBase& RHICmdList);

	/** Get splat count */
	int32 GetSplatCount() const { return SplatCount; }

	/** Get rendering parameters */
	int32 GetSHOrder() const { return SHOrder; }
	float GetOpacityScale() const { return OpacityScale; }
	float GetSplatScale() const { return SplatScale; }
	float GetLODErrorThreshold() const { return LODErrorThreshold; }
	float GetMaxDrawDistance() const { return MaxDrawDistance; }
	float GetFadeOutStartDistance() const { return FadeOutStartDistance; }
	int32 GetSortEveryNthFrame() const { return SortEveryNthFrame; }

	/** Check if this proxy is safe to use for rendering.
	 *  Returns false if proxy is being destroyed or has invalid resources.
	 *  Thread-safe - can be called from render thread.
	 */
	bool IsValidForRendering() const
	{
		// Check destruction flag first (atomic, no lock needed)
		if (bPendingDestruction.load(std::memory_order_acquire))
		{
			return false;
		}
		// Then check resources
		return GPUResources != nullptr && GPUResources->IsValid();
	}

	/** Mark this proxy as pending destruction. Called at start of DestroyRenderThreadResources. */
	void MarkPendingDestruction()
	{
		bPendingDestruction.store(true, std::memory_order_release);
	}

private:
	/** Atomic flag indicating this proxy is being destroyed.
	 *  Set at the start of DestroyRenderThreadResources to prevent
	 *  render commands from accessing resources during/after destruction.
	 */
	std::atomic<bool> bPendingDestruction{false};


	/** GPU resources */
	FGaussianSplatGPUResources* GPUResources = nullptr;

	/** Cached asset for initialization - weak ptr to prevent dangling pointer
	 *  if asset is GC'd or replaced via SetSplatAsset while proxy exists.
	 *  Must call .Get() and check validity before each access on render thread. */
	TWeakObjectPtr<UGaussianSplatAsset> CachedAsset;

	/** Optional fracture asset (v11+). Its shared render data already folds in
	 *  the source splats plus the bone binding and cap splats, so when set it is
	 *  the data source instead of CachedAsset. Weak ptr for the same reason. */
	TWeakObjectPtr<UGaussianSplatFractureAsset> CachedFractureAsset;

	/** Rendering parameters */
	int32 SplatCount = 0;
	int32 SHOrder = 3;
	float OpacityScale = 1.0f;
	float SplatScale = 1.0f;
	float LODErrorThreshold = 0.03f;
	float MaxDrawDistance = 0.0f;
	float FadeOutStartDistance = 0.0f;
	int32 SortEveryNthFrame = 1;
	bool bEnableFrustumCulling = true;

	/** Shadow parameters */
	bool bCastShadow = false;
	// 已废弃：旧的包围盒阴影代理参数（ShadowProxyDetail / ShadowIntensity）
	// uint8 ShadowProxyDetail = 1;  // 0=Box, 1=Convex, 2=Full
	// float ShadowIntensity = 1.0f;

	// ------------------------------------------------------------------
	// Shadow geometry (source asset local space), copied from the component at
	// construction. Empty unless bCastShadow is on *and* the component could
	// produce geometry, in which case the proxy submits it to the shadow depth
	// pass and nowhere else. Two sources, resolved on the component:
	//   1. the fracture asset's voxel shell (tightest hull), or
	//   2. a box soup over the splat cloud's occupied voxels (bare splats).
	// ------------------------------------------------------------------

	/** Shadow geometry vertex positions */
	TArray<FVector3f> ShadowShellVertices;

	/** Shadow geometry triangles (3 indices per triangle) */
	TArray<int32> ShadowShellTriangles;

	// ------------------------------------------------------------------
	// Shadow shell GPU buffers, built once in CreateRenderThreadResources().
	//
	// They cannot be built per frame: the shadow depth pass gathers dynamic mesh
	// elements from a task graph worker thread (FGatherShadowPrimitivesPacket in
	// ShadowSetup.cpp), and FDynamicMeshBuilder::GetMeshElement() internally calls
	// FRHICommandListImmediate::Get(), whose check(IsInRenderingThread()) asserts
	// there. Uploading once on the render thread and only touching
	// Collector.GetRHICommandList() afterwards keeps the gather thread safe.
	// ------------------------------------------------------------------

	TUniquePtr<FStaticMeshVertexBuffers> ShadowMeshVertexBuffers;
	TUniquePtr<FDynamicMeshIndexBuffer32> ShadowMeshIndexBuffer;
	TUniquePtr<FLocalVertexFactory> ShadowMeshVertexFactory;
	bool bShadowMeshResourcesReady = false;

	/** Upload ShadowShellVertices / ShadowShellTriangles to the GPU (render thread). */
	void BuildShadowMeshResources(FRHICommandListBase& RHICmdList);

	/** Release the upload above (render thread, before destruction). */
	void ReleaseShadowMeshResources();

	/**
	 * Submit the shadow geometry to the shadow depth pass for one view.
	 *
	 * The batch is flagged so it is used *only* as a shadow caster: no material,
	 * no depth prepass, no occluder. Letting it into those passes would write the
	 * geometry's depth over the splats (whose PSO tests with CF_DepthNearOrEqual)
	 * and dim the model, which is exactly the bug this replaces.
	 */
	void AddShadowMeshBatch(int32 ViewIndex, FMeshElementCollector& Collector) const;

	// ------------------------------------------------------------------
	// Collision geometry (engine collision view modes only)
	//
	// The splats themselves are drawn by a post-opaque pass, which the engine's
	// collision view knows nothing about; and a custom proxy has to submit its
	// own collision geometry (the engine only does that for its own proxies).
	// So this proxy caches the component's BodySetup at construction and draws
	// AggGeom when the view family is in a collision view mode.
	//
	// The pointer is only ever dereferenced on the render thread while collision
	// is enabled; the component marks its render state dirty whenever the
	// collision method changes, so a proxy can never outlive the BodySetup it
	// was built with.
	// ------------------------------------------------------------------

	/** BodySetup the component was using when this proxy was built (may be null). */
	const UBodySetup* CollisionBodySetup = nullptr;

	/** Submit the aggregated collision shapes as flat-shaded debug geometry. */
	void AddCollisionMeshBatches(int32 ViewIndex, FMeshElementCollector& Collector) const;

#if WITH_EDITOR
	/** Cached hit proxy created in CreateHitProxies, used for editor viewport click selection. */
	HHitProxy* SelectionHitProxy = nullptr;
#endif
};
