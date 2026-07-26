#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * Runtime module for the OpenSplat4D plugin.
 * Provides the data model (UOpenSplat4DPointCloud), the Niagara actor
 * (AOpenSplat4DPointCloudActor) and the 4D-aware Niagara data interface
 * (UNiagaraDataInterfaceOpenSplat4D) used to render 3DGS / 4DGS point clouds.
 *
 * Multiple rendering modes:
 *   - Native OpenSplat4D mode (default): Niagara assets created in pure C++,
 *     no external .uasset templates needed.
 *   - Teacher-compatible mode: loads .uasset templates from the reference
 *     GaussianSplattingForUnrealEngine plugin. Call RegisterTeacherCoreRedirects()
 *     before loading any template to enable transparent type remapping.
 */
class OPENSPLAT4DRUNTIME_API FOpenSplat4DRuntimeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Register CoreRedirects so teacher-authored .uasset templates (which
	 *  reference GaussianSplattingRuntime types) load as OpenSplat4D types.
	 *  Idempotent — safe to call multiple times. Only needed when the
	 *  teacher-compatible rendering mode is selected. */
	static void RegisterTeacherCoreRedirects();
};

#ifndef OPENSPLAT4D_LOG_DECLARED
#define OPENSPLAT4D_LOG_DECLARED
OPENSPLAT4DRUNTIME_API DECLARE_LOG_CATEGORY_EXTERN(LogOpenSplat4D, Log, All);
#endif
