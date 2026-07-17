#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Curves/RichCurve.h"
#include "OpenSplat4DPoint.h"
#include "OpenSplat4DPointCloud.generated.h"

OPENSPLAT4DRUNTIME_API DECLARE_LOG_CATEGORY_EXTERN(LogOpenSplat4D, Log, All);

class UOpenSplat4DPointCloud;

/**
 * A 3DGS / 4DGS point cloud asset.
 *
 * Holds the raw gaussian primitives and interprets them either as a static
 * 3DGS cloud (Static3D) or as a time-varying 4DGS cloud (Dynamic4D). In 4DGS
 * mode the temporal fields of each FOpenSplat4DPoint drive the rendering over
 * the [TimeStart, TimeEnd] axis (see UNiagaraDataInterfaceOpenSplat4D).
 */
UCLASS(Blueprintable, BlueprintType, EditInlineNew, CollapseCategories)
class OPENSPLAT4DRUNTIME_API UOpenSplat4DPointCloud : public UObject
{
	GENERATED_UCLASS_BODY()
public:
	/** Broadcast whenever the point set (or its interpretation) changes. */
	FSimpleMulticastDelegate OnPointsChanged;

	/** Absolute path of the source file this asset was imported from. Drives reimport. */
	UPROPERTY()
	FString SourceFilePath;

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

private:
	/** When an asset loads with 0 points and no recorded SourceFilePath, look for
	 *  a co-located <AssetName>.ply / .4dgs / .spz beside the .uasset and load it.
	 *  Returns true if a sibling source was found and parsed successfully. */
	bool TryLoadSiblingSource();

	void Serialize(FArchive& Ar) override;

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
