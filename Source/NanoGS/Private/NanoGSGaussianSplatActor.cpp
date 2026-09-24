// Copyright Epic Games, Inc. All Rights Reserved.
// Copyright OpenSplat4D 2026 All Rights Reserved.

#include "NanoGSGaussianSplatActor.h"
#include "NanoGSGaussianSplatComponent.h"

AGaussianSplatActor::AGaussianSplatActor()
{
	// Create the Gaussian Splat component as a default subobject
	GaussianSplatComponent = CreateDefaultSubobject<UGaussianSplatComponent>(TEXT("GaussianSplatComponent"));
	RootComponent = GaussianSplatComponent;
}