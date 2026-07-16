using System.IO;
using UnrealBuildTool;

public class OpenSplat4DEditor : ModuleRules
{
	public OpenSplat4DEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");

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
				"OpenSplat4DRuntime",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Slate",
				"SlateCore",
				"UMG",
				"Niagara",
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
