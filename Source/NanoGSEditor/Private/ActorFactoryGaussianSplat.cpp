// Copyright Epic Games, Inc. All Rights Reserved.

#include "ActorFactoryGaussianSplat.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatActor.h"
#include "GaussianSplatComponent.h"

UActorFactoryGaussianSplat::UActorFactoryGaussianSplat()
{
	DisplayName = NSLOCTEXT("GaussianSplatting", "GaussianSplatActorDisplayName", "Gaussian Splat Actor");
	NewActorClass = AGaussianSplatActor::StaticClass();
	bUseSurfaceOrientation = true;
}

bool UActorFactoryGaussianSplat::CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg)
{
	if (!AssetData.IsValid() || !AssetData.GetClass()->IsChildOf(UGaussianSplatAsset::StaticClass()))
	{
		OutErrorMsg = NSLOCTEXT("GaussianSplatting", "CannotCreateActorFrom", "No valid Gaussian Splat asset specified.");
		return false;
	}

	return true;
}

void UActorFactoryGaussianSplat::PostSpawnActor(UObject* Asset, AActor* NewActor)
{
	Super::PostSpawnActor(Asset, NewActor);

	UGaussianSplatAsset* GaussianSplatAsset = Cast<UGaussianSplatAsset>(Asset);
	AGaussianSplatActor* GaussianSplatActor = Cast<AGaussianSplatActor>(NewActor);

	if (GaussianSplatAsset && GaussianSplatActor && GaussianSplatActor->GaussianSplatComponent)
	{
		GaussianSplatActor->GaussianSplatComponent->SetSplatAsset(GaussianSplatAsset);
	}
}

UObject* UActorFactoryGaussianSplat::GetAssetFromActorInstance(AActor* ActorInstance)
{
	if (AGaussianSplatActor* GaussianSplatActor = Cast<AGaussianSplatActor>(ActorInstance))
	{
		if (GaussianSplatActor->GaussianSplatComponent)
		{
			return GaussianSplatActor->GaussianSplatComponent->GetSplatAsset();
		}
	}

	return nullptr;
}
