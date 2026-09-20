// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "Serialization/BulkData.h"
#include "GaussianDataTypes.h"
#include "GaussianClusterTypes.h"
#include "GaussianSplatAsset.generated.h"

// Forward declaration
class UGaussianSplatAsset;
class FGaussianSplatRenderData;
class UTexture2D;

// Delegate fired when asset data changes (e.g., Nanite enabled/disabled)
DECLARE_MULTICAST_DELEGATE_OneParam(FOnGaussianSplatAssetChanged, UGaussianSplatAsset*);

// Serialization magic/version for format identification
#define GAUSSIAN_SPLAT_ASSET_MAGIC   0x47535056  // "GSPV"
// v7: keyframe 4D data (bIsKeyframe4D/KeyframeCount/KeyframeBulkData).
// v8: fudan 4DGS native 4D data (bIsNative4D/Native4DBulkData). v5-v7 assets load unchanged.
#define GAUSSIAN_SPLAT_ASSET_VERSION 8

/**
 * Asset containing Gaussian Splatting data loaded from PLY files
 * Stores compressed splat data optimized for GPU rendering
 */
UCLASS(BlueprintType, hidecategories = Object)
class NANOGS_API UGaussianSplatAsset : public UObject
{
	GENERATED_BODY()

public:
	UGaussianSplatAsset();

	//~ Begin UObject Interface
	virtual void Serialize(FArchive& Ar) override;
	virtual void PostLoad() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	//~ End UObject Interface

	/** Get the number of splats in this asset */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅")
	int32 GetSplatCount() const { return SplatCount; }

	/** Get the bounding box of all splats */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅")
	FBox GetBounds() const { return BoundingBox; }

	/** Get estimated memory usage in bytes */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅")
	int64 GetMemoryUsage() const;

	/** Check if asset has valid data */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅")
	bool IsValid() const { return SplatCount > 0 && PositionBulkData.GetBulkDataSize() > 0; }

	/** Check if cluster hierarchy is available */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|聚类")
	bool HasClusterHierarchy() const { return ClusterHierarchy.IsValid(); }

	/** Check if Nanite is enabled for this asset */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|Nanite")
	bool IsNaniteEnabled() const { return bEnableNanite; }

#if WITH_EDITOR
	/**
	 * Generate a thumbnail image from raw splat data and store it in ThumbnailTexture.
	 * Called automatically at the end of InitializeFromSplatData().
	 */
	void GenerateThumbnail(const TArray<struct FGaussianSplatData>& InSplats);

	/**
	 * Build Nanite cluster hierarchy from source PLY file
	 * This re-reads the source file and builds cluster data
	 * @return True if successful, false if source file not found or build failed
	 */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|Nanite")
	bool BuildNaniteClusterHierarchy();

	/**
	 * Clear Nanite cluster hierarchy to reduce asset size
	 * Removes cluster data and LOD splats
	 */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|Nanite")
	void ClearNaniteClusterHierarchy();

	/**
	 * Set Nanite enabled state (internal use)
	 */
	void SetNaniteEnabled(bool bEnable) { bEnableNanite = bEnable; }
#endif

	/** Get number of clusters in hierarchy */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|聚类")
	int32 GetClusterCount() const { return ClusterHierarchy.Clusters.Num(); }

	/** Get number of LOD levels */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|聚类")
	int32 GetNumLODLevels() const { return ClusterHierarchy.NumLODLevels; }

	/** Get original splat count (excluding LOD splats) */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|Nanite")
	int32 GetOriginalSplatCount() const { return OriginalSplatCount > 0 ? OriginalSplatCount : SplatCount; }

	/** Get the cluster hierarchy (const reference) */
	const FGaussianClusterHierarchy& GetClusterHierarchy() const { return ClusterHierarchy; }

	/** Delegate fired when asset data changes (Nanite enabled/disabled, reimport, etc.) */
	FOnGaussianSplatAssetChanged OnAssetChanged;

	/** Get or create shared render data for this asset.
	 *  Lazy-initialized on first call; subsequent calls return the existing data.
	 *  Shared across all scene proxies referencing this asset.
	 */
	TSharedPtr<FGaussianSplatRenderData> GetOrCreateRenderData();

public:
	/** Total number of splats */
	UPROPERTY(VisibleAnywhere, Category = "信息", meta = (DisplayName = "Splat 数量"))
	int32 SplatCount = 0;

	/** World-space bounding box of all splats */
	UPROPERTY(VisibleAnywhere, Category = "信息", meta = (DisplayName = "包围盒"))
	FBox BoundingBox;

	/** Position compression format */
	UPROPERTY(VisibleAnywhere, Category = "格式", meta = (DisplayName = "位置格式"))
	EGaussianPositionFormat PositionFormat = EGaussianPositionFormat::Float32;

	/** Color compression format */
	UPROPERTY(VisibleAnywhere, Category = "格式", meta = (DisplayName = "颜色格式"))
	EGaussianColorFormat ColorFormat = EGaussianColorFormat::Float16x4;

	/** Spherical harmonics compression format */
	UPROPERTY(VisibleAnywhere, Category = "格式", meta = (DisplayName = "SH 格式"))
	EGaussianSHFormat SHFormat = EGaussianSHFormat::Float16;

	/** Number of SH bands stored (0-3) */
	UPROPERTY(VisibleAnywhere, Category = "格式", meta = (DisplayName = "SH 波段数"))
	int32 SHBands = 3;

	/** Compressed position data (stored as bulk data for fast loading) */
	FByteBulkData PositionBulkData;

	/** Compressed rotation + scale data (stored as bulk data for fast loading) */
	FByteBulkData OtherBulkData;

	/** Compressed spherical harmonics data (stored as bulk data for fast loading) */
	FByteBulkData SHBulkData;

	/** Chunk quantization info (one per 256 splats) - kept as TArray since it's small */
	UPROPERTY()
	TArray<FGaussianChunkInfo> ChunkData;

	/** Color texture (Morton-swizzled, 2048 x N) - created at runtime from ColorTextureBulkData */
	UPROPERTY(Transient)
	UTexture2D* ColorTexture;

	/** Raw color texture pixel data (stored as bulk data for fast loading) */
	FByteBulkData ColorTextureBulkData;

	// ------------------------------------------------------------------
	// 4D temporal data (only populated when bIs4D is true)
	// 16 bytes per splat: [AnchorTime f32 | TimeSigma f32 | VelocityXY half2 |
	// VelocityZ half + hasVelocity flag half]
	// Covers ALL splats (original + LOD). LOD splats get default
	// (AnchorTime=0, Sigma=1e10) => always visible.
	// ------------------------------------------------------------------

	/** Whether this asset has 4D (spacetime) temporal data */
	UPROPERTY(VisibleAnywhere, Category = "4D", meta = (DisplayName = "4D 时序数据"))
	bool bIs4D = false;

	/** 4D time axis start (PLY time units, e.g. normalized [-1,1]) */
	UPROPERTY(VisibleAnywhere, Category = "4D", meta = (DisplayName = "时间轴起点"))
	float TimeStart = 0.f;

	/** 4D time axis end (PLY time units) */
	UPROPERTY(VisibleAnywhere, Category = "4D", meta = (DisplayName = "时间轴终点"))
	float TimeEnd = 0.f;

	/** Per-splat temporal data (16 bytes/splat, only when bIs4D) */
	FByteBulkData TemporalBulkData;

	/** Temporal record stride in bytes */
	static constexpr int32 TemporalStride = 16;

	/** Whether this asset has 4D data in any of the three modes */
	UFUNCTION(BlueprintCallable, Category = "4D")
	bool Is4D() const { return bIs4D || bIsKeyframe4D || bIsNative4D; }

	/** Lock temporal bulk data in place. Returns nullptr if empty. */
	const void* LockTemporalDataReadOnly(int64* OutSize = nullptr) const;
	void UnlockTemporalData() const;

	// ------------------------------------------------------------------
	// Fudan 4DGS native-4D data (third 4D mode, mutually exclusive with the
	// temporal marginalization and keyframe modes above; guarded by
	// bIsNative4D). 80 bytes per splat:
	//   w0-2  mu.xyz f32 (UE cm, same conversion as PositionBulkData)
	//   w3    mu.t   f32 (PLY time units)
	//   w4-6  s.xyz  f32 (linear, PLY meters -- the shader conjugates the
	//              covariance built in PLY space into UE local space)
	//   w7    s.t    f32 (linear sigma_t = exp(scale_3), time units)
	//   w8-11 q_l    f32 x4 (a,b,c,d)
	//   w12-15 q_r   f32 x4 (p,q,r,s)
	//   w16   opacity f32 (linear, post-sigmoid)
	//   w17   prefilter variance f32 (added to sigma_t^2)
	//   w18-19 pad
	// SH (4D spherical-cylindrical harmonics) lives in SHBulkData with
	// SHBands reinterpreted as the sh_channels_4d index (C = [1,6,16,33]).
	// ------------------------------------------------------------------

	/** Whether this asset uses fudan-zvg 4DGS native 4D rendering */
	UPROPERTY(VisibleAnywhere, Category = "4D", meta = (DisplayName = "Native 4D 数据（复旦）"))
	bool bIsNative4D = false;

	/** Per-splat native 4D data (80 bytes/splat, only when bIsNative4D) */
	FByteBulkData Native4DBulkData;

	/** Native 4D record stride in bytes */
	static constexpr int32 Native4DStride = 80;

	/** Whether this asset uses fudan 4DGS native 4D rendering */
	UFUNCTION(BlueprintCallable, Category = "4D")
	bool IsNative4D() const { return bIsNative4D; }

	/** Lock native 4D bulk data in place. Returns nullptr if empty. */
	const void* LockNative4DDataReadOnly(int64* OutSize = nullptr) const;
	void UnlockNative4DData() const;

	// ------------------------------------------------------------------
	// Keyframe 4D data (second 4D mode, mutually exclusive with the
	// t/scale_t temporal marginalization above; guarded by bIsKeyframe4D).
	// Per-frame PLY sequence: each frame stores every splat's full state in
	// 64 bytes: Position 3xf32 (12B) | Rotation 4xf32 XYZW (16B) | Scale
	// 3xf32 (12B) | Opacity f32 (4B) | Color RGB 3xf32 (12B) | reserved 8B.
	// Record offset: base = (frame * SplatCount + splatIndex) * 64.
	// TimeStart=0, TimeEnd=KeyframeCount-1 => CurrentTime is the frame
	// index as a float (play rate = frames per second).
	// ------------------------------------------------------------------

	/** Whether this asset uses keyframe 4D playback (per-frame PLY sequence) */
	UPROPERTY(VisibleAnywhere, Category = "4D", meta = (DisplayName = "关键帧 4D 数据"))
	bool bIsKeyframe4D = false;

	/** Number of keyframes stored (N). TimeEnd = KeyframeCount - 1. */
	UPROPERTY(VisibleAnywhere, Category = "4D", meta = (DisplayName = "关键帧数"))
	int32 KeyframeCount = 0;

	/** Per-frame splat state (KeyframeCount * SplatCount * KeyframeStride bytes, only when bIsKeyframe4D) */
	FByteBulkData KeyframeBulkData;

	/** Keyframe record stride in bytes */
	static constexpr int32 KeyframeStride = 64;

	/** Whether this asset uses keyframe 4D playback */
	UFUNCTION(BlueprintCallable, Category = "4D")
	bool IsKeyframe4D() const { return bIsKeyframe4D; }

	/** Lock keyframe bulk data in place. Returns nullptr if empty. */
	const void* LockKeyframeDataReadOnly(int64* OutSize = nullptr) const;
	void UnlockKeyframeData() const;

	/**
	 * Save this asset to the dedicated .o4d v2 container file
	 * (magic 'O4D2' + header + splat records + optional keyframe block).
	 * @param FilePath Target file path (typically with .o4d extension)
	 * @return True on success
	 */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅")
	bool SaveToO4DFile(FString FilePath);

	/**
	 * Load an asset from a .o4d v2 container file created by SaveToO4DFile
	 * (or produced alongside the keyframe 4D importer).
	 * @param FilePath Source .o4d file path
	 * @param Outer Outer for the new asset (defaults to the transient package)
	 * @return The loaded asset, or nullptr on failure
	 */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅")
	static UGaussianSplatAsset* LoadFromO4DFile(FString FilePath, UObject* Outer);

	/**
	 * Decompress the stored bulk data back into raw splat records
	 * (positions float32, rotation/scale float32, opacity + SH DC from the
	 * color texture, higher-order SH from the SH buffer).
	 * Used by SaveToO4DFile; not every field is bit-exact (float16 colors/SH).
	 */
	bool DecompressToSplatData(TArray<FGaussianSplatData>& OutSplats) const;

	/** Color texture width */
	UPROPERTY()
	int32 ColorTextureWidth = 0;

	/** Color texture height */
	UPROPERTY()
	int32 ColorTextureHeight = 0;

	/** Source file path (for reimport) */
	UPROPERTY(VisibleAnywhere, Category = "导入", meta = (DisplayName = "源文件路径"))
	FString SourceFilePath;

	/** Quality level used during import */
	UPROPERTY(VisibleAnywhere, Category = "导入", meta = (DisplayName = "导入质量"))
	EGaussianQualityLevel ImportQuality = EGaussianQualityLevel::Medium;

	/**
	 * Whether Nanite-style LOD and culling is enabled for this asset.
	 * Toggleable at runtime — disabling it makes the asset render at full
	 * resolution (all splats, no LOD compaction). Useful for quality
	 * comparison or when the cluster hierarchy is corrupted.
	 *
	 * When you change this in the editor, the owning SceneProxy will pick
	 * up the new value on next re-create (e.g. when the actor is moved
	 * or the level is saved/loaded). To force an immediate update in the
	 * viewport, close and re-open the asset editor.
	 */
	UPROPERTY(EditAnywhere, Category = "Nanite", meta = (DisplayName = "启用 Nanite"))
	bool bEnableNanite = false;

	// ============================================================================
	// 预览设置（在 OpenSplat 资产编辑器中可调，仅影响预览渲染，不修改资产数据）
	// ============================================================================
	// 这些字段让用户在 Asset Editor 中调整渲染参数，立即看到效果。
	// 它们不参与序列化的资产数据，仅作为编辑器预览状态存在。
	// ============================================================================
	UPROPERTY(EditAnywhere, Category = "预览设置", meta = (DisplayName = "预览 Splat 缩放"))
	float PreviewSplatScale = 1.0f;

	UPROPERTY(EditAnywhere, Category = "预览设置", meta = (DisplayName = "预览不透明度系数", ClampMin = "0.0", ClampMax = "2.0"))
	float PreviewOpacityScale = 1.0f;

	UPROPERTY(EditAnywhere, Category = "预览设置", meta = (DisplayName = "预览 SH 阶数", ClampMin = "0", ClampMax = "3", UIMin = "0", UIMax = "3"))
	int32 PreviewSHOrder = 3;

	UPROPERTY(EditAnywhere, Category = "预览设置", meta = (DisplayName = "预览 LOD 误差阈值", ClampMin = "0.0"))
	float PreviewLODErrorThreshold = 0.1f;

	/** 预览 Nanite 精度（LOD 误差阈值，与关卡组件同名属性同义）。调大 = 更快。 */
	UPROPERTY(EditAnywhere, Category = "预览设置", meta = (DisplayName = "预览 Nanite 精度（LOD 误差阈值）", ClampMin = "0.001", ClampMax = "1.0"))
	float PreviewNanitePrecision = 0.03f;

	/** 预览最大可视距离（cm）。0 = 不限制。远处 splat 超过该距离被剔除。 */
	UPROPERTY(EditAnywhere, Category = "预览设置", meta = (DisplayName = "预览最大可视距离", ClampMin = "0.0", UIMin = "0.0", UIMax = "100000.0"))
	float PreviewMaxDrawDistance = 0.0f;

	/** 预览淡出起始距离（cm）。0 = 到达最大可视距离时硬切。 */
	UPROPERTY(EditAnywhere, Category = "预览设置", meta = (DisplayName = "预览淡出起始距离", ClampMin = "0.0", UIMin = "0.0", UIMax = "100000.0", EditCondition = "PreviewMaxDrawDistance > 0"))
	float PreviewFadeOutStartDistance = 0.0f;

	/** 预览视口自动播放 4D 时序（仅含时间数据的 4D 资产生效）。 */
	UPROPERTY(EditAnywhere, Category = "预览设置", meta = (DisplayName = "预览 4D 自动播放"))
	bool PreviewAutoPlay = true;

	/** 预览 4D 播放速度（1 = 原速）。 */
	UPROPERTY(EditAnywhere, Category = "预览设置", meta = (DisplayName = "预览 4D 播放速度", ClampMin = "0.0", ClampMax = "10.0", UIMin = "0.0", UIMax = "5.0", EditCondition = "PreviewAutoPlay"))
	float PreviewPlayRate = 1.0f;

	UPROPERTY(EditAnywhere, Category = "预览设置", meta = (DisplayName = "显示包围盒"))
	bool bPreviewShowBounds = false;

	UPROPERTY(EditAnywhere, Category = "预览设置", meta = (DisplayName = "显示簇边界（调试）"))
	bool bPreviewShowClusterBounds = false;

	/**
	 * Hierarchical cluster structure for Nanite-style LOD and culling
	 * Only populated when bEnableNanite is true
	 */
	UPROPERTY()
	FGaussianClusterHierarchy ClusterHierarchy;

	/**
	 * Number of original splats (before LOD splats are appended)
	 * When Nanite is enabled, SplatCount includes both original + LOD splats
	 */
	UPROPERTY()
	int32 OriginalSplatCount = 0;

#if WITH_EDITORONLY_DATA
	/**
	 * Thumbnail image generated at import time.
	 * Stored persistently in the asset package so the Content Browser can display it.
	 * NOTE: This is Transient - recreated from ThumbnailData in PostLoad()
	 */
	UPROPERTY(Transient)
	UTexture2D* ThumbnailTexture;

	/**
	 * Raw thumbnail pixel data (BGRA, 256x256).
	 * Stored persistently and used to recreate ThumbnailTexture on load.
	 */
	UPROPERTY()
	TArray<uint8> ThumbnailData;

	/** Size of the thumbnail (width = height) */
	UPROPERTY()
	int32 ThumbnailSize = 0;

private:
	/** Recreate ThumbnailTexture from stored ThumbnailData (called in PostLoad) */
	void CreateThumbnailTextureFromData();
#endif

public:
	/**
	 * Initialize asset from raw splat data
	 * @param InSplats Raw splat data from PLY file
	 * @param InQuality Compression quality level
	 */
	void InitializeFromSplatData(const TArray<FGaussianSplatData>& InSplats, EGaussianQualityLevel InQuality);

	/**
	 * Decompress and return all splat positions (for debugging)
	 * @return Array of world-space positions
	 */
	UFUNCTION(BlueprintCallable, Category = "高斯泼溅|调试")
	TArray<FVector> GetDecompressedPositions() const;

	/** Get bytes per splat for position data based on format */
	static int32 GetPositionBytesPerSplat(EGaussianPositionFormat Format);

	/** Get bytes per splat for color data based on format */
	static int32 GetColorBytesPerSplat(EGaussianColorFormat Format);

	/** Get bytes per splat for SH data based on format */
	static int32 GetSHBytesPerSplat(EGaussianSHFormat Format, int32 Bands);

	/**
	 * Copy position bulk data to a TArray (for GPU upload)
	 * @param OutData Array to copy data into
	 */
	void GetPositionData(TArray<uint8>& OutData) const;

	/**
	 * Copy rotation/scale bulk data to a TArray (for GPU upload)
	 * @param OutData Array to copy data into
	 */
	void GetOtherData(TArray<uint8>& OutData) const;

	/**
	 * Copy SH bulk data to a TArray (for GPU upload)
	 * @param OutData Array to copy data into
	 */
	void GetSHData(TArray<uint8>& OutData) const;

	/**
	 * Copy color texture bulk data to a TArray
	 * @param OutData Array to copy data into
	 */
	void GetColorTextureData(TArray<uint8>& OutData) const;

	/** Get size of position bulk data in bytes */
	int64 GetPositionDataSize() const { return PositionBulkData.GetBulkDataSize(); }

	/** Get size of other bulk data in bytes */
	int64 GetOtherDataSize() const { return OtherBulkData.GetBulkDataSize(); }

	/** Get size of SH bulk data in bytes */
	int64 GetSHDataSize() const { return SHBulkData.GetBulkDataSize(); }

	/** Get size of color texture bulk data in bytes */
	int64 GetColorTextureDataSize() const { return ColorTextureBulkData.GetBulkDataSize(); }

	// ---------------------------------------------------------------
	// Zero-copy read-only accessors (performance: avoid full TArray
	// copies of potentially hundreds of MB when packing render data).
	// Caller MUST call the matching Unlock while holding the pointer,
	// and must not dereference it afterwards.
	// ---------------------------------------------------------------

	/** Lock position bulk data in place. Returns nullptr if empty. */
	const void* LockPositionDataReadOnly(int64* OutSize = nullptr) const;
	/** Lock rotation/scale bulk data in place. Returns nullptr if empty. */
	const void* LockOtherDataReadOnly(int64* OutSize = nullptr) const;
	/** Lock SH bulk data in place. Returns nullptr if empty. */
	const void* LockSHDataReadOnly(int64* OutSize = nullptr) const;
	/** Lock color texture bulk data in place. Returns nullptr if empty. */
	const void* LockColorTextureDataReadOnly(int64* OutSize = nullptr) const;

	void UnlockPositionData() const;
	void UnlockOtherData() const;
	void UnlockSHData() const;
	void UnlockColorTextureData() const;

	/** Shared render data (lazy-initialized, shared across all proxies) */
	TSharedPtr<FGaussianSplatRenderData> RenderData;

private:
	/** Compress and store position data */
	void CompressPositions(const TArray<FGaussianSplatData>& InSplats);

	/** Compress and store rotation/scale data */
	void CompressRotationScale(const TArray<FGaussianSplatData>& InSplats);

	/** Create color texture data with Morton swizzling (stores raw data for serialization) */
	void CreateColorTextureData(const TArray<FGaussianSplatData>& InSplats);

	/** Create UTexture2D from stored ColorTextureData (called after load or import) */
	void CreateColorTextureFromData();

	/** Compress and store SH data */
	void CompressSH(const TArray<FGaussianSplatData>& InSplats);

	/** Calculate bounding box from splat data */
	void CalculateBounds(const TArray<FGaussianSplatData>& InSplats);

	/** Calculate chunk quantization bounds */
	void CalculateChunkBounds(const TArray<FGaussianSplatData>& InSplats);
};
