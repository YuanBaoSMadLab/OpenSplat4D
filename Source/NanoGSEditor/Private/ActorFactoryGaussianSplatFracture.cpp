// Copyright OpenSplat4D 2026 All Rights Reserved.

#include "ActorFactoryGaussianSplatFracture.h"

#include "GaussianSplatFractureActor.h"
#include "GaussianSplatFractureAsset.h"

UActorFactoryGaussianSplatFracture::UActorFactoryGaussianSplatFracture()
{
	DisplayName = NSLOCTEXT("GaussianSplatting", "GaussianSplatFractureActorDisplayName", "高斯泼溅破裂 Actor");
	NewActorClass = AGaussianSplatFractureActor::StaticClass();
}

bool UActorFactoryGaussianSplatFracture::CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg)
{
	if (!AssetData.IsValid() || !AssetData.GetClass()->IsChildOf(UGaussianSplatFractureAsset::StaticClass()))
	{
		OutErrorMsg = NSLOCTEXT("GaussianSplatting", "CannotCreateFractureActorFrom", "未指定有效的高斯泼溅破裂资产。");
		return false;
	}

	return true;
}

void UActorFactoryGaussianSplatFracture::PostSpawnActor(UObject* Asset, AActor* NewActor)
{
	Super::PostSpawnActor(Asset, NewActor);

	UGaussianSplatFractureAsset* FractureAsset = Cast<UGaussianSplatFractureAsset>(Asset);
	AGaussianSplatFractureActor* FractureActor = Cast<AGaussianSplatFractureActor>(NewActor);

	if (FractureAsset && FractureActor)
	{
		FractureActor->SetFractureAsset(FractureAsset);
	}
}

UObject* UActorFactoryGaussianSplatFracture::GetAssetFromActorInstance(AActor* ActorInstance)
{
	if (AGaussianSplatFractureActor* FractureActor = Cast<AGaussianSplatFractureActor>(ActorInstance))
	{
		return FractureActor->GetFractureAsset();
	}

	return nullptr;
}
