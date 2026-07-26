// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatAssetViewport.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatActor.h"
#include "GaussianSplatComponent.h"
#include "AdvancedPreviewScene.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"

#define LOCTEXT_NAMESPACE "GaussianSplatAssetViewport"

//////////////////////////////////////////////////////////////////////////
// FGaussianSplatAssetViewportClient
//////////////////////////////////////////////////////////////////////////

FGaussianSplatAssetViewportClient::FGaussianSplatAssetViewportClient(FEditorViewportClient* InParentClient)
	: FEditorViewportClient(nullptr, nullptr, nullptr)
{
	// Create an advanced preview scene for proper lighting and environment
	FAdvancedPreviewScene::ConstructionValues ConstructionValues;
	ConstructionValues.LightBrightness = 3.0f;
	ConstructionValues.SkyBrightness = 1.0f;
	ConstructionValues.SkyLightBrightness = 1.0f;

	PreviewScene = MakeShareable(new FAdvancedPreviewScene(ConstructionValues));

	// Set the preview scene for the viewport client
	PreviewScene->SetSimulatePhysics(false);

	// Set default camera position
	SetViewLocation(FVector(0, 0, 100));
	SetViewRotation(FRotator(-15.0f, 0.0f, 0.0f));
	SetViewLocationForOrbiting(FVector(0, 0, 0));
	SetOrbitDistance(500.0f);

	// Enable orbit camera by default
	bSetListenerPosition = false;
	bDrawAxes = false;
	EngineShowFlags.SetStats(false);
	EngineShowFlags.SetEnableLightFunctions(false);

	// Set background color
	BackgroundSettings.BackgroundColor = FLinearColor(0.1f, 0.1f, 0.1f, 1.0f);
}

FGaussianSplatAssetViewportClient::~FGaussianSplatAssetViewportClient()
{
	if (PreviewActor.IsValid())
	{
		PreviewScene->GetWorld()->DestroyActor(PreviewActor.Get(), false, false);
		PreviewActor.Reset();
	}
}

void FGaussianSplatAssetViewportClient::Draw(FViewport* Viewport, FCanvas* Canvas)
{
	FEditorViewportClient::Draw(Viewport, Canvas);
}

FLinearColor FGaussianSplatAssetViewportClient::GetBackgroundColor() const
{
	return FLinearColor(0.1f, 0.1f, 0.1f, 1.0f);
}

void FGaussianSplatAssetViewportClient::Tick(float DeltaSeconds)
{
	FEditorViewportClient::Tick(DeltaSeconds);

	// Tick the preview scene's world
	if (PreviewScene.IsValid())
	{
		UWorld* PreviewWorld = PreviewScene->GetWorld();
		if (PreviewWorld)
		{
			// Update the preview world
			PreviewWorld->Tick(LEVELTICK_All, DeltaSeconds);
		}
	}
}

void FGaussianSplatAssetViewportClient::SetSplatAsset(UGaussianSplatAsset* InAsset)
{
	CurrentAsset = InAsset;

	if (!PreviewScene.IsValid())
	{
		return;
	}

	UWorld* PreviewWorld = PreviewScene->GetWorld();
	if (!PreviewWorld)
	{
		return;
	}

	// Destroy existing preview actor
	if (PreviewActor.IsValid())
	{
		PreviewWorld->DestroyActor(PreviewActor.Get(), false, false);
		PreviewActor.Reset();
	}

	if (!InAsset || InAsset->GetSplatCount() <= 0)
	{
		return;
	}

	// Spawn a preview actor in the preview world
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.bNoFail = true;

	AGaussianSplatActor* NewActor = PreviewWorld->SpawnActor<AGaussianSplatActor>(
		AGaussianSplatActor::StaticClass(),
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		SpawnParams);

	if (NewActor && NewActor->GaussianSplatComponent)
	{
		NewActor->GaussianSplatComponent->SetSplatAsset(InAsset);
		PreviewActor = NewActor;

		// Adjust camera to frame the asset
		FBox Bounds = InAsset->GetBounds();
		if (Bounds.IsValid)
		{
			FVector Center = Bounds.GetCenter();
			float Extent = Bounds.GetExtent().Length();
			if (Extent > 0.0f)
			{
				SetViewLocationForOrbiting(Center);
				SetOrbitDistance(Extent * 2.5f);
			}
		}

		UE_LOG(LogTemp, Log, TEXT("GaussianSplatAssetViewport: Preview actor created for asset '%s' (%d splats)"),
			*InAsset->GetName(), InAsset->GetSplatCount());
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("GaussianSplatAssetViewport: Failed to create preview actor"));
	}
}

//////////////////////////////////////////////////////////////////////////
// SGaussianSplatAssetViewport
//////////////////////////////////////////////////////////////////////////

void SGaussianSplatAssetViewport::Construct(const FArguments& InArgs, TSharedPtr<FGaussianSplatAssetViewportClient> InClient)
{
	ViewportClient = InClient;

	SEditorViewport::Construct(
		SEditorViewport::FArguments()
		.IsEnabled(true)
		.AddMetaData<FTagMetaData>(FTagMetaData(TEXT("GaussianSplatAssetEditor.Viewport")))
	);

	if (ViewportClient.IsValid())
	{
		ViewportClient->Viewport = SharedThis(this);
	}
}

TSharedRef<FEditorViewportClient> SGaussianSplatAssetViewport::MakeEditorViewportClient()
{
	if (ViewportClient.IsValid())
	{
		return ViewportClient.ToSharedRef();
	}

	// Create a default client if none provided
	ViewportClient = MakeShareable(new FGaussianSplatAssetViewportClient());
	return ViewportClient.ToSharedRef();
}

TSharedPtr<SWidget> SGaussianSplatAssetViewport::MakeViewportToolbar()
{
	return nullptr;
}

void SGaussianSplatAssetViewport::SetSplatAsset(UGaussianSplatAsset* InAsset)
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->SetSplatAsset(InAsset);
	}
}

#undef LOCTEXT_NAMESPACE
