#include "OpenSplat4DPointCloudEditor.h"
#include "OpenSplat4DLocalization.h"
#include "AssetEditorModeManager.h"
#include "SOpenSplat4DPointCloudEditorViewport.h"
#include "OpenSplat4DPointCloud.h"
#include "EditorViewportTabContent.h"
#include "AdvancedPreviewSceneModule.h"
#include "SOpenSplat4DPointCloudFeatureEditor.h"

#define LOCTEXT_NAMESPACE "OpenSplat4D"

// Set when an OpenSplat4D point cloud editor opens; consumed by the
// OpenSplat4D.Reload console command so a blank (0-point) asset can be
// repopulated from disk without re-importing.
TWeakPtr<FOpenSplat4DPointCloudEditor> GActiveOpenSplatEditor;

const FName OpenSplat4DPointCloudEditorAppIdentifier = FName(TEXT("OpenSplat4DPointCloudEditorApp"));
const FName FOpenSplat4DPointCloudEditor::ViewportTabId(TEXT("OpenSplat4DPointCloudEditor_Viewport"));
const FName FOpenSplat4DPointCloudEditor::PropertiesTabId(TEXT("OpenSplat4DPointCloudEditor_Properties"));
const FName FOpenSplat4DPointCloudEditor::PreviewSceneSettingsTabId(TEXT("OpenSplat4DPointCloudEditor_PreviewScene"));
const FName FOpenSplat4DPointCloudEditor::FeatureEditorTabId(TEXT("OpenSplat4DPointCloudEditor_Features"));

FOpenSplat4DPointCloudEditor::~FOpenSplat4DPointCloudEditor()
{
	if (GEditor)
	{
		GEditor->UnregisterForUndo(this);
	}
}

UOpenSplat4DPointCloud* FOpenSplat4DPointCloudEditor::GetPointCloud()
{
	return PointCloud;
}

void FOpenSplat4DPointCloudEditor::InitEditor(const EToolkitMode::Type Mode, const TSharedPtr<class IToolkitHost>& InitToolkitHost, UOpenSplat4DPointCloud* ObjectToEdit)
{
	PointCloud = ObjectToEdit;
	PointCloud->SetFlags(RF_Transactional);

	// Remember this editor so the OpenSplat4D.Reload console command can target it.
	GActiveOpenSplatEditor = SharedThis(this);

	// The point cloud asset is a pure preview object. If it was opened with no
	// points (e.g. an asset saved before the splat data was serialised, or a
	// freshly created one), but we still know its source file, reload it so the
	// preview is never silently empty. The on-disk file remains the source of truth.
	if (PointCloud && PointCloud->GetPointCount() == 0 && !PointCloud->SourceFilePath.IsEmpty()
		&& FPaths::FileExists(PointCloud->SourceFilePath))
	{
		const FString Ext = FPaths::GetExtension(PointCloud->SourceFilePath).ToLower();
		if (Ext == TEXT("ply") || Ext == TEXT("4dgs"))
		{
			PointCloud->LoadFromFile(PointCloud->SourceFilePath);
			UE_LOG(LogOpenSplat4D, Log,
				TEXT("OpenSplat4D: 资产打开时点数为 0，已从源文件自动重载：%s -> 点数=%d"),
				*PointCloud->SourceFilePath, PointCloud->GetPointCount());
		}
	}

	const TSharedRef<FTabManager::FLayout> StandaloneDefaultLayout = FTabManager::NewLayout("Standalone_OpenSplat4DPointCloudEditor_Layout_v1")
		->AddArea
		(
			FTabManager::NewPrimaryArea()->SetOrientation(Orient_Vertical)
			->Split
			(
				FTabManager::NewSplitter()->SetOrientation(Orient_Horizontal)
				->Split
				(
					FTabManager::NewSplitter()->SetOrientation(Orient_Vertical)
					->SetSizeCoefficient(0.7f)
					->Split
					(
						FTabManager::NewStack()
						->AddTab(ViewportTabId, ETabState::OpenedTab)
						->SetSizeCoefficient(0.8f)
						->SetHideTabWell(true)
					)
					->Split
					(
						FTabManager::NewStack()
						->SetSizeCoefficient(0.2f)
						->AddTab(FeatureEditorTabId, ETabState::OpenedTab)
						->SetHideTabWell(true)
					)
				)
				->Split
				(
					FTabManager::NewSplitter()->SetOrientation(Orient_Vertical)
					->SetSizeCoefficient(0.25f)
					->Split
					(
						FTabManager::NewStack()
						->SetSizeCoefficient(0.7f)
						->AddTab(PropertiesTabId, ETabState::OpenedTab)
						->SetForegroundTab(PropertiesTabId)
					)
				)
			)
		);

	const bool bCreateDefaultStandaloneMenu = true;
	const bool bCreateDefaultToolbar = true;
	FAssetEditorToolkit::InitAssetEditor(Mode, InitToolkitHost, OpenSplat4DPointCloudEditorAppIdentifier, StandaloneDefaultLayout, bCreateDefaultStandaloneMenu, bCreateDefaultToolbar, ObjectToEdit);

	GEditor->RegisterForUndo(this);
}

void FOpenSplat4DPointCloudEditor::SelectPointsByIndex(TArray<uint32> InIndices)
{
	SelectedIndices = InIndices;
}

void FOpenSplat4DPointCloudEditor::RemovePointsByIndex(TArray<uint32> InIndices)
{
	if (!InIndices.IsEmpty() && PointCloud)
	{
		TArray<FOpenSplat4DPoint> Points = PointCloud->GetPoints();
		InIndices.Sort();
		for (int32 i = InIndices.Num() - 1; i >= 0; --i)
		{
			int32 IndexToRemove = InIndices[i];
			if (IndexToRemove < Points.Num())
			{
				Points.RemoveAt(IndexToRemove);
			}
		}

		GEditor->BeginTransaction(OS4D_TEXT("Remove Points"));
		PointCloud->Modify();
		PointCloud->SetPoints(Points, /*bReorder=*/false);
		GEditor->EndTransaction();
	}
	SelectPointsByIndex({});
}

void FOpenSplat4DPointCloudEditor::RemoveSelectedPoints()
{
	RemovePointsByIndex(SelectedIndices);
}

void FOpenSplat4DPointCloudEditor::RegisterTabSpawners(const TSharedRef<class FTabManager>& InTabManager)
{
	WorkspaceMenuCategory = InTabManager->AddLocalWorkspaceMenuCategory(OS4D_TEXT("OpenSplat4D Point Cloud Editor"));
	auto WorkspaceMenuCategoryRef = WorkspaceMenuCategory.ToSharedRef();

	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	InTabManager->RegisterTabSpawner(ViewportTabId, FOnSpawnTab::CreateSP(this, &FOpenSplat4DPointCloudEditor::SpawnTab_Viewport))
		.SetDisplayName(OS4D_TEXT("Viewport"))
		.SetGroup(WorkspaceMenuCategoryRef)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"))
		.SetReadOnlyBehavior(ETabReadOnlyBehavior::Custom);

	InTabManager->RegisterTabSpawner(PropertiesTabId, FOnSpawnTab::CreateSP(this, &FOpenSplat4DPointCloudEditor::SpawnTab_Properties))
		.SetDisplayName(OS4D_TEXT("Details"))
		.SetGroup(WorkspaceMenuCategoryRef)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"))
		.SetReadOnlyBehavior(ETabReadOnlyBehavior::Custom);

	InTabManager->RegisterTabSpawner(PreviewSceneSettingsTabId, FOnSpawnTab::CreateSP(this, &FOpenSplat4DPointCloudEditor::SpawnTab_PreviewSceneSettings))
		.SetDisplayName(OS4D_TEXT("Preview Scene Settings"))
		.SetGroup(WorkspaceMenuCategoryRef)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"))
		.SetReadOnlyBehavior(ETabReadOnlyBehavior::Custom);

	InTabManager->RegisterTabSpawner(FeatureEditorTabId, FOnSpawnTab::CreateSP(this, &FOpenSplat4DPointCloudEditor::SpawnTab_FeatureEditor))
		.SetDisplayName(OS4D_TEXT("Features"))
		.SetGroup(WorkspaceMenuCategoryRef)
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Features"))
		.SetReadOnlyBehavior(ETabReadOnlyBehavior::Custom);
}

void FOpenSplat4DPointCloudEditor::UnregisterTabSpawners(const TSharedRef<class FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);

	InTabManager->UnregisterTabSpawner(ViewportTabId);
	InTabManager->UnregisterTabSpawner(PropertiesTabId);
	InTabManager->UnregisterTabSpawner(PreviewSceneSettingsTabId);
	InTabManager->UnregisterTabSpawner(FeatureEditorTabId);
}

TSharedRef<SDockTab> FOpenSplat4DPointCloudEditor::SpawnTab_Viewport(const FSpawnTabArgs& Args)
{
	TSharedRef<SDockTab> DockableTab = SNew(SDockTab);

	TWeakPtr<FOpenSplat4DPointCloudEditor> WeakSharedThis(SharedThis(this));
	AssetEditorViewportFactoryFunction MakeViewportFunc = [WeakSharedThis](const FAssetEditorViewportConstructionArgs& InArgs)
		{
			return SNew(SOpenSplat4DPointCloudEditorViewport)
				.Editor(WeakSharedThis);
		};

	ViewportTabContent = MakeShareable(new FEditorViewportTabContent());
	ViewportTabContent->OnViewportTabContentLayoutChanged().AddRaw(this, &FOpenSplat4DPointCloudEditor::OnEditorLayoutChanged);

	const FString LayoutId = FString("OpenSplat4DPointCloudEditorViewport");
	ViewportTabContent->Initialize(MakeViewportFunc, DockableTab, LayoutId);
	return DockableTab;
}

TSharedRef<SDockTab> FOpenSplat4DPointCloudEditor::SpawnTab_Properties(const FSpawnTabArgs& Args)
{
	FPropertyEditorModule& EditModule = FModuleManager::Get().GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.bShowObjectLabel = false;
	DetailsViewArgs.bAllowSearch = true;
	DetailsViewArgs.bAllowFavoriteSystem = true;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::ENameAreaSettings::HideNameArea;
	DetailsViewArgs.ViewIdentifier = FName("OpenSplat4DPointCloud");
	auto DetailsView = EditModule.CreateDetailView(DetailsViewArgs);
	DetailsView->SetObject(PointCloud);
	TSharedRef<SDockTab> DockableTab = SNew(SDockTab)
		[
			DetailsView
		];
	return DockableTab;
}

TSharedRef<SDockTab> FOpenSplat4DPointCloudEditor::SpawnTab_FeatureEditor(const FSpawnTabArgs& Args)
{
	TWeakPtr<FOpenSplat4DPointCloudEditor> WeakSharedThis(SharedThis(this));
	TSharedRef<SDockTab> DockableTab = SNew(SDockTab)
		[
			SAssignNew(FeatureEditor, SOpenSplat4DPointCloudFeatureEditor)
				.Editor(WeakSharedThis)
		];
	return DockableTab;
}

TSharedRef<SDockTab> FOpenSplat4DPointCloudEditor::SpawnTab_PreviewSceneSettings(const FSpawnTabArgs& Args)
{
	check(Args.GetTabId() == PreviewSceneSettingsTabId);
	return SAssignNew(PreviewSceneDockTab, SDockTab)
		.Label(OS4D_TEXT("Preview Scene Settings"))
		[
			AdvancedPreviewSettingsWidget.IsValid() ? AdvancedPreviewSettingsWidget.ToSharedRef() : SNullWidget::NullWidget
		];
}

void FOpenSplat4DPointCloudEditor::PostInitAssetEditor()
{
}

void FOpenSplat4DPointCloudEditor::CreateEditorModeManager()
{
	TSharedPtr<FAssetEditorModeManager> NewManager = MakeShared<FAssetEditorModeManager>();
	EditorModeManager = NewManager;
}

TSharedPtr<class SOpenSplat4DPointCloudEditorViewport> FOpenSplat4DPointCloudEditor::GetViewport() const
{
	if (ViewportTabContent.IsValid())
	{
		return StaticCastSharedPtr<SOpenSplat4DPointCloudEditorViewport>(ViewportTabContent->GetFirstViewport());
	}
	return TSharedPtr<SOpenSplat4DPointCloudEditorViewport>();
}

void FOpenSplat4DPointCloudEditor::ClearPropertyEditorSelection()
{
	if (FeatureEditor.IsValid())
	{
		FeatureEditor->ClearSelection();
	}
}

void FOpenSplat4DPointCloudEditor::OnEditorLayoutChanged()
{
	TSharedPtr<class SOpenSplat4DPointCloudEditorViewport> Viewport = GetViewport();
	if (!Viewport.IsValid())
	{
		return;
	}
	FAdvancedPreviewSceneModule& AdvancedPreviewSceneModule = FModuleManager::LoadModuleChecked<FAdvancedPreviewSceneModule>("AdvancedPreviewScene");
	AdvancedPreviewSettingsWidget = AdvancedPreviewSceneModule.CreateAdvancedPreviewSceneSettingsWidget(Viewport->GetPreviewScene(), nullptr, TArray<FAdvancedPreviewSceneModule::FDetailCustomizationInfo>(), TArray<FAdvancedPreviewSceneModule::FPropertyTypeCustomizationInfo>());
	if (PreviewSceneDockTab.IsValid())
	{
		PreviewSceneDockTab.Pin()->SetContent(AdvancedPreviewSettingsWidget.ToSharedRef());
	}
}

void FOpenSplat4DPointCloudEditor::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(PointCloud);
}

FString FOpenSplat4DPointCloudEditor::GetReferencerName() const
{
	return TEXT("FOpenSplat4DPointCloudEditor");
}

void FOpenSplat4DPointCloudEditor::PostUndo(bool bSuccess)
{
	if (PointCloud)
	{
		PointCloud->OnPointsChanged.Broadcast();
	}
}

void FOpenSplat4DPointCloudEditor::PostRedo(bool bSuccess)
{
	if (PointCloud)
	{
		PointCloud->OnPointsChanged.Broadcast();
	}
}

FName FOpenSplat4DPointCloudEditor::GetToolkitFName() const
{
	return FName("OpenSplat4DPointCloudEditor");
}

FText FOpenSplat4DPointCloudEditor::GetBaseToolkitName() const
{
	return OS4D_TEXT("OpenSplat4D Point Cloud Editor");
}

FString FOpenSplat4DPointCloudEditor::GetWorldCentricTabPrefix() const
{
	return OS4D_TEXT("OpenSplat4DPointCloud").ToString();
}

FLinearColor FOpenSplat4DPointCloudEditor::GetWorldCentricTabColorScale() const
{
	return FLinearColor(0.3f, 0.2f, 0.5f, 0.5f);
}

FString FOpenSplat4DPointCloudEditor::GetDocumentationLink() const
{
	return FString(TEXT("Engine/Content/Types/OpenSplat4DPointCloud/Editor"));
}

#undef LOCTEXT_NAMESPACE
