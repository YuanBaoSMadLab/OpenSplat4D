// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatAssetFactory.h"
#include "GaussianSplatAsset.h"
#include "PLYFileReader.h"
#include "EditorFramework/AssetImportData.h"
#include "Misc/FeedbackContext.h"
#include "Misc/ScopedSlowTask.h"
#include "HAL/PlatformMemory.h"

UGaussianSplatAssetFactory::UGaussianSplatAssetFactory()
{
	bCreateNew = false;
	bEditorImport = true;
	bText = false;

	SupportedClass = UGaussianSplatAsset::StaticClass();

	Formats.Add(TEXT("ply;PLY 高斯泼溅文件"));
}

bool UGaussianSplatAssetFactory::FactoryCanImport(const FString& Filename)
{
	const FString Extension = FPaths::GetExtension(Filename);
	return Extension.Equals(TEXT("ply"), ESearchCase::IgnoreCase) && FPLYFileReader::IsValidPLYFile(Filename);
}

UObject* UGaussianSplatAssetFactory::FactoryCreateFile(
	UClass* InClass,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	const FString& Filename,
	const TCHAR* Parms,
	FFeedbackContext* Warn,
	bool& bOutOperationCanceled)
{
	bOutOperationCanceled = false;

	UGaussianSplatAsset* NewAsset = ImportPLYFile(Filename, InParent, InName, Flags, nullptr);

	if (!NewAsset)
	{
		if (Warn)
		{
			Warn->Logf(ELogVerbosity::Error, TEXT("Failed to import Gaussian Splat from: %s"), *Filename);
		}
	}

	return NewAsset;
}

FText UGaussianSplatAssetFactory::GetDisplayName() const
{
	return FText::FromString(TEXT("OpenSplat 资产"));
}

bool UGaussianSplatAssetFactory::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
	UGaussianSplatAsset* Asset = Cast<UGaussianSplatAsset>(Obj);
	if (Asset && !Asset->SourceFilePath.IsEmpty())
	{
		OutFilenames.Add(Asset->SourceFilePath);
		return true;
	}
	return false;
}

void UGaussianSplatAssetFactory::SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths)
{
	UGaussianSplatAsset* Asset = Cast<UGaussianSplatAsset>(Obj);
	if (Asset && NewReimportPaths.Num() > 0)
	{
		Asset->SourceFilePath = NewReimportPaths[0];
	}
}

EReimportResult::Type UGaussianSplatAssetFactory::Reimport(UObject* Obj)
{
	UGaussianSplatAsset* Asset = Cast<UGaussianSplatAsset>(Obj);
	if (!Asset)
	{
		return EReimportResult::Failed;
	}

	if (Asset->SourceFilePath.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Cannot reimport: source file path is empty"));
		return EReimportResult::Failed;
	}

	if (!FPaths::FileExists(Asset->SourceFilePath))
	{
		UE_LOG(LogTemp, Error, TEXT("Cannot reimport: source file not found: %s"), *Asset->SourceFilePath);
		return EReimportResult::Failed;
	}

	// Preserve Nanite setting before reimport
	const bool bWasNaniteEnabled = Asset->IsNaniteEnabled();

	// Use the original quality level
	QualityLevel = Asset->ImportQuality;

	UGaussianSplatAsset* ReimportedAsset = ImportPLYFile(
		Asset->SourceFilePath,
		Asset->GetOuter(),
		Asset->GetFName(),
		Asset->GetFlags(),
		Asset
	);

	if (ReimportedAsset)
	{
		// If Nanite was enabled before reimport, rebuild the cluster hierarchy
		if (bWasNaniteEnabled)
		{
			UE_LOG(LogTemp, Log, TEXT("Reimport: Rebuilding Nanite cluster hierarchy (was enabled before reimport)"));
			if (!ReimportedAsset->BuildNaniteClusterHierarchy())
			{
				UE_LOG(LogTemp, Warning, TEXT("Reimport: Failed to rebuild Nanite cluster hierarchy"));
			}
		}
		return EReimportResult::Succeeded;
	}

	return EReimportResult::Failed;
}

UGaussianSplatAsset* UGaussianSplatAssetFactory::ImportPLYFile(
	const FString& FilePath,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	UGaussianSplatAsset* ExistingAsset)
{
	FScopedSlowTask SlowTask(100.0f, FText::FromString(TEXT("正在导入 OpenSplat 资产...")));
	SlowTask.MakeDialog(true);

	// Read PLY file
	SlowTask.EnterProgressFrame(30.0f, FText::FromString(TEXT("正在读取 PLY 文件...")));

	TArray<FGaussianSplatData> SplatData;
	FString ErrorMessage;
	int32 DetectedSHBands = 0;

	if (!FPLYFileReader::ReadPLYFile(FilePath, SplatData, ErrorMessage, &DetectedSHBands))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to read PLY file: %s"), *ErrorMessage);
		return nullptr;
	}

	UE_LOG(LogTemp, Log, TEXT("Read %d splats from PLY file (SH bands: %d)"), SplatData.Num(), DetectedSHBands);

	// Create or reuse asset
	SlowTask.EnterProgressFrame(10.0f, FText::FromString(TEXT("正在创建资产...")));

	UGaussianSplatAsset* Asset = ExistingAsset;
	if (!Asset)
	{
		Asset = NewObject<UGaussianSplatAsset>(InParent, UGaussianSplatAsset::StaticClass(), InName, Flags);
	}

	if (!Asset)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to create Gaussian Splat asset"));
		return nullptr;
	}

	// Store source file path
	Asset->SourceFilePath = FilePath;

	// Set the detected SH band count BEFORE initializing (CompressSH uses this)
	Asset->SHBands = DetectedSHBands;

	// Initialize asset from splat data (NO cluster building - user enables Nanite via Asset Actions)
	SlowTask.EnterProgressFrame(55.0f, FText::FromString(TEXT("正在压缩 splat 数据...")));

	Asset->InitializeFromSplatData(SplatData, QualityLevel);

	// NO cluster hierarchy by default - user enables Nanite via Asset Actions > Nanite
	Asset->ClusterHierarchy.Reset();

	// Mark package dirty
	Asset->MarkPackageDirty();

	UE_LOG(LogTemp, Log, TEXT("Successfully imported Gaussian Splat asset: %d splats, %lld bytes (Nanite disabled by default)"),
		Asset->GetSplatCount(), Asset->GetMemoryUsage());

	return Asset;
}

UGaussianSplatAsset* UGaussianSplatAssetFactory::ImportPLYSequence(
	const TArray<FString>& FilePaths,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	FString* OutError)
{
	auto Fail = [&OutError](const FString& Message) -> UGaussianSplatAsset*
	{
		UE_LOG(LogTemp, Error, TEXT("ImportPLYSequence: %s"), *Message);
		if (OutError)
		{
			*OutError = Message;
		}
		return nullptr;
	};

	if (FilePaths.Num() < 2)
	{
		return Fail(TEXT("至少需要 2 个 PLY 文件才能构成关键帧 4D 序列。"));
	}

	// 1. Sort by file name to determine the frame order (frame_0000.ply, frame_0001.ply, ...)
	TArray<FString> SortedFiles = FilePaths;
	SortedFiles.Sort([](const FString& A, const FString& B) { return A < B; });
	const int32 N = SortedFiles.Num();

	FScopedSlowTask SlowTask(100.0f, FText::FromString(TEXT("正在导入 PLY 序列（关键帧 4D）...")));
	SlowTask.MakeDialog(true);

	// 2. Parse frame 0 fully (defines M and the base splat array)
	SlowTask.EnterProgressFrame(15.0f, FText::FromString(TEXT("正在解析帧 0...")));

	TArray<FGaussianSplatData> FrameSplats;
	FString ErrorMessage;
	int32 DetectedSHBands = 0;
	if (!FPLYFileReader::ReadPLYFile(SortedFiles[0], FrameSplats, ErrorMessage, &DetectedSHBands))
	{
		return Fail(FString::Printf(TEXT("解析帧 0 失败（%s）：%s"), *SortedFiles[0], *ErrorMessage));
	}
	const int32 M = FrameSplats.Num();
	if (M <= 0)
	{
		return Fail(FString::Printf(TEXT("帧 0（%s）没有任何顶点。"), *SortedFiles[0]));
	}

	// 3. Memory guard for the keyframe block (N * M * 64 bytes)
	const int64 KeyframeBytes = static_cast<int64>(N) * M * UGaussianSplatAsset::KeyframeStride;
	const FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();
	if (MemStats.AvailablePhysical > 0 && KeyframeBytes > static_cast<int64>(MemStats.AvailablePhysical * 0.75))
	{
		return Fail(FString::Printf(TEXT("序列过大：%d 帧 x %d splat 需要 %.1f GB 内存存储关键帧数据，可用内存不足。请减少帧数或 splat 数量。"),
			N, M, KeyframeBytes / (1024.0 * 1024.0 * 1024.0)));
	}

	TArray<uint8> KeyframeData;
	KeyframeData.SetNumUninitialized(KeyframeBytes);

	// Pack one frame into the keyframe block:
	// 64B per splat: pos 3xf32 | rot 4xf32 XYZW | scale 3xf32 | opacity f32 | color RGB 3xf32 | reserved 8B
	// Color/opacity follow the PackedSplatBuffer convention: color = SH DC converted
	// to display color (0.5 + C0*DC, sRGB space), opacity = linear sigmoid value.
	auto PackFrame = [M, &KeyframeData](int32 FrameIndex, const TArray<FGaussianSplatData>& Splats)
	{
		for (int32 i = 0; i < M; i++)
		{
			const FGaussianSplatData& Splat = Splats[i];
			uint8* Dest = KeyframeData.GetData() + (static_cast<int64>(FrameIndex) * M + i) * UGaussianSplatAsset::KeyframeStride;
			float* Floats = reinterpret_cast<float*>(Dest);
			Floats[0] = Splat.Position.X;
			Floats[1] = Splat.Position.Y;
			Floats[2] = Splat.Position.Z;
			Floats[3] = Splat.Rotation.X;
			Floats[4] = Splat.Rotation.Y;
			Floats[5] = Splat.Rotation.Z;
			Floats[6] = Splat.Rotation.W;
			Floats[7] = Splat.Scale.X;
			Floats[8] = Splat.Scale.Y;
			Floats[9] = Splat.Scale.Z;
			Floats[10] = Splat.Opacity;
			const FVector3f Color = GaussianSplattingUtils::SHDCToColor(Splat.SH_DC);
			Floats[11] = Color.X;
			Floats[12] = Color.Y;
			Floats[13] = Color.Z;
			// Floats[14..15] reserved (stride 64B; 12+16+12+4+12=56 used)
		}
	};

	PackFrame(0, FrameSplats);

	// 4. Parse remaining frames (full parse per frame -- simplest correct approach)
	for (int32 F = 1; F < N; F++)
	{
		SlowTask.EnterProgressFrame(60.0f / N, FText::FromString(FString::Printf(TEXT("正在解析帧 %d/%d..."), F + 1, N)));

		TArray<FGaussianSplatData> Splats;
		if (!FPLYFileReader::ReadPLYFile(SortedFiles[F], Splats, ErrorMessage))
		{
			return Fail(FString::Printf(TEXT("解析帧 %d 失败（%s）：%s"), F, *SortedFiles[F], *ErrorMessage));
		}
		if (Splats.Num() != M)
		{
			return Fail(FString::Printf(
				TEXT("帧 %d 顶点数不匹配：期望 %d 实际 %d（%s）。来自 Postshot 逐帧训练的序列不满足逐帧对应，无法导入。"),
				F, M, Splats.Num(), *SortedFiles[F]));
		}
		PackFrame(F, Splats);
	}

	// 5. Create the asset: base splat data from frame 0, then keyframe 4D state
	SlowTask.EnterProgressFrame(20.0f, FText::FromString(TEXT("正在创建关键帧 4D 资产...")));

	UGaussianSplatAsset* Asset = NewObject<UGaussianSplatAsset>(InParent, UGaussianSplatAsset::StaticClass(), InName, Flags);
	if (!Asset)
	{
		return Fail(TEXT("创建资产失败。"));
	}

	Asset->SourceFilePath = SortedFiles[0];
	Asset->SHBands = DetectedSHBands;
	// Static context: use the factory default quality (UGaussianSplatAssetFactory::QualityLevel default)
	Asset->InitializeFromSplatData(FrameSplats, EGaussianQualityLevel::VeryHigh);
	Asset->ClusterHierarchy.Reset();

	// Keyframe 4D state (set after InitializeFromSplatData, which would otherwise
	// auto-detect temporal data from t/scale_t properties if the frames carried any)
	Asset->bIs4D = true;
	Asset->bIsKeyframe4D = true;
	Asset->TimeStart = 0.f;
	Asset->TimeEnd = static_cast<float>(N - 1);
	Asset->KeyframeCount = N;

	Asset->KeyframeBulkData.Lock(LOCK_READ_WRITE);
	void* Dest = Asset->KeyframeBulkData.Realloc(KeyframeData.Num());
	FMemory::Memcpy(Dest, KeyframeData.GetData(), KeyframeData.Num());
	Asset->KeyframeBulkData.Unlock();
	Asset->KeyframeBulkData.SetBulkDataFlags(BULKDATA_Force_NOT_InlinePayload);

	Asset->MarkPackageDirty();

	UE_LOG(LogTemp, Log, TEXT("Successfully imported PLY sequence asset '%s': %d frames x %d splats (%lld bytes of keyframe data)"),
		*Asset->GetName(), N, M, KeyframeBytes);

	return Asset;
}
