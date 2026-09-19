// Copyright Epic Games, Inc. All Rights Reserved.

#include "NanoGSGaussianSplatComponent.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatSceneProxy.h"
#include "GaussianSplatViewExtension.h"
#include "Engine/World.h"
#include "PhysicsEngine/BodySetup.h"
#include "HitProxies.h"

UGaussianSplatComponent::UGaussianSplatComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Tick is only enabled at runtime for 4D playback (Update4DTickEnabled);
	// static 3D assets keep it disabled so there is no per-frame CPU cost.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;

	bUseAsOccluder = false;
	SetGenerateOverlapEvents(false);

	// Default to no collision; user can enable via CollisionMethod property
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);

	// Enable dynamic rendering
	Mobility = EComponentMobility::Movable;

	// Shadow proxy detail: 0=Bounding box, 1=Convex hull, 2=Full
	ShadowProxyDetail = 1;
}

void UGaussianSplatComponent::PostLoad()
{
	Super::PostLoad();
}

#if WITH_EDITOR
void UGaussianSplatComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SplatAsset))
	{
		OnAssetChanged();
		bCollisionDirty = true;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SHOrder) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, OpacityScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SplatScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LODErrorThreshold) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, MaxDrawDistance) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FadeOutStartDistance) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bCastShadow) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ShadowIntensity))
	{
		MarkRenderStateDirty();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CollisionMethod) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CollisionMaxFaces) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, IgnoreFactor) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, VoxelSize))
	{
		bCollisionDirty = true;
		RebuildCollision();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CurrentTime) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bPlaying) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bLooping) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, PlayRate) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bAutoPlay))
	{
		// 4D playback props: push time to the proxy live instead of rebuilding it
		Update4DTickEnabled();
		PushCurrentTimeToProxy();
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif

void UGaussianSplatComponent::OnRegister()
{
	Super::OnRegister();

	if (SplatAsset)
	{
		bBoundsCached = false;
		SubscribeToAssetChanges();
	}

	// 4D: start playback automatically if requested
	if (Supports4DPlayback() && bAutoPlay && !bPlaying)
	{
		Play4D();
	}
	Update4DTickEnabled();
}

void UGaussianSplatComponent::OnUnregister()
{
	UnsubscribeFromAssetChanges();
	Super::OnUnregister();
}

void UGaussianSplatComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bPlaying || !SplatAsset || !SplatAsset->Is4D())
	{
		return;
	}

	const float TimeStart = SplatAsset->TimeStart;
	const float TimeEnd = SplatAsset->TimeEnd;
	const float Duration = TimeEnd - TimeStart;
	if (Duration <= 0.f)
	{
		return;
	}

	CurrentTime += DeltaTime * PlayRate;

	if (bLooping)
	{
		// Wrap into [TimeStart, TimeEnd]
		CurrentTime = TimeStart + FMath::Fmod(CurrentTime - TimeStart, Duration);
		if (CurrentTime < TimeStart)
		{
			CurrentTime += Duration;
		}
	}
	else
	{
		CurrentTime = FMath::Clamp(CurrentTime, TimeStart, TimeEnd);
		if (CurrentTime >= TimeEnd)
		{
			bPlaying = false;
			Update4DTickEnabled();
		}
	}

	PushCurrentTimeToProxy();
}

// ---------------------------------------------------------------------------
// 4D playback
// ---------------------------------------------------------------------------

bool UGaussianSplatComponent::Supports4DPlayback() const
{
	return SplatAsset && SplatAsset->Is4D() && (SplatAsset->TimeEnd > SplatAsset->TimeStart);
}

void UGaussianSplatComponent::Play4D()
{
	if (!Supports4DPlayback())
	{
		UE_LOG(LogTemp, Verbose, TEXT("GaussianSplat: Play4D ignored -- asset has no 4D temporal data"));
		return;
	}
	bPlaying = true;
	Update4DTickEnabled();
	PushCurrentTimeToProxy();
}

void UGaussianSplatComponent::Pause4D()
{
	bPlaying = false;
	Update4DTickEnabled();
}

void UGaussianSplatComponent::Stop4D()
{
	bPlaying = false;
	if (SplatAsset)
	{
		CurrentTime = SplatAsset->TimeStart;
	}
	Update4DTickEnabled();
	PushCurrentTimeToProxy();
}

void UGaussianSplatComponent::SetPlaybackTime(float InTime)
{
	if (SplatAsset)
	{
		CurrentTime = FMath::Clamp(InTime, SplatAsset->TimeStart, SplatAsset->TimeEnd);
	}
	else
	{
		CurrentTime = InTime;
	}
	PushCurrentTimeToProxy();
}

void UGaussianSplatComponent::Update4DTickEnabled()
{
	SetComponentTickEnabled(bPlaying && Supports4DPlayback());
}

void UGaussianSplatComponent::PushCurrentTimeToProxy()
{
	if (FGaussianSplatSceneProxy* SplatProxy = static_cast<FGaussianSplatSceneProxy*>(SceneProxy))
	{
		SplatProxy->SetCurrentTime(CurrentTime);
	}
}

FPrimitiveSceneProxy* UGaussianSplatComponent::CreateSceneProxy()
{
	if (!SplatAsset || !SplatAsset->IsValid())
	{
		return nullptr;
	}

	// Allow scene proxy creation in preview worlds (for asset editor viewport)
	// Only skip for PIE worlds that are not the main game world
	UWorld* World = GetWorld();
	if (World)
	{
		EWorldType::Type WorldType = World->WorldType;
		// Allow EditorPreview for asset editor viewport
		// Skip only for GamePreview (PIE) to avoid duplicate rendering
		if (WorldType == EWorldType::GamePreview)
		{
			return nullptr;
		}
	}

	return new FGaussianSplatSceneProxy(this);
}

FBoxSphereBounds UGaussianSplatComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (SplatAsset && SplatAsset->IsValid())
	{
		FBox LocalBox = SplatAsset->GetBounds();

		// Transform to world space
		FBox WorldBox = LocalBox.TransformBy(LocalToWorld);

		return FBoxSphereBounds(WorldBox);
	}

	// Return small default bounds if no asset
	return FBoxSphereBounds(FVector::ZeroVector, FVector(100.0f), 100.0f);
}

void UGaussianSplatComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	// Gaussian splatting doesn't use traditional materials
	// But we might add a material for composite pass later
}

void UGaussianSplatComponent::SetSplatAsset(UGaussianSplatAsset* NewAsset)
{
	if (SplatAsset != NewAsset)
	{
		// Unsubscribe from old asset
		UnsubscribeFromAssetChanges();

		SplatAsset = NewAsset;

		// Subscribe to new asset
		if (IsRegistered())
		{
			SubscribeToAssetChanges();
		}

		OnAssetChanged();
	}
}

int32 UGaussianSplatComponent::GetSplatCount() const
{
	return SplatAsset ? SplatAsset->GetSplatCount() : 0;
}

void UGaussianSplatComponent::OnAssetChanged()
{
	bBoundsCached = false;
	UpdateBounds();
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::MarkRenderStateDirty()
{
	MarkRenderDynamicDataDirty();

	if (IsRegistered())
	{
		Super::MarkRenderStateDirty();
	}
}

void UGaussianSplatComponent::OnAssetDataChanged(UGaussianSplatAsset* ChangedAsset)
{
	// Only respond if this is our asset
	if (ChangedAsset == SplatAsset)
	{
		UE_LOG(LogTemp, Log, TEXT("GaussianSplat: Asset data changed (Nanite state), recreating scene proxy"));

		// Invalidate cached bounds since splat count may have changed
		bBoundsCached = false;
		UpdateBounds();

		// Recreate the scene proxy with updated asset data
		// This will cause CreateSceneProxy to be called again with the new Nanite state
		MarkRenderStateDirty();
	}
}

void UGaussianSplatComponent::SubscribeToAssetChanges()
{
	if (SplatAsset && !AssetChangedDelegateHandle.IsValid())
	{
		AssetChangedDelegateHandle = SplatAsset->OnAssetChanged.AddUObject(this, &UGaussianSplatComponent::OnAssetDataChanged);
		UE_LOG(LogTemp, Verbose, TEXT("GaussianSplat: Subscribed to asset change notifications"));
	}
}

void UGaussianSplatComponent::UnsubscribeFromAssetChanges()
{
	if (SplatAsset && AssetChangedDelegateHandle.IsValid())
	{
		SplatAsset->OnAssetChanged.Remove(AssetChangedDelegateHandle);
		AssetChangedDelegateHandle.Reset();
		UE_LOG(LogTemp, Verbose, TEXT("GaussianSplat: Unsubscribed from asset change notifications"));
	}
}

//~ Shadow and Collision methods

UBodySetup* UGaussianSplatComponent::GetBodySetup()
{
	if (bCollisionDirty && CollisionMethod != EGaussianCollisionMethod::None)
	{
		RebuildCollision();
	}
	return BodySetup;
}

bool UGaussianSplatComponent::LineTraceComponent(FHitResult& OutHit, const FVector TraceStart, const FVector TraceEnd, const FCollisionQueryParams& TraceParams)
{
	if (CollisionMethod == EGaussianCollisionMethod::None || !BodySetup)
	{
		return false;
	}

	// Delegate to the default implementation which uses BodySetup's cooked physics mesh
	// Note: UPrimitiveComponent::LineTraceComponent handles the BodySetup collision test
	// through the physics engine (Chaos), so we just need to ensure BodySetup is properly
	// cooked via CreatePhysicsMeshes() in RebuildCollision().
	return Super::LineTraceComponent(OutHit, TraceStart, TraceEnd, TraceParams);
}

void UGaussianSplatComponent::RebuildCollision()
{
	if (!SplatAsset || !SplatAsset->IsValid())
	{
		return;
	}

	if (CollisionMethod == EGaussianCollisionMethod::None)
	{
		SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
		BodySetup = nullptr;
		bCollisionDirty = false;
		return;
	}

	// Create body setup if needed
	if (!BodySetup)
	{
		BodySetup = NewObject<UBodySetup>(this, NAME_None, RF_Transient);
		BodySetup->BodySetupGuid = FGuid::NewGuid();
	}
	else
	{
		BodySetup->AggGeom.EmptyElements();
	}

	// BoundingBox method: use the asset's cached bounds directly, avoiding the
	// O(N) decompression + copy of all splat positions (can be millions of points).
	// This is the cheapest collision method and should stay allocation-free.
	if (CollisionMethod == EGaussianCollisionMethod::BoundingBox)
	{
		const FBox LocalBounds = SplatAsset->GetBounds();
		if (!LocalBounds.IsValid || LocalBounds.GetSize().IsNearlyZero())
		{
			UE_LOG(LogTemp, Warning, TEXT("GaussianSplat: Asset bounds invalid for BoundingBox collision"));
			return;
		}
		FKBoxElem BoxElem;
		BoxElem.Center = LocalBounds.GetCenter();
		BoxElem.X = LocalBounds.GetExtent().X * 2.0f;
		BoxElem.Y = LocalBounds.GetExtent().Y * 2.0f;
		BoxElem.Z = LocalBounds.GetExtent().Z * 2.0f;
		BodySetup->AggGeom.BoxElems.Add(BoxElem);

		BodySetup->bGenerateMirroredCollision = false;
		BodySetup->bDoubleSidedGeometry = true;
		BodySetup->InvalidatePhysicsData();
		BodySetup->CreatePhysicsMeshes();

		SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		bCollisionDirty = false;

		UE_LOG(LogTemp, Log, TEXT("GaussianSplat: BoundingBox collision rebuilt (bounds=%s)"), *LocalBounds.GetExtent().ToString());
		return;
	}

	// All other methods need the actual point positions.
	TArray<FVector> Points = SplatAsset->GetDecompressedPositions();
	if (Points.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("GaussianSplat: No points available for collision generation"));
		return;
	}

	// Apply ignore factor: skip points pseudo-randomly to reduce collision geometry
	// complexity. Uses a hash (not rand()) so the same input yields deterministic
	// output across runs (reproducible collision shapes).
	if (IgnoreFactor > 0.0f && IgnoreFactor < 1.0f)
	{
		TArray<FVector> FilteredPoints;
		FilteredPoints.Reserve(FMath::Max(1, FMath::TruncToInt(Points.Num() * (1.0f - IgnoreFactor))));
		const uint32 Threshold = (uint32)(IgnoreFactor * 1000.0f);
		for (int32 i = 0; i < Points.Num(); i++)
		{
			// Knuth multiplicative hash - fast and well-distributed for sequential ints
			if ((i * 2654435761u) % 1000u >= Threshold)
			{
				FilteredPoints.Add(Points[i]);
			}
		}
		if (FilteredPoints.Num() > 0)
		{
			Points = MoveTemp(FilteredPoints);
		}
	}

	TArray<FVector> Vertices;
	TArray<int32> Indices;
	bool bSuccess = false;

	switch (CollisionMethod)
	{
	case EGaussianCollisionMethod::ConvexHull:
		bSuccess = GenerateConvexHull(Points, Vertices, Indices);
		break;
	case EGaussianCollisionMethod::ConvexDecomposition:
		bSuccess = GenerateSimplifiedCollision(Points, Vertices, Indices);
		break;
	case EGaussianCollisionMethod::Voxel:
		bSuccess = GenerateVoxelCollision(Points, Vertices, Indices);
		break;
	default:
		break;
	}

	if (bSuccess && Vertices.Num() > 0)
	{
		// Add as convex hull - VertexData is the source of truth, UpdateElemBox
		// recomputes the bounding box. CreatePhysicsMeshes() below cooks it for Chaos.
		FKConvexElem& ConvexElem = BodySetup->AggGeom.ConvexElems.AddDefaulted_GetRef();
		ConvexElem.VertexData = MoveTemp(Vertices);
		ConvexElem.UpdateElemBox();
	}

	BodySetup->bGenerateMirroredCollision = false;
	BodySetup->bDoubleSidedGeometry = true;
	BodySetup->InvalidatePhysicsData();
	BodySetup->CreatePhysicsMeshes();

	// Enable collision
	SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	bCollisionDirty = false;

	UE_LOG(LogTemp, Log, TEXT("GaussianSplat: Collision rebuilt (method=%d, points=%d, verts=%d, indices=%d)"),
		(uint8)CollisionMethod, Points.Num(), Vertices.Num(), Indices.Num());
}

void UGaussianSplatComponent::BuildCollisionBodySetup()
{
	RebuildCollision();
}

bool UGaussianSplatComponent::GenerateConvexHull(const TArray<FVector>& Points, TArray<FVector>& OutVertices, TArray<int32>& OutIndices)
{
	// ============================================================================
	// WARNING: STUB IMPLEMENTATION — NOT A REAL CONVEX HULL
	// ============================================================================
	// This is a placeholder that simply samples input points. It does NOT compute
	// a true convex hull and the result may be non-convex, contain interior points,
	// and produce incorrect collision behavior (e.g. false positives, missed
	// contacts, broken physics simulation).
	//
	// Use cases affected:
	//   - ConvexHull collision method (CollisionMethod == EGaussianCollisionMethod::ConvexHull)
	//   - Simplified collision (falls through to this stub)
	//
	// For production use, replace this with a real algorithm such as QuickHull
	// (see FConvexVolume::ComputeConvexHull in Engine) or integrate a third-party
	// library (e.g. V-HACD for decomposed convex hulls). The placeholder exists
	// only to avoid crashes when users select this collision method on a splat
	// asset; it should not be relied upon for accurate physics.
	//
	// TODO: Implement real QuickHull. For now, prefer BoundingBox collision
	// (cheap, correct for simple cases) or Voxel collision (approximate but
	// bounded) until this is replaced.
	// ============================================================================
	if (Points.Num() < 4)
	{
		return false;
	}

	// Placeholder: sample points uniformly. NOT a convex hull — kept only so the
	// collision pipeline has *something* to render into BodySetup instead of
	// failing silently. OutIndices is left empty, which means no convex element
	// will actually be created in the BodySetup (see RebuildCollision).
	int32 TargetCount = FMath::Min(Points.Num(), CollisionMaxFaces * 4);

	if (Points.Num() > TargetCount)
	{
		OutVertices.Reserve(TargetCount);
		float Step = (float)Points.Num() / TargetCount;
		for (int32 i = 0; i < TargetCount; i++)
		{
			int32 Idx = (int32)(i * Step);
			OutVertices.Add(Points[Idx]);
		}
	}
	else
	{
		OutVertices = Points;
	}

	UE_LOG(LogTemp, Warning, TEXT("GaussianSplat: ConvexHull collision is a STUB (not a real convex hull). ")
		TEXT("Returned %d sampled points — physics may be incorrect. Use BoundingBox or Voxel collision for now."), OutVertices.Num());
	return true;
}

bool UGaussianSplatComponent::GenerateSimplifiedCollision(const TArray<FVector>& Points, TArray<FVector>& OutVertices, TArray<int32>& OutIndices)
{
	// ============================================================================
	// WARNING: STUB — DELEGATES TO GenerateConvexHull (also a stub)
	// ============================================================================
	// A proper Simplified collision would use V-HACD (Volumetric Hierarchical
	// Approximate Convex Decomposition) to produce multiple small convex hulls
	// that together approximate the original geometry. This is critical for
	// non-convex splat assets (e.g. a statue, a room interior) where a single
	// convex hull would be wildly inaccurate.
	//
	// Until V-HACD or equivalent is integrated, this method falls through to
	// the convex-hull stub, which itself is just point sampling. Treat the
	// Simplified collision method as "best-effort, not for production" and
	// prefer Voxel or BoundingBox collision.
	// ============================================================================
	if (Points.Num() < 4)
	{
		return false;
	}

	return GenerateConvexHull(Points, OutVertices, OutIndices);
}

bool UGaussianSplatComponent::GenerateVoxelCollision(const TArray<FVector>& Points, TArray<FVector>& OutVertices, TArray<int32>& OutIndices)
{
	// Voxel-based collision: create a voxel grid and generate boxes for occupied cells
	if (Points.Num() == 0)
	{
		return false;
	}

	FBox LocalBounds(Points);
	FVector Extent = LocalBounds.GetExtent();
	FVector Origin = LocalBounds.Min;

	// Calculate grid dimensions
	int32 GridX = FMath::Max(1, (int32)(Extent.X / VoxelSize) + 1);
	int32 GridY = FMath::Max(1, (int32)(Extent.Y / VoxelSize) + 1);
	int32 GridZ = FMath::Max(1, (int32)(Extent.Z / VoxelSize) + 1);

	// Mark occupied cells
	TArray<bool> Occupied;
	Occupied.SetNum(GridX * GridY * GridZ);
	FMemory::Memzero(Occupied.GetData(), Occupied.Num() * sizeof(bool));

	for (const FVector& P : Points)
	{
		int32 gx = FMath::Clamp((int32)((P.X - Origin.X) / VoxelSize), 0, GridX - 1);
		int32 gy = FMath::Clamp((int32)((P.Y - Origin.Y) / VoxelSize), 0, GridY - 1);
		int32 gz = FMath::Clamp((int32)((P.Z - Origin.Z) / VoxelSize), 0, GridZ - 1);
		Occupied[gx + gy * GridX + gz * GridX * GridY] = true;
	}

	// Add box elements for occupied cells (limit count for performance)
	int32 CellCount = 0;
	int32 MaxCells = FMath::Min(CollisionMaxFaces, GridX * GridY * GridZ);

	for (int32 z = 0; z < GridZ && CellCount < MaxCells; z++)
	{
		for (int32 y = 0; y < GridY && CellCount < MaxCells; y++)
		{
			for (int32 x = 0; x < GridX && CellCount < MaxCells; x++)
			{
				if (Occupied[x + y * GridX + z * GridX * GridY])
				{
					FKBoxElem BoxElem;
					BoxElem.Center = FVector(
						Origin.X + (x + 0.5f) * VoxelSize,
						Origin.Y + (y + 0.5f) * VoxelSize,
						Origin.Z + (z + 0.5f) * VoxelSize
					);
					BoxElem.X = VoxelSize;
					BoxElem.Y = VoxelSize;
					BoxElem.Z = VoxelSize;
					BodySetup->AggGeom.BoxElems.Add(BoxElem);
					CellCount++;
				}
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("GaussianSplat: Voxel collision generated with %d cells (%dx%dx%d grid)"),
		CellCount, GridX, GridY, GridZ);

	// Voxel method adds boxes directly, no need for vertices/indices
	OutVertices.Reset();
	OutIndices.Reset();
	return true;
}