using System.IO;
using UnrealBuildTool;

// ============================================================================
// LEGACY MODULE — retained for backward compatibility, NOT actively used.
// ============================================================================
// The primary rendering pipeline is now `NanoGS` (custom Compute Shader +
// RDG). This module contains the older ISMC + Niagara code path, which is
// kept compilable so existing assets/levels that reference the legacy
// AOpenSplat4DSplatActor / UOpenSplat4DPointCloud types still load, but the
// Actor factory is disabled (see OpenSplat4DActorFactory.cpp) and no new
// content should target this path.
//
// Do not add new features here. New work belongs in NanoGS / NanoGSEditor.
// ============================================================================

public class OpenSplat4DRuntime : ModuleRules
{
	public OpenSplat4DRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"RenderCore",
				"RHI",
				"Renderer",
				"InputCore",
				"NanoGS",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Projects",
				"Slate",
				"SlateCore",
				"Niagara",
				"NiagaraShader"
			}
			);

		if (Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("UnrealEd");
		}

		// Shader 源码路径：UE 自动从 Plugins/<Name>/Shaders 查找 .usf 文件。
		// 这里同时把 Private 目录加入 include 路径，方便 Shader 头文件引用。
		PrivateIncludePaths.AddRange(new string[] { "OpenSplat4DRuntime/Private" });
	}
}
