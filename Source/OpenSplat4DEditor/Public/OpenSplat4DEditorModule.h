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
};

DECLARE_LOG_CATEGORY_EXTERN(LogOpenSplat4DEditor, Log, All);
