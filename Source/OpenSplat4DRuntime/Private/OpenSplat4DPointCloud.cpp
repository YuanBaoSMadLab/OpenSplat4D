#include "OpenSplat4DPointCloud.h"
#include "OpenSplat4DCompression.h"
#include "Compression/Spz.h"   // [ENHANCEMENT] Niantic SPZ (4D-aware) compression

#include "Algo/Sort.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
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
void UOpenSplat4DPointCloud::PostLoad()

{

	Super::PostLoad();



	// The on-disk source file is the source of truth. If the cloud came back

	// empty after (de)serialisation -- e.g. an asset saved before the splat

	// data was serialised, or a previously-failed import -- repopulate it so

	// the preview is never silently blank. This fires on EVERY load path

	// (opening the asset editor, dragging into a level, spawning an actor,

	// cooking), unlike the editor-only InitEditor reload.

	if (GetPointCount() == 0 && !SourceFilePath.IsEmpty() && FPaths::FileExists(SourceFilePath))

	{

		const FString Ext = FPaths::GetExtension(SourceFilePath).ToLower();

		if (Ext == TEXT("ply") || Ext == TEXT("4dgs"))

		{

			LoadFromFile(SourceFilePath);

			UE_LOG(LogOpenSplat4D, Log,

				TEXT("OpenSplat4D: asset loaded with 0 points, auto-reloaded from source: %s -> pointCount=%d"),

				*SourceFilePath, GetPointCount());

		}

	}

}
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
	// [FIX] Do NOT open the .ply via std::ifstream(const char*). On Windows that
	// path overload uses the ANSI codepage, so any non-ASCII characters in the
	// path (e.g. Chinese project names like "鎴戠殑椤圭洰3") get mangled and the
	// file silently fails to open -> "Unable to open: .../point_cloud.ply" even
	// though the file exists. Read the whole file through UE's wide-char-safe
	// FFileHelper into a memory buffer, then parse that buffer.
	TArray<uint8> Buffer;
	if (!FFileHelper::LoadFileToArray(Buffer, *InFilePath))
	{
		UE_LOG(LogOpenSplat4D, Warning, TEXT("Unable to open: %s"), *InFilePath);
		return {};
	}

	std::string Data(reinterpret_cast<const char*>(Buffer.GetData()), static_cast<size_t>(Buffer.Num()));
	std::istringstream Stream(Data);
	return ParseSplatFromStream(Stream);
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
	// [FIX] 瀵归綈鏁欏笀 GaussianSplattingPointCloud::Serialize 鐨勭ǔ濡ョ粨鏋勶細
	// 鍏?Super::Serialize锛堝啓/璇?UPROPERTY锛屽寘鎷?CompressionMethod锛夛紝
	// 鍐嶆寜鍘嬬缉鏂瑰紡澶勭悊 Points銆傛敞鎰?Points 鏄?Transient锛屽繀椤荤敱鏈嚱鏁版墜鍔ㄥ簭鍒楀寲銆?
	// 鍏抽敭淇锛氫笉鍐嶇淮鎶ょ嫭绔嬬殑 Count 鍙橀噺銆佷笉鍐嶇敤 Loaded 鏁扮粍 + 涓嶅尮閰嶅嵆娓呴浂锛?
	// 鑰屾槸鍍忔暀甯堥偅鏍锋妸 SPZ/Zlib 鐩存帴瑙ｅ帇杩?Points锛岄伩鍏?Points 琚剰澶栨竻闆躲€?

	// 淇濆瓨鍓嶅厛鍐冲畾鏈€缁堣惤鍦版柟寮忥紙鑻?SPZ 鍘嬬缉澶辫触鍒欐暣浣撳洖閫€涓?Zlib锛?
	// 涓斿湪 Super::Serialize 涔嬪墠淇敼 CompressionMethod锛岀‘淇濈鐩樿褰曟纭級銆?
	if (Ar.IsSaving() && GetCompressionMethod() == EOpenSplat4DCompressionMethod::Spz)
	{
		std::vector<uint8_t> TestBuf;
		if (!Spz::compress(Points, 3, 1, TestBuf, /*bInclude4D=*/true))
		{
			UE_LOG(LogOpenSplat4D, Warning, TEXT("OpenSplat4D: SPZ compression test failed; falling back to Zlib save."));
			CompressionMethod = EOpenSplat4DCompressionMethod::Zlib;
		}
	}

	Super::Serialize(Ar);

	const EOpenSplat4DCompressionMethod Method = GetCompressionMethod();

	if (Method == EOpenSplat4DCompressionMethod::None)
	{
		Ar << Points;
	}
	else if (Method == EOpenSplat4DCompressionMethod::Zlib)
	{
		if (Ar.IsLoading())
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
				UE_LOG(LogOpenSplat4D, Warning, TEXT("OpenSplat4D: Zlib point-cloud decompression failed."));
				Points.Reset();
				return;
			}
			FMemoryReader MemReader(Raw, true);
			MemReader << Points;
		}
		else
		{
			TArray<uint8> Raw;
			{
				FMemoryWriter MemWriter(Raw, true);
				MemWriter << Points;
			}
			TArray<uint8> Compressed;
			if (!FOpenSplat4DCompression::Compress(Raw, Compressed))
			{
				UE_LOG(LogOpenSplat4D, Warning, TEXT("OpenSplat4D: Zlib point-cloud compression failed."));
				return;
			}
			int32 RawSize = Raw.Num();
			int32 CompressedSize = Compressed.Num();
			Ar << RawSize << CompressedSize;
			Ar.Serialize(Compressed.GetData(), CompressedSize);
		}
	}
	else // Spz (Niantic, 4D-aware)
	{
		if (Ar.IsLoading())
		{
			int32 CompressedSize = 0;
			Ar << CompressedSize;
			TArray<uint8> Compressed;
			Compressed.SetNum(CompressedSize);
			Ar.Serialize(Compressed.GetData(), CompressedSize);
			std::vector<uint8_t> InBuf(Compressed.GetData(), Compressed.GetData() + CompressedSize);
			if (!Spz::decompress(InBuf, Points))
			{
				UE_LOG(LogOpenSplat4D, Warning, TEXT("OpenSplat4D: SPZ point-cloud decompression failed."));
				Points.Reset();
				return;
			}
			UE_LOG(LogOpenSplat4D, Log, TEXT("OpenSplat4D: asset deserialization done: pointCount=%d, method=Spz"), Points.Num());
		}
		else
		{
			std::vector<uint8_t> OutBuf;
			if (!Spz::compress(Points, 3, 1, OutBuf, /*bInclude4D=*/true))
			{
				UE_LOG(LogOpenSplat4D, Warning, TEXT("OpenSplat4D: SPZ point-cloud compression failed (pointCount=%d)."), Points.Num());
				return;
			}
			int32 CompressedSize = static_cast<int32>(OutBuf.size());
			Ar << CompressedSize;
			Ar.Serialize(OutBuf.data(), OutBuf.size());
		}
	}
}
