#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "OpenSplat4DEditorLibrary.generated.h"

class UOpenSplat4DPointCloud;
class UStaticMesh;
class UWorld;
class AActor;

/** How CreatePointCloudFromActors turns level geometry into a gaussian point cloud. */
UENUM(BlueprintType)
enum class EOpenSplat4DScanMode : uint8
{
	/** Sample the surface of selected static meshes directly (no rendering, reliable). */
	MeshSurface UMETA(DisplayName = "网格表面（可靠）"),
	/** Place a camera rig and back-project captured depth into world points. */
	CameraDepth UMETA(DisplayName = "相机深度扫描"),
};

/**
 * Editor blueprint library mirroring the capabilities of the reference plugin's
 * UGaussianSplattingEditorLibrary, but adapted to OpenSplat4D's self-contained
 * (Niagara-free) data model.
 *
 * Exposes: file loading, point/feature counts, recursive directory import,
 * spatial repartitioning, and static-mesh baking.
 */
UCLASS()
class UOpenSplat4DEditorLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	/** Load a .ply (3DGS) or .4dgs (3D/4D) file into a UOpenSplat4DPointCloud asset. */
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	static UOpenSplat4DPointCloud* LoadSplatFile(const FString& FileName, UObject* Outer, FName AssetName = NAME_None);

	/** Number of gaussians in the cloud. */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "OpenSplat4D")
	static int32 GetPointCount(const UOpenSplat4DPointCloud* Cloud);

	/** Number of gaussians ("features") in the cloud (alias of GetPointCount). */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "OpenSplat4D")
	static int32 GetFeatureCount(const UOpenSplat4DPointCloud* Cloud);

	/**
	 * Recursively import every .ply/.4dgs under SearchDir, save each as a
	 * UOpenSplat4DPointCloud asset under SaveContentDir (e.g. "/Game/OpenSplat4D")
	 * and spawn an AOpenSplat4DPointCloudActor for it. If SaveContentDir is
	 * empty the assets are kept transient (not saved to disk).
	 */
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D", meta = (WorldContext = "World"))
	static void ImportPointClouds(UWorld* World, const FString& SearchDir, const FString& SaveContentDir = TEXT(""));

	/**
	 * Spatially repartition every AOpenSplat4DPointCloudActor in the level into
	 * a regular grid of CellSize (world units) cells. Each non-empty cell becomes
	 * a new point-cloud asset (and actor) centred on the cell, splitting a large
	 * cloud into smaller tiles for streaming / LOD. If SaveContentDir is empty the
	 * new clouds are transient.
	 */
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D", meta = (WorldContext = "World"))
	static void RepartitionPointClouds(UWorld* World, const FString& PartitionBaseName = TEXT("Cell"),
		int32 CellSize = 51200, const FString& SaveContentDir = TEXT(""));

	/**
	 * Bake a point cloud into a static mesh: one oriented quad per gaussian with
	 * vertex colors (RGBA) plus packed data UVs (UV1 = scale.xy, UV2 = scale.z,
	 * UV3 = quat.xy, UV4 = quat.zw) so a custom material can reconstruct the
	 * gaussians. Useful as a Niagara-free proxy / for collision / export.
	 */
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	static UStaticMesh* CreateStaticMeshFromPointCloud(const UOpenSplat4DPointCloud* Cloud, UObject* Outer, FName AssetName = NAME_None);

	/**
	 * Scan level geometry into a UOpenSplat4DPointCloud — the self-contained,
	 * dependency-free way to create a point cloud from a scene or model (no colmap/python).
	 * - MeshSurface: samples the surface of the selected static meshes (world-space, vertex color).
	 * - CameraDepth: places a hemisphere camera rig and back-projects captured depth into world points.
	 * If SaveContentDir is empty the asset is kept transient (not saved to disk).
	 */
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D", meta = (WorldContext = "World"))
	static UOpenSplat4DPointCloud* CreatePointCloudFromActors(
		UWorld* World,
		const TArray<AActor*>& Actors,
		EOpenSplat4DScanMode ScanMode = EOpenSplat4DScanMode::MeshSurface,
		int32 Density = 8,
		float PointScale = 3.f,
		const FString& SaveContentDir = TEXT("/Game/OpenSplat4D"));
};
