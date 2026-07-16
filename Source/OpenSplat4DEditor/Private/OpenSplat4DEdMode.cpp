#include "OpenSplat4DEdMode.h"
#include "EditorModeManager.h"
#include "Toolkits/ToolkitManager.h"
#include "OpenSplat4DLocalization.h"

const FEditorModeID FOpenSplat4DEdMode::EdID(TEXT("EM_OpenSplat4D"));

void FOpenSplat4DEdMode::Enter()
{
	FEdMode::Enter();
	if (!Toolkit.IsValid())
	{
		Toolkit = MakeShareable(new FOpenSplat4DEdModeToolkit);
		Toolkit->Init(Owner->GetToolkitHost());
	}
}

void FOpenSplat4DEdMode::Exit()
{
	FToolkitManager::Get().CloseToolkit(Toolkit.ToSharedRef());
	Toolkit.Reset();
	FEdMode::Exit();
}

FOpenSplat4DEdModeToolkit::FOpenSplat4DEdModeToolkit()
{
	Panel = SNew(SOpenSplat4DEdModePanel);
}

FOpenSplat4DEdModeToolkit::~FOpenSplat4DEdModeToolkit()
{
}

FName FOpenSplat4DEdModeToolkit::GetToolkitFName() const
{
	return FName("OpenSplat4DEdMode");
}

FText FOpenSplat4DEdModeToolkit::GetBaseToolkitName() const
{
	return OS4D_TEXT("OpenSplat4D");
}

FEdMode* FOpenSplat4DEdModeToolkit::GetEditorMode() const
{
	return GLevelEditorModeTools().GetActiveMode(FOpenSplat4DEdMode::EdID);
}

TSharedPtr<SWidget> FOpenSplat4DEdModeToolkit::GetInlineContent() const
{
	return Panel;
}
