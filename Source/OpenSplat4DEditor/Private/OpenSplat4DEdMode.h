#pragma once

#include "EditorModes.h"
#include "EdMode.h"
#include "OpenSplat4DEdModePanel.h"

/**
 * Editor mode ("OpenSplat4D") that hosts the usage panel
 * (SOpenSplat4DEdModePanel). Modeled on FGaussianSplattingEdMode from the
 * reference plugin, but tailored to OpenSplat4D's rendering / playback API.
 */
class FOpenSplat4DEdMode : public FEdMode
{
public:
	static const FEditorModeID EdID;
	void Enter() override;
	void Exit() override;
};

class FOpenSplat4DEdModeToolkit : public FModeToolkit
{
public:
	FOpenSplat4DEdModeToolkit();
	~FOpenSplat4DEdModeToolkit();

	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FEdMode* GetEditorMode() const override;
	virtual TSharedPtr<SWidget> GetInlineContent() const override;

private:
	TSharedPtr<SOpenSplat4DEdModePanel> Panel;
};
