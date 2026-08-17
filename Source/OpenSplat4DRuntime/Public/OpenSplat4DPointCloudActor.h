#pragma once

#include "CoreMinimal.h"
#include "NanoGSGaussianSplatActor.h"
#include "OpenSplat4DPointCloudActor.generated.h"

class UGaussianSplatAsset;
class UOpenSplat4DPointCloud;

/**
 * OpenSplat4D 点云渲染 Actor。
 *
 * 继承自 NanoGS AGaussianSplatActor 的 Compute Shader 渲染管线，
 * 并添加 4DGS 时间轴播放控制。兼容旧 SetPointCloud API。
 */
UCLASS()
class OPENSPLAT4DRUNTIME_API AOpenSplat4DPointCloudActor : public AGaussianSplatActor
{
	GENERATED_BODY()

public:
	AOpenSplat4DPointCloudActor(const FObjectInitializer& ObjectInitializer);

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** Backward-compat: sets the point cloud asset for rendering. */
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	void SetPointCloud(UOpenSplat4DPointCloud* InCloud);

	/** Get the currently assigned point cloud. */
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	UGaussianSplatAsset* GetPointCloud() const;

	// ---- 4D 时间轴播放 ------------------------------------------------
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D|Playback")
	void Play();

	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D|Playback")
	void Pause();

	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D|Playback")
	void Seek(float Time);

	// ---- 播放属性 -----------------------------------------------------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D|Playback")
	bool bAutoPlay = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D|Playback")
	bool bLooping = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D|Playback", meta = (ClampMin = "0.0"))
	float PlayRate = 1.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "OpenSplat4D|Playback")
	float CurrentTime = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "OpenSplat4D|Playback")
	bool bIsPlaying = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D", meta = (ClampMin = "0.01", ClampMax = "10.0"))
	float SplatScale = 1.f;

	/** The point cloud asset (backward compat). */
	UPROPERTY()
	TObjectPtr<UOpenSplat4DPointCloud> PointCloud;
};
