#include "OpenSplat4DPointCloudActor.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "NiagaraDataInterfaceCurve.h"
#include "Misc/EngineVersion.h"

// ---------------------------------------------------------------------------
// Engine-version-aware Niagara System loading
// ---------------------------------------------------------------------------
// Different UE engine versions produce binary-incompatible .uasset files.
// We ship pre-built Niagara System templates for each supported engine
// version under Content/Niagara/Templates/UE{Major}_{Minor}/.
//
// At runtime we detect the current engine version and try paths in this order:
//   1. Exact match:    Templates/UE5_8/NS_OpenSplat4D
//   2. Minor -1:       Templates/UE5_7/NS_OpenSplat4D
//   3. Minor -2:       Templates/UE5_6/NS_OpenSplat4D
//   ...down to UE5_3 (oldest supported)
//   4. Legacy fallback: Templates/UE5_3/NS_GaussianSplattingPointCloud
//
// When adding support for a new engine version, place the .uasset in the
// corresponding Templates folder and rebuild.  No code changes are needed.

static UNiagaraSystem* LoadVersionedNiagaraSystem()
{
	const int32 Major = FEngineVersion::Current().GetMajor();
	const int32 Minor = FEngineVersion::Current().GetMinor();

	// Build candidate paths: exact version first, then step down
	TArray<FString> Candidates;

	// 1. Exact match: UE{Major}.{Minor}
	Candidates.Add(FString::Printf(
		TEXT("/OpenSplat4D/Niagara/Templates/UE%d_%d/NS_OpenSplat4D.NS_OpenSplat4D"),
		Major, Minor));

	// 2–N. Step down minor versions
	for (int32 M = Minor - 1; M >= 3; --M)
	{
		Candidates.Add(FString::Printf(
			TEXT("/OpenSplat4D/Niagara/Templates/UE%d_%d/NS_OpenSplat4D.NS_OpenSplat4D"),
			Major, M));
	}

	// Final fallback: root path
	Candidates.Add(TEXT("/OpenSplat4D/Niagara/NS_OpenSplat4D.NS_OpenSplat4D"));

	// Legacy fallback removed — loading UE5.3's NS_GaussianSplattingPointCloud
	// in UE5.8 causes a fatal NameEntry assertion crash (Header.Len == 0).

	for (const FString& Path : Candidates)
	{
		if (UNiagaraSystem* Sys = LoadObject<UNiagaraSystem>(nullptr, *Path))
		{
			UE_LOG(LogTemp, Log, TEXT("OpenSplat4D: loaded Niagara System from '%s' (engine %d.%d)"),
				*Path, Major, Minor);
			return Sys;
		}
	}
	return nullptr;
}

AOpenSplat4DPointCloudActor::AOpenSplat4DPointCloudActor(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	UNiagaraComponent* NiagaraComp = GetNiagaraComponent();
	if (NiagaraComp)
	{
		NiagaraComp->SetAutoActivate(false);
	}
}

void AOpenSplat4DPointCloudActor::BeginPlay()
{
	Super::BeginPlay();

	if (PointCloud)
	{
		CurrentTime = PointCloud->TimeStart;
	}
	PushStateToNiagara();

	if (bAutoPlay)
	{
		Play();
	}
}

void AOpenSplat4DPointCloudActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bIsPlaying && PointCloud)
	{
		const float Span = PointCloud->TimeEnd - PointCloud->TimeStart;
		if (!FMath::IsNearlyZero(Span))
		{
			CurrentTime += (DeltaSeconds * PlayRate) * Span;
			if (CurrentTime > PointCloud->TimeEnd)
			{
				if (bLooping)
				{
					CurrentTime = PointCloud->TimeStart
						+ FMath::Fmod(CurrentTime - PointCloud->TimeStart, Span);
				}
				else
				{
					CurrentTime = PointCloud->TimeEnd;
					bIsPlaying = false;
				}
			}
		}
	}

	// Lightweight update: only pushes DI->Time. Does NOT reinitialize,
	// duplicate DIs, or touch the system asset — those happen once in Init.
	UpdateNiagaraState();
}

// ---- One-shot init (teacher SetSetupPointCloudToNiagaraComponent pattern) ----

void AOpenSplat4DPointCloudActor::InitNiagaraSystem()
{
	UNiagaraComponent* NiagaraComp = GetNiagaraComponent();
	if (!NiagaraComp || !PointCloud)
	{
		return;
	}

	// Auto-detect engine version and load matching Niagara System
	UNiagaraSystem* Sys = LoadVersionedNiagaraSystem();
	if (!Sys)
	{
		static bool bWarnedOnce = false;
		if (!bWarnedOnce)
		{
			bWarnedOnce = true;
			UE_LOG(LogTemp, Warning,
				TEXT("OpenSplat4D: NS_OpenSplat4D not found for current engine version. ")
				TEXT("Ensure the .uasset exists in Content/Niagara/Templates/UE%d.%d/"),
				FEngineVersion::Current().GetMajor(),
				FEngineVersion::Current().GetMinor());
		}
		return;
	}

	// SetAsset — teacher pattern, idempotent but only needed once.
	NiagaraComp->SetAsset(Sys);

	// Set fixed bounds per-cloud (teacher pattern).
	const FBox Bounds = PointCloud->CalcBounds();
	if (Bounds.IsValid)
	{
		NiagaraComp->SetSystemFixedBounds(Bounds);
	}

	// ---- Wire the PointCloud DI (teacher pattern, extended for 4DGS) ----
	UNiagaraDataInterfaceOpenSplat4D* DI = UNiagaraFunctionLibrary::GetDataInterface<UNiagaraDataInterfaceOpenSplat4D>(
		NiagaraComp, TEXT("User.PointCloud"));
	if (DI)
	{
		DI->SetPointCloud(PointCloud);
		DI->Time = CurrentTime;
		DI->SplatScale = SplatScale;
		DI->bTemporalWeighting = (PointCloud->Mode == EOpenSplat4DMode::Dynamic4D);

#if WITH_EDITOR
		// Duplicate-and-override so each actor instance gets its own DI state.
		// Teacher uses DuplicateObject (not StaticDuplicateObject).
		UNiagaraDataInterfaceOpenSplat4D* VariantDI = Cast<UNiagaraDataInterfaceOpenSplat4D>(
			DuplicateObject(DI, NiagaraComp));
		if (VariantDI)
		{
			VariantDI->SetPointCloud(PointCloud);
			VariantDI->Time = CurrentTime;
			VariantDI->SplatScale = SplatScale;
			VariantDI->bTemporalWeighting = (PointCloud->Mode == EOpenSplat4DMode::Dynamic4D);
			NiagaraComp->SetParameterOverride(
				FNiagaraVariableBase(FNiagaraTypeDefinition(UNiagaraDataInterfaceOpenSplat4D::StaticClass()), TEXT("User.PointCloud")),
				FNiagaraVariant(VariantDI));
		}
#endif
	}
	else
	{
		static bool bWarnedDIMissing = false;
		if (!bWarnedDIMissing)
		{
			bWarnedDIMissing = true;
			UE_LOG(LogTemp, Warning,
				TEXT("OpenSplat4D: 'PointCloud' data interface not found in Niagara system. "
					 "The Niagara assets may be from an older version. Delete NS_OpenSplat4D and NE_OpenSplat4D "
					 "from /OpenSplat4D/Niagara/ and reimport a .ply file to regenerate them."));
		}
		bNiagaraInitialized = true;
		return;
	}

	// ---- Wire the FeatureCurve DI (teacher pattern, for LOD) ----
	if (UNiagaraDataInterfaceCurve* CurveDI = UNiagaraFunctionLibrary::GetDataInterface<UNiagaraDataInterfaceCurve>(
			NiagaraComp, TEXT("User.FeatureCurve")))
	{
		CurveDI->Curve = PointCloud->CalcFeatureCurve();
#if WITH_EDITORONLY_DATA
		CurveDI->UpdateLUT();  // [FIX] Teacher calls this — without it GPU LUT is stale.
#endif
#if WITH_EDITOR
		UNiagaraDataInterfaceCurve* VariantCurveDI = Cast<UNiagaraDataInterfaceCurve>(
			DuplicateObject(CurveDI, NiagaraComp));
		if (VariantCurveDI)
		{
			VariantCurveDI->Curve = CurveDI->Curve;
#if WITH_EDITORONLY_DATA
			VariantCurveDI->UpdateLUT();  // [FIX] Teacher updates LUT on variant too.
#endif
			NiagaraComp->SetParameterOverride(
				FNiagaraVariableBase(FNiagaraTypeDefinition(UNiagaraDataInterfaceCurve::StaticClass()), TEXT("User.FeatureCurve")),
				FNiagaraVariant(VariantCurveDI));
		}
#endif
	}

	// Activate or reinitialize (teacher pattern).
	if (NiagaraComp->IsActive())
	{
		NiagaraComp->ReinitializeSystem();
	}
	else
	{
		NiagaraComp->Activate(true);
	}

	bNiagaraInitialized = true;
}

// ---- Lightweight per-frame update (teacher does NOT do this every frame) ----

void AOpenSplat4DPointCloudActor::UpdateNiagaraState()
{
	if (!bNiagaraInitialized || !PointCloud)
	{
		return;
	}

	UNiagaraComponent* NiagaraComp = GetNiagaraComponent();
	if (!NiagaraComp || !NiagaraComp->IsActive())
	{
		return;
	}

	// Only update the DI properties that change per-frame.
	// Do NOT call SetAsset, ReinitializeSystem, SetParameterOverride, or DuplicateObject here.
	UNiagaraDataInterfaceOpenSplat4D* DI = UNiagaraFunctionLibrary::GetDataInterface<UNiagaraDataInterfaceOpenSplat4D>(
		NiagaraComp, TEXT("User.PointCloud"));
	if (DI)
	{
		DI->Time = CurrentTime;
		DI->SplatScale = SplatScale;
		DI->bTemporalWeighting = (PointCloud->Mode == EOpenSplat4DMode::Dynamic4D);
	}
}

// ---- Full push (init + update) — used by SetPointCloud / SetMode / Seek ----

void AOpenSplat4DPointCloudActor::PushStateToNiagara()
{
	if (!bNiagaraInitialized)
	{
		InitNiagaraSystem();
	}
	else
	{
		// If already initialized, we need to refresh DI data and possibly
		// reinitialize the system to pick up new point cloud / mode changes.
		UNiagaraComponent* NiagaraComp = GetNiagaraComponent();
		if (!NiagaraComp || !PointCloud)
		{
			return;
		}

		UNiagaraDataInterfaceOpenSplat4D* DI = UNiagaraFunctionLibrary::GetDataInterface<UNiagaraDataInterfaceOpenSplat4D>(
			NiagaraComp, TEXT("User.PointCloud"));
		if (DI)
		{
			DI->SetPointCloud(PointCloud);
			DI->Time = CurrentTime;
			DI->SplatScale = SplatScale;
			DI->bTemporalWeighting = (PointCloud->Mode == EOpenSplat4DMode::Dynamic4D);
		}

		if (UNiagaraDataInterfaceCurve* CurveDI = UNiagaraFunctionLibrary::GetDataInterface<UNiagaraDataInterfaceCurve>(
				NiagaraComp, TEXT("User.FeatureCurve")))
		{
			CurveDI->Curve = PointCloud->CalcFeatureCurve();
#if WITH_EDITORONLY_DATA
				CurveDI->UpdateLUT();
#endif
		}

		if (NiagaraComp->IsActive())
		{
			NiagaraComp->ReinitializeSystem();
		}
	}
}

// ---- Public API -------------------------------------------------------------

void AOpenSplat4DPointCloudActor::SetPointCloud(UOpenSplat4DPointCloud* InCloud)
{
	PointCloud = InCloud;
	if (PointCloud)
	{
		CurrentTime = PointCloud->TimeStart;
	}
	bNiagaraInitialized = false;  // Force re-init with new cloud
	PushStateToNiagara();
}

void AOpenSplat4DPointCloudActor::SetMode(EOpenSplat4DMode InMode)
{
	if (PointCloud)
	{
		PointCloud->Mode = InMode;
		PointCloud->OnPointsChanged.Broadcast();
	}
	PushStateToNiagara();
}

void AOpenSplat4DPointCloudActor::Play()
{
	bIsPlaying = true;
}

void AOpenSplat4DPointCloudActor::Pause()
{
	bIsPlaying = false;
}

void AOpenSplat4DPointCloudActor::Seek(float Time)
{
	if (PointCloud)
	{
		CurrentTime = FMath::Clamp(Time, PointCloud->TimeStart, PointCloud->TimeEnd);
	}
	PushStateToNiagara();
}

void AOpenSplat4DPointCloudActor::SeekFraction(float Fraction)
{
	if (PointCloud)
	{
		Seek(PointCloud->FractionToTime(Fraction));
	}
}

float AOpenSplat4DPointCloudActor::GetFraction() const
{
	return PointCloud ? PointCloud->TimeToFraction(CurrentTime) : 0.f;
}

// ---- [EDIT] Point editing API -----------------------------------------------

bool AOpenSplat4DPointCloudActor::GetPoint(int32 Index, FVector3f& OutPosition, FQuat4f& OutQuat, FVector3f& OutScale, FLinearColor& OutColor) const
{
	if (!PointCloud || Index < 0 || Index >= PointCloud->GetPointCount())
	{
		return false;
	}
	const TArray<FOpenSplat4DPoint>& Points = PointCloud->GetPoints();
	const FOpenSplat4DPoint& P = Points[Index];
	OutPosition = FVector3f(GetActorTransform().TransformPosition(FVector(P.Position)));
	OutQuat = P.Quat;
	OutScale = P.Scale;
	OutColor = P.Color;
	return true;
}

bool AOpenSplat4DPointCloudActor::UpdatePoint(int32 Index, FVector3f NewPosition, FQuat4f NewQuat, FVector3f NewScale, FLinearColor NewColor)
{
	if (!PointCloud || Index < 0 || Index >= PointCloud->GetPointCount())
	{
		return false;
	}

	// Get mutable access to the point array (we go through SetPoints with bReorder=false
	// to keep the existing order, but for single-point updates we directly modify
	// since the scale-order doesn't change significantly).
	TArray<FOpenSplat4DPoint> Points = PointCloud->GetPoints();
	FOpenSplat4DPoint& P = Points[Index];
	P.Position = FVector3f(GetActorTransform().InverseTransformPosition(FVector(NewPosition)));
	P.Quat = NewQuat;
	P.Scale = NewScale;
	P.Color = NewColor;

	// Use SetPoints with bReorder=false to preserve ordering and trigger GPU refresh
	const_cast<UOpenSplat4DPointCloud*>(PointCloud.Get())->SetPoints(Points, /*bReorder=*/false);

	return true;
}

void AOpenSplat4DPointCloudActor::TransformPoints(const FTransform& Transform, const TArray<int32>& Indices)
{
	if (!PointCloud || Indices.Num() == 0)
	{
		return;
	}

	TArray<FOpenSplat4DPoint> Points = PointCloud->GetPoints();
	for (int32 Idx : Indices)
	{
		if (Idx >= 0 && Idx < Points.Num())
		{
			FOpenSplat4DPoint& P = Points[Idx];
			FVector3f WorldPos = FVector3f(GetActorTransform().TransformPosition(FVector(P.Position)));
			WorldPos = FVector3f(Transform.TransformPosition(FVector(WorldPos)));
			P.Position = FVector3f(GetActorTransform().InverseTransformPosition(FVector(WorldPos)));
			P.Quat = FQuat4f(Transform.GetRotation() * FQuat(P.Quat));
			P.Scale = P.Scale * FVector3f(Transform.GetScale3D());
		}
	}
	const_cast<UOpenSplat4DPointCloud*>(PointCloud.Get())->SetPoints(Points, /*bReorder=*/false);
}

void AOpenSplat4DPointCloudActor::RemovePoints(const TArray<int32>& IndicesToRemove)
{
	if (!PointCloud || IndicesToRemove.Num() == 0)
	{
		return;
	}

	TArray<FOpenSplat4DPoint> Points = PointCloud->GetPoints();

	// Sort and deduplicate indices, then remove in reverse order
	TArray<int32> SortedIndices = IndicesToRemove;
	SortedIndices.Sort();
	for (int32 i = SortedIndices.Num() - 1; i >= 0; --i)
	{
		// Skip duplicates
		if (i < SortedIndices.Num() - 1 && SortedIndices[i] == SortedIndices[i + 1])
		{
			continue;
		}
		const int32 Idx = SortedIndices[i];
		if (Idx >= 0 && Idx < Points.Num())
		{
			Points.RemoveAt(Idx);
		}
	}

	if (Points.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("OpenSplat4D: RemovePoints removed all points from cloud."));
	}
	const_cast<UOpenSplat4DPointCloud*>(PointCloud.Get())->SetPoints(Points, /*bReorder=*/false);
}

void AOpenSplat4DPointCloudActor::SetPointsColor(const TArray<int32>& Indices, FLinearColor NewColor)
{
	if (!PointCloud || Indices.Num() == 0)
	{
		return;
	}

	TArray<FOpenSplat4DPoint> Points = PointCloud->GetPoints();
	for (int32 Idx : Indices)
	{
		if (Idx >= 0 && Idx < Points.Num())
		{
			Points[Idx].Color = NewColor;
		}
	}
	const_cast<UOpenSplat4DPointCloud*>(PointCloud.Get())->SetPoints(Points, /*bReorder=*/false);
}

void AOpenSplat4DPointCloudActor::ScalePoints(const TArray<int32>& Indices, float ScaleMultiplier)
{
	if (!PointCloud || Indices.Num() == 0 || ScaleMultiplier <= 0.f)
	{
		return;
	}

	TArray<FOpenSplat4DPoint> Points = PointCloud->GetPoints();
	for (int32 Idx : Indices)
	{
		if (Idx >= 0 && Idx < Points.Num())
		{
			Points[Idx].Scale *= ScaleMultiplier;
		}
	}
	const_cast<UOpenSplat4DPointCloud*>(PointCloud.Get())->SetPoints(Points, /*bReorder=*/false);
}

void AOpenSplat4DPointCloudActor::RefreshRenderState()
{
	if (!bNiagaraInitialized)
	{
		InitNiagaraSystem();
		return;
	}

	UNiagaraComponent* NiagaraComp = GetNiagaraComponent();
	if (!NiagaraComp || !PointCloud)
	{
		return;
	}

	// [OPT] Lightweight: only refresh DI data without reinitializing the system.
	// This is much faster than PushStateToNiagara for in-place edits.
	UNiagaraDataInterfaceOpenSplat4D* DI = UNiagaraFunctionLibrary::GetDataInterface<UNiagaraDataInterfaceOpenSplat4D>(
		NiagaraComp, TEXT("User.PointCloud"));
	if (DI)
	{
		DI->RefreshPointCloud();
	}
}
