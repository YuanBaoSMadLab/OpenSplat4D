#include "OpenSplat4DRuntimeModule.h"
#include "Interfaces/IPluginManager.h"
#include "ShaderCore.h"

OPENSPLAT4DRUNTIME_API DEFINE_LOG_CATEGORY(LogOpenSplat4D);

#define LOCTEXT_NAMESPACE "OpenSplat4D"

void FOpenSplat4DRuntimeModule::StartupModule()
{
	// Mount this plugin's Shaders folder so the global billboard shaders
	// (OpenSplat4DBillboard.usf) resolve under /Plugin/OpenSplat4D/.
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("OpenSplat4D")))
	{
		const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
		if (FPaths::DirectoryExists(ShaderDir))
		{
			AddShaderSourceDirectoryMapping(TEXT("/Plugin/OpenSplat4D"), ShaderDir);
		}
	}

	UE_LOG(LogOpenSplat4D, Log, TEXT("OpenSplat4D runtime module started."));
}

void FOpenSplat4DRuntimeModule::ShutdownModule()
{
	UE_LOG(LogOpenSplat4D, Log, TEXT("OpenSplat4D runtime module shut down."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FOpenSplat4DRuntimeModule, OpenSplat4DRuntime)
