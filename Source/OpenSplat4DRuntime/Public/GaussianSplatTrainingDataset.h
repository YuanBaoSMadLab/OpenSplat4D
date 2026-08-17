// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "OpenSplat4DCaptureSet.h"
#include "GaussianSplatTrainingDataset.generated.h"

/**
 * 训练数据集索引资产，继承自 UOpenSplat4DCaptureSet 以兼容现有管线。
 *
 * 用法：
 *   1. 在内容浏览器右键 → 杂项 → 训练数据集
 *   2. 在 Details 面板勾选「使用外部路径」
 *   3. 选择外部图片文件夹
 *   4. 点击「扫描目录」自动解析图片和相机参数
 *
 * 与父类的区别：
 *   - CaptureSet 的 WorkDirectory 由引擎捕捉自动填充
 *   - TrainingDataset 的 ExternalDirectory 由用户手动指定外部路径
 *   - 勾选 bUseExternalPath 后，WorkDirectory 自动同步为 ExternalDirectory
 *
 * 支持的目录结构：
 *   - 扁平结构：所有 .jpg/.png 直接在目录中
 *   - COLMAP 结构：sparse/0/cameras.bin 或 cameras.txt + images.txt
 */
UCLASS(BlueprintType, meta = (DisplayName = "训练数据集（外部图片）"))
class OPENSPLAT4DRUNTIME_API UGaussianSplatTrainingDataset : public UOpenSplat4DCaptureSet
{
	GENERATED_BODY()

public:
	UGaussianSplatTrainingDataset();

	//~ Begin UObject Interface
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	//~ End UObject Interface

	// ------------------------------------------------------------------------
	// 数据源设置
	// ------------------------------------------------------------------------

	/** 是否使用引擎外部的图片目录 */
	UPROPERTY(EditAnywhere, Category = "数据源", meta = (DisplayName = "使用外部路径"))
	bool bUseExternalPath = false;

	/** 外部图片目录路径（仅 bUseExternalPath = true 时可用）。
	 *  勾选后自动同步到父类的 WorkDirectory，使管线能正确识别。 */
	UPROPERTY(EditAnywhere, Category = "数据源", meta = (DisplayName = "外部目录", EditCondition = "bUseExternalPath", EditConditionHides, AbsoluteDir))
	FDirectoryPath ExternalDirectory;

	// ------------------------------------------------------------------------
	// 扫描结果（只读，自动填充）
	// ------------------------------------------------------------------------

	/** 是否有相机参数 */
	UPROPERTY(VisibleAnywhere, Category = "扫描结果", meta = (DisplayName = "含相机参数"))
	bool bHasCameraData = false;

	/** 图片文件列表（相对于外部目录的路径） */
	UPROPERTY(VisibleAnywhere, Category = "扫描结果", meta = (DisplayName = "图片文件"))
	TArray<FString> ImageFiles;

	/** 相机参数文件路径（如果有） */
	UPROPERTY(VisibleAnywhere, Category = "扫描结果", meta = (DisplayName = "相机参数文件"))
	FString CameraDataPath;

	// ------------------------------------------------------------------------
	// 操作
	// ------------------------------------------------------------------------

	/** 扫描外部目录，自动识别图片和相机参数，并同步 WorkDirectory */
	UFUNCTION(CallInEditor, Category = "数据源", meta = (DisplayName = "扫描目录"))
	void ScanDirectory();

	/** 获取所有图片的完整路径 */
	UFUNCTION(BlueprintCallable, Category = "训练数据集")
	TArray<FString> GetFullImagePaths() const;

	/** 获取图片分辨率（通过读取第一张图片头） */
	UFUNCTION(BlueprintCallable, Category = "训练数据集")
	bool GetImageResolution(int32& OutWidth, int32& OutHeight) const;
};
