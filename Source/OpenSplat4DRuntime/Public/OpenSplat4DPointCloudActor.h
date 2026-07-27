#pragma once

#include "CoreMinimal.h"
#include "GaussianSplatActor.h"
#include "OpenSplat4DPointCloudActor.generated.h"

/**
 * OpenSplat4D 点云渲染 Actor。
 *
 * 继承自 NanoGS AGaussianSplatActor 的 Compute Shader 渲染管线，
 * 并添加 4DGS 时间轴播放控制（Play/Pause/Seek/CurrentTime/PlayRate/bLooping）。
 * 3DGS 资产忽略时间字段即可正常工作。
 */
UCLASS()
class OPENSPLAT4DRUNTIME_API AOpenSplat4DPointCloudActor : public AGaussianSplatActor
{
	GENERATED_BODY()

public:
	AOpenSplat4DPointCloudActor(const FObjectInitializer& ObjectInitializer);

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	// ---- 4D 时间轴播放 ------------------------------------------------
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D|Playback")
	void Play();

	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D|Playback")
	void Pause();

	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D|Playback")
	void Seek(float Time);

	// ---- 播放属性 -----------------------------------------------------
	/** 是否自 BeginPlay 时自动开始播放。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D|Playback")
	bool bAutoPlay = true;

	/** 时间轴是否循环。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D|Playback")
	bool bLooping = true;

	/** 播放速率倍乘。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D|Playback", meta = (ClampMin = "0.0"))
	float PlayRate = 1.0f;

	/** 当前播放时间（只读）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "OpenSplat4D|Playback")
	float CurrentTime = 0.f;

	/** 是否正在播放（只读）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "OpenSplat4D|Playback")
	bool bIsPlaying = false;

	// ---- Splat 缩放 ---------------------------------------------------
	/** Splat 尺寸缩放因子。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D", meta = (ClampMin = "0.01", ClampMax = "10.0"))
	float SplatScale = 1.f;
};
