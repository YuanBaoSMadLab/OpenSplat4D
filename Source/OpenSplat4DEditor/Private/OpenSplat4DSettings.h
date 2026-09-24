// Copyright OpenSplat4D 2026 All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "OpenSplat4DSettings.generated.h"

/**
 * Project settings for the OpenSplat4D capture -> sparse reconstruction -> train
 * pipeline. Mirrors the reference plugin's UGaussianSplattingEditorSettings:
 * points at the user's colmap / python / training-repo installs so the editor
 * steps can shell out to them.
 */
UCLASS(EditInlineNew, CollapseCategories, config = OpenSplat4D, defaultconfig)
class UOpenSplat4DSettings : public UObject
{
	GENERATED_BODY()
public:
	FString GetPythonExecutablePath() const;
	FString GetColmapExecutablePath() const;
	FString GetHelperScriptPath() const;
	/** 3DGS (Inria-style) training repository directory. */
	FString Get3DGSRepoDir() const;
	/** 4DGS (4d-gaussian-splatting) training repository directory. */
	FString Get4DGSRepoDir() const;

	FString GetWorkHome() const;
	FString GetWorkDir(const FString& WorkName) const;

	void PostInitProperties() override;
	void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;

public:
	UPROPERTY(EditAnywhere, Category = "OpenSplat4D", meta = (FilePathFilter = "exe", RelativeToGameDir, DisplayName = "Python 可执行文件路径"))
	FFilePath PythonExecutablePath;

	UPROPERTY(EditAnywhere, Category = "OpenSplat4D", meta = (FilePathFilter = "exe", RelativeToGameDir, DisplayName = "Colmap 可执行文件路径"))
	FFilePath ColmapExecutablePath;

	UPROPERTY(EditAnywhere, Category = "OpenSplat4D", meta = (RelativeToGameDir, DisplayName = "3DGS 训练仓库目录"))
	FDirectoryPath GaussianSplattingRepoDir;

	UPROPERTY(EditAnywhere, Category = "OpenSplat4D", meta = (RelativeToGameDir, DisplayName = "4DGS 训练仓库目录"))
	FDirectoryPath GaussianSplatting4DRepoDir;

	UPROPERTY(EditAnywhere, Category = "OpenSplat4D", meta = (FilePathFilter = "py", RelativeToGameDir, DisplayName = "辅助脚本路径"))
	FFilePath HelperScriptPath;

	UPROPERTY(Config)
	FString PythonExecutablePathConfig;
	UPROPERTY(Config)
	FString ColmapExecutablePathConfig;
	UPROPERTY(Config)
	FString GaussianSplattingRepoDirConfig;
	UPROPERTY(Config)
	FString GaussianSplatting4DRepoDirConfig;
	UPROPERTY(Config)
	FString HelperScriptPathConfig;
};
