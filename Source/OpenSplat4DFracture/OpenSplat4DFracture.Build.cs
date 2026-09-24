// Copyright OpenSplat4D 2026 All Rights Reserved.

using UnrealBuildTool;

// ============================================================================
// Runtime 模块：把「破裂资产」变成可以拖进场景 / 在蓝图里控制的 Actor。
//
// 为什么要单独开一个模块：
//   NanoGS / OpenSplat4DRuntime 是 PostConfigInit 相位，早于 GeometryCollectionEngine
//   加载，所以它们永远不能依赖该模块（现有一律走 FindObject<UClass> + ProcessEvent
//   反射）。而「拖入场景即得到一个蓝图里能直接调的 UGeometryCollectionComponent」
//   要求 C++ 里硬声明该类型 —— 二者只能在 Default 相位的新模块里同时成立。
// ============================================================================
public class OpenSplat4DFracture : ModuleRules
{
	public OpenSplat4DFracture(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				// UGaussianSplatFractureAsset / UGaussianSplatComponent 所在模块。
				"NanoGS",
				// UGeometryCollectionComponent / UGeometryCollection。放在 Public：
				// 本模块的公开头会把 GeometryCollectionComponent 暴露给使用方
				// （游戏模块要能直接对组件调 Chaos 的破裂节点），必须完整可见。
				"GeometryCollectionEngine"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// FGeometryCollection（UGeometryCollection 的内部数据）。
				"Chaos"
			}
		);
	}
}
