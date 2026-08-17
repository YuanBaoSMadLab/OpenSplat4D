// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "GaussianSplatTrainingDatasetFactory.generated.h"

/**
 * Factory for creating Gaussian Splat Training Dataset assets.
 * Supports Content Browser right-click → 杂项 → 训练数据集.
 */
UCLASS(hidecategories = Object)
class NANOGSEDITOR_API UGaussianSplatTrainingDatasetFactory : public UFactory
{
	GENERATED_BODY()

public:
	UGaussianSplatTrainingDatasetFactory();

	//~ Begin UFactory Interface
	virtual UObject* FactoryCreateNew(
		UClass* InClass,
		UObject* InParent,
		FName InName,
		EObjectFlags Flags,
		UObject* Context,
		FFeedbackContext* Warn
	) override;
	virtual bool ShouldShowInNewMenu() const override { return true; }
	virtual FText GetDisplayName() const override;
	virtual FText GetToolTip() const override;
	//~ End UFactory Interface
};
