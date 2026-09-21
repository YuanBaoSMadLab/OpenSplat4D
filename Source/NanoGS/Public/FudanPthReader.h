// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GaussianDataTypes.h"

/**
 * Reader for fudan-zvg 4DGS PyTorch checkpoints (.pth files written with torch.save).
 *
 * The fudan checkpoint stores the gaussian_model.capture() tensors:
 *   _xyz(N,3) / _t(N,1) / _scaling(N,3) / _scaling_t(N,1) / _rotation(N,4) /
 *   _rotation_r(N,4) / _opacity(N,1) / _features_dc(N,1,3) / _features_rest(N,C-1,3)
 * with the same semantics as the fudan PLY export: log-encoded spatial scales,
 * logit opacity, dual quaternion (q_l = _rotation, q_r = _rotation_r) and 4D
 * spherical-cylindrical SH (DC in _features_dc, rest in _features_rest).
 *
 * DECLARED LIMITATIONS (explicit, non-crashing -- clear error messages):
 *  - Only the modern zip-based torch.save format (PyTorch >= 1.6) is supported.
 *    Legacy (torch < 1.6) pickle checkpoints are rejected.
 *  - Only float32 storages are supported (the fudan capture() is float32 anyway).
 *  - The checkpoint must carry the fudan 4DGS tensor set (missing required
 *    tensors produce a "not a fudan 4DGS checkpoint" error).
 */
class NANOGS_API FFudanPthReader
{
public:
	/**
	 * Read a fudan 4DGS torch.save checkpoint and extract gaussian splat data.
	 * @param FilePath Path to the .pth file
	 * @param OutSplats Output array of splat data (bFudan4D = true)
	 * @param OutError Error message if reading failed
	 * @param OutSHBands Optional output: 4D SH channel count C (1/6/16/32/33/48)
	 * @return True if successful
	 */
	static bool ReadPthFile(const FString& FilePath, TArray<FGaussianSplatData>& OutSplats, FString& OutError, int32* OutSHBands = nullptr);

	/**
	 * Check if a file starts with the torch.save zip magic ("PK\x03\x04")
	 * @param FilePath Path to the .pth file
	 * @return True if the file looks like a modern torch.save checkpoint
	 */
	static bool IsValidPthFile(const FString& FilePath);
};
