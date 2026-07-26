// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ActorFactories/ActorFactory.h"
#include "ActorFactoryGaussianSplat.generated.h"

/**
 * Actor factory for creating Gaussian Splat Actors from Gaussian Splat Assets.
 * Enables drag-and-drop from Content Browser into the level.
 */
UCLASS(MinimalAPI, config = Editor)
class UActorFactoryGaussianSplat : public UActorFactory
{
	GENERATED_BODY()

public:
	UActorFactoryGaussianSplat();

	//~ Begin UActorFactory Interface
	virtual bool CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg) override;
	virtual void PostSpawnActor(UObject* Asset, AActor* NewActor) override;
	virtual UObject* GetAssetFromActorInstance(AActor* ActorInstance) override;
	//~ End UActorFactory Interface
};
