// Copyright OpenSplat4D 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/** Runtime 模块：承载可拖入场景的破裂 Actor（见 OpenSplat4DFracture.Build.cs 的说明）。 */
class FOpenSplat4DFractureModule : public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
