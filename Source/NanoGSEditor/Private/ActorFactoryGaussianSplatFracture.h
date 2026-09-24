// Copyright OpenSplat4D 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ActorFactories/ActorFactory.h"
#include "ActorFactoryGaussianSplatFracture.generated.h"

/**
 * 把「破裂资产」从内容浏览器拖进场景 = 生成一个 AGaussianSplatFractureActor。
 * 与引擎的 UActorFactoryGeometryCollection（拖 Chaos 资产）行为对齐：拖进去
 * 就能在蓝图里对该 Actor 的 GeometryCollectionComponent 调 Chaos 破裂节点。
 *
 * 工厂无需手工注册：UEditorEngine::InitializeObjectReferences 会扫描所有
 * bShouldAutoRegister 的 UActorFactory 派生类并自动 NewObject 一份。
 */
UCLASS(MinimalAPI, config = Editor)
class UActorFactoryGaussianSplatFracture : public UActorFactory
{
	GENERATED_BODY()

public:
	UActorFactoryGaussianSplatFracture();

	//~ Begin UActorFactory Interface
	virtual bool CanCreateActorFrom(const FAssetData& AssetData, FText& OutErrorMsg) override;
	virtual void PostSpawnActor(UObject* Asset, AActor* NewActor) override;
	virtual UObject* GetAssetFromActorInstance(AActor* ActorInstance) override;
	//~ End UActorFactory Interface
};
