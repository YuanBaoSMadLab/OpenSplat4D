#include "OpenSplat4DActorFactory.h"
#include "OpenSplat4DPointCloud.h"
#include "OpenSplat4DPointCloudActor.h"
#include "OpenSplat4DLocalization.h"

#define LOCTEXT_NAMESPACE "OpenSplat4D"

UActorFactory_OpenSplat4DPointCloud::UActorFactory_OpenSplat4DPointCloud(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DisplayName = OS4D_TEXT("OpenSplat4D Point Cloud");
	NewActorClass = AOpenSplat4DPointCloudActor::StaticClass();
	bUseSurfaceOrientation = true;
}

bool UActorFactory_OpenSplat4DPointCloud::CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg)
{
	if (!AssetData.IsValid() || !AssetData.IsInstanceOf(UOpenSplat4DPointCloud::StaticClass()))
	{
		OutErrorMsg = OS4D_TEXT("A valid OpenSplat4D point cloud must be specified.");
		return false;
	}
	return true;
}

void UActorFactory_OpenSplat4DPointCloud::PostSpawnActor(UObject* Asset, AActor* NewActor)
{
	Super::PostSpawnActor(Asset, NewActor);

	UOpenSplat4DPointCloud* Cloud = CastChecked<UOpenSplat4DPointCloud>(Asset);
	AOpenSplat4DPointCloudActor* Actor = CastChecked<AOpenSplat4DPointCloudActor>(NewActor);
	Actor->SetPointCloud(Cloud);
}

UObject* UActorFactory_OpenSplat4DPointCloud::GetAssetFromActorInstance(AActor* ActorInstance)
{
	check(ActorInstance->IsA(NewActorClass));
	AOpenSplat4DPointCloudActor* Actor = CastChecked<AOpenSplat4DPointCloudActor>(ActorInstance);
	return Actor->GetPointCloud();
}

#undef LOCTEXT_NAMESPACE
