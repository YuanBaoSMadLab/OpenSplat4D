#include "OpenSplat4DPointCloudActor.h"
#include "OpenSplat4DBillboardComponent.h"

AOpenSplat4DPointCloudActor::AOpenSplat4DPointCloudActor(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	Billboard = CreateDefaultSubobject<UOpenSplat4DBillboardComponent>(TEXT("OpenSplat4DBillboard"));
	RootComponent = Billboard;
}

void AOpenSplat4DPointCloudActor::BeginPlay()
{
	Super::BeginPlay();

	if (PointCloud)
	{
		CurrentTime = PointCloud->TimeStart;
	}
	PushStateToBillboard();

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
					CurrentTime = PointCloud->TimeStart
						+ FMath::Fmod(CurrentTime - PointCloud->TimeStart, Span);
				}
				else
				{
					CurrentTime = PointCloud->TimeEnd;
					bIsPlaying = false;
				}
			}
		}
	}

	PushStateToBillboard();
}

void AOpenSplat4DPointCloudActor::PushStateToBillboard()
{
	if (!Billboard)
	{
		return;
	}
	const bool bCloudChanged = Billboard->PointCloud != PointCloud;
	Billboard->PointCloud = PointCloud;
	if (bCloudChanged)
	{
		Billboard->RebuildBuffer();
	}
	Billboard->Time = CurrentTime;
	Billboard->SplatScale = SplatScale;
	Billboard->bTemporalWeighting = (PointCloud && PointCloud->Mode == EOpenSplat4DMode::Dynamic4D);
}

void AOpenSplat4DPointCloudActor::SetPointCloud(UOpenSplat4DPointCloud* InCloud)
{
	PointCloud = InCloud;
	if (PointCloud)
	{
		CurrentTime = PointCloud->TimeStart;
	}
	PushStateToBillboard();
}

void AOpenSplat4DPointCloudActor::SetMode(EOpenSplat4DMode InMode)
{
	if (PointCloud)
	{
		PointCloud->Mode = InMode;
		PointCloud->OnPointsChanged.Broadcast();
	}
	PushStateToBillboard();
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
	PushStateToBillboard();
}

void AOpenSplat4DPointCloudActor::SeekFraction(float Fraction)
{
	if (PointCloud)
	{
		Seek(PointCloud->FractionToTime(Fraction));
	}
}

float AOpenSplat4DPointCloudActor::GetFraction() const
{
	return PointCloud ? PointCloud->TimeToFraction(CurrentTime) : 0.f;
}
