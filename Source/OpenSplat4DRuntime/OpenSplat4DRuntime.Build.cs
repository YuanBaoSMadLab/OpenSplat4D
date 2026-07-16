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
	}
}
