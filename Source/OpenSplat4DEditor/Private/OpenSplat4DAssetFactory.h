#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "Misc/FeedbackContext.h"
#include "EditorReimportHandler.h"
#include "Factories/ReimportFactory.h"
#include "AssetDefinitionDefault.h"
#include "OpenSplat4DAssetFactory.generated.h"

/**
 * Factory that creates an empty UOpenSplat4DPointCloud asset and imports
 * .ply (3DGS) / .4dgs (native OpenSplat4D) files via drag & drop or the
 * Content Browser import dialog.
 */
UCLASS()
class UOpenSplat4DPointCloudAssetFactory : public UReimportFactory
{
	GENERATED_BODY()

public:
	UOpenSplat4DPointCloudAssetFactory();

	virtual UObject* FactoryCreateNew(
		UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
		UObject* Context, FFeedbackContext* Warn) override;

	virtual UObject* FactoryCreateFile(
		UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
		const FString& Filename, const TCHAR* Parms, FFeedbackContext* Warn,
		bool& bOutOperationCanceled) override;

	virtual bool CanCreateNew() const override { return true; }
	virtual bool ShouldShowInNewMenu() const override { return true; }
	virtual bool FactoryCanImport(const FString& Filename) override;

	// [FIX] Reimport support so right-click -> Reimport on an existing asset
	// reloads it from its source file (SourceFilePath) instead of staying empty.
	virtual bool CanReimport(UObject* Obj, TArray<FString>& OutFilenames) override;
	virtual void SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths) override;
	virtual EReimportResult::Type Reimport(UObject* Obj) override;
};

/** Asset definition so double-clicking a point cloud opens the OpenSplat4D editor. */
UCLASS()
class UAssetDefinition_OpenSplat4DPointCloud : public UAssetDefinitionDefault
{
	GENERATED_BODY()
public:
	virtual FText GetAssetDisplayName() const override final;
	TSoftClassPtr<UObject> GetAssetClass() const override final;
	virtual FLinearColor GetAssetColor() const override final;
	TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override final;
	EAssetCommandResult OpenAssets(const FAssetOpenArgs& OpenArgs) const override final;
};
