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

	/** Absolute working directory holding images/ masks/ depths/ cameras.txt. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "OpenSplat4D")
	FString WorkDirectory;

	/** How many color images this set captured. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "OpenSplat4D")
	int32 ImageCount = 0;

	/** Absolute file paths of every captured color image. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "OpenSplat4D")
	TArray<FString> Images;

	/** Absolute path of the masks directory. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "OpenSplat4D")
	FString MasksDir;

	/** Absolute path of the depths directory. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "OpenSplat4D")
	FString DepthsDir;

	/** Absolute path of the cameras.txt file. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "OpenSplat4D")
	FString CamerasFile;

	// ---- dataset locations (this is the data asset's job, not the point cloud's) --
	/** Source colour-image set directory (used by sparse reconstruction / training). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D|数据集位置", DisplayName = "图片集目录")
	FDirectoryPath ImagesDir;

	/** Sparse-reconstruction (initial model) output directory. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D|数据集位置", DisplayName = "初次模型目录（稀疏重建）")
	FDirectoryPath InitialModelDir;

	/** Trained gaussian-model output directory. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OpenSplat4D|数据集位置", DisplayName = "训练结果目录")
	FDirectoryPath TrainedModelDir;

	/** Optional preview thumbnail shown in the Content Browser. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "OpenSplat4D")
	TSoftObjectPtr<UTexture2D> Preview;

	/** When this capture set was last written. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "OpenSplat4D")
	FDateTime CapturedAt;
};
