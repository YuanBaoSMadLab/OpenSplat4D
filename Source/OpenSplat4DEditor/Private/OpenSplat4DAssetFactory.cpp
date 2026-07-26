#include "OpenSplat4DAssetFactory.h"
#include "OpenSplat4DEditorModule.h"
#include "OpenSplat4DPointCloud.h"
#include "OpenSplat4DPointCloudEditor.h"
#include "OpenSplat4DLocalization.h"
#include "OpenSplat4DNiagaraSetup.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "OpenSplat4DAssetFactory"

UOpenSplat4DPointCloudAssetFactory::UOpenSplat4DPointCloudAssetFactory()
{
	// [DISABLED] 自研管线已停用，PLY 导入由 NanoGS 模块的 GaussianSplatAssetFactory 接管。
	// 保留此类以兼容已存在的 OpenSplat4DPointCloud 资产，但不再注册 PLY/4dgs 格式。
	bCreateNew = false;
	bEditAfterNew = true;
	bEditorImport = false;
	SupportedClass = UOpenSplat4DPointCloud::StaticClass();
	// Formats.Add(TEXT("ply;PLY Point Cloud (3DGS)"));
	// Formats.Add(TEXT("4dgs;OpenSplat4D Point Cloud"));
}

UObject* UOpenSplat4DPointCloudAssetFactory::FactoryCreateNew(
	UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
	UObject* Context, FFeedbackContext* Warn)
{
	// [FIX] UE5.8 sometimes routes file-drops through FactoryCreateNew instead of
	// FactoryCreateFile, even when bCreateNew=false. When a source file is pending
	// (drag-drop import), delegate to the real file importer. Without this the
	// asset is created empty (1 KB .uasset from a 65 MB .ply).
	const FString ImportFilename = GetCurrentFilename();
	if (!ImportFilename.IsEmpty())
	{
		bool bCanceled = false;
		return FactoryCreateFile(InClass, InParent, InName, Flags, ImportFilename, nullptr, Warn, bCanceled);
	}

	return NewObject<UOpenSplat4DPointCloud>(InParent, InClass, InName, Flags);
}

bool UOpenSplat4DPointCloudAssetFactory::FactoryCanImport(const FString& Filename)
{
	const FString Ext = FPaths::GetExtension(Filename).ToLower();
	return Ext == TEXT("ply") || Ext == TEXT("4dgs");
}

UObject* UOpenSplat4DPointCloudAssetFactory::FactoryCreateFile(
	UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
	const FString& Filename, const TCHAR* Parms, FFeedbackContext* Warn,
	bool& bOutOperationCanceled)
{
	bOutOperationCanceled = false;

	// [DISABLED] 全面禁用 Niagara 路径：导入时不再自动创建 Niagara 资产。
	// OpenSplat4DNiagaraSetup::EnsureAssetsExist();

	UOpenSplat4DPointCloud* Asset = NewObject<UOpenSplat4DPointCloud>(InParent, InClass, InName, Flags);
	if (!Asset)
	{
		return nullptr;
	}

	const FString Ext = FPaths::GetExtension(Filename).ToLower();
	bool bOk = false;
		if (Ext == TEXT("ply"))
		{
			Asset->LoadFromFile(Filename);
			bOk = Asset->GetPointCount() > 0;
			UE_LOG(LogOpenSplat4DEditor, Log, TEXT("OpenSplat4D: 瀵煎叆 '%s' -> 鐐规暟=%d"), *Filename, Asset->GetPointCount());
		}
	else if (Ext == TEXT("4dgs"))
	{
		bOk = Asset->LoadFrom4DGS(Filename);
	}

	if (!bOk)
	{
		UE_LOG(LogOpenSplat4DEditor, Warning, TEXT("OpenSplat4D: failed to import '%s'."), *Filename);
		return nullptr;
	}

	Asset->SourceFilePath = Filename;
	Asset->MarkPackageDirty();
	return Asset;
}

FText UAssetDefinition_OpenSplat4DPointCloud::GetAssetDisplayName() const
{
	return OS4D_TEXT("OpenSplat4D Point Cloud");
}

TSoftClassPtr<UObject> UAssetDefinition_OpenSplat4DPointCloud::GetAssetClass() const
{
	return UOpenSplat4DPointCloud::StaticClass();
}

FLinearColor UAssetDefinition_OpenSplat4DPointCloud::GetAssetColor() const
{
	return FLinearColor(0.0f, 0.5f, 1.0f);
}

TConstArrayView<FAssetCategoryPath> UAssetDefinition_OpenSplat4DPointCloud::GetAssetCategories() const
{
	static const auto Categories = { EAssetCategoryPaths::Misc };
	return Categories;
}

EAssetCommandResult UAssetDefinition_OpenSplat4DPointCloud::OpenAssets(const FAssetOpenArgs& OpenArgs) const
{
	for (UOpenSplat4DPointCloud* PointCloud : OpenArgs.LoadObjects<UOpenSplat4DPointCloud>())
	{
		TSharedRef<FOpenSplat4DPointCloudEditor> NewEditor(new FOpenSplat4DPointCloudEditor());
		NewEditor->InitEditor(OpenArgs.GetToolkitMode(), OpenArgs.ToolkitHost, PointCloud);
	}
	return EAssetCommandResult::Handled;
}

bool UOpenSplat4DPointCloudAssetFactory::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
	if (UOpenSplat4DPointCloud* Asset = Cast<UOpenSplat4DPointCloud>(Obj))
	{
		if (!Asset->SourceFilePath.IsEmpty())
		{
			OutFilenames.Add(Asset->SourceFilePath);
			return true;
		}
	}
	return false;
}

void UOpenSplat4DPointCloudAssetFactory::SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths)
{
	if (UOpenSplat4DPointCloud* Asset = Cast<UOpenSplat4DPointCloud>(Obj))
	{
		if (NewReimportPaths.Num() > 0)
		{
			Asset->SourceFilePath = NewReimportPaths[0];
		}
	}
}

EReimportResult::Type UOpenSplat4DPointCloudAssetFactory::Reimport(UObject* Obj)
{
	UOpenSplat4DPointCloud* Asset = Cast<UOpenSplat4DPointCloud>(Obj);
	if (!Asset)
	{
		return EReimportResult::Failed;
	}
	if (Asset->SourceFilePath.IsEmpty() || !FPaths::FileExists(Asset->SourceFilePath))
	{
		return EReimportResult::Failed;
	}
	Asset->LoadFromFile(Asset->SourceFilePath);
	if (Asset->GetPointCount() > 0)
	{
		Asset->MarkPackageDirty();
		UE_LOG(LogOpenSplat4DEditor, Log, TEXT("OpenSplat4D: Reimport %s -> points=%d"), *Asset->SourceFilePath, Asset->GetPointCount());
		return EReimportResult::Succeeded;
	}
	return EReimportResult::Failed;
}
#undef LOCTEXT_NAMESPACE
