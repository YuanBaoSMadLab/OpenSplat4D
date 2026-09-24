// Copyright OpenSplat4D 2026 All Rights Reserved.
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

	// Resolve the optional local ThirdParty drop-in directory, e.g.
	//   <Project>/Plugins/OpenSplat4D/ThirdParty/
	// This folder is NOT shipped with the plugin: it is only used as a fallback
	// when the user drops their own COLMAP / Python / training repo copies there
	// instead of pointing the settings at them explicitly.
	static FString GetPluginThirdPartyDir()
	{
		const FString Base = GetPluginBaseDir();
		return Base.IsEmpty() ? FString() : (Base / TEXT("ThirdParty"));
	}

	// Resolve the plugin's Python directory (the orchestrator scripts that DO
	// ship with the plugin), e.g.
	//   <Project>/Plugins/OpenSplat4D/Content/Python/OpenSplat4D/
	static FString GetPluginPythonDir()
	{
		const FString Base = GetPluginBaseDir();
		return Base.IsEmpty() ? FString() : (Base / TEXT("Content/Python/OpenSplat4D"));
	}

	// Optional local drop-in locations, used ONLY when the user did not point the
	// corresponding setting at their own copy. None of these ship with the plugin
	// (the training pipeline and its runtime are the user's own responsibility):
	//   <Plugin>/ThirdParty/colmap/colmap.exe        (optional local COLMAP)
	//   <Plugin>/ThirdParty/gaussian-splatting/      (optional local Inria 3DGS)
	//   <Plugin>/ThirdParty/4d-gaussian-splatting/   (optional local 4DGS)
	static FString GetBundledColmapPath()
	{
		const FString TP = GetPluginThirdPartyDir();
		if (TP.IsEmpty())
		{
			return FString();
		}
		// Prefer the real executable under bin/. The python helper derives the Qt
		// plugin path as dirname(dirname(colmap)), so pointing at
		//   <Plugin>/ThirdParty/colmap/bin/colmap.exe
		// makes that resolve to <Plugin>/ThirdParty/colmap/plugins (correct).
		const FString BinExe = TP / TEXT("colmap/bin/colmap.exe");
		if (FPaths::FileExists(BinExe))
		{
			return BinExe;
		}
		// Fallback: the COLMAP.bat launcher (it sets PATH / QT_PLUGIN_PATH itself).
		const FString Bat = TP / TEXT("colmap/COLMAP.bat");
		if (FPaths::FileExists(Bat))
		{
			return Bat;
		}
		return FString();
	}

	// Optional local Python interpreter drop-in. The plugin does NOT ship a Python
	// runtime: training needs the user's own interpreter (system Python, venv or
	// conda env) with torch+cu128 and the locally compiled CUDA extensions.
	//   <Plugin>/ThirdParty/Python/python.exe
	static FString GetBundledPythonPath()
	{
		const FString TP = GetPluginThirdPartyDir();
		return TP.IsEmpty() ? FString() : (TP / TEXT("Python/python.exe"));
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
}

void UOpenSplat4DSettings::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	PythonExecutablePathConfig = PythonExecutablePath.FilePath;
	ColmapExecutablePathConfig = ColmapExecutablePath.FilePath;
	GaussianSplattingRepoDirConfig = GaussianSplattingRepoDir.Path;
	GaussianSplatting4DRepoDirConfig = GaussianSplatting4DRepoDir.Path;
	HelperScriptPathConfig = HelperScriptPath.FilePath;
}

FString UOpenSplat4DSettings::GetPythonExecutablePath() const
{
	// 1) A user-specified path always wins. This is the supported way to point
	//    the plugin at any Python (system Python, a venv, a conda env, ...).
	if (!PythonExecutablePathConfig.IsEmpty())
	{
		return PythonExecutablePathConfig;
	}
	// 2) Fall back to an optional local drop-in (<Plugin>/ThirdParty/Python).
	//    Nothing ships with the plugin, so this only resolves when the developer
	//    placed an interpreter there themselves. We deliberately do NOT rely on
	//    "conda activate <env>" or a bare "python" from PATH here: the settings
	//    value is what the pipeline is told to use, so it must be an explicit
	//    path the user chose (step 1) or a file we can see exists.
	const FString Bundled = GetBundledPythonPath();
	if (FPaths::FileExists(Bundled))
	{
		return Bundled;
	}
	// 3) Neither exists: return empty so the editor shows an actionable error
	//    telling the user to set PythonExecutablePath in
	//    Project Settings > OpenSplat4D.
	return FString();
}

FString UOpenSplat4DSettings::GetColmapExecutablePath() const
{
	// 1) A user-specified path always wins over the optional local drop-in.
	if (!ColmapExecutablePathConfig.IsEmpty())
	{
		return ColmapExecutablePathConfig;
	}
	// 2) Otherwise use an optional local COLMAP under <Plugin>/ThirdParty/colmap.
	const FString Bundled = GetBundledColmapPath();
	if (FPaths::FileExists(Bundled))
	{
		return Bundled;
	}
	// 3) Neither exists: return empty so the editor can remind the user to
	//    specify ColmapExecutablePath in Project Settings > OpenSplat4D.
	return FString();
}

FString UOpenSplat4DSettings::GetHelperScriptPath() const
{
	if (!HelperScriptPathConfig.IsEmpty())
	{
		return HelperScriptPathConfig;
	}
	// Default to the orchestrator script that ships inside Content/Python.
	const FString PythonDir = GetPluginPythonDir();
	return PythonDir.IsEmpty() ? FString() : (PythonDir / TEXT("openplat4d_helper.py"));
}

FString UOpenSplat4DSettings::Get3DGSRepoDir() const
{
	// 1) User-specified repo wins. 2) Fall back to an optional local drop-in.
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
	// 1) User-specified repo wins. 2) Fall back to an optional local drop-in.
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
