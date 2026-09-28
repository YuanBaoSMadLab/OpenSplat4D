// Copyright OpenSplat4D 2026 All Rights Reserved.
using System.IO;
using UnrealBuildTool;

// ============================================================================
// LEGACY MODULE — retained for backward compatibility.
// ============================================================================
// The active editor pipeline lives in `NanoGSEditor`. This module still hosts
// the EdMode panel, capture / reconstruction / training step UI, and the
// legacy ISMC + Niagara actor factories, so users can keep opening older
// maps. The SplatActor factory is disabled (see OpenSplat4DActorFactory.cpp);
// dragging a PLY into the viewport now creates a NanoGS actor instead.
//
// New editor features should go in NanoGSEditor. Do not extend this module
// unless you are fixing a regression in legacy content.
// ============================================================================

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

		// No Niagara template assets are staged: NS_OpenSplat4D / NE_OpenSplat4D
		// are generated programmatically at editor startup by
		// OpenSplat4DNiagaraSetup::BuildNiagaraSystem() into
		// Content/Niagara/Templates/UE5_8/ (saved with the running engine's version).
		// The old pre-baked UE5_3 template assets were removed — they were
		// unreferenced by any code and only matched UE 5.3.

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

		// UE 6.0+ splits FEditorViewportClientBase into a separate EditorViewport module
		if (Target.Version.MajorVersion >= 6)
		{
			PrivateDependencyModuleNames.Add("EditorViewport");
		}
	}
}
