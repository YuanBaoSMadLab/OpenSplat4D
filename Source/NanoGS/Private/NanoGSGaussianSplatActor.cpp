// Copyright Epic Games, Inc. All Rights Reserved.

#include "NanoGSGaussianSplatActor.h"
#include "NanoGSGaussianSplatComponent.h"

AGaussianSplatActor::AGaussianSplatActor()
{
	// Create the Gaussian Splat component as a default subobject
	GaussianSplatComponent = CreateDefaultSubobject<UGaussianSplatComponent>(TEXT("GaussianSplatComponent"));
	RootComponent = GaussianSplatComponent;
}