// Copyright Epic Games, Inc. All Rights Reserved.

#include "PLYFileReader.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMemory.h"

bool FPLYFileReader::ReadPLYFile(const FString& FilePath, TArray<FGaussianSplatData>& OutSplats, FString& OutError, int32* OutSHBands, bool* OutHasTemporal)
{
	OutSplats.Empty();
	if (OutHasTemporal)
	{
		*OutHasTemporal = false;
	}

	// Open file with IFileHandle for streamed reading (supports files > 2 GB)
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> FileHandle(PlatformFile.OpenRead(*FilePath));

	if (!FileHandle)
	{
		OutError = FString::Printf(TEXT("Failed to open file: %s"), *FilePath);
		return false;
	}

	const int64 FileSize = FileHandle->Size();
	UE_LOG(LogTemp, Log, TEXT("PLY file size: %lld bytes (%.2f GB)"), FileSize, FileSize / (1024.0 * 1024.0 * 1024.0));

	if (FileSize < 4)
	{
		OutError = TEXT("File too small to be a valid PLY file");
		return false;
	}

	// Parse header (positions file handle at start of vertex data)
	FPLYHeader Header;
	if (!ParseHeader(FileHandle.Get(), Header, OutError))
	{
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("PLY Header parsed: %d vertices, %d bytes per vertex, data at offset %lld"),
		Header.VertexCount, Header.VertexStride, Header.DataOffset);

	// Detect SH band count from header properties
	// PLY stores SH in planar format: all R coefficients, then G, then B
	// Band counts and f_rest indices:
	// - 0 bands: no f_rest (DC only in f_dc)
	// - 1 band:  3 coeffs/channel × 3 = 9 total  → f_rest_0..8
	// - 2 bands: 8 coeffs/channel × 3 = 24 total → f_rest_0..23
	// - 3 bands: 15 coeffs/channel × 3 = 45 total → f_rest_0..44
	if (OutSHBands)
	{
		if (Header.PropertyOffsets.Contains(TEXT("f_rest_44")))
		{
			*OutSHBands = 3;  // Has all 45 coefficients (15 per channel)
		}
		else if (Header.PropertyOffsets.Contains(TEXT("f_rest_23")))
		{
			*OutSHBands = 2;  // Has 24 coefficients (8 per channel)
		}
		else if (Header.PropertyOffsets.Contains(TEXT("f_rest_8")))
		{
			*OutSHBands = 1;  // Has 9 coefficients (3 per channel)
		}
		else
		{
			*OutSHBands = 0;  // No f_rest data (DC only)
		}
		UE_LOG(LogTemp, Log, TEXT("PLYFileReader: Detected SH bands = %d"), *OutSHBands);
	}

	// Detect 4D temporal properties (spacetime gaussian format):
	//   t       = anchor time of each gaussian
	//   scale_t = log-encoded temporal sigma (sigma = exp(scale_t))
	const bool bHasTemporalProps =
		Header.PropertyOffsets.Contains(TEXT("t")) ||
		Header.PropertyOffsets.Contains(TEXT("scale_t"));
	if (OutHasTemporal)
	{
		*OutHasTemporal = bHasTemporalProps;
	}
	if (bHasTemporalProps)
	{
		UE_LOG(LogTemp, Log, TEXT("PLYFileReader: Detected 4D temporal properties (t/scale_t)"));
	}

	// Validate file size against expected data
	const int64 ExpectedEnd = Header.DataOffset + static_cast<int64>(Header.VertexCount) * Header.VertexStride;
	if (ExpectedEnd > FileSize)
	{
		OutError = FString::Printf(TEXT("File truncated: expected %lld bytes of vertex data, file size is %lld"),
			ExpectedEnd, FileSize);
		return false;
	}

	// Memory guard: the CPU-side splat array is ~240 bytes per vertex. Refuse
	// huge imports up-front with a clear message instead of crashing (OOM can
	// surface as a random access violation deep inside the read loop).
	const int64 RequiredBytes = static_cast<int64>(Header.VertexCount) * sizeof(FGaussianSplatData);
	const FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();
	if (MemStats.AvailablePhysical > 0 && RequiredBytes > static_cast<int64>(MemStats.AvailablePhysical * 0.75))
	{
		OutError = FString::Printf(TEXT("Not enough memory to import %d splats: needs %.1f GB, only %.1f GB available. Reduce the splat count (decimate the PLY) or close other applications."),
			Header.VertexCount,
			RequiredBytes / (1024.0 * 1024.0 * 1024.0),
			MemStats.AvailablePhysical / (1024.0 * 1024.0 * 1024.0));
		return false;
	}

	// Read vertex data using streamed I/O
	if (!ReadVertexData(FileHandle.Get(), Header, OutSplats, OutError))
	{
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("Successfully read %d splats from PLY file"), OutSplats.Num());
	return true;
}

bool FPLYFileReader::IsValidPLYFile(const FString& FilePath)
{
	// Quick check: read first few bytes and look for "ply" magic
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> FileHandle(PlatformFile.OpenRead(*FilePath));

	if (!FileHandle)
	{
		return false;
	}

	uint8 HeaderBytes[4];
	if (!FileHandle->Read(HeaderBytes, 4))
	{
		return false;
	}

	// Check for "ply\n" or "ply\r\n" magic
	return HeaderBytes[0] == 'p' && HeaderBytes[1] == 'l' && HeaderBytes[2] == 'y';
}

bool FPLYFileReader::ParseHeader(IFileHandle* FileHandle, FPLYHeader& OutHeader, FString& OutError)
{
	// PLY headers are ASCII text, typically < 4 KB but we read up to 64 KB to be safe
	constexpr int32 MaxHeaderSize = 65536;
	TArray<uint8> HeaderBuffer;
	HeaderBuffer.SetNumUninitialized(MaxHeaderSize);

	// Read header bytes from the start of the file
	FileHandle->Seek(0);
	const int64 FileSize = FileHandle->Size();
	const int32 BytesToRead = static_cast<int32>(FMath::Min(static_cast<int64>(MaxHeaderSize), FileSize));

	if (!FileHandle->Read(HeaderBuffer.GetData(), BytesToRead))
	{
		OutError = TEXT("Failed to read PLY header bytes");
		return false;
	}

	// Find "end_header" marker
	const char* EndHeaderMarker = "end_header";
	const int32 MarkerLen = FCStringAnsi::Strlen(EndHeaderMarker);
	int32 HeaderEnd = -1;

	for (int32 i = 0; i < BytesToRead - MarkerLen; i++)
	{
		if (FMemory::Memcmp(&HeaderBuffer[i], EndHeaderMarker, MarkerLen) == 0)
		{
			// Find the newline after end_header
			for (int32 j = i + MarkerLen; j < BytesToRead; j++)
			{
				if (HeaderBuffer[j] == '\n')
				{
					HeaderEnd = j + 1;
					break;
				}
			}
			break;
		}
	}

	if (HeaderEnd < 0)
	{
		OutError = TEXT("Could not find 'end_header' in PLY file (or header exceeds 64 KB)");
		return false;
	}

	// Convert header to string
	FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(HeaderBuffer.GetData()), HeaderEnd);
	FString HeaderString(Converter.Length(), Converter.Get());

	OutHeader.DataOffset = HeaderEnd;

	// Parse header lines
	TArray<FString> Lines;
	HeaderString.ParseIntoArrayLines(Lines);

	bool bFoundPly = false;
	bool bInVertexElement = false;
	int32 CurrentOffset = 0;

	for (const FString& Line : Lines)
	{
		FString TrimmedLine = Line.TrimStartAndEnd();

		if (TrimmedLine == TEXT("ply"))
		{
			bFoundPly = true;
			continue;
		}

		if (TrimmedLine.StartsWith(TEXT("format")))
		{
			if (TrimmedLine.Contains(TEXT("binary_little_endian")))
			{
				OutHeader.bBinaryLittleEndian = true;
			}
			else if (TrimmedLine.Contains(TEXT("binary_big_endian")))
			{
				OutHeader.bBinaryLittleEndian = false;
				OutError = TEXT("Big endian PLY files are not supported");
				return false;
			}
			else if (TrimmedLine.Contains(TEXT("ascii")))
			{
				OutError = TEXT("ASCII PLY files are not supported, please use binary format");
				return false;
			}
			continue;
		}

		if (TrimmedLine.StartsWith(TEXT("element vertex")))
		{
			TArray<FString> Parts;
			// ParseIntoArrayWS: split on ANY whitespace (space/tab/multiple), so
			// headers written with tabs or double spaces still parse correctly.
			TrimmedLine.ParseIntoArrayWS(Parts);
			if (Parts.Num() >= 3)
			{
				OutHeader.VertexCount = FCString::Atoi(*Parts[2]);
			}
			bInVertexElement = true;
			continue;
		}

		if (TrimmedLine.StartsWith(TEXT("element")))
		{
			bInVertexElement = false;
			continue;
		}

		if (bInVertexElement && TrimmedLine.StartsWith(TEXT("property")))
		{
			TArray<FString> Parts;
			TrimmedLine.ParseIntoArrayWS(Parts);

			if (Parts.Num() >= 3)
			{
				FString Type = Parts[1];
				FString Name = Parts[2];

				int32 TypeSize = 0;
				if (Type == TEXT("float") || Type == TEXT("float32"))
				{
					TypeSize = 4;
				}
				else if (Type == TEXT("double") || Type == TEXT("float64"))
				{
					TypeSize = 8;
				}
				else if (Type == TEXT("uchar") || Type == TEXT("uint8") || Type == TEXT("char") || Type == TEXT("int8"))
				{
					TypeSize = 1;
				}
				else if (Type == TEXT("ushort") || Type == TEXT("uint16") || Type == TEXT("short") || Type == TEXT("int16"))
				{
					TypeSize = 2;
				}
				else if (Type == TEXT("uint") || Type == TEXT("uint32") || Type == TEXT("int") || Type == TEXT("int32"))
				{
					TypeSize = 4;
				}
				else if (Type == TEXT("int64") || Type == TEXT("uint64") || Type == TEXT("long"))
				{
					TypeSize = 8;
				}
				else
				{
					// Unknown type (e.g. "list"): skip the property entirely so it
					// can never produce a bogus offset within the vertex data.
					UE_LOG(LogTemp, Warning, TEXT("PLYFileReader: Skipping unsupported property type '%s' (name '%s')"), *Type, *Name);
					continue;
				}

				OutHeader.PropertyNames.Add(Name);
				OutHeader.PropertyOffsets.Add(Name, CurrentOffset);
				OutHeader.PropertySizes.Add(Name, TypeSize);
				CurrentOffset += TypeSize;
			}
			continue;
		}
	}

	if (!bFoundPly)
	{
		OutError = TEXT("File does not start with 'ply' magic");
		return false;
	}

	if (OutHeader.VertexCount <= 0)
	{
		OutError = TEXT("No vertices found in PLY file");
		return false;
	}

	// Sanity check on vertex count: 3DGS models rarely exceed ~50M splats.
	// Rejecting implausibly large counts up-front prevents OOM from a corrupt
	// or hostile header (e.g. VertexCount = INT_MAX).
	constexpr int32 MaxReasonableVertexCount = 200 * 1024 * 1024;  // 200 M
	if (OutHeader.VertexCount > MaxReasonableVertexCount)
	{
		OutError = FString::Printf(TEXT("Vertex count %d exceeds safety limit %d (likely a corrupt PLY header)"),
			OutHeader.VertexCount, MaxReasonableVertexCount);
		return false;
	}

	OutHeader.VertexStride = CurrentOffset;

	if (OutHeader.VertexStride <= 0)
	{
		OutError = FString::Printf(TEXT("Parsed vertex stride is 0 — PLY header has no recognized properties. Found property names: [%s]"),
			*FString::Join(OutHeader.PropertyNames, TEXT(", ")));
		return false;
	}

	// Verify we have the minimum required properties for a point cloud.
	// x, y, z are mandatory; gaussian properties (opacity, scale, rotation,
	// SH) are optional — standard COLMAP point clouds will get sensible
	// defaults injected in ReadVertexData.
	TArray<FString> RequiredProps = { TEXT("x"), TEXT("y"), TEXT("z") };

	for (const FString& Prop : RequiredProps)
	{
		if (!OutHeader.PropertyOffsets.Contains(Prop))
		{
			OutError = FString::Printf(TEXT("Missing required property: %s"), *Prop);
			return false;
		}
	}

	// Detect whether this is a full gaussian PLY or a bare point cloud
	const bool bHasGaussianProps = OutHeader.PropertyOffsets.Contains(TEXT("opacity"));

	// Seek file handle to start of vertex data
	FileHandle->Seek(OutHeader.DataOffset);

	return true;
}

bool FPLYFileReader::ReadVertexData(IFileHandle* FileHandle, const FPLYHeader& Header, TArray<FGaussianSplatData>& OutSplats, FString& OutError)
{
	OutSplats.SetNum(Header.VertexCount);

	// Detect SH coefficients per channel from available properties
	// PLY stores SH in planar format: all R coefficients, then G, then B
	// - 1 band:  3 coeffs/channel (f_rest_0..8)
	// - 2 bands: 8 coeffs/channel (f_rest_0..23)
	// - 3 bands: 15 coeffs/channel (f_rest_0..44)
	int32 CoeffsPerChannel = 0;
	if (Header.PropertyOffsets.Contains(TEXT("f_rest_44")))
	{
		CoeffsPerChannel = 15;  // 3 bands
	}
	else if (Header.PropertyOffsets.Contains(TEXT("f_rest_23")))
	{
		CoeffsPerChannel = 8;   // 2 bands
	}
	else if (Header.PropertyOffsets.Contains(TEXT("f_rest_8")))
	{
		CoeffsPerChannel = 3;   // 1 band
	}

	UE_LOG(LogTemp, Log, TEXT("PLYFileReader: VertexCount=%d, SH CoeffsPerChannel=%d"),
		Header.VertexCount, CoeffsPerChannel);

	// Read vertices in chunks for efficiency (4096 vertices per chunk)
	constexpr int32 ChunkSize = 4096;
	const int32 VertexStride = Header.VertexStride;
	TArray<uint8> ChunkBuffer;
	ChunkBuffer.SetNumUninitialized(ChunkSize * VertexStride);

	// Precompute property offsets ONCE (not per vertex): with 57M vertices and
	// 20+ lookups each, per-vertex FString Printf + TMap Find dominated import
	// time and hammered the allocator. All cached properties below are declared
	// float in 3DGS PLY files; color/other fallbacks keep using the type-aware
	// GetPropertyFloat.
	auto FindOff = [&Header](const TCHAR* Name) -> int32
	{
		if (const int32* P = Header.PropertyOffsets.Find(FString(Name)))
		{
			return *P;
		}
		return -1;
	};
	auto ReadF = [VertexStride](const uint8* VD, int32 Off, float Def) -> float
	{
		// Bounds check keeps the chunk read inside the vertex record.
		return (Off >= 0 && Off + 4 <= VertexStride) ? *reinterpret_cast<const float*>(VD + Off) : Def;
	};

	const int32 OffX = FindOff(TEXT("x")), OffY = FindOff(TEXT("y")), OffZ = FindOff(TEXT("z"));
	const int32 OffRot[4] = { FindOff(TEXT("rot_0")), FindOff(TEXT("rot_1")), FindOff(TEXT("rot_2")), FindOff(TEXT("rot_3")) };
	const int32 OffScale[3] = { FindOff(TEXT("scale_0")), FindOff(TEXT("scale_1")), FindOff(TEXT("scale_2")) };
	const int32 OffOpacity = FindOff(TEXT("opacity"));
	const int32 OffDC[3] = { FindOff(TEXT("f_dc_0")), FindOff(TEXT("f_dc_1")), FindOff(TEXT("f_dc_2")) };
	const int32 OffT = FindOff(TEXT("t"));
	const int32 OffScaleT = FindOff(TEXT("scale_t"));
	const bool bHasDC = OffDC[0] >= 0 && OffDC[1] >= 0 && OffDC[2] >= 0;
	const bool bHasTemporal = OffT >= 0 || OffScaleT >= 0;

	int32 OffRest[3][15];
	for (int32 c = 0; c < 15; c++)
	{
		for (int32 ch = 0; ch < 3; ch++)
		{
			OffRest[ch][c] = (c < CoeffsPerChannel)
				? FindOff(*FString::Printf(TEXT("f_rest_%d"), c + ch * CoeffsPerChannel))
				: -1;
		}
	}

	int32 VerticesRemaining = Header.VertexCount;
	int32 VertexIndex = 0;

	while (VerticesRemaining > 0)
	{
		const int32 VerticesToRead = FMath::Min(ChunkSize, VerticesRemaining);
		const int32 BytesToRead = VerticesToRead * VertexStride;

		if (!FileHandle->Read(ChunkBuffer.GetData(), BytesToRead))
		{
			OutError = FString::Printf(TEXT("Failed to read vertex data at vertex %d"), VertexIndex);
			return false;
		}

		for (int32 i = 0; i < VerticesToRead; i++)
		{
			const uint8* VertexData = ChunkBuffer.GetData() + i * VertexStride;
			FGaussianSplatData& Splat = OutSplats[VertexIndex];

			// Position - Convert from Y-down (COLMAP/OpenCV convention) to Z-up (Unreal)
			// PLY: X-right, Y-down, Z-forward (right-handed) -> UE: X-forward, Y-right, Z-up (left-handed)
			// Most 3DGS training pipelines use COLMAP which has Y pointing down
			// Multiply by 100 to convert from meters (PLY) to centimeters (UE)
			constexpr float MetersToUE = 100.0f;
			float PlyX = ReadF(VertexData, OffX, 0.0f);
			float PlyY = ReadF(VertexData, OffY, 0.0f);
			float PlyZ = ReadF(VertexData, OffZ, 0.0f);
			Splat.Position.X = PlyZ * MetersToUE;    // PLY Z -> UE X (forward)
			Splat.Position.Y = PlyX * MetersToUE;    // PLY X -> UE Y (right)
			Splat.Position.Z = -PlyY * MetersToUE;   // PLY -Y -> UE Z (up, negated because PLY Y points down)

			// Rotation (quaternion) - Convert coordinate system
			// PLY uses (w, x, y, z) format with Y-down (COLMAP convention)
			// Pattern: when position axis is NOT negated, quaternion component IS negated (and vice versa)
			// Position: PLY.X -> UE.Y (not negated), PLY.Y -> UE.-Z (negated), PLY.Z -> UE.X (not negated)
			float QW = ReadF(VertexData, OffRot[0], 1.0f); // identity w
			float QX = ReadF(VertexData, OffRot[1], 0.0f);
			float QY = ReadF(VertexData, OffRot[2], 0.0f);
			float QZ = ReadF(VertexData, OffRot[3], 0.0f);
			Splat.Rotation.W = QW;
			Splat.Rotation.X = -QZ;   // PLY Z -> UE X (negated: position not negated)
			Splat.Rotation.Y = -QX;   // PLY X -> UE Y (negated: position not negated)
			Splat.Rotation.Z = QY;    // PLY Y -> UE Z (not negated: position was negated)

			// Scale - Reorder to match coordinate system conversion
			// Scale is always positive magnitude, no negation needed
			float ScaleX = ReadF(VertexData, OffScale[0], -4.5f); // ~1cm default
			float ScaleY = ReadF(VertexData, OffScale[1], -4.5f);
			float ScaleZ = ReadF(VertexData, OffScale[2], -4.5f);
			Splat.Scale.X = ScaleZ;  // PLY Z -> UE X
			Splat.Scale.Y = ScaleX;  // PLY X -> UE Y
			Splat.Scale.Z = ScaleY;  // PLY Y -> UE Z

			// Opacity
			Splat.Opacity = ReadF(VertexData, OffOpacity, 10.0f); // ≈1.0 after sigmoid

			// SH DC (base color) — fall back to red/green/blue if f_dc_*
			// isn't present (standard COLMAP point cloud PLY), then to white.
			if (bHasDC)
			{
				Splat.SH_DC.X = ReadF(VertexData, OffDC[0], 0.0f);
				Splat.SH_DC.Y = ReadF(VertexData, OffDC[1], 0.0f);
				Splat.SH_DC.Z = ReadF(VertexData, OffDC[2], 0.0f);
			}
			else if (Header.PropertyOffsets.Contains(TEXT("red")))
			{
				// Normalize from [0,255] to [0,1] (may be uchar — type-aware read)
				Splat.SH_DC.X = GetPropertyFloat(VertexData, Header, TEXT("red")) / 255.0f;
				Splat.SH_DC.Y = GetPropertyFloat(VertexData, Header, TEXT("green")) / 255.0f;
				Splat.SH_DC.Z = GetPropertyFloat(VertexData, Header, TEXT("blue")) / 255.0f;
			}
			else if (Header.PropertyOffsets.Contains(TEXT("r")))
			{
				Splat.SH_DC.X = GetPropertyFloat(VertexData, Header, TEXT("r")) / 255.0f;
				Splat.SH_DC.Y = GetPropertyFloat(VertexData, Header, TEXT("g")) / 255.0f;
				Splat.SH_DC.Z = GetPropertyFloat(VertexData, Header, TEXT("b")) / 255.0f;
			}
			else
			{
				// White default
				Splat.SH_DC.X = 1.0f;
				Splat.SH_DC.Y = 1.0f;
				Splat.SH_DC.Z = 1.0f;
			}

			// SH rest coefficients (bands 1-3), planar layout via cached offsets
			for (int32 c = 0; c < GaussianSplattingConstants::NumSHCoefficients; c++)
			{
				if (c < CoeffsPerChannel)
				{
					Splat.SH[c].X = ReadF(VertexData, OffRest[0][c], 0.0f);
					Splat.SH[c].Y = ReadF(VertexData, OffRest[1][c], 0.0f);
					Splat.SH[c].Z = ReadF(VertexData, OffRest[2][c], 0.0f);
				}
				else
				{
					// Zero out coefficients beyond what the file contains
					Splat.SH[c] = FVector3f::ZeroVector;
				}
			}

			// 4D temporal properties (optional; defaults keep splats static)
			// NOTE: t stays in PLY time units (e.g. normalized [-1,1]); scale_t is
			// log-encoded like spatial scales but is in TIME units (no meter conversion).
			if (bHasTemporal)
			{
				if (OffT >= 0)
				{
					Splat.AnchorTime = ReadF(VertexData, OffT, 0.0f);
				}
				if (OffScaleT >= 0)
				{
					const float ScaleT = ReadF(VertexData, OffScaleT, 23.0f);
					Splat.TimeSigma = FMath::Exp(ScaleT); // ~1e10 default when absent
				}

				// Linear velocity: SpacetimeGaussians stores a 9-term polynomial
				// motion (motion_0..8); the first-order term motion_0..2 is the
				// velocity. pos(t) = pos0 + velocity * (t - anchor). Units match
				// the raw PLY position (meters) and time (asset time units), so
				// no conversion is applied here.
				Splat.Velocity.X = GetPropertyFloat(VertexData, Header, TEXT("motion_0"), 0.0f);
				Splat.Velocity.Y = GetPropertyFloat(VertexData, Header, TEXT("motion_1"), 0.0f);
				Splat.Velocity.Z = GetPropertyFloat(VertexData, Header, TEXT("motion_2"), 0.0f);
			}

			// Linearize the data
			LinearizeSplatData(Splat);

			VertexIndex++;
		}

		VerticesRemaining -= VerticesToRead;

		// Log progress for large files
		if (Header.VertexCount > 1000000 && VertexIndex % (Header.VertexCount / 10) < ChunkSize)
		{
			UE_LOG(LogTemp, Log, TEXT("  Reading PLY vertices: %d / %d (%.0f%%)"),
				VertexIndex, Header.VertexCount, 100.0f * VertexIndex / Header.VertexCount);
		}
	}

	return true;
}

void FPLYFileReader::LinearizeSplatData(FGaussianSplatData& Splat)
{
	// Normalize quaternion
	Splat.Rotation = GaussianSplattingUtils::NormalizeQuat(Splat.Rotation);

	// Apply exp to scale (PLY stores log-scale)
	// Then multiply by 100 to convert from meters (PLY) to centimeters (UE)
	constexpr float MetersToUE = 100.0f;
	Splat.Scale.X = FMath::Exp(Splat.Scale.X) * MetersToUE;
	Splat.Scale.Y = FMath::Exp(Splat.Scale.Y) * MetersToUE;
	Splat.Scale.Z = FMath::Exp(Splat.Scale.Z) * MetersToUE;

	// Apply sigmoid to opacity
	Splat.Opacity = GaussianSplattingUtils::Sigmoid(Splat.Opacity);
}

float FPLYFileReader::GetPropertyFloat(const uint8* VertexData, const FPLYHeader& Header, const FString& PropertyName, float DefaultValue)
{
	const int32* OffsetPtr = Header.PropertyOffsets.Find(PropertyName);
	if (!OffsetPtr)
	{
		return DefaultValue;
	}

	const int32 Offset = *OffsetPtr;
	const int32* SizePtr = Header.PropertySizes.Find(PropertyName);
	const int32 PropSize = SizePtr ? *SizePtr : 4;

	// NEVER read past the end of the vertex record: doing so for the last
	// vertex in a chunk overruns the chunk buffer (access violation), which is
	// exactly what happens when a file mixes float properties with uchar ones
	// and every property is blindly read as a 4-byte float.
	if (Offset + PropSize > Header.VertexStride)
	{
		return DefaultValue;
	}

	switch (PropSize)
	{
	case 1:
		return static_cast<float>(*reinterpret_cast<const uint8*>(VertexData + Offset));
	case 2:
		return static_cast<float>(*reinterpret_cast<const int16*>(VertexData + Offset));
	case 8:
		return static_cast<float>(*reinterpret_cast<const double*>(VertexData + Offset));
	default:
		return *reinterpret_cast<const float*>(VertexData + Offset);
	}
}
