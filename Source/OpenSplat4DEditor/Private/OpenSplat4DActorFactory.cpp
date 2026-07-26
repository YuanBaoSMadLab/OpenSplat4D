#include "OpenSplat4DActorFactory.h"
#include "OpenSplat4DPointCloud.h"
#include "OpenSplat4DPointCloudActor.h"
#include "OpenSplat4DSplatActor.h"
#include "OpenSplat4DLocalization.h"
#include "OpenSplat4DNiagaraSetup.h"

#define LOCTEXT_NAMESPACE "OpenSplat4D"

UActorFactory_OpenSplat4DPointCloud::UActorFactory_OpenSplat4DPointCloud(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// [DISABLED] 自研 ISMC 管线已停用。
	// PLY 导入和 Actor 创建由 NanoGS 模块接管（GaussianSplatAssetFactory + ActorFactoryGaussianSplat）。
	// 保留此类以兼容已存在的 OpenSplat4DPointCloud 资产，但 NewActorClass 设为 Niagara 路径（保留代码，已禁用）。
	DisplayName = OS4D_TEXT("OpenSplat4D Point Cloud (Legacy)");
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

	// 主路径：自研渲染管线（SplatActor）
	if (AOpenSplat4DSplatActor* SplatActor = Cast<AOpenSplat4DSplatActor>(NewActor))
	{
		SplatActor->SetPointCloud(Cloud);
		return;
	}

	// 兼容路径：若用户从右键菜单选择了 Niagara Actor（模式 2）
	if (AOpenSplat4DPointCloudActor* NiagaraActor = Cast<AOpenSplat4DPointCloudActor>(NewActor))
	{
		// [DISABLED] 全面禁用 Niagara 路径：不再自动创建 Niagara 资产。
		// NiagaraActor 需要时由用户手动调用 OpenSplat4DNiagaraSetup::EnsureAssetsExist()
		// OpenSplat4DNiagaraSetup::EnsureAssetsExist();
		NiagaraActor->SetPointCloud(Cloud);
		return;
	}
}

UObject* UActorFactory_OpenSplat4DPointCloud::GetAssetFromActorInstance(AActor* ActorInstance)
{
	// 兼容多种 Actor 类型（自研管线 + Niagara 管线）
	if (AOpenSplat4DSplatActor* SplatActor = Cast<AOpenSplat4DSplatActor>(ActorInstance))
	{
		return SplatActor->GetPointCloud();
	}
	if (AOpenSplat4DPointCloudActor* NiagaraActor = Cast<AOpenSplat4DPointCloudActor>(ActorInstance))
	{
		return NiagaraActor->GetPointCloud();
	}
	return nullptr;
}

#undef LOCTEXT_NAMESPACE
