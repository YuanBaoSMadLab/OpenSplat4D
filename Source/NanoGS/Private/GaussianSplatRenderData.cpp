// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatRenderData.h"
#include "GaussianSplatAsset.h"
#include "RHICommandList.h"
#include "Misc/ScopeExit.h"

FGaussianSplatRenderData::FGaussianSplatRenderData()
{
}

FGaussianSplatRenderData::~FGaussianSplatRenderData()
{
	ReleaseGPUBuffers();
}

void FGaussianSplatRenderData::Initialize(UGaussianSplatAsset* Asset)
{
	FScopeLock Lock(&InitLock);

	if (bIsInitialized)
	{
		return;
	}

	if (!Asset || !Asset->IsValid())
	{
		return;
	}

	AssetName = Asset->GetName();
	SplatCount = Asset->GetSplatCount();
	PositionFormat = Asset->PositionFormat;
	bIs4D = Asset->Is4D();
	bIsKeyframe4D = Asset->IsKeyframe4D();
	bIsNative4D = Asset->IsNative4D();

	// --- Pack native 4D (fudan) data (80 bytes/splat) ---
	if (bIsNative4D)
	{
		int64 Native4DSize = 0;
		const uint8* RawNative4DData = static_cast<const uint8*>(Asset->LockNative4DDataReadOnly(&Native4DSize));
		ON_SCOPE_EXIT
		{
			Asset->UnlockNative4DData();
		};

		if (RawNative4DData && Native4DSize > 0)
		{
			Native4DSplatCount = static_cast<int32>(Native4DSize / UGaussianSplatAsset::Native4DStride);
			Native4DData.SetNumUninitialized(static_cast<int32>(Native4DSize));
			FMemory::Memcpy(Native4DData.GetData(), RawNative4DData, Native4DData.Num());
			TimeDuration4D = Asset->TimeEnd - Asset->TimeStart;
		}
		else
		{
			// Native 4D data missing (e.g. .o4d round-trip) -- fall back to static rendering
			UE_LOG(LogTemp, Warning, TEXT("GaussianSplatRenderData: asset '%s' marked native 4D but native 4D data missing; treating as static"),
				*AssetName);
			Native4DData.Reset();
			bIsNative4D = false;
			Native4DSplatCount = 0;
		}
	}

	// --- Pack keyframe 4D data (64 bytes/splat/frame) ---
	if (bIsKeyframe4D)
	{
		int64 KeyframeSize = 0;
		const uint8* RawKeyframeData = static_cast<const uint8*>(Asset->LockKeyframeDataReadOnly(&KeyframeSize));
		ON_SCOPE_EXIT
		{
			Asset->UnlockKeyframeData();
		};

		KeyframeFrameCount = Asset->KeyframeCount;
		if (RawKeyframeData && KeyframeSize > 0 && KeyframeFrameCount > 0)
		{
			// Derive M from the actual bulk data size so a later Nanite rebuild
			// (which grows SplatCount with LOD splats) cannot invalidate the check.
			KeyframeSplatCount = static_cast<int32>(KeyframeSize / (static_cast<int64>(KeyframeFrameCount) * UGaussianSplatAsset::KeyframeStride));
			KeyframeData.SetNumUninitialized(static_cast<int32>(KeyframeSize));
			FMemory::Memcpy(KeyframeData.GetData(), RawKeyframeData, KeyframeData.Num());
		}
		else
		{
			// Keyframe data missing (shouldn't happen) -- fall back to static frame-0 rendering
			UE_LOG(LogTemp, Warning, TEXT("GaussianSplatRenderData: asset '%s' marked keyframe 4D but keyframe data missing; treating as static"),
				*AssetName);
			KeyframeData.Reset();
			bIsKeyframe4D = false;
			KeyframeFrameCount = 0;
			KeyframeSplatCount = 0;
		}
	}

	// --- Pack 4D temporal data (16 bytes/splat; skipped for native 4D assets,
	// which carry no t/scale_t records and keep bIs4D only for sort gating) ---
	if (bIs4D && !bIsNative4D)
	{
		int64 TemporalSize = 0;
		const uint8* RawTemporalData = static_cast<const uint8*>(Asset->LockTemporalDataReadOnly(&TemporalSize));
		ON_SCOPE_EXIT
		{
			Asset->UnlockTemporalData();
		};

		if (RawTemporalData && TemporalSize >= static_cast<int64>(SplatCount) * UGaussianSplatAsset::TemporalStride)
		{
			TemporalData.SetNumUninitialized(SplatCount * UGaussianSplatAsset::TemporalStride);
			FMemory::Memcpy(TemporalData.GetData(), RawTemporalData, TemporalData.Num());
		}
		else
		{
			// Temporal data missing/short (shouldn't happen) -- fall back to static
			UE_LOG(LogTemp, Warning, TEXT("GaussianSplatRenderData: asset '%s' marked 4D but temporal data missing; treating as static"),
				*AssetName);
			TemporalData.Reset();
			bIs4D = false;
		}
	}

	// --- Pack splat data (16 bytes/splat) ---
	// PERF: read directly from asset bulk data (locked in place) instead of
	// copying Position/Other/ColorTexture into temporary TArray first. For a
	// 1M-splat asset this avoids ~50MB+ of transient allocations and 3 extra
	// full-buffer memcpys during render-data build.
	{
		int64 PositionSize = 0, OtherSize = 0, ColorSize = 0;
		const uint8* RawPositionData = static_cast<const uint8*>(Asset->LockPositionDataReadOnly(&PositionSize));
		const uint8* RawOtherData = static_cast<const uint8*>(Asset->LockOtherDataReadOnly(&OtherSize));
		const uint8* RawColorTextureData = static_cast<const uint8*>(Asset->LockColorTextureDataReadOnly(&ColorSize));
		ON_SCOPE_EXIT
		{
			Asset->UnlockPositionData();
			Asset->UnlockOtherData();
			Asset->UnlockColorTextureData();
		};

		const int32 ColorTexWidth = Asset->ColorTextureWidth;
		const int32 ColorTexHeight = Asset->ColorTextureHeight;
		const FFloat16Color* ColorPixels = nullptr;
		bool bHasColor = (RawColorTextureData && ColorSize > 0 && ColorTexWidth > 0 && ColorTexHeight > 0);
		if (bHasColor)
		{
			ColorPixels = reinterpret_cast<const FFloat16Color*>(RawColorTextureData);
		}

		PackedSplatData.SetNumUninitialized(SplatCount * GaussianSplattingConstants::PackedSplatStride);
		uint32* PackedPtr = reinterpret_cast<uint32*>(PackedSplatData.GetData());
		const float* PosFloats = reinterpret_cast<const float*>(RawPositionData);
		const float* OtherFloats = reinterpret_cast<const float*>(RawOtherData);

		for (int32 i = 0; i < SplatCount; i++)
		{
			FVector3f Position(
				PosFloats[i * 3 + 0],
				PosFloats[i * 3 + 1],
				PosFloats[i * 3 + 2]);

			const float* OtherBase = OtherFloats + i * 7;
			FQuat4f Rotation(OtherBase[0], OtherBase[1], OtherBase[2], OtherBase[3]);
			FVector3f Scale(OtherBase[4], OtherBase[5], OtherBase[6]);

			float ColorR = 1.0f, ColorG = 1.0f, ColorB = 1.0f, Opacity = 1.0f;
			if (bHasColor)
			{
				int32 TexX, TexY;
				GaussianSplattingUtils::SplatIndexToTextureCoord(i, ColorTexWidth, TexX, TexY);
				if (TexY < ColorTexHeight)
				{
					const FFloat16Color& Pixel = ColorPixels[TexY * ColorTexWidth + TexX];
					ColorR = FMath::Clamp(Pixel.R.GetFloat(), 0.0f, 1.0f);
					ColorG = FMath::Clamp(Pixel.G.GetFloat(), 0.0f, 1.0f);
					ColorB = FMath::Clamp(Pixel.B.GetFloat(), 0.0f, 1.0f);
					Opacity = FMath::Clamp(Pixel.A.GetFloat(), 0.0f, 1.0f);
				}
			}

			GaussianSplattingUtils::PackSplatToUint4(
				Position, Rotation, Scale,
				ColorR, ColorG, ColorB, Opacity,
				&PackedPtr[i * 4]);
		}
	}

	// Load chunk data
	CachedChunkData = Asset->ChunkData;

	// Load SH data
	if (Asset->SHBands > 0)
	{
		Asset->GetSHData(SHData);
		SHBands = Asset->SHBands;
	}

	// Load Nanite cluster data
	bEnableNanite = Asset->IsNaniteEnabled();

	if (bEnableNanite && Asset->HasClusterHierarchy())
	{
		const FGaussianClusterHierarchy& Hierarchy = Asset->GetClusterHierarchy();
		Hierarchy.ToGPUClusters(CachedClusterData);
		ClusterCount = CachedClusterData.Num();
		LeafClusterCount = Hierarchy.NumLeafClusters;
		bHasClusterData = true;

		const uint32 OriginalSplatCount = Hierarchy.TotalSplatCount;
		LODSplatCount = Hierarchy.TotalLODSplatCount;
		bHasLODSplats = (LODSplatCount > 0);

		// Build splat-to-cluster index mapping
		CachedSplatClusterIndices.SetNumZeroed(SplatCount);

		for (int32 ClusterIdx = 0; ClusterIdx < Hierarchy.Clusters.Num(); ++ClusterIdx)
		{
			const FGaussianCluster& Cluster = Hierarchy.Clusters[ClusterIdx];
			if (Cluster.IsLeaf())
			{
				for (uint32 i = 0; i < Cluster.SplatCount; ++i)
				{
					uint32 SplatIdx = Cluster.SplatStartIndex + i;
					if (SplatIdx < OriginalSplatCount)
					{
						CachedSplatClusterIndices[SplatIdx] = ClusterIdx;
					}
				}
			}
		}

		for (int32 ClusterIdx = 0; ClusterIdx < Hierarchy.Clusters.Num(); ++ClusterIdx)
		{
			const FGaussianCluster& Cluster = Hierarchy.Clusters[ClusterIdx];
			if (!Cluster.IsLeaf() && Cluster.LODSplatCount > 0)
			{
				for (uint32 i = 0; i < Cluster.LODSplatCount; ++i)
				{
					uint32 SplatIdx = Cluster.LODSplatStartIndex + i;
					if (SplatIdx < static_cast<uint32>(SplatCount))
					{
						CachedSplatClusterIndices[SplatIdx] = ClusterIdx;
					}
				}
			}
		}
	}
	else
	{
		bHasClusterData = false;
		ClusterCount = 0;
		LeafClusterCount = 0;
		bHasLODSplats = false;
		LODSplatCount = 0;
	}

	bIsInitialized = true;

	UE_LOG(LogTemp, Verbose, TEXT("GaussianSplatRenderData: Created shared CPU data for asset '%s' (%d splats)"),
		*AssetName, SplatCount);
}

void FGaussianSplatRenderData::CreateGPUBuffers(FRHICommandListBase& RHICmdList)
{
	FScopeLock Lock(&GPUInitLock);

	if (bGPUBuffersCreated)
	{
		return;
	}

	int32 SharedBufferCount = 0;

	// --- Packed splat buffer ---
	if (PackedSplatData.Num() > 0)
	{
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianPackedSplatBuffer"),
			PackedSplatData.Num(),
			0,
			BUF_Static | BUF_ShaderResource | BUF_ByteAddressBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		PackedSplatBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(PackedSplatBuffer, 0, PackedSplatData.Num(), RLM_WriteOnly);
		FMemory::Memcpy(Data, PackedSplatData.GetData(), PackedSplatData.Num());
		RHICmdList.UnlockBuffer(PackedSplatBuffer);

		PackedSplatBufferSRV = RHICmdList.CreateShaderResourceView(
			PackedSplatBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Raw));
		SharedBufferCount++;
	}

	// --- SH buffer (always create at least a dummy for shader binding) ---
	{
		uint32 SHDataSize = SHData.Num();
		if (SHDataSize == 0)
		{
			SHDataSize = 16;
		}

		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianSHBuffer"),
			SHDataSize,
			0,
			BUF_Static | BUF_ShaderResource | BUF_ByteAddressBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		SHBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(SHBuffer, 0, SHDataSize, RLM_WriteOnly);
		if (SHData.Num() > 0)
		{
			FMemory::Memcpy(Data, SHData.GetData(), SHData.Num());
		}
		else
		{
			FMemory::Memzero(Data, SHDataSize);
		}
		RHICmdList.UnlockBuffer(SHBuffer);

		SHBufferSRV = RHICmdList.CreateShaderResourceView(
			SHBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Raw));
		SharedBufferCount++;
	}

	// --- 4D temporal buffer (always create at least a dummy for shader binding) ---
	{
		uint32 TemporalDataSize = TemporalData.Num();
		if (TemporalDataSize == 0)
		{
			TemporalDataSize = 16; // dummy single record
		}

		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianTemporalBuffer"),
			TemporalDataSize,
			0,
			BUF_Static | BUF_ShaderResource | BUF_ByteAddressBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		TemporalBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(TemporalBuffer, 0, TemporalDataSize, RLM_WriteOnly);
		if (TemporalData.Num() > 0)
		{
			FMemory::Memcpy(Data, TemporalData.GetData(), TemporalData.Num());
		}
		else
		{
			FMemory::Memzero(Data, TemporalDataSize);
		}
		RHICmdList.UnlockBuffer(TemporalBuffer);

		TemporalBufferSRV = RHICmdList.CreateShaderResourceView(
			TemporalBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Raw));
		SharedBufferCount++;
	}

	// --- Keyframe 4D buffer (always create at least a dummy for shader binding) ---
	{
		uint32 KeyframeDataSize = KeyframeData.Num();
		if (KeyframeDataSize == 0)
		{
			KeyframeDataSize = UGaussianSplatAsset::KeyframeStride; // dummy single record
		}

		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianKeyframeBuffer"),
			KeyframeDataSize,
			0,
			BUF_Static | BUF_ShaderResource | BUF_ByteAddressBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		KeyframeBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(KeyframeBuffer, 0, KeyframeDataSize, RLM_WriteOnly);
		if (KeyframeData.Num() > 0)
		{
			FMemory::Memcpy(Data, KeyframeData.GetData(), KeyframeData.Num());
		}
		else
		{
			FMemory::Memzero(Data, KeyframeDataSize);
		}
		RHICmdList.UnlockBuffer(KeyframeBuffer);

		KeyframeBufferSRV = RHICmdList.CreateShaderResourceView(
			KeyframeBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Raw));
		SharedBufferCount++;
	}

	// --- Native 4D (fudan) buffer (always create at least a dummy for shader binding) ---
	{
		uint32 Native4DDataSize = Native4DData.Num();
		if (Native4DDataSize == 0)
		{
			Native4DDataSize = UGaussianSplatAsset::Native4DStride; // dummy single record
		}

		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianNative4DBuffer"),
			Native4DDataSize,
			0,
			BUF_Static | BUF_ShaderResource | BUF_ByteAddressBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		Native4DBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(Native4DBuffer, 0, Native4DDataSize, RLM_WriteOnly);
		if (Native4DData.Num() > 0)
		{
			FMemory::Memcpy(Data, Native4DData.GetData(), Native4DData.Num());
		}
		else
		{
			FMemory::Memzero(Data, Native4DDataSize);
		}
		RHICmdList.UnlockBuffer(Native4DBuffer);

		Native4DBufferSRV = RHICmdList.CreateShaderResourceView(
			Native4DBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Raw));
		SharedBufferCount++;
	}

	// --- Chunk buffer (always create at least a dummy for shader binding) ---
	{
		uint32 ChunkCount = CachedChunkData.Num();
		if (ChunkCount == 0)
		{
			ChunkCount = 1;
		}

		const uint32 ChunkSize = ChunkCount * sizeof(FGaussianChunkInfo);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianChunkBuffer"),
			ChunkSize,
			sizeof(FGaussianChunkInfo),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		ChunkBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(ChunkBuffer, 0, ChunkSize, RLM_WriteOnly);
		if (CachedChunkData.Num() > 0)
		{
			FMemory::Memcpy(Data, CachedChunkData.GetData(), ChunkSize);
		}
		else
		{
			FMemory::Memzero(Data, ChunkSize);
		}
		RHICmdList.UnlockBuffer(ChunkBuffer);

		ChunkBufferSRV = RHICmdList.CreateShaderResourceView(
			ChunkBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(FGaussianChunkInfo)));
		SharedBufferCount++;
	}

	// --- Index buffer (6 indices per quad) ---
	{
		TArray<uint16> Indices = { 0, 1, 2, 1, 3, 2 };

		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianSplatIndexBuffer"),
			Indices.Num() * sizeof(uint16),
			sizeof(uint16),
			BUF_Static | BUF_IndexBuffer)
			.SetInitialState(ERHIAccess::VertexOrIndexBuffer);
		IndexBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(IndexBuffer, 0, Indices.Num() * sizeof(uint16), RLM_WriteOnly);
		FMemory::Memcpy(Data, Indices.GetData(), Indices.Num() * sizeof(uint16));
		RHICmdList.UnlockBuffer(IndexBuffer);
		SharedBufferCount++;
	}

	// --- Cluster buffer (static, from asset) ---
	if (bHasClusterData && CachedClusterData.Num() > 0)
	{
		const uint32 BufferSize = CachedClusterData.Num() * sizeof(FGaussianGPUCluster);
		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianClusterBuffer"),
			BufferSize,
			sizeof(FGaussianGPUCluster),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		ClusterBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(ClusterBuffer, 0, BufferSize, RLM_WriteOnly);
		FMemory::Memcpy(Data, CachedClusterData.GetData(), BufferSize);
		RHICmdList.UnlockBuffer(ClusterBuffer);

		ClusterBufferSRV = RHICmdList.CreateShaderResourceView(
			ClusterBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(FGaussianGPUCluster)));
		SharedBufferCount++;
	}

	// --- Splat-to-cluster index buffer (static, from asset; or dummy for non-Nanite) ---
	{
		const uint32 BufferSize = CachedSplatClusterIndices.Num() > 0
			? CachedSplatClusterIndices.Num() * sizeof(uint32)
			: sizeof(uint32);  // dummy 1-element buffer for shader binding

		FRHIBufferCreateDesc Desc = FRHIBufferCreateDesc::Create(
			TEXT("GaussianSplatClusterIndexBuffer"),
			BufferSize,
			sizeof(uint32),
			BUF_Static | BUF_ShaderResource | BUF_StructuredBuffer)
			.SetInitialState(ERHIAccess::SRVMask);
		SplatClusterIndexBuffer = RHICmdList.CreateBuffer(Desc);

		void* Data = RHICmdList.LockBuffer(SplatClusterIndexBuffer, 0, BufferSize, RLM_WriteOnly);
		if (CachedSplatClusterIndices.Num() > 0)
		{
			FMemory::Memcpy(Data, CachedSplatClusterIndices.GetData(), BufferSize);
		}
		else
		{
			FMemory::Memzero(Data, BufferSize);
		}
		RHICmdList.UnlockBuffer(SplatClusterIndexBuffer);

		SplatClusterIndexBufferSRV = RHICmdList.CreateShaderResourceView(
			SplatClusterIndexBuffer, FRHIViewDesc::CreateBufferSRV()
				.SetType(FRHIViewDesc::EBufferType::Structured)
				.SetStride(sizeof(uint32)));
		SharedBufferCount++;
	}

	// Free CPU-side cached data after GPU upload
	PackedSplatData.Empty();
	SHData.Empty();
	TemporalData.Empty();
	KeyframeData.Empty();
	Native4DData.Empty();
	CachedChunkData.Empty();
	CachedClusterData.Empty();
	CachedSplatClusterIndices.Empty();

	bGPUBuffersCreated = true;

	UE_LOG(LogTemp, Verbose, TEXT("GaussianSplatRenderData: Created %d shared GPU buffers for asset '%s'"),
		SharedBufferCount, *AssetName);
}

void FGaussianSplatRenderData::ReleaseGPUBuffers()
{
	PackedSplatBuffer.SafeRelease();
	PackedSplatBufferSRV.SafeRelease();
	SHBuffer.SafeRelease();
	SHBufferSRV.SafeRelease();
	ChunkBuffer.SafeRelease();
	ChunkBufferSRV.SafeRelease();
	IndexBuffer.SafeRelease();
	ClusterBuffer.SafeRelease();
	ClusterBufferSRV.SafeRelease();
	TemporalBuffer.SafeRelease();
	TemporalBufferSRV.SafeRelease();
	KeyframeBuffer.SafeRelease();
	KeyframeBufferSRV.SafeRelease();
	Native4DBuffer.SafeRelease();
	Native4DBufferSRV.SafeRelease();
	SplatClusterIndexBuffer.SafeRelease();
	SplatClusterIndexBufferSRV.SafeRelease();
	bGPUBuffersCreated = false;
}
