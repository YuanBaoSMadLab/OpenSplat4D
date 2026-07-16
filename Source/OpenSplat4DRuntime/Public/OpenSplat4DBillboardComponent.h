#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "SceneViewExtension.h"
#include "OpenSplat4DBillboardComponent.generated.h"

class UOpenSplat4DPointCloud;
class FSceneViewExtensionBase;

/**
 * Self-contained Gaussian-splat billboard renderer.
 *
 * Uploads the point cloud to a GPU structured buffer and draws one camera-facing
 * quad per gaussian through a custom global shader (OpenSplat4DBillboard.usf).
 * Unlike the Niagara data-interface path, this component needs no external
 * Niagara System asset -- drop an AOpenSplat4DPointCloudActor (or this component)
 * in the level, assign a UOpenSplat4DPointCloud, and it renders.
 */
UCLASS(ClassGroup = (Rendering), meta = (BlueprintSpawnableComponent))
class OPENSPLAT4DRUNTIME_API UOpenSplat4DBillboardComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UOpenSplat4DBillboardComponent();

	/** The point cloud asset rendered by this component. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D")
	TObjectPtr<UOpenSplat4DPointCloud> PointCloud;

	/** Global playback time, pushed by the owning actor each frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D")
	float Time = 0.f;

	/** When true, the GPU applies the 4DGS temporal marginal; otherwise weight == 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D")
	bool bTemporalWeighting = false;

	/** Runtime multiplier on every splat's size (world units). 1 = as-authored.
	 *  Lower values shrink the cloud (e.g. 0.1 turns a 30cm scan into a 3cm one),
	 *  higher values fatten it. Handy for tuning a scanned point cloud without
	 *  re-capturing, or fixing scans whose Point Size was set too large. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D", meta = (ClampMin = "0.01", ClampMax = "10.0"))
	float SplatScale = 1.f;

	/** Rebuild the GPU buffer from the current point cloud. Call after the cloud changes. */
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	void RebuildBuffer();

	virtual void OnRegister() override;
	virtual void OnUnregister() override;

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

private:
	/** Scene view extension that performs the actual splat draw inside the base pass. */
	TSharedPtr<FSceneViewExtensionBase> OpenSplatViewExtension;
};
