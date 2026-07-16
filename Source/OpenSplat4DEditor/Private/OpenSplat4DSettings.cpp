#include "OpenSplat4DSettings.h"

#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"

namespace
{
	// Resolve the plugin's base directory, e.g.
	//   <Project>/Plugins/OpenSplat4D/
	static FString GetPluginBaseDir()
	{
		if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("OpenSplat4D")))
		{
			return FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
		}
		return FString();
	}

	// Resolve the plugin's bundled ThirdParty directory, e.g.
	//   <Project>/Plugins/OpenSplat4D/ThirdParty/
	static FString GetPluginThirdPartyDir()
	{
		const FString Base = GetPluginBaseDir();
		return Base.IsEmpty() ? FString() : (Base / TEXT("ThirdParty"));
	}

	// Resolve the plugin's bundled Scripts directory, e.g.
	//   <Project>/Plugins/OpenSplat4D/Scripts/
	static FString GetPluginScriptsDir()
	{
		const FString Base = GetPluginBaseDir();
		return Base.IsEmpty() ? FString() : (Base / TEXT("Scripts"));
	}

	// The plugin's own bundled copies (so the pipeline works out-of-the-box once
	// the user drops the official binaries / repos into ThirdParty/):
	//   <Plugin>/ThirdParty/colmap/colmap.exe
	//   <Plugin>/ThirdParty/gaussian-splatting/        (Inria 3DGS)
	//   <Plugin>/ThirdParty/4d-gaussian-splatting/     (4DGS)
	static FString GetBundledColmapPath()
	{
		const FString TP = GetPluginThirdPartyDir();
		return TP.IsEmpty() ? FString() : (TP / TEXT("colmap/colmap.exe"));
	}
	static FString GetBundled3DGSRepoDir()
	{
		const FString TP = GetPluginThirdPartyDir();
		return TP.IsEmpty() ? FString() : (TP / TEXT("gaussian-splatting"));
	}
	static FString GetBundled4DGSRepoDir()
	{
		const FString TP = GetPluginThirdPartyDir();
		return TP.IsEmpty() ? FString() : (TP / TEXT("4d-gaussian-splatting"));
	}
}

void UOpenSplat4DSettings::PostInitProperties()
{
	Super::PostInitProperties();
	PythonExecutablePathConfig = PythonExecutablePath.FilePath;
	ColmapExecutablePathConfig = ColmapExecutablePath.FilePath;
	GaussianSplattingRepoDirConfig = GaussianSplattingRepoDir.Path;
	GaussianSplatting4DRepoDirConfig = GaussianSplatting4DRepoDir.Path;
	HelperScriptPathConfig = HelperScriptPath.FilePath;

	// Apply the persisted UI language (defaults to Chinese) to the global state.
	OpenSplat4DLocalization::SetUILanguage(UISLanguage);
}

void UOpenSplat4DSettings::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	PythonExecutablePathConfig = PythonExecutablePath.FilePath;
	ColmapExecutablePathConfig = ColmapExecutablePath.FilePath;
	GaussianSplattingRepoDirConfig = GaussianSplattingRepoDir.Path;
	GaussianSplatting4DRepoDirConfig = GaussianSplatting4DRepoDir.Path;
	HelperScriptPathConfig = HelperScriptPath.FilePath;

	if (PropertyChangedEvent.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(UOpenSplat4DSettings, UISLanguage))
	{
		OpenSplat4DLocalization::SetUILanguage(UISLanguage);
	}
}

FString UOpenSplat4DSettings::GetPythonExecutablePath() const
{
	// Fall back to whatever "python" is on PATH so the pipeline works out-of-the-box.
	return PythonExecutablePathConfig.IsEmpty() ? TEXT("python") : PythonExecutablePathConfig;
}

FString UOpenSplat4DSettings::GetColmapExecutablePath() const
{
	// 1) A user-specified path always wins over the bundled copy.
	if (!ColmapExecutablePathConfig.IsEmpty())
	{
		return ColmapExecutablePathConfig;
	}
	// 2) Otherwise default to the plugin's own bundled colmap.exe.
	const FString Bundled = GetBundledColmapPath();
	if (FPaths::FileExists(Bundled))
	{
		return Bundled;
	}
	// 3) Neither exists: return empty so the editor can remind the user to
	//    either specify a path in settings or drop colmap into ThirdParty/colmap/.
	return FString();
}

FString UOpenSplat4DSettings::GetHelperScriptPath() const
{
	if (!HelperScriptPathConfig.IsEmpty())
	{
		return HelperScriptPathConfig;
	}
	// Default to the plugin's bundled helper script.
	const FString ScriptsDir = GetPluginScriptsDir();
	return ScriptsDir.IsEmpty() ? FString() : (ScriptsDir / TEXT("openplat4d_helper.py"));
}

FString UOpenSplat4DSettings::Get3DGSRepoDir() const
{
	// 1) User-specified repo wins. 2) Fall back to the bundled 3DGS repo.
	if (!GaussianSplattingRepoDirConfig.IsEmpty())
	{
		return GaussianSplattingRepoDirConfig;
	}
	const FString Bundled = GetBundled3DGSRepoDir();
	if (FPaths::DirectoryExists(Bundled))
	{
		return Bundled;
	}
	return FString();
}

FString UOpenSplat4DSettings::Get4DGSRepoDir() const
{
	// 1) User-specified repo wins. 2) Fall back to the bundled 4DGS repo.
	if (!GaussianSplatting4DRepoDirConfig.IsEmpty())
	{
		return GaussianSplatting4DRepoDirConfig;
	}
	const FString Bundled = GetBundled4DGSRepoDir();
	if (FPaths::DirectoryExists(Bundled))
	{
		return Bundled;
	}
	return FString();
}

FString UOpenSplat4DSettings::GetWorkHome() const
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir() / TEXT("OpenSplat4D"));
}

FString UOpenSplat4DSettings::GetWorkDir(const FString& WorkName) const
{
	return GetWorkHome() / WorkName;
}
