// Copyright OpenSplat4D 2026 All Rights Reserved.
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
				"OpenSplat4DRuntime",
				// AGaussianSplatFractureActor（拖入场景的破裂 Actor 工厂要引用它）。
				"OpenSplat4DFracture",
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
					// Editor-only Chaos fracture support (proxy mesh -> GeometryCollection
					// -> splat bone binding). NanoGS (Runtime) must never depend on this.
					"GeometryCollectionEngine",
					// FGeometryCollection / GeometryCollectionAlgo (rest bone matrices and
					// component space hierarchy) live in the Chaos runtime module.
					"Chaos",
					// IContentBrowserSingleton, for the GeometryCollection picker dialog.
					"ContentBrowser",
					// Chaos 破裂解算：FPlanarCells / CutWithPlanarCells / CreateCuttingSurfacePreview
					// （Editor-only；NanoGS (Runtime) 绝不能依赖这些）。
					"PlanarCut",
					"FractureEngine",
					// FVoronoiDiagram（Voronoi 站点破裂来源）。
					"Voronoi",
					// FDynamicMesh3（切割面预览）与几何工具。
					"GeometryCore",
					"DynamicMesh"
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
