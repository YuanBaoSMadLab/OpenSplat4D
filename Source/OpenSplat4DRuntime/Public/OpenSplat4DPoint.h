#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "OpenSplat4DPoint.generated.h"

/** How the point cloud is interpreted by the renderer. */
UENUM(BlueprintType)
enum class EOpenSplat4DMode : uint8
{
	/** Static 3D Gaussian Splatting (compatible with the base 3DGS pipeline). */
	Static3D UMETA(DisplayName = "3DGS (Static)"),
	/** Time-varying 4D Gaussian Splatting; points appear / move over a time axis. */
	Dynamic4D UMETA(DisplayName = "4DGS (Dynamic)"),
};

/** Disk compression applied when the point cloud asset is serialized. */
UENUM(BlueprintType)
enum class EOpenSplat4DCompressionMethod : uint8
{
	None UMETA(DisplayName = "无"),
	Zlib UMETA(DisplayName = "Zlib (raw blob)"),
	// [ENHANCEMENT] Niantic "spz" format: 64-byte-per-gaussian fixed-point
	// quantization + gzip. Far smaller than the raw Zlib blob and (uniquely for
	// OpenSplat4D) carries the 4DGS temporal fields through an appended O4D4 block.
	// This is the preferred method and is strictly richer than the reference
	// GaussianSplattingRuntime (which only had this 3D-only SPZ path).
	Spz UMETA(DisplayName = "SPZ (Niantic, 4D-aware)"),
};

/**
 * A single Gaussian primitive.
 *
 * In Static3D mode only Position / Quat / Scale / Color are used.
 * In Dynamic4D mode the temporal fields drive appearance over the time axis:
 *   - AnchorTime    : the gaussian's "center" time t (within the cloud's time range)
 *   - TimeVariance  : exp(scaling_t), i.e. the temporal variance of the gaussian
 *   - Velocity      : optional rigid motion, PositionOffset = Velocity * (Time - AnchorTime)
 *   - Color.A       : base opacity (already sigmoid-activated); the renderer multiplies
 *                     it by the temporal marginal weight GetTimeWeight().
 */
USTRUCT(BlueprintType, meta = (DisplayName = "OpenSplat4D Point"))
struct OPENSPLAT4DRUNTIME_API FOpenSplat4DPoint
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OpenSplat4D")
	FVector3f Position = FVector3f::ZeroVector;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OpenSplat4D")
	FQuat4f Quat = FQuat4f::Identity;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OpenSplat4D")
	FVector3f Scale = FVector3f(1.f, 1.f, 1.f);

	/** RGBA. Alpha is the base opacity (already sigmoid-activated); 4D temporal weight is applied on top at render time. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OpenSplat4D")
	FLinearColor Color = FLinearColor::Black;

	/** 4D anchor time t (used in Dynamic4D mode). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OpenSplat4D")
	float AnchorTime = 0.f;

	/** 4D temporal variance = exp(scaling_t). Larger => the gaussian stays visible over a wider time window. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OpenSplat4D")
	float TimeVariance = 1e10f;

	/** 4D optional per-gaussian velocity (world units per unit time). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OpenSplat4D")
	FVector3f Velocity = FVector3f::ZeroVector;

	/** Whether Velocity should be applied in Dynamic4D mode. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OpenSplat4D")
	bool bUseVelocity = false;

	FOpenSplat4DPoint() = default;

	FOpenSplat4DPoint(FVector3f InPos, FQuat4f InQuat, FVector3f InScale, FLinearColor InColor)
		: Position(InPos), Quat(InQuat), Scale(InScale), Color(InColor)
	{
	}

	bool operator==(const FOpenSplat4DPoint& Other) const
	{
		return Position == Other.Position && Quat == Other.Quat && Scale == Other.Scale && Color == Other.Color
			&& AnchorTime == Other.AnchorTime && TimeVariance == Other.TimeVariance
			&& Velocity == Other.Velocity && bUseVelocity == Other.bUseVelocity;
	}
	bool operator!=(const FOpenSplat4DPoint& Other) const { return !(*this == Other); }

	// [ENHANCEMENT / parity with the reference GaussianSplattingPoint] The reference
	// defines operator< so points can be used as map/set keys and sorted. We sort by
	// Position.X first, then AnchorTime (so 4DGS points with the same spatial slot
	// stay ordered by their time axis) -- strictly more informative than the
	// reference's Position.X-only comparator.
	bool operator<(const FOpenSplat4DPoint& Other) const
	{
		if (Position.X != Other.Position.X) return Position.X < Other.Position.X;
		return AnchorTime < Other.AnchorTime;
	}

	// [ENHANCEMENT / parity with the reference GaussianSplattingPoint] Stable hash so
	// FOpenSplat4DPoint can live in TSet/TMap. We fold in every field (including the
	// new 4D temporal ones) so two points that differ only in time/velocity are not
	// collapsed -- the reference only hashed Position/Quat/Scale/Color.
	friend FORCEINLINE uint32 GetTypeHash(const FOpenSplat4DPoint& ID)
	{
		uint32 Hash = HashCombine(HashCombine(HashCombine(GetTypeHash(ID.Position), GetTypeHash(ID.Quat)), GetTypeHash(ID.Scale)), GetTypeHash(ID.Color));
		Hash = HashCombine(Hash, GetTypeHash(ID.AnchorTime));
		Hash = HashCombine(Hash, GetTypeHash(ID.TimeVariance));
		Hash = HashCombine(Hash, GetTypeHash(ID.Velocity));
		Hash = HashCombine(Hash, GetTypeHash(ID.bUseVelocity));
		return Hash;
	}

	/** Serialize a single point field-by-field. Used by the custom .4dgs binary format. */
	friend FArchive& operator<<(FArchive& Ar, FOpenSplat4DPoint& Point)
	{
		Ar << Point.Position;
		Ar << Point.Quat;
		Ar << Point.Scale;
		Ar << Point.Color;
		Ar << Point.AnchorTime;
		Ar << Point.TimeVariance;
		Ar << Point.Velocity;
		Ar << Point.bUseVelocity;
		return Ar;
	}
};
