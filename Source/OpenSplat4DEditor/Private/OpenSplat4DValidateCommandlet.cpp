#include "OpenSplat4DValidateCommandlet.h"
#include "OpenSplat4DNiagaraSetup.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"

int32 UOpenSplat4DValidateCommandlet::Main(const FString& Params)
{
	// Load required modules — the plugin .uplugin registers them as loading
	// phases but commandlets skip normal startup, so load explicitly.
	FModuleManager::Get().LoadModule(TEXT("OpenSplat4DRuntime"));
	FModuleManager::Get().LoadModule(TEXT("OpenSplat4DEditor"));

	UE_LOG(LogTemp, Display, TEXT("=== OpenSplat4D Validate: starting ==="));

	// [DISABLED] 全面禁用 Niagara 路径：验证命令不再创建 Niagara 资产。
	// 自研管线（SplatActor + ISMC）不需要 Niagara 资产。
	// if (!OpenSplat4DNiagaraSetup::EnsureAssetsExist())
	// {
	// 	UE_LOG(LogTemp, Error, TEXT("=== OpenSplat4D Validate: FAILED ==="));
	// 	return 1;
	// }

	UE_LOG(LogTemp, Display, TEXT("=== OpenSplat4D Validate: PASSED (Niagara disabled, 自研管线 active) ==="));
	return 0;
}
