#pragma once

#include "CoreMinimal.h"
#include "EditorReimportHandler.h"

/**
 * Reimport handler for UOpenSplat4DPointCloud assets. Because FReimportHandler's
 * constructor registers the instance with FReimportManager, a single static
 * instance of this class (declared in OpenSplat4DReimport.cpp) makes the
 * Content Browser "Reimport" action work for imported .ply / .4dgs assets.
 */
class FOpenSplat4DPointCloudReimportHandler : public FReimportHandler
{
public:
	virtual bool CanReimport(UObject* Obj, TArray<FString>& OutFilenames) override;
	virtual void SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths) override;
	virtual EReimportResult::Type Reimport(UObject* Obj) override;
	virtual int32 GetPriority() const override { return 1; }
};
