#include "OpenSplat4DEdModePanel.h"
#include "OpenSplat4DPointCloud.h"
#include "GaussianSplatActor.h"
#include "GaussianSplatComponent.h"
#include "OpenSplat4DEditorLibrary.h"
#include "OpenSplat4DSettings.h"
#include "OpenSplat4DLocalization.h"

#include "UObject/Object.h"

#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "PropertyCustomizationHelpers.h"
#include "PropertyEditorModule.h"
#include "IDetailsView.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"

#include "Editor.h"
#include "Selection.h"
#include "FileHelpers.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Misc/PackagePath.h"
#include "HAL/FileManager.h"
#include "UObject/SavePackage.h"

#define LOCTEXT_NAMESPACE "OpenSplat4D"

SOpenSplat4DEdModePanel::~SOpenSplat4DEdModePanel()
{
	FCoreUObjectDelegates::OnObjectPropertyChanged.RemoveAll(this);
	if (StepCapture) { StepCapture->Deactivate(); }
	if (StepSparseReconstruction) { StepSparseReconstruction->Deactivate(); }
	if (StepGaussianSplatting) { StepGaussianSplatting->Deactivate(); }
}

void SOpenSplat4DEdModePanel::Construct(const FArguments& InArgs)
{
	// ---- Details view (shared by the pipeline tabs) -------------------------
	FPropertyEditorModule& EditModule = FModuleManager::Get().GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.bShowObjectLabel = false;
	DetailsViewArgs.bAllowSearch = true;
	DetailsViewArgs.bAllowFavoriteSystem = true;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::ENameAreaSettings::HideNameArea;
	DetailsViewArgs.ViewIdentifier = FName("OpenSplat4DDefaults");
	DetailsView = EditModule.CreateDetailView(DetailsViewArgs);
	DetailsView->OnFinishedChangingProperties().AddSP(this, &SOpenSplat4DEdModePanel::OnTabPropertyChanged);

	// ---- Pipeline step objects ---------------------------------------------
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	const bool bPackageDirty = EditorWorld ? EditorWorld->GetPackage()->IsDirty() : false;
	UObject* Outer = EditorWorld != nullptr ? (UObject*)EditorWorld->GetPackage() : (UObject*)GetTransientPackage();
	BaseWorkDir = GetDefault<UOpenSplat4DSettings>()->GetWorkDir(TEXT("OpenSplat4DEditor"));

	StepCapture = NewObject<UOpenSplat4DStep_Capture>(Outer, NAME_None, RF_Transient);
	StepSparseReconstruction = NewObject<UOpenSplat4DStep_SparseReconstruction>(Outer, NAME_None, RF_Transient);
	StepGaussianSplatting = NewObject<UOpenSplat4DStep_GaussianSplatting>(Outer, NAME_None, RF_Transient);

	for (UOpenSplat4DStepBase* Step : { (UOpenSplat4DStepBase*)StepCapture,
										(UOpenSplat4DStepBase*)StepSparseReconstruction,
										(UOpenSplat4DStepBase*)StepGaussianSplatting })
	{
		Step->SetWorld(EditorWorld);
		Step->LoadConfig();
		Step->Activate();
	}

	// Discover existing capture-set assets (persisted in Content) and pick the
	// first as the active one. If none exist yet, leave the picker empty and let
	// the capture step create one on first capture (WorkDir falls back to a
	// "Default" folder under BaseWorkDir).
	TArray<UOpenSplat4DCaptureSet*> Sets;
	OpenSplat4DEnumerateCaptureSets(Sets);
	if (Sets.Num() > 0)
	{
		StepCapture->CaptureSetAsset = Sets[0];
		WorkDir = Sets[0]->WorkDirectory;
		// Pre-select the same capture set as the reconstruction / training target
		// so the downstream steps know which index asset to operate on.
		StepSparseReconstruction->TargetCaptureSet = Sets[0];
		StepGaussianSplatting->TargetCaptureSet = Sets[0];
	}
	else
	{
		WorkDir = BaseWorkDir / TEXT("Default");
	}
	StepCapture->SetWorkDir(WorkDir);
	StepSparseReconstruction->SetWorkDir(WorkDir);
	StepGaussianSplatting->SetWorkDir(WorkDir);

	if (EditorWorld && !bPackageDirty)
	{
		EditorWorld->GetPackage()->SetDirtyFlag(false);
	}

	StepCapture->OnRequestTaskStart.BindSP(this, &SOpenSplat4DEdModePanel::OnRequestTaskStart, (UOpenSplat4DStepBase*)StepCapture.Get());
	StepSparseReconstruction->OnRequestTaskStart.BindSP(this, &SOpenSplat4DEdModePanel::OnRequestTaskStart, (UOpenSplat4DStepBase*)StepSparseReconstruction.Get());
	StepGaussianSplatting->OnRequestTaskStart.BindSP(this, &SOpenSplat4DEdModePanel::OnRequestTaskStart, (UOpenSplat4DStepBase*)StepGaussianSplatting.Get());

	StepCapture->OnTaskFinished.AddSP(this, &SOpenSplat4DEdModePanel::OnTaskFinished, (UOpenSplat4DStepBase*)StepCapture.Get());
	StepSparseReconstruction->OnTaskFinished.AddSP(this, &SOpenSplat4DEdModePanel::OnTaskFinished, (UOpenSplat4DStepBase*)StepSparseReconstruction.Get());
	StepGaussianSplatting->OnTaskFinished.AddSP(this, &SOpenSplat4DEdModePanel::OnTaskFinished, (UOpenSplat4DStepBase*)StepGaussianSplatting.Get());

	// ---- Layout (delegated to BuildContent so the panel can rebuild live
	//      when the UI language (UISLanguage) setting changes) ------------
	ChildSlot[ BuildContent() ];

	// Rebuild this panel if the user flips the UI language in project settings.
	FCoreUObjectDelegates::OnObjectPropertyChanged.AddSP(this, &SOpenSplat4DEdModePanel::OnSettingsPropertyChanged);

	OnTabChanged(0);
}

TSharedRef<SWidget> SOpenSplat4DEdModePanel::BuildContent()
{
	return SNew(SVerticalBox)

		// Capture-set asset picker (native UE DataAsset — drag in like a material)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(5, 2)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, 4, 0)
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("捕获组:")))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(100)
			.MaxWidth(240)
			.VAlign(VAlign_Center)
			[
			SNew(SObjectPropertyEntryBox)
			.AllowedClass(UOpenSplat4DCaptureSet::StaticClass())
			.ObjectPath(this, &SOpenSplat4DEdModePanel::GetCaptureSetObjectPath)
			.OnObjectChanged(this, &SOpenSplat4DEdModePanel::OnCaptureSetChanged)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4, 0, 0, 0)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("新建捕获组")))
				.OnClicked(this, &SOpenSplat4DEdModePanel::OnNewCaptureSetClicked)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4, 0, 0, 0)
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("打开索引")))
				.ToolTipText(FText::FromString(TEXT("打开当前选中的【捕获组】索引资产，查看 / 编辑其记录的文件位置与范围。")))
				.OnClicked(this, &SOpenSplat4DEdModePanel::OnOpenCaptureSetClicked)
			]
		]

		// Tab bar + browse work-dir button
		+ SVerticalBox::Slot()
		.Padding(0)
		.AutoHeight()
		.MaxHeight(100)
		.VAlign(VAlign_Top)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(100)
			[
				SNew(SSegmentedControl<int32>)
				.Value(0)
				.OnValueChanged(this, &SOpenSplat4DEdModePanel::OnTabChanged)
				+ SSegmentedControl<int32>::Slot(0).Text(OS4D_TEXT("Capture"))
				+ SSegmentedControl<int32>::Slot(1).Text(OS4D_TEXT("Sparse"))
				+ SSegmentedControl<int32>::Slot(2).Text(OS4D_TEXT("Gaussian"))
				+ SSegmentedControl<int32>::Slot(3)
					.Icon(FAppStyle::Get().GetBrush("Icons.Settings"))
					.Text(OS4D_TEXT("Settings"))
				+ SSegmentedControl<int32>::Slot(4).Text(OS4D_TEXT("Usage"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(5)
			[
				PropertyCustomizationHelpers::MakeBrowseButton(
					FSimpleDelegate::CreateSP(this, &SOpenSplat4DEdModePanel::OnClicked_Browse),
					OS4D_TEXT("Open the working directory"))
			]
		]

		// Progress bar + cancel
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Fill)
		.Padding(10, 5)
		[
			SNew(SHorizontalBox)
			.Visibility(this, &SOpenSplat4DEdModePanel::OnGetProgressBarVisibility)
			+ SHorizontalBox::Slot()
			.FillWidth(100)
			[
				SNew(SBox)
				.HeightOverride(5)
				[
					SNew(SProgressBar)
					.Percent(this, &SOpenSplat4DEdModePanel::OnGetProgressPercent)
					.FillColorAndOpacity(FSlateColor(FLinearColor(0.0f, 1.0f, 1.0f)))
				]
			]
			+ SHorizontalBox::Slot()
			.HAlign(HAlign_Center)
			.AutoWidth()
			.Padding(5, 0)
			[
				SNew(SButton)
				.ToolTipText(OS4D_TEXT("Cancel Current Task"))
				.OnClicked(this, &SOpenSplat4DEdModePanel::OnClicked_Cancel)
				.ContentPadding(0.0f)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Symbols.X"))
					.DesiredSizeOverride(FVector2D(12, 12))
					.ColorAndOpacity(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f))
				]
			]
		]

		// Body: details view (tabs 0-3) or usage widget (tab 4)
		+ SVerticalBox::Slot()
		.VAlign(VAlign_Fill)
		.FillHeight(100)
		[
			SAssignNew(BodySwitcher, SWidgetSwitcher)
			+ SWidgetSwitcher::Slot()
			[
				DetailsView.ToSharedRef()
			]
			+ SWidgetSwitcher::Slot()
			[
				BuildUsageTab()
			]
		];
}

void SOpenSplat4DEdModePanel::Rebuild()
{
	// Tear down the previous bindings and rebuild from scratch with the new
	// language. Tab selection resets to 0; acceptable for a language switch.
	FCoreUObjectDelegates::OnObjectPropertyChanged.RemoveAll(this);
	ChildSlot[ BuildContent() ];
	FCoreUObjectDelegates::OnObjectPropertyChanged.AddSP(this, &SOpenSplat4DEdModePanel::OnSettingsPropertyChanged);
	OnTabChanged(0);
}

void SOpenSplat4DEdModePanel::OnSettingsPropertyChanged(UObject* Object, struct FPropertyChangedEvent& Event)
{
	if (Object == GetDefault<UOpenSplat4DSettings>() &&
		Event.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(UOpenSplat4DSettings, UISLanguage))
	{
		Rebuild();
	}
}

void SOpenSplat4DEdModePanel::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(StepCapture);
	Collector.AddReferencedObject(StepSparseReconstruction);
	Collector.AddReferencedObject(StepGaussianSplatting);
	Collector.AddReferencedObject(CurrentTask);
}

void SOpenSplat4DEdModePanel::OnTabChanged(int32 TabIndex)
{
	if (TabIndex >= 4)
	{
		if (BodySwitcher.IsValid()) { BodySwitcher->SetActiveWidgetIndex(1); }
		return;
	}

	UObject* ObjectToEdit = nullptr;
	if (TabIndex == 0) { ObjectToEdit = StepCapture; }
	else if (TabIndex == 1) { ObjectToEdit = StepSparseReconstruction; }
	else if (TabIndex == 2) { ObjectToEdit = StepGaussianSplatting; }
	else { ObjectToEdit = GetMutableDefault<UOpenSplat4DSettings>(); }

	if (BodySwitcher.IsValid()) { BodySwitcher->SetActiveWidgetIndex(0); }
	if (DetailsView.IsValid()) { DetailsView->SetObject(ObjectToEdit); }
}

void SOpenSplat4DEdModePanel::OnTabPropertyChanged(const FPropertyChangedEvent& ChangedEvent)
{
	if (ChangedEvent.GetNumObjectsBeingEdited() == 1)
	{
		if (UObject* ObjectToEdit = const_cast<UObject*>(ChangedEvent.GetObjectBeingEdited(0)))
		{
			ObjectToEdit->TryUpdateDefaultConfigFile();
		}
	}
}

bool SOpenSplat4DEdModePanel::OnRequestTaskStart(UOpenSplat4DStepBase* Step)
{
	if (CurrentTask != nullptr)
	{
		FNotificationInfo NotifyInfo(OS4D_TEXT("Execution failed\n there is currently a running task"));
		NotifyInfo.ExpireDuration = 5.0f;
		NotifyInfo.bUseSuccessFailIcons = true;
		TSharedPtr<SNotificationItem> NotificationPtr = FSlateNotificationManager::Get().AddNotification(NotifyInfo);
		if (NotificationPtr)
		{
			NotificationPtr->SetCompletionState(SNotificationItem::CS_Fail);
		}
		return false;
	}

	// Dependency pre-flight. Resolution order for every external tool / repo is
	//   (1) user-specified path in settings  ->  (2) the plugin's bundled
	//       ThirdParty copy  ->  (3) nothing.
	// If neither (1) nor (2) resolves, remind the user to specify a path before
	// running the step, instead of failing deep inside an external process.
	const UOpenSplat4DSettings* S = GetDefault<UOpenSplat4DSettings>();
	TArray<FString> Missing;
	auto CheckPath = [&](const FString& Resolved, bool bIsDir, const TCHAR* Label)
	{
		if (Resolved.IsEmpty())
		{
			Missing.Add(FString::Printf(
				TEXT("%s：插件 ThirdParty 中未找到，也未在【项目设置 → OpenSplat4D】中指定。请在设置里指定路径，或把对应程序/仓库放入插件 ThirdParty 目录。"),
				Label));
		}
		else if (bIsDir ? !FPaths::DirectoryExists(Resolved) : !FPaths::FileExists(Resolved))
		{
			Missing.Add(FString::Printf(TEXT("%s：已指定的路径不存在或无效：%s"), Label, *Resolved));
		}
	};

	if (Step == StepSparseReconstruction.Get() || Step == StepGaussianSplatting.Get())
	{
		CheckPath(S->GetColmapExecutablePath(), false, TEXT("Colmap 可执行文件"));
		// Require an explicit target capture set (index asset) so the step never
		// silently reconstructs the wrong data.
		UOpenSplat4DCaptureSet* Target = (Step == StepSparseReconstruction.Get())
			? StepSparseReconstruction->TargetCaptureSet
			: StepGaussianSplatting->TargetCaptureSet;
		if (!Target)
		{
			Missing.Add(TEXT("目标捕获组（索引资产）未选择：请先在对应步骤的【目标捕获组（索引资产）】中选择要重建 / 训练的捕获组（也可在面板顶部的【捕获组】下拉中选择，会自动同步）。未选择时不会执行，以避免重建错误的资产。"));
		}
	}
	if (Step == StepGaussianSplatting.Get())
	{
		// Only the repo matching the current train mode is required.
		if (StepGaussianSplatting->bTrain4D)
		{
			CheckPath(S->Get4DGSRepoDir(), true, TEXT("4DGS 训练仓库目录"));
		}
		else
		{
			CheckPath(S->Get3DGSRepoDir(), true, TEXT("3DGS 训练仓库目录"));
		}
	}

	if (Missing.Num() > 0)
	{
		const FString Msg = FString::Join(Missing, TEXT("\n"));
		FNotificationInfo NotifyInfo(FText::FromString(Msg));
		NotifyInfo.ExpireDuration = 8.0f;
		NotifyInfo.bUseSuccessFailIcons = true;
		if (TSharedPtr<SNotificationItem> N = FSlateNotificationManager::Get().AddNotification(NotifyInfo))
		{
			N->SetCompletionState(SNotificationItem::CS_Fail);
		}
		return false;
	}

	CurrentTask = Step;
	return true;
}

void SOpenSplat4DEdModePanel::OnTaskFinished(UOpenSplat4DStepBase* Step)
{
	CurrentTask = nullptr;
	// A capture just created / updated a CaptureSet asset: refresh the picker so
	// the new asset is shown and selected.
	if (Step == StepCapture.Get())
	{
		Rebuild();
	}
}

EVisibility SOpenSplat4DEdModePanel::OnGetProgressBarVisibility() const
{
	return CurrentTask ? EVisibility::Visible : EVisibility::Hidden;
}

TOptional<float> SOpenSplat4DEdModePanel::OnGetProgressPercent() const
{
	return CurrentTask ? CurrentTask->TaskProgressPercent : 0.f;
}

void SOpenSplat4DEdModePanel::OnClicked_Browse()
{
	if (!IFileManager::Get().DirectoryExists(*WorkDir))
	{
		IFileManager::Get().MakeDirectory(*WorkDir, true);
	}
	if (IFileManager::Get().DirectoryExists(*WorkDir))
	{
		FPlatformProcess::ExploreFolder(*WorkDir);
	}
}

FReply SOpenSplat4DEdModePanel::OnClicked_Cancel()
{
	if (CurrentTask)
	{
		CurrentTask->bRequestCancelTask = true;
	}
	return FReply::Handled();
}

// ===========================================================================
// Capture-set asset selector (native UOpenSplat4DCaptureSet DataAsset)
// ===========================================================================
FString SOpenSplat4DEdModePanel::GetCaptureSetObjectPath() const
{
	if (StepCapture && StepCapture->CaptureSetAsset)
	{
		return StepCapture->CaptureSetAsset->GetPathName();
	}
	return FString();
}

void SOpenSplat4DEdModePanel::OnCaptureSetChanged(const FAssetData& AssetData)
{
	UOpenSplat4DCaptureSet* Set = Cast<UOpenSplat4DCaptureSet>(AssetData.GetAsset());
	if (!Set)
	{
		return;
	}
	StepCapture->CaptureSetAsset = Set;
	WorkDir = Set->WorkDirectory;
	StepCapture->SetWorkDir(WorkDir);
	StepSparseReconstruction->SetWorkDir(WorkDir);
	StepGaussianSplatting->SetWorkDir(WorkDir);
	// Keep the reconstruction / training target in sync with the panel picker.
	StepSparseReconstruction->TargetCaptureSet = Set;
	StepGaussianSplatting->TargetCaptureSet = Set;
}

FReply SOpenSplat4DEdModePanel::OnNewCaptureSetClicked()
{
	const FString SetName = OpenSplat4DBuildCaptureSetName();
	const FString SetWorkDir = BaseWorkDir / SetName;
	UOpenSplat4DCaptureSet* Set = OpenSplat4DCreateCaptureSet(SetName, SetWorkDir);
	if (!Set)
	{
		FMessageDialog::Open(EAppMsgType::Ok, OS4D_TEXT("Failed to create capture set asset."));
		return FReply::Handled();
	}
	StepCapture->CaptureSetAsset = Set;
	WorkDir = SetWorkDir;
	StepCapture->SetWorkDir(WorkDir);
	StepSparseReconstruction->SetWorkDir(WorkDir);
	StepGaussianSplatting->SetWorkDir(WorkDir);
	// The newly created capture set becomes the active reconstruction / training target.
	StepSparseReconstruction->TargetCaptureSet = Set;
	StepGaussianSplatting->TargetCaptureSet = Set;
	Rebuild();
	return FReply::Handled();
}

FReply SOpenSplat4DEdModePanel::OnOpenCaptureSetClicked()
{
	UOpenSplat4DCaptureSet* Set = StepCapture ? StepCapture->CaptureSetAsset : nullptr;
	if (!Set)
	{
		FMessageDialog::Open(EAppMsgType::Ok,
			FText::FromString(TEXT("请先在顶部的【捕获组】下拉中选择一个索引资产，再点击【打开索引】。")));
		return FReply::Handled();
	}
	if (GEditor)
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Set);
	}
	return FReply::Handled();
}

// ===========================================================================
// Usage tab
// ===========================================================================
TSharedRef<SWidget> SOpenSplat4DEdModePanel::BuildUsageTab()
{
	// Helper: a collapsible help section with a Chinese explanation.
	auto MakeHelpSection = [](const FString& Title, const FString& Body) -> TSharedRef<SWidget>
	{
		return SNew(SExpandableArea)
			.AreaTitle(FText::FromString(Title))
			.InitiallyCollapsed(false)
			.HeaderPadding(FMargin(4.f, 2.f))
			.BodyContent()
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Margin(FMargin(8.f, 4.f, 8.f, 8.f))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(FText::FromString(Body))
			];
	};

	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		.Padding(8.f)
		[
			SNew(SVerticalBox)

			// ---- Intro ----------------------------------------------------
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f, 4.f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(FText::FromString(TEXT(
					"OpenSplat4D 把真实物体 / 人物变成可在 UE 里实时渲染的高斯泼溅模型。\n"
					"顶部三个标签页【捕获】→【稀疏重建】→【高斯训练】是按顺序执行的三步；本页是用法说明；【设置】里配置 python / colmap / 训练仓库路径。\n"
					"先用视口选中要处理的角色，再开始。"))
				)
				.ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.9f, 1.0f)))
			]

			// ---- Full pipeline --------------------------------------------
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f, 4.f)
			[
				MakeHelpSection(TEXT("完整流程：训练一个真正的高斯模型（推荐）"), TEXT(
					"第 1 步 · 捕获：在【捕获】标签页选中要扫描的角色（视口里点选 Actor），设置相机阵列行列数、捕获距离等，点【捕获】。"
					"引擎从多个角度自动截图，保存到工作目录的 images/（彩色图）、masks/（遮罩）、depths/（深度）、cameras.txt（相机位姿）。\n"
					"第 2 步 · 稀疏重建：切到【稀疏重建】标签页点【稀疏重建】。COLMAP 会做特征提取→匹配→映射→对齐，把图片变成稀疏点云和相机位姿（这是训练的输入）。"
					"可用【查看 Colmap 结果】检查质量，用【编辑 Colmap 配置】调参。\n"
					"第 3 步 · 高斯训练：切到【高斯训练】标签页，按需调整迭代次数等，点【训练】。网络把稀疏重建拟合为大量高斯基元（默认约 7000 次迭代，耗时取决于显卡与图片数）。"
					"完成后自动【重新加载】，视口即可看到真正的高斯模型。"))
			]

			// ---- Quick scan -------------------------------------------------
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f, 4.f)
			[
				MakeHelpSection(TEXT("快速预览：扫描到点云（无需 python / colmap）"), TEXT(
					"在【捕获】标签页点【扫描到点云】，或本页下方【扫描选中对象 → 点云】：直接把选中几何体采样成点云，立刻能在视口渲染，用来快速看形状。\n"
					"注意：它【不生成图片】，也不能拿去训练，只是编辑器内的即时预览。想要可训练 / 可导出的最终模型，请走上面的完整流程（捕获→稀疏重建→高斯训练）。"))
			]

			// ---- Where are the images -------------------------------------
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f, 4.f)
			[
				MakeHelpSection(TEXT("我捕获的图片去哪了？"), TEXT(
					"全部保存在插件工作目录：\n"
					"  <你的项目>/Intermediate/OpenSplat4D/OpenSplat4DEditor/\n"
					"其下包含 images、masks、depths、cameras.txt。\n"
					"打开方式：点【捕获】标签页顶部的文件夹图标（浏览工作目录）即可一键打开；或在文件管理器地址栏粘贴该路径。\n"
					"也可在 Output Log 搜索 \"Capture Finished\"，日志会打印每张图片的确切完整路径。"))
			]

			// ---- Dependencies ----------------------------------------------
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f, 4.f)
			[
				MakeHelpSection(TEXT("前置依赖（必须先在【设置】配置）"), TEXT(
					"· Python：需安装 Python 3，并在【设置】指定 python 可执行文件路径（留空则用系统 PATH 里的 python）。\n"
					"· COLMAP：需安装 COLMAP，并在【设置】指定 colmap 可执行文件（用于稀疏重建与查看结果）。\n"
					"· 训练仓库：需填写 3DGS（以及可选的 4DGS）仓库路径，指向你本地的 gaussian-splatting / 4d-gaussian-splatting 代码目录（【训练】会调用其中的训练脚本）。\n"
					"未配置或路径错误时，对应步骤会在 Output Log 报错。"))
			]

			// ---- FAQ --------------------------------------------------------
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f, 4.f)
			[
				MakeHelpSection(TEXT("常见问题"), TEXT(
					"· 第二步（稀疏重建 / 训练）报错或没反应：通常是外部命令路径含空格（如装在 \"C:/Program Files\"、项目在 \"Unreal Projects\" 下）或 python / colmap 未安装。空格问题现已自动处理，请确保依赖已安装且【设置】路径正确。\n"
					"· 模型是一坨巨大透明的东西 / 帧率极低：这是扫描点云尺寸过大（每个点被画成巨大面片，满屏重复绘制）。在视口选中该点云 Actor → Details → OpenSplat4D → 把 \"Splat Scale\" 调到 0.05~0.1 即可缩小；或重新扫描（默认点尺寸已调小）。真正训练出的高斯模型尺度较小，不会出现此问题。\n"
					"· 【编辑 Colmap 配置】/【查看 Colmap 结果】点了没反应：同外部命令问题，现已修复；若仍无反应请检查 Output Log。"))
			]

			// ---- Create from scene (scan) ----------------------------------
			+ SVerticalBox::Slot().AutoHeight().Padding(8.f, 8.f, 8.f, 2.f)
			[
				SNew(SBorder)
				.BorderBackgroundColor(FSlateColor(FLinearColor(0.12f, 0.1f, 0.18f)))
				.Padding(8.f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).Text(OS4D_TEXT("0. Create from Scene (Scan)"))
					]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(2.f)
					[
						SNew(SCheckBox)
						.IsChecked(this, &SOpenSplat4DEdModePanel::IsCameraDepth)
						.OnCheckStateChanged(this, &SOpenSplat4DEdModePanel::OnCameraDepthChanged)
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2.f)
					[
						SNew(STextBlock).Text(OS4D_TEXT("Camera Depth Scan (off = Mesh Surface)"))
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(2.f)
					[ SNew(STextBlock).Text(OS4D_TEXT("Density")) ]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2.f)
					[
						SNew(SNumericEntryBox<int32>)
						.Value(this, &SOpenSplat4DEdModePanel::GetScanDensity)
						.OnValueChanged(this, &SOpenSplat4DEdModePanel::OnScanDensityChanged)
						.MinValue(1).MaxValue(64)
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2.f)
					[ SNew(STextBlock).Text(OS4D_TEXT("Point Size")) ]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2.f)
					[
						SNew(SNumericEntryBox<float>)
						.Value(this, &SOpenSplat4DEdModePanel::GetScanPointScale)
						.OnValueChanged(this, &SOpenSplat4DEdModePanel::OnScanPointScaleChanged)
						.MinValue(0.1f).MaxValue(500.f)
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
				[
					SNew(SButton)
					.Text(OS4D_TEXT("Scan Selected -> Point Cloud"))
					.OnClicked(this, &SOpenSplat4DEdModePanel::OnScanClicked)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(OS4D_TEXT("Select one or more actors in the viewport, then scan. Mesh Surface samples static-mesh geometry directly; Camera Depth places a camera rig and reconstructs depth. No colmap / python needed -- you can create a point cloud and test rendering immediately."))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
		]

		// ---- Import --------------------------------------------------------
		+ SVerticalBox::Slot().AutoHeight().Padding(8.f, 2.f)
		[
			SNew(SBorder)
			.BorderBackgroundColor(FSlateColor(FLinearColor(0.1f, 0.1f, 0.12f)))
			.Padding(8.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(OS4D_TEXT("1. Import"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
				[
					SNew(SButton)
					.Text(OS4D_TEXT("Import .ply / .4dgs ..."))
					.OnClicked(this, &SOpenSplat4DEdModePanel::OnImportClicked)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(OS4D_TEXT("Or drag a .ply / .4dgs file into the Content Browser. The asset is created under /Game/OpenSplat4D/."))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
		]

		// ---- Add to scene --------------------------------------------------
		+ SVerticalBox::Slot().AutoHeight().Padding(8.f, 2.f)
		[
			SNew(SBorder)
			.BorderBackgroundColor(FSlateColor(FLinearColor(0.1f, 0.1f, 0.12f)))
			.Padding(8.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(OS4D_TEXT("2. Place in level"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
				[
					SNew(SObjectPropertyEntryBox)
					.AllowedClass(UOpenSplat4DPointCloud::StaticClass())
					.OnObjectChanged(this, &SOpenSplat4DEdModePanel::OnCloudPicked)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
				[
					SNew(SButton)
					.Text(OS4D_TEXT("Spawn Actor in Level"))
					.OnClicked(this, &SOpenSplat4DEdModePanel::OnAddToSceneClicked)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(OS4D_TEXT("Or drag the asset from the Content Browser into the viewport. You can also right-click the asset -> 'Create OpenSplat4D Actor'."))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
		]

		// ---- Playback ------------------------------------------------------
		+ SVerticalBox::Slot().AutoHeight().Padding(8.f, 2.f)
		[
			SNew(SBorder)
			.BorderBackgroundColor(FSlateColor(FLinearColor(0.1f, 0.1f, 0.12f)))
			.Padding(8.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(OS4D_TEXT("3. 4D Playback"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
				[
					SNew(SButton)
					.Text(OS4D_TEXT("Use Selected Actor"))
					.OnClicked(this, &SOpenSplat4DEdModePanel::OnUseSelectedClicked)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(2.f)
					[
						SNew(SButton).Text(OS4D_TEXT("Play")).OnClicked(this, &SOpenSplat4DEdModePanel::OnPlayClicked)
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2.f)
					[
						SNew(SButton).Text(OS4D_TEXT("Pause")).OnClicked(this, &SOpenSplat4DEdModePanel::OnPauseClicked)
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2.f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock).Text(OS4D_TEXT("Time")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SNumericEntryBox<float>)
							.Value(this, &SOpenSplat4DEdModePanel::GetTime)
							.OnValueChanged(this, &SOpenSplat4DEdModePanel::OnTimeChanged)
							.MinValue(-10.f).MaxValue(10.f)
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2.f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock).Text(OS4D_TEXT("Speed")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SNumericEntryBox<float>)
							.Value(this, &SOpenSplat4DEdModePanel::GetPlayRate)
							.OnValueChanged(this, &SOpenSplat4DEdModePanel::OnPlayRateChanged)
							.MinValue(0.f).MaxValue(10.f)
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2.f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock).Text(OS4D_TEXT("Loop")).ColorAndOpacity(FSlateColor::UseSubduedForeground())
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SCheckBox)
							.IsChecked(this, &SOpenSplat4DEdModePanel::IsLooping)
							.OnCheckStateChanged(this, &SOpenSplat4DEdModePanel::OnLoopingChanged)
						]
					]
				]
			]
		]
		]
	;
}

// ---------------------------------------------------------------------------
void SOpenSplat4DEdModePanel::OnCloudPicked(const FAssetData& AssetData)
{
	PickedCloud = Cast<UOpenSplat4DPointCloud>(AssetData.GetAsset());
}

FReply SOpenSplat4DEdModePanel::OnImportClicked()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		return FReply::Handled();
	}

	TArray<FString> OutFiles;
	const bool bOpened = DesktopPlatform->OpenFileDialog(
		nullptr,
		TEXT("Import OpenSplat4D Point Cloud"),
		TEXT(""),
		TEXT(""),
		TEXT("PLY / 4DGS (*.ply;*.4dgs)|*.ply;*.4dgs"),
		EFileDialogFlags::None,
		OutFiles);

	if (!bOpened || OutFiles.Num() == 0)
	{
		return FReply::Handled();
	}

	const FString File = OutFiles[0];
	const FString AssetName = FPaths::GetBaseFilename(File);
	const FString PackageName = FString::Printf(TEXT("/Game/OpenSplat4D/%s"), *AssetName);

	UPackage* Pkg = CreatePackage(*PackageName);
	UOpenSplat4DPointCloud* Cloud = NewObject<UOpenSplat4DPointCloud>(
		Pkg, UOpenSplat4DPointCloud::StaticClass(), *AssetName, RF_Public | RF_Standalone);

	const FString Ext = FPaths::GetExtension(File).ToLower();
	bool bOk = false;
	if (Ext == TEXT("ply"))
	{
		Cloud->LoadFromFile(File);
		bOk = Cloud->GetPointCount() > 0;
	}
	else if (Ext == TEXT("4dgs"))
	{
		bOk = Cloud->LoadFrom4DGS(File);
	}

	if (!bOk)
	{
		FMessageDialog::Open(EAppMsgType::Ok,
			FText::Format(OpenSplat4DLocalization::GetText(TEXT("Failed to import '{0}'. Not a valid .ply / .4dgs.")), FText::FromString(File)));
		return FReply::Handled();
	}

	Cloud->SourceFilePath = File;
	Cloud->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Cloud);

	FPackagePath PkgPath = FPackagePath::FromPackageNameChecked(Pkg->GetName());
	const FString LocalPath = PkgPath.GetLocalFullPath();
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	SaveArgs.bWarnOfLongFilename = false;
	UPackage::SavePackage(Pkg, Cloud, *LocalPath, SaveArgs);

	TArray<UObject*> ObjectsToSync;
	ObjectsToSync.Add(Cloud);
	GEditor->SyncBrowserToObjects(ObjectsToSync);

	return FReply::Handled();
}

FReply SOpenSplat4DEdModePanel::OnAddToSceneClicked()
{
	UOpenSplat4DPointCloud* Cloud = PickedCloud.Get();
	if (!Cloud)
	{
		FMessageDialog::Open(EAppMsgType::Ok, OS4D_TEXT("Pick a point cloud asset first."));
		return FReply::Handled();
	}

	UWorld* World = GetEditorWorld();
	if (!World)
	{
		return FReply::Handled();
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.bNoFail = true;
	AGaussianSplatActor* Actor = World->SpawnActor<AGaussianSplatActor>(AGaussianSplatActor::StaticClass(), SpawnParams);
	if (Actor)
	{
		Actor->GaussianSplatComponent->SetSplatAsset(Cloud);
		CurrentActor = Actor;
		GEditor->SelectActor(Actor, true, true);
	}

	return FReply::Handled();
}

UWorld* SOpenSplat4DEdModePanel::GetEditorWorld() const
{
	if (GEditor)
	{
		return GEditor->GetEditorWorldContext().World();
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
FReply SOpenSplat4DEdModePanel::OnUseSelectedClicked()
{
	if (GEditor)
	{
        USelection* Selection = GEditor->GetSelectedActors();
        for (int32 SelIdx = 0; SelIdx < Selection->Num(); ++SelIdx)
        {
            if (AGaussianSplatActor* Actor = Cast<AGaussianSplatActor>(Selection->GetSelectedObject(SelIdx)))
            {
                CurrentActor = Actor;
                break;
            }
        }
	}
	return FReply::Handled();
}

FReply SOpenSplat4DEdModePanel::OnPlayClicked()
{
	if (CurrentActor.IsValid()) CurrentActor->Play();
	return FReply::Handled();
}

FReply SOpenSplat4DEdModePanel::OnPauseClicked()
{
	if (CurrentActor.IsValid()) CurrentActor->Pause();
	return FReply::Handled();
}

void SOpenSplat4DEdModePanel::OnTimeChanged(float Value)
{
	if (CurrentActor.IsValid()) CurrentActor->Seek(Value);
}

TOptional<float> SOpenSplat4DEdModePanel::GetTime() const
{
	return TOptional<float>(CurrentActor.IsValid() ? CurrentActor->CurrentTime : 0.f);
}

void SOpenSplat4DEdModePanel::OnPlayRateChanged(float Value)
{
	if (CurrentActor.IsValid()) CurrentActor->PlayRate = Value;
}

TOptional<float> SOpenSplat4DEdModePanel::GetPlayRate() const
{
	return TOptional<float>(CurrentActor.IsValid() ? CurrentActor->PlayRate : 1.f);
}

ECheckBoxState SOpenSplat4DEdModePanel::IsLooping() const
{
	return (CurrentActor.IsValid() && CurrentActor->bLooping) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SOpenSplat4DEdModePanel::OnLoopingChanged(ECheckBoxState State)
{
	if (CurrentActor.IsValid()) CurrentActor->bLooping = (State == ECheckBoxState::Checked);
}

// ---------------------------------------------------------------------------
FReply SOpenSplat4DEdModePanel::OnScanClicked()
{
	UWorld* World = GetEditorWorld();
	if (!World)
	{
		return FReply::Handled();
	}

	TArray<AActor*> Actors;
	if (GEditor)
	{
        USelection* SelectedActors = GEditor->GetSelectedActors();
        for (int32 SelIdx = 0; SelIdx < SelectedActors->Num(); ++SelIdx)
        {
            if (AActor* A = Cast<AActor>(SelectedActors->GetSelectedObject(SelIdx)))
            {
                Actors.AddUnique(A);
            }
        }
	}

	const EOpenSplat4DScanMode Mode = bCameraDepth ? EOpenSplat4DScanMode::CameraDepth : EOpenSplat4DScanMode::MeshSurface;
	const int32 Density = ScanDensity.Get(8);
	const float Scale = ScanPointScale.Get(3.f);

	UOpenSplat4DPointCloud* Cloud = UOpenSplat4DEditorLibrary::CreatePointCloudFromActors(
		World, Actors, Mode, Density, Scale, TEXT("/Game/OpenSplat4D"));

	if (!Cloud)
	{
		FMessageDialog::Open(EAppMsgType::Ok, OS4D_TEXT("Scan produced no points. Select static-mesh actor(s) for Mesh Surface, or ensure visible geometry for Camera Depth."));
		return FReply::Handled();
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.bNoFail = true;
	if (AGaussianSplatActor* Actor = World->SpawnActor<AGaussianSplatActor>(
		AGaussianSplatActor::StaticClass(), SpawnParams))
	{
		Actor->GaussianSplatComponent->SetSplatAsset(Cloud);
		CurrentActor = Actor;
		if (GEditor)
		{
			GEditor->SelectActor(Actor, true, true);
			TArray<UObject*> ObjectsToSync;
			ObjectsToSync.Add(Cloud);
			GEditor->SyncBrowserToObjects(ObjectsToSync);
		}
	}
	return FReply::Handled();
}

ECheckBoxState SOpenSplat4DEdModePanel::IsCameraDepth() const
{
	return bCameraDepth ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}

void SOpenSplat4DEdModePanel::OnCameraDepthChanged(ECheckBoxState State)
{
	bCameraDepth = (State == ECheckBoxState::Checked);
}

TOptional<int32> SOpenSplat4DEdModePanel::GetScanDensity() const
{
	return ScanDensity.IsSet() ? ScanDensity : TOptional<int32>(8);
}

void SOpenSplat4DEdModePanel::OnScanDensityChanged(int32 Value)
{
	ScanDensity = Value;
}

TOptional<float> SOpenSplat4DEdModePanel::GetScanPointScale() const
{
	return ScanPointScale.IsSet() ? ScanPointScale : TOptional<float>(3.f);
}

void SOpenSplat4DEdModePanel::OnScanPointScaleChanged(float Value)
{
	ScanPointScale = Value;
}

#undef LOCTEXT_NAMESPACE
