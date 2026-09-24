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
#include "GeometryCollection/GeometryCollectionUtility.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "GaussianSplatBoneBinder.h"
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

FReply FGaussianSplatFractureAssetEditor::OnSolveClicked()
{
	SolveFracture();
	return FReply::Handled();
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
		SetSolveStatus(TEXT("该破裂资产还没有碰撞体素网格。请先在 Content Browser 右键源 Splat 资产 →「生成碰撞体素网格」。"), true);
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
		FIslandSplitSettings());

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
			.BorderImage(FAppStyle::GetBrush("Toolbox.GroupBorder"))
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

	TSharedRef<SWidget> DetailsContent = DetailsView.IsValid()
		? DetailsView.ToSharedRef()
		: static_cast<TSharedRef<SWidget>>(SNew(STextBlock).Text(LOCTEXT("NoDetails", "无可用详情")));

	return SNew(SDockTab)
		.Label(LOCTEXT("DetailsTitle", "详情"))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Toolbox.GroupBorder"))
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
