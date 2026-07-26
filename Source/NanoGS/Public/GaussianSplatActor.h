// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GaussianSplatActor.generated.h"

class UGaussianSplatComponent;

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
