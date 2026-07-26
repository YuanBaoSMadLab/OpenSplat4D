// Copyright Epic Games, Inc. All Rights Reserved.

#include "PLYFileReader.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"

bool FPLYFileReader::ReadPLYFile(const FString& FilePath, TArray<FGaussianSplatData>& OutSplats, FString& OutError, int32* OutSHBands)
{
	OutSplats.Empty();

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

	// Validate file size against expected data
	const int64 ExpectedEnd = Header.DataOffset + static_cast<int64>(Header.VertexCount) * Header.VertexStride;
	if (ExpectedEnd > FileSize)
	{
		OutError = FString::Printf(TEXT("File truncated: expected %lld bytes of vertex data, file size is %lld"),
			ExpectedEnd, FileSize);
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
			TrimmedLine.ParseIntoArray(Parts, TEXT(" "));
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
			TrimmedLine.ParseIntoArray(Parts, TEXT(" "));

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
				else if (Type == TEXT("uchar") || Type == TEXT("uint8"))
				{
					TypeSize = 1;
				}
				else if (Type == TEXT("int") || Type == TEXT("int32"))
				{
					TypeSize = 4;
				}
				else if (Type == TEXT("short") || Type == TEXT("int16"))
				{
					TypeSize = 2;
				}

				OutHeader.PropertyNames.Add(Name);
				OutHeader.PropertyOffsets.Add(Name, CurrentOffset);
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

	OutHeader.VertexStride = CurrentOffset;

	// Verify we have required properties
	TArray<FString> RequiredProps = { TEXT("x"), TEXT("y"), TEXT("z"), TEXT("opacity"),
		TEXT("scale_0"), TEXT("scale_1"), TEXT("scale_2"),
		TEXT("rot_0"), TEXT("rot_1"), TEXT("rot_2"), TEXT("rot_3") };

	for (const FString& Prop : RequiredProps)
	{
		if (!OutHeader.PropertyOffsets.Contains(Prop))
		{
			OutError = FString::Printf(TEXT("Missing required property: %s"), *Prop);
			return false;
		}
	}

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
			float PlyX = GetPropertyFloat(VertexData, Header, TEXT("x"));
			float PlyY = GetPropertyFloat(VertexData, Header, TEXT("y"));
			float PlyZ = GetPropertyFloat(VertexData, Header, TEXT("z"));
			Splat.Position.X = PlyZ * MetersToUE;    // PLY Z -> UE X (forward)
			Splat.Position.Y = PlyX * MetersToUE;    // PLY X -> UE Y (right)
			Splat.Position.Z = -PlyY * MetersToUE;   // PLY -Y -> UE Z (up, negated because PLY Y points down)

			// Rotation (quaternion) - Convert coordinate system
			// PLY uses (w, x, y, z) format with Y-down (COLMAP convention)
			// Pattern: when position axis is NOT negated, quaternion component IS negated (and vice versa)
			// Position: PLY.X -> UE.Y (not negated), PLY.Y -> UE.-Z (negated), PLY.Z -> UE.X (not negated)
			float QW = GetPropertyFloat(VertexData, Header, TEXT("rot_0"));
			float QX = GetPropertyFloat(VertexData, Header, TEXT("rot_1"));
			float QY = GetPropertyFloat(VertexData, Header, TEXT("rot_2"));
			float QZ = GetPropertyFloat(VertexData, Header, TEXT("rot_3"));
			Splat.Rotation.W = QW;
			Splat.Rotation.X = -QZ;   // PLY Z -> UE X (negated: position not negated)
			Splat.Rotation.Y = -QX;   // PLY X -> UE Y (negated: position not negated)
			Splat.Rotation.Z = QY;    // PLY Y -> UE Z (not negated: position was negated)

			// Scale - Reorder to match coordinate system conversion
			// Scale is always positive magnitude, no negation needed
			float ScaleX = GetPropertyFloat(VertexData, Header, TEXT("scale_0"));
			float ScaleY = GetPropertyFloat(VertexData, Header, TEXT("scale_1"));
			float ScaleZ = GetPropertyFloat(VertexData, Header, TEXT("scale_2"));
			Splat.Scale.X = ScaleZ;  // PLY Z -> UE X
			Splat.Scale.Y = ScaleX;  // PLY X -> UE Y
			Splat.Scale.Z = ScaleY;  // PLY Y -> UE Z

			// Opacity
			Splat.Opacity = GetPropertyFloat(VertexData, Header, TEXT("opacity"));

			// SH DC (base color)
			Splat.SH_DC.X = GetPropertyFloat(VertexData, Header, TEXT("f_dc_0"));
			Splat.SH_DC.Y = GetPropertyFloat(VertexData, Header, TEXT("f_dc_1"));
			Splat.SH_DC.Z = GetPropertyFloat(VertexData, Header, TEXT("f_dc_2"));

			// SH rest coefficients (bands 1-3)
			// Uses CoeffsPerChannel detected earlier (outside the loop)
			for (int32 c = 0; c < GaussianSplattingConstants::NumSHCoefficients; c++)
			{
				if (c < CoeffsPerChannel)
				{
					// Read from correct planar positions
					FString PropNameR = FString::Printf(TEXT("f_rest_%d"), c);
					FString PropNameG = FString::Printf(TEXT("f_rest_%d"), c + CoeffsPerChannel);
					FString PropNameB = FString::Printf(TEXT("f_rest_%d"), c + CoeffsPerChannel * 2);

					Splat.SH[c].X = GetPropertyFloat(VertexData, Header, PropNameR);
					Splat.SH[c].Y = GetPropertyFloat(VertexData, Header, PropNameG);
					Splat.SH[c].Z = GetPropertyFloat(VertexData, Header, PropNameB);
				}
				else
				{
					// Zero out coefficients beyond what the file contains
					Splat.SH[c] = FVector3f::ZeroVector;
				}
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

	// Assuming all properties are float (which is standard for 3DGS PLY files)
	const float* ValuePtr = reinterpret_cast<const float*>(VertexData + *OffsetPtr);
	return *ValuePtr;
}
