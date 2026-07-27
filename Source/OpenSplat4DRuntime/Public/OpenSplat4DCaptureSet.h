#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/Texture2D.h"
#include "OpenSplat4DCaptureSet.generated.h"

/**
 * A native Unreal Engine asset that bundles one capture session (a set of color
 * images, masks, depths and a cameras.txt) into a single re-usable, draggable
 * object living in the Content Browser under /Game/OpenSplat4D/Captures.
 *
 * Unlike a loose JSON index, this is a first-class UE asset: it appears in the
 * Content Browser, can be dragged into the OpenSplat4D panel picker (the
 * "捕获组" box uses SObjectPropertyEntryBox, just like dropping a material onto
 * a slot), referenced from Blueprints, and survives an editor restart because
 * it is a real .uasset. The downstream sparse-reconstruction / training steps
 * read WorkDirectory + Images straight off the asset.
 *
 * ALL file paths beneath the working directory (images/, masks/, depths/,
 * cameras.txt, sparse/, output/) are DERIVED from WorkDirectory via the
 * accessor functions below.  The asset no longer stores individual image
 * paths or per-subfolder strings — that was redundant and caused the index
 * to become stale the moment any intermediate directory was cleaned.
 *
 * The final trained gaussian result is a *separate* asset (UOpenSplat4DPointCloud)
 * that can itself be dropped into the level (UActorFactory_OpenSplat4DPointCloud).
 * This data asset is where dataset / on-disk locations are recorded -- the point
 * cloud asset itself stays a pure preview object and does NOT store them.
 */
UCLASS(BlueprintType, Blueprintable, meta = (DisplayName = "数据资产（OpenSplat4D）"))
class OPENSPLAT4DRUNTIME_API UOpenSplat4DCaptureSet : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Human readable name of this capture group. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "OpenSplat4D")
	FString SetName;

	/** Absolute working directory — the single source of truth.  Every
	 *  sub-path (images, masks, depths, cameras.txt, sparse, output) is
	 *  derived from this root, so moving / renaming the folder keeps the
	 *  asset valid as long as the internal layout stays the same. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "OpenSplat4D")
	FString WorkDirectory;

	/** How many colour images this set captured. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "OpenSplat4D")
	int32 ImageCount = 0;

	// ---- Derived paths (read-only, computed from WorkDirectory) ----------
	/** Colour-image capture directory: <WorkDirectory>/images/ */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "OpenSplat4D|Derived")
	FString GetImagesDir() const { return WorkDirectory / TEXT("images"); }

	/** Mask directory: <WorkDirectory>/masks/ */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "OpenSplat4D|Derived")
	FString GetMasksDir()  const { return WorkDirectory / TEXT("masks"); }

	/** Depth directory: <WorkDirectory>/depths/ */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "OpenSplat4D|Derived")
	FString GetDepthsDir() const { return WorkDirectory / TEXT("depths"); }

	/** Camera file: <WorkDirectory>/cameras.txt */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "OpenSplat4D|Derived")
	FString GetCamerasFile() const { return WorkDirectory / TEXT("cameras.txt"); }

	/** Sparse-reconstruction output: <WorkDirectory>/sparse/ */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "OpenSplat4D|Derived")
	FString GetInitialModelDir() const { return WorkDirectory / TEXT("sparse"); }

	/** Training output root: <WorkDirectory>/output/ */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "OpenSplat4D|Derived")
	FString GetTrainedModelDir() const { return WorkDirectory / TEXT("output"); }

	// ---- dataset locations (for editor UI, auto-populated) ---------------
	/** Source colour-image set directory.  Mirrors GetImagesDir(). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "OpenSplat4D|数据集位置", DisplayName = "图片集目录")
	FDirectoryPath ImagesDir;

	/** Sparse-reconstruction output directory.  Mirrors GetInitialModelDir(). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "OpenSplat4D|数据集位置", DisplayName = "初次模型目录（稀疏重建）")
	FDirectoryPath InitialModelDir;

	/** Trained gaussian-model output directory.  Mirrors GetTrainedModelDir(). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "OpenSplat4D|数据集位置", DisplayName = "训练结果目录")
	FDirectoryPath TrainedModelDir;

	/** Optional preview thumbnail shown in the Content Browser. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "OpenSplat4D")
	TSoftObjectPtr<UTexture2D> Preview;

	/** When this capture set was last written. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "OpenSplat4D")
	FDateTime CapturedAt;
};
