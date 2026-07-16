#include "OpenSplat4DReimport.h"
#include "OpenSplat4DEditorModule.h"
#include "OpenSplat4DPointCloud.h"
#include "Misc/Paths.h"

// A single persistent instance. Its constructor registers it with
// FReimportManager, and its destructor unregisters it on module shutdown.
static FOpenSplat4DPointCloudReimportHandler GOpenSplat4DPointCloudReimportHandler;

bool FOpenSplat4DPointCloudReimportHandler::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
	UOpenSplat4DPointCloud* Asset = Cast<UOpenSplat4DPointCloud>(Obj);
	if (!Asset)
	{
		return false;
	}
	if (!Asset->SourceFilePath.IsEmpty())
	{
		OutFilenames.Add(Asset->SourceFilePath);
	}
	return true;
}

void FOpenSplat4DPointCloudReimportHandler::SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths)
{
	UOpenSplat4DPointCloud* Asset = Cast<UOpenSplat4DPointCloud>(Obj);
	if (Asset && NewReimportPaths.Num() > 0)
	{
		Asset->SourceFilePath = NewReimportPaths[0];
	}
}

EReimportResult::Type FOpenSplat4DPointCloudReimportHandler::Reimport(UObject* Obj)
{
	UOpenSplat4DPointCloud* Asset = Cast<UOpenSplat4DPointCloud>(Obj);
	if (!Asset)
	{
		return EReimportResult::Failed;
	}

	const FString Path = Asset->SourceFilePath;
	if (Path.IsEmpty() || !FPaths::FileExists(Path))
	{
		return EReimportResult::Failed;
	}

	const FString Ext = FPaths::GetExtension(Path).ToLower();
	bool bOk = false;
	if (Ext == TEXT("ply"))
	{
		Asset->LoadFromFile(Path);
		bOk = Asset->GetPointCount() > 0;
	}
	else if (Ext == TEXT("4dgs"))
	{
		bOk = Asset->LoadFrom4DGS(Path);
	}

	if (!bOk)
	{
		UE_LOG(LogOpenSplat4DEditor, Warning, TEXT("OpenSplat4D: reimport of '%s' failed."), *Path);
		return EReimportResult::Failed;
	}

	Asset->MarkPackageDirty();
	Asset->OnPointsChanged.Broadcast();
	return EReimportResult::Succeeded;
}
