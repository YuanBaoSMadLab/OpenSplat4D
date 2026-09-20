// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplat4DEditor.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatAssetViewport.h"
#include "S4DTransportBar.h"
#include "NanoGSGaussianSplatComponent.h"
#include "NanoGSGaussianSplatActor.h"
#include "PropertyEditorModule.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "GaussianSplat4DEditor"

const FName FGaussianSplat4DEditor::ViewportTabId(TEXT("GaussianSplat4DEditor_Viewport"));
const FName FGaussianSplat4DEditor::DetailsTabId(TEXT("GaussianSplat4DEditor_Details"));

FGaussianSplat4DEditor::FGaussianSplat4DEditor()
{
}

FGaussianSplat4DEditor::~FGaussianSplat4DEditor()
{
}

void FGaussianSplat4DEditor::NotifyPostChange(const FPropertyChangedEvent& PropertyChangedEvent, FProperty* PropertyThatChanged)
{
	FNotifyHook::NotifyPostChange(PropertyChangedEvent, PropertyThatChanged);

	const FName ChangedProp = PropertyChangedEvent.GetPropertyName();

	// Nanite toggle: cluster hierarchy rebuilt/cleared -> respawn the preview
	// actor, then REBIND the transport bar to the new preview component.
	if (SplatAsset.IsValid() && ViewportWidget.IsValid() &&
		ChangedProp == GET_MEMBER_NAME_CHECKED(UGaussianSplatAsset, bEnableNanite))
	{
		ViewportWidget->SetSplatAsset(SplatAsset.Get(), /*bFrameAsset=*/false);

		if (TransportBar.IsValid())
		{
			AGaussianSplatActor* PreviewActor = ViewportWidget->GetPreviewActor();
			TransportBar->BindComponent(SplatAsset.Get(), PreviewActor ? PreviewActor->GaussianSplatComponent : nullptr);
		}
	}
	// 4D playback preview settings: push live + rebind (cheap, keeps bar in sync)
	else if (ChangedProp == GET_MEMBER_NAME_CHECKED(UGaussianSplatAsset, PreviewAutoPlay) ||
			 ChangedProp == GET_MEMBER_NAME_CHECKED(UGaussianSplatAsset, PreviewPlayRate))
	{
		if (AGaussianSplatActor* PreviewActor = ViewportWidget.IsValid() ? ViewportWidget->GetPreviewActor() : nullptr)
		{
			if (UGaussianSplatComponent* Comp = PreviewActor->GaussianSplatComponent)
			{
				Comp->bAutoPlay = SplatAsset->PreviewAutoPlay;
				Comp->PlayRate = SplatAsset->PreviewPlayRate;
				if (SplatAsset->PreviewAutoPlay)
				{
					Comp->Play4D();
				}
				else
				{
					Comp->Pause4D();
				}
			}
		}
	}
	// Performance preview settings: push live onto the preview component
	else if (ChangedProp == GET_MEMBER_NAME_CHECKED(UGaussianSplatAsset, PreviewNanitePrecision) ||
			 ChangedProp == GET_MEMBER_NAME_CHECKED(UGaussianSplatAsset, PreviewMaxDrawDistance) ||
			 ChangedProp == GET_MEMBER_NAME_CHECKED(UGaussianSplatAsset, PreviewFadeOutStartDistance))
	{
		if (AGaussianSplatActor* PreviewActor = ViewportWidget.IsValid() ? ViewportWidget->GetPreviewActor() : nullptr)
		{
			if (UGaussianSplatComponent* Comp = PreviewActor->GaussianSplatComponent)
			{
				Comp->ApplyPerformanceSettings(
					SplatAsset->PreviewNanitePrecision,
					SplatAsset->PreviewMaxDrawDistance,
					SplatAsset->PreviewFadeOutStartDistance);
			}
		}
	}
}

void FGaussianSplat4DEditor::RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	WorkspaceMenuCategory = InTabManager->AddLocalWorkspaceMenuCategory(LOCTEXT("WorkspaceMenu_GaussianSplat4DEditor", "OpenSplat 4D 播放器"));

	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	InTabManager->RegisterTabSpawner(ViewportTabId, FOnSpawnTab::CreateSP(this, &FGaussianSplat4DEditor::SpawnTab_Viewport))
		.SetDisplayName(LOCTEXT("ViewportTab", "视口 + 播放器"))
		.SetTooltipText(LOCTEXT("ViewportTabTooltip", "OpenSplat 4D 资产预览与播放控制"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef());

	InTabManager->RegisterTabSpawner(DetailsTabId, FOnSpawnTab::CreateSP(this, &FGaussianSplat4DEditor::SpawnTab_Details))
		.SetDisplayName(LOCTEXT("DetailsTab", "详情"))
		.SetTooltipText(LOCTEXT("DetailsTabTooltip", "OpenSplat 4D 资产的属性面板"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef());
}

void FGaussianSplat4DEditor::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);

	InTabManager->UnregisterTabSpawner(ViewportTabId);
	InTabManager->UnregisterTabSpawner(DetailsTabId);
}

FName FGaussianSplat4DEditor::GetToolkitFName() const
{
	return FName("GaussianSplat4DEditor");
}

FText FGaussianSplat4DEditor::GetBaseToolkitName() const
{
	return LOCTEXT("BaseToolkitName", "OpenSplat 4D 播放器");
}

FText FGaussianSplat4DEditor::GetToolkitName() const
{
	if (SplatAsset.IsValid())
	{
		return FText::FromString(FString::Printf(TEXT("OpenSplat 4D 播放器: %s"), *SplatAsset->GetName()));
	}
	return LOCTEXT("BaseToolkitName", "OpenSplat 4D 播放器");
}

FString FGaussianSplat4DEditor::GetWorldCentricTabPrefix() const
{
	return TEXT("OpenSplat4DEditor");
}

FLinearColor FGaussianSplat4DEditor::GetWorldCentricTabColorScale() const
{
	// 与 3D 编辑器区分的暖色调
	return FLinearColor(0.85f, 0.55f, 0.2f, 1.0f);
}

FString FGaussianSplat4DEditor::GetDocumentationLink() const
{
	return TEXT("");
}

void FGaussianSplat4DEditor::InitGaussianSplat4DEditor(const EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& InitToolkitHost, UGaussianSplatAsset* InAsset)
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

	// Layout: 水平分割（左 = 视口+底部播放器，右 = details）
	const TSharedRef<FTabManager::FLayout> StandaloneDefaultLayout = FTabManager::NewLayout("Standalone_GaussianSplat4DEditor_Layout")
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

	const bool bCreateDefaultStandaloneMenu = true;
	const bool bCreateDefaultToolbar = false; // 播放器不需要编辑工具栏
	FAssetEditorToolkit::InitAssetEditor(Mode, InitToolkitHost, TEXT("GaussianSplat4DEditorApp"), StandaloneDefaultLayout, bCreateDefaultStandaloneMenu, bCreateDefaultToolbar, InAsset);
}

TSharedRef<SDockTab> FGaussianSplat4DEditor::SpawnTab_Viewport(const FSpawnTabArgs& Args)
{
	check(Args.GetTabId() == ViewportTabId);

	// 创建 viewport widget
	TSharedRef<SGaussianSplatAssetViewport> ViewportWidgetRef =
		SNew(SGaussianSplatAssetViewport, nullptr);
	ViewportWidget = ViewportWidgetRef;

	// 预览组件引用（供 transport bar 绑定）
	UGaussianSplatComponent* PreviewComponent = nullptr;

	if (SplatAsset.IsValid())
	{
		ViewportWidgetRef->SetSplatAsset(SplatAsset.Get());
		if (AGaussianSplatActor* PreviewActor = ViewportWidgetRef->GetPreviewActor())
		{
			PreviewComponent = PreviewActor->GaussianSplatComponent;
		}
	}

	// 底部播放器控制条
	TransportBar = SNew(S4DTransportBar, SplatAsset.Get(), PreviewComponent);

	// 垂直组合：视口（占满）+ 底部播放器
	return SNew(SDockTab)
		.Label(LOCTEXT("ViewportTitle", "4D 预览"))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Toolbox.GroupBorder"))
				[
					ViewportWidgetRef
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				TransportBar.ToSharedRef()
			]
		];
}

TSharedRef<SDockTab> FGaussianSplat4DEditor::SpawnTab_Details(const FSpawnTabArgs& Args)
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
