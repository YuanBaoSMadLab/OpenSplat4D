#include "OpenSplat4DSplatActor.h"
#include "Misc/EngineVersionComparison.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"

#if WITH_EDITOR
#include "EditorViewportClient.h"
#include "Editor.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "MaterialShared.h"
#include "MaterialShaderPrecompileMode.h"
#include "UObject/Package.h"
#endif

// ---------------------------------------------------------------------------
// v13: Masked Circular Billboard + WPO + Per-Point Radius + Distance LOD + Box Selection Editor
//   - Plane mesh, TinyScale=1e-5, Identity rotation (WPO handles billboard)
//   - Masked mode, hard-edge circle with alpha threshold clip
//   - WPO billboard: camera-facing circles sized by per-point Radius
//   - CustomData 6 floats: [R, G, B, A, Radius, Dither]
//   - Distance LOD (material + CPU):
//       * Material: LODScale param scales Radius for screen-size preservation
//       * Material: DitherFrac param controls random clipping of distant splats
//       * CPU Tick: updates params based on camera distance each frame
//   - Box Selection Editor: edit-mode shows a dragable BoxComponent in viewport,
//       adjust SelectionPreviewScale slider to preview size changes for points
//       inside the box, call ApplyScaleToSelection to write to asset's
//       PerPointSizeScale array (serialized, saved with the PointCloud asset).
// ---------------------------------------------------------------------------

static UMaterial* GetOrCreateSplatISCMaterial()
{
#if WITH_EDITOR
	static UMaterial* CachedMat = nullptr;
	static constexpr int32 MatVersion = 16;  // v13: selection editor + per-point size scale
	static int32 CachedVersion = 0;
	if (CachedMat && IsValid(CachedMat) && CachedVersion == MatVersion)
	{
		return CachedMat;
	}
	if (CachedMat)
	{
		UE_LOG(LogOpenSplat4D, Log, TEXT("SplatActor: v13: material version changed (%d→%d), recreating"), CachedVersion, MatVersion);
		CachedMat = nullptr;
	}
	CachedVersion = MatVersion;

	UE_LOG(LogOpenSplat4D, Log, TEXT("SplatActor: v13 creating Masked Circular Billboard material with LOD"));

	constexpr uint32 MASK_R = 1;
	constexpr uint32 MASK_RG = 3;
	constexpr uint32 MASK_RGB = 7;
	constexpr uint32 MASK_RGBA = 15;

	UPackage* Pkg = GetTransientPackage();
	if (!Pkg) { UE_LOG(LogOpenSplat4D, Error, TEXT("SplatActor: v13: GetTransientPackage FAILED")); return nullptr; }
	UMaterial* Mat = NewObject<UMaterial>(Pkg, NAME_None, RF_Transient | RF_Public);
	if (!Mat) { UE_LOG(LogOpenSplat4D, Error, TEXT("SplatActor: v13: NewObject FAILED")); return nullptr; }

	Mat->MaterialDomain = MD_Surface;
	Mat->BlendMode = BLEND_Masked;
	Mat->SetShadingModel(MSM_Unlit);
	Mat->TwoSided = true;
	Mat->bEnableResponsiveAA = false;
#if UE_VERSION_OLDER_THAN(5, 8, 0)
	{	bool bNeedsRecompile = false; Mat->SetMaterialUsage(bNeedsRecompile, MATUSAGE_InstancedStaticMeshes); }
#else
	Mat->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
#endif
	Mat->bAutomaticallySetUsageInEditor = false;
	Mat->bDisableDepthTest = false;
	Mat->OpacityMaskClipValue = 0.5f;

	UMaterialEditorOnlyData* Ed = Mat->GetEditorOnlyData();
	if (!Ed) { UE_LOG(LogOpenSplat4D, Error, TEXT("SplatActor: v13: GetEditorOnlyData NULL")); return nullptr; }

	auto AddExpr = [Mat](UMaterialExpression* E, int X, int Y) -> UMaterialExpression*
	{
		E->MaterialExpressionEditorX = X;
		E->MaterialExpressionEditorY = Y;
		Mat->GetExpressionCollection().AddExpression(E);
		return E;
	};

	auto ConnectInput = [](FExpressionInput& Dst, UMaterialExpression* Src, uint32 Mask)
	{
		Dst.Expression = Src;
		Dst.OutputIndex = 0;
		Dst.Mask = Mask;
		Dst.MaskR = (Mask & 1) != 0;
		Dst.MaskG = (Mask & 2) != 0;
		Dst.MaskB = (Mask & 4) != 0;
		Dst.MaskA = (Mask & 8) != 0;
	};

	// UV
	auto* TexCoord = CastChecked<UMaterialExpressionTextureCoordinate>(
		AddExpr(NewObject<UMaterialExpressionTextureCoordinate>(Mat), -800, 100));
	TexCoord->CoordinateIndex = 0;
	TexCoord->UTiling = 1.0f;
	TexCoord->VTiling = 1.0f;

	// Color RGB (CustomData floats 0,1,2)
	auto* Color3 = CastChecked<UMaterialExpressionPerInstanceCustomData3Vector>(
		AddExpr(NewObject<UMaterialExpressionPerInstanceCustomData3Vector>(Mat), -600, -100));
	Color3->DataIndex = 0;
	Color3->ConstDefaultValue = FLinearColor(1.0f, 0.0f, 1.0f, 1.0f);

	// Alpha (CustomData float 3)
	auto* AlphaS = CastChecked<UMaterialExpressionPerInstanceCustomData>(
		AddExpr(NewObject<UMaterialExpressionPerInstanceCustomData>(Mat), -800, 50));
	AlphaS->DataIndex = 3;
	AlphaS->ConstDefaultValue = 1.0f;

	// Radius (CustomData float 4)
	auto* RadiusS = CastChecked<UMaterialExpressionPerInstanceCustomData>(
		AddExpr(NewObject<UMaterialExpressionPerInstanceCustomData>(Mat), -800, -100));
	RadiusS->DataIndex = 4;
	RadiusS->ConstDefaultValue = 1.0f;

	// Dither random (CustomData float 5)
	auto* DitherS = CastChecked<UMaterialExpressionPerInstanceCustomData>(
		AddExpr(NewObject<UMaterialExpressionPerInstanceCustomData>(Mat), -800, 0));
	DitherS->DataIndex = 5;
	DitherS->ConstDefaultValue = 0.5f;

	// LOD Scale scalar parameter (updated from C++ Tick)
	auto* LODScaleParam = CastChecked<UMaterialExpressionScalarParameter>(
		AddExpr(NewObject<UMaterialExpressionScalarParameter>(Mat), -600, -200));
	LODScaleParam->ParameterName = TEXT("LODScale");
	LODScaleParam->DefaultValue = 1.0f;

	// Dither Fraction scalar parameter (0=all visible, 1=all clipped; updated from C++ Tick)
	auto* DitherFracParam = CastChecked<UMaterialExpressionScalarParameter>(
		AddExpr(NewObject<UMaterialExpressionScalarParameter>(Mat), -600, -150));
	DitherFracParam->ParameterName = TEXT("DitherFrac");
	DitherFracParam->DefaultValue = 0.0f;

	ConnectInput(Ed->EmissiveColor, Color3, MASK_RGB);

	// --- WPO: billboard expansion with LOD scale ---
	auto* WPOCustom = CastChecked<UMaterialExpressionCustom>(
		AddExpr(NewObject<UMaterialExpressionCustom>(Mat), -300, 200));
	WPOCustom->Description = TEXT("v13 WPO Billboard + LOD");
	WPOCustom->OutputType = CMOT_Float3;
	WPOCustom->Inputs.SetNum(3);
	WPOCustom->Inputs[0].InputName = TEXT("UV");
	WPOCustom->Inputs[1].InputName = TEXT("Radius");
	WPOCustom->Inputs[2].InputName = TEXT("LODScale");
	WPOCustom->Code = TEXT(R"(
float2 dir = (UV - 0.5) * 2;
float effR = Radius * LODScale;
return ResolvedView.ViewRight * dir.x * effR + ResolvedView.ViewUp * dir.y * effR;
)");
	ConnectInput(WPOCustom->Inputs[0].Input, TexCoord, MASK_RG);
	ConnectInput(WPOCustom->Inputs[1].Input, RadiusS, MASK_R);
	ConnectInput(WPOCustom->Inputs[2].Input, LODScaleParam, MASK_R);
	ConnectInput(Ed->WorldPositionOffset, WPOCustom, MASK_RGB);

	// --- OpacityMask: hard circle + distance dithering + alpha threshold ---
	auto* OpacityCustom = CastChecked<UMaterialExpressionCustom>(
		AddExpr(NewObject<UMaterialExpressionCustom>(Mat), -300, 0));
	OpacityCustom->Description = TEXT("v13 Circle Mask + Dither + Alpha");
	OpacityCustom->OutputType = CMOT_Float1;
	OpacityCustom->Inputs.SetNum(4);
	OpacityCustom->Inputs[0].InputName = TEXT("UV");
	OpacityCustom->Inputs[1].InputName = TEXT("Dither");
	OpacityCustom->Inputs[2].InputName = TEXT("DitherFrac");
	OpacityCustom->Inputs[3].InputName = TEXT("Alpha");
	OpacityCustom->Code = TEXT(R"(
float2 d = UV - 0.5;
float r2 = dot(d,d) * 4.0;
float circle = (r2 < 1.0) ? 1.0 : 0.0;
float keep = (Dither >= DitherFrac) ? 1.0 : 0.0;
float avis = (Alpha > 0.1) ? 1.0 : 0.0;
return circle * keep * avis;
)");
	ConnectInput(OpacityCustom->Inputs[0].Input, TexCoord, MASK_RG);
	ConnectInput(OpacityCustom->Inputs[1].Input, DitherS, MASK_R);
	ConnectInput(OpacityCustom->Inputs[2].Input, DitherFracParam, MASK_R);
	ConnectInput(OpacityCustom->Inputs[3].Input, AlphaS, MASK_R);
	ConnectInput(Ed->OpacityMask, OpacityCustom, MASK_R);

	UE_LOG(LogOpenSplat4D, Log, TEXT("SplatActor: v13: compiling material shaders..."));

	{
		FMaterialUpdateContext UpdateContext;
		Mat->PreEditChange(nullptr);
		Mat->PostEditChange();
	}
	Mat->CacheShaders(EMaterialShaderPrecompileMode::Synchronous);

	UE_LOG(LogOpenSplat4D, Log,
		TEXT("SplatActor: v13 dynamic material created! Blend=Masked(%d) ISMC=%d TwoSided=%d ClipVal=%.1f Transient"),
		(int)Mat->BlendMode,
		Mat->GetUsageByFlag(MATUSAGE_InstancedStaticMeshes)?1:0, Mat->TwoSided?1:0, Mat->OpacityMaskClipValue);

	CachedMat = Mat;
	return Mat;
#else
	return nullptr;
#endif
}

// ---------------------------------------------------------------------------
// Actor
// ---------------------------------------------------------------------------
AOpenSplat4DSplatActor::AOpenSplat4DSplatActor(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	UE_LOG(LogOpenSplat4D, Log, TEXT("SplatActor: v13 CONSTRUCTOR"));

	SplatComponent = ObjectInitializer.CreateDefaultSubobject<UInstancedStaticMeshComponent>(
		this, TEXT("SplatComponent"));
	RootComponent = SplatComponent;

	SelectionBox = ObjectInitializer.CreateDefaultSubobject<UBoxComponent>(
		this, TEXT("SelectionBox"));
	SelectionBox->SetupAttachment(RootComponent);
	SelectionBox->SetBoxExtent(FVector(50.0f, 50.0f, 50.0f));
	SelectionBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SelectionBox->SetHiddenInGame(true);
	SelectionBox->ShapeColor = FColor(0, 255, 0, 128);

	DynMaterial = nullptr;
	CurrentLODScale = 1.0f;
	CurrentDitherFrac = 0.0f;
	SelectionPreviewScale = 1.0f;
	SelectedPointCount = 0;
	bSelectionDirty = true;

	if (SplatComponent)
	{
		SplatComponent->NumCustomDataFloats = 6;

		SplatComponent->CastShadow = false;
		SplatComponent->bAffectDynamicIndirectLighting = false;
		SplatComponent->bAffectDistanceFieldLighting = false;
		SplatComponent->bCastDynamicShadow = false;
		SplatComponent->bCastStaticShadow = false;
		SplatComponent->bReceivesDecals = false;
		SplatComponent->bUseAsOccluder = false;

		SplatComponent->Mobility = EComponentMobility::Movable;
		SplatComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		SplatComponent->SetGenerateOverlapEvents(false);
		SplatComponent->bEvaluateWorldPositionOffset = true;
		SplatComponent->bEvaluateWorldPositionOffsetInRayTracing = false;

		static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneFinder(
			TEXT("/Engine/BasicShapes/Plane"));
		if (PlaneFinder.Succeeded())
		{
			SplatComponent->SetStaticMesh(PlaneFinder.Object);
			UE_LOG(LogOpenSplat4D, Log, TEXT("SplatActor: v13: Plane mesh loaded (%s)"),
				*PlaneFinder.Object->GetName());
		}
		else
		{
			UE_LOG(LogOpenSplat4D, Error, TEXT("SplatActor: v13: FAILED to load Plane mesh!"));
		}
	}

	UpdateSelectionBoxVisibility();
}

void AOpenSplat4DSplatActor::UpdateSelectionBoxVisibility()
{
	if (SelectionBox)
	{
		SelectionBox->SetVisibility(bEditMode);
		SelectionBox->SetHiddenInGame(!bEditMode);
	}
}

void AOpenSplat4DSplatActor::UpdateSelectedCount()
{
	if (!PointCloud || !SelectionBox)
	{
		SelectedPointCount = 0;
		CachedSelected.Reset();
		return;
	}

	const TArray<FOpenSplat4DPoint>& Points = PointCloud->GetPoints();
	const int32 PointCount = Points.Num();
	if (PointCount == 0)
	{
		SelectedPointCount = 0;
		CachedSelected.Reset();
		return;
	}

	FTransform BoxWorldXf = SelectionBox->GetComponentTransform();
	FVector BoxExtent = SelectionBox->GetScaledBoxExtent();
	FVector BoxCenter = BoxWorldXf.GetLocation();
	FQuat BoxRot = BoxWorldXf.GetRotation();

	FVector LocalMin = -BoxExtent;
	FVector LocalMax = BoxExtent;
	FTransform BoxToWorld = BoxWorldXf;
	FTransform WorldToBox = BoxToWorld.Inverse();

	CachedSelected.SetNum(PointCount);
	int32 Count = 0;

	FVector3f MinPos(FLT_MAX), MaxPos(-FLT_MAX);
	for (int32 i = 0; i < PointCount; ++i)
	{
		MinPos = FVector3f(FMath::Min(MinPos.X,Points[i].Position.X),FMath::Min(MinPos.Y,Points[i].Position.Y),FMath::Min(MinPos.Z,Points[i].Position.Z));
		MaxPos = FVector3f(FMath::Max(MaxPos.X,Points[i].Position.X),FMath::Max(MaxPos.Y,Points[i].Position.Y),FMath::Max(MaxPos.Z,Points[i].Position.Z));
	}
	FVector CenterPos((MinPos.X+MaxPos.X)*0.5f, (MinPos.Y+MaxPos.Y)*0.5f, (MinPos.Z+MaxPos.Z)*0.5f);
	FVector Offset = GetActorLocation() - CenterPos;

	for (int32 i = 0; i < PointCount; ++i)
	{
		FVector WorldPos = FVector(Points[i].Position) + Offset;
		FVector LocalPos = WorldToBox.TransformPosition(WorldPos);
		bool bInside = (LocalPos.X >= LocalMin.X && LocalPos.X <= LocalMax.X &&
		                LocalPos.Y >= LocalMin.Y && LocalPos.Y <= LocalMax.Y &&
		                LocalPos.Z >= LocalMin.Z && LocalPos.Z <= LocalMax.Z);
		CachedSelected[i] = bInside;
		if (bInside) ++Count;
	}

	SelectedPointCount = Count;
	bSelectionDirty = false;
	UE_LOG(LogOpenSplat4D, Log, TEXT("SplatActor: Selection editor: %d / %d points inside selection box"), Count, PointCount);
}

void AOpenSplat4DSplatActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!SplatComponent || !PointCloud) return;

#if WITH_EDITOR
	if (bEditMode && SelectionBox && SelectionBox->IsRegistered())
	{
		if (SelectionBox->GetComponentTransform().GetLocation().Equals(LastSelectionBoxPos, 0.5f) == false ||
		    !SelectionBox->GetComponentTransform().GetRotation().Equals(LastSelectionBoxRot, 0.01f) ||
		    !SelectionBox->GetScaledBoxExtent().Equals(LastSelectionBoxExtent, 0.5f))
		{
			LastSelectionBoxPos = SelectionBox->GetComponentTransform().GetLocation();
			LastSelectionBoxRot = SelectionBox->GetComponentTransform().GetRotation();
			LastSelectionBoxExtent = SelectionBox->GetScaledBoxExtent();
			bSelectionDirty = true;
			bNeedsRebuild = true;
		}
	}
#endif

	if (bNeedsRebuild && PointCloud->GetPointCount() > 0)
	{
		bNeedsRebuild = false;
		RebuildInstances();
	}

	FVector CamPos = FVector::ZeroVector;
	bool bHasCamera = false;

	if (UWorld* World = GetWorld())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (APlayerCameraManager* CM = PC->PlayerCameraManager)
			{
				CamPos = CM->GetCameraLocation();
				bHasCamera = true;
			}
		}
	}

#if WITH_EDITOR
	if (!bHasCamera && GEditor)
	{
		FViewport* VP = GEditor->GetActiveViewport();
		if (VP)
		{
			FViewportClient* VC = VP->GetClient();
			FEditorViewportClient* EditorVC = static_cast<FEditorViewportClient*>(VC);
			if (EditorVC)
			{
				CamPos = EditorVC->GetViewLocation();
				bHasCamera = true;
			}
		}
	}
#endif

	if (!bHasCamera) return;

	FVector ActorCenter = GetActorLocation();
	float Dist = FVector::Dist(ActorCenter, CamPos);

	const float MinSplatScreenSize = PointCloud->MinSplatScreenSize;
	float LodScale = 1.0f;
	if (Dist > 1.0f && MinSplatScreenSize > 0.0f)
	{
		float MinWorldSize = Dist * MinSplatScreenSize;
		LodScale = FMath::Max(1.0f, MinWorldSize / 0.25f);
	}
	LodScale = FMath::Clamp(LodScale, 1.0f, 30.0f);

	const float DitherStart = PointCloud->LODDitherStartDistance;
	const float DitherEnd = PointCloud->LODDitherEndDistance;
	float DitherFrac = 0.0f;
	if (Dist > DitherStart && DitherEnd > DitherStart)
	{
		float T = FMath::Clamp((Dist - DitherStart) / (DitherEnd - DitherStart), 0.0f, 1.0f);
		DitherFrac = T * 0.9f;
	}

	if (FMath::Abs(LodScale - CurrentLODScale) > 0.01f || FMath::Abs(DitherFrac - CurrentDitherFrac) > 0.01f)
	{
		CurrentLODScale = LodScale;
		CurrentDitherFrac = DitherFrac;

		if (!DynMaterial)
		{
			if (UMaterial* BaseMat = GetOrCreateSplatISCMaterial())
			{
				DynMaterial = UMaterialInstanceDynamic::Create(BaseMat, this);
				SplatComponent->SetMaterial(0, DynMaterial);
			}
		}
		if (DynMaterial)
		{
			DynMaterial->SetScalarParameterValue(TEXT("LODScale"), CurrentLODScale);
			DynMaterial->SetScalarParameterValue(TEXT("DitherFrac"), CurrentDitherFrac);
		}
	}
}

void AOpenSplat4DSplatActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	if (SplatComponent)
	{
		if (UMaterial* Mat = GetOrCreateSplatISCMaterial())
		{
			DynMaterial = UMaterialInstanceDynamic::Create(Mat, this);
			SplatComponent->SetMaterial(0, DynMaterial);
		}
	}
	UpdateSelectionBoxVisibility();
	if (bNeedsRebuild) { bNeedsRebuild = false; RebuildInstances(); }
}

void AOpenSplat4DSplatActor::OnPointCloudChanged()
{
	bNeedsRebuild = true;
	bSelectionDirty = true;
}

void AOpenSplat4DSplatActor::SetPointCloud(UOpenSplat4DPointCloud* InCloud)
{
	if (OnPointsChangedHandle.IsValid())
	{
		if (PointCloud)
		{
			PointCloud->OnPointsChanged.Remove(OnPointsChangedHandle);
		}
		OnPointsChangedHandle.Reset();
	}

	if (PointCloud != InCloud)
	{
		PointCloud = InCloud;
		bNeedsRebuild = true;
		bSelectionDirty = true;

		if (PointCloud)
		{
			OnPointsChangedHandle = PointCloud->OnPointsChanged.AddUObject(this, &AOpenSplat4DSplatActor::OnPointCloudChanged);
		}

		if (SplatComponent && PointCloud && PointCloud->GetPointCount() > 0)
		{
			bNeedsRebuild = false;
			RebuildInstances();
		}
	}
}

#if WITH_EDITOR
void AOpenSplat4DSplatActor::PostEditChangeProperty(struct FPropertyChangedEvent& e)
{
	Super::PostEditChangeProperty(e);
	const FName PropName = e.GetPropertyName();
	if (PropName == GET_MEMBER_NAME_CHECKED(AOpenSplat4DSplatActor, PointCloud))
	{
		bNeedsRebuild = true;
		bSelectionDirty = true;
		if (OnPointsChangedHandle.IsValid() && PointCloud)
		{
			PointCloud->OnPointsChanged.Remove(OnPointsChangedHandle);
			OnPointsChangedHandle.Reset();
		}
		if (PointCloud)
		{
			OnPointsChangedHandle = PointCloud->OnPointsChanged.AddUObject(this, &AOpenSplat4DSplatActor::OnPointCloudChanged);
		}
		if (bNeedsRebuild) { bNeedsRebuild = false; RebuildInstances(); }
	}
	else if (PropName == GET_MEMBER_NAME_CHECKED(AOpenSplat4DSplatActor, SplatScale) ||
	         PropName == GET_MEMBER_NAME_CHECKED(AOpenSplat4DSplatActor, SelectionPreviewScale))
	{
		bNeedsRebuild = true;
		if (bNeedsRebuild) { bNeedsRebuild = false; RebuildInstances(); }
	}
	else if (PropName == GET_MEMBER_NAME_CHECKED(AOpenSplat4DSplatActor, bEditMode))
	{
		UpdateSelectionBoxVisibility();
		bSelectionDirty = true;
		bNeedsRebuild = true;
		if (bNeedsRebuild) { bNeedsRebuild = false; RebuildInstances(); }
	}
}
#endif

void AOpenSplat4DSplatActor::PostLoad()
{
	Super::PostLoad();
	if (PointCloud)
	{
		bNeedsRebuild = true;
		bSelectionDirty = true;
		OnPointsChangedHandle = PointCloud->OnPointsChanged.AddUObject(this, &AOpenSplat4DSplatActor::OnPointCloudChanged);
	}
	UpdateSelectionBoxVisibility();
}

void AOpenSplat4DSplatActor::PostActorCreated()
{
	Super::PostActorCreated();
	if (PointCloud)
	{
		bNeedsRebuild = true;
		bSelectionDirty = true;
	}
	UpdateSelectionBoxVisibility();
}

void AOpenSplat4DSplatActor::ApplyScaleToSelection()
{
	if (!PointCloud) return;
	if (bSelectionDirty)
	{
		UpdateSelectedCount();
	}
	if (CachedSelected.Num() != PointCloud->GetPointCount())
	{
		UpdateSelectedCount();
	}

	TArray<float>& Scales = PointCloud->PerPointSizeScale;
	if (Scales.Num() != PointCloud->GetPointCount())
	{
		Scales.SetNum(PointCloud->GetPointCount());
		for (int32 i = 0; i < Scales.Num(); ++i) Scales[i] = 1.0f;
	}

	const float Mult = SelectionPreviewScale;
	int32 Applied = 0;
	for (int32 i = 0; i < CachedSelected.Num(); ++i)
	{
		if (CachedSelected[i])
		{
			Scales[i] *= Mult;
			Scales[i] = FMath::Clamp(Scales[i], 0.01f, 50.0f);
			++Applied;
		}
	}

	SelectionPreviewScale = 1.0f;
	PointCloud->MarkPackageDirty();
	PointCloud->OnPointsChanged.Broadcast();

	UE_LOG(LogOpenSplat4D, Log, TEXT("SplatActor: Applied scale=%.3f to %d selected points"), Mult, Applied);
}

void AOpenSplat4DSplatActor::ResetSelectionPreview()
{
	SelectionPreviewScale = 1.0f;
	bNeedsRebuild = true;
	RebuildInstances();
}

void AOpenSplat4DSplatActor::SelectAllPoints()
{
	if (!PointCloud || !SelectionBox) return;
	const TArray<FOpenSplat4DPoint>& Points = PointCloud->GetPoints();
	if (Points.Num() == 0) return;

	FVector3f MinPos(FLT_MAX), MaxPos(-FLT_MAX);
	for (const FOpenSplat4DPoint& P : Points)
	{
		MinPos = FVector3f(FMath::Min(MinPos.X,P.Position.X),FMath::Min(MinPos.Y,P.Position.Y),FMath::Min(MinPos.Z,P.Position.Z));
		MaxPos = FVector3f(FMath::Max(MaxPos.X,P.Position.X),FMath::Max(MaxPos.Y,P.Position.Y),FMath::Max(MaxPos.Z,P.Position.Z));
	}
	FVector CenterPos((MinPos.X+MaxPos.X)*0.5f, (MinPos.Y+MaxPos.Y)*0.5f, (MinPos.Z+MaxPos.Z)*0.5f);
	FVector Extent((MaxPos.X-MinPos.X)*0.5f + 1.0f, (MaxPos.Y-MinPos.Y)*0.5f + 1.0f, (MaxPos.Z-MinPos.Z)*0.5f + 1.0f);
	FVector Offset = GetActorLocation() - CenterPos;

	SelectionBox->SetWorldLocationAndRotation(CenterPos + Offset, FQuat::Identity);
	SelectionBox->SetBoxExtent(Extent);
	bSelectionDirty = true;
	bNeedsRebuild = true;
	RebuildInstances();
}

void AOpenSplat4DSplatActor::ResetAllPointSizes()
{
	if (PointCloud)
	{
		PointCloud->ResetAllPointSizeScales();
	}
}

void AOpenSplat4DSplatActor::RebuildInstances()
{
	if (!SplatComponent || !PointCloud) return;
	const int32 PointCount = PointCloud->GetPointCount();
	if (PointCount == 0) { SplatComponent->ClearInstances(); return; }

	if (bEditMode && (bSelectionDirty || CachedSelected.Num() != PointCount))
	{
		UpdateSelectedCount();
	}

	const float EffectiveSplatScale = SplatScale * PointCloud->SplatScale;
	const float EffectiveMinRadius = PointCloud->MinSplatRadius;
	constexpr float MaxSplatRadius = 30.0f;

	const bool bUsePreviewScale = bEditMode && FMath::Abs(SelectionPreviewScale - 1.0f) > 0.001f;

	UE_LOG(LogOpenSplat4D, Log, TEXT("SplatActor: v13 rebuilding %d circular billboards (SplatScale=%.3f × %.3f = %.3f, MinRadius=%.2f, EditMode=%d, PreviewScale=%.3f, Selected=%d)"),
		PointCount, SplatScale, PointCloud->SplatScale, EffectiveSplatScale, EffectiveMinRadius,
		bEditMode?1:0, SelectionPreviewScale, SelectedPointCount);

	if (UMaterial* BaseMat = GetOrCreateSplatISCMaterial())
	{
		DynMaterial = UMaterialInstanceDynamic::Create(BaseMat, this);
		SplatComponent->SetMaterial(0, DynMaterial);
	}
	SplatComponent->ClearInstances();
	SplatComponent->NumCustomDataFloats = 6;

	const TArray<FOpenSplat4DPoint>& Points = PointCloud->GetPoints();
	TArray<float>& PerPtScale = PointCloud->PerPointSizeScale;
	if (PerPtScale.Num() != PointCount)
	{
		PerPtScale.SetNum(PointCount);
		for (int32 i = 0; i < PointCount; ++i) PerPtScale[i] = 1.0f;
	}

	const FQuat IdentRot = FQuat::Identity;
	constexpr float TinyScale = 0.00001f;

	FVector3f MinPos(FLT_MAX), MaxPos(-FLT_MAX);
	for (int32 i = 0; i < PointCount; ++i)
	{
		const FOpenSplat4DPoint& P = Points[i];
		MinPos = FVector3f(FMath::Min(MinPos.X,P.Position.X),FMath::Min(MinPos.Y,P.Position.Y),FMath::Min(MinPos.Z,P.Position.Z));
		MaxPos = FVector3f(FMath::Max(MaxPos.X,P.Position.X),FMath::Max(MaxPos.Y,P.Position.Y),FMath::Max(MaxPos.Z,P.Position.Z));
	}
	FVector CenterPos((MinPos.X+MaxPos.X)*0.5f, (MinPos.Y+MaxPos.Y)*0.5f, (MinPos.Z+MaxPos.Z)*0.5f);
	FVector ActorLoc = GetActorLocation();
	FVector Offset = ActorLoc - CenterPos;

	float MinR = FLT_MAX, MaxR = -FLT_MAX;
	for (int32 i = 0; i < PointCount; ++i)
	{
		const FOpenSplat4DPoint& P = Points[i];
		float SigmaX = P.Scale.X * 0.01f;
		float SigmaY = P.Scale.Y * 0.01f;
		float SigmaZ = P.Scale.Z * 0.01f;
		float MaxSigma = FMath::Max3(SigmaX, SigmaY, SigmaZ);
		float PtMult = PerPtScale[i];
		float PreviewMult = (bUsePreviewScale && CachedSelected.IsValidIndex(i) && CachedSelected[i]) ? SelectionPreviewScale : 1.0f;
		float R = MaxSigma * 3.0f * EffectiveSplatScale * PtMult * PreviewMult;
		MinR = FMath::Min(MinR, R);
		MaxR = FMath::Max(MaxR, R);
	}
	UE_LOG(LogOpenSplat4D, Log,
		TEXT("SplatActor: v13 radius raw=[%.4f..%.3f] clamped=[%.2f..%.1f] TinyScale=%g"),
		MinR, MaxR, (double)EffectiveMinRadius, (double)MaxSplatRadius, TinyScale);

	TArray<FTransform> Transforms;
	Transforms.Reserve(PointCount);
	TArray<float> CustomData;
	CustomData.SetNumZeroed(PointCount * 6);

	FRandomStream Rng(12345);

	for (int32 i = 0; i < PointCount; ++i)
	{
		const FOpenSplat4DPoint& P = Points[i];
		float SigmaX = P.Scale.X * 0.01f;
		float SigmaY = P.Scale.Y * 0.01f;
		float SigmaZ = P.Scale.Z * 0.01f;
		float MaxSigma = FMath::Max3(SigmaX, SigmaY, SigmaZ);
		float PtMult = PerPtScale[i];
		float PreviewMult = (bUsePreviewScale && CachedSelected.IsValidIndex(i) && CachedSelected[i]) ? SelectionPreviewScale : 1.0f;
		float Radius = FMath::Clamp(MaxSigma * 3.0f * EffectiveSplatScale * PtMult * PreviewMult, EffectiveMinRadius, MaxSplatRadius);

		FLinearColor OutCol = P.Color;
		if (bEditMode && CachedSelected.IsValidIndex(i) && CachedSelected[i])
		{
			OutCol = FLinearColor(
				FMath::Clamp(P.Color.R * 1.5f + 0.2f, 0.0f, 1.0f),
				FMath::Clamp(P.Color.G * 1.5f + 0.2f, 0.0f, 1.0f),
				FMath::Clamp(P.Color.B * 0.5f, 0.0f, 1.0f),
				P.Color.A);
		}

		FTransform Xform(IdentRot, FVector(P.Position) + Offset, FVector(TinyScale));
		Transforms.Add(Xform);

		const int32 off = i * 6;
		CustomData[off+0] = FMath::Clamp(OutCol.R, 0.0f, 1.0f);
		CustomData[off+1] = FMath::Clamp(OutCol.G, 0.0f, 1.0f);
		CustomData[off+2] = FMath::Clamp(OutCol.B, 0.0f, 1.0f);
		CustomData[off+3] = FMath::Clamp(OutCol.A, 0.0f, 1.0f);
		CustomData[off+4] = Radius;
		CustomData[off+5] = Rng.GetFraction();
	}

	SplatComponent->AddInstances(Transforms, false, /*bWorldSpace=*/true, false);
	SplatComponent->PerInstanceSMCustomData = MoveTemp(CustomData);
	SplatComponent->MarkRenderStateDirty();
	SplatComponent->UpdateBounds();

	CurrentLODScale = 1.0f;
	CurrentDitherFrac = 0.0f;
	if (DynMaterial)
	{
		DynMaterial->SetScalarParameterValue(TEXT("LODScale"), 1.0f);
		DynMaterial->SetScalarParameterValue(TEXT("DitherFrac"), 0.0f);
	}

	UE_LOG(LogOpenSplat4D, Log,
		TEXT("SplatActor: v13 %d instances added (Mat=%s, Mesh=%s, CustomData=%d floats, NumInstances=%d, Mode=SelectionEditor)"),
		PointCount,
		SplatComponent->GetMaterial(0) ? *SplatComponent->GetMaterial(0)->GetName() : TEXT("NULL"),
		SplatComponent->GetStaticMesh() ? *SplatComponent->GetStaticMesh()->GetName() : TEXT("NULL"),
		SplatComponent->PerInstanceSMCustomData.Num(),
		SplatComponent->GetNumInstances());
}
