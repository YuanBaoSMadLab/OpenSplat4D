// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatAssetEditor.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatAssetViewport.h"
#include "NanoGSGaussianSplatComponent.h"
#include "NanoGSGaussianSplatActor.h"
#include "GaussianSplatEditorData.h"
#include "PropertyEditorModule.h"
#include "IDetailsView.h"
#include "Modules/ModuleManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
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
	if (EditorData.IsValid())
	{
		EditorData->OnChanged.RemoveAll(this);
	}
}

void FGaussianSplatAssetEditor::NotifyPostChange(const FPropertyChangedEvent& PropertyChangedEvent, FProperty* PropertyThatChanged)
{
	FNotifyHook::NotifyPostChange(PropertyChangedEvent, PropertyThatChanged);

	// Details-panel edits must stay in sync with the preview viewport.
	const FName ChangedProp = PropertyChangedEvent.GetPropertyName();

	// Nanite toggle: the cluster hierarchy is (re)built/cleared and the shared
	// render data is invalidated -- re-create the preview actor so the viewport
	// immediately reflects the new state instead of showing stale splats.
	if (SplatAsset.IsValid() && ViewportWidget.IsValid() &&
		ChangedProp == GET_MEMBER_NAME_CHECKED(UGaussianSplatAsset, bEnableNanite))
	{
		ViewportWidget->SetSplatAsset(SplatAsset.Get(), /*bFrameAsset=*/false);
	}
	// Performance preview settings: push live onto the existing preview actor's
	// component (no actor respawn / camera change needed).
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

	// ============================================================================
	// 创建 EditorData（编辑会话状态：选中 / 隐藏 flags）
	// ============================================================================
	// 用 GetTransientPackage() + NewObject 创建 Transient 对象，不写入磁盘。
	// TStrongObjectPtr 持有强引用防止 GC 回收，编辑器关闭时自动释放。
	if (InAsset)
	{
		EditorData = TStrongObjectPtr<UGaussianSplatEditorData>(
			NewObject<UGaussianSplatEditorData>(GetTransientPackage(), NAME_None, RF_Transient));
		EditorData->Initialize(InAsset);
		EditorData->OnChanged.AddSP(this, &FGaussianSplatAssetEditor::OnEditorDataChanged);
	}

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
	// Layout: 水平分割（左 viewport [含顶部工具栏] + 右 details）
	// 工具栏按钮直接嵌入 viewport tab 内部（在 SpawnTab_Viewport 中构建），
	// 不需要单独的 toolbar tab，避免注册未使用的 tab id。
	const TSharedRef<FTabManager::FLayout> StandaloneDefaultLayout = FTabManager::NewLayout("Standalone_GaussianSplatAssetEditor_Layout")
		->AddArea
		(
			FTabManager::NewPrimaryArea()
			->SetOrientation(Orient_Horizontal)
			->Split
			(
				// Left: Viewport (takes most of the space, toolbar embedded inside)
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

	// 在标准 toolbar 之外，添加自定义编辑工具按钮（在 toolkit toolbar 右侧追加）
	BuildToolbar();
}

void FGaussianSplatAssetEditor::BuildToolbar()
{
	// ============================================================================
	// 工具栏按钮已经直接嵌入 Viewport tab 内部（在 SpawnTab_Viewport 中用 SButton
	// 构建），不使用 FToolBarBuilder / FUICommandInfo 机制。这样：
	//   1. 布局更紧凑，按钮和视口在同一 tab 内
	//   2. 不需要注册 FUICommandInfo（避免 Commands 模块的样板代码）
	//   3. 按钮状态（选中/可用）通过 lambda 实时查询 EditorData
	//
	// 这个函数保留为空，是为了未来如果需要把按钮迁移到 toolkit 顶部 toolbar 时的
	// 扩展点。当前所有按钮逻辑都在 SpawnTab_Viewport 中。
	// ============================================================================
}

void FGaussianSplatAssetEditor::OnToolButtonClicked(EGaussianEditTool InTool)
{
	if (!EditorData.IsValid()) return;

	// 切换工具：如果点击当前活动工具，则关闭（变为 None）；否则切换到新工具
	const EGaussianEditTool Current = EditorData->GetActiveTool();
	EditorData->SetActiveTool(Current == InTool ? EGaussianEditTool::None : InTool);

	// 工具切换不触发 OnChanged（不改选中状态），但需要刷新 viewport 显示鼠标提示
	RefreshDetails();
}

void FGaussianSplatAssetEditor::OnSelectAllClicked()
{
	if (EditorData.IsValid())
	{
		EditorData->SelectAll();
	}
}

void FGaussianSplatAssetEditor::OnDeselectAllClicked()
{
	if (EditorData.IsValid())
	{
		EditorData->DeselectAll();
	}
}

void FGaussianSplatAssetEditor::OnInvertSelectionClicked()
{
	if (EditorData.IsValid())
	{
		EditorData->InvertSelection();
	}
}

void FGaussianSplatAssetEditor::OnDeleteSelectedClicked()
{
	if (!EditorData.IsValid()) return;

	const int32 Deleted = EditorData->DeleteSelected();
	if (Deleted > 0)
	{
		// 弹通知告知用户（删除是 stub，已自动隐藏）
		FNotificationInfo NotifyInfo(FText::Format(
			LOCTEXT("DeleteNotification", "已隐藏 {0} 个 splat（删除功能后续实现，当前用隐藏替代）"),
			FText::AsNumber(Deleted)));
		NotifyInfo.ExpireDuration = 4.0f;
		NotifyInfo.bUseSuccessFailIcons = true;
		if (TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(NotifyInfo))
		{
			Notification->SetCompletionState(SNotificationItem::CS_Success);
		}
	}
}

void FGaussianSplatAssetEditor::OnHideSelectedClicked()
{
	if (EditorData.IsValid())
	{
		EditorData->HideSelected();
	}
}

void FGaussianSplatAssetEditor::OnIsolateSelectedClicked()
{
	if (EditorData.IsValid())
	{
		EditorData->IsolateSelected();
	}
}

void FGaussianSplatAssetEditor::OnShowAllClicked()
{
	if (EditorData.IsValid())
	{
		EditorData->ShowAll();
	}
}

void FGaussianSplatAssetEditor::RefreshDetails()
{
	// 让 DetailsView 重新读取对象属性（如果 EditorData 暴露了选中数量等）
	if (DetailsView.IsValid() && SplatAsset.IsValid())
	{
		DetailsView->ForceRefresh();
	}
}

void FGaussianSplatAssetEditor::OnEditorDataChanged()
{
	// EditorData 变化时刷新 viewport 和 details
	RefreshDetails();
}

TSharedPtr<SGaussianSplatAssetViewport> FGaussianSplatAssetEditor::GetViewportWidget() const
{
	return ViewportWidget;
}

TSharedRef<SDockTab> FGaussianSplatAssetEditor::SpawnTab_Viewport(const FSpawnTabArgs& Args)
{
	check(Args.GetTabId() == ViewportTabId);

	// 创建 viewport widget
	TSharedRef<SGaussianSplatAssetViewport> ViewportWidgetRef =
		SNew(SGaussianSplatAssetViewport, nullptr);
	ViewportWidget = ViewportWidgetRef;

	// Set the asset to preview
	if (SplatAsset.IsValid())
	{
		ViewportWidgetRef->SetSplatAsset(SplatAsset.Get());
	}

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
