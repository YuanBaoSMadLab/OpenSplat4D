using System.IO;
using UnrealBuildTool;

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
