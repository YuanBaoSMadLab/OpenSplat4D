// Copyright OpenSplat4D 2026 All Rights Reserved.
#include "OpenSplat4DEdModePanel.h"
#include "OpenSplat4DPointCloud.h"
#include "OpenSplat4DPointCloudActor.h"
#include "NanoGSGaussianSplatComponent.h"
#include "OpenSplat4DEditorLibrary.h"
#include "OpenSplat4DSettings.h"
#include "OpenSplat4DLocalization.h"
#include "GaussianSplatAsset.h"
#include "PLYFileReader.h"
#include "GaussianDataTypes.h"
#include "HAL/FileManager.h"

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
#include "Widgets/Input/SHyperlink.h"
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
#include "HAL/PlatformProcess.h"
#include "UObject/SavePackage.h"

#define LOCTEXT_NAMESPACE "OpenSplat4D"

SOpenSplat4DEdModePanel::~SOpenSplat4DEdModePanel()
{
	OpenSplat4DLocalization::OnUILanguageChanged().RemoveAll(this);
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
	//      when the editor language changes) --------------------------------
	ChildSlot[ BuildContent() ];

	// Rebuild this panel in place whenever the editor language changes.
	OpenSplat4DLocalization::OnUILanguageChanged().AddSP(this, &SOpenSplat4DEdModePanel::Rebuild);

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
				.Text(OS4D_TEXT("Capture Set:"))
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
				.Text(OS4D_TEXT("New Capture Set"))
				.OnClicked(this, &SOpenSplat4DEdModePanel::OnNewCaptureSetClicked)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4, 0, 0, 0)
			[
				SNew(SButton)
				.Text(OS4D_TEXT("Open Index"))
				.ToolTipText(OS4D_TEXT("Open the currently selected capture-set (index) asset to view / edit the file locations and ranges it records."))
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
	// Rebuild from scratch with the new language. Tab selection resets to 0;
	// acceptable for a language switch.
	ChildSlot[ BuildContent() ];
	OnTabChanged(0);
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
		FNotificationInfo NotifyInfo(OS4D_TEXT("Execution failed: a task is already running. Wait for it to finish, or cancel it first."));
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
	//   (1) user-specified path in settings  ->  (2) an optional local ThirdParty
	//       copy dropped in by the user (none of it ships with the plugin)
	//       ->  (3) nothing.
	// If neither (1) nor (2) resolves, remind the user to specify a path before
	// running the step, instead of failing deep inside an external process.
	const UOpenSplat4DSettings* S = GetDefault<UOpenSplat4DSettings>();
	TArray<FText> Missing;
	auto CheckPath = [&](const FString& Resolved, bool bIsDir, const FText& Label)
	{
		if (Resolved.IsEmpty())
		{
			Missing.Add(FText::Format(
				OS4D_TEXT("{0}: not found in the optional local ThirdParty folder and not specified in Project Settings > OpenSplat4D. The plugin does not ship these tools: set the path in Settings, or drop your own copy into the plugin's ThirdParty folder."),
				Label));
		}
		else if (bIsDir ? !FPaths::DirectoryExists(Resolved) : !FPaths::FileExists(Resolved))
		{
			Missing.Add(FText::Format(
				OS4D_TEXT("{0}: the specified path does not exist or is invalid: {1}"),
				Label, FText::FromString(Resolved)));
		}
	};

	if (Step == StepSparseReconstruction.Get() || Step == StepGaussianSplatting.Get())
	{
		CheckPath(S->GetColmapExecutablePath(), false, OS4D_TEXT("COLMAP executable"));
		// Require an explicit target capture set (index asset) so the step never
		// silently reconstructs the wrong data.
		UOpenSplat4DCaptureSet* Target = (Step == StepSparseReconstruction.Get())
			? StepSparseReconstruction->TargetCaptureSet
			: StepGaussianSplatting->TargetCaptureSet;
		if (!Target)
		{
			Missing.Add(OS4D_TEXT("No target capture set (index asset) selected: pick the capture set to reconstruct / train in 'Target Capture Set (Index Asset)' for this step (you can also pick it in the 'Capture Set' drop-down at the top of the panel -- the two stay in sync). The step is skipped when no target is selected, to avoid reconstructing the wrong asset."));
		}
	}
	if (Step == StepGaussianSplatting.Get())
	{
		// Only the repo matching the current train mode is required.
		if (StepGaussianSplatting->bTrain4D)
		{
			CheckPath(S->Get4DGSRepoDir(), true, OS4D_TEXT("4DGS training repository directory"));
		}
		else
		{
			CheckPath(S->Get3DGSRepoDir(), true, OS4D_TEXT("3DGS training repository directory"));
		}
	}

	if (Missing.Num() > 0)
	{
		FNotificationInfo NotifyInfo(FText::Join(FText::FromString(TEXT("\n")), Missing));
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
		FMessageDialog::Open(EAppMsgType::Ok, OS4D_TEXT("Failed to create the capture-set asset."));
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
			OS4D_TEXT("Pick an index asset in the 'Capture Set' drop-down at the top of the panel first, then click 'Open Index'."));
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
	// Helper: a collapsible help section (titles / bodies follow the editor language).
	auto MakeHelpSection = [](const FText& Title, const FText& Body) -> TSharedRef<SWidget>
	{
		return SNew(SExpandableArea)
			.AreaTitle(Title)
			.InitiallyCollapsed(false)
			.HeaderPadding(FMargin(4.f, 2.f))
			.BodyContent()
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Margin(FMargin(8.f, 4.f, 8.f, 8.f))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(Body)
			];
	};

	// Helper: a labelled external link row. Clicking opens the URL in the system browser.
	auto MakeLinkRow = [](const FText& Label, const FString& Url) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.f, 1.f, 6.f, 1.f)
			[
				SNew(STextBlock).Text(Label)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(100)
			.VAlign(VAlign_Center)
			[
				SNew(SHyperlink)
				.Text(FText::FromString(Url))
				.OnNavigate(FSimpleDelegate::CreateLambda([Url]()
				{
					FPlatformProcess::LaunchURL(*Url, nullptr, nullptr);
				}))
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
				.Text(OS4D_TEXT(
					"OpenSplat4D turns real objects / people into gaussian splat models that render in real time inside UE.\n"
					"The three tabs at the top -- Capture -> Sparse -> Gaussian -- are the three steps, executed in order. This page explains the workflow; the Settings tab configures the python / colmap / training-repository paths.\n"
					"Select the actor you want to process in the viewport first, then start."))
				.ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.9f, 1.0f)))
			]

			// ---- Full pipeline --------------------------------------------
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f, 4.f)
			[
				MakeHelpSection(OS4D_TEXT("Full pipeline: train a real gaussian model (recommended)"), OS4D_TEXT(
					"Step 1 - Capture: on the Capture tab, select the actor to scan (click it in the viewport), set the camera grid rows / columns and capture distance, then click Capture. "
					"The engine screenshots the actor from many angles into the working directory: images/ (colour), masks/, depths/ and cameras.txt (camera poses).\n"
					"Step 2 - Sparse: switch to the Sparse tab and click Sparse. COLMAP runs feature extraction -> matching -> mapping -> alignment, turning the images into a sparse point cloud plus camera poses (the input for training). "
					"Use 'View Colmap Result' to check the quality and 'Edit Colmap Config' to tune the parameters.\n"
					"Step 3 - Gaussian: switch to the Gaussian tab, adjust the iteration count if needed, then click Train. The network fits the sparse reconstruction with a large number of gaussian primitives (about 7000 iterations by default; the runtime depends on your GPU and image count). "
					"On completion it automatically reloads, and the real gaussian model appears in the viewport."))
			]

			// ---- Quick scan -------------------------------------------------
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f, 4.f)
			[
				MakeHelpSection(OS4D_TEXT("Quick preview: scan to a point cloud (no python / colmap needed)"), OS4D_TEXT(
					"On the Capture tab click 'Scan to Point Cloud', or use 'Scan Selected -> Point Cloud' further down this page: the selected geometry is sampled straight into a point cloud that can be rendered in the viewport immediately, for a quick shape check.\n"
					"Note: it does NOT produce images and cannot be used for training -- it is an in-editor instant preview only. For a trainable / exportable final model, follow the full pipeline above (Capture -> Sparse -> Gaussian)."))
			]

			// ---- Where are the images -------------------------------------
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f, 4.f)
			[
				MakeHelpSection(OS4D_TEXT("Where did my captured images go?"), OS4D_TEXT(
					"Everything is stored in the plugin working directory:\n"
					"  <your project>/Intermediate/OpenSplat4D/OpenSplat4DEditor/\n"
					"containing images, masks, depths and cameras.txt.\n"
					"To open it: click the folder icon at the top of the Capture tab (browse working directory), or paste that path into your file manager.\n"
					"You can also search the Output Log for \"Capture Finished\" -- the log prints the exact full path of every image."))
			]

			// ---- Dependencies ----------------------------------------------
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f, 4.f)
			[
				MakeHelpSection(OS4D_TEXT("Dependencies (configure these in Settings first)"), OS4D_TEXT(
					"- Python: install Python 3 and set the python executable path in Settings (leave it empty to use the python on your PATH).\n"
					"- COLMAP: install COLMAP and set the colmap executable in Settings (used for the sparse reconstruction and for viewing its result).\n"
					"- Training repositories: set the 3DGS (and optionally 4DGS) repository path to your local gaussian-splatting / 4d-gaussian-splatting source folder (Train calls the training script inside it).\n"
					"When a path is missing or wrong the corresponding step reports an error in the Output Log."))
			]

			// ---- FAQ --------------------------------------------------------
			+ SVerticalBox::Slot().AutoHeight().Padding(2.f, 4.f)
			[
				MakeHelpSection(OS4D_TEXT("FAQ"), OS4D_TEXT(
					"- Step 2 (sparse reconstruction / training) errors out or does nothing: this is usually an external command path containing spaces (e.g. installed under \"C:/Program Files\", project under \"Unreal Projects\") or python / colmap not being installed. Spaces are now handled automatically -- make sure the dependencies are installed and the Settings paths are correct.\n"
					"- The model is a huge transparent blob / the frame rate is terrible: the scanned point cloud is oversized (every point is drawn as an enormous quad, redrawn over the whole screen). Select the point-cloud actor in the viewport -> Details -> OpenSplat4D -> lower \"Splat Scale\" to 0.05~0.1, or rescan (the default point size is now smaller). Real trained gaussian models have a much smaller scale and do not show this problem.\n"
					"- 'Edit Colmap Config' / 'View Colmap Result' do nothing: same external-command issue, now fixed; if it still does nothing, check the Output Log."))
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

		// ---- About / Support -----------------------------------------------
		+ SVerticalBox::Slot().AutoHeight().Padding(8.f, 8.f, 8.f, 4.f)
		[
			SNew(SBorder)
			.BorderBackgroundColor(FSlateColor(FLinearColor(0.14f, 0.12f, 0.18f)))
			.Padding(8.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Text(OS4D_TEXT("About / Support Us"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(OS4D_TEXT("OpenSplat4D is developed and open-sourced by YuanBaoSMadLab under Apache-2.0. If it helps you, please consider supporting future development:"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
				[
					MakeLinkRow(OS4D_TEXT("Afdian (China)"), TEXT("https://afdian.com/a/YuanBaoSMadLab"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
				[
					MakeLinkRow(OS4D_TEXT("Quark Drive (China mirror, code UZ2J)"), TEXT("https://pan.quark.cn/s/e85b7b7660f1?pwd=UZ2J"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
				[
					MakeLinkRow(OS4D_TEXT("Project home"), TEXT("https://github.com/YuanBaoSMadLab/OpenSplat4D"))
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
		OS4D_TEXT("Import OpenSplat4D Point Cloud").ToString(),
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
	const FString PackageName = FString::Printf(TEXT("/Game/OpenSplat4D/Models/%s"), *AssetName);

	UPackage* Pkg = CreatePackage(*PackageName);
	UGaussianSplatAsset* SplatAsset = NewObject<UGaussianSplatAsset>(
		Pkg, UGaussianSplatAsset::StaticClass(), *AssetName, RF_Public | RF_Standalone);

	TArray<FGaussianSplatData> SplatData;
	FString ErrorMessage;
	int32 DetectedSHBands = 0;
	bool bOk = FPLYFileReader::ReadPLYFile(File, SplatData, ErrorMessage, &DetectedSHBands);

	if (!bOk)
	{
		FMessageDialog::Open(EAppMsgType::Ok,
			FText::Format(OS4D_TEXT("Failed to import '{0}'. Not a valid .ply / .4dgs."), FText::FromString(File)));
		return FReply::Handled();
	}

	SplatAsset->SourceFilePath = File;
	SplatAsset->SHBands = DetectedSHBands;
	SplatAsset->InitializeFromSplatData(SplatData, EGaussianQualityLevel::High);
	SplatAsset->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(SplatAsset);

	FString LocalPath;
	if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, LocalPath, FPackageName::GetAssetPackageExtension()))
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(LocalPath), true);
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		SaveArgs.bWarnOfLongFilename = false;
		UPackage::SavePackage(Pkg, SplatAsset, *LocalPath, SaveArgs);
	}

	TArray<UObject*> ObjectsToSync;
	ObjectsToSync.Add(SplatAsset);
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
	AOpenSplat4DPointCloudActor* Actor = World->SpawnActor<AOpenSplat4DPointCloudActor>(AOpenSplat4DPointCloudActor::StaticClass(), SpawnParams);
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
            if (AOpenSplat4DPointCloudActor* Actor = Cast<AOpenSplat4DPointCloudActor>(Selection->GetSelectedObject(SelIdx)))
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
	if (AOpenSplat4DPointCloudActor* Actor = World->SpawnActor<AOpenSplat4DPointCloudActor>(
		AOpenSplat4DPointCloudActor::StaticClass(), SpawnParams))
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
