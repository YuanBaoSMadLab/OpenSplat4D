// Copyright OpenSplat4D 2026 All Rights Reserved.
// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
// UE 5.9 起 UHT 对「仅前置声明、未见定义」的 UPROPERTY 类型会猜错跨模块 API 宏，
// 必须 include 定义头让 UHT 解析到正确模块（NANOGS_API）。
#include "NanoGSGaussianSplatComponent.h"
#include "NanoGSGaussianSplatActor.generated.h"

/**
 * Simple actor for placing Gaussian Splats in the level.
 * Drag and drop a Gaussian Splat Asset from the Content Browser to spawn this actor.
 */
UCLASS(NotPlaceable)
class NANOGS_API AGaussianSplatActor : public AActor
{
	GENERATED_BODY()

public:
	AGaussianSplatActor();

	/** The Gaussian Splat component */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gaussian Splatting")
	TObjectPtr<UGaussianSplatComponent> GaussianSplatComponent;
};