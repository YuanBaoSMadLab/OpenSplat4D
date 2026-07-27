#pragma once

#include "CoreMinimal.h"
#include "OpenSplat4DTypes.generated.h"

/** 3DGS / 4DGS 渲染模式 */
UENUM(BlueprintType)
enum class EOpenSplat4DMode : uint8
{
	Static3D  UMETA(DisplayName = "3DGS (Static)"),
	Dynamic4D UMETA(DisplayName = "4DGS (Dynamic)"),
};

/** 点云资产序列化压缩方式 */
UENUM(BlueprintType)
enum class EOpenSplat4DCompressionMethod : uint8
{
	None UMETA(DisplayName = "无"),
	Zlib UMETA(DisplayName = "Zlib"),
	Spz  UMETA(DisplayName = "SPZ (Niantic, 4D-aware)"),
};
