// Copyright OpenSplat4D 2026 All Rights Reserved.

#include "GaussianSplatFractureActor.h"

#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatFractureAsset.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "NanoGSGaussianSplatComponent.h"

AGaussianSplatFractureActor::AGaussianSplatFractureActor()
{
	// 破裂由几何集合组件自己驱动（Chaos 求解 + 物理），Actor 不需要 tick。
	PrimaryActorTick.bCanEverTick = false;

	USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(SceneRoot);

	// 与 Chaos 资产拖进场景得到的 Actor 等价：一个普通的 GeometryCollection
	// 组件，构造时默认就带物理（BodyInstance.bSimulatePhysics = true、
	// 物体类型 ECC_Destructible、状态 Dynamic），所以这里不再重复开物理。
	GeometryCollectionComponent = CreateDefaultSubobject<UGeometryCollectionComponent>(TEXT("GeometryCollection"));
	GeometryCollectionComponent->SetupAttachment(SceneRoot);

	// 源 splat 点云的渲染组件。与几何集合组件同挂 Root、相对变换均为单位阵，
	// 于是「几何集合组件空间」==「splat 组件本地空间」，骨头 Delta 可直接用。
	GaussianSplatComponent = CreateDefaultSubobject<UGaussianSplatComponent>(TEXT("GaussianSplat"));
	GaussianSplatComponent->SetupAttachment(SceneRoot);
}

void AGaussianSplatFractureActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	SyncFractureBinding();
}

void AGaussianSplatFractureActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();

	SyncFractureBinding();
}

#if WITH_EDITOR
void AGaussianSplatFractureActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	SyncFractureBinding();
}
#endif

void AGaussianSplatFractureActor::SetFractureAsset(UGaussianSplatFractureAsset* NewAsset)
{
	if (GaussianSplatComponent == nullptr)
	{
		return;
	}

	GaussianSplatComponent->SetFractureAsset(NewAsset);
	SyncFractureBinding();
}

UGaussianSplatFractureAsset* AGaussianSplatFractureActor::GetFractureAsset() const
{
	return GaussianSplatComponent ? GaussianSplatComponent->FractureAsset : nullptr;
}

void AGaussianSplatFractureActor::ResetFracture()
{
	if (GeometryCollectionComponent)
	{
		GeometryCollectionComponent->ResetState();
	}
}

void AGaussianSplatFractureActor::SyncFractureBinding()
{
	if (GaussianSplatComponent == nullptr || GeometryCollectionComponent == nullptr)
	{
		return;
	}

	UGaussianSplatFractureAsset* Fracture = GaussianSplatComponent->FractureAsset;
	if (Fracture == nullptr)
	{
		GaussianSplatComponent->FractureGeometryCollection = nullptr;
		return;
	}

	// SetFractureAsset 里的源资产回落只认「已经在内存里」的对象；从内容浏览器
	// 拖进来 / 复制出来的 Actor 通常没有，于是渲染不出东西、存档后 SplatAsset
	// 仍是空。这里补一次加载。
	if (GaussianSplatComponent->GetSplatAsset() == nullptr)
	{
		if (UGaussianSplatAsset* SourceAsset = Fracture->LoadSourceSplatAsset())
		{
			GaussianSplatComponent->SetSplatAsset(SourceAsset);
		}
	}

	// 与 Chaos 资产的 ActorFactory 对齐：把几何集合交给组件。
	// SetRestCollection(nullptr) 是空操作（不会清空），而重复设同一个集合会白白
	// 重建一次动态集合与物理状态，所以这里双重守卫。
	if (UGeometryCollection* RestCollection = Cast<UGeometryCollection>(Fracture->GeometryCollection))
	{
		if (GeometryCollectionComponent->GetRestCollection() != RestCollection)
		{
			GeometryCollectionComponent->SetRestCollection(RestCollection);

			// 引擎的 UActorFactoryGeometryCollection 在集合没有物理材质时补一个
			// 「可破坏物默认物理材质」，这里照做，行为与拖 Chaos 资产一致。
			if (RestCollection->PhysicsMaterial == nullptr && GEngine != nullptr)
			{
				GeometryCollectionComponent->SetPhysMaterialOverride(GEngine->DefaultDestructiblePhysMaterial);
			}
		}
	}

	// 骨头来源：splat 组件靠这一句才知道去读哪个几何集合组件的骨头变换。
	GaussianSplatComponent->FractureGeometryCollection = GeometryCollectionComponent;

	// SetFractureAsset -> Update4DTickEnabled 执行时 FractureGeometryCollection
	// 还是空，tick 被关掉了。现在骨头来源已接上，必须重新打开，否则 splat 冻结。
	if (Fracture->HasBoneBinding() && !GaussianSplatComponent->IsComponentTickEnabled())
	{
		GaussianSplatComponent->SetComponentTickEnabled(true);
	}
}
