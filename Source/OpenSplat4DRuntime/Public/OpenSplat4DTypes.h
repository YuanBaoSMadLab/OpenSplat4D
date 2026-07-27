#pragma once

#include "CoreMinimal.h"
#include "OpenSplat4DTypes.generated.h"

/** 3DGS / 4DGS 渲染模式
 *
 * NOTE: Dynamic4D (4DGS) is EXPERIMENTAL in this build.
 *   - 帧间一致性 / 时间戳映射 / 插值逻辑尚未完整验证，可能在边界条件下
 *     出现抖动、漂移或闪烁。
 *   - 4D 资产的导入、序列化、渲染路径都缺少端到端测试覆盖。
 *   - 在生产场景中请优先使用 Static3D；4D 模式仅用于研究和预览。
 *   - 如需启用 4D，请同时阅读 STATUS.md 中 "4D 实验性" 一节。 */
UENUM(BlueprintType)
enum class EOpenSplat4DMode : uint8
{
	Static3D  UMETA(DisplayName = "3DGS (Static)"),
	Dynamic4D UMETA(DisplayName = "4DGS (Dynamic, 实验性)"),
};

/** 点云资产序列化压缩方式 */
UENUM(BlueprintType)
enum class EOpenSplat4DCompressionMethod : uint8
{
	None UMETA(DisplayName = "无"),
	Zlib UMETA(DisplayName = "Zlib"),
	Spz  UMETA(DisplayName = "SPZ (Niantic, 4D-aware)"),
};
