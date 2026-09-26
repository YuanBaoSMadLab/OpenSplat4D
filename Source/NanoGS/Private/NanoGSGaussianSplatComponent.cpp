// Copyright Epic Games, Inc. All Rights Reserved.
// Copyright OpenSplat4D 2026 All Rights Reserved.

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
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SplatScale) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, LODErrorThreshold) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, MaxDrawDistance) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FadeOutStartDistance) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, SortEveryNthFrame))
	{
		MarkRenderStateDirty();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, bCastShadow))
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
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, VoxelSize) ||
			 PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, MaxCollisionElements) ||
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

		// ProxyMesh collision takes its geometry from the fracture asset, so
		// swapping the asset also swaps the collision shell.
		if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatComponent, FractureAsset) &&
			CollisionMethod == EGaussianCollisionMethod::ProxyMesh)
		{
			bCollisionDirty = true;
			RebuildCollision();
			ApplyCollisionProfile();
		}

		// Shadows also come from the fracture asset's voxel shell.
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

	// Shadow geometry comes from the new asset's voxel shell
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
	if (CollisionMethod == EGaussianCollisionMethod::None)
	{
		return false;
	}

	// ProxyMesh borrows its geometry from the fracture asset: without a shell —
	// or with the shell marked "仅用于破裂" — there is nothing to collide with, and
	// the body setup must stay empty so no physics state is ever created for it.
	if (CollisionMethod == EGaussianCollisionMethod::ProxyMesh)
	{
		return FractureAsset != nullptr
			&& FractureAsset->bUseVoxelShellAsCollision
			&& FractureAsset->HasVoxelShell();
	}

	return true;
}

//~ Begin IInterface_CollisionDataProvider Interface

bool UGaussianSplatComponent::ContainsPhysicsTriMeshData(bool bInUseAllTriData) const
{
	return HasEffectiveCollision() && CollisionMethod == EGaussianCollisionMethod::ProxyMesh;
}

bool UGaussianSplatComponent::GetTriMeshSizeEstimates(struct FTriMeshCollisionDataEstimates& OutTriMeshEstimates, bool bInUseAllTriData) const
{
	OutTriMeshEstimates.VerticeCount = 0;

	if (!ContainsPhysicsTriMeshData(bInUseAllTriData))
	{
		return false;
	}

	// Cheap: the cached soup is filled by RebuildCollision(); fall back to the
	// asset's own counter if the cache has not been built yet.
	OutTriMeshEstimates.VerticeCount = VoxelShellVertices.Num() > 0
		? VoxelShellVertices.Num()
		: FractureAsset->VoxelShellVertexCount;
	return true;
}

bool UGaussianSplatComponent::GetPhysicsTriMeshData(struct FTriMeshCollisionData* CollisionData, bool bInUseAllTriData)
{
	if (CollisionData == nullptr || !ContainsPhysicsTriMeshData(bInUseAllTriData))
	{
		return false;
	}

	if (VoxelShellVertices.Num() < 3 || VoxelShellIndices.Num() < 3)
	{
		// Cooked before the cache existed (or an empty shell): nothing to hand over.
		return false;
	}

	CollisionData->Vertices = VoxelShellVertices;
	CollisionData->Indices = VoxelShellIndices;
	// MaterialIndices / UVs stay empty: Chaos only reads them when they are valid
	// per-triangle / per-vertex, and this shell has neither.
	CollisionData->MaterialIndices.Reset();
	CollisionData->UVs.Reset();
	CollisionData->bFlipNormals = false;
	CollisionData->bDeformableMesh = false;
	CollisionData->bFastCook = false;
	CollisionData->bDisableActiveEdgePrecompute = false;
	CollisionData->bReducedMemoryRepresentation = false;
	return true;
}

void UGaussianSplatComponent::GetMeshId(FString& OutMeshId)
{
	OutMeshId.Empty();

	if (CollisionMethod != EGaussianCollisionMethod::ProxyMesh || !HasEffectiveCollision())
	{
		return;
	}

	if (VoxelShellVertices.Num() == 0)
	{
		// Cache not built yet — identify by the owning asset so at least two
		// components on the same fracture share one cook.
		OutMeshId = FractureAsset->GetPathName();
		return;
	}

	// The shell is data, not a shared asset: hash its content so a re-solved
	// fracture (new shell on the same component) cannot be mistaken for a
	// cooked-cache hit, while identical shells still share one entry.
	uint32 Hash = FCrc::MemCrc32(VoxelShellVertices.GetData(), VoxelShellVertices.Num() * (int32)sizeof(FVector3f));
	for (const FTriIndices& Tri : VoxelShellIndices)
	{
		Hash = FCrc::MemCrc32(&Tri.v0, sizeof(int32), Hash);
		Hash = FCrc::MemCrc32(&Tri.v1, sizeof(int32), Hash);
		Hash = FCrc::MemCrc32(&Tri.v2, sizeof(int32), Hash);
	}
	OutMeshId = FString::Printf(TEXT("GaussianSplatVoxelShell_%08X_%d_%d"),
		Hash, VoxelShellVertices.Num(), VoxelShellIndices.Num());
}

//~ End IInterface_CollisionDataProvider Interface

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
	// ProxyMesh is the odd one out: its geometry is the fracture asset's collision
	// voxel shell, cooked as a real triangle mesh. It never touches the splat cloud,
	// so it is resolved before the source-asset guard below — the shell alone has to
	// be enough, and nothing here may decompress a single splat.
	if (CollisionMethod == EGaussianCollisionMethod::ProxyMesh)
	{
		VoxelShellVertices.Reset();
		VoxelShellIndices.Reset();

		// No fracture asset, no shell, or "仅用于破裂": this component has no
		// collision at all and ApplyCollisionProfile() keeps it on NoCollision.
		if (!HasEffectiveCollision())
		{
			BodySetup = nullptr;
			bCollisionDirty = false;
			return;
		}

		TArray<int32> ShellTriangles;
		if (!FractureAsset->GetVoxelShell(VoxelShellVertices, ShellTriangles) ||
			VoxelShellVertices.Num() < 3 || ShellTriangles.Num() < 3)
		{
			VoxelShellVertices.Reset();
			BodySetup = nullptr;
			bCollisionDirty = false;
			UE_LOG(LogTemp, Warning, TEXT("GaussianSplat: fracture '%s' voxel shell could not be read for ProxyMesh collision"), *FractureAsset->GetName());
			return;
		}

		VoxelShellIndices.Reserve(ShellTriangles.Num() / 3);
		for (int32 i = 0; i + 2 < ShellTriangles.Num(); i += 3)
		{
			FTriIndices& Tri = VoxelShellIndices.AddDefaulted_GetRef();
			Tri.v0 = ShellTriangles[i];
			Tri.v1 = ShellTriangles[i + 1];
			Tri.v2 = ShellTriangles[i + 2];
		}

		if (!BodySetup)
		{
			BodySetup = NewObject<UBodySetup>(this, NAME_None, RF_Transient);
			BodySetup->BodySetupGuid = FGuid::NewGuid();
		}
		else
		{
			BodySetup->AggGeom.EmptyElements();
		}

		// No simple shapes at all — the triangle mesh *is* the collision, which is
		// why the trace flag has to say so: Chaos only keeps a triangle mesh in the
		// simulation set when the shape reports UseComplexAsSimple, and conversely
		// drops every non-mesh shape while that flag is set. Set before cooking.
		BodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;
		BodySetup->bGenerateMirroredCollision = false;
		BodySetup->bDoubleSidedGeometry = true;
		BodySetup->InvalidatePhysicsData();
		BodySetup->CreatePhysicsMeshes();

		bCollisionDirty = false;

		UE_LOG(LogTemp, Log, TEXT("GaussianSplat: ProxyMesh collision rebuilt from fracture '%s' (verts=%d, triangles=%d)"),
			*FractureAsset->GetName(), VoxelShellVertices.Num(), VoxelShellIndices.Num());
		return;
	}

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

	// The ProxyMesh method above switches this to UseComplexAsSimple; restore the
	// default so a switch back to a simple-shape method keeps a simulated body
	// (Chaos drops every non-mesh shape while that flag is set).
	BodySetup->CollisionTraceFlag = CTF_UseDefault;

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
	// SplatSpheres is exempt: it is meant to place one sphere per splat, and its own
	// MaxSplatSpheres cap already controls the element count. Thinning it here would
	// silently drop splats that the method exists to represent.
	if (CollisionMethod != EGaussianCollisionMethod::SplatSpheres && IgnoreFactor > 0.0f && IgnoreFactor < 1.0f)
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

	// Preferred source: the fracture asset's voxel shell — the tightest closed hull
	// available, and free to read. Read regardless of bUseVoxelShellAsCollision:
	// that flag gates *collision*, and the shell is still the correct hull to cast
	// shadows from.
	if (FractureAsset != nullptr &&
		FractureAsset->GetVoxelShell(ShadowShellVertices, ShadowShellTriangles) &&
		ShadowShellVertices.Num() >= 3 && ShadowShellTriangles.Num() >= 3)
	{
		UE_LOG(LogTemp, Log, TEXT("GaussianSplat: shadow geometry from fracture '%s' voxel shell (verts=%d, triangles=%d)"),
			*FractureAsset->GetName(), ShadowShellVertices.Num(), ShadowShellTriangles.Num() / 3);
		return;
	}

	// Fallback: a bare splat has no fracture asset at all, so build the geometry
	// from the point cloud. Without this, ticking 投射阴影 would silently do
	// nothing until the user had gone through the entire fracture workflow, which
	// is exactly how shadows ended up never having worked.
	BuildShadowVoxelGeometry();
}

void UGaussianSplatComponent::BuildShadowVoxelGeometry()
{
	if (!SplatAsset || !SplatAsset->IsValid())
	{
		return;
	}

	// Occupancy grid over the splat cloud, emitted as one box per occupied cell.
	//
	// The positions are read straight out of the bulk block with a stride rather
	// than through GetDecompressedPositions(): that helper allocates a TArray<FVector>
	// entry for every splat (24 B each, ~1.4 GB at 57M splats), which a shadow cache
	// must not do. The only allocation here is the occupancy bit array, and its size
	// is capped below.
	const int64 SplatCount = SplatAsset->GetSplatCount();
	if (SplatCount <= 0 || SplatAsset->PositionBulkData.GetBulkDataSize() <= 0)
	{
		return;
	}

	// Same record layout GetDecompressedPositions() reads: 3 x float32 (12 B).
	constexpr int32 PositionBytesPerSplat = 12;
	constexpr int64 MaxSplatsToScan = 2ll * 1024 * 1024;

	const FBox LocalBounds = SplatAsset->GetBounds();
	if (!LocalBounds.IsValid || LocalBounds.GetSize().IsNearlyZero())
	{
		return;
	}

	const uint8* DataPtr = static_cast<const uint8*>(SplatAsset->PositionBulkData.LockReadOnly());
	if (!DataPtr)
	{
		UE_LOG(LogTemp, Warning, TEXT("GaussianSplat: position bulk data could not be locked for shadow geometry"));
		return;
	}
	ON_SCOPE_EXIT
	{
		SplatAsset->PositionBulkData.Unlock();
	};

	const int64 ScanStride = FMath::Max<int64>(1, SplatCount / MaxSplatsToScan);
	const FVector Extent = LocalBounds.GetExtent();
	const FVector Origin = LocalBounds.Min;

	// The cell size starts at roughly 128 cells across the longest axis and doubles
	// until the occupied count fits the budget, so the box — and therefore the
	// triangle — count has a hard ceiling no matter how large the asset is.
	const int32 MaxVoxels = 4096;
	static constexpr int64 MaxGridCells = 32ll * 1024 * 1024;   // 4 MB of bit array
	static constexpr int32 MaxGrowAttempts = 24;

	float CellSize = FMath::Max((float)(Extent.Size() / 128.0), 1.0f);
	int64 GridX = 1;
	int64 GridY = 1;
	int64 GridZ = 1;
	TBitArray<> Occupied;
	int32 OccupiedCount = 0;

	for (int32 Attempt = 0; Attempt < MaxGrowAttempts; ++Attempt)
	{
		GridX = FMath::Max<int64>(1, (int64)(Extent.X / CellSize) + 1);
		GridY = FMath::Max<int64>(1, (int64)(Extent.Y / CellSize) + 1);
		GridZ = FMath::Max<int64>(1, (int64)(Extent.Z / CellSize) + 1);

		const int64 TotalCells = GridX * GridY * GridZ;
		if (TotalCells > MaxGridCells)
		{
			CellSize *= 2.0f;
			continue;
		}

		Occupied.Init(false, (int32)TotalCells);
		const int64 SliceStride = GridX * GridY;
		const float InvCellSize = 1.0f / CellSize;

		OccupiedCount = 0;
		for (int64 SplatIndex = 0; SplatIndex < SplatCount; SplatIndex += ScanStride)
		{
			const float* Position = reinterpret_cast<const float*>(DataPtr + SplatIndex * PositionBytesPerSplat);
			const int64 gx = FMath::Clamp<int64>((int64)((Position[0] - Origin.X) * InvCellSize), 0, GridX - 1);
			const int64 gy = FMath::Clamp<int64>((int64)((Position[1] - Origin.Y) * InvCellSize), 0, GridY - 1);
			const int64 gz = FMath::Clamp<int64>((int64)((Position[2] - Origin.Z) * InvCellSize), 0, GridZ - 1);
			const int32 CellIndex = (int32)(gx + gy * GridX + gz * SliceStride);
			if (!(bool)Occupied[CellIndex])
			{
				Occupied[CellIndex] = true;
				++OccupiedCount;
			}
		}

		if (OccupiedCount == 0)
		{
			UE_LOG(LogTemp, Warning, TEXT("GaussianSplat: no occupied voxels could be found for shadow geometry; shadows stay off"));
			return;
		}
		if (OccupiedCount <= MaxVoxels)
		{
			break;
		}
		CellSize *= 2.0f;
	}

	// One box per occupied cell: 8 corners, 12 triangles. Neighbouring cells do not
	// share corners, which keeps this a straight loop with no vertex dedup table —
	// and the internal faces between cells are harmless because the batch is
	// submitted with backface culling off (see AddShadowMeshBatch).
	ShadowShellVertices.Reserve(OccupiedCount * 8);
	ShadowShellTriangles.Reserve(OccupiedCount * 36);

	// Winding is irrelevant for the same reason.
	static const int32 BoxFaces[12][3] =
	{
		{ 0, 1, 2 }, { 0, 2, 3 },   // -Z
		{ 4, 6, 5 }, { 4, 7, 6 },   // +Z
		{ 0, 4, 5 }, { 0, 5, 1 },   // -Y
		{ 3, 2, 6 }, { 3, 6, 7 },   // +Y
		{ 0, 3, 7 }, { 0, 7, 4 },   // -X
		{ 1, 5, 6 }, { 1, 6, 2 }    // +X
	};

	int32 BoxCount = 0;
	for (int64 z = 0; z < GridZ; ++z)
	{
		for (int64 y = 0; y < GridY; ++y)
		{
			for (int64 x = 0; x < GridX; ++x)
			{
				if (!(bool)Occupied[(int32)(x + y * GridX + z * GridX * GridY)])
				{
					continue;
				}

				const FVector3f Min(
					(float)(Origin.X + (double)x * CellSize),
					(float)(Origin.Y + (double)y * CellSize),
					(float)(Origin.Z + (double)z * CellSize));
				const FVector3f Max(Min.X + CellSize, Min.Y + CellSize, Min.Z + CellSize);

				const int32 Base = ShadowShellVertices.Num();
				ShadowShellVertices.Add(FVector3f(Min.X, Min.Y, Min.Z));
				ShadowShellVertices.Add(FVector3f(Max.X, Min.Y, Min.Z));
				ShadowShellVertices.Add(FVector3f(Max.X, Max.Y, Min.Z));
				ShadowShellVertices.Add(FVector3f(Min.X, Max.Y, Min.Z));
				ShadowShellVertices.Add(FVector3f(Min.X, Min.Y, Max.Z));
				ShadowShellVertices.Add(FVector3f(Max.X, Min.Y, Max.Z));
				ShadowShellVertices.Add(FVector3f(Max.X, Max.Y, Max.Z));
				ShadowShellVertices.Add(FVector3f(Min.X, Max.Y, Max.Z));

				for (int32 FaceIndex = 0; FaceIndex < 12; ++FaceIndex)
				{
					ShadowShellTriangles.Add(Base + BoxFaces[FaceIndex][0]);
					ShadowShellTriangles.Add(Base + BoxFaces[FaceIndex][1]);
					ShadowShellTriangles.Add(Base + BoxFaces[FaceIndex][2]);
				}
				++BoxCount;
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("GaussianSplat: shadow geometry from splat voxels (boxes=%d, cell=%.3f, grid=%dx%dx%d, scanned=%lld of %lld splats)"),
		BoxCount, CellSize, (int32)GridX, (int32)GridY, (int32)GridZ, SplatCount / ScanStride, SplatCount);
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
	// GenerateVoxelCollision appends boxes), so OutVertices stays empty and
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
	// 球心就是 splat 自己的位置，所以碰撞体积与点云一一对应；球体是解析形状，
	// 不依赖 Chaos cooking 出来的隐式网格，因此编辑器和运行时表现一致
	// （ConvexHull / ConvexDecomposition / ProxyMesh 都要等烹饪结果到齐）。
	//
	// 消耗护栏：球体数量上限 MaxSplatSpheres。超过上限就按等步长抽稀到上限，
	// 并把半径放大到 sqrt3(步长) 倍，让留下的球体继续填满同一块体积，而不是
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

	// 半径：用户没指定就按「球体刚好互相贴住」算 —— 每个球体分到的体积是
	// 点云体积 / SphereCount，等体积球的半径即 0.5 * sqrt3(体积/数量)。
	float Radius = SplatSphereRadius;
	if (Radius <= 0.0f)
	{
		FBox LocalBounds(ForceInit);
		for (int32 i = 0; i < Points.Num(); i += Stride)
		{
			LocalBounds += Points[i];
		}
		const double Volume = (double)LocalBounds.GetVolume();
		Radius = Volume > 0.0
			? (float)(0.5 * FMath::Pow(Volume / (double)SphereCount, 1.0 / 3.0))
			: 1.0f;
	}

	BodySetup->AggGeom.SphereElems.Reserve(SphereCount);
	for (int32 i = 0; i < Points.Num(); i += Stride)
	{
		FKSphereElem& SphereElem = BodySetup->AggGeom.SphereElems.AddDefaulted_GetRef();
		SphereElem.Center = Points[i];
		SphereElem.Radius = Radius;
	}

	OutVertices.Reset();
	OutIndices.Reset();

	UE_LOG(LogTemp, Log, TEXT("GaussianSplat: SplatSpheres produced %d spheres (points=%d, cap=%d, stride=%d, radius=%.3f)"),
		BodySetup->AggGeom.SphereElems.Num(), Points.Num(), Cap, Stride, Radius);

	return BodySetup->AggGeom.SphereElems.Num() > 0;
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