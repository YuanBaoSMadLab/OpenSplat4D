// Copyright OpenSplat4D 2026 All Rights Reserved.
// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatFractureAssetEditor.h"
#include "Misc/EngineVersionComparison.h"
#include "GaussianSplatFractureAsset.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatAssetViewport.h"
#include "PropertyEditorModule.h"
#include "IDetailsView.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/AppStyle.h"
#include "UObject/Package.h"
#include "PlanarCut.h"
#include "Voronoi/Voronoi.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "GeometryCollection/GeometryCollectionUtility.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GaussianSplatBoneBinder.h"
#include "GaussianSplatProxyMeshBuilder.h"
#include "Materials/MaterialInterface.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"

#define LOCTEXT_NAMESPACE "GaussianSplatFractureAssetEditor"

namespace
{
	/**
	 * 在包围盒里随机撒 Voronoi 站点。与 Chaos 的
	 * FractureEngineFracturing.cpp::GenerateVoronoiSites 同一套逻辑
	 * （同一种子得到同一套站点，所以预览与解算结果一致）。
	 */
	void GenerateFractureSites(const FGaussianSplatFractureSettings& Settings, const FBox& Bounds, TArray<FVector>& OutSites)
	{
		const int32 MinSites = FMath::Max(1, FMath::Min(Settings.MinPieceCount, Settings.MaxPieceCount));
		const int32 MaxSites = FMath::Max(MinSites, Settings.MaxPieceCount);

		FRandomStream RandomStream(Settings.RandomSeed);
		const int32 SiteCount = RandomStream.RandRange(MinSites, MaxSites);
		const FVector Extent = Bounds.Max - Bounds.Min;

		OutSites.Reset(SiteCount);
		for (int32 SiteIndex = 0; SiteIndex < SiteCount; ++SiteIndex)
		{
			OutSites.Emplace(Bounds.Min + FVector(RandomStream.FRand(), RandomStream.FRand(), RandomStream.FRand()) * Extent);
		}
	}

	/** 顶点可能被缝隙 + 噪声推出的最大距离，与 UFractureCutterSettings::GetMaxVertexMovement 一致 */
	float GetMaxCutVertexMovement(const FGaussianSplatFractureSettings& Settings)
	{
		float MaxDisplacement = Settings.Grout;
		float ScaledAmplitude = Settings.NoiseAmplitude;
		for (int32 OctaveIndex = 0; OctaveIndex < Settings.NoiseOctaves; ++OctaveIndex, ScaledAmplitude *= Settings.NoisePersistence)
		{
			MaxDisplacement += FMath::Abs(ScaledAmplitude);
		}
		return MaxDisplacement;
	}

	/**
	 * 三角汤的面积加权平滑法线。Chaos 的 MakeMeshElement 允许传空法线数组，
	 * 但那样每个顶点法线都是零向量，切出来的断面质量差，所以这里自己算一份。
	 * 退化三角形跳过；累加后仍为零向量的顶点回退到 (0,0,1)。
	 */
	void ComputeTriangleSoupNormals(
		const TArray<FVector3f>& Vertices,
		const TArray<FIntVector>& Triangles,
		TArray<FVector3f>& OutNormals)
	{
		OutNormals.Reset();
		OutNormals.SetNumZeroed(Vertices.Num());

		const int32 VertexCount = Vertices.Num();
		for (const FIntVector& Triangle : Triangles)
		{
			if (Triangle[0] < 0 || Triangle[1] < 0 || Triangle[2] < 0 ||
				Triangle[0] >= VertexCount || Triangle[1] >= VertexCount || Triangle[2] >= VertexCount)
			{
				continue;
			}

			const FVector3f& V0 = Vertices[Triangle[0]];
			const FVector3f& V1 = Vertices[Triangle[1]];
			const FVector3f& V2 = Vertices[Triangle[2]];

			// 叉积长度正比于三角形面积，天然形成面积加权。
			const FVector3f FaceNormal = (V1 - V0) ^ (V2 - V0);
			OutNormals[Triangle[0]] += FaceNormal;
			OutNormals[Triangle[1]] += FaceNormal;
			OutNormals[Triangle[2]] += FaceNormal;
		}

		for (FVector3f& Normal : OutNormals)
		{
			const FVector3f Normalized = Normal.GetSafeNormal();
			Normal = Normalized.IsNearlyZero() ? FVector3f(0.0f, 0.0f, 1.0f) : Normalized;
		}
	}

	/**
	 * 把一个包围盒的 12 条棱追加成线段（每 2 个元素一条）。
	 * 用来在视口里画出估算网格（体素壳包围盒）。无效包围盒什么也不加。
	 */
	void AppendBoxSegments(const FBox& Box, TArray<FVector>& OutSegments)
	{
		if (!Box.IsValid)
		{
			return;
		}

		const FVector Corner[8] =
		{
			FVector(Box.Min.X, Box.Min.Y, Box.Min.Z),
			FVector(Box.Max.X, Box.Min.Y, Box.Min.Z),
			FVector(Box.Max.X, Box.Max.Y, Box.Min.Z),
			FVector(Box.Min.X, Box.Max.Y, Box.Min.Z),
			FVector(Box.Min.X, Box.Min.Y, Box.Max.Z),
			FVector(Box.Max.X, Box.Min.Y, Box.Max.Z),
			FVector(Box.Max.X, Box.Max.Y, Box.Max.Z),
			FVector(Box.Min.X, Box.Max.Y, Box.Max.Z)
		};

		static const int32 Edges[12][2] =
		{
			{ 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
			{ 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
			{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
		};

		for (int32 EdgeIndex = 0; EdgeIndex < 12; ++EdgeIndex)
		{
			OutSegments.Add(Corner[Edges[EdgeIndex][0]]);
			OutSegments.Add(Corner[Edges[EdgeIndex][1]]);
		}
	}
}

const FName FGaussianSplatFractureAssetEditor::ViewportTabId(TEXT("GaussianSplatFractureAssetEditor_Viewport"));
const FName FGaussianSplatFractureAssetEditor::DetailsTabId(TEXT("GaussianSplatFractureAssetEditor_Details"));

FGaussianSplatFractureAssetEditor::FGaussianSplatFractureAssetEditor()
{
}

FGaussianSplatFractureAssetEditor::~FGaussianSplatFractureAssetEditor()
{
}

void FGaussianSplatFractureAssetEditor::NotifyPostChange(const FPropertyChangedEvent& PropertyChangedEvent, FProperty* PropertyThatChanged)
{
	FNotifyHook::NotifyPostChange(PropertyChangedEvent, PropertyThatChanged);

	// 换源 splat 资产后预览必须立刻跟着换，否则视口里还是上一个点云。
	const FName ChangedProp = PropertyChangedEvent.GetPropertyName();
	if (ChangedProp == GET_MEMBER_NAME_CHECKED(UGaussianSplatFractureAsset, SourceSplatAsset))
	{
		RefreshPreview();
	}

	// 破裂设置里任何一个字段变化（含结构体子字段）都会改变切割面，需要重算预览。
	if (PropertyChangedEvent.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(UGaussianSplatFractureAsset, FractureSettings))
	{
		RefreshCuttingPreview();

		// 破碎预览的参数也在这个结构体里，改了要按新参数重开预览才能立刻看到。
		const FName LeafProperty = PropertyChangedEvent.GetPropertyName();
		if (LeafProperty == GET_MEMBER_NAME_CHECKED(FGaussianSplatFractureSettings, bPreviewGravity) ||
			LeafProperty == GET_MEMBER_NAME_CHECKED(FGaussianSplatFractureSettings, PreviewExplosionSpeed) ||
			LeafProperty == GET_MEMBER_NAME_CHECKED(FGaussianSplatFractureSettings, PreviewExplosionCenterOffset) ||
			LeafProperty == GET_MEMBER_NAME_CHECKED(FGaussianSplatFractureSettings, PreviewMode) ||
			LeafProperty == GET_MEMBER_NAME_CHECKED(FGaussianSplatFractureSettings, PreviewChaosExplosionStrength))
		{
			SyncFracturePreview();
		}
	}
}

void FGaussianSplatFractureAssetEditor::RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	WorkspaceMenuCategory = InTabManager->AddLocalWorkspaceMenuCategory(LOCTEXT("WorkspaceMenu_FractureEditor", "破裂资产编辑器"));

	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	InTabManager->RegisterTabSpawner(ViewportTabId, FOnSpawnTab::CreateSP(this, &FGaussianSplatFractureAssetEditor::SpawnTab_Viewport))
		.SetDisplayName(LOCTEXT("ViewportTab", "视口"))
		.SetTooltipText(LOCTEXT("ViewportTabTooltip", "源 OpenSplat 资产的 3D 预览"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef());

	InTabManager->RegisterTabSpawner(DetailsTabId, FOnSpawnTab::CreateSP(this, &FGaussianSplatFractureAssetEditor::SpawnTab_Details))
		.SetDisplayName(LOCTEXT("DetailsTab", "详情"))
		.SetTooltipText(LOCTEXT("DetailsTabTooltip", "破裂资产的属性面板"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef());
}

void FGaussianSplatFractureAssetEditor::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);

	InTabManager->UnregisterTabSpawner(ViewportTabId);
	InTabManager->UnregisterTabSpawner(DetailsTabId);
}

FName FGaussianSplatFractureAssetEditor::GetToolkitFName() const
{
	return FName("GaussianSplatFractureAssetEditor");
}

FText FGaussianSplatFractureAssetEditor::GetBaseToolkitName() const
{
	return LOCTEXT("BaseToolkitName", "破裂资产编辑器");
}

FText FGaussianSplatFractureAssetEditor::GetToolkitName() const
{
	if (FractureAsset.IsValid())
	{
		return FText::FromString(FString::Printf(TEXT("破裂资产: %s"), *FractureAsset->GetName()));
	}
	return LOCTEXT("BaseToolkitName", "破裂资产编辑器");
}

FString FGaussianSplatFractureAssetEditor::GetWorldCentricTabPrefix() const
{
	return TEXT("GaussianSplatFractureAssetEditor");
}

FLinearColor FGaussianSplatFractureAssetEditor::GetWorldCentricTabColorScale() const
{
	return FLinearColor(0.8f, 0.35f, 0.25f, 1.0f);
}

FString FGaussianSplatFractureAssetEditor::GetDocumentationLink() const
{
	return TEXT("");
}

void FGaussianSplatFractureAssetEditor::InitGaussianSplatFractureAssetEditor(
	const EToolkitMode::Type Mode,
	const TSharedPtr<IToolkitHost>& InitToolkitHost,
	UGaussianSplatFractureAsset* InAsset)
{
	FractureAsset = TStrongObjectPtr<UGaussianSplatFractureAsset>(InAsset);

	// Details 面板：直接展示破裂资产自身的属性。源资产引用、几何集合与绑定
	// 统计都是资产上的 UPROPERTY，改完即时生效，不需要额外编辑会话对象。
	FPropertyEditorModule& PropertyEditorModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	DetailsViewArgs.bHideSelectionTip = true;
	DetailsViewArgs.bShowPropertyMatrixButton = false;
	DetailsViewArgs.NotifyHook = this;
	DetailsViewArgs.DefaultsOnlyVisibility = EEditDefaultsOnlyNodeVisibility::Hide;

	DetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);
	if (DetailsView.IsValid() && FractureAsset.IsValid())
	{
		DetailsView->SetObject(FractureAsset.Get());
	}

	// 布局：左视口 + 右详情
	const TSharedRef<FTabManager::FLayout> StandaloneDefaultLayout = FTabManager::NewLayout("Standalone_GaussianSplatFractureAssetEditor_Layout")
		->AddArea
		(
			FTabManager::NewPrimaryArea()
			->SetOrientation(Orient_Horizontal)
			->Split
			(
				FTabManager::NewStack()
				->SetSizeCoefficient(0.7f)
				->AddTab(ViewportTabId, ETabState::OpenedTab)
			)
			->Split
			(
				FTabManager::NewSplitter()
				->SetSizeCoefficient(0.3f)
				->SetOrientation(Orient_Vertical)
				->Split
				(
					FTabManager::NewStack()
					->AddTab(DetailsTabId, ETabState::OpenedTab)
				)
			)
		);

	FAssetEditorToolkit::InitAssetEditor(
		Mode,
		InitToolkitHost,
		TEXT("GaussianSplatFractureAssetEditorApp"),
		StandaloneDefaultLayout,
		/*bCreateDefaultStandaloneMenu=*/true,
		/*bCreateDefaultToolbar=*/true,
		InAsset);
}

void FGaussianSplatFractureAssetEditor::RefreshPreview()
{
	if (!ViewportWidget.IsValid())
	{
		return;
	}

	// 预览的是源 splat 资产（破裂资产本身不持有任何点云）。
	UGaussianSplatAsset* SourceAsset = FractureAsset.IsValid() ? FractureAsset->LoadSourceSplatAsset() : nullptr;
	ViewportWidget->SetSplatAsset(SourceAsset);

	// 组件改渲染破裂资产（源点云 + 骨头绑定 + 断面 + 网格外剔除标记），
	// 而不是裸源资产 —— 破碎预览要靠这份骨架数据才能位移碎片。
	ViewportWidget->SetFractureAsset(FractureAsset.Get());

	// 源资产换了包围盒也就变了，切割面预览必须一起重算。
	RefreshCuttingPreview();

	// 视口 actor 刚被整体重建，正在跑的破碎预览要按新的绑定重开一次。
	SyncFracturePreview();
}

void FGaussianSplatFractureAssetEditor::RefreshCuttingPreview()
{
	if (!ViewportWidget.IsValid() || !FractureAsset.IsValid())
	{
		return;
	}

	// 估算网格（体素壳包围盒）与「显示切割面预览」无关，始终画出来：它决定了
	// 哪些 splat 参与破裂，用户改「估算网格位置偏移」并重新生成体素壳后，就能
	// 直接看到这个框挪到了哪里。
	TArray<FVector> Segments;
	AppendBoxSegments(FractureAsset->VoxelShellBounds, Segments);

	const auto PushPreview = [this, &Segments](const TArray<FVector>& InSites)
	{
		if (Segments.Num() > 0)
		{
			ViewportWidget->SetCuttingPreview(Segments, InSites);
		}
		else
		{
			ViewportWidget->ClearCuttingPreview();
		}
	};

	const FGaussianSplatFractureSettings& Settings = FractureAsset->FractureSettings;

	UGaussianSplatAsset* SourceAsset = FractureAsset->LoadSourceSplatAsset();
	if (!Settings.bShowCuttingPreview || !SourceAsset || SourceAsset->GetSplatCount() <= 0)
	{
		PushPreview(TArray<FVector>());
		return;
	}

	// 切割面在源资产的局部空间里算，预览 actor 摆在世界原点，两者一致。
	const FBox Bounds = SourceAsset->GetBounds();
	if (!Bounds.IsValid)
	{
		PushPreview(TArray<FVector>());
		return;
	}

	TArray<FVector> Sites;
	GenerateFractureSites(Settings, Bounds, Sites);
	if (Sites.Num() == 0)
	{
		PushPreview(TArray<FVector>());
		return;
	}

	// 与 Chaos UFractureToolVoronoiCutterBase::GetVoronoiBounds 同构
	FBox VoronoiBounds = Bounds + FBox(Sites);
	VoronoiBounds = VoronoiBounds.ExpandBy(GetMaxCutVertexMovement(Settings) + KINDA_SMALL_NUMBER);

	FVoronoiDiagram Voronoi(Sites, VoronoiBounds, .1f);
	FPlanarCells Cells(Sites, Voronoi);

	if (Settings.NoiseAmplitude > 0.0f)
	{
		FNoiseSettings NoiseSettings;
		NoiseSettings.PointSpacing = Settings.NoisePointSpacing;
		NoiseSettings.Amplitude = Settings.NoiseAmplitude;
		NoiseSettings.Frequency = Settings.NoiseFrequency;
		NoiseSettings.Octaves = Settings.NoiseOctaves;
		NoiseSettings.Lacunarity = Settings.NoiseLacunarity;
		NoiseSettings.Persistence = Settings.NoisePersistence;
		Cells.SetNoise(NoiseSettings);
	}

	UE::Geometry::FDynamicMesh3 CuttingMesh;
	CreateCuttingSurfacePreview(
		Cells,
		VoronoiBounds,
		Settings.Grout,
		Settings.RandomSeed,
		CuttingMesh,
		[](int32) { return true; });

	// 切割面以边表示，转成扁平线段数组（每 2 个元素一条线段）
	Segments.Reserve(Segments.Num() + CuttingMesh.EdgeCount() * 2);
	for (const int32 EdgeID : CuttingMesh.EdgeIndicesItr())
	{
		const UE::Geometry::FIndex2i EdgeVertices = CuttingMesh.GetEdgeV(EdgeID);
		if (EdgeVertices.A < 0 || EdgeVertices.B < 0)
		{
			continue;
		}
		Segments.Add((FVector)CuttingMesh.GetVertex(EdgeVertices.A));
		Segments.Add((FVector)CuttingMesh.GetVertex(EdgeVertices.B));
	}

	ViewportWidget->SetCuttingPreview(Segments, Sites);
}

FReply FGaussianSplatFractureAssetEditor::OnSolveClicked()
{
	SolveFracture();
	return FReply::Handled();
}

FReply FGaussianSplatFractureAssetEditor::OnRegenerateShellClicked()
{
	RegenerateVoxelShell();
	return FReply::Handled();
}

FReply FGaussianSplatFractureAssetEditor::OnFracturePreviewClicked()
{
	if (bFracturePreviewActive)
	{
		StopFracturePreview();
		SetSolveStatus(TEXT("破碎预览已停止，碎片回到初始位置。"), false);
	}
	else
	{
		StartFracturePreview();
	}

	return FReply::Handled();
}

void FGaussianSplatFractureAssetEditor::StartFracturePreview()
{
	if (!ViewportWidget.IsValid() || !FractureAsset.IsValid())
	{
		SetSolveStatus(TEXT("没有可预览的破裂资产。"), true);
		return;
	}

	UGaussianSplatFractureAsset* Fracture = FractureAsset.Get();

	// The preview displaces splats through the bone binding, so it can only run on
	// a solved fracture (the binding is what produces the per-splat bone indices).
	if (!Fracture->HasBoneBinding())
	{
		SetSolveStatus(TEXT("还没有破裂骨头绑定，无法做破碎预览。先点「解算」生成破裂块。"), true);
		return;
	}

	TArray<FMatrix44f> RestBoneMatrices;
	if (!Fracture->GetRestBoneMatrices(RestBoneMatrices) || RestBoneMatrices.Num() < 2)
	{
		SetSolveStatus(TEXT("破裂骨头数不足（至少需要一块碎片），无法做破碎预览。请重新解算。"), true);
		return;
	}

	// Explosion center: the body box the binding recorded, i.e. the voxel shell
	// at bind time. Zero offset = explode at the object center (the default).
	FGaussianSplatBoneBindingHeader Header;
	FVector BodyCenter = FVector::ZeroVector;
	if (Fracture->GetBoneBindingHeader(Header))
	{
		BodyCenter = FVector((Header.BoundsMin + Header.BoundsMax) * 0.5f);
	}

	const FGaussianSplatFractureSettings& Settings = Fracture->FractureSettings;

	// The component must render the fracture asset (binding + caps), not the bare
	// source cloud, or there would be no bones to displace.
	ViewportWidget->SetFractureAsset(Fracture);

	if (Settings.PreviewMode == EGaussianSplatFracturePreviewMode::Chaos)
	{
		// 真实 Chaos 预览要在预览场景里现搭几何集合，没有几何集合就只能走轻量。
		if (!Fracture->HasGeometryCollection())
		{
			SetSolveStatus(TEXT("资产里没有几何集合（破裂数据不完整），无法做真实 Chaos 预览。请重新解算，或把「破碎预览：模式」改回轻量。"), true);
			return;
		}

		ViewportWidget->StartChaosFracturePreview(
			RestBoneMatrices,
			BodyCenter,
			Settings.bPreviewGravity,
			Settings.PreviewChaosExplosionStrength,
			Settings.PreviewExplosionCenterOffset);

		bFracturePreviewActive = ViewportWidget->IsFracturePreviewActive();

		SetSolveStatus(bFracturePreviewActive
			? FString::Printf(TEXT("真实 Chaos 破碎预览中：%d 块碎片做刚体模拟（%s，强度 %.0f cm/s）。再点一次按钮停止。"),
				RestBoneMatrices.Num() - 1,
				Settings.bPreviewGravity ? TEXT("有重力") : TEXT("无重力"),
				Settings.PreviewChaosExplosionStrength)
			: TEXT("真实 Chaos 破碎预览未能启动（几何集合或骨头绑定不可用）。"), !bFracturePreviewActive);
		return;
	}

	ViewportWidget->StartFracturePreview(
		RestBoneMatrices,
		BodyCenter,
		Settings.bPreviewGravity,
		Settings.PreviewExplosionSpeed,
		Settings.PreviewExplosionCenterOffset);

	bFracturePreviewActive = ViewportWidget->IsFracturePreviewActive();

	SetSolveStatus(bFracturePreviewActive
		? FString::Printf(TEXT("破碎预览中：%d 块碎片从物体中心向外飞散（%s，速度 %.0f cm/s）。再点一次按钮停止。"),
			RestBoneMatrices.Num() - 1,
			Settings.bPreviewGravity ? TEXT("有重力") : TEXT("无重力"),
			Settings.PreviewExplosionSpeed)
		: TEXT("破碎预览未能启动（骨头数不足）。"), !bFracturePreviewActive);
}

void FGaussianSplatFractureAssetEditor::StopFracturePreview()
{
	bFracturePreviewActive = false;

	if (ViewportWidget.IsValid())
	{
		ViewportWidget->StopFracturePreview();
	}
}

void FGaussianSplatFractureAssetEditor::SyncFracturePreview()
{
	if (!bFracturePreviewActive)
	{
		return;
	}

	// A re-solve / re-bind produces a new bone set, so the running preview has to
	// be re-armed from scratch (old indices would point at the wrong pieces).
	StopFracturePreview();
	StartFracturePreview();
}

void FGaussianSplatFractureAssetEditor::RegenerateVoxelShell()
{
	if (!FractureAsset.IsValid())
	{
		SetSolveStatus(TEXT("没有可用的破裂资产。"), true);
		return;
	}

	UGaussianSplatFractureAsset* Fracture = FractureAsset.Get();
	UGaussianSplatAsset* SourceAsset = Fracture->LoadSourceSplatAsset();
	if (!SourceAsset || SourceAsset->GetSplatCount() <= 0)
	{
		SetSolveStatus(TEXT("未指定源 Splat 资产（或该资产内没有 splat），无法生成体素壳。"), true);
		return;
	}

	// 体素尺寸仍走自动推定（与 Content Browser 右键「生成碰撞体素网格」完全一致），
	// 这里只把用户调好的网格位置传下去。想换体素尺寸依旧可以走右键那条路。
	FGaussianSplatProxyMeshParams Params;
	Params.GridOriginOffset = Fracture->GridOriginOffset;

	FString Error;
	FGaussianSplatProxyMeshResult Result;
	if (!FGaussianSplatProxyMeshBuilder::BuildVoxelShell(SourceAsset, Fracture, Params, Error, &Result))
	{
		SetSolveStatus(FString::Printf(TEXT("重新生成体素壳失败：%s"),
			Error.IsEmpty() ? TEXT("未知错误，详见输出日志。") : *Error), true);
		return;
	}

	if (DetailsView.IsValid())
	{
		DetailsView->ForceRefresh();
	}

	// 壳换了，视口里的估算网格框（就是壳的包围盒）必须跟着挪；切割面预览也顺带重画。
	RefreshCuttingPreview();

	SetSolveStatus(FString::Printf(TEXT("体素壳已重新生成（网格位置 %s）。\n%s"), *Fracture->GridOriginOffset.ToCompactString(), *Result.Summary), false);
}

void FGaussianSplatFractureAssetEditor::SetSolveStatus(const FString& Message, bool bIsError)
{
	if (bIsError)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GaussianSplatFracture] %s"), *Message);
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("[GaussianSplatFracture] %s"), *Message);
	}

	if (SolveStatusText.IsValid())
	{
		SolveStatusText->SetText(FText::FromString(Message));
		SolveStatusText->SetColorAndOpacity(FSlateColor(
			bIsError ? FLinearColor(0.95f, 0.35f, 0.30f) : FLinearColor(0.35f, 0.85f, 0.45f)));
	}
}

void FGaussianSplatFractureAssetEditor::SolveFracture()
{
	if (!FractureAsset.IsValid())
	{
		SetSolveStatus(TEXT("没有可解算的破裂资产。"), true);
		return;
	}

	UGaussianSplatFractureAsset* Fracture = FractureAsset.Get();

	// ----------------------------------------------------------------
	// 1) 输入：源 splat 资产 + 碰撞体素壳
	// ----------------------------------------------------------------
	UGaussianSplatAsset* SourceAsset = Fracture->LoadSourceSplatAsset();
	if (!SourceAsset)
	{
		SetSolveStatus(TEXT("未指定源 Splat 资产，无法解算。"), true);
		return;
	}

	TArray<FVector3f> ShellVertices;
	TArray<int32> ShellIndices;
	if (!Fracture->GetVoxelShell(ShellVertices, ShellIndices))
	{
		SetSolveStatus(TEXT("该破裂资产还没有碰撞体素网格。点上方「重新生成体素壳」，或先在 Content Browser 右键源 Splat 资产 →「生成碰撞体素网格」。"), true);
		return;
	}

	if (ShellVertices.Num() < 3 || ShellIndices.Num() < 3 || (ShellIndices.Num() % 3) != 0)
	{
		SetSolveStatus(FString::Printf(TEXT("体素壳数据不合法（顶点 %d，索引 %d），请重新生成碰撞体素网格。"),
			ShellVertices.Num(), ShellIndices.Num()), true);
		return;
	}

	// ----------------------------------------------------------------
	// 2) 体素壳三角汤 → FGeometryCollection
	//
	// 用 Chaos 的公共工具（路线见 GeometryCollectionUtility.h）。注意
	// MakeMeshElement 的 NumberOfMaterials 默认是 2，本项目只有一种材质，
	// 必须显式传 1，否则 MaterialID = FaceIndex / (FaceCount/2) 会错乱
	// （FaceCount < 2 时还会除零）。
	// ----------------------------------------------------------------
	TArray<FIntVector> Triangles;
	Triangles.Reserve(ShellIndices.Num() / 3);
	for (int32 Index = 0; Index + 2 < ShellIndices.Num(); Index += 3)
	{
		Triangles.Emplace(ShellIndices[Index], ShellIndices[Index + 1], ShellIndices[Index + 2]);
	}

	TArray<FVector3f> Normals;
	ComputeTriangleSoupNormals(ShellVertices, Triangles, Normals);

	TSharedPtr<FGeometryCollection> SourceCollection = GeometryCollection::MakeMeshElement(
		ShellVertices,
		Normals,
		Triangles,
		TArray<FVector2D>(),
		FTransform::Identity,
		FTransform::Identity,
		/*NumberOfMaterials=*/1);
	if (!SourceCollection.IsValid())
	{
		SetSolveStatus(TEXT("无法把体素壳转成 Chaos 几何集合。"), true);
		return;
	}

	// ----------------------------------------------------------------
	// 3) 站点与 Voronoi 边界：与 RefreshCuttingPreview 用完全同一套构造，
	//    保证「预览看到的切割面 == 解算实际用的切割面」。
	// ----------------------------------------------------------------
	const FGaussianSplatFractureSettings& Settings = Fracture->FractureSettings;

	FBox ShellBounds(ForceInit);
	for (const FVector3f& Vertex : ShellVertices)
	{
		ShellBounds += FVector(Vertex);
	}
	if (!ShellBounds.IsValid)
	{
		SetSolveStatus(TEXT("体素壳包围盒无效，无法解算。"), true);
		return;
	}

	TArray<FVector> Sites;
	GenerateFractureSites(Settings, ShellBounds, Sites);
	if (Sites.Num() == 0)
	{
		SetSolveStatus(TEXT("破裂站点数为 0，请检查「最少块数 / 最多块数」设置。"), true);
		return;
	}

	FBox VoronoiBounds = ShellBounds + FBox(Sites);
	VoronoiBounds = VoronoiBounds.ExpandBy(GetMaxCutVertexMovement(Settings) + KINDA_SMALL_NUMBER);

	FVoronoiDiagram Voronoi(Sites, VoronoiBounds, .1f);
	FPlanarCells Cells(Sites, Voronoi);

	if (Settings.NoiseAmplitude > 0.0f)
	{
		FNoiseSettings NoiseSettings;
		NoiseSettings.PointSpacing = Settings.NoisePointSpacing;
		NoiseSettings.Amplitude = Settings.NoiseAmplitude;
		NoiseSettings.Frequency = Settings.NoiseFrequency;
		NoiseSettings.Octaves = Settings.NoiseOctaves;
		NoiseSettings.Lacunarity = Settings.NoiseLacunarity;
		NoiseSettings.Persistence = Settings.NoisePersistence;
		Cells.SetNoise(NoiseSettings);
	}

	// ----------------------------------------------------------------
	// 4) 切割。体素壳顶点与站点都在资产局部空间（原点重合），
	//    所以 CellsOrigin 用零向量，不需要平移站点。
	// ----------------------------------------------------------------
	TArray<int32> TransformIndices;
	TransformIndices.Add(0);

	const int32 NewGeometryStartIndex = CutMultipleWithPlanarCells(
		Cells,
		*SourceCollection,
		TransformIndices,
		Settings.Grout,
		Settings.CollisionSampleSpacing,
		Settings.RandomSeed,
		TOptional<FTransform>(),
		/*bIncludeOutsideCellInOutput=*/true,
		/*bSetDefaultInternalMaterialsFromCollection=*/true,
		/*Progress=*/nullptr,
		FVector::ZeroVector,
		// UE 5.7 took a plain bool here; UE 5.8 replaced it with FIslandSplitSettings.
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		/*bSplitIslands=*/true);
#else
		FIslandSplitSettings());
#endif

	if (NewGeometryStartIndex == INDEX_NONE)
	{
		SetSolveStatus(TEXT("切割没有产出任何碎片。可能站点全部落在体素壳之外，或体素壳不是封闭的。"), true);
		return;
	}

	// 碎片包围盒是绑定流程的前置条件（每块都要有有效几何包围盒）。
	FGeometryCollection::UpdateBoundingBox(*SourceCollection);

	// ----------------------------------------------------------------
	// 5) 把结果内嵌进破裂资产（几何集合是破裂资产的子对象，
	//    源 splat 点云只被引用、不复制）。
	// ----------------------------------------------------------------
	UGeometryCollection* CollectionObject = Cast<UGeometryCollection>(Fracture->GeometryCollection);
	if (!CollectionObject || CollectionObject->GetOuter() != Fracture)
	{
		CollectionObject = FindObject<UGeometryCollection>(Fracture, TEXT("FractureGeometryCollection"));
		if (!CollectionObject)
		{
			CollectionObject = NewObject<UGeometryCollection>(Fracture, TEXT("FractureGeometryCollection"), RF_Transactional);
		}
	}

	Fracture->Modify();
	CollectionObject->Modify();

	// ResetFrom 内部会 CopyTo，绝不能先 SetGeometryCollection 再 ResetFrom 同一个对象
	// （那会变成自己拷自己）。材质留空 = 单个空材质槽，正是本管线要的。
	const TArray<UMaterialInterface*> NoMaterials;
	CollectionObject->ResetFrom(*SourceCollection, NoMaterials, /*bHasInternalMaterials=*/false);

	if (FGeometryCollection* SolvedCollection = CollectionObject->GetGeometryCollection().Get())
	{
		GeometryCollection::GenerateTemporaryGuids(SolvedCollection, 0, true);
	}
	CollectionObject->InvalidateCollection();
	CollectionObject->RebuildRenderData();
	CollectionObject->CreateSimulationDataIfNeeded();

	// 集合是「原地」改写的：解算复用同一个 UGeometryCollection 对象，ResetFrom 会
	// 直接换掉里面的骨头数（体素块越多、块数变化越大越明显）。关卡里和破碎预览里
	// 已经引用它的几何集合组件，手上仍是旧的动态集合与按旧骨头数分配的渲染缓冲，
	// 引擎会在 GeometryCollectionSceneProxy 的 check(NumTransforms == Transforms.Num())
	// 处直接崩掉。必须让这些组件重跑一遍 SetRestCollection（重置动态集合 + 重建物理），
	// 再标脏让场景代理按新骨头数重建 —— 引擎自己在 PostEditUndo 里就是这么做的。
	CollectionObject->PropagateTransformUpdateToComponents();
	CollectionObject->PropagateMarkDirtyToComponents();

	Fracture->GeometryCollection = CollectionObject;

	// ----------------------------------------------------------------
	// 6) 绑定回写：每个 splat 归属到包含它的破裂块，并把断面 splat
	//    写进破裂资产（源资产保持不变）。
	// ----------------------------------------------------------------
	const FGaussianSplatBoneBindResult BindResult =
		FGaussianSplatBoneBinder::BindAssetToCollection(SourceAsset, Fracture, CollectionObject);

	Fracture->MarkPackageDirty();
	if (DetailsView.IsValid())
	{
		// VisibleAnywhere 的骨头数 / 断面数等统计全部变了。
		DetailsView->ForceRefresh();
	}

	if (!BindResult.bSuccess)
	{
		SetSolveStatus(FString::Printf(TEXT("切割完成，但 splat 绑定失败：%s"), *BindResult.Error), true);
		return;
	}

	// 新的骨头绑定已经就位，正在跑的破碎预览必须按新索引重开（旧索引指向的块已经
	// 不存在了）。解算前没开预览的话这里什么也不做。
	SyncFracturePreview();

	SetSolveStatus(FString::Printf(TEXT("解算完成。%s"), *BindResult.Summary), false);
}

TSharedRef<SDockTab> FGaussianSplatFractureAssetEditor::SpawnTab_Viewport(const FSpawnTabArgs& Args)
{
	check(Args.GetTabId() == ViewportTabId);

	TSharedRef<SGaussianSplatAssetViewport> ViewportWidgetRef = SNew(SGaussianSplatAssetViewport, nullptr);
	ViewportWidget = ViewportWidgetRef;

	RefreshPreview();

	return SNew(SDockTab)
		.Label(LOCTEXT("ViewportTitle", "视口"))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			[
				ViewportWidgetRef
			]
		];
}

TSharedRef<SDockTab> FGaussianSplatFractureAssetEditor::SpawnTab_Details(const FSpawnTabArgs& Args)
{
	check(Args.GetTabId() == DetailsTabId);

	// 解算入口：放在详情面板顶部自建的头栏里。不用工具栏扩展是因为
	// UToolMenus::ExtendMenu 在多开同类编辑器时会重注册（清掉/重复条目），
	// 而 FExtender 路线依赖 ToolkitCommands 的构造时机，两条都有不确定性。
	// 自建头栏零外部机制，还天然承载解算状态与错误文本。
	SolveStatusText = SNew(STextBlock)
		.Text(LOCTEXT("SolveStatusIdle", "尚未解算。先设置好上面的破裂参数，再点「解算」。"))
		.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
		.AutoWrapText(true);

	TSharedRef<SWidget> SolveButton = SNew(SButton)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.ToolTipText(LOCTEXT("SolveButtonTooltip", "按当前「破裂设置」切割体素壳，生成破裂几何集合，并把 splat 绑定到各破裂块"))
		.OnClicked(FOnClicked::CreateSP(this, &FGaussianSplatFractureAssetEditor::OnSolveClicked))
		[
			SNew(STextBlock)
			.Text(LOCTEXT("SolveButton", "解 算"))
		];

	// 体素壳的重生成入口。放在这里是因为「估算网格位置偏移」只有在重生成壳之后
	// 才生效，而解算又依赖壳，三者是一条链，挨着放用户不会漏点。
	TSharedRef<SWidget> RegenerateShellButton = SNew(SButton)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.ToolTipText(LOCTEXT("RegenerateShellButtonTooltip", "按「估算网格位置偏移」重新生成碰撞体素壳：落在网格外的 splat 会从壳的形状里去掉，解算时也不再参与破裂（源 Splat 资产不会被修改）"))
		.OnClicked(FOnClicked::CreateSP(this, &FGaussianSplatFractureAssetEditor::OnRegenerateShellClicked))
		[
			SNew(STextBlock)
			.Text(LOCTEXT("RegenerateShellButton", "重新生成体素壳"))
		];

	// 破碎预览入口。和解算挨着放：预览靠的是解算产出的骨头绑定，两者是一条链。
	// 文案随开/关切换，所以用 Text_Lambda 而不是固定文本。
	TSharedRef<SWidget> FracturePreviewButton = SNew(SButton)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.ToolTipText(LOCTEXT("FracturePreviewButtonTooltip", "轻量破碎预览：让已解算的破裂块从物体中心向外飞散，可叠加重力（开关与速度见「预览」分类）。不跑 Chaos，不修改任何数据"))
		.OnClicked(FOnClicked::CreateSP(this, &FGaussianSplatFractureAssetEditor::OnFracturePreviewClicked))
		[
			SNew(STextBlock)
			.Text_Lambda([this]()
			{
				return bFracturePreviewActive
					? LOCTEXT("StopFracturePreviewButton", "停止预览")
					: LOCTEXT("FracturePreviewButton", "破碎预览");
			})
		];

	TSharedRef<SWidget> DetailsContent = DetailsView.IsValid()
		? DetailsView.ToSharedRef()
		: static_cast<TSharedRef<SWidget>>(SNew(STextBlock).Text(LOCTEXT("NoDetails", "无可用详情")));

	return SNew(SDockTab)
		.Label(LOCTEXT("DetailsTitle", "详情"))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(4.0f)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						SolveButton
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						RegenerateShellButton
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						FracturePreviewButton
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SolveStatusText.ToSharedRef()
					]
				]

				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					DetailsContent
				]
			]
		];
}

#undef LOCTEXT_NAMESPACE
