#pragma once

#include <array>
#include <span>
#include <string>
#include <vector>
#include "OpenSplat4DPoint.h"

// =============================================================================
// OpenSplat4D SPZ compression
// -----------------------------------------------------------------------------
// This module is a *faithful port + 4D enhancement* of the Niantic "spz" format
// used by the reference GaussianSplattingForUnrealEngine runtime
// (teachers/GaussianSplattingForUnrealEngine/Source/GaussianSplattingRuntime/Private/Compression/Spz.*).
//
// [ENHANCEMENT vs. the reference Spz]
//   The reference Spz only stores the *static 3DGS* fields (position / rotation /
//   scale / base color). OpenSplat4D additionally carries 4DGS temporal fields
//   (AnchorTime, TimeVariance, Velocity, bUseVelocity). To preserve the compact
//   64-byte-per-gaussian Niantic layout AND keep 4D data, we append an *optional
//   4D extension block* ("O4D4") directly after the standard SPZ stream. A plain
//   Niantic SPZ decoder simply stops after the packed gaussians and never reads
//   the extra bytes, so the 4D block is fully backward-compatible with the
//   original format while giving OpenSplat4D full 4D round-tripping.
//
// Compared with the reference (and with OpenSplat4D's raw-blob FOpenSplat4DCompression),
// this module provides:
//   * Per-gaussian 24-bit fixed-point position quantization (~0.25 mm) instead of
//     dumping the whole TArray<FOpenSplat4DPoint> as raw floats -> ~6-10x smaller.
//   * A real gzip stream (zlib deflate, windowBits=16+MAX_WBITS) for entropy coding.
//   * Optional 4D temporal fields (unique to OpenSplat4D).
//   * UE-aware logging routed through the LogOpenSplat4D category.
// =============================================================================

// https://github.com/nianticlabs/spz
namespace Spz {

// Represents a single inflated gaussian. Each gaussian has 236 bytes. Although the data is easier
// to intepret in this format, it is not more precise than the packed format, since it was inflated.
struct UnpackedGaussian {
  std::array<float, 3> position;  // x, y, z
  std::array<float, 4> rotation;  // x, y, z, w
  std::array<float, 3> scale;     // std::log(scale)
  std::array<float, 3> color;     // rgb sh0 encoding
  float alpha;                    // inverse logistic
};

// Represents a single low precision gaussian. Each gaussian has exactly 64 bytes, even if it does
// not have full spherical harmonics.
struct PackedGaussian {
  std::array<uint8_t, 9> position{};
  std::array<uint8_t, 3> rotation{};
  std::array<uint8_t, 3> scale{};
  std::array<uint8_t, 3> color{};
  uint8_t alpha = 0;
  UnpackedGaussian unpack(bool usesFloat16, int fractionalBits) const;
};

// Represents a full splat with lower precision. Each splat has at most 64 bytes, although splats
// with fewer spherical harmonics degrees will have less. The data is stored non-interleaved.
struct PackedGaussians {
  int numPoints = 0;        // Total number of points (gaussians)
  int fractionalBits = 0;   // Number of bits used for fractional part of fixed-point coords

  std::vector<uint8_t> positions;
  std::vector<uint8_t> scales;
  std::vector<uint8_t> rotations;
  std::vector<uint8_t> alphas;
  std::vector<uint8_t> colors;

  bool usesFloat16() const;
  PackedGaussian at(int i) const;
  UnpackedGaussian unpack(int i) const;
};

using Half = uint16_t;

// Half-precision helpers.
float halfToFloat(Half h);
Half floatToHalf(float f);

// ---------------------------------------------------------------------------
// [ENHANCEMENT] 4DGS temporal extension block (OpenSplat4D-only, appended after
// the standard SPZ stream). These are the fields that make a static 3DGS point
// become a time-varying 4DGS point; they are stored separately so the core SPZ
// layout stays byte-compatible with Niantic's reference decoder.
// ---------------------------------------------------------------------------
struct Packed4DExtra {
  float AnchorTime = 0.f;        // gaussian center time t
  float TimeVariance = 1e10f;    // exp(scaling_t); larger => visible over a wider time window
  float Velocity[3] = {0.f, 0.f, 0.f}; // world units / unit time
  uint8_t bUseVelocity = 0;      // whether Velocity is applied at render time
};

// [ENHANCEMENT] Compress a cloud of OpenSplat4D points.
//   * g            : the source point cloud (3DGS or 4DGS).
//   * compressionLevel / workers : forwarded to zlib (matches reference API).
//   * bInclude4D   : when true (default), an O4D4 block is appended *only if* the
//                    cloud actually carries 4D data, keeping static clouds minimal.
// This is strictly richer than the reference Spz::compress, which only accepted
// FGaussianSplattingPoint (static) and had no 4D concept.
OPENSPLAT4DRUNTIME_API bool compress(
	const TArray<FOpenSplat4DPoint> g,
	int compressionLevel,
	int workers,
	std::vector<uint8_t>& output,
	bool bInclude4D = true);

// [ENHANCEMENT] Decompress a (possibly 4D-extended) SPZ stream back into
// TArray<FOpenSplat4DPoint>. The 4D block, if present, is applied automatically;
// if absent (e.g. a standard Niantic .spz file), the temporal fields keep their
// safe 3DGS defaults (AnchorTime=0, TimeVariance=1e10, no velocity), so the
// decoder also reads plain SPZ files.
OPENSPLAT4DRUNTIME_API bool decompress(
	const std::span<const uint8_t> input,
    TArray<FOpenSplat4DPoint>& output);

}  // namespace Spz
