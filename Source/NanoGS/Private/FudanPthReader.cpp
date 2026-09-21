// Copyright Epic Games, Inc. All Rights Reserved.

#include "FudanPthReader.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMemory.h"
#include "Misc/Compression.h"

// ============================================================================
// Fudan 4DGS torch.save checkpoint reader.
//
// torch.save (PyTorch >= 1.6) writes a ZIP archive (ZIP_STORED entries):
//   <archive>/data.pkl        pickle protocol 2 stream
//   <archive>/data/<key>      raw little-endian float32 tensor bytes
//   <archive>/version         "3"
//   <archive>/byteorder       "little"
// Tensors appear in the pickle stream as REDUCE calls of
// torch._utils._rebuild_tensor_v2 / _rebuild_parameter whose first argument is
// a BINPERSID tuple ('storage', <Storage class>, '<key>', '<location>', numel).
// We run a minimal stack-machine pickle interpreter (protocol 2 + a few
// protocol 4/5 opcodes for robustness), collect every named tensor reference,
// and read the storage bytes from the matching zip entries.
// ============================================================================

namespace
{
	// ---------------------------------------------------------------------
	// Minimal zip (central directory) reader -- STORED + DEFLATE entries.
	// ---------------------------------------------------------------------

	constexpr uint32 LocalHeaderSignature = 0x04034b50;   // "PK\x03\x04"
	constexpr uint32 CentralHeaderSignature = 0x02014b50; // "PK\x01\x02"
	constexpr uint32 EocdSignature = 0x06054b50;          // "PK\x05\x06"
	constexpr uint32 Eocd64Signature = 0x06064b50;        // "PK\x06\x06"
	constexpr uint32 Eocd64LocatorSignature = 0x07064b50; // "PK\x06\x07"

	struct FZipEntry
	{
		FString Name;
		int64 LocalHeaderOffset = 0;
		uint64 CompressedSize = 0;
		uint64 UncompressedSize = 0;
		uint16 Method = 0;
	};

	uint16 ReadU16(const uint8* P)
	{
		return static_cast<uint16>(P[0] | (P[1] << 8));
	}

	uint32 ReadU32(const uint8* P)
	{
		return static_cast<uint32>(P[0]) | (static_cast<uint32>(P[1]) << 8) |
			(static_cast<uint32>(P[2]) << 16) | (static_cast<uint32>(P[3]) << 24);
	}

	uint64 ReadU64(const uint8* P)
	{
		return static_cast<uint64>(ReadU32(P)) | (static_cast<uint64>(ReadU32(P + 4)) << 32);
	}

	// zip DEFLATE (method 8) stores a raw deflate stream; UE's zlib inflate
	// expects a zlib-format stream, so wrap it (2-byte header + adler32 tail).
	uint32 ComputeAdler32(const uint8* Data, int64 Size)
	{
		uint32 A = 1, B = 0;
		for (int64 i = 0; i < Size; i++)
		{
			A = (A + Data[i]) % 65521u;
			B = (B + A) % 65521u;
		}
		return (B << 16) | A;
	}

	FString Utf8ToString(const uint8* Data, int64 Size)
	{
		FString Result;
		if (Size <= 0)
		{
			return Result;
		}
		FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Data), static_cast<int32>(Size));
		Result.AppendChars(Conv.Get(), Conv.Length());
		return Result;
	}

	bool ParseCentralDirectory(IFileHandle* FileHandle, TArray<FZipEntry>& OutEntries, FString& OutError)
	{
		const int64 FileSize = FileHandle->Size();

		// Locate the end-of-central-directory record (scan the last 64 KB + 22).
		const int64 ScanStart = FMath::Max<int64>(0, FileSize - (66 * 1024));
		const int64 ScanLen = FileSize - ScanStart;
		TArray<uint8> Tail;
		Tail.SetNumUninitialized(ScanLen);
		FileHandle->Seek(ScanStart);
		if (!FileHandle->Read(Tail.GetData(), ScanLen))
		{
			OutError = TEXT("无法读取 zip 文件尾部（EOCD 定位失败）。");
			return false;
		}

		int64 EocdPos = -1;
		for (int64 i = ScanLen - 22; i >= 0; i--)
		{
			if (Tail[i] == 0x50 && Tail[i + 1] == 0x4b && Tail[i + 2] == 0x05 && Tail[i + 3] == 0x06)
			{
				EocdPos = i;
				break;
			}
		}
		if (EocdPos < 0)
		{
			OutError = TEXT("不是有效的 zip 文件（未找到 central directory）。");
			return false;
		}

		const uint8* Eocd = Tail.GetData() + EocdPos;
		uint64 NumEntries = ReadU16(Eocd + 10);
		uint64 CentralOffset = ReadU32(Eocd + 16);
		uint64 CentralSize = ReadU32(Eocd + 20);

		// ZIP64: the classic EOCD fields saturate to 0xFF..; the real values
		// live in the zip64 EOCD record pointed to by the 20-byte locator that
		// immediately precedes the EOCD.
		if (CentralOffset == 0xFFFFFFFFu || NumEntries == 0xFFFFu || CentralSize == 0xFFFFFFFFu)
		{
			const int64 LocPos = EocdPos - 20;
			if (LocPos >= 0 && Tail[LocPos] == 0x50 && Tail[LocPos + 1] == 0x4b &&
				Tail[LocPos + 2] == 0x06 && Tail[LocPos + 3] == 0x07)
			{
				const uint64 Eocd64Offset = ReadU64(Tail.GetData() + LocPos + 8);
				uint8 Buf[56];
				FileHandle->Seek(Eocd64Offset);
				if (!FileHandle->Read(Buf, sizeof(Buf)) || ReadU32(Buf) != Eocd64Signature)
				{
					OutError = TEXT("zip64 EOCD 读取失败。");
					return false;
				}
				NumEntries = ReadU64(Buf + 32);
				CentralSize = ReadU64(Buf + 40);
				CentralOffset = ReadU64(Buf + 48);
			}
			else
			{
				OutError = TEXT("zip64 locator 缺失，无法解析超大 zip 文件。");
				return false;
			}
		}

		if (NumEntries == 0 || NumEntries > 1000000)
		{
			OutError = FString::Printf(TEXT("zip 条目数异常（%llu）。"), NumEntries);
			return false;
		}

		TArray<uint8> Central;
		Central.SetNumUninitialized(CentralSize);
		FileHandle->Seek(CentralOffset);
		if (!FileHandle->Read(Central.GetData(), CentralSize))
		{
			OutError = TEXT("central directory 读取失败。");
			return false;
		}

		int64 Pos = 0;
		for (uint64 n = 0; n < NumEntries && Pos + 46 <= static_cast<int64>(CentralSize); n++)
		{
			const uint8* C = Central.GetData() + Pos;
			if (ReadU32(C) != CentralHeaderSignature)
			{
				OutError = TEXT("central directory 损坏（条目签名不符）。");
				return false;
			}

			const uint16 Flags = ReadU16(C + 8);
			const uint16 Method = ReadU16(C + 10);
			uint64 CSize = ReadU32(C + 20);
			uint64 USize = ReadU32(C + 24);
			const uint16 NameLen = ReadU16(C + 28);
			const uint16 ExtraLen = ReadU16(C + 30);
			const uint16 CommentLen = ReadU16(C + 32);
			uint64 Lho = ReadU32(C + 42);

			// ZIP64 extra field (header id 0x0001): fields appear in the fixed
			// order uncompressed(8) / compressed(8) / local-header-offset(8),
			// only for the values that saturated in the fixed-size record.
			if (USize == 0xFFFFFFFFu || CSize == 0xFFFFFFFFu || Lho == 0xFFFFFFFFu)
			{
				const uint8* X = C + 46 + NameLen;
				int64 XRemain = ExtraLen;
				auto Take64 = [&X, &XRemain]() -> uint64
				{
					if (XRemain < 8)
					{
						return 0;
					}
					const uint64 V = ReadU64(X);
					X += 8;
					XRemain -= 8;
					return V;
				};
				while (XRemain >= 4)
				{
					const uint16 HeaderId = ReadU16(X);
					const uint16 HeaderSize = ReadU16(X + 2);
					if (HeaderId == 0x0001)
					{
						X += 4;
						XRemain -= 4;
						if (USize == 0xFFFFFFFFu) { USize = Take64(); }
						if (CSize == 0xFFFFFFFFu) { CSize = Take64(); }
						if (Lho == 0xFFFFFFFFu) { Lho = Take64(); }
					}
					X += HeaderSize;
					XRemain -= HeaderSize;
				}
			}

			if ((Flags & 0x1) == 0) // skip encrypted entries
			{
				FZipEntry Entry;
				Entry.Name = Utf8ToString(C + 46, NameLen);
				Entry.LocalHeaderOffset = static_cast<int64>(Lho);
				Entry.CompressedSize = CSize;
				Entry.UncompressedSize = USize;
				Entry.Method = Method;
				OutEntries.Add(MoveTemp(Entry));
			}

			Pos += 46 + static_cast<int64>(NameLen) + ExtraLen + CommentLen;
		}

		return true;
	}

	bool ReadZipEntry(IFileHandle* FileHandle, const FZipEntry& Entry, TArray<uint8>& OutData, FString& OutError)
	{
		uint8 Header[30];
		FileHandle->Seek(Entry.LocalHeaderOffset);
		if (!FileHandle->Read(Header, sizeof(Header)) || ReadU32(Header) != LocalHeaderSignature)
		{
			OutError = FString::Printf(TEXT("zip 条目 local header 读取失败：%s"), *Entry.Name);
			return false;
		}
		const uint16 NameLen = ReadU16(Header + 26);
		const uint16 ExtraLen = ReadU16(Header + 28);
		const int64 DataOffset = Entry.LocalHeaderOffset + 30 + NameLen + ExtraLen;

		if (Entry.Method == 0) // STORED (what torch.save always writes)
		{
			OutData.SetNumUninitialized(static_cast<int32>(Entry.UncompressedSize));
			FileHandle->Seek(DataOffset);
			if (!FileHandle->Read(OutData.GetData(), static_cast<int32>(Entry.UncompressedSize)))
			{
				OutError = FString::Printf(TEXT("zip 条目数据读取失败：%s"), *Entry.Name);
				return false;
			}
			return true;
		}

		if (Entry.Method == 8) // DEFLATE (rare: repacked archives)
		{
			TArray<uint8> Compressed;
			Compressed.SetNumUninitialized(static_cast<int32>(Entry.CompressedSize));
			FileHandle->Seek(DataOffset);
			if (!FileHandle->Read(Compressed.GetData(), static_cast<int32>(Entry.CompressedSize)))
			{
				OutError = FString::Printf(TEXT("zip 条目数据读取失败：%s"), *Entry.Name);
				return false;
			}

			// Wrap the raw deflate stream into a zlib-format stream for UE's
			// zlib inflate (2-byte header + big-endian adler32 trailer).
			const uint32 Adler = ComputeAdler32(Compressed.GetData(), Compressed.Num());
			TArray<uint8> ZlibStream;
			ZlibStream.Reserve(Compressed.Num() + 6);
			ZlibStream.Add(0x78);
			ZlibStream.Add(0x01);
			ZlibStream.Append(Compressed);
			ZlibStream.Add(static_cast<uint8>((Adler >> 24) & 0xFF));
			ZlibStream.Add(static_cast<uint8>((Adler >> 16) & 0xFF));
			ZlibStream.Add(static_cast<uint8>((Adler >> 8) & 0xFF));
			ZlibStream.Add(static_cast<uint8>(Adler & 0xFF));

			OutData.SetNumUninitialized(static_cast<int32>(Entry.UncompressedSize));
			if (!FCompression::UncompressMemory(NAME_Zlib, OutData.GetData(), OutData.Num(), ZlibStream.GetData(), ZlibStream.Num()))
			{
				OutError = FString::Printf(TEXT("zip 条目 deflate 解压失败：%s"), *Entry.Name);
				return false;
			}
			return true;
		}

		OutError = FString::Printf(TEXT("不支持的 zip 压缩方法（method=%d，条目 %s）。"), Entry.Method, *Entry.Name);
		return false;
	}

	// ---------------------------------------------------------------------
	// Minimal pickle (protocol 2, plus protocol 4/5 opcodes) interpreter.
	// ---------------------------------------------------------------------

	struct FPthTensorRef
	{
		FString StorageKey;
		int64 Numel = 0;
		TArray<int64> Shape;
	};

	enum class EPthKind : uint8
	{
		None,
		Bool,
		Int,
		Float,
		Str,
		Bytes,
		List,
		Tuple,
		Dict,
		Callable,   // global reference "Module.Name"
		StorageRef, // BINPERSID ('storage', <class>, key, location, numel)
		TensorRef   // result of _rebuild_tensor_v2 / _rebuild_parameter
	};

	struct FPthValue;
	using FPthValuePtr = TSharedPtr<FPthValue>;

	struct FPthValue
	{
		EPthKind Kind = EPthKind::None;
		bool bBool = false;
		int64 Int = 0;
		double Float = 0.0;
		FString Str;
		TArray<uint8> Bytes;
		TArray<FPthValuePtr> Items;       // List / Tuple
		TMap<FString, FPthValuePtr> Map;  // Dict (keys stringified)
		FString Module;                   // Callable
		FString Name;                     // Callable
		FPthTensorRef Tensor;             // StorageRef / TensorRef
	};

	static FPthValuePtr NewValue(EPthKind Kind)
	{
		FPthValuePtr V = MakeShared<FPthValue>();
		V->Kind = Kind;
		return V;
	}

	// Stringify any scalar dict key. Only the key kinds torch actually writes
	// are supported (strings for tensor names, ints for optimizer state).
	FString DictKeyOf(const FPthValuePtr& V, FString& OutError)
	{
		switch (V->Kind)
		{
		case EPthKind::Str: return V->Str;
		case EPthKind::Int: return FString::Printf(TEXT("%lld"), V->Int);
		case EPthKind::Bool: return V->bBool ? TEXT("True") : TEXT("False");
		case EPthKind::Float: return FString::Printf(TEXT("%f"), V->Float);
		case EPthKind::None: return TEXT("None");
		default:
			OutError = TEXT("pickle 中出现不支持的 dict 键类型（tuple/list 作键）。");
			return FString();
		}
	}

	int64 IntOf(const FPthValuePtr& V)
	{
		switch (V->Kind)
		{
		case EPthKind::Int: return V->Int;
		case EPthKind::Bool: return V->bBool ? 1 : 0;
		default: return 0;
		}
	}

	class FPickleInterpreter
	{
	public:
		bool Run(const uint8* Data, int64 Size, FString& OutError)
		{
			Data_ = Data;
			Size_ = Size;
			Pos_ = 0;
			bFailed = false;
			Error.Empty();

			while (!bFailed && Pos_ < Size_)
			{
				const uint8 Op = Data_[Pos_++];
				Step(Op);
			}

			if (!bFailed && Stack.Num() != 1)
			{
				Fail(TEXT("pickle 流解析后栈元素数异常。"));
			}
			if (!bFailed)
			{
				TopValue = Stack[0];
			}
			OutError = Error;
			return !bFailed;
		}

		/** Every (string key -> tensor) assignment observed in any dict. */
		TMap<FString, FPthTensorRef> NamedTensors;

		/** Top-level object (for tuple-based capture() fallback detection). */
		FPthValuePtr TopValue;

	private:
		// Safety limits against malformed/hostile files.
		static constexpr int64 MaxStack = 2000000;
		static constexpr int32 MaxMarkDepth = 10000;
		static constexpr int64 MaxMemo = 1000000;
		static constexpr int64 MaxStringLength = 16 * 1024 * 1024;

		void Fail(const FString& Message)
		{
			if (!bFailed)
			{
				bFailed = true;
				Error = Message;
			}
		}

		void Push(FPthValuePtr V)
		{
			if (Stack.Num() >= MaxStack)
			{
				Fail(TEXT("pickle 栈深度超限（文件可能已损坏）。"));
				return;
			}
			Stack.Push(MoveTemp(V));
		}

		FPthValuePtr Pop()
		{
			if (Stack.Num() == 0)
			{
				Fail(TEXT("pickle 栈下溢（文件可能已损坏）。"));
				return NewValue(EPthKind::None);
			}
			FPthValuePtr V = Stack.Pop();
			return V;
		}

		int64 ReadBytes(int64 Count, const uint8*& OutPtr)
		{
			if (Pos_ + Count > Size_)
			{
				Fail(TEXT("pickle 流被截断。"));
				return -1;
			}
			OutPtr = Data_ + Pos_;
			Pos_ += Count;
			return Count;
		}

		FString ReadString(int64 Length)
		{
			if (Length < 0 || Length > MaxStringLength)
			{
				Fail(TEXT("pickle 字符串长度超限。"));
				return FString();
			}
			const uint8* Ptr = nullptr;
			if (ReadBytes(Length, Ptr) < 0)
			{
				return FString();
			}
			return Utf8ToString(Ptr, Length);
		}

		FString ReadLine()
		{
			int64 End = Pos_;
			while (End < Size_ && Data_[End] != '\n')
			{
				End++;
			}
			if (End >= Size_)
			{
				Fail(TEXT("pickle 流被截断（缺少换行）。"));
				return FString();
			}
			const FString Line = Utf8ToString(Data_ + Pos_, End - Pos_);
			Pos_ = End + 1;
			return Line;
		}

		void Step(uint8 Op)
		{
			switch (Op)
			{
			// ---- framing / no-ops ------------------------------------------
			case 0x80: // PROTO
			{
				const uint8* P = nullptr;
				ReadBytes(1, P);
				break;
			}
			case 0x95: // FRAME
			{
				const uint8* P = nullptr;
				ReadBytes(8, P);
				break;
			}

			// ---- containers -------------------------------------------------
			case '}': // EMPTY_DICT
				Push(NewValue(EPthKind::Dict));
				break;
			case ']': // EMPTY_LIST
				Push(NewValue(EPthKind::List));
				break;
			case ')': // EMPTY_TUPLE
				Push(NewValue(EPthKind::Tuple));
				break;
			case '(': // MARK
				if (Marks.Num() >= MaxMarkDepth)
				{
					Fail(TEXT("pickle MARK 深度超限。"));
					return;
				}
				Marks.Push(Stack.Num());
				break;
			case 't': // TUPLE
			{
				if (Marks.Num() == 0)
				{
					Fail(TEXT("pickle TUPLE 缺少 MARK。"));
					return;
				}
				const int32 Start = Marks.Pop();
				FPthValuePtr Tuple = NewValue(EPthKind::Tuple);
				Tuple->Items.Append(Stack.GetData() + Start, Stack.Num() - Start);
				Stack.SetNum(Start);
				Push(Tuple);
				break;
			}
			case 0x85: // TUPLE1
			{
				FPthValuePtr A = Pop();
				FPthValuePtr Tuple = NewValue(EPthKind::Tuple);
				Tuple->Items.Add(A);
				Push(Tuple);
				break;
			}
			case 0x86: // TUPLE2
			{
				FPthValuePtr B = Pop();
				FPthValuePtr A = Pop();
				FPthValuePtr Tuple = NewValue(EPthKind::Tuple);
				Tuple->Items.Add(A);
				Tuple->Items.Add(B);
				Push(Tuple);
				break;
			}
			case 0x87: // TUPLE3
			{
				FPthValuePtr C = Pop();
				FPthValuePtr B = Pop();
				FPthValuePtr A = Pop();
				FPthValuePtr Tuple = NewValue(EPthKind::Tuple);
				Tuple->Items.Add(A);
				Tuple->Items.Add(B);
				Tuple->Items.Add(C);
				Push(Tuple);
				break;
			}
			case 'a': // APPEND
			{
				FPthValuePtr V = Pop();
				FPthValuePtr List = Pop();
				if (List->Kind != EPthKind::List)
				{
					Fail(TEXT("pickle APPEND 目标不是 list。"));
					return;
				}
				List->Items.Add(V);
				Push(List);
				break;
			}
			case 'e': // APPENDS
			{
				if (Marks.Num() == 0)
				{
					Fail(TEXT("pickle APPENDS 缺少 MARK。"));
					return;
				}
				const int32 Start = Marks.Pop();
				const int32 Count = Stack.Num() - Start;
				// 容器在 MARK 下方（Stack[Start-1]），mark 区域内只有待追加的元素。
				if (Start < 1)
				{
					Fail(TEXT("pickle APPENDS MARK 下方缺少 list。"));
					return;
				}
				FPthValuePtr List = Stack[Start - 1];
				if (List->Kind != EPthKind::List)
				{
					Fail(TEXT("pickle APPENDS 目标不是 list。"));
					return;
				}
				List->Items.Append(Stack.GetData() + Start, Count);
				Stack.SetNum(Start - 1);
				Push(List);
				break;
			}
			case 's': // SETITEM
			{
				FPthValuePtr V = Pop();
				FPthValuePtr K = Pop();
				FPthValuePtr Dict = Pop();
				StoreDictItem(Dict, K, V);
				Push(Dict);
				break;
			}
			case 'u': // SETITEMS
			{
				if (Marks.Num() == 0)
				{
					Fail(TEXT("pickle SETITEMS 缺少 MARK。"));
					return;
				}
				const int32 Start = Marks.Pop();
				const int32 Count = Stack.Num() - Start;
				if (Count % 2 != 0)
				{
					Fail(TEXT("pickle SETITEMS 键值不成对。"));
					return;
				}
				// dict 在 MARK 下方（Stack[Start-1]），mark 区域内只有键值对。
				if (Start < 1)
				{
					Fail(TEXT("pickle SETITEMS MARK 下方缺少 dict。"));
					return;
				}
				FPthValuePtr Dict = Stack[Start - 1];
				if (Dict->Kind != EPthKind::Dict)
				{
					Fail(TEXT("pickle SETITEMS 目标不是 dict。"));
					return;
				}
				for (int32 i = 0; i < Count; i += 2)
				{
					StoreDictItem(Dict, Stack[Start + i], Stack[Start + i + 1]);
				}
				Stack.SetNum(Start - 1);
				Push(Dict);
				break;
			}

			// ---- scalars ----------------------------------------------------
			case 'X': // BINUNICODE (4-byte length)
			{
				const uint8* P = nullptr;
				if (ReadBytes(4, P) < 0) { return; }
				PushString(ReadString(static_cast<int64>(ReadU32(P))));
				break;
			}
			case 0x8c: // SHORT_BINUNICODE (1-byte length)
			{
				const uint8* P = nullptr;
				if (ReadBytes(1, P) < 0) { return; }
				PushString(ReadString(P[0]));
				break;
			}
			case 0x8d: // BINUNICODE8 (8-byte length, protocol 4)
			{
				const uint8* P = nullptr;
				if (ReadBytes(8, P) < 0) { return; }
				PushString(ReadString(static_cast<int64>(ReadU64(P))));
				break;
			}
			case 'B': // BINBYTES (4-byte length)
			{
				const uint8* P = nullptr;
				if (ReadBytes(4, P) < 0) { return; }
				AppendBytes(ReadBytesBytes(static_cast<int64>(ReadU32(P))));
				break;
			}
			case 'C': // SHORT_BINBYTES (1-byte length)
			{
				const uint8* P = nullptr;
				if (ReadBytes(1, P) < 0) { return; }
				AppendBytes(ReadBytesBytes(P[0]));
				break;
			}
			case 0x8e: // BINBYTES8 (protocol 4)
			{
				const uint8* P = nullptr;
				if (ReadBytes(8, P) < 0) { return; }
				AppendBytes(ReadBytesBytes(static_cast<int64>(ReadU64(P))));
				break;
			}
			case 0x96: // BYTEARRAY8 (protocol 5)
			{
				const uint8* P = nullptr;
				if (ReadBytes(8, P) < 0) { return; }
				AppendBytes(ReadBytesBytes(static_cast<int64>(ReadU64(P))));
				break;
			}
			case 'K': // BININT1
			{
				const uint8* P = nullptr;
				if (ReadBytes(1, P) < 0) { return; }
				PushInt(P[0]);
				break;
			}
			case 'M': // BININT2
			{
				const uint8* P = nullptr;
				if (ReadBytes(2, P) < 0) { return; }
				PushInt(ReadU16(P));
				break;
			}
			case 'J': // BININT (signed 32-bit)
			{
				const uint8* P = nullptr;
				if (ReadBytes(4, P) < 0) { return; }
				PushInt(static_cast<int32>(ReadU32(P)));
				break;
			}
			case 0x8a: // LONG1
			{
				const uint8* P = nullptr;
				if (ReadBytes(1, P) < 0) { return; }
				PushInt(ReadSignedLong(P[0]));
				break;
			}
			case 0x8b: // LONG4
			{
				const uint8* P = nullptr;
				if (ReadBytes(4, P) < 0) { return; }
				PushInt(ReadSignedLong(static_cast<int64>(ReadU32(P))));
				break;
			}
			case 'G': // BINFLOAT (8-byte big-endian double)
			{
				const uint8* P = nullptr;
				if (ReadBytes(8, P) < 0) { return; }
				uint64 Bits = 0;
				for (int32 i = 0; i < 8; i++)
				{
					Bits = (Bits << 8) | P[i];
				}
				double D;
				FMemory::Memcpy(&D, &Bits, sizeof(D));
				FPthValuePtr V = NewValue(EPthKind::Float);
				V->Float = D;
				Push(V);
				break;
			}
			case 0x88: // NEWTRUE
			{
				FPthValuePtr V = NewValue(EPthKind::Bool);
				V->bBool = true;
				Push(V);
				break;
			}
			case 0x89: // NEWFALSE
			{
				FPthValuePtr V = NewValue(EPthKind::Bool);
				V->bBool = false;
				Push(V);
				break;
			}
			case 'N': // NONE
				Push(NewValue(EPthKind::None));
				break;

			// ---- memo -------------------------------------------------------
			case 'q': // BINPUT (1-byte index)
			{
				const uint8* P = nullptr;
				if (ReadBytes(1, P) < 0) { return; }
				Memoize(static_cast<int64>(P[0]));
				break;
			}
			case 'r': // LONG_BINPUT (4-byte index)
			{
				const uint8* P = nullptr;
				if (ReadBytes(4, P) < 0) { return; }
				Memoize(static_cast<int64>(ReadU32(P)));
				break;
			}
			case 0x94: // MEMOIZE (protocol 4)
				Memoize(NextMemoId++);
				break;
			case 'h': // BINGET (1-byte index)
			{
				const uint8* P = nullptr;
				if (ReadBytes(1, P) < 0) { return; }
				LoadMemo(static_cast<int64>(P[0]));
				break;
			}
			case 'j': // LONG_BINGET (4-byte index)
			{
				const uint8* P = nullptr;
				if (ReadBytes(4, P) < 0) { return; }
				LoadMemo(static_cast<int64>(ReadU32(P)));
				break;
			}
			case 'g': // GET (protocol 0, decimal index + newline)
			{
				const FString IdxStr = ReadLine();
				LoadMemo(FCString::Atoi64(*IdxStr));
				break;
			}
			case 'p': // PUT (protocol 0)
				ReadLine(); // applies to stack top; torch streams don't use it
				break;

			// ---- globals / callables ---------------------------------------
			case 'c': // GLOBAL ("module\nname\n")
			{
				const FString Module = ReadLine();
				const FString Name = ReadLine();
				if (bFailed) { return; }
				FPthValuePtr V = NewValue(EPthKind::Callable);
				V->Module = Module;
				V->Name = Name;
				Push(V);
				break;
			}
			case 0x93: // STACK_GLOBAL (protocol 4)
			{
				FPthValuePtr Name = Pop();
				FPthValuePtr Module = Pop();
				if (bFailed) { return; }
				FPthValuePtr V = NewValue(EPthKind::Callable);
				if (Module->Kind == EPthKind::Str && Name->Kind == EPthKind::Str)
				{
					V->Module = Module->Str;
					V->Name = Name->Str;
				}
				Push(V);
				break;
			}
			case 'R': // REDUCE
			{
				FPthValuePtr Args = Pop();
				FPthValuePtr Fn = Pop();
				if (bFailed) { return; }
				ApplyReduce(Fn, Args);
				break;
			}
			case 'b': // BUILD
			{
				FPthValuePtr State = Pop();
				FPthValuePtr Obj = Pop();
				if (bFailed) { return; }
				// collections.OrderedDict pickles with a BUILD state dict in
				// some Python versions; merge it into the dict container.
				if (Obj->Kind == EPthKind::Dict && State->Kind == EPthKind::Dict)
				{
					for (const TPair<FString, FPthValuePtr>& KV : State->Map)
					{
						Obj->Map.Add(KV.Key, KV.Value);
					}
				}
				Push(Obj);
				break;
			}

			// ---- persistent ids (torch storages) ----------------------------
			case 'Q': // BINPERSID
			{
				FPthValuePtr V = Pop();
				if (bFailed) { return; }
				ApplyBinPersId(V);
				break;
			}
			case 'T': // BINSTRING (legacy counted string; torch streams don't use it)
			{
				const uint8* P = nullptr;
				if (ReadBytes(4, P) < 0) { return; }
				PushString(ReadString(static_cast<int64>(ReadU32(P))));
				break;
			}
			case 'P': // PERSID (protocol 0, string id) -- not usable without the
					  // pickler's persistent_load; torch does not emit it.
				Fail(TEXT("pickle PERSID 不支持（torch checkpoint 应使用 BINPERSID）。"));
				break;

			case '.': // STOP
				// Loop condition consumes the rest; nothing to do.
				Pos_ = Size_;
				break;

			default:
				Fail(FString::Printf(TEXT("不支持的 pickle 操作码 0x%02X（声明：仅支持 torch.save 新格式）。"), Op));
				break;
			}
		}

		// ---- helpers --------------------------------------------------------

		void PushString(const FString& S)
		{
			if (bFailed) { return; }
			FPthValuePtr V = NewValue(EPthKind::Str);
			V->Str = S;
			Push(V);
		}

		void PushInt(int64 I)
		{
			if (bFailed) { return; }
			FPthValuePtr V = NewValue(EPthKind::Int);
			V->Int = I;
			Push(V);
		}

		TArray<uint8> ReadBytesBytes(int64 Length)
		{
			TArray<uint8> Out;
			if (Length < 0 || Length > MaxStringLength)
			{
				Fail(TEXT("pickle bytes 长度超限。"));
				return Out;
			}
			const uint8* Ptr = nullptr;
			if (ReadBytes(Length, Ptr) < 0)
			{
				return Out;
			}
			Out.Append(Ptr, static_cast<int32>(Length));
			return Out;
		}

		void AppendBytes(TArray<uint8>&& B)
		{
			if (bFailed) { return; }
			FPthValuePtr V = NewValue(EPthKind::Bytes);
			V->Bytes = MoveTemp(B);
			Push(V);
		}

		int64 ReadSignedLong(int64 NumBytes)
		{
			const uint8* P = nullptr;
			if (ReadBytes(NumBytes, P) < 0)
			{
				return 0;
			}
			int64 Result = 0;
			for (int64 i = 0; i < NumBytes; i++)
			{
				Result |= static_cast<int64>(P[i]) << (8 * i);
			}
			// sign-extend from the top payload byte
			const int64 SignBit = static_cast<int64>(1) << (8 * NumBytes - 1);
			if (NumBytes > 0 && (Result & SignBit))
			{
				Result -= static_cast<int64>(1) << (8 * NumBytes);
			}
			return Result;
		}

		void Memoize(int64 Index)
		{
			if (bFailed || Stack.Num() == 0)
			{
				return;
			}
			if (Memo.Num() >= MaxMemo)
			{
				Fail(TEXT("pickle memo 超限。"));
				return;
			}
			Memo.Add(Index, Stack.Top());
		}

		void LoadMemo(int64 Index)
		{
			if (bFailed) { return; }
			const FPthValuePtr* Found = Memo.Find(Index);
			if (!Found || !*Found)
			{
				Fail(FString::Printf(TEXT("pickle memo 引用缺失（index=%lld）。"), Index));
				return;
			}
			Push(*Found);
		}

		void StoreDictItem(const FPthValuePtr& Dict, const FPthValuePtr& K, const FPthValuePtr& V)
		{
			if (Dict->Kind != EPthKind::Dict)
			{
				Fail(TEXT("pickle SETITEM 目标不是 dict。"));
				return;
			}
			FString Err;
			const FString Key = DictKeyOf(K, Err);
			if (!Err.IsEmpty())
			{
				Fail(Err);
				return;
			}
			Dict->Map.Add(Key, V);
			// Collect every named tensor reference (the state-dict path).
			if (V.IsValid() && V->Kind == EPthKind::TensorRef)
			{
				NamedTensors.Add(Key, V->Tensor);
			}
		}

		// BINPERSID: the stack top must be the ('storage', <Storage class>,
		// '<key>', '<location>', numel) tuple that torch's persistent_id emits.
		void ApplyBinPersId(const FPthValuePtr& Tuple)
		{
			if (Tuple->Kind != EPthKind::Tuple || Tuple->Items.Num() < 5)
			{
				Fail(TEXT("torch storage persistent id 结构异常。"));
				return;
			}
			if (Tuple->Items[0]->Kind != EPthKind::Str || Tuple->Items[0]->Str != TEXT("storage"))
			{
				Fail(TEXT("torch persistent id 不是 storage 类型。"));
				return;
			}

			// Storage dtype check: FloatStorage (and UntypedStorage carrying
			// float32 capture() data) are supported; anything else is rejected
			// with a clear message (DECLARED LIMITATION: float32 only).
			if (Tuple->Items[1]->Kind == EPthKind::Callable)
			{
				const FString StorageType = Tuple->Items[1]->Module + TEXT(".") + Tuple->Items[1]->Name;
				const bool bFloat32 = StorageType == TEXT("torch.FloatStorage") || StorageType == TEXT("torch.UntypedStorage");
				if (!bFloat32)
				{
					Fail(FString::Printf(TEXT("torch checkpoint 含非 float32 存储（%s）。声明：仅支持 float32 checkpoint。"), *StorageType));
					return;
				}
			}

			FPthValuePtr Ref = NewValue(EPthKind::StorageRef);
			Ref->Tensor.StorageKey = Tuple->Items[2]->Kind == EPthKind::Str ? Tuple->Items[2]->Str : DictKeyOf(Tuple->Items[2], Error);
			Ref->Tensor.Numel = IntOf(Tuple->Items[4]);
			if (bFailed)
			{
				return;
			}
			NumStorageRefs++;
			Push(Ref);
		}

		void ApplyReduce(const FPthValuePtr& Fn, const FPthValuePtr& Args)
		{
			const FString Full = (Fn->Kind == EPthKind::Callable) ? (Fn->Module + TEXT(".") + Fn->Name) : FString();

			// torch._utils._rebuild_tensor_v2(storage, storage_offset, size, stride, requires_grad, hooks)
			// torch._utils._rebuild_tensor(storage, storage_offset, size, stride)
			// torch._utils._rebuild_tensor_v3(storage, storage_offset, size, stride, requires_grad, hooks, dtype, metadata)
			const bool bTensorRebuild =
				Full == TEXT("torch._utils._rebuild_tensor_v2") ||
				Full == TEXT("torch._utils._rebuild_tensor") ||
				Full == TEXT("torch._utils._rebuild_tensor_v3");
			if (bTensorRebuild)
			{
				if (Args->Kind != EPthKind::Tuple || Args->Items.Num() < 4)
				{
					Fail(TEXT("_rebuild_tensor 参数异常。"));
					return;
				}
				const FPthValuePtr& Storage = Args->Items[0];
				if (Storage->Kind != EPthKind::StorageRef)
				{
					Fail(TEXT("_rebuild_tensor 的 storage 引用缺失（文件可能不是 torch.save checkpoint）。"));
					return;
				}
				FPthValuePtr Tensor = NewValue(EPthKind::TensorRef);
				Tensor->Tensor = Storage->Tensor;
				if (Args->Items[2]->Kind == EPthKind::Tuple || Args->Items[2]->Kind == EPthKind::List)
				{
					for (const FPthValuePtr& Dim : Args->Items[2]->Items)
					{
						Tensor->Tensor.Shape.Add(IntOf(Dim));
					}
				}
				Push(Tensor);
				return;
			}

			// nn.Parameter pickles as _rebuild_parameter(data, requires_grad, hooks)
			// where data is the nested _rebuild_tensor_v2 result.
			if (Full == TEXT("torch._utils._rebuild_parameter") || Full == TEXT("torch._utils._rebuild_parameter_v1"))
			{
				if (Args->Kind != EPthKind::Tuple || Args->Items.Num() < 1 || Args->Items[0]->Kind != EPthKind::TensorRef)
				{
					Fail(TEXT("_rebuild_parameter 参数异常。"));
					return;
				}
				Push(Args->Items[0]);
				return;
			}

			// Containers produced via REDUCE (OrderedDict etc.) become dicts.
			if (Full == TEXT("collections.OrderedDict") || Full == TEXT("collections.defaultdict") ||
				Full == TEXT("__builtin__.OrderedDict") || Full == TEXT("builtins.OrderedDict") ||
				Full == TEXT("torch.nn.parameter.OrderedDict"))
			{
				Push(NewValue(EPthKind::Dict));
				return;
			}

			// Unknown callables (torch.Size etc. degrade to a tuple/value).
			if (Full == TEXT("torch.Size"))
			{
				Push(Args); // keep the tuple shape
				return;
			}

			// Anything else: degrade to None and keep parsing (the fudan
			// capture() tensors never depend on these).
			Push(NewValue(EPthKind::None));
		}

		const uint8* Data_ = nullptr;
		int64 Size_ = 0;
		int64 Pos_ = 0;
		TArray<FPthValuePtr> Stack;
		TArray<int32> Marks;
		TMap<int64, FPthValuePtr> Memo;
		int64 NextMemoId = 0;
		bool bFailed = false;
		FString Error;

	public:
		int64 NumStorageRefs = 0;
	};

	// ---------------------------------------------------------------------
	// capture() tuple fallback: collect candidate tuples and map positions.
	// ---------------------------------------------------------------------

	void CollectTuples(const FPthValuePtr& V, TArray<FPthValuePtr>& OutTuples, int32& Budget)
	{
		if (!V.IsValid() || Budget <= 0)
		{
			return;
		}
		Budget--;
		switch (V->Kind)
		{
		case EPthKind::Tuple:
			OutTuples.Add(V);
			[[fallthrough]];
		case EPthKind::List:
			for (const FPthValuePtr& Item : V->Items)
			{
				CollectTuples(Item, OutTuples, Budget);
			}
			break;
		case EPthKind::Dict:
			for (const TPair<FString, FPthValuePtr>& KV : V->Map)
			{
				CollectTuples(KV.Value, OutTuples, Budget);
			}
			break;
		default:
			break;
		}
	}

	/**
	 * Find the fudan capture() tuple: a tuple whose first 8 elements are
	 * tensor refs in the capture() order
	 * (_xyz, _t, _scaling, _scaling_t, _rotation, _rotation_r, _opacity, _features_dc)
	 * plus optionally _features_rest at index 8.
	 */
	bool FindCaptureTuple(const FPthValuePtr& TopValue, FPthTensorRef OutByIndex[9])
	{
		TArray<FPthValuePtr> Tuples;
		int32 Budget = 100000;
		CollectTuples(TopValue, Tuples, Budget);

		const FPthValuePtr* Best = nullptr;
		int32 BestRun = 0;
		for (const FPthValuePtr& T : Tuples)
		{
			int32 Run = 0;
			while (Run < T->Items.Num() && Run < 9 && T->Items[Run]->Kind == EPthKind::TensorRef)
			{
				Run++;
			}
			if (Run >= 8 && Run > BestRun)
			{
				Best = &T;
				BestRun = Run;
			}
		}
		if (!Best)
		{
			return false;
		}
		for (int32 i = 0; i < BestRun; i++)
		{
			OutByIndex[i] = (*Best)->Items[i]->Tensor;
		}
		return true;
	}
}

// ============================================================================
// FFudanPthReader
// ============================================================================

bool FFudanPthReader::IsValidPthFile(const FString& FilePath)
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> FileHandle(PlatformFile.OpenRead(*FilePath));
	if (!FileHandle)
	{
		return false;
	}

	uint8 Magic[4];
	if (!FileHandle->Read(Magic, 4))
	{
		return false;
	}
	return Magic[0] == 'P' && Magic[1] == 'K' && Magic[2] == 0x03 && Magic[3] == 0x04;
}

bool FFudanPthReader::ReadPthFile(const FString& FilePath, TArray<FGaussianSplatData>& OutSplats, FString& OutError, int32* OutSHBands)
{
	OutSplats.Empty();

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> FileHandle(PlatformFile.OpenRead(*FilePath));
	if (!FileHandle)
	{
		OutError = FString::Printf(TEXT("Failed to open file: %s"), *FilePath);
		return false;
	}

	const int64 FileSize = FileHandle->Size();
	UE_LOG(LogTemp, Log, TEXT("PTH file size: %lld bytes (%.2f GB)"), FileSize, FileSize / (1024.0 * 1024.0 * 1024.0));

	// 1) Magic check: modern torch.save is a zip archive.
	uint8 Magic[4] = { 0 };
	if (!FileHandle->Read(Magic, 4))
	{
		OutError = TEXT("文件太小，不是有效的 torch.save checkpoint。");
		return false;
	}
	const bool bIsZip = Magic[0] == 'P' && Magic[1] == 'K' && Magic[2] == 0x03 && Magic[3] == 0x04;
	if (!bIsZip)
	{
		OutError = TEXT("旧版 torch.save 格式不支持（需 PyTorch >= 1.6 的 zip 格式 checkpoint）。");
		return false;
	}

	// 2) Parse the zip central directory.
	TArray<FZipEntry> Entries;
	if (!ParseCentralDirectory(FileHandle.Get(), Entries, OutError))
	{
		return false;
	}
	UE_LOG(LogTemp, Log, TEXT("PTH zip entries: %d"), Entries.Num());

	// Map storage keys ("data/<key>") to entries.
	TMap<FString, const FZipEntry*> EntriesByKey;
	const FZipEntry* PickleEntry = nullptr;
	const FZipEntry* ByteOrderEntry = nullptr;
	for (const FZipEntry& Entry : Entries)
	{
		if (!PickleEntry && (Entry.Name == TEXT("data.pkl") || Entry.Name.EndsWith(TEXT("/data.pkl"))))
		{
			PickleEntry = &Entry;
			continue;
		}
		if (!ByteOrderEntry && (Entry.Name == TEXT("byteorder") || Entry.Name.EndsWith(TEXT("/byteorder"))))
		{
			ByteOrderEntry = &Entry;
			continue;
		}

		FString Key;
		int32 Idx = Entry.Name.Find(TEXT("/data/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (Idx >= 0)
		{
			Key = Entry.Name.Mid(Idx + 6);
		}
		else if (Entry.Name.StartsWith(TEXT("data/")))
		{
			Key = Entry.Name.Mid(5);
		}
		if (!Key.IsEmpty())
		{
			EntriesByKey.Add(Key, &Entry);
		}
	}

	if (!PickleEntry)
	{
		OutError = TEXT("不是有效的 torch.save checkpoint（zip 中没有 data.pkl 条目）。");
		return false;
	}

	// byteorder sanity (torch >= 2.1 records it).
	if (ByteOrderEntry)
	{
		TArray<uint8> ByteOrder;
		FString EntryError;
		if (ReadZipEntry(FileHandle.Get(), *ByteOrderEntry, ByteOrder, EntryError))
		{
			const FString Order = Utf8ToString(ByteOrder.GetData(), ByteOrder.Num()).TrimStartAndEnd();
			if (Order == TEXT("big"))
			{
				OutError = TEXT("torch checkpoint 为 big-endian 存档，不支持（仅支持小端 float32）。");
				return false;
			}
		}
	}

	// 3) Parse data.pkl (pickle protocol 2 stack machine).
	TArray<uint8> PickleData;
	if (!ReadZipEntry(FileHandle.Get(), *PickleEntry, PickleData, OutError))
	{
		return false;
	}
	constexpr int64 MaxPickleSize = 512 * 1024 * 1024;
	if (PickleData.Num() > MaxPickleSize)
	{
		OutError = FString::Printf(TEXT("data.pkl 过大（%d 字节），疑似损坏的 checkpoint。"), PickleData.Num());
		return false;
	}

	FPickleInterpreter Interpreter;
	FString PickleError;
	if (!Interpreter.Run(PickleData.GetData(), PickleData.Num(), PickleError))
	{
		OutError = FString::Printf(TEXT("data.pkl 解析失败：%s"), *PickleError);
		return false;
	}
	UE_LOG(LogTemp, Log, TEXT("PTH pickle parsed: %d named tensors, %lld storage refs"),
		Interpreter.NamedTensors.Num(), Interpreter.NumStorageRefs);

	// 4) Resolve the fudan capture() tensor set.
	//    Preferred: dict keys (state-dict style). Fallback: capture() tuple
	//    order (chkpnt.pth is torch.save(capture()) -- a plain tuple).
	TMap<FString, FPthTensorRef> Tensors = Interpreter.NamedTensors;
	static const TCHAR* CaptureNames[9] = {
		TEXT("_xyz"), TEXT("_t"), TEXT("_scaling"), TEXT("_scaling_t"),
		TEXT("_rotation"), TEXT("_rotation_r"), TEXT("_opacity"),
		TEXT("_features_dc"), TEXT("_features_rest")
	};

	bool bAnyMissing = false;
	for (const TCHAR* Name : CaptureNames)
	{
		if (!Tensors.Contains(Name))
		{
			bAnyMissing = true;
		}
	}
	if (bAnyMissing)
	{
		FPthTensorRef ByIndex[9];
		if (FindCaptureTuple(Interpreter.TopValue, ByIndex))
		{
			for (int32 i = 0; i < 9; i++)
			{
				if (!Tensors.Contains(CaptureNames[i]) && ByIndex[i].StorageKey.Len() > 0)
				{
					Tensors.Add(CaptureNames[i], ByIndex[i]);
				}
			}
		}
	}

	TArray<FString> Missing;
	for (const TCHAR* Name : CaptureNames)
	{
		if (FCString::Strcmp(Name, TEXT("_features_rest")) == 0)
		{
			continue; // optional: absent => C=1 (static color)
		}
		if (!Tensors.Contains(Name))
		{
			Missing.Add(Name);
		}
	}
	if (Missing.Num() > 0)
	{
		OutError = FString::Printf(
			TEXT("不是复旦 4DGS checkpoint（缺少张量: %s）。仅支持 fudan-zvg 4DGS 的 torch.save checkpoint（capture() 张量集）。"),
			*FString::Join(Missing, TEXT(", ")));
		return false;
	}

	// 5) Validate dimensions.
	auto NumelOf = [&Tensors](const TCHAR* Name) -> int64
	{
		const FPthTensorRef* T = Tensors.Find(Name);
		return T ? T->Numel : 0;
	};

	const int64 N = NumelOf(TEXT("_xyz")) / 3;
	const bool bHasRest = Tensors.Contains(TEXT("_features_rest"));
	if (N <= 0 || NumelOf(TEXT("_xyz")) % 3 != 0)
	{
		OutError = FString::Printf(TEXT("_xyz 张量大小异常（numel=%lld）。"), NumelOf(TEXT("_xyz")));
		return false;
	}
	struct FDimCheck { const TCHAR* Name; int64 Expected; };
	const FDimCheck DimChecks[] = {
		{ TEXT("_t"), N },
		{ TEXT("_scaling"), N * 3 },
		{ TEXT("_scaling_t"), N },
		{ TEXT("_rotation"), N * 4 },
		{ TEXT("_rotation_r"), N * 4 },
		{ TEXT("_opacity"), N },
		{ TEXT("_features_dc"), N * 3 },
	};
	for (const FDimCheck& Check : DimChecks)
	{
		if (NumelOf(Check.Name) != Check.Expected)
		{
			OutError = FString::Printf(
				TEXT("张量 %s 尺寸不符（期望 %lld 实际 %lld，splat 数 N=%lld）——不是复旦 4DGS checkpoint。"),
				Check.Name, Check.Expected, NumelOf(Check.Name), N);
			return false;
		}
	}

	// 4D SH channel count C = K + 1, K = numel(_features_rest) / (3N).
	int32 FudanChannels = 1;
	int64 RestPerChannel = 0;
	if (bHasRest)
	{
		const int64 RestNumel = NumelOf(TEXT("_features_rest"));
		if (RestNumel % 3 != 0 || (RestNumel / 3) % N != 0)
		{
			OutError = FString::Printf(
				TEXT("_features_rest 尺寸不符（numel=%lld，N=%lld）——不是复旦 4DGS checkpoint。"),
				RestNumel, N);
			return false;
		}
		RestPerChannel = RestNumel / (3 * N);
		FudanChannels = static_cast<int32>(RestPerChannel) + 1;
		// Same channel table as the PLY fudan path (sh_channels_4d plus the
		// deg_t>0 variants C=32/48).
		if (FudanChannels != 1 && FudanChannels != 6 && FudanChannels != 16 &&
			FudanChannels != 32 && FudanChannels != 33 && FudanChannels != 48)
		{
			OutError = FString::Printf(
				TEXT("4D SH 通道数 C=%d 不受支持（仅支持 1/6/16/32/33/48）。"), FudanChannels);
			return false;
		}
	}

	// Memory guard (mirrors the PLY path): splat array + largest tensor read.
	const int64 SplatBytes = static_cast<int64>(N) * sizeof(FGaussianSplatData);
	int64 TotalTensorBytes = 0;
	for (const TPair<FString, FPthTensorRef>& KV : Tensors)
	{
		TotalTensorBytes += KV.Value.Numel * 4;
	}
	const FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();
	if (MemStats.AvailablePhysical > 0 &&
		SplatBytes + TotalTensorBytes > static_cast<int64>(MemStats.AvailablePhysical * 0.75))
	{
		OutError = FString::Printf(
			TEXT("内存不足以导入 %lld 个 splat：需要 %.1f GB，仅 %.1f GB 可用。请关闭其他程序或使用导出的 PLY（可降采样）。"),
			N,
			(SplatBytes + TotalTensorBytes) / (1024.0 * 1024.0 * 1024.0),
			MemStats.AvailablePhysical / (1024.0 * 1024.0 * 1024.0));
		return false;
	}

	// 6) Read tensor payloads and fill FGaussianSplatData.
	OutSplats.SetNum(static_cast<int32>(N));
	for (FGaussianSplatData& Splat : OutSplats)
	{
		Splat.bFudan4D = true;
		Splat.SH4D.SetNum(FudanChannels);
	}

	// Reads a tensor's storage into a float buffer (little-endian f32).
	auto ReadTensorFloats = [&FileHandle, &EntriesByKey, &OutError](const TCHAR* Name, const FPthTensorRef& Tensor, TArray<float>& OutFloats) -> bool
	{
		const FZipEntry** EntryPtr = EntriesByKey.Find(Tensor.StorageKey);
		if (!EntryPtr)
		{
			OutError = FString::Printf(
				TEXT("张量 %s 的存储数据缺失（zip 中没有 data/%s 条目）。"),
				Name, *Tensor.StorageKey);
			return false;
		}
		const int64 ExpectedBytes = Tensor.Numel * 4;
		if (static_cast<int64>((*EntryPtr)->UncompressedSize) != ExpectedBytes)
		{
			OutError = FString::Printf(
				TEXT("张量 %s 数据大小不符（期望 %lld 字节，实际 %lld）。"),
				Name, ExpectedBytes, static_cast<int64>((*EntryPtr)->UncompressedSize));
			return false;
		}
		TArray<uint8> Raw;
		if (!ReadZipEntry(FileHandle.Get(), **EntryPtr, Raw, OutError))
		{
			return false;
		}
		OutFloats.SetNumUninitialized(static_cast<int32>(Tensor.Numel));
		FMemory::Memcpy(OutFloats.GetData(), Raw.GetData(), static_cast<int64>(Raw.Num()));
		return true;
	};

	// Common conversion helpers (identical semantics to the fudan PLY branch
	// in FPLYFileReader::ReadVertexData / LinearizeSplatData).
	constexpr float MetersToUE = 100.0f;
	auto Linearize = [](FGaussianSplatData& Splat)
	{
		Splat.Rotation = GaussianSplattingUtils::NormalizeQuat(Splat.Rotation);
		Splat.Scale.X = FMath::Exp(Splat.Scale.X) * MetersToUE;
		Splat.Scale.Y = FMath::Exp(Splat.Scale.Y) * MetersToUE;
		Splat.Scale.Z = FMath::Exp(Splat.Scale.Z) * MetersToUE;
		Splat.Opacity = GaussianSplattingUtils::Sigmoid(Splat.Opacity);
	};

	TArray<float> Buffer;
	bool bFilledRest = false;

	// --- _xyz: positions (meters, Y-down) -> UE centimeters
	if (!ReadTensorFloats(TEXT("_xyz"), Tensors[TEXT("_xyz")], Buffer)) { return false; }
	for (int32 i = 0; i < static_cast<int32>(N); i++)
	{
		const float PlyX = Buffer[i * 3 + 0];
		const float PlyY = Buffer[i * 3 + 1];
		const float PlyZ = Buffer[i * 3 + 2];
		OutSplats[i].Position.X = PlyZ * MetersToUE;
		OutSplats[i].Position.Y = PlyX * MetersToUE;
		OutSplats[i].Position.Z = -PlyY * MetersToUE;
	}

	// --- _t: anchor time (mu_t, time units)
	if (!ReadTensorFloats(TEXT("_t"), Tensors[TEXT("_t")], Buffer)) { return false; }
	for (int32 i = 0; i < static_cast<int32>(N); i++)
	{
		OutSplats[i].AnchorTime = Buffer[i];
	}

	// --- _scaling: log spatial scales -> PLY log layout (Scale.X=Z, Y=X, Z=Y)
	if (!ReadTensorFloats(TEXT("_scaling"), Tensors[TEXT("_scaling")], Buffer)) { return false; }
	for (int32 i = 0; i < static_cast<int32>(N); i++)
	{
		const float ScaleX = Buffer[i * 3 + 0];
		const float ScaleY = Buffer[i * 3 + 1];
		const float ScaleZ = Buffer[i * 3 + 2];
		OutSplats[i].Scale.X = ScaleZ;
		OutSplats[i].Scale.Y = ScaleX;
		OutSplats[i].Scale.Z = ScaleY;
	}

	// --- _scaling_t: log temporal scale -> linear sigma_t
	if (!ReadTensorFloats(TEXT("_scaling_t"), Tensors[TEXT("_scaling_t")], Buffer)) { return false; }
	for (int32 i = 0; i < static_cast<int32>(N); i++)
	{
		OutSplats[i].TimeScale4D = FMath::Exp(Buffer[i]);
	}

	// --- _rotation: q_l(a,b,c,d) -> 3D quat + Rot4L
	if (!ReadTensorFloats(TEXT("_rotation"), Tensors[TEXT("_rotation")], Buffer)) { return false; }
	for (int32 i = 0; i < static_cast<int32>(N); i++)
	{
		const float QW = Buffer[i * 4 + 0];
		const float QX = Buffer[i * 4 + 1];
		const float QY = Buffer[i * 4 + 2];
		const float QZ = Buffer[i * 4 + 3];
		// Same coordinate conversion as the PLY rot_0..3 path.
		OutSplats[i].Rotation.W = QW;
		OutSplats[i].Rotation.X = -QZ;
		OutSplats[i].Rotation.Y = -QX;
		OutSplats[i].Rotation.Z = QY;

		OutSplats[i].Rot4L = FVector4f(QW, QX, QY, QZ);
		const float NormL = OutSplats[i].Rot4L.Size();
		if (NormL > 1e-12f)
		{
			OutSplats[i].Rot4L /= NormL;
		}
	}

	// --- _rotation_r: q_r(p,q,r,s) -> Rot4R
	if (!ReadTensorFloats(TEXT("_rotation_r"), Tensors[TEXT("_rotation_r")], Buffer)) { return false; }
	for (int32 i = 0; i < static_cast<int32>(N); i++)
	{
		OutSplats[i].Rot4R = FVector4f(Buffer[i * 4 + 0], Buffer[i * 4 + 1], Buffer[i * 4 + 2], Buffer[i * 4 + 3]);
		const float NormR = OutSplats[i].Rot4R.Size();
		if (NormR > 1e-12f)
		{
			OutSplats[i].Rot4R /= NormR;
		}
	}

	// --- _opacity: logit opacity (linearized below)
	if (!ReadTensorFloats(TEXT("_opacity"), Tensors[TEXT("_opacity")], Buffer)) { return false; }
	for (int32 i = 0; i < static_cast<int32>(N); i++)
	{
		OutSplats[i].Opacity = Buffer[i];
	}

	// --- _features_dc: (N,1,3) -> SH_DC + SH4D[0]
	if (!ReadTensorFloats(TEXT("_features_dc"), Tensors[TEXT("_features_dc")], Buffer)) { return false; }
	for (int32 i = 0; i < static_cast<int32>(N); i++)
	{
		OutSplats[i].SH_DC = FVector3f(Buffer[i * 3 + 0], Buffer[i * 3 + 1], Buffer[i * 3 + 2]);
		OutSplats[i].SH4D[0] = OutSplats[i].SH_DC;
	}

	// --- _features_rest: (N,K,3), row-major [c][ch] -> SH4D[c+1]
	if (bHasRest)
	{
		if (!ReadTensorFloats(TEXT("_features_rest"), Tensors[TEXT("_features_rest")], Buffer)) { return false; }
		for (int32 i = 0; i < static_cast<int32>(N); i++)
		{
			const float* Rest = Buffer.GetData() + static_cast<int64>(i) * RestPerChannel * 3;
			for (int32 c = 0; c < static_cast<int32>(RestPerChannel); c++)
			{
				OutSplats[i].SH4D[c + 1] = FVector3f(Rest[c * 3 + 0], Rest[c * 3 + 1], Rest[c * 3 + 2]);
			}
		}
		bFilledRest = true;
	}

	// Linearize (exp scale x100, sigmoid opacity, quat normalize) -- identical
	// to the PLY path. SH[15] stays zero for fudan 4D imports (same as PLY).
	for (FGaussianSplatData& Splat : OutSplats)
	{
		Linearize(Splat);
	}

	if (OutSHBands)
	{
		*OutSHBands = FudanChannels; // native-4D assets interpret SHBands as C
	}

	UE_LOG(LogTemp, Log,
		TEXT("Fudan 4DGS checkpoint read: N=%lld splats, 4D SH channels C=%d (features_rest=%s), dual-quaternion import via torch.save zip format."),
		N, FudanChannels, bFilledRest ? TEXT("present") : TEXT("absent"));
	return true;
}
