// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatAsset.h"
#include "GaussianSplatRenderData.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "GaussianClusterBuilder.h"
#include "PLYFileReader.h"
#include "Misc/FileHelper.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"

#if WITH_EDITOR
#include "Misc/ScopedSlowTask.h"
#endif

UGaussianSplatAsset::UGaussianSplatAsset()
{
	BoundingBox.Init();
}

TSharedPtr<FGaussianSplatRenderData> UGaussianSplatAsset::GetOrCreateRenderData()
{
	if (RenderData.IsValid() && RenderData->IsInitialized())
	{
		UE_LOG(LogTemp, Verbose, TEXT("GaussianSplatRenderData: Reusing existing shared data for asset '%s'"),
			*GetName());
		return RenderData;
	}

	RenderData = MakeShared<FGaussianSplatRenderData>();
	RenderData->Initialize(this);
	return RenderData;
}

void UGaussianSplatAsset::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	int32 Magic = GAUSSIAN_SPLAT_ASSET_MAGIC;
	int32 Version = GAUSSIAN_SPLAT_ASSET_VERSION;
	Ar << Magic;
	Ar << Version;

	if (Ar.IsLoading() && (Magic != GAUSSIAN_SPLAT_ASSET_MAGIC || Version > GAUSSIAN_SPLAT_ASSET_VERSION || Version < 5))
	{
		UE_LOG(LogTemp, Error, TEXT("GaussianSplatAsset: Incompatible asset format (Magic=0x%08X, Version=%d). Please reimport the asset."), Magic, Version);
		// Explicitly reset state so the asset fails IsValid() cleanly instead of
		// being left in a half-loaded state with stale SplatCount / dangling BulkData.
		SplatCount = 0;
		OriginalSplatCount = 0;
		BoundingBox.Init();
		SHBands = 0;
		ClusterHierarchy.Reset();
		return;
	}

	Ar << SplatCount;
	Ar << BoundingBox;
	Ar << PositionFormat;
	Ar << ColorFormat;
	Ar << SHFormat;
	Ar << SHBands;
	Ar << SourceFilePath;
	Ar << ImportQuality;
	Ar << ColorTextureWidth;
	Ar << ColorTextureHeight;
	Ar << ChunkData;

	PositionBulkData.Serialize(Ar, this);
	OtherBulkData.Serialize(Ar, this);
	SHBulkData.Serialize(Ar, this);
	ColorTextureBulkData.Serialize(Ar, this);

	// Nanite-related fields (Version 4+)
	Ar << bEnableNanite;
	Ar << OriginalSplatCount;
	Ar << ClusterHierarchy;

	// 4D temporal fields (Version 6+; v5 assets keep defaults bIs4D=false)
	if (Version >= 6)
	{
		Ar << bIs4D;
		Ar << TimeStart;
		Ar << TimeEnd;
		TemporalBulkData.Serialize(Ar, this);
	}
	else if (Ar.IsLoading())
	{
		bIs4D = false;
		TimeStart = 0.f;
		TimeEnd = 0.f;
	}

	// Keyframe 4D fields (Version 7+; v5/v6 assets keep defaults)
	if (Version >= 7)
	{
		Ar << bIsKeyframe4D;
		Ar << KeyframeCount;
		KeyframeBulkData.Serialize(Ar, this);
	}
	else if (Ar.IsLoading())
	{
		bIsKeyframe4D = false;
		KeyframeCount = 0;
	}
}

void UGaussianSplatAsset::PostLoad()
{
	Super::PostLoad();

	const int64 ColorDataSize = ColorTextureBulkData.GetBulkDataSize();
	UE_LOG(LogTemp, Log, TEXT("GaussianSplatAsset::PostLoad - SplatCount=%d, ColorTextureBulkData.Size=%lld, Width=%d, Height=%d"),
		SplatCount, ColorDataSize, ColorTextureWidth, ColorTextureHeight);

	// Recreate the ColorTexture from the stored bulk data
	if (ColorDataSize > 0 && ColorTextureWidth > 0 && ColorTextureHeight > 0)
	{
		CreateColorTextureFromData();
		UE_LOG(LogTemp, Log, TEXT("GaussianSplatAsset::PostLoad - ColorTexture recreated successfully"));
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("GaussianSplatAsset::PostLoad - No ColorTextureBulkData to restore (might be old asset format)"));
	}

#if WITH_EDITORONLY_DATA
	// Recreate the ThumbnailTexture from stored pixel data
	if (ThumbnailData.Num() > 0 && ThumbnailSize > 0)
	{
		CreateThumbnailTextureFromData();
		UE_LOG(LogTemp, Log, TEXT("GaussianSplatAsset::PostLoad - ThumbnailTexture recreated successfully"));
	}
#endif
}

#if WITH_EDITOR
void UGaussianSplatAsset::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// The Details-panel checkbox must be a REAL toggle: enabling builds the
	// cluster hierarchy (same as the context-menu action); disabling clears it.
	// On build failure the flag is reverted so the checkbox never lies.
	if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UGaussianSplatAsset, bEnableNanite))
	{
		if (bEnableNanite)
		{
			if (!ClusterHierarchy.IsValid() && SplatCount > 0)
			{
				if (!BuildNaniteClusterHierarchy())
				{
					bEnableNanite = false;
					UE_LOG(LogTemp, Warning,
						TEXT("Enable Nanite failed for asset %s: cannot build cluster hierarchy (missing or invalid source PLY). Flag reverted."),
						*GetName());
				}
			}
		}
		else if (ClusterHierarchy.IsValid())
		{
			ClearNaniteClusterHierarchy();
		}
	}
}
#endif

int64 UGaussianSplatAsset::GetMemoryUsage() const
{
	int64 TotalBytes = 0;

	TotalBytes += PositionBulkData.GetBulkDataSize();
	TotalBytes += OtherBulkData.GetBulkDataSize();
	TotalBytes += SHBulkData.GetBulkDataSize();
	TotalBytes += ChunkData.Num() * sizeof(FGaussianChunkInfo);
	TotalBytes += ColorTextureBulkData.GetBulkDataSize();
	TotalBytes += TemporalBulkData.GetBulkDataSize();
	TotalBytes += KeyframeBulkData.GetBulkDataSize();

	if (ColorTexture)
	{
		TotalBytes += ColorTexture->CalcTextureMemorySizeEnum(TMC_ResidentMips);
	}

	// Cluster hierarchy memory
	if (ClusterHierarchy.IsValid())
	{
		TotalBytes += ClusterHierarchy.Clusters.Num() * sizeof(FGaussianCluster);
		// Account for dynamic arrays within clusters (ChildClusterIDs)
		for (const FGaussianCluster& Cluster : ClusterHierarchy.Clusters)
		{
			TotalBytes += Cluster.ChildClusterIDs.Num() * sizeof(uint32);
		}
	}

	return TotalBytes;
}

void UGaussianSplatAsset::InitializeFromSplatData(const TArray<FGaussianSplatData>& InSplats, EGaussianQualityLevel InQuality)
{
	SplatCount = InSplats.Num();
	ImportQuality = InQuality;

	if (SplatCount == 0)
	{
		return;
	}

	// Simplified format: Always use Float32 for positions (most reliable)
	// This avoids complex quantization/dequantization that can cause precision issues
	PositionFormat = EGaussianPositionFormat::Float32;
	ColorFormat = EGaussianColorFormat::Float16x4;  // Good balance for colors
	SHFormat = EGaussianSHFormat::Float16;          // Good balance for SH

	// Calculate bounds (still useful for culling)
	CalculateBounds(InSplats);

	// Store position data (always Float32 now)
	CompressPositions(InSplats);
	CompressRotationScale(InSplats);
	CreateColorTextureData(InSplats);  // Store raw data for serialization
	CreateColorTextureFromData();       // Create the runtime texture
	CompressSH(InSplats);

	// ---- 4D temporal data: build if any splat carries temporal info ----
	{
		bool bHasTemporal = false;
		float MinT = FLT_MAX, MaxT = -FLT_MAX;
		for (const FGaussianSplatData& Splat : InSplats)
		{
			// A gaussian is temporal if its sigma is finite (well below the
			// "static" sentinel 1e10) -- i.e. the PLY had t/scale_t properties.
			if (Splat.TimeSigma < 1e9f)
			{
				bHasTemporal = true;
				MinT = FMath::Min(MinT, Splat.AnchorTime);
				MaxT = FMath::Max(MaxT, Splat.AnchorTime);
			}
		}

		if (bHasTemporal)
		{
			bIs4D = true;
			TimeStart = MinT;
			TimeEnd = MaxT;

			TArray<uint8> Temporal;
			Temporal.SetNumUninitialized(SplatCount * TemporalStride);
			uint32* Ptr = reinterpret_cast<uint32*>(Temporal.GetData());
			for (int32 i = 0; i < SplatCount; i++)
			{
				const FGaussianSplatData& Splat = InSplats[i];
				Ptr[i * 4 + 0] = *reinterpret_cast<const uint32*>(&Splat.AnchorTime);
				Ptr[i * 4 + 1] = *reinterpret_cast<const uint32*>(&Splat.TimeSigma);
				Ptr[i * 4 + 2] = 0; // reserved (future: velocity XY, half2)
				Ptr[i * 4 + 3] = 0; // reserved (future: velocity Z + flag, half2)
			}

			TemporalBulkData.Lock(LOCK_READ_WRITE);
			void* Dest = TemporalBulkData.Realloc(Temporal.Num());
			FMemory::Memcpy(Dest, Temporal.GetData(), Temporal.Num());
			TemporalBulkData.Unlock();

			UE_LOG(LogTemp, Log, TEXT("GaussianSplatAsset: 4D temporal data built (%d splats, time range [%.4f, %.4f])"),
				SplatCount, TimeStart, TimeEnd);
		}
		else
		{
			bIs4D = false;
		}
	}

#if WITH_EDITOR
	GenerateThumbnail(InSplats);
#endif

	UE_LOG(LogTemp, Log, TEXT("GaussianSplatAsset: Initialized with %d splats, memory: %lld bytes"),
		SplatCount, GetMemoryUsage());
}

int32 UGaussianSplatAsset::GetPositionBytesPerSplat(EGaussianPositionFormat Format)
{
	switch (Format)
	{
	case EGaussianPositionFormat::Float32: return 12; // 3 x 4 bytes
	case EGaussianPositionFormat::Norm16:  return 6;  // 3 x 2 bytes
	case EGaussianPositionFormat::Norm11:  return 4;  // 11+10+11 bits packed
	case EGaussianPositionFormat::Norm6:   return 2;  // 6+5+5 bits packed
	default: return 12;
	}
}

int32 UGaussianSplatAsset::GetColorBytesPerSplat(EGaussianColorFormat Format)
{
	switch (Format)
	{
	case EGaussianColorFormat::Float32x4: return 16; // 4 x 4 bytes
	case EGaussianColorFormat::Float16x4: return 8;  // 4 x 2 bytes
	case EGaussianColorFormat::Norm8x4:   return 4;  // 4 x 1 byte
	case EGaussianColorFormat::BC7:       return 1;  // Approximate
	default: return 8;
	}
}

int32 UGaussianSplatAsset::GetSHBytesPerSplat(EGaussianSHFormat Format, int32 Bands)
{
	// SH buffer now includes DC coefficient for view-dependent rendering
	// band0=0 (no buffer), band1=4 (DC + 3), band2=9 (DC + 8), band3=16 (DC + 15)
	int32 NumCoeffs = 0;
	switch (Bands)
	{
	case 0: NumCoeffs = 0; break;   // DC only (stored in color, no SH buffer)
	case 1: NumCoeffs = 4; break;   // DC + 3 coeffs
	case 2: NumCoeffs = 9; break;   // DC + 3 + 5 coeffs
	case 3: NumCoeffs = 16; break;  // DC + 3 + 5 + 7 coeffs
	default: NumCoeffs = 16; break;
	}

	// Each coefficient has 3 color channels
	int32 TotalValues = NumCoeffs * 3;

	switch (Format)
	{
	case EGaussianSHFormat::Float32: return TotalValues * 4;
	case EGaussianSHFormat::Float16: return TotalValues * 2;
	case EGaussianSHFormat::Norm11:  return (TotalValues * 11 + 7) / 8; // Approximate
	case EGaussianSHFormat::Norm6:   return (TotalValues * 6 + 7) / 8;  // Approximate
	default: return TotalValues * 2; // Assume Float16
	}
}

void UGaussianSplatAsset::CalculateBounds(const TArray<FGaussianSplatData>& InSplats)
{
	BoundingBox.Init();

	for (const FGaussianSplatData& Splat : InSplats)
	{
		BoundingBox += FVector(Splat.Position);
	}
}

TArray<FVector> UGaussianSplatAsset::GetDecompressedPositions() const
{
	TArray<FVector> Positions;

	const int64 DataSize = PositionBulkData.GetBulkDataSize();
	if (SplatCount == 0 || DataSize == 0)
	{
		return Positions;
	}

	Positions.SetNum(SplatCount);

	// Lock bulk data for reading. ON_SCOPE_EXIT guarantees Unlock() runs even
	// if anything below returns early or throws — without it, a stuck lock
	// would prevent future serialize/deserialize of this asset.
	const uint8* DataPtr = static_cast<const uint8*>(PositionBulkData.LockReadOnly());
	ON_SCOPE_EXIT
	{
		PositionBulkData.Unlock();
	};

	// Simplified: Always Float32 format (12 bytes per position)
	const int32 BytesPerSplat = 12;

	// Defensive: make sure DataPtr is valid before dereferencing.
	if (!DataPtr)
	{
		UE_LOG(LogTemp, Error, TEXT("GaussianSplatAsset: PositionBulkData.LockReadOnly returned null"));
		Positions.Reset();
		return Positions;
	}

	for (int32 i = 0; i < SplatCount; i++)
	{
		const float* FloatPtr = reinterpret_cast<const float*>(DataPtr + i * BytesPerSplat);
		Positions[i] = FVector(FloatPtr[0], FloatPtr[1], FloatPtr[2]);
	}

	return Positions;
}

void UGaussianSplatAsset::CalculateChunkBounds(const TArray<FGaussianSplatData>& InSplats)
{
	const int32 NumChunks = FMath::DivideAndRoundUp(SplatCount, GaussianSplattingConstants::SplatsPerChunk);
	ChunkData.SetNum(NumChunks);

	for (int32 ChunkIdx = 0; ChunkIdx < NumChunks; ChunkIdx++)
	{
		const int32 StartIdx = ChunkIdx * GaussianSplattingConstants::SplatsPerChunk;
		const int32 EndIdx = FMath::Min(StartIdx + GaussianSplattingConstants::SplatsPerChunk, SplatCount);

		FGaussianChunkInfo& Chunk = ChunkData[ChunkIdx];

		// Initialize with first splat values
		if (StartIdx < InSplats.Num())
		{
			const FGaussianSplatData& First = InSplats[StartIdx];
			Chunk.PosMinMaxX = FVector2f(First.Position.X, First.Position.X);
			Chunk.PosMinMaxY = FVector2f(First.Position.Y, First.Position.Y);
			Chunk.PosMinMaxZ = FVector2f(First.Position.Z, First.Position.Z);
		}

		// Find min/max for this chunk
		for (int32 i = StartIdx; i < EndIdx; i++)
		{
			const FGaussianSplatData& Splat = InSplats[i];

			Chunk.PosMinMaxX.X = FMath::Min(Chunk.PosMinMaxX.X, Splat.Position.X);
			Chunk.PosMinMaxX.Y = FMath::Max(Chunk.PosMinMaxX.Y, Splat.Position.X);
			Chunk.PosMinMaxY.X = FMath::Min(Chunk.PosMinMaxY.X, Splat.Position.Y);
			Chunk.PosMinMaxY.Y = FMath::Max(Chunk.PosMinMaxY.Y, Splat.Position.Y);
			Chunk.PosMinMaxZ.X = FMath::Min(Chunk.PosMinMaxZ.X, Splat.Position.Z);
			Chunk.PosMinMaxZ.Y = FMath::Max(Chunk.PosMinMaxZ.Y, Splat.Position.Z);
		}
	}
}

void UGaussianSplatAsset::CompressPositions(const TArray<FGaussianSplatData>& InSplats)
{
	// Simplified: Always use Float32 format (12 bytes per position)
	const int32 BytesPerSplat = 12;
	const int32 TotalBytes = SplatCount * BytesPerSplat;

	// Lock bulk data for writing
	PositionBulkData.Lock(LOCK_READ_WRITE);
	uint8* DataPtr = static_cast<uint8*>(PositionBulkData.Realloc(TotalBytes));

	for (int32 i = 0; i < SplatCount; i++)
	{
		const FGaussianSplatData& Splat = InSplats[i];
		float* FloatPtr = reinterpret_cast<float*>(DataPtr + i * BytesPerSplat);
		FloatPtr[0] = Splat.Position.X;
		FloatPtr[1] = Splat.Position.Y;
		FloatPtr[2] = Splat.Position.Z;
	}

	PositionBulkData.Unlock();

	// Set bulk data flags for optimal storage
	PositionBulkData.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload);
}

void UGaussianSplatAsset::CompressRotationScale(const TArray<FGaussianSplatData>& InSplats)
{
	// Rotation: 4 floats (quaternion) = 16 bytes or packed to 4 bytes (10.10.10.2)
	// Scale: 3 floats = 12 bytes or packed
	// For now, use full precision: 28 bytes per splat

	const int32 BytesPerSplat = 28; // 16 (quat) + 12 (scale)
	const int32 TotalBytes = SplatCount * BytesPerSplat;

	// Lock bulk data for writing
	OtherBulkData.Lock(LOCK_READ_WRITE);
	uint8* DataPtr = static_cast<uint8*>(OtherBulkData.Realloc(TotalBytes));

	for (int32 i = 0; i < SplatCount; i++)
	{
		const FGaussianSplatData& Splat = InSplats[i];
		float* FloatPtr = reinterpret_cast<float*>(DataPtr + i * BytesPerSplat);

		// Quaternion (normalized)
		FQuat4f NormQuat = GaussianSplattingUtils::NormalizeQuat(Splat.Rotation);
		FloatPtr[0] = NormQuat.X;
		FloatPtr[1] = NormQuat.Y;
		FloatPtr[2] = NormQuat.Z;
		FloatPtr[3] = NormQuat.W;

		// Scale
		FloatPtr[4] = Splat.Scale.X;
		FloatPtr[5] = Splat.Scale.Y;
		FloatPtr[6] = Splat.Scale.Z;
	}

	OtherBulkData.Unlock();

	// Set bulk data flags for optimal storage
	OtherBulkData.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload);
}

void UGaussianSplatAsset::CreateColorTextureData(const TArray<FGaussianSplatData>& InSplats)
{
	// Store texture dimensions
	ColorTextureWidth = GaussianSplattingConstants::ColorTextureWidth;
	const int32 TileSize = GaussianSplattingConstants::MortonTileSize; // 16
	const int32 EntriesPerTile = TileSize * TileSize; // 256
	const int32 TilesPerRow = ColorTextureWidth / TileSize; // 128
	int32 NumTiles = FMath::DivideAndRoundUp(SplatCount, EntriesPerTile);
	int32 TileRows = FMath::DivideAndRoundUp(NumTiles, TilesPerRow);
	ColorTextureHeight = TileRows * TileSize;

	UE_LOG(LogTemp, Log, TEXT("CreateColorTextureData: Creating %dx%d data for %d splats"),
		ColorTextureWidth, ColorTextureHeight, SplatCount);

	// Allocate raw pixel data (FFloat16Color = 8 bytes per pixel: R16G16B16A16)
	const int32 NumBytes = ColorTextureWidth * ColorTextureHeight * sizeof(FFloat16Color);

	// Lock bulk data for writing
	ColorTextureBulkData.Lock(LOCK_READ_WRITE);
	FFloat16Color* PixelData = reinterpret_cast<FFloat16Color*>(ColorTextureBulkData.Realloc(NumBytes));

	// Initialize to black
	FMemory::Memzero(PixelData, NumBytes);

	// Fill with color data using Morton swizzling
	for (int32 i = 0; i < SplatCount; i++)
	{
		const FGaussianSplatData& Splat = InSplats[i];

		// Calculate Morton-swizzled texture coordinate
		int32 TexX, TexY;
		GaussianSplattingUtils::SplatIndexToTextureCoord(i, ColorTextureWidth, TexX, TexY);

		if (TexY < ColorTextureHeight)
		{
			// Convert SH DC to color
			// IMPORTANT: Do NOT clamp RGB - SH coefficients can produce values outside [0,1]
			// Float16 texture format can store HDR values, clamping would cause color errors
			FVector3f Color = GaussianSplattingUtils::SHDCToColor(Splat.SH_DC);

			FFloat16Color& Pixel = PixelData[TexY * ColorTextureWidth + TexX];
			Pixel.R = FFloat16(Color.X);  // No clamping - allow HDR values
			Pixel.G = FFloat16(Color.Y);
			Pixel.B = FFloat16(Color.Z);
			Pixel.A = FFloat16(FMath::Clamp(Splat.Opacity, 0.0f, 1.0f));  // Opacity is always [0,1]
		}
	}

	ColorTextureBulkData.Unlock();

	// Set bulk data flags for optimal storage
	ColorTextureBulkData.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload);

	UE_LOG(LogTemp, Log, TEXT("CreateColorTextureData: Stored %d bytes of pixel data"), NumBytes);
}

void UGaussianSplatAsset::CreateColorTextureFromData()
{
	const int64 BulkDataSize = ColorTextureBulkData.GetBulkDataSize();
	if (BulkDataSize == 0 || ColorTextureWidth <= 0 || ColorTextureHeight <= 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("CreateColorTextureFromData: No data to create texture from"));
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("CreateColorTextureFromData: Creating %dx%d texture from stored bulk data"),
		ColorTextureWidth, ColorTextureHeight);

	// Create texture - use Transient flag since this is recreated at runtime
	ColorTexture = NewObject<UTexture2D>(this, NAME_None, RF_Transient);
	UTexture2D* Tex = ColorTexture;

	// Initialize platform data
	FTexturePlatformData* PlatformData = new FTexturePlatformData();
	PlatformData->SizeX = ColorTextureWidth;
	PlatformData->SizeY = ColorTextureHeight;
	PlatformData->PixelFormat = PF_FloatRGBA;
	Tex->SetPlatformData(PlatformData);

	// Configure texture settings
	Tex->SRGB = false;
	Tex->CompressionSettings = TC_HDR;
	Tex->Filter = TF_Nearest;
	Tex->AddressX = TA_Clamp;
	Tex->AddressY = TA_Clamp;
	Tex->NeverStream = true;
	Tex->LODGroup = TEXTUREGROUP_Pixels2D;
#if WITH_EDITORONLY_DATA
	Tex->MipGenSettings = TMGS_NoMipmaps;
#endif

	// Create mip 0
	FTexture2DMipMap* Mip = new FTexture2DMipMap();
	PlatformData->Mips.Add(Mip);
	Mip->SizeX = ColorTextureWidth;
	Mip->SizeY = ColorTextureHeight;

	// Lock bulk data for reading and copy to mip
	const void* SrcData = ColorTextureBulkData.LockReadOnly();
	const int32 NumBytes = static_cast<int32>(BulkDataSize);

	Mip->BulkData.Lock(LOCK_READ_WRITE);
	void* MipData = Mip->BulkData.Realloc(NumBytes);
	FMemory::Memcpy(MipData, SrcData, NumBytes);
	Mip->BulkData.Unlock();

	ColorTextureBulkData.Unlock();

	// Create the GPU resource
	Tex->UpdateResource();

	UE_LOG(LogTemp, Log, TEXT("CreateColorTextureFromData: Texture created and resource updated"));
}

void UGaussianSplatAsset::CompressSH(const TArray<FGaussianSplatData>& InSplats)
{
	if (SHBands == 0)
	{
		// No additional SH data needed (DC stored in color texture)
		// Clear any existing bulk data
		SHBulkData.RemoveBulkData();
		return;
	}

	// Debug: Check if any SH coefficients are non-zero
	int32 NonZeroSHCount = 0;
	float MaxSHValue = 0.0f;
	for (int32 i = 0; i < FMath::Min(InSplats.Num(), 100); i++)  // Check first 100 splats
	{
		const FGaussianSplatData& Splat = InSplats[i];
		for (int32 c = 0; c < 15; c++)
		{
			float Mag = FMath::Abs(Splat.SH[c].X) + FMath::Abs(Splat.SH[c].Y) + FMath::Abs(Splat.SH[c].Z);
			if (Mag > 0.0001f)
			{
				NonZeroSHCount++;
				MaxSHValue = FMath::Max(MaxSHValue, Mag);
			}
		}
	}
	UE_LOG(LogTemp, Log, TEXT("CompressSH: SHBands=%d, SplatCount=%d, InSplats.Num()=%d, NonZeroSH(first100)=%d, MaxSHValue=%.6f"),
		SHBands, SplatCount, InSplats.Num(), NonZeroSHCount, MaxSHValue);

	// Higher-order coefficients: 3 for band1, 8 for band1+2, 15 for band1+2+3
	const int32 NumHigherCoeffs = (SHBands == 1) ? 3 : (SHBands == 2) ? 8 : 15;

	// Total coefficients INCLUDING DC (SH_DC stored first for view-dependent rendering)
	const int32 TotalCoeffs = NumHigherCoeffs + 1;  // +1 for DC

	// Always store as Float16 for now (TODO: implement Norm11/Norm6 compression)
	// Calculate actual bytes needed: TotalCoeffs * 3 channels * 2 bytes per FFloat16
	const int32 BytesPerSplat = TotalCoeffs * 3 * sizeof(FFloat16);
	const int32 TotalBytes = SplatCount * BytesPerSplat;

	// Lock bulk data for writing
	SHBulkData.Lock(LOCK_READ_WRITE);
	FFloat16* HalfPtr = reinterpret_cast<FFloat16*>(SHBulkData.Realloc(TotalBytes));

	for (int32 i = 0; i < SplatCount; i++)
	{
		const FGaussianSplatData& Splat = InSplats[i];
		const int32 BaseIdx = i * TotalCoeffs * 3;

		// Store DC coefficient first (index 0)
		HalfPtr[BaseIdx + 0] = FFloat16(Splat.SH_DC.X);
		HalfPtr[BaseIdx + 1] = FFloat16(Splat.SH_DC.Y);
		HalfPtr[BaseIdx + 2] = FFloat16(Splat.SH_DC.Z);

		// Store higher-order coefficients (indices 1..NumHigherCoeffs)
		for (int32 c = 0; c < NumHigherCoeffs; c++)
		{
			int32 Idx = BaseIdx + (c + 1) * 3;  // +1 to skip DC
			HalfPtr[Idx + 0] = FFloat16(Splat.SH[c].X);
			HalfPtr[Idx + 1] = FFloat16(Splat.SH[c].Y);
			HalfPtr[Idx + 2] = FFloat16(Splat.SH[c].Z);
		}
	}

	SHBulkData.Unlock();

	// Set bulk data flags for optimal storage
	SHBulkData.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload);

	// Update format to reflect actual storage
	SHFormat = EGaussianSHFormat::Float16;
}

void UGaussianSplatAsset::GetPositionData(TArray<uint8>& OutData) const
{
	const int64 DataSize = PositionBulkData.GetBulkDataSize();
	if (DataSize > 0)
	{
		OutData.SetNum(static_cast<int32>(DataSize));
		const void* SrcData = PositionBulkData.LockReadOnly();
		FMemory::Memcpy(OutData.GetData(), SrcData, DataSize);
		PositionBulkData.Unlock();
	}
	else
	{
		OutData.Empty();
	}
}

void UGaussianSplatAsset::GetOtherData(TArray<uint8>& OutData) const
{
	const int64 DataSize = OtherBulkData.GetBulkDataSize();
	if (DataSize > 0)
	{
		OutData.SetNum(static_cast<int32>(DataSize));
		const void* SrcData = OtherBulkData.LockReadOnly();
		FMemory::Memcpy(OutData.GetData(), SrcData, DataSize);
		OtherBulkData.Unlock();
	}
	else
	{
		OutData.Empty();
	}
}

void UGaussianSplatAsset::GetSHData(TArray<uint8>& OutData) const
{
	const int64 DataSize = SHBulkData.GetBulkDataSize();
	if (DataSize > 0)
	{
		OutData.SetNum(static_cast<int32>(DataSize));
		const void* SrcData = SHBulkData.LockReadOnly();
		FMemory::Memcpy(OutData.GetData(), SrcData, DataSize);
		SHBulkData.Unlock();
	}
	else
	{
		OutData.Empty();
	}
}

void UGaussianSplatAsset::GetColorTextureData(TArray<uint8>& OutData) const
{
	const int64 DataSize = ColorTextureBulkData.GetBulkDataSize();
	if (DataSize > 0)
	{
		OutData.SetNum(static_cast<int32>(DataSize));
		const void* SrcData = ColorTextureBulkData.LockReadOnly();
		FMemory::Memcpy(OutData.GetData(), SrcData, DataSize);
		ColorTextureBulkData.Unlock();
	}
	else
	{
		OutData.Empty();
	}
}

// --- Zero-copy read-only accessors (see header for contract) ---

const void* UGaussianSplatAsset::LockPositionDataReadOnly(int64* OutSize) const
{
	const int64 DataSize = PositionBulkData.GetBulkDataSize();
	if (OutSize) *OutSize = DataSize;
	return (DataSize > 0) ? PositionBulkData.LockReadOnly() : nullptr;
}

const void* UGaussianSplatAsset::LockOtherDataReadOnly(int64* OutSize) const
{
	const int64 DataSize = OtherBulkData.GetBulkDataSize();
	if (OutSize) *OutSize = DataSize;
	return (DataSize > 0) ? OtherBulkData.LockReadOnly() : nullptr;
}

const void* UGaussianSplatAsset::LockSHDataReadOnly(int64* OutSize) const
{
	const int64 DataSize = SHBulkData.GetBulkDataSize();
	if (OutSize) *OutSize = DataSize;
	return (DataSize > 0) ? SHBulkData.LockReadOnly() : nullptr;
}

const void* UGaussianSplatAsset::LockColorTextureDataReadOnly(int64* OutSize) const
{
	const int64 DataSize = ColorTextureBulkData.GetBulkDataSize();
	if (OutSize) *OutSize = DataSize;
	return (DataSize > 0) ? ColorTextureBulkData.LockReadOnly() : nullptr;
}

const void* UGaussianSplatAsset::LockTemporalDataReadOnly(int64* OutSize) const
{
	const int64 DataSize = TemporalBulkData.GetBulkDataSize();
	if (OutSize) *OutSize = DataSize;
	return (DataSize > 0) ? TemporalBulkData.LockReadOnly() : nullptr;
}

void UGaussianSplatAsset::UnlockTemporalData() const
{
	if (TemporalBulkData.GetBulkDataSize() > 0) TemporalBulkData.Unlock();
}

const void* UGaussianSplatAsset::LockKeyframeDataReadOnly(int64* OutSize) const
{
	const int64 DataSize = KeyframeBulkData.GetBulkDataSize();
	if (OutSize) *OutSize = DataSize;
	return (DataSize > 0) ? KeyframeBulkData.LockReadOnly() : nullptr;
}

void UGaussianSplatAsset::UnlockKeyframeData() const
{
	if (KeyframeBulkData.GetBulkDataSize() > 0) KeyframeBulkData.Unlock();
}

// ----------------------------------------------------------------------------
// .o4d v2 dedicated container (magic 'O4D2')
//
// Layout (all little-endian):
//   [0..3]   magic 'O','4','D','2'
//   u32      Version = 2
//   u32      Flags   (bit0 = keyframe 4D)
//   i32      M       (splats per frame = SplatCount)
//   i32      N       (keyframe count; 0 = static/3D)
//   f32      TimeStart
//   f32      TimeEnd
//   M x      FGaussianSplatData records (via operator<<)
//   [N*M*64] keyframe block (same layout as KeyframeBulkData, present when N>0)
//
// NOTE: FGaussianSplatData::operator<< does not serialize AnchorTime/TimeSigma,
// so a t/scale_t (temporal marginalization) 4D asset saved this way comes back
// as a static asset. The .o4d v2 container targets keyframe 4D / 3D assets.
// ----------------------------------------------------------------------------

#define O4D2_CONTAINER_VERSION 2u
#define O4D2_FLAG_KEYFRAME_4D  0x1u

bool UGaussianSplatAsset::SaveToO4DFile(FString FilePath)
{
	if (SplatCount <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("SaveToO4DFile: asset '%s' has no splat data"), *GetName());
		return false;
	}

	// 1. Decompress the stored bulk data into raw splat records
	TArray<FGaussianSplatData> Splats;
	if (!DecompressToSplatData(Splats))
	{
		UE_LOG(LogTemp, Error, TEXT("SaveToO4DFile: failed to decompress splat data for asset '%s'"), *GetName());
		return false;
	}

	// Non-const: FArchive::operator<< binds int32 to a mutable lvalue reference
	int32 M = SplatCount;
	int32 N = (bIsKeyframe4D && KeyframeCount > 0) ? KeyframeCount : 0;

	// 2. Serialize container into memory
	TArray<uint8> Buffer;
	FMemoryWriter Writer(Buffer, true);

	uint8 Magic[4] = { 'O', '4', 'D', '2' };
	Writer.Serialize(Magic, 4);

	uint32 Version = O4D2_CONTAINER_VERSION;
	Writer << Version;

	uint32 Flags = bIsKeyframe4D ? O4D2_FLAG_KEYFRAME_4D : 0u;
	Writer << Flags;

	Writer << M;
	Writer << N;

	float Start = TimeStart;
	float End = TimeEnd;
	Writer << Start;
	Writer << End;

	for (int32 i = 0; i < M; i++)
	{
		Writer << Splats[i];
	}

	// 3. Keyframe block (raw bytes, same layout as KeyframeBulkData)
	if (N > 0)
	{
		int64 KeyframeSize = 0;
		const void* KeyframePtr = LockKeyframeDataReadOnly(&KeyframeSize);
		if (!KeyframePtr || KeyframeSize < static_cast<int64>(N) * M * KeyframeStride)
		{
			if (KeyframePtr)
			{
				UnlockKeyframeData();
			}
			UE_LOG(LogTemp, Error, TEXT("SaveToO4DFile: keyframe data missing/short on asset '%s' (need %lld bytes, have %lld)"),
				*GetName(), static_cast<int64>(N) * M * KeyframeStride, KeyframeSize);
			return false;
		}
		Writer.Serialize(const_cast<void*>(KeyframePtr), KeyframeSize);
		UnlockKeyframeData();
	}

	return FFileHelper::SaveArrayToFile(Buffer, *FilePath);
}

UGaussianSplatAsset* UGaussianSplatAsset::LoadFromO4DFile(FString FilePath, UObject* Outer)
{
	TArray<uint8> Buffer;
	if (!FFileHelper::LoadFileToArray(Buffer, *FilePath))
	{
		UE_LOG(LogTemp, Error, TEXT("LoadFromO4DFile: cannot open file: %s"), *FilePath);
		return nullptr;
	}

	FMemoryReader Reader(Buffer, true);

	uint8 Magic[4] = { 0, 0, 0, 0 };
	Reader.Serialize(Magic, 4);
	if (Magic[0] != 'O' || Magic[1] != '4' || Magic[2] != 'D' || Magic[3] != '2')
	{
		UE_LOG(LogTemp, Error, TEXT("LoadFromO4DFile: bad magic in %s (expected O4D2)"), *FilePath);
		return nullptr;
	}

	uint32 Version = 0;
	Reader << Version;
	if (Version != O4D2_CONTAINER_VERSION)
	{
		UE_LOG(LogTemp, Error, TEXT("LoadFromO4DFile: unsupported container version %u in %s"), Version, *FilePath);
		return nullptr;
	}

	uint32 Flags = 0;
	int32 M = 0;
	int32 N = 0;
	float Start = 0.f;
	float End = 0.f;
	Reader << Flags;
	Reader << M;
	Reader << N;
	Reader << Start;
	Reader << End;

	// Sanity limits (mirror the PLY reader's 200M splat safety cap)
	constexpr int32 MaxReasonableSplats = 200 * 1024 * 1024;
	constexpr int32 MaxReasonableKeyframes = 1024 * 1024;
	if (M <= 0 || M > MaxReasonableSplats || N < 0 || N > MaxReasonableKeyframes)
	{
		UE_LOG(LogTemp, Error, TEXT("LoadFromO4DFile: invalid header (M=%d, N=%d) in %s"), M, N, *FilePath);
		return nullptr;
	}

	// 1. Read splat records
	TArray<FGaussianSplatData> Splats;
	Splats.SetNum(M);
	for (int32 i = 0; i < M; i++)
	{
		Reader << Splats[i];
	}

	// 2. Read keyframe block
	TArray<uint8> KeyframeData;
	if (N > 0)
	{
		const int64 KeyframeBytes = static_cast<int64>(N) * M * KeyframeStride;
		const int64 Remaining = Reader.TotalSize() - Reader.Tell();
		if (Remaining < KeyframeBytes)
		{
			UE_LOG(LogTemp, Error, TEXT("LoadFromO4DFile: truncated keyframe block in %s (need %lld, have %lld)"),
				*FilePath, KeyframeBytes, Remaining);
			return nullptr;
		}
		KeyframeData.SetNumUninitialized(KeyframeBytes);
		Reader.Serialize(KeyframeData.GetData(), KeyframeBytes);
	}

	// 3. Create the asset and rebuild its render-facing data
	UObject* OuterObject = Outer;
	if (!OuterObject)
	{
		OuterObject = (UObject*)GetTransientPackage();
	}
	UGaussianSplatAsset* Asset = NewObject<UGaussianSplatAsset>(OuterObject, NAME_None, RF_Public);
	if (!Asset)
	{
		return nullptr;
	}

	// Pick SH bands by scanning for non-zero higher-order coefficients
	int32 DetectedSHBands = 0;
	for (int32 i = 0; i < M && DetectedSHBands == 0; i++)
	{
		for (int32 c = 0; c < 15; c++)
		{
			if (FMath::Abs(Splats[i].SH[c].X) + FMath::Abs(Splats[i].SH[c].Y) + FMath::Abs(Splats[i].SH[c].Z) > 1e-6f)
			{
				DetectedSHBands = 3;
				break;
			}
		}
	}
	Asset->SHBands = DetectedSHBands;

	Asset->InitializeFromSplatData(Splats, EGaussianQualityLevel::VeryHigh);
	Asset->SourceFilePath = FilePath;

	// Keyframe/temporal state is set AFTER InitializeFromSplatData (whose
	// temporal auto-detection only sees the default AnchorTime/TimeSigma
	// because operator<< does not serialize them).
	Asset->bIsKeyframe4D = (Flags & O4D2_FLAG_KEYFRAME_4D) != 0;
	Asset->KeyframeCount = N;
	Asset->TimeStart = Start;
	Asset->TimeEnd = End;
	Asset->bIs4D = (N > 0);

	if (N > 0)
	{
		Asset->KeyframeBulkData.Lock(LOCK_READ_WRITE);
		void* Dest = Asset->KeyframeBulkData.Realloc(KeyframeData.Num());
		FMemory::Memcpy(Dest, KeyframeData.GetData(), KeyframeData.Num());
		Asset->KeyframeBulkData.Unlock();
		Asset->KeyframeBulkData.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload);
	}

	UE_LOG(LogTemp, Log, TEXT("LoadFromO4DFile: loaded '%s' (%d splats, %d keyframes, keyframe4D=%d)"),
		*FilePath, M, N, Asset->bIsKeyframe4D ? 1 : 0);
	return Asset;
}

bool UGaussianSplatAsset::DecompressToSplatData(TArray<FGaussianSplatData>& OutSplats) const
{
	OutSplats.Reset();
	if (SplatCount <= 0)
	{
		return false;
	}

	OutSplats.SetNum(SplatCount);

	int64 PosSize = 0, OtherSize = 0, ColorSize = 0, SHSize = 0;
	const uint8* RawPosition = static_cast<const uint8*>(LockPositionDataReadOnly(&PosSize));
	const uint8* RawOther = static_cast<const uint8*>(LockOtherDataReadOnly(&OtherSize));
	const uint8* RawColor = static_cast<const uint8*>(LockColorTextureDataReadOnly(&ColorSize));
	const uint8* RawSH = (SHBands > 0) ? static_cast<const uint8*>(LockSHDataReadOnly(&SHSize)) : nullptr;
	ON_SCOPE_EXIT
	{
		UnlockPositionData();
		UnlockOtherData();
		UnlockColorTextureData();
		UnlockSHData();
	};

	if (!RawPosition || PosSize < static_cast<int64>(SplatCount) * 12 ||
		!RawOther || OtherSize < static_cast<int64>(SplatCount) * 28)
	{
		return false;
	}

	const FFloat16Color* ColorPixels = nullptr;
	const bool bHasColor = (RawColor && ColorSize > 0 && ColorTextureWidth > 0 && ColorTextureHeight > 0);
	if (bHasColor)
	{
		ColorPixels = reinterpret_cast<const FFloat16Color*>(RawColor);
	}

	const FFloat16* SHHalf = reinterpret_cast<const FFloat16*>(RawSH);
	// SH buffer stores TotalCoeffs (= NumHigherCoeffs + 1 for DC) coefficients, 3 channels each
	const int32 TotalCoeffs = (SHBands == 1) ? 4 : (SHBands == 2) ? 9 : (SHBands == 3) ? 16 : 0;
	const bool bHasSH = (RawSH && SHSize >= static_cast<int64>(SplatCount) * TotalCoeffs * 3 * static_cast<int32>(sizeof(FFloat16)));

	for (int32 i = 0; i < SplatCount; i++)
	{
		FGaussianSplatData& Splat = OutSplats[i];

		// Position (float32, UE cm)
		const float* Pos = reinterpret_cast<const float*>(RawPosition + i * 12);
		Splat.Position = FVector3f(Pos[0], Pos[1], Pos[2]);

		// Rotation (normalized quaternion) + Scale (linear cm), 28B record
		const float* Other = reinterpret_cast<const float*>(RawOther + i * 28);
		Splat.Rotation = FQuat4f(Other[0], Other[1], Other[2], Other[3]);
		Splat.Scale = FVector3f(Other[4], Other[5], Other[6]);

		// Color + opacity from the Morton-swizzled float16 texture.
		// The texture stores the final display color (0.5 + C0 * DC) and the
		// sigmoid-opacity; invert the DC conversion to recover SH_DC.
		if (bHasColor)
		{
			int32 TexX, TexY;
			GaussianSplattingUtils::SplatIndexToTextureCoord(i, ColorTextureWidth, TexX, TexY);
			if (TexY < ColorTextureHeight)
			{
				const FFloat16Color& Pixel = ColorPixels[TexY * ColorTextureWidth + TexX];
				Splat.Opacity = FMath::Clamp(Pixel.A.GetFloat(), 0.0f, 1.0f);
				Splat.SH_DC = FVector3f(
					(Pixel.R.GetFloat() - 0.5f) / GaussianSplattingConstants::SH_C0,
					(Pixel.G.GetFloat() - 0.5f) / GaussianSplattingConstants::SH_C0,
					(Pixel.B.GetFloat() - 0.5f) / GaussianSplattingConstants::SH_C0);
			}
		}

		// Higher-order SH coefficients (float16), DC first then bands 1-3
		if (bHasSH)
		{
			const int32 BaseIdx = i * TotalCoeffs * 3;
			for (int32 c = 1; c < TotalCoeffs; c++)
			{
				Splat.SH[c - 1] = FVector3f(
					SHHalf[BaseIdx + c * 3 + 0].GetFloat(),
					SHHalf[BaseIdx + c * 3 + 1].GetFloat(),
					SHHalf[BaseIdx + c * 3 + 2].GetFloat());
			}
		}

		// AnchorTime/TimeSigma intentionally left at defaults (0 / 1e10):
		// they are not part of the .o4d container's splat records.
	}

	return true;
}

void UGaussianSplatAsset::UnlockPositionData() const
{
	if (PositionBulkData.GetBulkDataSize() > 0) PositionBulkData.Unlock();
}

void UGaussianSplatAsset::UnlockOtherData() const
{
	if (OtherBulkData.GetBulkDataSize() > 0) OtherBulkData.Unlock();
}

void UGaussianSplatAsset::UnlockSHData() const
{
	if (SHBulkData.GetBulkDataSize() > 0) SHBulkData.Unlock();
}

void UGaussianSplatAsset::UnlockColorTextureData() const
{
	if (ColorTextureBulkData.GetBulkDataSize() > 0) ColorTextureBulkData.Unlock();
}

#if WITH_EDITOR
bool UGaussianSplatAsset::BuildNaniteClusterHierarchy()
{
	// Check if source file exists
	if (SourceFilePath.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("BuildNaniteClusterHierarchy: Source file path is empty"));
		return false;
	}

	if (!FPaths::FileExists(SourceFilePath))
	{
		UE_LOG(LogTemp, Error, TEXT("BuildNaniteClusterHierarchy: Source file not found: %s"), *SourceFilePath);
		return false;
	}

	FScopedSlowTask SlowTask(100.0f, FText::FromString(TEXT("Building Nanite Cluster Hierarchy...")));
	SlowTask.MakeDialog(true);

	// Step 1: Re-read PLY file
	SlowTask.EnterProgressFrame(30.0f, FText::FromString(TEXT("Reading PLY file...")));

	TArray<FGaussianSplatData> SplatData;
	FString ErrorMessage;

	if (!FPLYFileReader::ReadPLYFile(SourceFilePath, SplatData, ErrorMessage))
	{
		UE_LOG(LogTemp, Error, TEXT("BuildNaniteClusterHierarchy: Failed to read PLY file: %s"), *ErrorMessage);
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("BuildNaniteClusterHierarchy: Read %d splats from PLY file"), SplatData.Num());

	// Store original splat count before LOD splats are appended
	OriginalSplatCount = SplatData.Num();

	// Step 2: Build cluster hierarchy
	SlowTask.EnterProgressFrame(50.0f, FText::FromString(TEXT("Building cluster hierarchy...")));

	FGaussianClusterHierarchy NewClusterHierarchy;
	FGaussianClusterBuilder::FBuildSettings BuildSettings;
	BuildSettings.SplatsPerCluster = 128;
	BuildSettings.MaxChildrenPerCluster = 8;
	BuildSettings.bReorderSplats = true;
	BuildSettings.bGenerateLOD = true;

	bool bClusteringSucceeded = FGaussianClusterBuilder::BuildClusterHierarchy(
		SplatData, NewClusterHierarchy, BuildSettings);

	if (!bClusteringSucceeded)
	{
		UE_LOG(LogTemp, Error, TEXT("BuildNaniteClusterHierarchy: Failed to build cluster hierarchy"));
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("BuildNaniteClusterHierarchy: Built %d clusters, %d LOD levels, %d LOD splats"),
		NewClusterHierarchy.Clusters.Num(), NewClusterHierarchy.NumLODLevels, NewClusterHierarchy.LODSplats.Num());

	// Step 3: Append LOD splats to main buffer (unified approach)
	SlowTask.EnterProgressFrame(15.0f, FText::FromString(TEXT("Appending LOD splats...")));

	if (NewClusterHierarchy.LODSplats.Num() > 0)
	{
		// Update cluster LODSplatStartIndex to point into unified buffer
		for (FGaussianCluster& Cluster : NewClusterHierarchy.Clusters)
		{
			if (Cluster.LODSplatCount > 0)
			{
				Cluster.LODSplatStartIndex += OriginalSplatCount;
			}
		}

		// Append LOD splats to main splat array
		SplatData.Append(NewClusterHierarchy.LODSplats);

		UE_LOG(LogTemp, Log, TEXT("BuildNaniteClusterHierarchy: Unified buffer: %d original + %d LOD = %d total splats"),
			OriginalSplatCount, NewClusterHierarchy.LODSplats.Num(), SplatData.Num());

		// Clear LODSplats from hierarchy (now stored in main buffer)
		NewClusterHierarchy.LODSplats.Empty();
	}

	// Step 4: Reinitialize asset with new splat data (includes LOD splats)
	SlowTask.EnterProgressFrame(5.0f, FText::FromString(TEXT("Updating asset data...")));

	// Store the cluster hierarchy
	ClusterHierarchy = MoveTemp(NewClusterHierarchy);

	// Reinitialize from the new splat data (now includes LOD splats)
	InitializeFromSplatData(SplatData, ImportQuality);

	// Enable Nanite
	bEnableNanite = true;

	// Mark package dirty
	MarkPackageDirty();

	UE_LOG(LogTemp, Log, TEXT("BuildNaniteClusterHierarchy: Successfully built Nanite hierarchy. Total splats: %d, Clusters: %d"),
		SplatCount, ClusterHierarchy.Clusters.Num());

	// Invalidate shared render data so it will be recreated with new cluster data
	RenderData.Reset();

	// Notify listeners (components) that asset has changed so they can recreate their scene proxies
	OnAssetChanged.Broadcast(this);

	return true;
}

void UGaussianSplatAsset::ClearNaniteClusterHierarchy()
{
	if (!bEnableNanite && !ClusterHierarchy.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("ClearNaniteClusterHierarchy: Nanite is not enabled"));
		return;
	}

	// If we have LOD splats appended, we need to re-read the original data
	if (OriginalSplatCount > 0 && OriginalSplatCount < SplatCount)
	{
		// Re-read the original PLY file to get clean splat data without LOD splats
		if (!SourceFilePath.IsEmpty() && FPaths::FileExists(SourceFilePath))
		{
			TArray<FGaussianSplatData> SplatData;
			FString ErrorMessage;

			if (FPLYFileReader::ReadPLYFile(SourceFilePath, SplatData, ErrorMessage))
			{
				// Reinitialize with original splats only (no LOD)
				InitializeFromSplatData(SplatData, ImportQuality);
				UE_LOG(LogTemp, Log, TEXT("ClearNaniteClusterHierarchy: Restored %d original splats"), SplatData.Num());
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("ClearNaniteClusterHierarchy: Could not re-read source file, keeping current splat data"));
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("ClearNaniteClusterHierarchy: Source file not found, keeping current splat data (includes LOD splats)"));
		}
	}

	// Clear cluster hierarchy
	ClusterHierarchy.Reset();

	// Reset counters
	OriginalSplatCount = 0;
	bEnableNanite = false;

	// Mark package dirty
	MarkPackageDirty();

	UE_LOG(LogTemp, Log, TEXT("ClearNaniteClusterHierarchy: Cleared Nanite hierarchy"));

	// Invalidate shared render data so it will be recreated without cluster data
	RenderData.Reset();

	// Notify listeners (components) that asset has changed so they can recreate their scene proxies
	OnAssetChanged.Broadcast(this);
}

void UGaussianSplatAsset::GenerateThumbnail(const TArray<FGaussianSplatData>& InSplats)
{
	if (InSplats.IsEmpty()) return;

	ThumbnailSize = 256;

	// Initialize pixel buffer with a dark background
	TArray<FColor> Pixels;
	Pixels.Init(FColor(30, 30, 30, 255), ThumbnailSize * ThumbnailSize);

	// Depth buffer: keep the front-most (min depth) splat per pixel
	TArray<float> DepthBuffer;
	DepthBuffer.Init(TNumericLimits<float>::Max(), ThumbnailSize * ThumbnailSize);

	// ------------------------------------------------------------------
	// Fixed camera: 45° yaw, -25° pitch, orthographic projection
	// ------------------------------------------------------------------
	const float PitchRad = FMath::DegreesToRadians(-25.f);
	const float YawRad   = FMath::DegreesToRadians(45.f);

	// Camera's forward vector (direction it is looking)
	const FVector CamForward(
		FMath::Cos(PitchRad) * FMath::Cos(YawRad),
		FMath::Cos(PitchRad) * FMath::Sin(YawRad),
		FMath::Sin(PitchRad)
	);
	const FVector CamRight = FVector::CrossProduct(CamForward, FVector::UpVector).GetSafeNormal();
	const FVector CamUp    = FVector::CrossProduct(CamRight,   CamForward).GetSafeNormal();

	// Scale: find the half-extent of the bounds projected onto the camera plane
	const FVector Center = BoundingBox.GetCenter();
	const FVector Extent = BoundingBox.GetExtent();

	float MaxExtent = 0.f;
	for (int32 sx : {-1, 1}) for (int32 sy : {-1, 1}) for (int32 sz : {-1, 1})
	{
		const FVector Corner = Extent * FVector(sx, sy, sz);
		MaxExtent = FMath::Max(MaxExtent, FMath::Abs(FVector::DotProduct(Corner, CamRight)));
		MaxExtent = FMath::Max(MaxExtent, FMath::Abs(FVector::DotProduct(Corner, CamUp)));
	}
	MaxExtent = FMath::Max(MaxExtent, 1.f) * 1.1f; // 10 % padding

	// ------------------------------------------------------------------
	// Rasterize: stride so we don't spend more than ~200k iterations
	// ------------------------------------------------------------------
	const int32 Step = FMath::Max(1, InSplats.Num() / 200000);

	for (int32 i = 0; i < InSplats.Num(); i += Step)
	{
		const FGaussianSplatData& Splat = InSplats[i];
		const FVector Rel = FVector(Splat.Position) - Center;

		const float U     =  FVector::DotProduct(Rel, CamRight);
		const float V     =  FVector::DotProduct(Rel, CamUp);
		const float Depth =  FVector::DotProduct(Rel, CamForward); // smaller = closer

		const float NormU = (U / MaxExtent + 1.f) * 0.5f;
		const float NormV = (1.f - (V / MaxExtent + 1.f) * 0.5f); // flip Y

		const int32 PX = FMath::Clamp((int32)(NormU * ThumbnailSize), 0, ThumbnailSize - 1);
		const int32 PY = FMath::Clamp((int32)(NormV * ThumbnailSize), 0, ThumbnailSize - 1);
		const int32 PixIdx = PY * ThumbnailSize + PX;

		if (Depth < DepthBuffer[PixIdx])
		{
			DepthBuffer[PixIdx] = Depth;

			// SH DC → sRGB-ish base color (clamped, linear → sRGB gamma)
			const FVector3f BaseColor = GaussianSplattingUtils::SHDCToColor(Splat.SH_DC);
			const auto ToSRGB = [](float Lin) -> uint8
			{
				float Clamped = FMath::Clamp(Lin, 0.f, 1.f);
				return (uint8)(FMath::Pow(Clamped, 1.f / 2.2f) * 255.f + 0.5f);
			};

			Pixels[PixIdx] = FColor(ToSRGB(BaseColor.X), ToSRGB(BaseColor.Y), ToSRGB(BaseColor.Z), 255);
		}
	}

	// ------------------------------------------------------------------
	// Store pixel data persistently (will be serialized with the asset)
	// ------------------------------------------------------------------
	const int32 NumBytes = ThumbnailSize * ThumbnailSize * 4;
	ThumbnailData.SetNum(NumBytes);

	// FColor is RGBA; we store as BGRA for PF_B8G8R8A8 format
	for (int32 Idx = 0; Idx < ThumbnailSize * ThumbnailSize; ++Idx)
	{
		ThumbnailData[Idx * 4 + 0] = Pixels[Idx].B;
		ThumbnailData[Idx * 4 + 1] = Pixels[Idx].G;
		ThumbnailData[Idx * 4 + 2] = Pixels[Idx].R;
		ThumbnailData[Idx * 4 + 3] = Pixels[Idx].A;
	}

	// Create the texture from the stored data
	CreateThumbnailTextureFromData();

	UE_LOG(LogTemp, Log, TEXT("GaussianSplatAsset: Thumbnail generated (%dx%d)"), ThumbnailSize, ThumbnailSize);
}

void UGaussianSplatAsset::CreateThumbnailTextureFromData()
{
	if (ThumbnailData.Num() == 0 || ThumbnailSize <= 0)
	{
		return;
	}

	// Create a transient texture (not saved, recreated on load)
	ThumbnailTexture = NewObject<UTexture2D>(this, NAME_None, RF_Transient);

	FTexturePlatformData* PlatformData = new FTexturePlatformData();
	PlatformData->SizeX       = ThumbnailSize;
	PlatformData->SizeY       = ThumbnailSize;
	PlatformData->PixelFormat = PF_B8G8R8A8;
	ThumbnailTexture->SetPlatformData(PlatformData);

	ThumbnailTexture->SRGB             = true;
	ThumbnailTexture->CompressionSettings = TC_Default;
	ThumbnailTexture->Filter           = TF_Bilinear;
	ThumbnailTexture->NeverStream      = true;
#if WITH_EDITORONLY_DATA
	ThumbnailTexture->MipGenSettings   = TMGS_NoMipmaps;
#endif

	FTexture2DMipMap* Mip = new FTexture2DMipMap();
	PlatformData->Mips.Add(Mip);
	Mip->SizeX = ThumbnailSize;
	Mip->SizeY = ThumbnailSize;

	// Copy the stored pixel data to the mip
	Mip->BulkData.Lock(LOCK_READ_WRITE);
	uint8* MipData = reinterpret_cast<uint8*>(Mip->BulkData.Realloc(ThumbnailData.Num()));
	if (MipData)
	{
		FMemory::Memcpy(MipData, ThumbnailData.GetData(), ThumbnailData.Num());
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("GaussianSplatAsset: Thumbnail BulkData.Realloc returned null (size=%d)"), ThumbnailData.Num());
	}
	Mip->BulkData.Unlock();

	// Only update GPU resource if we actually wrote pixel data
	if (MipData)
	{
		ThumbnailTexture->UpdateResource();
	}
}
#endif
