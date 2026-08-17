// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatTrainingDataset.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

UGaussianSplatTrainingDataset::UGaussianSplatTrainingDataset()
{
}

#if WITH_EDITOR
void UGaussianSplatTrainingDataset::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (!PropertyChangedEvent.Property) return;

	const FName PropertyName = PropertyChangedEvent.Property->GetFName();

	if (PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatTrainingDataset, bUseExternalPath) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UGaussianSplatTrainingDataset, ExternalDirectory))
	{
		if (bUseExternalPath && !ExternalDirectory.Path.IsEmpty())
		{
			// 同步 WorkDirectory 到 ExternalDirectory，使管线能识别
			WorkDirectory = ExternalDirectory.Path;
			ScanDirectory();
		}
		else if (!bUseExternalPath)
		{
			ImageFiles.Reset();
			ImageCount = 0;
			bHasCameraData = false;
			CameraDataPath.Reset();
		}
	}
}
#endif

void UGaussianSplatTrainingDataset::ScanDirectory()
{
	ImageFiles.Reset();
	bHasCameraData = false;
	CameraDataPath.Reset();

	const FString DirPath = ExternalDirectory.Path;
	if (DirPath.IsEmpty())
	{
		ImageCount = 0;
		UE_LOG(LogTemp, Warning, TEXT("TrainingDataset: 外部目录路径为空，无法扫描"));
		return;
	}

	// 同步 WorkDirectory（父类字段），使稀疏重建/训练步骤能正确找到数据
	WorkDirectory = DirPath;

	// 递归搜索所有图片文件
	TArray<FString> FoundFiles;
	const TArray<FString> ImageExtensions = { TEXT("jpg"), TEXT("jpeg"), TEXT("png"), TEXT("bmp"), TEXT("tiff"), TEXT("tif"), TEXT("exr") };

	for (const FString& Ext : ImageExtensions)
	{
		TArray<FString> ExtFiles;
		IFileManager::Get().FindFilesRecursive(ExtFiles, *DirPath, *(TEXT("*.") + Ext), true, false);
		for (const FString& FullPath : ExtFiles)
		{
			FString RelativePath = FullPath;
			FPaths::MakePathRelativeTo(RelativePath, *(DirPath + TEXT("/")));
			FoundFiles.Add(RelativePath);
		}
	}

	FoundFiles.Sort();
	ImageFiles = MoveTemp(FoundFiles);
	ImageCount = ImageFiles.Num();

	// 检测 COLMAP 相机参数
	auto CheckFile = [&](const FString& SubPath) -> bool
	{
		return FPaths::FileExists(DirPath / SubPath);
	};

	struct { const TCHAR* Path; } CameraPaths[] = {
		{ TEXT("sparse/0/cameras.bin") },
		{ TEXT("sparse/0/cameras.txt") },
		{ TEXT("sparse/cameras.bin") },
		{ TEXT("sparse/cameras.txt") },
		{ TEXT("colmap/sparse/0/cameras.bin") },
		{ TEXT("colmap/sparse/0/cameras.txt") },
	};
	for (const auto& CP : CameraPaths)
	{
		if (CheckFile(CP.Path))
		{
			bHasCameraData = true;
			CameraDataPath = CP.Path;
			break;
		}
	}

	UE_LOG(LogTemp, Log, TEXT("TrainingDataset: 扫描完成 — %d 张图片, 相机参数: %s"),
		ImageCount, bHasCameraData ? *CameraDataPath : TEXT("无"));

	Modify();
}

TArray<FString> UGaussianSplatTrainingDataset::GetFullImagePaths() const
{
	TArray<FString> FullPaths;
	if (!bUseExternalPath || ExternalDirectory.Path.IsEmpty()) return FullPaths;

	FullPaths.Reserve(ImageFiles.Num());
	for (const FString& RelPath : ImageFiles)
	{
		FullPaths.Add(ExternalDirectory.Path / RelPath);
	}
	return FullPaths;
}

bool UGaussianSplatTrainingDataset::GetImageResolution(int32& OutWidth, int32& OutHeight) const
{
	OutWidth = 0;
	OutHeight = 0;

	if (ImageFiles.IsEmpty()) return false;

	const FString FirstImagePath = ExternalDirectory.Path / ImageFiles[0];
	if (!FPaths::FileExists(FirstImagePath)) return false;

	TArray<uint8> HeaderData;
	if (!FFileHelper::LoadFileToArray(HeaderData, *FirstImagePath, FILEREAD_Silent))
		return false;

	if (HeaderData.Num() < 24) return false;

	// JPEG: search SOF0 marker (0xFF 0xC0)
	if (HeaderData[0] == 0xFF && HeaderData[1] == 0xD8)
	{
		for (int32 i = 2; i < HeaderData.Num() - 9; ++i)
		{
			if (HeaderData[i] == 0xFF && (HeaderData[i + 1] >= 0xC0 && HeaderData[i + 1] <= 0xC3))
			{
				OutHeight = (HeaderData[i + 5] << 8) | HeaderData[i + 6];
				OutWidth = (HeaderData[i + 7] << 8) | HeaderData[i + 8];
				return true;
			}
		}
	}

	// PNG: IHDR chunk at byte 16
	if (HeaderData[0] == 0x89 && HeaderData[1] == 0x50 && HeaderData[2] == 0x4E && HeaderData[3] == 0x47)
	{
		OutWidth = (HeaderData[16] << 24) | (HeaderData[17] << 16) | (HeaderData[18] << 8) | HeaderData[19];
		OutHeight = (HeaderData[20] << 24) | (HeaderData[21] << 16) | (HeaderData[22] << 8) | HeaderData[23];
		return true;
	}

	return false;
}
