// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatShaders.h"
#include "ShaderParameterUtils.h"
#include "RHIStaticStates.h"

// Implement global shaders
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatCalcViewDataCS, "/Plugin/NanoGS/Private/CalcViewData.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatCalcDistancesCS, "/Plugin/NanoGS/Private/CalcDistances.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatVS, "/Plugin/NanoGS/Private/GaussianSplatRendering.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatPS, "/Plugin/NanoGS/Private/GaussianSplatRendering.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatCompositeVS, "/Plugin/NanoGS/Private/GaussianSplatComposite.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FGaussianSplatCompositePS, "/Plugin/NanoGS/Private/GaussianSplatComposite.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FRadixSortCountCS, "/Plugin/NanoGS/Private/RadixSort.usf", "CountCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRadixSortPrefixSumCS, "/Plugin/NanoGS/Private/RadixSort.usf", "PrefixSumCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRadixSortDigitPrefixSumCS, "/Plugin/NanoGS/Private/RadixSort.usf", "DigitPrefixSumCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRadixSortScatterCS, "/Plugin/NanoGS/Private/RadixSort.usf", "ScatterCS", SF_Compute);

// Cluster culling shaders
IMPLEMENT_GLOBAL_SHADER(FClusterCullingResetCS, "/Plugin/NanoGS/Private/ClusterCulling.usf", "ResetCounterCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FClusterCullingCS, "/Plugin/NanoGS/Private/ClusterCulling.usf", "MainCS", SF_Compute);

// Splat compaction shaders (GPU-driven work reduction)
IMPLEMENT_GLOBAL_SHADER(FCompactSplatsCS, "/Plugin/NanoGS/Private/CompactSplats.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FPrepareIndirectArgsCS, "/Plugin/NanoGS/Private/PrepareIndirectArgs.usf", "MainCS", SF_Compute);

// NOTE: GPU-driven LOD shaders removed in unified approach
// LOD splats are now processed by CalcViewData.usf using the same buffers as original splats

// Global accumulator + compaction prefix-sum shaders
IMPLEMENT_GLOBAL_SHADER(FGatherVisibleCountsCS,     "/Plugin/NanoGS/Private/GlobalAccumulatorPrefixSum.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FPrefixSumVisibleCountsCS,  "/Plugin/NanoGS/Private/GlobalAccumulatorPrefixSum.usf", "MainCS", SF_Compute);
