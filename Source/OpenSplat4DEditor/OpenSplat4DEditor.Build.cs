using System.IO;
using UnrealBuildTool;

public class OpenSplat4DEditor : ModuleRules
{
	public OpenSplat4DEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");

			// NiagaraEditor private headers for programmatic graph building
			PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Plugins", "FX", "Niagara", "Source", "NiagaraEditor", "Private"));

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"UnrealEd",
				"AssetTools",
				"Kismet",
				"Core",
				"CoreUObject",
				"Engine",
				"RenderCore",
				"RHI",
				"AssetRegistry",
				"EditorFramework",
				"ImageCore",
				"Niagara",
				"NanoGS",
				"OpenSplat4DRuntime",
			}
			);

		// Stage Niagara template assets to build output
		RuntimeDependencies.Add("$(PluginDir)/Content/Niagara/Templates/UE5_8/NS_OpenSplat4D.uasset");
		RuntimeDependencies.Add("$(PluginDir)/Content/Niagara/Templates/UE5_8/NE_OpenSplat4D.uasset");

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Slate",
				"SlateCore",
				"UMG",
				"Niagara",
				"NiagaraShader",
				"MeshDescription",
				"StaticMeshDescription",
				"PropertyEditor",
				"UnrealEd",
				"AssetRegistry",
				"EditorStyle",
				"InputCore",
				"ContentBrowser",
				"ContentBrowserData",
				"AdvancedPreviewScene",
				"ToolMenus",
				"Projects",
				"DesktopPlatform",
				"NiagaraEditor",
				"PythonScriptPlugin",
				"LevelEditor",
				"Settings",
				"Json",
				"JsonUtilities",
				"AssetDefinition",
				"Landscape",
			}
			);
	}
}
