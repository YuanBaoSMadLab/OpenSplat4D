#pragma once

#include "CoreMinimal.h"
#include "ActorFactories/ActorFactory.h"
#include "OpenSplat4DActorFactory.generated.h"

/**
 * Lets the user drag an UOpenSplat4DPointCloud asset from the Content Browser
 * straight into the level viewport: the editor spawns an
 * AOpenSplat4DPointCloudActor and wires the cloud to it. Mirrors the
 * UActorFactory_GaussianSplattingPointCloud pattern from the reference plugin.
 */
UCLASS(MinimalAPI)
class UActorFactory_OpenSplat4DPointCloud : public UActorFactory
{
	GENERATED_BODY()

public:
	UActorFactory_OpenSplat4DPointCloud(const FObjectInitializer& ObjectInitializer);

	virtual bool CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg) override;
	virtual void PostSpawnActor(UObject* Asset, AActor* NewActor) override;
	virtual UObject* GetAssetFromActorInstance(AActor* ActorInstance) override;
};
