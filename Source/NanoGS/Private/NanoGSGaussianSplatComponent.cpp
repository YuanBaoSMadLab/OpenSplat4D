// Copyright Epic Games, Inc. All Rights Reserved.
// Copyright OpenSplat4D 2026 All Rights Reserved.

#include "NanoGSGaussianSplatComponent.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatSceneProxy.h"
#include "GaussianSplatViewExtension.h"
#include "Engine/World.h"
#include "PhysicsEngine/BodySetup.h"
#include "HitProxies.h"
#include "Containers/BitArray.h"

UGaussianSplatComponent::UGaussianSplatComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Tick is only enabled at runtime for 4D playback or fracture following
	// (Update4DTickEnabled); static assets keep it disabled so there is no
	// per-frame CPU cost.
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	// Fracture following must read the collection *after* physics has stepped.
	PrimaryComponentTick.TickGroup = TG_PostPhysics;

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
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SortEveryNthFrame) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bCastShadow) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, ShadowIntensity))
	{
		MarkRenderStateDirty();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CollisionMethod) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CollisionMaxFaces) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, IgnoreFactor) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, VoxelSize) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, MaxCollisionElements))
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
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FractureGeometryCollection))
	{
		// Rest pose is relative to the previously-bound asset; reload it and
		// re-evaluate whether the fracture tick is needed at all.
		FractureRestMatrices.Reset();
		FractureLastPushedDeltas.Reset();
		bFractureWasActive = false;
		Update4DTickEnabled();
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

	// Fracture following runs independently of 4D playback
	TickFractureBoneDeltas();

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
	SetComponentTickEnabled((bPlaying && Supports4DPlayback()) || IsFractureBindingActive());
}

void UGaussianSplatComponent::PushCurrentTimeToProxy()
{
	if (FGaussianSplatSceneProxy* SplatProxy = static_cast<FGaussianSplatSceneProxy*>(SceneProxy))
	{
		SplatProxy->SetCurrentTime(CurrentTime);
	}
}

// ---------------------------------------------------------------------------
// 破裂跟随（Chaos GeometryCollection）
// ---------------------------------------------------------------------------

bool UGaussianSplatComponent::IsFractureBindingActive() const
{
	return FractureGeometryCollection != nullptr
		&& SplatAsset != nullptr
		&& SplatAsset->HasBoneBinding();
}

bool UGaussianSplatComponent::EnsureFractureRestMatrices()
{
	if (FractureRestMatrices.Num() > 0)
	{
		return true;
	}

	if (!SplatAsset || !SplatAsset->HasBoneBinding())
	{
		return false;
	}

	if (!SplatAsset->GetRestBoneMatrices(FractureRestMatrices))
	{
		FractureRestMatrices.Reset();
		return false;
	}

	return FractureRestMatrices.Num() > 0;
}

void UGaussianSplatComponent::TickFractureBoneDeltas()
{
	FGaussianSplatSceneProxy* SplatProxy = static_cast<FGaussianSplatSceneProxy*>(SceneProxy);

	// A rebuilt scene proxy starts with an empty delta buffer, so forget what we
	// had pushed and re-upload on the next tick instead of assuming it is current.
	if (SplatProxy != LastFractureProxy)
	{
		LastFractureProxy = SplatProxy;
		FractureLastPushedDeltas.Reset();
		bFractureWasActive = false;
	}

	if (!IsFractureBindingActive() || !SplatProxy || !EnsureFractureRestMatrices())
	{
		// Nothing to follow: release the bone path exactly once
		if (bFractureWasActive && SplatProxy)
		{
			SplatProxy->SetBoneDeltas(TArray<FMatrix44f>());
			bFractureWasActive = false;
		}
		return;
	}

	// Read the collection's bone transforms. NanoGS loads at PostConfigInit,
	// before GeometryCollectionEngine, so this must go through reflection.
	// GetCurrentTransforms() returns TArray<FTransform> (component space,
	// center-of-mass frame, i.e. already carrying MassToLocal -- which is also
	// what the binding stored, so the term cancels in the delta).
	UFunction* GetTransformsFn = FractureGeometryCollection->FindFunction(TEXT("GetCurrentTransforms"));
	if (!GetTransformsFn || GetTransformsFn->ParmsSize == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("GaussianSplat: fracture target '%s' has no GetCurrentTransforms(); fracture following disabled"),
			*FractureGeometryCollection->GetName());
		FractureGeometryCollection = nullptr;
		Update4DTickEnabled();
		return;
	}

	TArray<uint8> Parms;
	Parms.SetNumZeroed(GetTransformsFn->ParmsSize);
	FractureGeometryCollection->ProcessEvent(GetTransformsFn, Parms.GetData());

	TArray<FTransform> CurrentTransforms;
	for (TFieldIterator<FProperty> It(GetTransformsFn); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			if (const TArray<FTransform>* Result = It->ContainerPtrToValuePtr<TArray<FTransform>>(Parms.GetData()))
			{
				CurrentTransforms = *Result;  // deep copy; Parms dies below
			}
		}
	}

	const int32 BoneCount = FMath::Min(FractureRestMatrices.Num(), CurrentTransforms.Num());
	if (BoneCount <= 0)
	{
		return;
	}

	FractureBoneDeltas.SetNumUninitialized(FractureRestMatrices.Num());

	bool bAnyDisplaced = false;
	for (int32 BoneIndex = 0; BoneIndex < FractureRestMatrices.Num(); ++BoneIndex)
	{
		if (BoneIndex >= BoneCount)
		{
			// Collection lost this piece (e.g. removed on break): leave it at rest
			FractureBoneDeltas[BoneIndex] = FMatrix44f::Identity;
			continue;
		}

		const FMatrix CurM = CurrentTransforms[BoneIndex].ToMatrixWithScale();
		const FMatrix RestM = FMatrix(FractureRestMatrices[BoneIndex]);
		const FMatrix DeltaM = CurM * RestM.InverseFast();

		FractureBoneDeltas[BoneIndex] = FMatrix44f(DeltaM);

		if (!bAnyDisplaced && !DeltaM.Equals(FMatrix::Identity, 1e-4))
		{
			bAnyDisplaced = true;
		}
	}

	if (!bAnyDisplaced)
	{
		// At rest: disable the shader's bone path (zero per-frame render cost)
		if (bFractureWasActive)
		{
			SplatProxy->SetBoneDeltas(TArray<FMatrix44f>());
			bFractureWasActive = false;
		}
		FractureLastPushedDeltas.Reset();
		return;
	}

	// Only push when something actually moved. A settled pile keeps the deltas
	// already on the GPU -- so the debris stays displaced, but CalcViewData and
	// the depth sort stop being forced to re-run every frame.
	const bool bUnchanged = bFractureWasActive
		&& FractureLastPushedDeltas.Num() == FractureBoneDeltas.Num()
		&& FMemory::Memcmp(FractureLastPushedDeltas.GetData(), FractureBoneDeltas.GetData(),
			FractureBoneDeltas.Num() * sizeof(FMatrix44f)) == 0;
	if (bUnchanged)
	{
		return;
	}

	SplatProxy->SetBoneDeltas(FractureBoneDeltas);
	FractureLastPushedDeltas = FractureBoneDeltas;
	bFractureWasActive = true;
}

void UGaussianSplatComponent::SetNanitePrecision(float InThreshold)
{
	LODErrorThreshold = FMath::Clamp(InThreshold, 0.001f, 1.0f);
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::SetVisibilityRange(float InDistance)
{
	MaxDrawDistance = FMath::Max(InDistance, 0.0f);
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::SetFadeOutStart(float InDistance)
{
	FadeOutStartDistance = FMath::Max(InDistance, 0.0f);
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::SetSortInterval(int32 InFrames)
{
	SortEveryNthFrame = FMath::Clamp(InFrames, 1, 8);
	MarkRenderStateDirty();
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

	// The rest bone matrices belong to the previous asset
	FractureRestMatrices.Reset();
	FractureLastPushedDeltas.Reset();
	bFractureWasActive = false;
	Update4DTickEnabled();
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

		// The rest bone matrices may belong to a now-replaced binding
		FractureRestMatrices.Reset();
		FractureLastPushedDeltas.Reset();
		bFractureWasActive = false;
		Update4DTickEnabled();

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
	// Samples the cloud down to CollisionMaxFaces * 4 vertices and returns them as
	// OutVertices. This is NOT the hull itself: RebuildCollision() stores these
	// points in BodySetup->AggGeom.ConvexElems, and the engine's Chaos cooking
	// (FConvexBuilder::Build) computes the actual convex hull from them — the exact
	// same path a Static Mesh's auto-generated "Convex Hull" collision uses.
	// OutIndices is intentionally left empty; the convex element takes its geometry
	// from VertexData, not from an index buffer.
	if (Points.Num() < 4)
	{
		return false;
	}

	// Uniform sampling keeps the vertex count (and therefore the cooking cost)
	// bounded regardless of how many splats the asset holds.
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

	UE_LOG(LogTemp, Log, TEXT("GaussianSplat: ConvexHull collision sampled %d vertices (Chaos cooking builds the hull)"), OutVertices.Num());
	return true;
}

bool UGaussianSplatComponent::GenerateSimplifiedCollision(const TArray<FVector>& Points, TArray<FVector>& OutVertices, TArray<int32>& OutIndices)
{
	// Currently produces a SINGLE convex hull (same result as the ConvexHull
	// method): the points are sampled down and handed to Chaos cooking, which
	// builds one hull for the whole cloud.
	//
	// A true convex decomposition (V-HACD: split the volume into several convex
	// pieces so a concave shape such as a statue or a room interior is approximated
	// correctly) is NOT implemented. Until it is, prefer Voxel collision for
	// concave assets — the voxel grid follows the shape far better than one hull.
	if (Points.Num() < 4)
	{
		return false;
	}

	return GenerateConvexHull(Points, OutVertices, OutIndices);
}

bool UGaussianSplatComponent::GenerateVoxelCollision(const TArray<FVector>& Points, TArray<FVector>& OutVertices, TArray<int32>& OutIndices)
{
	// Voxel-based collision: one box element per occupied cell of a uniform grid.
	//
	// Two budgets are enforced by COARSENING the cell size and rebuilding the grid,
	// never by truncating the cell loop: the old code stopped as soon as it had
	// emitted MaxCells box elements while walking z/y/x in order, so the collision
	// shape only covered one corner of the asset.
	//   - MaxCollisionElements: how many box elements may end up in the BodySetup
	//   - MaxGridCells: hard cap on the raw grid size (memory guard)
	// Grid indices are int64 and occupancy uses a bit array (1 bit/cell instead of
	// 1 byte), so a fine VoxelSize on a large asset cannot overflow the index math
	// or blow up memory.
	if (Points.Num() == 0)
	{
		return false;
	}

	// 32M cells ≈ 4 MB of bit array; still allows a 320^3 grid while keeping the
	// guard comfortably inside the editor's memory budget.
	static constexpr int64 MaxGridCells = 32ll * 1024 * 1024;
	static constexpr int32 MaxGrowAttempts = 24;

	const int32 MaxElements = FMath::Clamp(MaxCollisionElements, 1, 65536);
	const FBox LocalBounds(Points);
	const FVector Extent = LocalBounds.GetExtent();
	const FVector Origin = LocalBounds.Min;

	float CellSize = FMath::Max(VoxelSize, 1e-4f);
	int64 GridX = 1;
	int64 GridY = 1;
	int64 GridZ = 1;
	TBitArray<> Occupied;
	int32 OccupiedCount = 0;

	for (int32 Attempt = 0; Attempt < MaxGrowAttempts; ++Attempt)
	{
		// Calculate grid dimensions
		GridX = FMath::Max<int64>(1, (int64)(Extent.X / CellSize) + 1);
		GridY = FMath::Max<int64>(1, (int64)(Extent.Y / CellSize) + 1);
		GridZ = FMath::Max<int64>(1, (int64)(Extent.Z / CellSize) + 1);

		const int64 TotalCells = GridX * GridY * GridZ;
		if (TotalCells > MaxGridCells)
		{
			CellSize *= 2.0f;
			continue;
		}

		// Mark occupied cells
		Occupied.Init(false, (int32)TotalCells);

		const int64 PlaneStride = GridX;
		const int64 SliceStride = GridX * GridY;
		const float InvCellSize = 1.0f / CellSize;

		OccupiedCount = 0;
		for (const FVector& P : Points)
		{
			const int64 gx = FMath::Clamp<int64>((int64)((P.X - Origin.X) * InvCellSize), 0, GridX - 1);
			const int64 gy = FMath::Clamp<int64>((int64)((P.Y - Origin.Y) * InvCellSize), 0, GridY - 1);
			const int64 gz = FMath::Clamp<int64>((int64)((P.Z - Origin.Z) * InvCellSize), 0, GridZ - 1);
			const int64 CellIndex = gx + gy * PlaneStride + gz * SliceStride;
			if (!(bool)Occupied[(int32)CellIndex])
			{
				Occupied[(int32)CellIndex] = true;
				++OccupiedCount;
			}
		}

		if (OccupiedCount <= MaxElements)
		{
			break;
		}

		// Too many occupied cells for the element budget: coarsen and retry so the
		// boxes keep covering the whole cloud instead of being truncated.
		CellSize *= 2.0f;
	}

	// Add box elements for occupied cells
	int32 CellCount = 0;
	for (int64 z = 0; z < GridZ && CellCount < MaxElements; ++z)
	{
		for (int64 y = 0; y < GridY && CellCount < MaxElements; ++y)
		{
			for (int64 x = 0; x < GridX && CellCount < MaxElements; ++x)
			{
				if (!(bool)Occupied[(int32)(x + y * GridX + z * GridX * GridY)])
				{
					continue;
				}
				FKBoxElem BoxElem;
				BoxElem.Center = FVector(
					Origin.X + ((float)x + 0.5f) * CellSize,
					Origin.Y + ((float)y + 0.5f) * CellSize,
					Origin.Z + ((float)z + 0.5f) * CellSize
				);
				BoxElem.X = CellSize;
				BoxElem.Y = CellSize;
				BoxElem.Z = CellSize;
				BodySetup->AggGeom.BoxElems.Add(BoxElem);
				++CellCount;
			}
		}
	}

	EffectiveVoxelSize = CellSize;

	UE_LOG(LogTemp, Log, TEXT("GaussianSplat: Voxel collision generated with %d boxes (%dx%dx%d grid, cell=%.4f, occupied=%d)"),
		CellCount, (int32)GridX, (int32)GridY, (int32)GridZ, CellSize, OccupiedCount);

	// Voxel method adds boxes directly, no need for vertices/indices
	OutVertices.Reset();
	OutIndices.Reset();
	return true;
}