// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatComponent.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatSceneProxy.h"
#include "GaussianSplatViewExtension.h"
#include "Engine/World.h"
#include "PhysicsEngine/BodySetup.h"
#include "HitProxies.h"

UGaussianSplatComponent::UGaussianSplatComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

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
}

void UGaussianSplatComponent::OnUnregister()
{
	UnsubscribeFromAssetChanges();
	Super::OnUnregister();
}

void UGaussianSplatComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
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

	TArray<FVector> Points = SplatAsset->GetDecompressedPositions();
	if (Points.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("GaussianSplat: No points available for collision generation"));
		return;
	}

	// Apply ignore factor: skip points based on density
	if (IgnoreFactor > 0.0f && IgnoreFactor < 1.0f)
	{
		TArray<FVector> FilteredPoints;
		FilteredPoints.Reserve(Points.Num() * (1.0f - IgnoreFactor));
		for (int32 i = 0; i < Points.Num(); i++)
		{
			// Use simple hash to skip points pseudo-randomly
			if ((i * 2654435761u) % 1000 >= (uint32)(IgnoreFactor * 1000))
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
	case EGaussianCollisionMethod::BoundingBox:
		{
			// Simple bounding box collision
			FBox LocalBounds(Points);
			FKBoxElem BoxElem;
			BoxElem.Center = LocalBounds.GetCenter();
			BoxElem.X = LocalBounds.GetExtent().X * 2;
			BoxElem.Y = LocalBounds.GetExtent().Y * 2;
			BoxElem.Z = LocalBounds.GetExtent().Z * 2;
			BodySetup->AggGeom.BoxElems.Add(BoxElem);
			bSuccess = true;
		}
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
	// Simple convex hull using gift wrapping algorithm
	if (Points.Num() < 4)
	{
		return false;
	}

	// For simplicity, use all points as convex hull vertices
	// A proper implementation would use QuickHull or similar
	// For now, we sample points to reduce count
	int32 TargetCount = FMath::Min(Points.Num(), CollisionMaxFaces * 4);

	if (Points.Num() > TargetCount)
	{
		// Sample points evenly
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

	UE_LOG(LogTemp, Log, TEXT("GaussianSplat: Convex hull generated with %d vertices"), OutVertices.Num());
	return true;
}

bool UGaussianSplatComponent::GenerateSimplifiedCollision(const TArray<FVector>& Points, TArray<FVector>& OutVertices, TArray<int32>& OutIndices)
{
	// Simplified collision: divide space into cells and create a convex hull per cell
	if (Points.Num() < 4)
	{
		return false;
	}

	// For now, use the same convex hull approach but with fewer points
	// A proper implementation would use V-HACD or similar
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
