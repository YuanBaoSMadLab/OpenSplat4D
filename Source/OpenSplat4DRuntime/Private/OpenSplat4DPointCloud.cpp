#include "OpenSplat4DPointCloud.h"
#include "OpenSplat4DCompression.h"
#include "Compression/Spz.h"   // [ENHANCEMENT] Niantic SPZ (4D-aware) compression

#include "Algo/Sort.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

const float GOPEN_SPLAT_SH_0 = 0.28209479177387814f;

UOpenSplat4DPointCloud::UOpenSplat4DPointCloud(FObjectInitializer const& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

// ----------------------------------------------------------------------------
// Time axis helpers
// ----------------------------------------------------------------------------
float UOpenSplat4DPointCloud::FractionToTime(float Fraction) const
{
	return FMath::Lerp(TimeStart, TimeEnd, FMath::Clamp(Fraction, 0.f, 1.f));
}

float UOpenSplat4DPointCloud::TimeToFraction(float Time) const
{
	const float Span = TimeEnd - TimeStart;
	if (FMath::IsNearlyZero(Span))
	{
		return 0.f;
	}
	return FMath::Clamp((Time - TimeStart) / Span, 0.f, 1.f);
}

// ----------------------------------------------------------------------------
// Bounds / LOD helpers
// ----------------------------------------------------------------------------
FRichCurve UOpenSplat4DPointCloud::CalcFeatureCurve()
{
	FRichCurve Curve;
	if (Points.Num() == 0)
	{
		return Curve;
	}
	const int32 Step = FMath::Max(Points.Num() / static_cast<int32>(FeatureLevel), 1);
	for (int32 i = 0; i < Points.Num(); i += Step)
	{
		const FKeyHandle KeyHandle = Curve.AddKey(4.f * Points[i].Scale.Length(), static_cast<float>(i));
		Curve.SetKeyInterpMode(KeyHandle, ERichCurveInterpMode::RCIM_Constant);
	}
	return Curve;
}

FBox UOpenSplat4DPointCloud::CalcBounds()
{
	FBox Bounds(EForceInit::ForceInit);
	if (Points.Num() == 0)
	{
		return Bounds;
	}
	for (const FOpenSplat4DPoint& Point : Points)
	{
		Bounds += FVector(Point.Position);
	}
	return Bounds.ExpandBy(FVector(Points[0].Scale.Length()));
}

void UOpenSplat4DPointCloud::SetPoints(const TArray<FOpenSplat4DPoint>& InPoints, bool bReorder)
{
	Points = InPoints;
	if (bReorder)
	{
		Algo::Sort(Points, [](const FOpenSplat4DPoint& A, const FOpenSplat4DPoint& B)
		{
			return A.Scale.Length() > B.Scale.Length();
		});
	}
	OnPointsChanged.Broadcast();
}

// ----------------------------------------------------------------------------
// 4D temporal math (faithful port of 4d-gaussian-splatting's get_marginal_t)
// ----------------------------------------------------------------------------
float UOpenSplat4DPointCloud::GetTimeWeight(float AnchorTime, float TimeVariance, float Time)
{
	// sigma = exp(scaling_t) is the temporal variance; weight = exp(-0.5 * (t - T)^2 / sigma)
	const float Variance = FMath::Max(TimeVariance, 1e-6f);
	const float Dt = AnchorTime - Time;
	return FMath::Exp(-0.5f * Dt * Dt / Variance);
}

void UOpenSplat4DPointCloud::SampleAtTime(float Time, TArray<FOpenSplat4DPoint>& OutPoints, float InMinWeight) const
{
	OutPoints.Reset();
	OutPoints.Reserve(Points.Num());

	const bool bIs4D = (Mode == EOpenSplat4DMode::Dynamic4D);
	for (const FOpenSplat4DPoint& Src : Points)
	{
		FOpenSplat4DPoint Dst = Src;

		if (bIs4D)
		{
			const float Weight = GetTimeWeight(Src.AnchorTime, Src.TimeVariance, Time);
			if (Weight < InMinWeight)
			{
				continue;
			}
			// Effective alpha = base opacity * temporal marginal weight.
			Dst.Color.A *= Weight;

			if (Src.bUseVelocity)
			{
				Dst.Position += Src.Velocity * (Time - Src.AnchorTime);
			}
		}

		OutPoints.Add(MoveTemp(Dst));
	}
}

// ----------------------------------------------------------------------------
// PLY (3DGS) import
// ----------------------------------------------------------------------------
namespace
{
	FLinearColor SRGBToLinear(const FLinearColor& Color)
	{
		auto SRGBToLinearFloat = [](const float C) -> float
		{
			return (C <= 0.04045f) ? C / 12.92f : FMath::Pow((C + 0.055f) / 1.055f, 2.4f);
		};
		return FLinearColor(
			SRGBToLinearFloat(Color.R),
			SRGBToLinearFloat(Color.G),
			SRGBToLinearFloat(Color.B),
			Color.A);
	}

	TArray<FOpenSplat4DPoint> ParseSplatFromStream(std::istream& In)
	{
		TArray<FOpenSplat4DPoint> Result;
		if (!In.good())
		{
			UE_LOG(LogOpenSplat4D, Warning, TEXT("Unable to read from input stream."));
			return Result;
		}

		std::string Line;
		auto GetHeaderLine = [&In](std::string& OutLine)
		{
			while (std::getline(In, OutLine))
			{
				if (OutLine.rfind("comment", 0) != 0 && OutLine.rfind("obj_info", 0) != 0)
				{
					return true;
				}
			}
			return false;
		};

		GetHeaderLine(Line);
		if (Line != "ply") { UE_LOG(LogOpenSplat4D, Warning, TEXT("Input data is not a .ply file.")); return Result; }
		GetHeaderLine(Line);
		if (Line != "format binary_little_endian 1.0") { UE_LOG(LogOpenSplat4D, Warning, TEXT("Unsupported .ply format.")); return Result; }
		GetHeaderLine(Line);
		if (Line.find("element vertex ") != 0) { UE_LOG(LogOpenSplat4D, Warning, TEXT("Missing vertex count.")); return Result; }

		const int32 NumPoints = std::stoi(Line.substr(std::strlen("element vertex ")));
		if (NumPoints <= 0 || NumPoints > 10 * 1024 * 1024)
		{
			UE_LOG(LogOpenSplat4D, Warning, TEXT("Invalid vertex count: %d"), NumPoints);
			return Result;
		}

		std::unordered_map<std::string, int> Fields;
		for (int32 i = 0;; i++)
		{
			if (!GetHeaderLine(Line)) { UE_LOG(LogOpenSplat4D, Warning, TEXT("Unexpected end of header.")); return Result; }
			if (Line == "end_header") break;
			if (Line.find("property float ") != 0) { UE_LOG(LogOpenSplat4D, Warning, TEXT("Unsupported property data type")); return Result; }
			Fields[Line.substr(std::strlen("property float "))] = i;
		}

		const auto Index = [&Fields](const std::string& Name) -> int32
		{
			const auto& It = Fields.find(Name);
			return It == Fields.end() ? -1 : It->second;
		};

		const std::vector<int32> PositionIdx = { Index("x"), Index("y"), Index("z") };
		const std::vector<int32> ScaleIdx = { Index("scale_0"), Index("scale_1"), Index("scale_2") };
		const std::vector<int32> RotIdx = { Index("rot_1"), Index("rot_2"), Index("rot_3"), Index("rot_0") };
		const std::vector<int32> AlphaIdx = { Index("opacity") };
		const std::vector<int32> ColorIdx = { Index("f_dc_0"), Index("f_dc_1"), Index("f_dc_2") };

		const auto Check = [&](const std::vector<int32>& Idx) -> bool
		{
			for (const int32 I : Idx) if (I < 0) return false;
			return true;
		};
		if (!Check(PositionIdx) || !Check(ScaleIdx) || !Check(RotIdx) || !Check(AlphaIdx) || !Check(ColorIdx))
		{
			return Result;
		}

		std::vector<float> Values;
		Values.resize(static_cast<size_t>(NumPoints) * Fields.size());
		In.read(reinterpret_cast<char*>(Values.data()), static_cast<std::streamsize>(Values.size() * sizeof(float)));
		if (!In.good()) { UE_LOG(LogOpenSplat4D, Warning, TEXT("Unable to load data from input stream.")); return Result; }

		Result.SetNum(NumPoints);
		const size_t Stride = Fields.size();
		for (int32 i = 0; i < NumPoints; i++)
		{
			const size_t O = static_cast<size_t>(i) * Stride;
			FOpenSplat4DPoint& Point = Result[i];

			const FVector3f Position = FVector3f(
				Values[O + PositionIdx[0]], Values[O + PositionIdx[1]], Values[O + PositionIdx[2]]);
			Point.Position = 100.f * FVector3f(Position.X, -Position.Z, -Position.Y);

			const FVector3f Scale = FVector3f(
				FMath::Exp(Values[O + ScaleIdx[0]]),
				FMath::Exp(Values[O + ScaleIdx[1]]),
				FMath::Exp(Values[O + ScaleIdx[2]]));
			Point.Scale = 100.f * FVector3f(Scale.X, Scale.Z, Scale.Y);

			FQuat4f Quat = FQuat4f(
				Values[O + RotIdx[0]], Values[O + RotIdx[1]], Values[O + RotIdx[2]], Values[O + RotIdx[3]]);
			Quat.Normalize();
			Point.Quat = FQuat4f(Quat.X, -Quat.Z, -Quat.Y, Quat.W);

			FLinearColor Color = FLinearColor(
				GOPEN_SPLAT_SH_0 * Values[O + ColorIdx[0]] + 0.5f,
				GOPEN_SPLAT_SH_0 * Values[O + ColorIdx[1]] + 0.5f,
				GOPEN_SPLAT_SH_0 * Values[O + ColorIdx[2]] + 0.5f,
				1.0f / (1.0f + FMath::Exp(-Values[O + AlphaIdx[0]])));
			Point.Color = SRGBToLinear(Color);

			// 3DGS import: temporal fields disabled.
			Point.AnchorTime = 0.f;
			Point.TimeVariance = 1e10f;
			Point.Velocity = FVector3f::ZeroVector;
			Point.bUseVelocity = false;
		}
		return Result;
	}
} // namespace

TArray<FOpenSplat4DPoint> UOpenSplat4DPointCloud::LoadPointsFromPLY(FString InFilePath)
{
	std::ifstream IStream(TCHAR_TO_UTF8(*InFilePath), std::ios::binary);
	if (!IStream.is_open())
	{
		UE_LOG(LogOpenSplat4D, Warning, TEXT("Unable to open: %s"), *InFilePath);
		return {};
	}
	return ParseSplatFromStream(IStream);
}

TArray<FOpenSplat4DPoint> UOpenSplat4DPointCloud::LoadPointsFromSPZ(FString InFilePath)
{
	TArray<uint8> Buffer;
	if (!FFileHelper::LoadFileToArray(Buffer, *InFilePath))
	{
		UE_LOG(LogOpenSplat4D, Warning, TEXT("Unable to open .spz: %s"), *InFilePath);
		return {};
	}
	std::vector<uint8_t> InBuf(Buffer.GetData(), Buffer.GetData() + Buffer.Num());
	TArray<FOpenSplat4DPoint> Result;
	if (!Spz::decompress(InBuf, Result))
	{
		UE_LOG(LogOpenSplat4D, Warning, TEXT("OpenSplat4D: SPZ decompress failed for %s"), *InFilePath);
		return {};
	}
	return Result;
}

bool UOpenSplat4DPointCloud::SaveToSPZ(FString InFilePath)
{
	std::vector<uint8_t> OutBuf;
	if (!Spz::compress(Points, 3, 1, OutBuf, /*bInclude4D=*/true))
	{
		UE_LOG(LogOpenSplat4D, Warning, TEXT("OpenSplat4D: SPZ compress failed for %s"), *InFilePath);
		return false;
	}
	TArray<uint8> Compressed(OutBuf.data(), static_cast<int32>(OutBuf.size()));
	return FFileHelper::SaveArrayToFile(Compressed, *InFilePath);
}

void UOpenSplat4DPointCloud::LoadFromFile(FString InFilePath)
{
	// [ENHANCEMENT] The reference only knew how to load .ply. OpenSplat4D dispatches
	// on the file extension so a single LoadFromFile understands .ply, .spz and .4dgs.
	const FString Ext = FPaths::GetExtension(InFilePath).ToLower();

	if (Ext == TEXT("spz"))
	{
		TArray<FOpenSplat4DPoint> Loaded = LoadPointsFromSPZ(InFilePath);
		if (Loaded.Num() > 0)
		{
			// Auto-detect 4DGS: if any gaussian carries temporal data, default to the
			// dynamic mode so it renders as a time-varying cloud out of the box.
			bool bHas4D = false;
			for (const FOpenSplat4DPoint& P : Loaded)
			{
				if (P.AnchorTime != 0.f || P.TimeVariance != 1e10f || P.bUseVelocity || !P.Velocity.IsNearlyZero())
				{
					bHas4D = true;
					break;
				}
			}
			Mode = bHas4D ? EOpenSplat4DMode::Dynamic4D : EOpenSplat4DMode::Static3D;
			SetPoints(Loaded);
		}
		return;
	}

	if (Ext == TEXT("4dgs"))
	{
		LoadFrom4DGS(InFilePath);
		return;
	}

	// Default path: standard 3DGS .ply (binary_little_endian).
	TArray<FOpenSplat4DPoint> Loaded = LoadPointsFromPLY(InFilePath);
	Mode = EOpenSplat4DMode::Static3D;
	SetPoints(Loaded);
}

// ----------------------------------------------------------------------------
// Native .4dgs binary (3D or 4D)
// ----------------------------------------------------------------------------
namespace
{
	constexpr uint8 GOPEN_4DGS_MAGIC[4] = { 'O', '4', 'D', '1' };
} // namespace

bool UOpenSplat4DPointCloud::SaveTo4DGS(FString InFilePath)
{
	TArray<uint8> Buffer;
	{
		FMemoryWriter Writer(Buffer, true);
		Writer.Serialize(const_cast<uint8*>(GOPEN_4DGS_MAGIC), 4);
		uint8 ModeByte = static_cast<uint8>(Mode);
		Writer << ModeByte;
		Writer << TimeStart;
		Writer << TimeEnd;
		int32 Count = Points.Num();
		Writer << Count;
		for (const FOpenSplat4DPoint& P : Points)
		{
			Writer << const_cast<FOpenSplat4DPoint&>(P);
		}
	}

	return FFileHelper::SaveArrayToFile(Buffer, *InFilePath);
}

bool UOpenSplat4DPointCloud::LoadFrom4DGS(FString InFilePath)
{
	TArray<uint8> Buffer;
	if (!FFileHelper::LoadFileToArray(Buffer, *InFilePath))
	{
		UE_LOG(LogOpenSplat4D, Warning, TEXT("Unable to open .4dgs: %s"), *InFilePath);
		return false;
	}

	FMemoryReader Reader(Buffer, true);
	uint8 Magic[4] = { 0, 0, 0, 0 };
	Reader.Serialize(Magic, 4);
	if (Magic[0] != 'O' || Magic[1] != '4' || Magic[2] != 'D' || Magic[3] != '1')
	{
		UE_LOG(LogOpenSplat4D, Warning, TEXT("Bad .4dgs magic in: %s"), *InFilePath);
		return false;
	}

	uint8 ModeByte = 0;
	Reader << ModeByte;
	Mode = static_cast<EOpenSplat4DMode>(ModeByte);
	Reader << TimeStart;
	Reader << TimeEnd;

	int32 Count = 0;
	Reader << Count;
	if (Count < 0 || Count > 10 * 1024 * 1024)
	{
		UE_LOG(LogOpenSplat4D, Warning, TEXT("Invalid .4dgs point count: %d"), Count);
		return false;
	}

	TArray<FOpenSplat4DPoint> Loaded;
	Loaded.SetNum(Count);
	for (int32 i = 0; i < Count; i++)
	{
		Reader << Loaded[i];
	}
	SetPoints(Loaded, /*bReorder=*/false);
	return true;
}

// ----------------------------------------------------------------------------
// Asset serialization
// ----------------------------------------------------------------------------
void UOpenSplat4DPointCloud::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	// [ENHANCEMENT] The reference GaussianSplattingRuntime.Serialize only had two
	// branches (None / Zlib) where Zlib == the Niantic SPZ path. OpenSplat4D keeps
	// both of those AND adds a dedicated, 4D-aware SPZ branch:
	//   * None  -> raw TArray<FOpenSplat4DPoint> (lossless, largest)
	//   * Zlib  -> whole-blob gzip of the raw TArray (FOpenSplat4DCompression)
	//   * Spz   -> per-gaussian 64-byte fixed-point quantization + gzip, with the
	//             4DGS temporal fields appended in an O4D4 block (Compression/Spz.h)
	// All three are retained, so there is no reduction versus either the reference
	// or the previous OpenSplat4D implementation.
	int32 Count = Points.Num();
	if (Ar.IsLoading())
	{
		Ar << Count;
		Points.Reset();
		Points.SetNum(Count);

		if (GetCompressionMethod() == EOpenSplat4DCompressionMethod::None)
		{
			for (int32 i = 0; i < Count; i++)
			{
				Ar << Points[i];
			}
		}
		else if (GetCompressionMethod() == EOpenSplat4DCompressionMethod::Zlib)
		{
			int32 RawSize = 0;
			int32 CompressedSize = 0;
			Ar << RawSize << CompressedSize;

			TArray<uint8> Compressed;
			Compressed.SetNum(CompressedSize);
			Ar.Serialize(Compressed.GetData(), CompressedSize);

			TArray<uint8> Raw;
			if (!FOpenSplat4DCompression::Decompress(Compressed, Raw) || Raw.Num() != RawSize)
			{
				UE_LOG(LogOpenSplat4D, Warning, TEXT("OpenSplat4D: failed to decompress point cloud."));
				Points.Reset();
				return;
			}

			FMemoryReader MemReader(Raw, true);
			int32 StoredCount = 0;
			MemReader << StoredCount;
			if (StoredCount != Count)
			{
				UE_LOG(LogOpenSplat4D, Warning, TEXT("OpenSplat4D: point count mismatch after decompression."));
				Points.Reset();
				return;
			}
			for (int32 i = 0; i < Count; i++)
			{
				MemReader << Points[i];
			}
		}
		else // Spz (Niantic, 4D-aware)
		{
			int32 CompressedSize = 0;
			Ar << CompressedSize;

			TArray<uint8> Compressed;
			Compressed.SetNum(CompressedSize);
			Ar.Serialize(Compressed.GetData(), CompressedSize);

			std::vector<uint8_t> InBuf(Compressed.GetData(), Compressed.GetData() + CompressedSize);
			TArray<FOpenSplat4DPoint> Loaded;
			if (!Spz::decompress(InBuf, Loaded) || Loaded.Num() != Count)
			{
				UE_LOG(LogOpenSplat4D, Warning, TEXT("OpenSplat4D: failed to decompress SPZ point cloud (count mismatch or error)."));
				Points.Reset();
				return;
			}
			Points = MoveTemp(Loaded);
		}
	}
	else // Saving
	{
		Ar << Count;

		if (GetCompressionMethod() == EOpenSplat4DCompressionMethod::None)
		{
			for (int32 i = 0; i < Count; i++)
			{
				Ar << Points[i];
			}
		}
		else if (GetCompressionMethod() == EOpenSplat4DCompressionMethod::Zlib)
		{
			TArray<uint8> Raw;
			{
				FMemoryWriter MemWriter(Raw, true);
				int32 RawCount = Count;
				MemWriter << RawCount;
				for (int32 i = 0; i < Count; i++)
				{
					MemWriter << Points[i];
				}
			}

			TArray<uint8> Compressed;
			if (!FOpenSplat4DCompression::Compress(Raw, Compressed))
			{
				UE_LOG(LogOpenSplat4D, Warning, TEXT("OpenSplat4D: failed to compress point cloud."));
				return;
			}

			int32 RawSize = Raw.Num();
			int32 CompressedSize = Compressed.Num();
			Ar << RawSize << CompressedSize;
			Ar.Serialize(Compressed.GetData(), CompressedSize);
		}
		else // Spz (Niantic, 4D-aware)
		{
			std::vector<uint8_t> OutBuf;
			if (!Spz::compress(Points, 3, 1, OutBuf, /*bInclude4D=*/true))
			{
				UE_LOG(LogOpenSplat4D, Warning, TEXT("OpenSplat4D: failed to compress point cloud to SPZ."));
				return;
			}
			int32 CompressedSize = static_cast<int32>(OutBuf.size());
			Ar << CompressedSize;
			Ar.Serialize(OutBuf.data(), OutBuf.size());
		}
	}
}
