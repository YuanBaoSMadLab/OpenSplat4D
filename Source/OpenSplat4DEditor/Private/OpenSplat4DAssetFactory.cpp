#include "OpenSplat4DAssetFactory.h"
#include "OpenSplat4DEditorModule.h"
#include "OpenSplat4DPointCloud.h"
#include "OpenSplat4DPointCloudEditor.h"
#include "OpenSplat4DLocalization.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "OpenSplat4DAssetFactory"

UOpenSplat4DPointCloudAssetFactory::UOpenSplat4DPointCloudAssetFactory()
{
	bCreateNew = true;
	bEditAfterNew = true;
	bEditorImport = true;
	SupportedClass = UOpenSplat4DPointCloud::StaticClass();
	Formats.Add(TEXT("ply;PLY Point Cloud (3DGS)"));
	Formats.Add(TEXT("4dgs;OpenSplat4D Point Cloud"));
}

UObject* UOpenSplat4DPointCloudAssetFactory::FactoryCreateNew(
	UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
	UObject* Context, FFeedbackContext* Warn)
{
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

FText FOpenSplat4DPointCloudAssetActions::GetName() const
{
	return OS4D_TEXT("OpenSplat4D Point Cloud");
}

FColor FOpenSplat4DPointCloudAssetActions::GetTypeColor() const
{
	return FColor(120, 200, 255);
}

UClass* FOpenSplat4DPointCloudAssetActions::GetSupportedClass() const
{
	return UOpenSplat4DPointCloud::StaticClass();
}

uint32 FOpenSplat4DPointCloudAssetActions::GetCategories()
{
	return EAssetTypeCategories::Misc;
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

#undef LOCTEXT_NAMESPACE
