// Copyright Epic Games, Inc. All Rights Reserved.
// Copyright OpenSplat4D 2026 All Rights Reserved.

#include "GaussianSplatFractureAssetTypeActions.h"
#include "GaussianSplatFractureAsset.h"
#include "GaussianSplatFractureAssetEditor.h"

#define LOCTEXT_NAMESPACE "AssetTypeActions_GaussianSplatFractureAsset"

FText FAssetTypeActions_GaussianSplatFractureAsset::GetName() const
{
	return LOCTEXT("AssetName", "破裂资产");
}

FColor FAssetTypeActions_GaussianSplatFractureAsset::GetTypeColor() const
{
	// 暖红，与源 OpenSplat 资产的青色区分开
	return FColor(220, 90, 70);
}

UClass* FAssetTypeActions_GaussianSplatFractureAsset::GetSupportedClass() const
{
	return UGaussianSplatFractureAsset::StaticClass();
}

uint32 FAssetTypeActions_GaussianSplatFractureAsset::GetCategories()
{
	return EAssetTypeCategories::Misc;
}

void FAssetTypeActions_GaussianSplatFractureAsset::OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<IToolkitHost> EditWithinLevelEditor)
{
	const EToolkitMode::Type Mode = EditWithinLevelEditor.IsValid() ? EToolkitMode::WorldCentric : EToolkitMode::Standalone;

	for (UObject* Object : InObjects)
	{
		if (UGaussianSplatFractureAsset* Asset = Cast<UGaussianSplatFractureAsset>(Object))
		{
			TSharedRef<FGaussianSplatFractureAssetEditor> NewEditor = MakeShareable(new FGaussianSplatFractureAssetEditor());
			NewEditor->InitGaussianSplatFractureAssetEditor(Mode, EditWithinLevelEditor, Asset);
		}
	}
}

#undef LOCTEXT_NAMESPACE
