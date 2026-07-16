#pragma once

#include "CoreMinimal.h"

/**
 * Minimal zlib helpers for compressing / decompressing raw byte buffers.
 * Used by UOpenSplat4DPointCloud asset serialization (EOpenSplat4DCompressionMethod::Zlib).
 *
 * [NOTE] For new code prefer the Niantic SPZ format in Private/Compression/Spz.h: it
 * quantizes each gaussian to 64 bytes first (so it is ~6-10x smaller) and is 4DGS-aware.
 * This raw-blob helper is kept for backward compatibility with existing Zlib assets.
 */
namespace FOpenSplat4DCompression
{
	/** Compress InRaw into OutCompressed. Returns false on error. */
	bool Compress(const TArray<uint8>& InRaw, TArray<uint8>& OutCompressed);

	/** Decompress InCompressed (produced by Compress) into OutRaw. Returns false on error. */
	bool Decompress(const TArray<uint8>& InCompressed, TArray<uint8>& OutRaw);
}
