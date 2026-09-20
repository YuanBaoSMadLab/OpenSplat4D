// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatAssetTypeActions.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatAssetEditor.h"
#include "GaussianSplat4DEditor.h"
#include "GaussianSplatAssetFactory.h"
#include "EditorReimportHandler.h"
#include "ToolMenuSection.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Misc/Paths.h"
#include "Misc/MessageDialog.h"
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "ObjectTools.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "AssetTypeActions_GaussianSplatAsset"

FText FAssetTypeActions_GaussianSplatAsset::GetName() const
{
	return LOCTEXT("AssetName", "OpenSplat 资产");
}

FColor FAssetTypeActions_GaussianSplatAsset::GetTypeColor() const
{
	// Teal color to distinguish from other assets
	return FColor(64, 200, 180);
}

UClass* FAssetTypeActions_GaussianSplatAsset::GetSupportedClass() const
{
	return UGaussianSplatAsset::StaticClass();
}

uint32 FAssetTypeActions_GaussianSplatAsset::GetCategories()
{
	return EAssetTypeCategories::Misc;
}

void FAssetTypeActions_GaussianSplatAsset::GetActions(const TArray<UObject*>& InObjects, FToolMenuSection& Section)
{
	TArray<TWeakObjectPtr<UGaussianSplatAsset>> GaussianSplatAssets;
	for (UObject* Object : InObjects)
	{
		if (UGaussianSplatAsset* Asset = Cast<UGaussianSplatAsset>(Object))
		{
			GaussianSplatAssets.Add(Asset);
		}
	}

	// GAUSSIAN SPLAT ACTIONS section
	Section.AddMenuEntry(
		"GaussianSplatAsset_Reimport",
		LOCTEXT("ReimportLabel", "重新导入"),
		LOCTEXT("ReimportTooltip", "从源 PLY 文件重新导入 OpenSplat 资产"),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateSP(this, &FAssetTypeActions_GaussianSplatAsset::ExecuteReimport, GaussianSplatAssets),
			FCanExecuteAction()
		)
	);

	Section.AddMenuEntry(
		"GaussianSplatAsset_ShowInfo",
		LOCTEXT("ShowInfoLabel", "显示信息"),
		LOCTEXT("ShowInfoTooltip", "显示 OpenSplat 资产的详细信息"),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateSP(this, &FAssetTypeActions_GaussianSplatAsset::ExecuteShowInfo, GaussianSplatAssets),
			FCanExecuteAction()
		)
	);

	// PLY sequence import (keyframe 4D): opens a multi-select file dialog and
	// builds a single keyframe 4D asset from the selected per-frame PLY files.
	Section.AddMenuEntry(
		"GaussianSplatAsset_ImportPLYSequence",
		LOCTEXT("ImportPLYSequenceLabel", "导入 PLY 序列（关键帧 4D）"),
		LOCTEXT("ImportPLYSequenceTooltip", "选择多个逐帧 PLY 文件（按文件名排序），导入为关键帧 4D 资产。要求每帧顶点数一致。"),
		FSlateIcon(),
		FUIAction(
			FExecuteAction::CreateSP(this, &FAssetTypeActions_GaussianSplatAsset::ExecuteImportPLYSequence, GaussianSplatAssets),
			FCanExecuteAction()
		)
	);

	// Nanite submenu (similar to UE's native Nanite menu for Static Meshes)
	Section.AddSubMenu(
		"GaussianSplatAsset_Nanite",
		LOCTEXT("NaniteSubMenuLabel", "Nanite"),
		LOCTEXT("NaniteSubMenuTooltip", "Nanite LOD 和裁剪选项"),
		FNewMenuDelegate::CreateLambda([this, GaussianSplatAssets](FMenuBuilder& SubMenuBuilder)
		{
			// Determine current state
			bool bAllEnabled = AreAllNaniteEnabled(GaussianSplatAssets);

			// Nanite checkbox - checked if enabled. This is the ONLY Nanite
			// entry: toggling it on builds the cluster hierarchy (via
			// ExecuteEnableNanite), toggling it off clears it. Previously the
			// checkbox only flipped the flag while a separate "(N assets)"
			// entry did the real build -- two confusing items that users
			// could mix up, now merged into one truthful toggle.
			SubMenuBuilder.AddMenuEntry(
				LOCTEXT("NaniteEnabledLabel", "启用 Nanite"),
				LOCTEXT("NaniteEnabledTooltip", "启用：构建 Nanite 簇层次结构（LOD + 裁剪）；取消：清除层次结构并禁用"),
				FSlateIcon(),
				FUIAction(
					FExecuteAction::CreateLambda([this, GaussianSplatAssets, bAllEnabled]()
					{
						if (bAllEnabled)
						{
							ExecuteDisableNanite(GaussianSplatAssets);
						}
						else
						{
							ExecuteEnableNanite(GaussianSplatAssets);
						}
					}),
					FCanExecuteAction(),
					FIsActionChecked::CreateLambda([bAllEnabled]() { return bAllEnabled; })
				),
				NAME_None,
				EUserInterfaceActionType::ToggleButton
			);
		}),
		false,
		FSlateIcon()
	);
}

void FAssetTypeActions_GaussianSplatAsset::OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<IToolkitHost> EditWithinLevelEditor)
{
	// Open custom asset editor with 3D viewport + details panel
	EToolkitMode::Type Mode = EditWithinLevelEditor.IsValid() ? EToolkitMode::WorldCentric : EToolkitMode::Standalone;

	for (UObject* Object : InObjects)
	{
		if (UGaussianSplatAsset* Asset = Cast<UGaussianSplatAsset>(Object))
		{
			// 3D / 4D 分流：4D 资产打开专用"播放器式"编辑器（带时间轴 transport bar），
			// 3D 资产维持原编辑器（编辑工具 + details）。
			if (Asset->Is4D())
			{
				TSharedRef<FGaussianSplat4DEditor> NewEditor = MakeShareable(new FGaussianSplat4DEditor());
				NewEditor->InitGaussianSplat4DEditor(Mode, EditWithinLevelEditor, Asset);
			}
			else
			{
				TSharedRef<FGaussianSplatAssetEditor> NewEditor = MakeShareable(new FGaussianSplatAssetEditor());
				NewEditor->InitGaussianSplatAssetEditor(Mode, EditWithinLevelEditor, Asset);
			}
		}
	}
}

void FAssetTypeActions_GaussianSplatAsset::ExecuteReimport(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects)
{
	for (const TWeakObjectPtr<UGaussianSplatAsset>& AssetPtr : Objects)
	{
		if (UGaussianSplatAsset* Asset = AssetPtr.Get())
		{
			FReimportManager::Instance()->Reimport(Asset, /*bAskForNewFileIfMissing=*/true);
		}
	}
}

void FAssetTypeActions_GaussianSplatAsset::ExecuteShowInfo(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects)
{
	for (const TWeakObjectPtr<UGaussianSplatAsset>& AssetPtr : Objects)
	{
		if (UGaussianSplatAsset* Asset = AssetPtr.Get())
		{
			FString NaniteStatus = Asset->IsNaniteEnabled() ?
				FString::Printf(TEXT("已启用 (%d 个聚类, %d 个 LOD 层级)"), Asset->GetClusterCount(), Asset->GetNumLODLevels()) :
				TEXT("已禁用");

			FString InfoMessage = FString::Printf(
				TEXT("OpenSplat 资产信息:\n\n")
				TEXT("名称: %s\n")
				TEXT("Splat 数量: %d\n")
				TEXT("原始 Splat 数量: %d\n")
				TEXT("内存占用: %.2f MB\n")
				TEXT("包围盒: %s\n")
				TEXT("源文件: %s\n")
				TEXT("质量: %s\n")
				TEXT("Nanite: %s"),
				*Asset->GetName(),
				Asset->GetSplatCount(),
				Asset->GetOriginalSplatCount(),
				Asset->GetMemoryUsage() / (1024.0 * 1024.0),
				*Asset->GetBounds().ToString(),
				*Asset->SourceFilePath,
				*UEnum::GetValueAsString(Asset->ImportQuality),
				*NaniteStatus
			);

			UE_LOG(LogTemp, Log, TEXT("%s"), *InfoMessage);

			// Show message box
			FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(InfoMessage));
		}
	}
}

void FAssetTypeActions_GaussianSplatAsset::ExecuteEnableNanite(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects)
{
	for (const TWeakObjectPtr<UGaussianSplatAsset>& AssetPtr : Objects)
	{
		if (UGaussianSplatAsset* Asset = AssetPtr.Get())
		{
			if (!Asset->IsNaniteEnabled())
			{
				// Check if source file exists
				if (Asset->SourceFilePath.IsEmpty() || !FPaths::FileExists(Asset->SourceFilePath))
				{
					FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
						LOCTEXT("SourceFileNotFound", "无法为 {0} 启用 Nanite:\n找不到源 PLY 文件: {1}\n\n请先重新导入该资产。"),
						FText::FromString(Asset->GetName()),
						FText::FromString(Asset->SourceFilePath)
					));
					continue;
				}

				UE_LOG(LogTemp, Log, TEXT("Enabling Nanite for asset: %s"), *Asset->GetName());

				if (Asset->BuildNaniteClusterHierarchy())
				{
					UE_LOG(LogTemp, Log, TEXT("Successfully enabled Nanite for asset: %s (%d clusters)"),
						*Asset->GetName(), Asset->GetClusterCount());
				}
				else
				{
					FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
						LOCTEXT("NaniteBuildFailed", "无法为 {0} 启用 Nanite。\n请查看输出日志获取详情。"),
						FText::FromString(Asset->GetName())
					));
				}
			}
		}
	}
}

void FAssetTypeActions_GaussianSplatAsset::ExecuteDisableNanite(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects)
{
	for (const TWeakObjectPtr<UGaussianSplatAsset>& AssetPtr : Objects)
	{
		if (UGaussianSplatAsset* Asset = AssetPtr.Get())
		{
			if (Asset->IsNaniteEnabled())
			{
				UE_LOG(LogTemp, Log, TEXT("Disabling Nanite for asset: %s"), *Asset->GetName());
				Asset->ClearNaniteClusterHierarchy();
				UE_LOG(LogTemp, Log, TEXT("Successfully disabled Nanite for asset: %s"), *Asset->GetName());
			}
		}
	}
}

bool FAssetTypeActions_GaussianSplatAsset::AreAllNaniteEnabled(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects) const
{
	for (const TWeakObjectPtr<UGaussianSplatAsset>& AssetPtr : Objects)
	{
		if (UGaussianSplatAsset* Asset = AssetPtr.Get())
		{
			if (!Asset->IsNaniteEnabled())
			{
				return false;
			}
		}
	}
	return Objects.Num() > 0;
}

bool FAssetTypeActions_GaussianSplatAsset::AreAllNaniteDisabled(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects) const
{
	for (const TWeakObjectPtr<UGaussianSplatAsset>& AssetPtr : Objects)
	{
		if (UGaussianSplatAsset* Asset = AssetPtr.Get())
		{
			if (Asset->IsNaniteEnabled())
			{
				return false;
			}
		}
	}
	return Objects.Num() > 0;
}

void FAssetTypeActions_GaussianSplatAsset::ExecuteImportPLYSequence(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects)
{
	// 1. Multi-select file dialog for the PLY frames
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("ImportSequenceNoPlatform", "无法打开文件选择对话框。"));
		return;
	}

	// Default browse directory: folder of the first selected asset's source file
	FString DefaultFolder;
	for (const TWeakObjectPtr<UGaussianSplatAsset>& AssetPtr : Objects)
	{
		if (UGaussianSplatAsset* Asset = AssetPtr.Get())
		{
			if (!Asset->SourceFilePath.IsEmpty())
			{
				DefaultFolder = FPaths::GetPath(Asset->SourceFilePath);
				break;
			}
		}
	}
	if (DefaultFolder.IsEmpty())
	{
		DefaultFolder = FPaths::ProjectContentDir();
	}

	TArray<FString> SelectedFiles;
	const void* ParentWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	if (!DesktopPlatform->OpenFileDialog(
			ParentWindowHandle,
			TEXT("选择 PLY 序列帧（可多选）"),
			DefaultFolder,
			TEXT(""),
			TEXT("PLY 文件 (*.ply)|*.ply"),
			EFileDialogFlags::Multiple,
			SelectedFiles) || SelectedFiles.Num() < 2)
	{
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("ImportSequenceNeedFrames", "请至少选择 2 个 PLY 文件才能构成关键帧 4D 序列。"));
		return;
	}

	// 2. Target package path: same folder as the first selected asset (fallback /Game/)
	FString PackagePath = TEXT("/Game");
	for (const TWeakObjectPtr<UGaussianSplatAsset>& AssetPtr : Objects)
	{
		if (UGaussianSplatAsset* Asset = AssetPtr.Get())
		{
			const FString AssetPackageName = Asset->GetPackage()->GetName();
			const int32 SlashIndex = AssetPackageName.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			if (SlashIndex != INDEX_NONE)
			{
				PackagePath = AssetPackageName.Left(SlashIndex);
			}
			break;
		}
	}

	// 3. Asset name: first sorted frame's base name, sanitized and made unique
	SelectedFiles.Sort([](const FString& A, const FString& B) { return A < B; });
	FString AssetBaseName = ObjectTools::SanitizeObjectName(FPaths::GetBaseFilename(SelectedFiles[0]));
	if (AssetBaseName.IsEmpty())
	{
		AssetBaseName = TEXT("PLYSequence");
	}
	FString OutPackageName, OutAssetName;
	FAssetToolsModule::GetModule().Get().CreateUniqueAssetName(PackagePath / AssetBaseName, TEXT(""), OutPackageName, OutAssetName);

	UPackage* TargetPackage = CreatePackage(*OutPackageName);

	// 4. Run the sequence import
	FString ImportError;
	UGaussianSplatAsset* NewAsset = UGaussianSplatAssetFactory::ImportPLYSequence(
		SelectedFiles, TargetPackage, FName(*OutAssetName), RF_Public | RF_Standalone, &ImportError);

	if (!NewAsset)
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(
			ImportError.IsEmpty() ? TEXT("PLY 序列导入失败，请查看输出日志。") : ImportError));
		return;
	}

	FAssetRegistryModule::AssetCreated(NewAsset);
	NewAsset->MarkPackageDirty();

	UE_LOG(LogTemp, Log, TEXT("Created keyframe 4D asset '%s' from %d PLY frames"), *NewAsset->GetName(), SelectedFiles.Num());
}

#undef LOCTEXT_NAMESPACE
