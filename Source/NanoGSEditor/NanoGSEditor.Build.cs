// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class NanoGSEditor : ModuleRules
{
	public NanoGSEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(
			new string[] {
			}
		);

		PrivateIncludePaths.AddRange(
			new string[] {
			}
		);

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"NanoGS",
				"OpenSplat4DRuntime"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Slate",
				"SlateCore",
				"UnrealEd",
				"AssetTools",
				"EditorFramework",
				"Projects",
				"ToolMenus",
				"PropertyEditor",
				"AdvancedPreviewScene",
				"EditorStyle",
				"ComponentVisualizers",
				"WorkspaceMenuStructure"
			}
		);

		// UE 6.0+ splits FEditorViewportClientBase into a separate EditorViewport module
		if (Target.Version.MajorVersion >= 6)
		{
			PrivateDependencyModuleNames.Add("EditorViewport");
		}

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
			}
		);
	}
}
