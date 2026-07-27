#pragma once

#include "CoreMinimal.h"
#include "UObject/GCObject.h"
#include "Widgets/SCompoundWidget.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/Optional.h"
#include "OpenSplat4DPointCloud.h"
#include "OpenSplat4DPointCloudActor.h"
#include "OpenSplat4DStep.h"
#include "PropertyCustomizationHelpers.h"

class IDetailsView;
class SWidgetSwitcher;

/**
 * The OpenSplat4D editor-mode panel.
 *
 * Ported from the reference plugin's SGaussianSplattingEdModePanel: a tabbed
 * IDetailsView driving the capture -> sparse reconstruction -> gaussian
 * training pipeline (Capture / Sparse / Gaussian / Settings), complete with a
 * task progress bar + cancel button.
 *
 * A fifth "Usage" tab keeps OpenSplat4D's own clear-page workflow:
 *   - Scan level geometry into a point cloud (no colmap/python needed).
 *   - Import a .ply (3DGS) or .4dgs (native) point cloud.
 *   - Drop a cloud into the level and drive 4D playback.
 */
class SOpenSplat4DEdModePanel : public SCompoundWidget, public FGCObject
{
public:
	SLATE_BEGIN_ARGS(SOpenSplat4DEdModePanel) {}
	SLATE_END_ARGS()

	~SOpenSplat4DEdModePanel();
	void Construct(const FArguments& InArgs);

protected:
	// ---- FGCObject ----------------------------------------------------------
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("OpenSplat4DEditor"); }

	// ---- Pipeline tabs ------------------------------------------------------
	void OnTabChanged(int32 TabIndex);
	void OnTabPropertyChanged(const FPropertyChangedEvent& ChangedEvent);
	bool OnRequestTaskStart(UOpenSplat4DStepBase* Step);
	void OnTaskFinished(UOpenSplat4DStepBase* Step);
	EVisibility OnGetProgressBarVisibility() const;
	TOptional<float> OnGetProgressPercent() const;
	void OnClicked_Browse();
	FReply OnClicked_Cancel();
	TSharedRef<SWidget> BuildUsageTab();

	// ---- UI language (live switch) -----------------------------------------
	/** Rebuild the whole panel from scratch (used when the UI language changes). */
	void Rebuild();
	/** Listens for UOpenSplat4DSettings::UISLanguage changes and rebuilds. */
	void OnSettingsPropertyChanged(UObject* Object, struct FPropertyChangedEvent& Event);
	/** Builds the main vertical-box content of the panel (without ChildSlot assignment). */
	TSharedRef<SWidget> BuildContent();

	// ---- Capture-set asset selector (native DataAsset) --------------------
	/** Return the object-path string for the currently selected CaptureSet asset
	 *  (bound to SObjectPropertyEntryBox's ObjectPath attribute). */
	FString GetCaptureSetObjectPath() const;
	/** Asset-picker callback: assign the chosen CaptureSet asset and point every
	 *  pipeline step's working directory at its images folder. */
	void OnCaptureSetChanged(const FAssetData& AssetData);
	/** "New" button: create a fresh (empty) CaptureSet asset and make it active. */
	FReply OnNewCaptureSetClicked();

	/** "Open index" button: open the currently selected CaptureSet (index asset)
	 *  in its asset editor so the user can view / edit its file locations. */
	FReply OnOpenCaptureSetClicked();

private:
	FString WorkDir;
	FString BaseWorkDir;
	TSharedPtr<IDetailsView> DetailsView;
	TSharedPtr<SWidgetSwitcher> BodySwitcher;

	TObjectPtr<UOpenSplat4DStep_Capture> StepCapture;
	TObjectPtr<UOpenSplat4DStep_SparseReconstruction> StepSparseReconstruction;
	TObjectPtr<UOpenSplat4DStep_GaussianSplatting> StepGaussianSplatting;

	TObjectPtr<UOpenSplat4DStepBase> CurrentTask;

	// ---- Usage tab: import --------------------------------------------------
	FReply OnImportClicked();

	// ---- Usage tab: add to scene -------------------------------------------
	TWeakObjectPtr<UOpenSplat4DPointCloud> PickedCloud;
	void OnCloudPicked(const FAssetData& AssetData);
	FReply OnAddToSceneClicked();

	// ---- Usage tab: create from scene (scan) -------------------------------
	bool bCameraDepth = false;
	TOptional<int32> ScanDensity;
	TOptional<float> ScanPointScale;
	FReply OnScanClicked();
	ECheckBoxState IsCameraDepth() const;
	void OnCameraDepthChanged(ECheckBoxState State);
	TOptional<int32> GetScanDensity() const;
	void OnScanDensityChanged(int32 Value);
	TOptional<float> GetScanPointScale() const;
	void OnScanPointScaleChanged(float Value);

	// ---- Usage tab: playback ------------------------------------------------
	TWeakObjectPtr<AGaussianSplatActor> CurrentActor;
	FReply OnUseSelectedClicked();
	FReply OnPlayClicked();
	FReply OnPauseClicked();
	void OnTimeChanged(float Value);
	TOptional<float> GetTime() const;
	void OnPlayRateChanged(float Value);
	TOptional<float> GetPlayRate() const;
	ECheckBoxState IsLooping() const;
	void OnLoopingChanged(ECheckBoxState State);

	UWorld* GetEditorWorld() const;
};
