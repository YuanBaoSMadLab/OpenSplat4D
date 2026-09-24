// Copyright OpenSplat4D 2026 All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FOpenSplat4DEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();

	/** Editor language changed: re-derive the plugin UI language and rebuild. */
	void HandleEditorCultureChanged();
};

DECLARE_LOG_CATEGORY_EXTERN(LogOpenSplat4DEditor, Log, All);
