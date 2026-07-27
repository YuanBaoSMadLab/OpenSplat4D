#pragma once

#include "CoreMinimal.h"
#include "Curves/RichCurve.h"
#include "GaussianSplatAsset.h"
#include "OpenSplat4DPoint.h"
#include "OpenSplat4DTypes.h"
#include "OpenSplat4DRuntimeModule.h"
#include "OpenSplat4DPointCloud.generated.h"

/**
 * A 3DGS / 4DGS point cloud asset.
 *
 * Inherits NanoGS UGaussianSplatAsset for GPU rendering compatibility,
 * adding 4D temporal fields, LOD controls, and the Points array for
 * CPU-side access / editing.
 *
 * Holds the raw gaussian primitives and interprets them either as a static
 * 3DGS cloud (Static3D) or as a time-varying 4DGS cloud (Dynamic4D). In 4DGS
 * mode the temporal fields of each FOpenSplat4DPoint drive the rendering over
 * the [TimeStart, TimeEnd] axis.
 */
UCLASS(Blueprintable, BlueprintType, EditInlineNew, CollapseCategories)
class OPENSPLAT4DRUNTIME_API UOpenSplat4DPointCloud : public UGaussianSplatAsset
{
	GENERATED_UCLASS_BODY()
public:
	/** Broadcast whenever the point set (or its interpretation) changes. */
	FSimpleMulticastDelegate OnPointsChanged;

	/** SourceFilePath inherited from UGaussianSplatAsset — no duplicate needed. */

	/** Rendering mode: static 3DGS vs dynamic 4DGS. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D")
	EOpenSplat4DMode Mode = EOpenSplat4DMode::Static3D;

	/** 4DGS time axis start (matches the trained model's time_duration[0]). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D")
	float TimeStart = -0.5f;

	/** 4DGS time axis end (matches the trained model's time_duration[1]). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D")
	float TimeEnd = 0.5f;

	/** LOD feature granularity used by CalcFeatureCurve. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D")
	int32 FeatureLevel = 64;

	/** Splat 尺寸缩放因子（渲染参数）。默认 1.0 为标准 3DGS 尺度。
	 *  调大 splat 更粗（适合整体过小/稀疏），调小更精细。
	 *  修改后资产编辑器预览和关卡Actor会自动重建。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D",
		meta = (ClampMin = "0.01", ClampMax = "50.0", UIMin = "0.01", UIMax = "10.0"))
	float SplatScale = 1.0f;

	/** 最小 splat 半径（世界单位）。小于此值的 splat 会被钳制到此大小以防止亚像素消失。*/
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D|LOD",
		meta = (ClampMin = "0.01", ClampMax = "10.0", UIMin = "0.01", UIMax = "5.0"))
	float MinSplatRadius = 0.5f;

	/** 距离相机多远（世界单位）开始减少粒子数量以优化性能。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D|LOD",
		meta = (ClampMin = "50.0", ClampMax = "20000.0", UIMin = "50.0", UIMax = "5000.0"))
	float LODDitherStartDistance = 500.0f;

	/** 粒子数量减少到最大（保留10%）的距离。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D|LOD",
		meta = (ClampMin = "500.0", ClampMax = "50000.0", UIMin = "500.0", UIMax = "10000.0"))
	float LODDitherEndDistance = 5000.0f;

	/** 远处 splat 最小屏幕占比（控制放大保可见尺寸的力度，0=不放大）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D|LOD",
		meta = (ClampMin = "0.0", ClampMax = "0.05", UIMin = "0.0", UIMax = "0.02"))
	float MinSplatScreenSize = 0.005f;

	/** Per-point size multiplier (default 1.0). Edited via SplatActor box selection tool. Serialized with asset. */
	UPROPERTY()
	TArray<float> PerPointSizeScale;

	/** Reset all per-point size scales to 1.0. */
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	void ResetAllPointSizeScales();

	// ---- point access -------------------------------------------------------
	// NOTE: This asset is a *pure preview* object. The on-disk dataset locations
	// (source images / sparse reconstruction / trained-model output) live on the
	// data asset (UOpenSplat4DCaptureSet), NOT here -- keeping this asset free of
	// dataset bookkeeping means it only has to worry about rendering the splats.
	void SetPoints(const TArray<FOpenSplat4DPoint>& InPoints, bool bReorder = true);
	const TArray<FOpenSplat4DPoint>& GetPoints() const { return Points; }
	int32 GetPointCount() const { return Points.Num(); }

	// ---- helpers ------------------------------------------------------------
	FRichCurve CalcFeatureCurve();
	FBox CalcBounds();

	// ---- (de)serialization to disk -----------------------------------------
	/** Normalize a 0..1 playback fraction into the cloud's time axis. */
	float FractionToTime(float Fraction) const;
	/** Normalize a time value into 0..1 across the cloud's time axis. */
	float TimeToFraction(float Time) const;

	// ---- (de)serialization to disk -----------------------------------------
	/**
	 * Load a point cloud from disk, dispatching on the file extension:
	 *   - .ply  : standard 3DGS (binary_little_endian) -> Static3D
	 *   - .spz  : Niantic SPZ (4D-aware, see Compression/Spz.h) -> keeps its Mode
	 *   - .4dgs : OpenSplat4D native binary -> keeps its Mode / time axis
	 * [ENHANCEMENT] The reference only supported .ply via LoadFromFile; OpenSplat4D
	 * additionally understands .spz and .4dgs from a single entry point.
	 */
	void LoadFromFile(FString InFilePath);
	static TArray<FOpenSplat4DPoint> LoadPointsFromPLY(FString InFilePath);

	/** Load a Niantic SPZ file (4D-aware). Returns an empty array on failure. */
	static TArray<FOpenSplat4DPoint> LoadPointsFromSPZ(FString InFilePath);

	/** Load the plugin's native .4dgs binary (3D or 4D). */
	bool LoadFrom4DGS(FString InFilePath);
	bool SaveTo4DGS(FString InFilePath);

	/** Save to a Niantic SPZ file (4D-aware). Returns false on failure. */
	bool SaveToSPZ(FString InFilePath);

	// ---- 4D inference (CPU reference path) ----------------------------------
	/**
	 * Evaluate the cloud at a given time, returning the effective (time-weighted)
	 * gaussians. Points whose temporal weight falls below InMinWeight are dropped.
	 * This is the CPU reference implementation of the GPU path used by the
	 * Niagara data interface; useful for static-mesh export and LOD snapshots.
	 */
	void SampleAtTime(float Time, TArray<FOpenSplat4DPoint>& OutPoints, float InMinWeight = 1e-3f) const;

	/** Temporal marginal weight of a single point at the given time (4DGS math). */
	static float GetTimeWeight(float AnchorTime, float TimeVariance, float Time);

	EOpenSplat4DCompressionMethod GetCompressionMethod() const { return CompressionMethod; }
	void SetCompressionMethod(EOpenSplat4DCompressionMethod Val) { CompressionMethod = Val; }

	/** Auto-reload from the source file when the deserialised cloud is empty, so a
	 *  stale / failed-import asset still previews its splats. Covers editor-open,
	 *  drag-into-level, actor-spawn and cook load paths (unlike the editor-only
	 *  InitEditor reload). */
	virtual void PostLoad() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/** When an asset loads with 0 points and no recorded SourceFilePath, look for
	 *  a co-located <AssetName>.ply / .4dgs / .spz beside the .uasset and load it.
	 *  Returns true if a sibling source was found and parsed successfully. */
	bool TryLoadSiblingSource();

	void Serialize(FArchive& Ar) override;

	/** Serialize SHRest arrays with a version guard (backward-compat with old assets). */
	void SerializeSHRest(FArchive& Ar);

	// [Robustness] Default is None (raw, lossless TArray<FOpenSplat4DPoint> bytes).
	// This guarantees a freshly imported / re-imported cloud round-trips with every
	// point intact -- the previous Spz default could, under certain point counts,
	// serialise an empty/short blob and reopen as a 0-point asset ("nothing displays").
	// Spz / Zlib remain selectable for size-sensitive shipping builds.
	UPROPERTY(EditAnywhere, Category = "OpenSplat4D")
	EOpenSplat4DCompressionMethod CompressionMethod = EOpenSplat4DCompressionMethod::None;

	UPROPERTY(Transient)
	TArray<FOpenSplat4DPoint> Points;
};
