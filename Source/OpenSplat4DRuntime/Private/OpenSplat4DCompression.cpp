#include "OpenSplat4DCompression.h"
#include <zlib.h>

namespace FOpenSplat4DCompression
{
	bool Compress(const TArray<uint8>& InRaw, TArray<uint8>& OutCompressed)
	{
		if (InRaw.Num() == 0)
		{
			OutCompressed.Empty();
			return true;
		}

		// Upper bound on the compressed size.
		uLongf DestLen = compressBound(static_cast<uLong>(InRaw.Num()));
		OutCompressed.SetNumUninitialized(static_cast<int32>(DestLen));

		const int ZResult = compress(
			OutCompressed.GetData(),
			&DestLen,
			InRaw.GetData(),
			static_cast<uLong>(InRaw.Num()));

		if (ZResult != Z_OK)
		{
			OutCompressed.Empty();
			return false;
		}

		OutCompressed.SetNum(static_cast<int32>(DestLen));
		return true;
	}

	bool Decompress(const TArray<uint8>& InCompressed, TArray<uint8>& OutRaw)
	{
		if (InCompressed.Num() == 0)
		{
			OutRaw.Empty();
			return true;
		}

		// Standard zlib stream expansion strategy: keep doubling the output buffer.
		uLongf DestLen = static_cast<uLong>(InCompressed.Num()) * 4 + 1024;
		TArray<uint8> Scratch;
		Scratch.SetNumUninitialized(static_cast<int32>(DestLen));

		int ZResult = uncompress(
			Scratch.GetData(),
			&DestLen,
			InCompressed.GetData(),
			static_cast<uLong>(InCompressed.Num()));

		while (ZResult == Z_BUF_ERROR)
		{
			DestLen *= 2;
			Scratch.SetNumUninitialized(static_cast<int32>(DestLen));
			ZResult = uncompress(
				Scratch.GetData(),
				&DestLen,
				InCompressed.GetData(),
				static_cast<uLong>(InCompressed.Num()));
		}

		if (ZResult != Z_OK)
		{
			OutRaw.Empty();
			return false;
		}

		OutRaw = MoveTemp(Scratch);
		OutRaw.SetNum(static_cast<int32>(DestLen));
		return true;
	}
}
