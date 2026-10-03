// Copyright OpenSplat4D 2026 All Rights Reserved.
// Copyright Epic Games, Inc. All Rights Reserved.

#include "NanoGSGaussianSplatComponent.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatFractureAsset.h"
#include "GaussianSplatSceneProxy.h"
#include "GaussianSplatViewExtension.h"
#include "Engine/World.h"
#include "PhysicsEngine/BodySetup.h"
#include "HitProxies.h"
#include "Containers/BitArray.h"
#include "Misc/Crc.h"
#include "Misc/ScopeExit.h"

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

	// 已废弃：阴影代理类型（旧包围盒路径已移除）
	// ShadowProxyDetail = 1;
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
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LODErrorThreshold) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, MaxDrawDistance) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FadeOutStartDistance) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SortEveryNthFrame))
	{
		MarkRenderStateDirty();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SplatScale))
	{
		// SplatScale sizes the per-splat shadow boxes *and* the collision spheres,
		// so both caches have to be rebuilt — not just the proxy.
		RebuildShadowGeometry();
		bCollisionDirty = true;
		RebuildCollision();
		ApplyCollisionProfile();
		MarkRenderStateDirty();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bCastShadow) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, MaxShadowSplatSamples))
	{
		// Shadow geometry is cached on the component and copied into the proxy
		// at construction, so re-cache before rebuilding the proxy.
		RebuildShadowGeometry();
		MarkRenderStateDirty();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CollisionMethod) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, CollisionMaxFaces) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, MaxConvexHulls) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, IgnoreFactor) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, MaxSplatSpheres) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SplatSphereRadius))
	{
		bCollisionDirty = true;
		RebuildCollision();
		ApplyCollisionProfile();
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
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FractureGeometryCollection) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FractureAsset))
	{
		// Rest pose is relative to the previously-bound asset; reload it and
		// re-evaluate whether the fracture tick is needed at all.
		FractureRestMatrices.Reset();
		FractureLastPushedDeltas.Reset();
		bFractureWasActive = false;
		// A new fracture asset may carry a different source splat cloud; keep the
		// render asset in sync when the user has not picked one explicitly.
		if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FractureAsset) &&
			FractureAsset != nullptr && SplatAsset == nullptr)
		{
			if (UGaussianSplatAsset* SourceAsset = FractureAsset->GetSourceSplatAsset())
			{
				SetSplatAsset(SourceAsset);
			}
		}

		// Swapping the asset swaps the splat cloud the shadow geometry is built
		// from, so re-cache it.
		if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FractureAsset))
		{
			RebuildShadowGeometry();
		}

		MarkRenderStateDirty();
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

	if (FractureAsset)
	{
		SubscribeToFractureAssetChanges();
	}

	// 4D: start playback automatically if requested
	if (Supports4DPlayback() && bAutoPlay && !bPlaying)
	{
		Play4D();
	}
	Update4DTickEnabled();

	// Collision: geometry must exist *before* the collision profile is switched on.
	// RebuildCollision() is where BodySetup gets cooked; ApplyCollisionProfile() then
	// flips the profile to BlockAll, which is what actually makes the engine create
	// the physics state (UPrimitiveComponent::ShouldCreatePhysicsState() returns false
	// while the body is still on NoCollision). With CollisionMethod == None both calls
	// are cheap no-ops, so static assets pay nothing here.
	bCollisionDirty = true;
	RebuildCollision();
	ApplyCollisionProfile();

	// Shadow geometry is copied into the proxy at construction time, so the cache
	// must be ready before the render state is created.
	RebuildShadowGeometry();
}

void UGaussianSplatComponent::OnUnregister()
{
	UnsubscribeFromAssetChanges();
	UnsubscribeFromFractureAssetChanges();
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
	// v11: the bone binding lives in the fracture asset, not on the splat asset.
	return FractureGeometryCollection != nullptr
		&& FractureAsset != nullptr
		&& FractureAsset->HasBoneBinding();
}

bool UGaussianSplatComponent::EnsureFractureRestMatrices()
{
	if (FractureRestMatrices.Num() > 0)
	{
		return true;
	}

	if (!FractureAsset || !FractureAsset->HasBoneBinding())
	{
		return false;
	}

	if (!FractureAsset->GetRestBoneMatrices(FractureRestMatrices))
	{
		FractureRestMatrices.Reset();
		return false;
	}

	return FractureRestMatrices.Num() > 0;
}

void UGaussianSplatComponent::SetFractureAsset(UGaussianSplatFractureAsset* NewAsset)
{
	if (FractureAsset == NewAsset)
	{
		return;
	}

	UnsubscribeFromFractureAssetChanges();

	FractureAsset = NewAsset;

	// Render data (source cloud + bone binding + caps) lives on the fracture asset
	// and is dropped whenever its binding / caps change, so subscribe to be told
	// when the proxy has to be rebuilt around the new data.
	if (IsRegistered())
	{
		SubscribeToFractureAssetChanges();
	}

	// Fall back to the fracture asset's source cloud when the component has no
	// splat asset of its own, so a freshly created component renders right away.
	if (SplatAsset == nullptr && FractureAsset != nullptr)
	{
		SetSplatAsset(FractureAsset->GetSourceSplatAsset());
	}

	// The rest pose belongs to the previous binding
	FractureRestMatrices.Reset();
	FractureLastPushedDeltas.Reset();
	bFractureWasActive = false;

	// A new fracture asset may bring a new source splat asset with it, and the
	// shadow proxies are built from that cloud — re-cache them.
	RebuildShadowGeometry();

	Update4DTickEnabled();
	MarkRenderStateDirty();
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

#if WITH_EDITOR
	// The editor fracture preview owns the bone path while it runs: the viewport
	// client pushes its own deltas every frame, so the collection is left alone
	// (and the preview must not be released here).
	if (bPreviewBoneDeltasActive)
	{
		return;
	}
#endif

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

#if WITH_EDITOR
void UGaussianSplatComponent::SetPreviewBoneDeltas(const TArray<FMatrix44f>& InDeltas)
{
	bPreviewBoneDeltasActive = InDeltas.Num() > 0;

	// The proxy validates the length against the binding's bone count itself and
	// simply keeps the bone path off when they disagree, so an empty array (end
	// of preview) needs no special casing here.
	if (FGaussianSplatSceneProxy* SplatProxy = static_cast<FGaussianSplatSceneProxy*>(SceneProxy))
	{
		SplatProxy->SetBoneDeltas(InDeltas);
	}

	// The preview bypasses the collection path, so the "last pushed" bookkeeping
	// of that path must not be inherited by it.
	FractureLastPushedDeltas.Reset();
	bFractureWasActive = false;
}

void UGaussianSplatComponent::SetFractureTargetForPreview(UObject* InCollectionComponent)
{
	if (FractureGeometryCollection == InCollectionComponent)
	{
		return;
	}

	// Same bookkeeping as the FractureGeometryCollection property path in
	// PostEditChangeProperty: the rest pose belongs to the previous target.
	FractureGeometryCollection = InCollectionComponent;
	FractureRestMatrices.Reset();
	FractureLastPushedDeltas.Reset();
	bFractureWasActive = false;

	// TickFractureBoneDeltas runs from the component tick, which is only enabled
	// while a fracture is actually followed. Setting the property directly (as the
	// editor preview does) would leave it off.
	Update4DTickEnabled();
}
#endif

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

	// The engine reads UPrimitiveComponent::CastShadow while constructing the
	// proxy descriptor (FPrimitiveSceneProxyDesc::InitializeFromPrimitiveComponent),
	// and the shadow culling pass skips any primitive whose bCastDynamicShadow is
	// false. That bitfield defaults to false, so it must be mirrored here before
	// the proxy is created. SetCastShadow() is deliberately avoided: it calls
	// MarkRenderStateDirty(), which would rebuild the proxy every frame from
	// inside this very call. The qualified name bypasses the hiding accessor
	// UGaussianSplatComponent::CastShadow().
	UPrimitiveComponent::CastShadow = bCastShadow ? 1 : 0;

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

	// The proxy visualises the aggregated collision shapes in the engine's collision
	// view modes; hand it the BodySetup it should draw. Collision changes force a
	// render state rebuild (ApplyCollisionProfile), so the proxy never outlives it.
	return new FGaussianSplatSceneProxy(this, BodySetup);
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

	// The shadow fallback geometry is built from the splat cloud itself, so a new
	// asset needs a new cache before the proxy is rebuilt below.
	RebuildShadowGeometry();

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

void UGaussianSplatComponent::OnFractureAssetDataChanged(UGaussianSplatFractureAsset* ChangedAsset)
{
	if (ChangedAsset != FractureAsset)
	{
		return;
	}

	// The fracture asset dropped its cached render data (new bone binding / caps).
	// Every scene proxy copies that shared data at construction, so all of them must
	// be rebuilt: otherwise a proxy built before the solve keeps the pre-solve
	// binding (bHasBoneBinding == false) and SetBoneDeltas silently drops every
	// delta -- the editor's 破碎预览 would then do nothing at all.
	UE_LOG(LogTemp, Log, TEXT("GaussianSplat: fracture asset '%s' render data changed (binding/caps), recreating scene proxy"),
		*ChangedAsset->GetName());

	// The rest pose belongs to the replaced binding.
	FractureRestMatrices.Reset();
	FractureLastPushedDeltas.Reset();
	bFractureWasActive = false;
	Update4DTickEnabled();

	MarkRenderStateDirty();
}

void UGaussianSplatComponent::SubscribeToFractureAssetChanges()
{
	if (FractureAsset && !FractureAssetChangedDelegateHandle.IsValid())
	{
		FractureAssetChangedDelegateHandle = FractureAsset->OnFractureChanged.AddUObject(
			this, &UGaussianSplatComponent::OnFractureAssetDataChanged);
		UE_LOG(LogTemp, Verbose, TEXT("GaussianSplat: Subscribed to fracture asset change notifications"));
	}
}

void UGaussianSplatComponent::UnsubscribeFromFractureAssetChanges()
{
	if (FractureAsset && FractureAssetChangedDelegateHandle.IsValid())
	{
		FractureAsset->OnFractureChanged.Remove(FractureAssetChangedDelegateHandle);
		FractureAssetChangedDelegateHandle.Reset();
		UE_LOG(LogTemp, Verbose, TEXT("GaussianSplat: Unsubscribed from fracture asset change notifications"));
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

bool UGaussianSplatComponent::HasEffectiveCollision() const
{
	// Every method now builds its geometry from the splat cloud itself, so the
	// only way to have no collision is to ask for none.
	return CollisionMethod != EGaussianCollisionMethod::None;
}

bool UGaussianSplatComponent::LineTraceComponent(FHitResult& OutHit, const FVector TraceStart, const FVector TraceEnd, const FCollisionQueryParams& TraceParams)
{
	if (!HasEffectiveCollision() || !BodySetup)
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
	// Every method now builds its shapes from the splat cloud itself (Voxel and
	// ProxyMesh used to quantise into voxels, which is what made the collision
	// look blocky and offset — they share the per-splat path with SplatSpheres).
	if (!SplatAsset || !SplatAsset->IsValid())
	{
		return;
	}

	if (CollisionMethod == EGaussianCollisionMethod::None)
	{
		// Profile is handled by ApplyCollisionProfile() so that this function can be
		// called from GetBodySetup() without re-entering physics state creation.
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
	//
	// The per-splat methods are exempt: they place one element per splat and read
	// that splat's own size from the bulk data by index, so thinning the point
	// array here would both drop splats the method exists to represent and break
	// the Points[i] -> splat index correspondence. Their own element cap
	// (MaxSplatSpheres) controls the count instead.
	if (CollisionMethod != EGaussianCollisionMethod::Voxel
		&& CollisionMethod != EGaussianCollisionMethod::ProxyMesh
		&& CollisionMethod != EGaussianCollisionMethod::SplatSpheres
		&& IgnoreFactor > 0.0f && IgnoreFactor < 1.0f)
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
	case EGaussianCollisionMethod::ProxyMesh:
	case EGaussianCollisionMethod::SplatSpheres:
		bSuccess = GenerateSplatSpheres(Points, Vertices, Indices);
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

	bCollisionDirty = false;

	UE_LOG(LogTemp, Log, TEXT("GaussianSplat: Collision rebuilt (method=%d, points=%d, verts=%d, indices=%d)"),
		(uint8)CollisionMethod, Points.Num(), Vertices.Num(), Indices.Num());

	// 诊断：凸包元素只有拿到 Chaos 隐式网格才会真正参与模拟与查询，而它由
	// CreatePhysicsMeshes() 烹饪出来。烹饪没成功时这里会显示 cooked < convex ——
	// 有这一行就能把「形状没生成」和「形状生成了但没进物理状态」分开。
	if (BodySetup->AggGeom.ConvexElems.Num() > 0)
	{
		int32 CookedConvex = 0;
		for (const FKConvexElem& ConvexElem : BodySetup->AggGeom.ConvexElems)
		{
			CookedConvex += ConvexElem.GetChaosConvexMesh().IsValid() ? 1 : 0;
		}
		UE_LOG(LogTemp, Log, TEXT("GaussianSplat: collision shapes -> convex=%d (cooked=%d), spheres=%d, boxes=%d"),
			BodySetup->AggGeom.ConvexElems.Num(), CookedConvex,
			BodySetup->AggGeom.SphereElems.Num(), BodySetup->AggGeom.BoxElems.Num());
		if (CookedConvex < BodySetup->AggGeom.ConvexElems.Num())
		{
			UE_LOG(LogTemp, Warning, TEXT("GaussianSplat: %d of %d convex elements have no cooked Chaos mesh -- they carry no collision until Chaos cooking succeeds (method=%d)"),
				BodySetup->AggGeom.ConvexElems.Num() - CookedConvex, BodySetup->AggGeom.ConvexElems.Num(), (uint8)CollisionMethod);
		}
	}
}

void UGaussianSplatComponent::ApplyCollisionProfile()
{
	const FName DesiredProfile = HasEffectiveCollision()
		? UCollisionProfile::BlockAll_ProfileName
		: UCollisionProfile::NoCollision_ProfileName;

	const bool bProfileChanged = BodyInstance.GetCollisionProfileName() != DesiredProfile;
	if (bProfileChanged)
	{
		SetCollisionProfileName(DesiredProfile);
	}

	// The profile switch only creates a body when collision was off before. That is not
	// enough here: RebuildCollision() rewrites BodySetup->AggGeom, and an already-live
	// Chaos body does not notice — it keeps the shapes it was built with. So switching
	// between two collision-enabled methods (e.g. 包围盒 -> 凸分解) would leave the old
	// shape in the simulation while the details panel shows the new one. Recreate the
	// state whenever there is geometry to publish, or when a state may need tearing
	// down. With collision off before and after, nothing is rebuilt and static assets
	// still pay nothing.
	if (HasEffectiveCollision() || bProfileChanged)
	{
		RecreatePhysicsState();
	}

	// The scene proxy holds the BodySetup it was built with (it draws those shapes in
	// the engine's collision view modes). RebuildCollision() may have replaced or
	// cleared that object, so the proxy has to be recreated with the new pointer
	// before it could ever be used again.
	MarkRenderStateDirty();
}

void UGaussianSplatComponent::BuildCollisionBodySetup()
{
	RebuildCollision();
}

void UGaussianSplatComponent::RebuildShadowGeometry()
{
	ShadowShellVertices.Reset();
	ShadowShellTriangles.Reset();

	// Off by default: without the flag the proxy never enters the shadow pass,
	// so the cache stays empty and the render path is untouched.
	if (!bCastShadow)
	{
		return;
	}

	// Per-splat oriented boxes: each proxy box follows one splat's own position,
	// orientation and size, so the hull no longer comes out blocky or offset the
	// way the old occupancy-voxel shell did. The fracture asset's voxel shell used
	// to feed this, but that geometry is quantised for physics — far too coarse to
	// project a readable shadow.
	BuildShadowSplatGeometry();
}

void UGaussianSplatComponent::BuildShadowSplatGeometry()
{
	if (!SplatAsset || !SplatAsset->IsValid())
	{
		return;
	}

	// The shadow cache is a triangle soup uploaded to the GPU once per rebuild, so
	// the sample count has a hard cap (MaxShadowSplatSamples) no matter how large
	// the asset is.
	const int64 SplatCount = SplatAsset->GetSplatCount();
	constexpr int32 PositionBytesPerSplat = 12;
	if (SplatCount <= 0 || SplatAsset->PositionBulkData.GetBulkDataSize() < SplatCount * PositionBytesPerSplat)
	{
		return;
	}

	const int32 SampleCap = FMath::Clamp(MaxShadowSplatSamples, 256, 1048576);
	const int64 Stride = FMath::Max<int64>(1, SplatCount / SampleCap);
	const int64 SampleCount = (SplatCount + Stride - 1) / Stride;
	// One proxy stands in for Stride splats, so scale it by cbrt(Stride) to keep the
	// total projected volume roughly equal to the cloud it replaces.
	const float StrideScale = FMath::Pow((float)Stride, 1.0f / 3.0f);

	// Positions are read straight out of the bulk block with a stride rather than
	// through GetDecompressedPositions(): that helper allocates a TArray<FVector>
	// entry for every splat (24 B each, ~1.4 GB at 57M splats), which a shadow
	// cache must not do.
	const uint8* PositionPtr = static_cast<const uint8*>(SplatAsset->PositionBulkData.LockReadOnly());
	if (!PositionPtr)
	{
		UE_LOG(LogTemp, Warning, TEXT("GaussianSplat: position bulk data could not be locked for shadow geometry"));
		return;
	}
	ON_SCOPE_EXIT
	{
		SplatAsset->PositionBulkData.Unlock();
	};

	// Per-splat rotation (normalised quaternion) + linear scale, same 28-byte
	// record GenerateSplatSpheres reads.
	constexpr int32 OtherBytesPerSplat = 28;
	const uint8* OtherPtr = nullptr;
	if (SplatAsset->OtherBulkData.GetBulkDataSize() >= SplatCount * OtherBytesPerSplat)
	{
		OtherPtr = static_cast<const uint8*>(SplatAsset->OtherBulkData.LockReadOnly());
	}
	ON_SCOPE_EXIT
	{
		if (OtherPtr)
		{
			SplatAsset->OtherBulkData.Unlock();
		}
	};
	if (!OtherPtr)
	{
		UE_LOG(LogTemp, Warning, TEXT("GaussianSplat: rotation/scale data missing; shadow splats fall back to 2 cm axis-aligned boxes"));
	}

	// One oriented box per sampled splat: 8 corners, 12 triangles. Corners are not
	// shared between splats, which keeps this a straight loop with no vertex dedup
	// table — and the internal faces are harmless because the batch is submitted
	// with backface culling off (see AddShadowMeshBatch).
	ShadowShellVertices.Reserve((int32)SampleCount * 8);
	ShadowShellTriangles.Reserve((int32)SampleCount * 36);

	// Winding is irrelevant for the same reason. Corner index bit0 = +X, bit1 = +Y,
	// bit2 = +Z.
	static const int32 BoxFaces[12][3] =
	{
		{ 0, 1, 2 }, { 0, 2, 3 },   // -Z
		{ 4, 6, 5 }, { 4, 7, 6 },   // +Z
		{ 0, 4, 5 }, { 0, 5, 1 },   // -Y
		{ 3, 2, 6 }, { 3, 6, 7 },   // +Y
		{ 0, 3, 7 }, { 0, 7, 4 },   // -X
		{ 1, 5, 6 }, { 1, 6, 2 }    // +X
	};

	const float GlobalScale = FMath::Max(SplatScale, 0.0f);
	int64 BoxCount = 0;
	for (int64 SplatIndex = 0; SplatIndex < SplatCount; SplatIndex += Stride)
	{
		const float* Position = reinterpret_cast<const float*>(PositionPtr + SplatIndex * PositionBytesPerSplat);
		const FVector3f Center(Position[0], Position[1], Position[2]);

		FQuat4f Rotation = FQuat4f::Identity;
		// Default: a 2 cm axis-aligned box, used only when the asset carries no
		// per-splat rotation/scale.
		FVector3f HalfExtent(1.0f, 1.0f, 1.0f);
		if (OtherPtr)
		{
			const float* Other = reinterpret_cast<const float*>(OtherPtr + SplatIndex * OtherBytesPerSplat);
			Rotation = FQuat4f(Other[0], Other[1], Other[2], Other[3]).GetNormalized();
			// The shader draws a splat out to sqrt(2) * Scale
			// (GaussianDataTypes.ush), so the stand-in box uses that same radius
			// along each of the splat's own axes.
			const float SizeScale = 1.41421356f * GlobalScale * StrideScale;
			HalfExtent = FVector3f(
				FMath::Max(FMath::Abs(Other[4]) * SizeScale, 0.01f),
				FMath::Max(FMath::Abs(Other[5]) * SizeScale, 0.01f),
				FMath::Max(FMath::Abs(Other[6]) * SizeScale, 0.01f));
		}

		const int32 Base = ShadowShellVertices.Num();
		for (int32 CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
		{
			const FVector3f Local(
				(CornerIndex & 1) ? HalfExtent.X : -HalfExtent.X,
				(CornerIndex & 2) ? HalfExtent.Y : -HalfExtent.Y,
				(CornerIndex & 4) ? HalfExtent.Z : -HalfExtent.Z);
			ShadowShellVertices.Add(Center + Rotation.RotateVector(Local));
		}

		for (int32 FaceIndex = 0; FaceIndex < 12; ++FaceIndex)
		{
			ShadowShellTriangles.Add(Base + BoxFaces[FaceIndex][0]);
			ShadowShellTriangles.Add(Base + BoxFaces[FaceIndex][1]);
			ShadowShellTriangles.Add(Base + BoxFaces[FaceIndex][2]);
		}
		++BoxCount;
	}

	UE_LOG(LogTemp, Log, TEXT("GaussianSplat: shadow geometry from splat boxes (boxes=%lld of %lld splats, stride=%lld, verts=%d, triangles=%d)"),
		BoxCount, SplatCount, Stride, ShadowShellVertices.Num(), ShadowShellTriangles.Num() / 3);
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
	// Multi-hull convex decomposition.
	//
	// One hull cannot represent a concave asset: it fills every dent, hole and gap,
	// so a seated statue comes out as a solid block between the arms and the body.
	// This splits the cloud into pieces and gives each piece its own convex element,
	// which is the same shape family a Static Mesh's "Convex Decomposition" collision
	// produces.
	//
	// Method: every round, pick the piece with the largest bounding-box volume and
	// cut it in two along its longest axis, splitting at the mean of that piece's
	// points on that axis. A cut is a plane, so all the points of a piece end up
	// strictly on one side: the two hulls cannot overlap, and each one still hugs
	// the surface instead of bridging across a concavity.
	//
	// Splitting stops when MaxConvexHulls pieces exist, when no piece has enough
	// points left to be worth halving, or when a cut cannot separate the points.
	//
	// The elements are appended to BodySetup->AggGeom directly (the same way
	// GenerateSplatSpheres appends spheres), so OutVertices stays empty and
	// RebuildCollision() does not add one more hull on top of these.
	//
	// Chaos is only used for cooking. Nothing here links against V-HACD
	// (GeometryCollectionConvexUtility lives in a module this PostConfigInit-phase
	// module cannot hard-depend on) — hence the subdivision instead of a
	// concavity-driven split.
	const int32 HullBudget = FMath::Clamp(MaxConvexHulls, 1, 64);
	const int32 MaxVertsPerHull = FMath::Clamp(CollisionMaxFaces, 4, 255) * 4;
	// Below this, halving a piece stops being useful: the hull of a handful of
	// points is a sliver, not a volume.
	const int32 MinPiecePoints = 16;

	if (Points.Num() < 4 || !BodySetup)
	{
		return false;
	}

	// Pieces are contiguous ranges of one working buffer, so nothing is copied per
	// split and the whole decomposition costs O(N log K).
	TArray<FVector> Work = Points;

	struct FPiece
	{
		int32 Begin = 0;
		int32 End = 0;
		FBox Bounds = FBox(ForceInit);
		/** Set when this piece cannot be halved any further, so it is not re-picked */
		bool bSealed = false;
	};

	TArray<FPiece> Pieces;
	{
		FPiece& Root = Pieces.AddDefaulted_GetRef();
		Root.Begin = 0;
		Root.End = Work.Num();
		for (const FVector& P : Work)
		{
			Root.Bounds += P;
		}
	}

	while (Pieces.Num() < HullBudget)
	{
		// The largest bounding box is the piece that still encloses the most empty
		// space, so halving it removes the most error.
		int32 Pick = INDEX_NONE;
		double BestVolume = -1.0;
		for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
		{
			const FPiece& Candidate = Pieces[PieceIndex];
			if (Candidate.bSealed || Candidate.End - Candidate.Begin < MinPiecePoints * 2)
			{
				continue;
			}
			const double Volume = Candidate.Bounds.GetVolume();
			if (Volume > BestVolume)
			{
				BestVolume = Volume;
				Pick = PieceIndex;
			}
		}
		if (Pick == INDEX_NONE)
		{
			break;
		}

		FPiece& Piece = Pieces[Pick];
		const FVector Extent = Piece.Bounds.GetExtent();
		const int32 Axis = (Extent.X >= Extent.Y && Extent.X >= Extent.Z)
			? 0
			: (Extent.Y >= Extent.Z ? 1 : 2);

		// Mean rather than median: an O(size) pass with no sort, and it still yields a
		// separating plane. Robust against the dense clusters a splat cloud has, where
		// a median can land on a value shared by thousands of points.
		double Sum = 0.0;
		for (int32 i = Piece.Begin; i < Piece.End; ++i)
		{
			Sum += Work[i][Axis];
		}
		const double Pivot = Sum / (double)(Piece.End - Piece.Begin);

		// In-place partition: [Begin, Left) keeps the low side, [Left, End) the high side.
		int32 Left = Piece.Begin;
		int32 Right = Piece.End;
		while (Left < Right)
		{
			if (Work[Left][Axis] < Pivot)
			{
				++Left;
			}
			else
			{
				--Right;
				Swap(Work[Left], Work[Right]);
			}
		}

		if (Left == Piece.Begin || Left == Piece.End)
		{
			// Every point sits on one side (all identical on this axis): the plane
			// does not separate anything, so stop touching this piece.
			Piece.bSealed = true;
			continue;
		}

		const int32 SplitAt = Left;
		FPiece High;
		High.Begin = SplitAt;
		High.End = Piece.End;
		for (int32 i = High.Begin; i < High.End; ++i)
		{
			High.Bounds += Work[i];
		}

		FPiece Low;
		Low.Begin = Piece.Begin;
		Low.End = SplitAt;
		for (int32 i = Low.Begin; i < Low.End; ++i)
		{
			Low.Bounds += Work[i];
		}

		Pieces[Pick] = Low;
		Pieces.Add(High);
	}

	// One convex element per piece. Each piece's vertex list is sampled down with the
	// same uniform stride a single hull uses, so cooking cost scales with the hull
	// count rather than with the splat count.
	int32 HullCount = 0;
	for (const FPiece& Piece : Pieces)
	{
		const int32 PieceCount = Piece.End - Piece.Begin;
		if (PieceCount < 4)
		{
			continue;
		}

		FKConvexElem& ConvexElem = BodySetup->AggGeom.ConvexElems.AddDefaulted_GetRef();
		if (PieceCount <= MaxVertsPerHull)
		{
			ConvexElem.VertexData.Reserve(PieceCount);
			for (int32 i = Piece.Begin; i < Piece.End; ++i)
			{
				ConvexElem.VertexData.Add(Work[i]);
			}
		}
		else
		{
			ConvexElem.VertexData.Reserve(MaxVertsPerHull);
			const float Step = (float)PieceCount / (float)MaxVertsPerHull;
			for (int32 i = 0; i < MaxVertsPerHull; ++i)
			{
				ConvexElem.VertexData.Add(Work[Piece.Begin + (int32)(i * Step)]);
			}
		}
		ConvexElem.UpdateElemBox();
		++HullCount;
	}

	// Appended above; RebuildCollision() must not add a single hull on top.
	OutVertices.Reset();
	OutIndices.Reset();

	UE_LOG(LogTemp, Log, TEXT("GaussianSplat: ConvexDecomposition produced %d convex hulls from %d points (budget=%d, maxVertsPerHull=%d)"),
		HullCount, Points.Num(), HullBudget, MaxVertsPerHull);

	return HullCount > 0;
}

bool UGaussianSplatComponent::GenerateSplatSpheres(const TArray<FVector>& Points, TArray<FVector>& OutVertices, TArray<int32>& OutIndices)
{
	// 逐 splat 球体（「原生碰撞」）。
	//
	// 球心就是 splat 自己的位置，半径取该 splat 自身的旋转/尺度推得的「与其可见
	// 椭球等体积」的球 —— 位置与大小都逐一对应，没有体素量化的方块化与偏位。
	// 可见椭球半轴 ≈ √2 × scale（着色器 r = sqrt(2λ)，见 GaussianDataTypes.ush），
	// 椭球体积 4/3 π (√2 s_x)(√2 s_y)(√2 s_z) 折算成等体积球即半径
	// √2 × cbrt(s_x s_y s_z)，再乘组件视觉倍率 SplatScale。
	//
	// 球体是解析形状，不依赖 Chaos cooking 出来的隐式网格，因此编辑器和运行时
	// 表现一致。Voxel / ProxyMesh / SplatSpheres 三个方法现在都走这条路径。
	//
	// 消耗护栏：球体数量上限 MaxSplatSpheres。超过上限就按等步长抽稀到上限，
	// 并把半径放大到 cbrt(步长) 倍，让留下的球体继续填满同一块体积，而不是
	// 塌成一张稀疏的网。
	//
	// 元素直接追加到 BodySetup->AggGeom，OutVertices 保持为空，
	// RebuildCollision() 不会再多加一个整体凸包。
	const int32 Cap = FMath::Clamp(MaxSplatSpheres, 16, 65536);
	if (Points.Num() == 0 || !BodySetup)
	{
		return false;
	}

	// 等步长抽稀：步长和最终数量互相确定，循环次数正好等于 SphereCount。
	const int32 Stride = FMath::Max(1, FMath::DivideAndRoundUp(Points.Num(), Cap));
	const int32 SphereCount = FMath::DivideAndRoundUp(Points.Num(), Stride);

	// 抽稀后留下来的每个球代表 Stride 个 splat，半径放大 cbrt(Stride) 倍才能保住
	// 覆盖的体积（体积 ∝ 半径³）。
	const float StrideScale = FMath::Pow((float)Stride, 1.0f / 3.0f);

	// 逐 splat 尺度：OtherBulkData 是 28 B/splat（float 0..3 = 归一化四元数，
	// 4..6 = 线性 scale，cm），与 Positions 的 splat 索引一一对应。只有在点云
	// 没被抽稀过（Points.Num() == SplatCount）时索引才对得上；否则退回统一半径。
	const int32 SplatCount = SplatAsset ? (int32)SplatAsset->GetSplatCount() : 0;
	const bool bPerSplatSize = SplatAsset != nullptr
		&& Points.Num() == SplatCount
		&& SplatCount > 0
		&& SplatAsset->OtherBulkData.GetBulkDataSize() >= (int64)SplatCount * 28;

	// 统一半径（SplatSphereRadius > 0 用户指定，或逐 splat 尺度不可用时回落）：
	// 按「球体刚好互相贴住」算 —— 每个球体分到的体积是点云体积 / SphereCount。
	float UniformRadius = SplatSphereRadius;
	if (!bPerSplatSize && UniformRadius <= 0.0f)
	{
		FBox LocalBounds(ForceInit);
		for (int32 i = 0; i < Points.Num(); i += Stride)
		{
			LocalBounds += Points[i];
		}
		const double Volume = (double)LocalBounds.GetVolume();
		UniformRadius = Volume > 0.0
			? (float)(0.5 * FMath::Pow(Volume / (double)SphereCount, 1.0 / 3.0))
			: 1.0f;
	}
	const float UseUniformRadius = SplatSphereRadius > 0.0f ? SplatSphereRadius : UniformRadius;

	const uint8* OtherPtr = nullptr;
	if (bPerSplatSize)
	{
		OtherPtr = static_cast<const uint8*>(SplatAsset->OtherBulkData.LockReadOnly());
	}
	ON_SCOPE_EXIT
	{
		if (OtherPtr)
		{
			SplatAsset->OtherBulkData.Unlock();
		}
	};

	BodySetup->AggGeom.SphereElems.Reserve(SphereCount);
	for (int32 i = 0; i < Points.Num(); i += Stride)
	{
		float Radius = UseUniformRadius;
		if (OtherPtr)
		{
			const float* Other = reinterpret_cast<const float*>(OtherPtr + (int64)i * 28);
			const float Sx = FMath::Abs(Other[4]);
			const float Sy = FMath::Abs(Other[5]);
			const float Sz = FMath::Abs(Other[6]);
			const float GeomMean = FMath::Pow(Sx * Sy * Sz, 1.0f / 3.0f);
			// √2 与 (2√2)^(1/3) 等价（等体积球折算）。
			Radius = 1.41421356f * FMath::Max(SplatScale, 0.0f) * GeomMean * StrideScale;
		}

		FKSphereElem& SphereElem = BodySetup->AggGeom.SphereElems.AddDefaulted_GetRef();
		SphereElem.Center = Points[i];
		SphereElem.Radius = FMath::Max(Radius, 0.01f);
	}

	OutVertices.Reset();
	OutIndices.Reset();

	UE_LOG(LogTemp, Log, TEXT("GaussianSplat: SplatSpheres produced %d spheres (points=%d, cap=%d, stride=%d, perSplatSize=%d)"),
		BodySetup->AggGeom.SphereElems.Num(), Points.Num(), Cap, Stride, bPerSplatSize ? 1 : 0);

	return BodySetup->AggGeom.SphereElems.Num() > 0;
}