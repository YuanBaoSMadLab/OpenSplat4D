// Copyright OpenSplat4D 2026 All Rights Reserved.
// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatAssetTypeActions.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatAssetEditor.h"
#include "GaussianSplat4DEditor.h"
#include "GaussianSplatAssetFactory.h"
#include "GaussianSplatProxyMeshBuilder.h"
// 已废弃（v2 重构）：旧的绑定直接写回 UGaussianSplatAsset，绑定入口移至破裂编辑器。
// #include "GaussianSplatBoneBinder.h"
#include "GaussianSplatFractureAsset.h"
#include "Engine/StaticMesh.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "EditorReimportHandler.h"
#include "ToolMenuSection.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Misc/Paths.h"
#include "Misc/MessageDialog.h"
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "ObjectTools.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "AssetTypeActions_GaussianSplatAsset"

namespace
{
	/** Content folder of an asset ("/Game/Splats" for "/Game/Splats/MySplat"). */
	FString GetFractureAssetFolderPath(const UGaussianSplatAsset* SourceAsset)
	{
		if (SourceAsset && SourceAsset->GetOutermost())
		{
			const FString PackageName = SourceAsset->GetOutermost()->GetName();
			const int32 SlashIndex = PackageName.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
			if (SlashIndex != INDEX_NONE)
			{
				return PackageName.Left(SlashIndex);
			}
		}
		return TEXT("/Game");
	}

	/** Asset name the fracture asset of a splat asset is generated under. */
	FString GetFractureAssetName(const UGaussianSplatAsset* SourceAsset)
	{
		return ObjectTools::SanitizeObjectName(SourceAsset->GetName()) + TEXT("_Fracture");
	}

	/** Already generated fracture asset of a splat asset (nullptr when none). */
	UGaussianSplatFractureAsset* FindFractureAsset(const UGaussianSplatAsset* SourceAsset)
	{
		if (!SourceAsset)
		{
			return nullptr;
		}

		const FString FractureName = GetFractureAssetName(SourceAsset);
		const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"),
			*GetFractureAssetFolderPath(SourceAsset), *FractureName, *FractureName);

		const FAssetData AssetData = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry")
			.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
		return AssetData.IsValid() ? Cast<UGaussianSplatFractureAsset>(AssetData.GetAsset()) : nullptr;
	}

	/**
	 * Fracture asset of a splat asset: the existing one when it was generated
	 * before, otherwise a new one created next to the splat asset (named
	 * "<Splat>_Fracture"). bOutCreated reports whether this call created it.
	 */
	UGaussianSplatFractureAsset* FindOrCreateFractureAsset(UGaussianSplatAsset* SourceAsset, bool& bOutCreated)
	{
		bOutCreated = false;
		if (!SourceAsset)
		{
			return nullptr;
		}

		if (UGaussianSplatFractureAsset* ExistingAsset = FindFractureAsset(SourceAsset))
		{
			return ExistingAsset;
		}

		FString OutPackageName;
		FString OutAssetName;
		FAssetToolsModule::GetModule().Get().CreateUniqueAssetName(
			GetFractureAssetFolderPath(SourceAsset) / GetFractureAssetName(SourceAsset), TEXT(""),
			OutPackageName, OutAssetName);

		UPackage* Package = CreatePackage(*OutPackageName);
		UGaussianSplatFractureAsset* NewAsset = NewObject<UGaussianSplatFractureAsset>(
			Package, UGaussianSplatFractureAsset::StaticClass(), FName(*OutAssetName), RF_Public | RF_Standalone);
		if (!NewAsset)
		{
			return nullptr;
		}

		// 只引用源 splat 资产（不复制点云）；碰撞体素壳、骨头绑定与断面 splat
		// 都由破裂编辑器生成后写回本资产。
		NewAsset->SourceSplatAsset = SourceAsset;
		NewAsset->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(NewAsset);
		bOutCreated = true;

		UE_LOG(LogTemp, Log, TEXT("Created fracture asset '%s' referencing splat asset '%s'"),
			*NewAsset->GetName(), *SourceAsset->GetName());
		return NewAsset;
	}
}

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

	// 破裂 (fracture) submenu.
	// 新流程：生成破裂资产（独立 UGaussianSplatFractureAsset）→ 双击进入破裂编辑器
	// → 生成碰撞体素壳 / Chaos 破裂设置 / 预览 / 解算 → 拖入关卡或蓝图使用。
	Section.AddSubMenu(
		"GaussianSplatAsset_Fracture",
		LOCTEXT("FractureSubMenuLabel", "破裂"),
		LOCTEXT("FractureSubMenuTooltip", "Chaos 破裂工具"),
		FNewMenuDelegate::CreateLambda([this, GaussianSplatAssets](FMenuBuilder& SubMenuBuilder)
		{
			SubMenuBuilder.AddMenuEntry(
				LOCTEXT("GenerateFractureAssetLabel", "生成破裂资产"),
				LOCTEXT("GenerateFractureAssetTooltip", "创建一个独立破裂资产（引用本 splat 资产，不复制点云）。之后双击该资产进入破裂编辑器生成碰撞体素壳、做 Chaos 破裂设置、预览与解算"),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateSP(this, &FAssetTypeActions_GaussianSplatAsset::ExecuteGenerateFractureAsset, GaussianSplatAssets))
			);

			// 主流程入口：体素壳就是 Chaos 切割用的实体（同时可当碰撞体），
			// 所以这一步会一并创建破裂资产（没有的话）并把壳写进去。
			SubMenuBuilder.AddMenuEntry(
				LOCTEXT("GenerateProxyMeshLabel", "生成碰撞体素网格"),
				LOCTEXT("GenerateProxyMeshTooltip", "按 splat 位置生成封闭的体素壳，写入对应的破裂资产（没有则一并创建）。该壳既是 Chaos 破裂切割的实体，也可在破裂编辑器里用「体素壳用作碰撞」开关决定它是否同时作为碰撞体"),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateSP(this, &FAssetTypeActions_GaussianSplatAsset::ExecuteGenerateProxyMesh, GaussianSplatAssets))
			);

			// 已废弃（v2 重构）：旧的「绑定破裂骨头」把绑定写回 splat 资产，新的绑定
			// 由破裂编辑器在解算时写入破裂资产。入口停用，代码保留在下方 #if 0 段内。
			// SubMenuBuilder.AddMenuEntry(
			// 	LOCTEXT("BindFractureBonesLabel", "绑定破裂骨头"),
			// 	LOCTEXT("BindFractureBonesTooltip", "把每个 splat 分配到已切割 GeometryCollection 的某一块骨头上，使 splat 随该块一起破裂（请选择切割代理网格得到的 GeometryCollection）"),
			// 	FSlateIcon(),
			// 	FUIAction(FExecuteAction::CreateSP(this, &FAssetTypeActions_GaussianSplatAsset::ExecuteBindFractureBones, GaussianSplatAssets))
			// );
		}),
		false,
		FSlateIcon()
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

void FAssetTypeActions_GaussianSplatAsset::ExecuteGenerateProxyMesh(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects)
{
	for (const TWeakObjectPtr<UGaussianSplatAsset>& AssetPtr : Objects)
	{
		UGaussianSplatAsset* Asset = AssetPtr.Get();
		if (!Asset)
		{
			continue;
		}

		UE_LOG(LogTemp, Log, TEXT("Generating collision voxel shell for asset: %s"), *Asset->GetName());

		// 体素壳是破裂资产的一部分（Chaos 切割的实体 + 可选碰撞体），所以先拿到
		// 对应的破裂资产：没有就一并新建，这样一次右键就同时得到壳与独立资产。
		bool bCreatedAsset = false;
		UGaussianSplatFractureAsset* FractureAsset = FindOrCreateFractureAsset(Asset, bCreatedAsset);
		if (!FractureAsset)
		{
			FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(
				TEXT("无法为 %s 创建破裂资产，碰撞体素壳未生成。"), *Asset->GetName())));
			continue;
		}

		FString Error;
		FGaussianSplatProxyMeshResult Result;
		if (!FGaussianSplatProxyMeshBuilder::BuildVoxelShell(
			Asset, FractureAsset, FGaussianSplatProxyMeshParams(), Error, &Result))
		{
			FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
				LOCTEXT("VoxelShellFailed", "无法为 {0} 生成碰撞体素壳:\n{1}"),
				FText::FromString(Asset->GetName()),
				FText::FromString(Error.IsEmpty() ? TEXT("未知错误，请查看输出日志。") : Error)));
			continue;
		}

		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(
			TEXT("已把碰撞体素壳写入破裂资产: %s%s\n\n%s\n\n下一步:\n")
			TEXT("1. 双击该破裂资产，进入破裂编辑器\n")
			TEXT("2. 在详情面板用「体素壳用作碰撞」决定它是否同时作为碰撞体；关闭则只作用于破裂\n")
			TEXT("3. 设置 Chaos 破裂参数并解算，之后即可像 GeometryCollection 一样拖入关卡 / 蓝图使用"),
			*FractureAsset->GetName(),
			bCreatedAsset ? TEXT("（本次新建）") : TEXT("（沿用已有资产）"),
			*Result.Summary)));
	}
}

void FAssetTypeActions_GaussianSplatAsset::ExecuteGenerateFractureAsset(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects)
{
	// 第一个有效资产既作为破裂资产的源资产，也决定新资产的存放目录与命名。
	UGaussianSplatAsset* SourceAsset = nullptr;
	for (const TWeakObjectPtr<UGaussianSplatAsset>& AssetPtr : Objects)
	{
		if (UGaussianSplatAsset* Asset = AssetPtr.Get())
		{
			SourceAsset = Asset;
			break;
		}
	}
	if (!SourceAsset)
	{
		return;
	}

	// 与源资产同目录，命名 "<源资产>_Fracture"。已存在就复用它 —— 破裂资产是
	// splat 资产的唯一对应物，重复生成只会得到看着一样的第二个资产。
	if (UGaussianSplatFractureAsset* ExistingAsset = FindFractureAsset(SourceAsset))
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(
			TEXT("%s 已经有破裂资产了: %s\n\n双击该资产即可进入破裂编辑器；若要重新生成碰撞体素壳，用「生成碰撞体素网格」。"),
			*SourceAsset->GetName(), *ExistingAsset->GetName())));
		return;
	}

	bool bCreatedAsset = false;
	UGaussianSplatFractureAsset* NewAsset = FindOrCreateFractureAsset(SourceAsset, bCreatedAsset);
	if (!NewAsset)
	{
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("GenerateFractureAssetFailed", "无法创建破裂资产。"));
		return;
	}

	FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(
		TEXT("已生成破裂资产: %s\n\n源 Splat 资产: %s\n\n下一步:\n")
		TEXT("1. 双击该破裂资产，进入破裂编辑器\n")
		TEXT("2. 在编辑器里生成碰撞体素壳并设置 Chaos 破裂参数\n")
		TEXT("3. 解算后即可把资产拖入关卡 / 蓝图使用"),
		*NewAsset->GetName(), *SourceAsset->GetName())));
}

// ----------------------------------------------------------------------------
// 已废弃（v2 重构）：旧的「绑定破裂骨头」入口。
// 它把骨头绑定与断面 splat 写回 UGaussianSplatAsset，而 v11 起绑定数据归属
// UGaussianSplatFractureAsset，Content Browser 也不再提供该菜单项。绑定改由
// 破裂编辑器在解算流程中调用 FGaussianSplatBoneBinder::BindAssetToCollection
// （源资产 + 破裂资产 + GeometryCollection 三参）完成。
// 代码保留不删，仅用 #if 0 停用以防误用；不要直接启用，先确认绑定目标。
// ----------------------------------------------------------------------------
#if 0
void FAssetTypeActions_GaussianSplatAsset::ExecuteBindFractureBones(TArray<TWeakObjectPtr<UGaussianSplatAsset>> Objects)
{
	TArray<UGaussianSplatAsset*> Assets;
	for (const TWeakObjectPtr<UGaussianSplatAsset>& AssetPtr : Objects)
	{
		if (UGaussianSplatAsset* Asset = AssetPtr.Get())
		{
			Assets.Add(Asset);
		}
	}
	if (Assets.Num() == 0)
	{
		return;
	}

	// One target collection for the whole selection: the proxy mesh of an asset
	// is cut into exactly one collection, and batch binding is the common case.
	FOpenAssetDialogConfig DialogConfig;
	DialogConfig.DialogTitleOverride = LOCTEXT("BindFractureBonesDialogTitle", "选择切割代理网格得到的 GeometryCollection");
	DialogConfig.bAllowMultipleSelection = false;
	DialogConfig.AssetClassNames.Add(FTopLevelAssetPath(FName(TEXT("/Script/GeometryCollectionEngine")), FName(TEXT("GeometryCollection"))));

	const TArray<FAssetData> SelectedAssets = IContentBrowserSingleton::Get().CreateModalOpenAssetDialog(DialogConfig);
	if (SelectedAssets.Num() == 0)
	{
		return;
	}

	const UGeometryCollection* Collection = Cast<UGeometryCollection>(SelectedAssets[0].GetAsset());
	if (!Collection)
	{
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("BindFractureBonesBadCollection", "所选资产不是 GeometryCollection。"));
		return;
	}

	FString Report;
	int32 SuccessCount = 0;
	for (UGaussianSplatAsset* Asset : Assets)
	{
		UE_LOG(LogTemp, Log, TEXT("Binding fracture bones for asset '%s' to collection '%s'"),
			*Asset->GetName(), *Collection->GetName());

		const bool bHadBinding = Asset->HasBoneBinding();
		const FGaussianSplatBoneBindResult Result = FGaussianSplatBoneBinder::BindAssetToCollection(Asset, Collection);

		if (!Result.bSuccess)
		{
			Report += FString::Printf(TEXT("[失败] %s\n%s\n\n"), *Asset->GetName(),
				Result.Error.IsEmpty() ? TEXT("未知错误，请查看输出日志。") : *Result.Error);
			continue;
		}

		++SuccessCount;
		Report += FString::Printf(TEXT("[完成] %s%s\n%s\n\n"), *Asset->GetName(),
			bHadBinding ? TEXT("（已替换原有绑定）") : TEXT(""), *Result.Summary);
	}

	FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(
		TEXT("绑定破裂骨头: %d / %d 个资产成功\n\n%s\n下一步:\n")
		TEXT("1. 把 GeometryCollection 与 splat 资产放入关卡\n")
		TEXT("2. 让 splat 组件的世界变换与几何集合组件一致（代理网格在原点切割）\n")
		TEXT("3. 在 splat 组件的【高斯泼溅|破裂】分类里，把【破裂几何集合组件】指向该 GeometryCollectionComponent\n")
		TEXT("4. 触发破裂：splat 会跟随各自所属的碎块一起飞散"),
		SuccessCount, Assets.Num(), *Report)));
}
#endif

#undef LOCTEXT_NAMESPACE
