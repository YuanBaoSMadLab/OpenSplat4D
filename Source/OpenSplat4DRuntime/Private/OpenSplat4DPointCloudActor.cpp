#include "OpenSplat4DPointCloudActor.h"
#include "OpenSplat4DPointCloud.h"
#include "NanoGSGaussianSplatComponent.h"

AOpenSplat4DPointCloudActor::AOpenSplat4DPointCloudActor(const FObjectInitializer& ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = true;
}

void AOpenSplat4DPointCloudActor::BeginPlay()
{
	Super::BeginPlay();

	if (PointCloud)
	{
		CurrentTime = PointCloud->TimeStart;
	}

	if (bAutoPlay)
	{
		Play();
	}
}

void AOpenSplat4DPointCloudActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bIsPlaying && PointCloud)
	{
		const float Span = PointCloud->TimeEnd - PointCloud->TimeStart;
		if (!FMath::IsNearlyZero(Span))
		{
			CurrentTime += (DeltaSeconds * PlayRate) * Span;
			if (CurrentTime > PointCloud->TimeEnd)
			{
				if (bLooping)
				{
					CurrentTime = PointCloud->TimeStart + FMath::Fmod(CurrentTime - PointCloud->TimeStart, Span);
				}
				else
				{
					CurrentTime = PointCloud->TimeEnd;
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
	if (PointCloud)
	{
		CurrentTime = FMath::Clamp(Time, PointCloud->TimeStart, PointCloud->TimeEnd);
	}
}

void AOpenSplat4DPointCloudActor::SetPointCloud(UOpenSplat4DPointCloud* InCloud)
{
	PointCloud = InCloud;
	if (GaussianSplatComponent && InCloud)
	{
		GaussianSplatComponent->SetSplatAsset(InCloud);
	}
	if (PointCloud)
	{
		CurrentTime = PointCloud->TimeStart;
	}
}

UGaussianSplatAsset* AOpenSplat4DPointCloudActor::GetPointCloud() const
{
	return PointCloud;
}
