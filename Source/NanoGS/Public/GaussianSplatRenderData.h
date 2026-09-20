// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianDataTypes.h"
#include "GaussianClusterTypes.h"
#include "RHI.h"
#include "RHIResources.h"

class UGaussianSplatAsset;

/**
 * Shared render data for a Gaussian Splat asset.
 * Holds CPU-side cached data and shared GPU buffers that are created once
 * per asset and shared across all FGaussianSplatGPUResources instances
 * referencing the same asset.
 */
class FGaussianSplatRenderData
{
public:
	FGaussianSplatRenderData();
	~FGaussianSplatRenderData();

	/** Initialize CPU-side data from asset. Only runs once (guarded by bIsInitialized). */
	void Initialize(UGaussianSplatAsset* Asset);

	/** Create shared GPU buffers. Thread-safe, only runs once. Must be called on render thread. */
	void CreateGPUBuffers(FRHICommandListBase& RHICmdList);

	/** Release shared GPU buffers. */
	void ReleaseGPUBuffers();

	/** Whether CPU-side data has been initialized */
	bool IsInitialized() const { return bIsInitialized; }

	/** Whether GPU buffers have been created */
	bool AreGPUBuffersCreated() const { return bGPUBuffersCreated; }

	/** Get asset name for logging */
	const FString& GetAssetName() const { return AssetName; }

public:
	// ---- Shared GPU buffers (created once, shared across all proxies) ----

	/** Packed splat data buffer (16 bytes/splat) */
	FBufferRHIRef PackedSplatBuffer;
	FShaderResourceViewRHIRef PackedSplatBufferSRV;

	/** Spherical harmonics buffer */
	FBufferRHIRef SHBuffer;
	FShaderResourceViewRHIRef SHBufferSRV;

	/** Chunk info buffer */
	FBufferRHIRef ChunkBuffer;
	FShaderResourceViewRHIRef ChunkBufferSRV;

	/** Index buffer for quad rendering */
	FBufferRHIRef IndexBuffer;

	/** Cluster data buffer (static, loaded from asset) */
	FBufferRHIRef ClusterBuffer;
	FShaderResourceViewRHIRef ClusterBufferSRV;

	/** 4D temporal data buffer (16 bytes/splat; dummy 16B when asset is not 4D) */
	FBufferRHIRef TemporalBuffer;
	FShaderResourceViewRHIRef TemporalBufferSRV;

	/** Keyframe 4D data buffer (64 bytes/splat/frame; dummy 64B when asset is not keyframe 4D) */
	FBufferRHIRef KeyframeBuffer;
	FShaderResourceViewRHIRef KeyframeBufferSRV;

	/** Native 4D (fudan) data buffer (80 bytes/splat; dummy 80B when asset is not native 4D) */
	FBufferRHIRef Native4DBuffer;
	FShaderResourceViewRHIRef Native4DBufferSRV;

	/** Whether the asset has 4D temporal data */
	bool bIs4D = false;

	/** Whether the asset uses keyframe 4D playback (mutually exclusive with temporal marginalization at bind time) */
	bool bIsKeyframe4D = false;

	/** Whether the asset uses native (fudan) 4D rendering */
	bool bIsNative4D = false;

	/** Number of keyframes stored in KeyframeBuffer */
	int32 KeyframeFrameCount = 0;

	/** Splats per frame covered by KeyframeBuffer (M at import; independent of later LOD-grown SplatCount) */
	int32 KeyframeSplatCount = 0;

	/** Number of splats covered by Native4DBuffer (independent of later LOD-grown SplatCount) */
	int32 Native4DSplatCount = 0;

	/** Time duration (TimeEnd - TimeStart); the eval_shfs_4d 'l' parameter */
	float TimeDuration4D = 0.f;

	/** Splat-to-cluster index buffer (static, loaded from asset) */
	FBufferRHIRef SplatClusterIndexBuffer;
	FShaderResourceViewRHIRef SplatClusterIndexBufferSRV;

	// ---- Metadata ----

	int32 SplatCount = 0;
	int32 SHBands = 0;
	int32 ClusterCount = 0;
	int32 LeafClusterCount = 0;
	int32 LODSplatCount = 0;
	bool bEnableNanite = false;
	bool bHasClusterData = false;
	bool bHasLODSplats = false;
	EGaussianPositionFormat PositionFormat = EGaussianPositionFormat::Float32;

private:
	// ---- CPU-side cached data (freed after GPU upload) ----

	TArray<uint8> PackedSplatData;
	TArray<uint8> SHData;
	TArray<uint8> TemporalData;
	TArray<uint8> KeyframeData;
	TArray<uint8> Native4DData;
	TArray<FGaussianChunkInfo> CachedChunkData;
	TArray<FGaussianGPUCluster> CachedClusterData;
	TArray<uint32> CachedSplatClusterIndices;

	bool bIsInitialized = false;
	bool bGPUBuffersCreated = false;
	FString AssetName;
	FCriticalSection InitLock;
	FCriticalSection GPUInitLock;
};
