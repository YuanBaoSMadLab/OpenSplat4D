// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplatAssetViewport.h"
#include "GaussianSplatAsset.h"
#include "NanoGSGaussianSplatActor.h"
#include "NanoGSGaussianSplatComponent.h"
#include "AdvancedPreviewScene.h"
#include "EngineUtils.h"
#include "Engine/World.h"

#define LOCTEXT_NAMESPACE "GaussianSplatAssetViewport"

//////////////////////////////////////////////////////////////////////////
// FGaussianSplatAssetViewportClient
//////////////////////////////////////////////////////////////////////////

FGaussianSplatAssetViewportClient::FGaussianSplatAssetViewportClient(FPreviewScene* InPreviewScene)
	: FEditorViewportClient(nullptr, nullptr, nullptr)
{
	// Create an advanced preview scene for proper lighting and environment
	FAdvancedPreviewScene::ConstructionValues ConstructionValues;
	ConstructionValues.LightBrightness = 3.0f;
	ConstructionValues.SkyBrightness = 1.0f;

	PreviewScene = MakeShareable(new FAdvancedPreviewScene(ConstructionValues));

	// Connect preview scene to the viewport client (protected member of FEditorViewportClient)
	// This ensures the viewport renders the preview scene, not the main world
	FEditorViewportClient::PreviewScene = PreviewScene.Get();

	// Set default camera position
	SetViewLocation(FVector(0, 0, 100));
	SetViewRotation(FRotator(-15.0f, 0.0f, 0.0f));
}

FGaussianSplatAssetViewportClient::~FGaussianSplatAssetViewportClient()
{
	if (PreviewActor.IsValid())
	{
		UWorld* PreviewWorld = PreviewScene.IsValid() ? PreviewScene->GetWorld() : nullptr;
		if (PreviewWorld)
		{
			PreviewWorld->DestroyActor(PreviewActor.Get(), false, false);
		}
		PreviewActor.Reset();
	}
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
				// Position camera to view the asset
				FVector CameraOffset(Extent * 1.5f, Extent * 1.5f, Extent * 0.8f);
				SetViewLocation(Center + CameraOffset);
				SetViewRotation((Center - (Center + CameraOffset)).Rotation());
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

void SGaussianSplatAssetViewport::SetSplatAsset(UGaussianSplatAsset* InAsset)
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->SetSplatAsset(InAsset);
	}
}

#undef LOCTEXT_NAMESPACE
