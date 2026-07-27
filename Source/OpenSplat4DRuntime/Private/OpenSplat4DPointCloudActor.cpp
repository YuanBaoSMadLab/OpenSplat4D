#include "OpenSplat4DPointCloudActor.h"
#include "OpenSplat4DPointCloud.h"
#include "GaussianSplatComponent.h"

AOpenSplat4DPointCloudActor::AOpenSplat4DPointCloudActor(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = true;
}

void AOpenSplat4DPointCloudActor::BeginPlay()
{
	Super::BeginPlay();

	UOpenSplat4DPointCloud* Cloud = Cast<UOpenSplat4DPointCloud>(GaussianSplatComponent->GetSplatAsset());
	if (Cloud)
	{
		CurrentTime = Cloud->TimeStart;
	}

	if (bAutoPlay)
	{
		Play();
	}
}

void AOpenSplat4DPointCloudActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UOpenSplat4DPointCloud* Cloud = Cast<UOpenSplat4DPointCloud>(GaussianSplatComponent->GetSplatAsset());
	if (bIsPlaying && Cloud)
	{
		const float Span = Cloud->TimeEnd - Cloud->TimeStart;
		if (!FMath::IsNearlyZero(Span))
		{
			CurrentTime += (DeltaSeconds * PlayRate) * Span;
			if (CurrentTime > Cloud->TimeEnd)
			{
				if (bLooping)
				{
					CurrentTime = Cloud->TimeStart + FMath::Fmod(CurrentTime - Cloud->TimeStart, Span);
				}
				else
				{
					CurrentTime = Cloud->TimeEnd;
					bIsPlaying = false;
				}
			}
		}
	}
}

void AOpenSplat4DPointCloudActor::Play()
{
	bIsPlaying = true;
}

void AOpenSplat4DPointCloudActor::Pause()
{
	bIsPlaying = false;
}

void AOpenSplat4DPointCloudActor::Seek(float Time)
{
	UOpenSplat4DPointCloud* Cloud = Cast<UOpenSplat4DPointCloud>(GaussianSplatComponent->GetSplatAsset());
	if (Cloud)
	{
		CurrentTime = FMath::Clamp(Time, Cloud->TimeStart, Cloud->TimeEnd);
	}
}
