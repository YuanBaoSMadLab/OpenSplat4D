#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OpenSplat4DPointCloud.h"
#include "OpenSplat4DBillboardComponent.h"
#include "OpenSplat4DPointCloudActor.generated.h"

/**
 * Renders an OpenSplat4D point cloud (3DGS or 4DGS) in the level.
 *
 * Uses the self-contained UOpenSplat4DBillboardComponent (no external Niagara
 * System asset required). In Dynamic4D mode the actor advances a global
 * playback time and pushes it, together with the temporal-weighting flag, into
 * the renderer each frame.
 */
UCLASS(CollapseCategories)
class OPENSPLAT4DRUNTIME_API AOpenSplat4DPointCloudActor : public AActor
{
	GENERATED_BODY()
public:
	AOpenSplat4DPointCloudActor(const FObjectInitializer& ObjectInitializer);

	/** The billboard renderer component (created by the actor). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "OpenSplat4D")
	TObjectPtr<UOpenSplat4DBillboardComponent> Billboard;

	/** The point cloud asset rendered by this actor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D")
	TObjectPtr<UOpenSplat4DPointCloud> PointCloud;

	/** Whether playback starts automatically on BeginPlay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D|Playback")
	bool bAutoPlay = true;

	/** Runtime multiplier on every splat's size (see UOpenSplat4DBillboardComponent::SplatScale). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D", meta = (ClampMin = "0.01", ClampMax = "10.0"))
	float SplatScale = 1.f;

	/** Loop the time axis when reaching the end. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D|Playback")
	bool bLooping = true;

	/** Playback speed multiplier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D|Playback", meta = (ClampMin = "0.0"))
	float PlayRate = 1.0f;

	/** Current playback time (within the cloud's [TimeStart, TimeEnd] axis). Read-only. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "OpenSplat4D|Playback")
	float CurrentTime = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "OpenSplat4D|Playback")
	bool bIsPlaying = false;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	void SetPointCloud(UOpenSplat4DPointCloud* InCloud);

	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	void SetMode(EOpenSplat4DMode InMode);

	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	UOpenSplat4DPointCloud* GetPointCloud() const { return PointCloud; }

	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	void Play();

	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	void Pause();

	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	void Seek(float Time);

	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	void SeekFraction(float Fraction);

	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	float GetFraction() const;

private:
	void PushStateToBillboard();
};
