#pragma once

#include "CoreMinimal.h"
#include "NiagaraActor.h"
#include "OpenSplat4DPointCloud.h"
#include "OpenSplat4DDataInterface.h"
#include "OpenSplat4DPointCloudActor.generated.h"

/**
 * Renders an OpenSplat4D point cloud (3DGS or 4DGS) in the level.
 *
 * Uses a Niagara System asset (NS_OpenSplat4D) with the
 * UNiagaraDataInterfaceOpenSplat4D data interface to feed point cloud
 * data to the GPU. All rendering is handled by Niagara's built-in
 * GPU Sprite Renderer — no custom shaders, SceneViewExtension,
 * or manual draw calls are needed.
 *
 * The actor pattern follows the reference GaussianSplattingForUnrealEngine
 * plugin: inherit from ANiagaraActor, wire the data interface through
 * UNiagaraFunctionLibrary::GetDataInterface<T>().
 */
UCLASS(CollapseCategories)
class OPENSPLAT4DRUNTIME_API AOpenSplat4DPointCloudActor : public ANiagaraActor
{
	GENERATED_BODY()
public:
	AOpenSplat4DPointCloudActor(const FObjectInitializer& ObjectInitializer);

	/** The point cloud asset rendered by this actor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D")
	TObjectPtr<UOpenSplat4DPointCloud> PointCloud;

	/** Whether playback starts automatically on BeginPlay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D|Playback")
	bool bAutoPlay = true;

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

	/** Runtime multiplier on every splat's size. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D", meta = (ClampMin = "0.01", ClampMax = "10.0"))
	float SplatScale = 1.f;

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	void SetPointCloud(UOpenSplat4DPointCloud* InCloud);

	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	void SetMode(EOpenSplat4DMode InMode);

	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	UOpenSplat4DPointCloud* GetPointCloud() const { return PointCloud; }

	// ---- Playback API ------------------------------------------------------

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

	// ---- [EDIT] Point editing API ------------------------------------------

	/** Get the number of points in the cloud. */
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D|Edit")
	int32 GetNumPoints() const { return PointCloud ? PointCloud->GetPointCount() : 0; }

	/** Get a single point's data (world-space position, quat, scale, color). Returns world-space. */
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D|Edit")
	bool GetPoint(int32 Index, FVector3f& OutPosition, FQuat4f& OutQuat, FVector3f& OutScale, FLinearColor& OutColor) const;

	/** Update a single point by index. Marks the GPU buffer dirty and refreshes. */
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D|Edit")
	bool UpdatePoint(int32 Index, FVector3f NewPosition, FQuat4f NewQuat, FVector3f NewScale, FLinearColor NewColor);

	/** Apply a world-space transform to a set of points. */
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D|Edit")
	void TransformPoints(const FTransform& Transform, const TArray<int32>& Indices);

	/** Remove points by their indices (will be sorted and deduplicated internally). */
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D|Edit")
	void RemovePoints(const TArray<int32>& IndicesToRemove);

	/** Set color of selected points. */
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D|Edit")
	void SetPointsColor(const TArray<int32>& Indices, FLinearColor NewColor);

	/** Scale selected points uniformly. */
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D|Edit")
	void ScalePoints(const TArray<int32>& Indices, float ScaleMultiplier);

	/** [OPT] Refresh GPU buffer after batch edits without full reinit. Call after batch editing. */
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D|Edit")
	void RefreshRenderState();

private:
	/** One-shot Niagara init: SetAsset, DI wiring, bounds, VariantDI override.
	 *  Follows teacher's SetupPointCloudToNiagaraComponent pattern — called once
	 *  on first use, never repeated per-frame. */
	void InitNiagaraSystem();

	/** Lightweight per-frame update: only updates DI->Time and SplatScale.
	 *  Does NOT reinitialize, set asset, or duplicate DIs. */
	void UpdateNiagaraState();

	/** Full push (init + update) for SetPointCloud / SetMode / Seek. */
	void PushStateToNiagara();

	bool bNiagaraInitialized = false;
};
