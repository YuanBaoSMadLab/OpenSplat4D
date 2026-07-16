#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * Runtime module for the OpenSplat4D plugin.
 * Provides the data model (UOpenSplat4DPointCloud), the Niagara actor
 * (AOpenSplat4DPointCloudActor) and the 4D-aware Niagara data interface
 * (UNiagaraDataInterfaceOpenSplat4D) used to render 3DGS / 4DGS point clouds.
 */
class FOpenSplat4DRuntimeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};

DECLARE_LOG_CATEGORY_EXTERN(LogOpenSplat4D, Log, All);
