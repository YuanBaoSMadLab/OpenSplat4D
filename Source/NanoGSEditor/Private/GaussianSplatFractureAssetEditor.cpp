// Copyright Epic Games, Inc. All Rights Reserved.
// Copyright OpenSplat4D 2026 All Rights Reserved.

#include "GaussianSplatFractureAssetEditor.h"
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

	// 源资产换了包围盒也就变了，切割面预览必须一起重算。
	RefreshCuttingPreview();
}

void FGaussianSplatFractureAssetEditor::RefreshCuttingPreview()
{
	if (!ViewportWidget.IsValid() || !FractureAsset.IsValid())
	{
		return;
	}

	const FGaussianSplatFractureSettings& Settings = FractureAsset->FractureSettings;

	UGaussianSplatAsset* SourceAsset = FractureAsset->LoadSourceSplatAsset();
	if (!Settings.bShowCuttingPreview || !SourceAsset || SourceAsset->GetSplatCount() <= 0)
	{
		ViewportWidget->ClearCuttingPreview();
		return;
	}

	// 切割面在源资产的局部空间里算，预览 actor 摆在世界原点，两者一致。
	const FBox Bounds = SourceAsset->GetBounds();
	if (!Bounds.IsValid)
	{
		ViewportWidget->ClearCuttingPreview();
		return;
	}

	TArray<FVector> Sites;
	GenerateFractureSites(Settings, Bounds, Sites);
	if (Sites.Num() == 0)
	{
		ViewportWidget->ClearCuttingPreview();
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
	TArray<FVector> Segments;
	Segments.Reserve(CuttingMesh.EdgeCount() * 2);
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
			.BorderImage(FAppStyle::GetBrush("Toolbox.GroupBorder"))
			[
				ViewportWidgetRef
			]
		];
}

TSharedRef<SDockTab> FGaussianSplatFractureAssetEditor::SpawnTab_Details(const FSpawnTabArgs& Args)
{
	check(Args.GetTabId() == DetailsTabId);

	return SNew(SDockTab)
		.Label(LOCTEXT("DetailsTitle", "详情"))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Toolbox.GroupBorder"))
			[
				DetailsView.IsValid() ? DetailsView.ToSharedRef() : static_cast<TSharedRef<SWidget>>(SNew(STextBlock).Text(LOCTEXT("NoDetails", "无可用详情")))
			]
		];
}

#undef LOCTEXT_NAMESPACE
