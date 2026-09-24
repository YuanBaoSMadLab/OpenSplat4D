// Copyright Epic Games, Inc. All Rights Reserved.
// Copyright OpenSplat4D 2026 All Rights Reserved.

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
				"OpenSplat4DRuntime",
				// FMeshDescription is part of the proxy mesh builder's interface.
				"MeshDescription"
			}
		);

		PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"Slate",
					"SlateCore",
					"InputCore",
					"UnrealEd",
					"AssetTools",
					"AssetRegistry",
					"DesktopPlatform",
					"EditorFramework",
					"Projects",
					"ToolMenus",
					"PropertyEditor",
					"AdvancedPreviewScene",
					"EditorStyle",
					"ComponentVisualizers",
					"WorkspaceMenuStructure",
					// FStaticMeshAttributes lives in StaticMeshDescription.
					"StaticMeshDescription",
					// Editor-only Chaos fracture support (proxy mesh -> GeometryCollection).
					// NanoGS (Runtime) must never depend on this.
					"GeometryCollectionEngine"
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
