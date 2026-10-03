// Copyright OpenSplat4D 2026 All Rights Reserved.
// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatFractureAsset.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatRenderData.h"

UGaussianSplatFractureAsset::UGaussianSplatFractureAsset()
{
}

// Defined here (not inline in the header) because TSoftObjectPtr<UGaussianSplatAsset>
// accessors require the complete UGaussianSplatAsset type.

UGaussianSplatAsset* UGaussianSplatFractureAsset::GetSourceSplatAsset() const
{
	return SourceSplatAsset.Get();
}

UGaussianSplatAsset* UGaussianSplatFractureAsset::LoadSourceSplatAsset() const
{
	return SourceSplatAsset.LoadSynchronous();
}

void UGaussianSplatFractureAsset::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	int32 Magic = GAUSSIAN_SPLAT_FRACTURE_ASSET_MAGIC;
	int32 Version = GAUSSIAN_SPLAT_FRACTURE_ASSET_VERSION;
	Ar << Magic;
	Ar << Version;

	// v1 assets (no voxel shell block) stay readable; anything outside the
	// known range is rejected so a stale package is never half-parsed. The
	// voxel shell fields themselves are UPROPERTYs, so a v1 package simply
	// leaves them at their constructor defaults.
	constexpr int32 MinSupportedVersion = 1;

	if (Ar.IsLoading() && (Magic != GAUSSIAN_SPLAT_FRACTURE_ASSET_MAGIC
		|| Version < MinSupportedVersion || Version > GAUSSIAN_SPLAT_FRACTURE_ASSET_VERSION))
	{
		UE_LOG(LogTemp, Error, TEXT("GaussianSplatFractureAsset: Incompatible asset format (magic 0x%08X version %d)"), Magic, Version);

		GeometryCollection = nullptr;
		bHasBoneBinding = false;
		BoneBindingBoneCount = 0;
		SourceSplatCount = 0;
		CapSplatCount = 0;
		VoxelShellVertexCount = 0;
		VoxelShellTriangleCount = 0;
		VoxelShellVoxelSize = 0.0f;
		VoxelShellBounds = FBox(ForceInit);
		BoneBindingBulkData.RemoveBulkData();
		CapSplatBulkData.RemoveBulkData();
		VoxelShellBulkData.RemoveBulkData();
		return;
	}

	Ar << bHasBoneBinding;
	Ar << BoneBindingBoneCount;
	Ar << SourceSplatCount;
	BoneBindingBulkData.Serialize(Ar, this);

	Ar << CapSplatCount;
	CapSplatBulkData.Serialize(Ar, this);

	if (Version >= 2)
	{
		Ar << VoxelShellVertexCount;
		Ar << VoxelShellTriangleCount;
		Ar << VoxelShellVoxelSize;
		Ar << VoxelShellBounds;
		VoxelShellBulkData.Serialize(Ar, this);
	}

	if (Version >= 3)
	{
		Ar << GridOriginOffset;
	}
}

// --- Collision voxel shell ---

const void* UGaussianSplatFractureAsset::LockVoxelShellDataReadOnly(int64* OutSize) const
{
	const int64 DataSize = VoxelShellBulkData.GetBulkDataSize();
	if (OutSize) *OutSize = DataSize;
	return (DataSize > 0) ? VoxelShellBulkData.LockReadOnly() : nullptr;
}

void UGaussianSplatFractureAsset::UnlockVoxelShellData() const
{
	if (VoxelShellBulkData.GetBulkDataSize() > 0) VoxelShellBulkData.Unlock();
}

bool UGaussianSplatFractureAsset::GetVoxelShellHeader(FGaussianSplatVoxelShellHeader& OutHeader) const
{
	if (!HasVoxelShell())
	{
		return false;
	}

	int64 DataSize = 0;
	const void* Data = LockVoxelShellDataReadOnly(&DataSize);
	if (!Data)
	{
		return false;
	}

	bool bOk = false;
	if (DataSize >= (int64)sizeof(FGaussianSplatVoxelShellHeader))
	{
		FMemory::Memcpy(&OutHeader, Data, sizeof(FGaussianSplatVoxelShellHeader));

		const int64 RequiredSize = (int64)sizeof(FGaussianSplatVoxelShellHeader)
			+ (int64)OutHeader.VertexCount * (int64)sizeof(FVector3f)
			+ (int64)OutHeader.TriangleCount * (int64)(3 * sizeof(int32));

		bOk = OutHeader.Magic == GAUSSIAN_SPLAT_VOXEL_SHELL_MAGIC
			&& OutHeader.Version == GAUSSIAN_SPLAT_VOXEL_SHELL_VERSION
			&& (int32)OutHeader.VertexCount == VoxelShellVertexCount
			&& (int32)OutHeader.TriangleCount == VoxelShellTriangleCount
			&& DataSize >= RequiredSize;
	}

	UnlockVoxelShellData();
	return bOk;
}

bool UGaussianSplatFractureAsset::GetVoxelShell(TArray<FVector3f>& OutVertices, TArray<int32>& OutIndices) const
{
	OutVertices.Reset();
	OutIndices.Reset();

	FGaussianSplatVoxelShellHeader Header;
	if (!GetVoxelShellHeader(Header))
	{
		return false;
	}

	int64 DataSize = 0;
	const void* Data = LockVoxelShellDataReadOnly(&DataSize);
	if (!Data)
	{
		return false;
	}

	OutVertices.SetNumUninitialized((int32)Header.VertexCount);
	OutIndices.SetNumUninitialized((int32)Header.TriangleCount * 3);

	const uint8* Cursor = static_cast<const uint8*>(Data) + sizeof(FGaussianSplatVoxelShellHeader);
	FMemory::Memcpy(OutVertices.GetData(), Cursor, (SIZE_T)OutVertices.Num() * sizeof(FVector3f));
	Cursor += (int64)OutVertices.Num() * (int64)sizeof(FVector3f);
	FMemory::Memcpy(OutIndices.GetData(), Cursor, (SIZE_T)OutIndices.Num() * sizeof(int32));

	UnlockVoxelShellData();
	return true;
}

void UGaussianSplatFractureAsset::SetVoxelShell(const TArray<FVector3f>& InVertices, const TArray<int32>& InIndices, float InVoxelSize)
{
	if (InVertices.Num() < 3 || InIndices.Num() < 3)
	{
		ClearVoxelShell();
		return;
	}

	if ((InIndices.Num() % 3) != 0)
	{
		UE_LOG(LogTemp, Error, TEXT("SetVoxelShell: index count %d is not a multiple of 3"), InIndices.Num());
		return;
	}

	const int32 VertexCount = InVertices.Num();
	const int32 TriangleCount = InIndices.Num() / 3;

	for (int32 i = 0; i < InIndices.Num(); i++)
	{
		if (InIndices[i] < 0 || InIndices[i] >= VertexCount)
		{
			UE_LOG(LogTemp, Error, TEXT("SetVoxelShell: index %d out of range (VertexCount=%d)"), i, VertexCount);
			return;
		}
	}

	const int64 HeaderSize = (int64)sizeof(FGaussianSplatVoxelShellHeader);
	const int64 VertexBytes = (int64)VertexCount * (int64)sizeof(FVector3f);
	const int64 IndexBytes = (int64)InIndices.Num() * (int64)sizeof(int32);
	const int64 TotalBytes = HeaderSize + VertexBytes + IndexBytes;
	if (TotalBytes > (int64)MAX_int32)
	{
		UE_LOG(LogTemp, Error, TEXT("SetVoxelShell: shell block too large (%lld bytes)"), TotalBytes);
		return;
	}

	FVector3f BoundsMin = InVertices[0];
	FVector3f BoundsMax = InVertices[0];
	for (int32 i = 1; i < VertexCount; i++)
	{
		BoundsMin = FVector3f::Min(BoundsMin, InVertices[i]);
		BoundsMax = FVector3f::Max(BoundsMax, InVertices[i]);
	}

	VoxelShellBulkData.Lock(LOCK_READ_WRITE);
	uint8* Dest = static_cast<uint8*>(VoxelShellBulkData.Realloc(TotalBytes));

	FGaussianSplatVoxelShellHeader Header;
	Header.Magic = GAUSSIAN_SPLAT_VOXEL_SHELL_MAGIC;
	Header.Version = GAUSSIAN_SPLAT_VOXEL_SHELL_VERSION;
	Header.VertexCount = (uint32)VertexCount;
	Header.TriangleCount = (uint32)TriangleCount;
	Header.VoxelSize = InVoxelSize;
	Header.BoundsMin = BoundsMin;
	Header.BoundsMax = BoundsMax;

	FMemory::Memcpy(Dest, &Header, sizeof(FGaussianSplatVoxelShellHeader));
	FMemory::Memcpy(Dest + HeaderSize, InVertices.GetData(), (SIZE_T)VertexBytes);
	FMemory::Memcpy(Dest + HeaderSize + VertexBytes, InIndices.GetData(), (SIZE_T)IndexBytes);

	VoxelShellBulkData.Unlock();
	VoxelShellBulkData.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload);

	VoxelShellVertexCount = VertexCount;
	VoxelShellTriangleCount = TriangleCount;
	VoxelShellVoxelSize = InVoxelSize;
	VoxelShellBounds = FBox(FVector(BoundsMin), FVector(BoundsMax));

	// The shell no longer matches the solved fracture, but it does not feed the
	// render data either, so only the package is marked dirty here.
#if WITH_EDITOR
	MarkPackageDirty();
#endif

	UE_LOG(LogTemp, Log, TEXT("SetVoxelShell: fracture '%s' stored a %d-vertex / %d-triangle shell (voxel %.2f, %lld bytes)"),
		*GetName(), VertexCount, TriangleCount, InVoxelSize, TotalBytes);
}

void UGaussianSplatFractureAsset::ClearVoxelShell()
{
	if (VoxelShellVertexCount == 0 && VoxelShellTriangleCount == 0 && VoxelShellBulkData.GetBulkDataSize() == 0)
	{
		return;
	}

	VoxelShellBulkData.RemoveBulkData();
	VoxelShellVertexCount = 0;
	VoxelShellTriangleCount = 0;
	VoxelShellVoxelSize = 0.0f;
	VoxelShellBounds = FBox(ForceInit);

#if WITH_EDITOR
	MarkPackageDirty();
#endif

	UE_LOG(LogTemp, Log, TEXT("ClearVoxelShell: fracture '%s' no longer has a collision shell"), *GetName());
}

// --- Bone binding ---

const void* UGaussianSplatFractureAsset::LockBoneBindingDataReadOnly(int64* OutSize) const
{
	const int64 DataSize = BoneBindingBulkData.GetBulkDataSize();
	if (OutSize) *OutSize = DataSize;
	return (DataSize > 0) ? BoneBindingBulkData.LockReadOnly() : nullptr;
}

void UGaussianSplatFractureAsset::UnlockBoneBindingData() const
{
	if (BoneBindingBulkData.GetBulkDataSize() > 0) BoneBindingBulkData.Unlock();
}

const void* UGaussianSplatFractureAsset::LockCapSplatDataReadOnly(int64* OutSize) const
{
	const int64 DataSize = CapSplatBulkData.GetBulkDataSize();
	if (OutSize) *OutSize = DataSize;
	return (DataSize > 0) ? CapSplatBulkData.LockReadOnly() : nullptr;
}

void UGaussianSplatFractureAsset::UnlockCapSplatData() const
{
	if (CapSplatBulkData.GetBulkDataSize() > 0) CapSplatBulkData.Unlock();
}

bool UGaussianSplatFractureAsset::GetBoneBindingHeader(FGaussianSplatBoneBindingHeader& OutHeader) const
{
	if (!HasBoneBinding())
	{
		return false;
	}

	int64 DataSize = 0;
	const void* Data = LockBoneBindingDataReadOnly(&DataSize);
	if (!Data)
	{
		return false;
	}

	bool bOk = false;
	if (DataSize >= (int64)sizeof(FGaussianSplatBoneBindingHeader))
	{
		FMemory::Memcpy(&OutHeader, Data, sizeof(FGaussianSplatBoneBindingHeader));

		// The index block must cover every splat of the source asset (and every
		// cap splat, which the renderer appends after the LOD splats); anything
		// shorter means the fracture was rebound after the splat count changed
		// or is corrupt.
		const int64 RequiredSize = (int64)GetBoneIndexBlockOffset(BoneBindingBoneCount)
			+ (int64)(OutHeader.SplatCount + (uint32)CapSplatCount) * (int64)sizeof(uint32);

		bOk = OutHeader.Magic == GAUSSIAN_SPLAT_BONE_BINDING_MAGIC
			&& OutHeader.Version == GAUSSIAN_SPLAT_BONE_BINDING_VERSION
			&& OutHeader.BoneCount == (uint32)BoneBindingBoneCount
			&& OutHeader.SplatCount == (uint32)SourceSplatCount
			&& DataSize >= RequiredSize;
	}

	UnlockBoneBindingData();
	return bOk;
}

bool UGaussianSplatFractureAsset::GetRestBoneMatrices(TArray<FMatrix44f>& OutMatrices) const
{
	OutMatrices.Reset();

	FGaussianSplatBoneBindingHeader Header;
	if (!GetBoneBindingHeader(Header))
	{
		return false;
	}

	int64 DataSize = 0;
	const void* Data = LockBoneBindingDataReadOnly(&DataSize);
	if (!Data)
	{
		return false;
	}

	const int32 BoneCount = BoneBindingBoneCount;
	OutMatrices.SetNumUninitialized(BoneCount);
	FMemory::Memcpy(OutMatrices.GetData(),
		static_cast<const uint8*>(Data) + sizeof(FGaussianSplatBoneBindingHeader),
		(SIZE_T)BoneCount * sizeof(FMatrix44f));

	UnlockBoneBindingData();
	return true;
}

bool UGaussianSplatFractureAsset::SetBoneBinding(const TArray<FMatrix44f>& RestBoneMatrices, const TArray<uint32>& SplatBoneIndices,
	const FVector3f& BoundsMin, const FVector3f& BoundsMax, const TArray<uint32>& CapBoneIndices)
{
	const int32 BoneCount = RestBoneMatrices.Num();
	if (BoneCount <= 0 || BoneCount > 65535)
	{
		UE_LOG(LogTemp, Error, TEXT("SetBoneBinding: invalid bone count %d (must be 1..65535)"), BoneCount);
		return false;
	}

	const int32 SplatCount = SplatBoneIndices.Num();
	if (SplatCount <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("SetBoneBinding: empty splat index array"));
		return false;
	}

	if (CapBoneIndices.Num() != CapSplatCount)
	{
		UE_LOG(LogTemp, Error, TEXT("SetBoneBinding: cap index count %d does not match CapSplatCount %d"),
			CapBoneIndices.Num(), CapSplatCount);
		return false;
	}

	for (int32 i = 0; i < SplatBoneIndices.Num(); i++)
	{
		// The bone index lives in the low 16 bits; bit 16 is the "outside the
		// estimation grid, do not render" flag (see the header), so only the
		// index part is range checked.
		if ((SplatBoneIndices[i] & 0xFFFFu) >= (uint32)BoneCount)
		{
			UE_LOG(LogTemp, Error, TEXT("SetBoneBinding: splat %d references bone %u out of range (BoneCount=%d)"),
				i, SplatBoneIndices[i] & 0xFFFFu, BoneCount);
			return false;
		}
	}

	for (int32 i = 0; i < CapBoneIndices.Num(); i++)
	{
		if (CapBoneIndices[i] >= (uint32)BoneCount)
		{
			UE_LOG(LogTemp, Error, TEXT("SetBoneBinding: cap %d references bone %u out of range (BoneCount=%d)"),
				i, CapBoneIndices[i], BoneCount);
			return false;
		}
	}

	const int32 TotalIndexCount = SplatCount + CapSplatCount;
	const int64 HeaderSize = (int64)sizeof(FGaussianSplatBoneBindingHeader);
	const int64 MatrixBytes = (int64)BoneCount * (int64)sizeof(FMatrix44f);
	const int64 IndexBytes = (int64)TotalIndexCount * (int64)sizeof(uint32);
	const int64 TotalBytes = HeaderSize + MatrixBytes + IndexBytes;
	if (TotalBytes > (int64)MAX_int32)
	{
		UE_LOG(LogTemp, Error, TEXT("SetBoneBinding: binding block too large (%lld bytes)"), TotalBytes);
		return false;
	}

	BoneBindingBulkData.Lock(LOCK_READ_WRITE);
	uint8* Dest = static_cast<uint8*>(BoneBindingBulkData.Realloc(TotalBytes));

	FGaussianSplatBoneBindingHeader Header;
	Header.Magic = GAUSSIAN_SPLAT_BONE_BINDING_MAGIC;
	Header.Version = GAUSSIAN_SPLAT_BONE_BINDING_VERSION;
	Header.BoneCount = (uint32)BoneCount;
	Header.SplatCount = (uint32)SplatCount;
	Header.BoundsMin = BoundsMin;
	Header.BoundsMax = BoundsMax;

	FMemory::Memcpy(Dest, &Header, sizeof(FGaussianSplatBoneBindingHeader));
	FMemory::Memcpy(Dest + HeaderSize, RestBoneMatrices.GetData(), (SIZE_T)MatrixBytes);

	// Store the 16-bit bone index in the low half of a 4-byte word so the
	// shader can use an aligned ByteAddressBuffer.Load. Cap splats follow the
	// real splats so the whole packed GPU buffer (splats + LOD + caps) can be
	// indexed with the same buffer.
	uint32* IndexDest = reinterpret_cast<uint32*>(Dest + HeaderSize + MatrixBytes);
	for (int32 i = 0; i < SplatCount; i++)
	{
		// Keep both the low 16-bit bone index and bit 16 (the estimation-grid
		// "do not render" flag). Masking with 0xFFFF alone turned every dropped
		// splat into bone 0, so the render shader never culled them.
		IndexDest[i] = SplatBoneIndices[i] & (0xFFFFu | GAUSSIAN_SPLAT_BONE_INDEX_DROPPED);
	}
	for (int32 i = 0; i < CapSplatCount; i++)
	{
		IndexDest[SplatCount + i] = CapBoneIndices[i] & 0xFFFFu;
	}

	BoneBindingBulkData.Unlock();
	BoneBindingBulkData.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload);

	bHasBoneBinding = true;
	BoneBindingBoneCount = BoneCount;
	SourceSplatCount = SplatCount;

	// The binding is baked into a static GPU buffer, so the cached render data
	// has to be rebuilt around it.
	InvalidateRenderData();
#if WITH_EDITOR
	MarkPackageDirty();
#endif

	UE_LOG(LogTemp, Log, TEXT("SetBoneBinding: fracture '%s' bound to %d bones (%d splats, %lld bytes)"),
		*GetName(), BoneCount, SplatCount, TotalBytes);
	return true;
}

void UGaussianSplatFractureAsset::ClearBoneBinding()
{
	if (!bHasBoneBinding && BoneBindingBulkData.GetBulkDataSize() == 0)
	{
		return;
	}

	BoneBindingBulkData.RemoveBulkData();
	bHasBoneBinding = false;
	BoneBindingBoneCount = 0;
	SourceSplatCount = 0;

	InvalidateRenderData();
#if WITH_EDITOR
	MarkPackageDirty();
#endif

	UE_LOG(LogTemp, Log, TEXT("ClearBoneBinding: fracture '%s' is no longer bound"), *GetName());
}

void UGaussianSplatFractureAsset::SetFractureCaps(const TArray<FGaussianSplatCap>& InCaps)
{
	if (InCaps.Num() == 0)
	{
		if (CapSplatCount == 0 && CapSplatBulkData.GetBulkDataSize() == 0)
		{
			return;
		}

		CapSplatBulkData.RemoveBulkData();
		CapSplatCount = 0;

		InvalidateRenderData();
#if WITH_EDITOR
		MarkPackageDirty();
#endif
		return;
	}

	const int64 TotalBytes = (int64)InCaps.Num() * (int64)CapSplatStride;
	if (TotalBytes > (int64)MAX_int32)
	{
		UE_LOG(LogTemp, Error, TEXT("SetFractureCaps: cap block too large (%lld bytes)"), TotalBytes);
		return;
	}

	CapSplatBulkData.Lock(LOCK_READ_WRITE);
	float* Dest = reinterpret_cast<float*>(CapSplatBulkData.Realloc(TotalBytes));

	for (int32 i = 0; i < InCaps.Num(); i++)
	{
		const FGaussianSplatCap& Cap = InCaps[i];
		float* Dst = Dest + (int64)i * (CapSplatStride / sizeof(float));

		Dst[0] = Cap.Position.X;
		Dst[1] = Cap.Position.Y;
		Dst[2] = Cap.Position.Z;
		Dst[3] = Cap.Rotation.X;
		Dst[4] = Cap.Rotation.Y;
		Dst[5] = Cap.Rotation.Z;
		Dst[6] = Cap.Rotation.W;
		Dst[7] = Cap.Scale.X;
		Dst[8] = Cap.Scale.Y;
		Dst[9] = Cap.Scale.Z;
		Dst[10] = Cap.Color.R;
		Dst[11] = Cap.Color.G;
		Dst[12] = Cap.Color.B;
		Dst[13] = Cap.Color.A;
	}

	CapSplatBulkData.Unlock();
	CapSplatBulkData.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload);

	CapSplatCount = InCaps.Num();

	// Caps are baked into the packed GPU buffer, so the cached render data has
	// to be rebuilt around them.
	InvalidateRenderData();
#if WITH_EDITOR
	MarkPackageDirty();
#endif

	UE_LOG(LogTemp, Log, TEXT("SetFractureCaps: fracture '%s' stored %d cap splats (%lld bytes)"),
		*GetName(), CapSplatCount, TotalBytes);
}

// --- Render data ---

TSharedPtr<FGaussianSplatRenderData> UGaussianSplatFractureAsset::GetOrCreateRenderData()
{
	if (RenderData.IsValid() && RenderData->IsInitialized())
	{
		return RenderData;
	}

	UGaussianSplatAsset* Source = GetSourceSplatAsset();
	if (!Source || !Source->IsValid())
	{
		return nullptr;
	}

	RenderData = MakeShared<FGaussianSplatRenderData>();
	RenderData->Initialize(Source, this);
	return RenderData;
}

void UGaussianSplatFractureAsset::InvalidateRenderData()
{
	RenderData.Reset();

	// Scene proxies copy the shared render data when they are built, so a live
	// proxy keeps the old binding until it is rebuilt. Tell the listeners (the
	// splat component) to recreate their proxies around the new data.
	OnFractureChanged.Broadcast(this);
}
