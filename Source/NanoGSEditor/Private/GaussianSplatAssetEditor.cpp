// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatAssetEditor.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatAssetViewport.h"
#include "PropertyEditorModule.h"
#include "IDetailsView.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SBoxPanel.h"
#include "Framework/Docking/LayoutService.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/AppStyle.h"
#include "Editor.h"

#define LOCTEXT_NAMESPACE "GaussianSplatAssetEditor"

const FName FGaussianSplatAssetEditor::ViewportTabId(TEXT("GaussianSplatAssetEditor_Viewport"));
const FName FGaussianSplatAssetEditor::DetailsTabId(TEXT("GaussianSplatAssetEditor_Details"));

FGaussianSplatAssetEditor::FGaussianSplatAssetEditor()
{
}

FGaussianSplatAssetEditor::~FGaussianSplatAssetEditor()
{
}

void FGaussianSplatAssetEditor::NotifyPostChange(const FPropertyChangedEvent& PropertyChangedEvent, FProperty* PropertyThatChanged)
{
	FNotifyHook::NotifyPostChange(PropertyChangedEvent, PropertyThatChanged);
}

void FGaussianSplatAssetEditor::RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	WorkspaceMenuCategory = InTabManager->AddLocalWorkspaceMenuCategory(LOCTEXT("WorkspaceMenu_GaussianSplatAssetEditor", "OpenSplat 资产编辑器"));

	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	InTabManager->RegisterTabSpawner(ViewportTabId, FOnSpawnTab::CreateSP(this, &FGaussianSplatAssetEditor::SpawnTab_Viewport))
		.SetDisplayName(LOCTEXT("ViewportTab", "视口"))
		.SetTooltipText(LOCTEXT("ViewportTabTooltip", "OpenSplat 资产的 3D 预览视口"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef());

	InTabManager->RegisterTabSpawner(DetailsTabId, FOnSpawnTab::CreateSP(this, &FGaussianSplatAssetEditor::SpawnTab_Details))
		.SetDisplayName(LOCTEXT("DetailsTab", "详情"))
		.SetTooltipText(LOCTEXT("DetailsTabTooltip", "OpenSplat 资产的属性面板"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef());
}

void FGaussianSplatAssetEditor::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);

	InTabManager->UnregisterTabSpawner(ViewportTabId);
	InTabManager->UnregisterTabSpawner(DetailsTabId);
}

FName FGaussianSplatAssetEditor::GetToolkitFName() const
{
	return FName("GaussianSplatAssetEditor");
}

FText FGaussianSplatAssetEditor::GetBaseToolkitName() const
{
	return LOCTEXT("BaseToolkitName", "OpenSplat 资产编辑器");
}

FText FGaussianSplatAssetEditor::GetToolkitName() const
{
	if (SplatAsset.IsValid())
	{
		return FText::FromString(FString::Printf(TEXT("OpenSplat 资产: %s"), *SplatAsset->GetName()));
	}
	return LOCTEXT("BaseToolkitName", "OpenSplat 资产编辑器");
}

FString FGaussianSplatAssetEditor::GetWorldCentricTabPrefix() const
{
	return TEXT("OpenSplatAssetEditor");
}

FLinearColor FGaussianSplatAssetEditor::GetWorldCentricTabColorScale() const
{
	return FLinearColor(0.3f, 0.6f, 0.8f, 1.0f);
}

FString FGaussianSplatAssetEditor::GetDocumentationLink() const
{
	return TEXT("");
}

void FGaussianSplatAssetEditor::InitGaussianSplatAssetEditor(const EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& InitToolkitHost, UGaussianSplatAsset* InAsset)
{
	SplatAsset = TStrongObjectPtr<UGaussianSplatAsset>(InAsset);

	// Create the details view
	FPropertyEditorModule& PropertyEditorModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	DetailsViewArgs.bHideSelectionTip = true;
	DetailsViewArgs.bShowPropertyMatrixButton = false;
	DetailsViewArgs.NotifyHook = this;
	DetailsViewArgs.DefaultsOnlyVisibility = EEditDefaultsOnlyNodeVisibility::Hide;

	DetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);
	if (DetailsView.IsValid() && SplatAsset.IsValid())
	{
		DetailsView->SetObject(SplatAsset.Get());
	}

	// Initialize the toolkit
	const TSharedRef<FTabManager::FLayout> StandaloneDefaultLayout = FTabManager::NewLayout("Standalone_GaussianSplatAssetEditor_Layout")
		->AddArea
		(
			FTabManager::NewPrimaryArea()
			->SetOrientation(Orient_Horizontal)
			->Split
			(
				// Left: Viewport (takes most of the space)
				FTabManager::NewStack()
				->SetSizeCoefficient(0.7f)
				->AddTab(ViewportTabId, ETabState::OpenedTab)
			)
			->Split
			(
				// Right: Details panel
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

	const bool bCreateDefaultStandaloneMenu = true;
	const bool bCreateDefaultToolbar = true;
	FAssetEditorToolkit::InitAssetEditor(Mode, InitToolkitHost, TEXT("GaussianSplatAssetEditorApp"), StandaloneDefaultLayout, bCreateDefaultStandaloneMenu, bCreateDefaultToolbar, InAsset);
}

TSharedRef<SDockTab> FGaussianSplatAssetEditor::SpawnTab_Viewport(const FSpawnTabArgs& Args)
{
	check(Args.GetTabId() == ViewportTabId);

	// Create the viewport widget
	TSharedRef<SGaussianSplatAssetViewport> ViewportWidget =
		SNew(SGaussianSplatAssetViewport, nullptr);

	// Set the asset to preview
	if (SplatAsset.IsValid())
	{
		ViewportWidget->SetSplatAsset(SplatAsset.Get());
	}

	return SNew(SDockTab)
		.Label(LOCTEXT("ViewportTitle", "视口"))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Toolbox.GroupBorder"))
			[
				ViewportWidget
			]
		];
}

TSharedRef<SDockTab> FGaussianSplatAssetEditor::SpawnTab_Details(const FSpawnTabArgs& Args)
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
