#include "OpenSplat4DEditorLibrary.h"

#include "OpenSplat4DPointCloud.h"
#include "OpenSplat4DPointCloudActor.h"

#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Kismet/GameplayStatics.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"

#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"

#include "StaticMeshResources.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Rendering/StaticMeshVertexBuffer.h"
#include "Rendering/ColorVertexBuffer.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "OpenSplat4DStep.h"

#define LOCTEXT_NAMESPACE "OpenSplat4DEditorLibrary"

namespace
{
	/** Save an asset object into its package on disk (UE5.8 SavePackage signature). */
	bool SaveCloudAsset(UPackage* Pkg, UObject* Asset)
	{
		if (!Pkg || !Asset)
		{
			return false;
		}
		const FString Filename = FPackageName::LongPackageNameToFilename(
			Pkg->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		Args.bWarnOfLongFilename = false;
		return UPackage::SavePackage(Pkg, Asset, *Filename, Args);
	}

	/** Create (or reuse) a package under a /Game/... path and a fresh point cloud inside it. */
	UOpenSplat4DPointCloud* CreateCloudAsset(const FString& ContentDir, const FString& AssetName,
		const TArray<FOpenSplat4DPoint>& Points, const FString& SourceFile, bool bSave)
	{
		UOpenSplat4DPointCloud* Cloud = nullptr;
		if (bSave && ContentDir.StartsWith(TEXT("/Game")))
		{
			const FString FullName = ContentDir / AssetName;
			UPackage* Pkg = CreatePackage(*FullName);
			Pkg->FullyLoad();
			Cloud = NewObject<UOpenSplat4DPointCloud>(Pkg, UOpenSplat4DPointCloud::StaticClass(),
				FName(*AssetName), RF_Public | RF_Standalone);
			Cloud->SetPoints(Points, /*bReorder=*/false);
			Cloud->SourceFilePath = SourceFile;
			Cloud->MarkPackageDirty();
			SaveCloudAsset(Pkg, Cloud);
		}
		else
		{
			Cloud = NewObject<UOpenSplat4DPointCloud>(GetTransientPackage(),
				UOpenSplat4DPointCloud::StaticClass(), FName(*AssetName), RF_Transient);
			Cloud->SetPoints(Points, /*bReorder=*/false);
			Cloud->SourceFilePath = SourceFile;
		}
		return Cloud;
	}
}

UOpenSplat4DPointCloud* UOpenSplat4DEditorLibrary::LoadSplatFile(const FString& FileName, UObject* Outer, FName AssetName)
{
	if (FileName.IsEmpty())
	{
		return nullptr;
	}
	UObject* UseOuter = Outer ? Outer : static_cast<UObject*>(GetTransientPackage());
	const FString Name = AssetName.IsNone() ? FPaths::GetBaseFilename(FileName) : AssetName.ToString();

	UOpenSplat4DPointCloud* Cloud = NewObject<UOpenSplat4DPointCloud>(UseOuter,
		UOpenSplat4DPointCloud::StaticClass(), FName(*Name), RF_Public | RF_Standalone);
	if (!Cloud)
	{
		return nullptr;
	}

	const FString Ext = FPaths::GetExtension(FileName).ToLower();
	bool bOk = false;
	if (Ext == TEXT("ply"))
	{
		Cloud->LoadFromFile(FileName);
		bOk = Cloud->GetPointCount() > 0;
		if (!bOk) { UE_LOG(LogOpenSplat4DStep, Warning, TEXT("OpenSplat4D: PLY 加载后点数为 0：%s"), *FileName); }
	}
	else if (Ext == TEXT("4dgs"))
	{
		bOk = Cloud->LoadFrom4DGS(FileName);
	}

	if (!bOk)
	{
		return nullptr;
	}
	Cloud->SourceFilePath = FileName;
	return Cloud;
}

int32 UOpenSplat4DEditorLibrary::GetPointCount(const UOpenSplat4DPointCloud* Cloud)
{
	return Cloud ? Cloud->GetPointCount() : 0;
}

int32 UOpenSplat4DEditorLibrary::GetFeatureCount(const UOpenSplat4DPointCloud* Cloud)
{
	return Cloud ? Cloud->GetPointCount() : 0;
}

void UOpenSplat4DEditorLibrary::ImportPointClouds(UWorld* World, const FString& SearchDir, const FString& SaveContentDir)
{
	if (!World || SearchDir.IsEmpty())
	{
		return;
	}

	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *SearchDir, TEXT("*.*"), /*Files=*/true, /*Directories=*/false);

	for (const FString& File : Files)
	{
		const FString Ext = FPaths::GetExtension(File).ToLower();
		if (Ext != TEXT("ply") && Ext != TEXT("4dgs"))
		{
			continue;
		}

		UOpenSplat4DPointCloud* Cloud = LoadSplatFile(File, GetTransientPackage());
		if (!Cloud)
		{
			continue;
		}

		const bool bSave = !SaveContentDir.IsEmpty();
		const FString AssetName = FPaths::GetBaseFilename(File);
		UOpenSplat4DPointCloud* Saved = CreateCloudAsset(SaveContentDir, AssetName,
			Cloud->GetPoints(), File, bSave);
		if (!Saved)
		{
			continue;
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transient;
		if (AOpenSplat4DPointCloudActor* Actor = World->SpawnActor<AOpenSplat4DPointCloudActor>(SpawnParams))
		{
			Actor->SetPointCloud(Saved);
		}
	}
}

void UOpenSplat4DEditorLibrary::RepartitionPointClouds(UWorld* World, const FString& PartitionBaseName,
	int32 CellSize, const FString& SaveContentDir)
{
	if (!World || CellSize <= 0)
	{
		return;
	}

	// Gather actors and their world-space 2D (X-Y) bounds.
	struct FActorEntry { AOpenSplat4DPointCloudActor* Actor; FBox2D Bound; };
	TArray<FActorEntry> Entries;
	FBox2D TotalBound(ForceInit);
	TArray<AActor*> ActorList;
	UGameplayStatics::GetAllActorsOfClass(World, AOpenSplat4DPointCloudActor::StaticClass(), ActorList);
	for (AActor* A : ActorList)
	{
		AOpenSplat4DPointCloudActor* Actor = Cast<AOpenSplat4DPointCloudActor>(A);
		if (!Actor || !Actor->PointCloud || Actor->PointCloud->GetPointCount() <= 0)
		{
			continue;
		}
		const FVector Loc = Actor->GetActorLocation();
		const FBox LocalBox = Actor->PointCloud->CalcBounds();
		const FVector2D Min(Loc.X + LocalBox.Min.X, Loc.Y + LocalBox.Min.Y);
		const FVector2D Max(Loc.X + LocalBox.Max.X, Loc.Y + LocalBox.Max.Y);
		Entries.Add({ Actor, FBox2D(Min, Max) });
		TotalBound += Entries.Last().Bound;
	}
	if (Entries.Num() == 0)
	{
		return;
	}

	// Bucket every point into a grid cell (world X-Y).
	TMap<FIntPoint, TArray<FOpenSplat4DPoint>> Partitions;
	for (const FActorEntry& Entry : Entries)
	{
		const FVector Loc = Entry.Actor->GetActorLocation();
		const TArray<FOpenSplat4DPoint>& Points = Entry.Actor->PointCloud->GetPoints();
		for (const FOpenSplat4DPoint& P : Points)
		{
			const double WX = Loc.X + P.Position.X;
			const double WY = Loc.Y + P.Position.Y;
			const int32 CellX = FMath::FloorToInt((WX - TotalBound.Min.X) / CellSize);
			const int32 CellY = FMath::FloorToInt((WY - TotalBound.Min.Y) / CellSize);
			const FIntPoint Cell(CellX, CellY);

			FOpenSplat4DPoint Local = P;
			const double CellCenterX = TotalBound.Min.X + (CellX + 0.5) * CellSize;
			const double CellCenterY = TotalBound.Min.Y + (CellY + 0.5) * CellSize;
			Local.Position = FVector3f(
				static_cast<float>(WX - CellCenterX),
				static_cast<float>(WY - CellCenterY),
				P.Position.Z);
			Partitions.FindOrAdd(Cell).Add(Local);
		}
	}

	// Emit one cloud + actor per non-empty cell.
	int32 CellIndex = 0;
	for (auto& Pair : Partitions)
	{
		if (Pair.Value.Num() == 0)
		{
			continue;
		}
		const FIntPoint Cell = Pair.Key;
		const FString AssetName = FString::Printf(TEXT("%s_%d_%d"), *PartitionBaseName, Cell.X, Cell.Y);
		const double CellCenterX = TotalBound.Min.X + (Cell.X + 0.5) * CellSize;
		const double CellCenterY = TotalBound.Min.Y + (Cell.Y + 0.5) * CellSize;

		const bool bSave = !SaveContentDir.IsEmpty();
		UOpenSplat4DPointCloud* Cloud = CreateCloudAsset(SaveContentDir, AssetName, Pair.Value, TEXT(""), bSave);
		if (!Cloud)
		{
			continue;
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transient;
		if (AOpenSplat4DPointCloudActor* Actor = World->SpawnActor<AOpenSplat4DPointCloudActor>(SpawnParams))
		{
			Actor->SetPointCloud(Cloud);
			Actor->SetActorLocation(FVector(CellCenterX, CellCenterY, 0.0));
		}
		CellIndex++;
	}
	UE_LOG(LogTemp, Log, TEXT("OpenSplat4D: repartitioned level into %d cells."), CellIndex);
}

UStaticMesh* UOpenSplat4DEditorLibrary::CreateStaticMeshFromPointCloud(const UOpenSplat4DPointCloud* Cloud,
	UObject* Outer, FName AssetName)
{
	if (!Cloud || Cloud->GetPointCount() == 0)
	{
		return nullptr;
	}
	UObject* UseOuter = Outer ? Outer : static_cast<UObject*>(GetTransientPackage());
	const FString Name = AssetName.IsNone() ? TEXT("SM_OpenSplat4D") : AssetName.ToString();

	UStaticMesh* Mesh = NewObject<UStaticMesh>(UseOuter, UStaticMesh::StaticClass(), FName(*Name), RF_Public | RF_Standalone);
	if (!Mesh)
	{
		return nullptr;
	}

	FMeshDescription* MeshDesc = Mesh->CreateMeshDescription(0);
	FStaticMeshAttributes Attributes(*MeshDesc);
	Attributes.Register();

	TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	UVs.SetNumChannels(4);

	const FPolygonGroupID PolygonGroup = MeshDesc->CreatePolygonGroup();

	const TArray<FOpenSplat4DPoint>& Points = Cloud->GetPoints();
	const int32 NumPoints = Points.Num();

	// Pre-compute per-point vertex indices so we can build UV/data once.
	TArray<FVertexID> CornerVerts[4];
	for (int32 c = 0; c < 4; c++)
	{
		CornerVerts[c].SetNumUninitialized(NumPoints);
	}

	TArray<FVertexInstanceID> CornerInstances[4];
	for (int32 c = 0; c < 4; c++)
	{
		CornerInstances[c].SetNumUninitialized(NumPoints);
	}

	for (int32 i = 0; i < NumPoints; i++)
	{
		const FOpenSplat4DPoint& P = Points[i];
		const FQuat4f Quat = P.Quat;
		const FVector3f AxisX = Quat.RotateVector(FVector3f::XAxisVector) * P.Scale.X;
		const FVector3f AxisY = Quat.RotateVector(FVector3f::YAxisVector) * P.Scale.Y;

		// Four quad corners (local), wound CCW for the +Z facing.
		const FVector3f Corners[4] =
		{
			P.Position - AxisX - AxisY,
			P.Position + AxisX - AxisY,
			P.Position + AxisX + AxisY,
			P.Position - AxisX + AxisY,
		};

		for (int32 c = 0; c < 4; c++)
		{
			const FVertexID V = MeshDesc->CreateVertex();
			CornerVerts[c][i] = V;
			Positions[V] = Corners[c];

			const FVertexInstanceID VI = MeshDesc->CreateVertexInstance(V);
			CornerInstances[c][i] = VI;
			Colors[VI] = FVector4f(P.Color.R, P.Color.G, P.Color.B, P.Color.A);
			Normals[VI] = Quat.RotateVector(FVector3f::ZAxisVector);
			Tangents[VI] = AxisX.GetSafeNormal();
			BinormalSigns[VI] = 1.0f;
			// Packed gaussian data for a custom reconstruct material.
			UVs[VI] = FVector2f(P.Scale.X, P.Scale.Y);
		}
	}

	// Build two triangles per point.
	for (int32 i = 0; i < NumPoints; i++)
	{
		const FVertexInstanceID Tris[6] =
		{
			CornerInstances[0][i], CornerInstances[1][i], CornerInstances[2][i],
			CornerInstances[0][i], CornerInstances[2][i], CornerInstances[3][i],
		};
		MeshDesc->CreatePolygon(PolygonGroup, MakeArrayView(Tris, 6));
	}

	// Packed params need more channels: UV1/UV2 = scale (xyz), UV3/UV4 = quat (xyzw).
	// The base UVs above hold scale.xy; overwrite the full set per channel.
	for (int32 i = 0; i < NumPoints; i++)
	{
		const FOpenSplat4DPoint& P = Points[i];
		for (int32 c = 0; c < 4; c++)
		{
			const FVertexInstanceID VI = CornerInstances[c][i];
			UVs.Set( VI, 0, FVector2f(P.Scale.X, P.Scale.Y) );
			UVs.Set( VI, 1, FVector2f(P.Scale.Z, P.bUseVelocity ? 1.f : 0.f) );
			UVs.Set( VI, 2, FVector2f(P.Quat.X, P.Quat.Y) );
			UVs.Set( VI, 3, FVector2f(P.Quat.Z, P.Quat.W) );
		}
	}

	Mesh->CommitMeshDescription(0);

	// Disable automatic normal/tangent/lightmap work so our baked data survives.
	FMeshBuildSettings& BuildSettings = Mesh->GetSourceModel(0).BuildSettings;
	BuildSettings.bRecomputeNormals = false;
	BuildSettings.bRecomputeTangents = false;
	BuildSettings.bUseMikkTSpace = false;
	BuildSettings.bGenerateLightmapUVs = false;
	BuildSettings.bRemoveDegenerates = false;

	Mesh->Build();
	Mesh->MarkPackageDirty();
	return Mesh;
}

// ----------------------------------------------------------------------------
// Scan level geometry into a gaussian point cloud (self-contained, no colmap/python)
// ----------------------------------------------------------------------------
namespace
{
	/** Area-weighted surface sampling of a static mesh into gaussian points. */
	void SampleStaticMeshComponent(UStaticMeshComponent* Comp, int32 Density, float PointScale, TArray<FOpenSplat4DPoint>& Out)
	{
		UStaticMesh* SM = Comp->GetStaticMesh();
		if (!SM || !SM->GetRenderData() || SM->GetRenderData()->LODResources.Num() == 0)
		{
			return;
		}
		const FStaticMeshLODResources& LOD = SM->GetRenderData()->LODResources[0];
		const FPositionVertexBuffer& PosBuf = LOD.VertexBuffers.PositionVertexBuffer;
		const FStaticMeshVertexBuffer& VtxBuf = LOD.VertexBuffers.StaticMeshVertexBuffer;
		const FColorVertexBuffer& ColBuf = LOD.VertexBuffers.ColorVertexBuffer;
		const FRawStaticIndexBuffer& IdxBuf = LOD.IndexBuffer;
		const int32 NumTri = IdxBuf.GetNumIndices() / 3;
		const FTransform LocalToWorld = Comp->GetComponentToWorld();
		const bool bHasColor = ColBuf.GetNumVertices() > 0;
		const float InvDensity = 1.f / FMath::Max(1, Density);

		for (int32 t = 0; t < NumTri; t++)
		{
			const uint32 i0 = IdxBuf.GetIndex(3 * t + 0);
			const uint32 i1 = IdxBuf.GetIndex(3 * t + 1);
			const uint32 i2 = IdxBuf.GetIndex(3 * t + 2);
			const FVector3f ln0 = VtxBuf.VertexTangentZ(i0);
			const FVector3f ln1 = VtxBuf.VertexTangentZ(i1);
			const FVector3f ln2 = VtxBuf.VertexTangentZ(i2);
			const FVector wp0 = LocalToWorld.TransformPosition(FVector(PosBuf.VertexPosition(i0)));
			const FVector wp1 = LocalToWorld.TransformPosition(FVector(PosBuf.VertexPosition(i1)));
			const FVector wp2 = LocalToWorld.TransformPosition(FVector(PosBuf.VertexPosition(i2)));
			const double Area = FVector::CrossProduct(wp1 - wp0, wp2 - wp0).Size() * 0.5;
			const int32 NumSamples = FMath::Max(1, (int32)(Area * InvDensity * 0.5));
			for (int32 s = 0; s < NumSamples; s++)
			{
				float u = FMath::FRand();
				float v = FMath::FRand();
				if (u + v > 1.f) { u = 1.f - u; v = 1.f - v; }
				const float w = 1.f - u - v;
				const FVector Pos = wp0 * w + wp1 * u + wp2 * v;
				const FVector3f Nrm = (ln0 * w + ln1 * u + ln2 * v).GetSafeNormal();
				FLinearColor Col = FLinearColor(0.8f, 0.8f, 0.8f, 1.f);
				if (bHasColor)
				{
					const FColor c0 = ColBuf.VertexColor(i0);
					const FColor c1 = ColBuf.VertexColor(i1);
					const FColor c2 = ColBuf.VertexColor(i2);
					const FColor C = FColor(
						(uint8)(c0.R * w + c1.R * u + c2.R * v),
						(uint8)(c0.G * w + c1.G * u + c2.G * v),
						(uint8)(c0.B * w + c1.B * u + c2.B * v), 255);
					Col = FLinearColor(C);
				}
				// Orient the gaussian so its local Z axis aligns with the surface normal (surfel-like).
				const FQuat4f Quat = FQuat4f(FQuat::FindBetweenNormals(FVector::ZAxisVector, FVector(Nrm)));
				Out.Add(FOpenSplat4DPoint(FVector3f(Pos), Quat, FVector3f(PointScale), Col));
			}
		}
	}

	/** Place a hemisphere camera rig and back-project captured (linear) scene depth into world points. */
	void ScanComponentsByCamera(UWorld* World, const TArray<UPrimitiveComponent*>& Comps, int32 Views, float PointScale, TArray<FOpenSplat4DPoint>& Out)
	{
		if (!World || Comps.Num() == 0)
		{
			return;
		}
		FBoxSphereBounds Bounds; Bounds.SphereRadius = 0;
		for (UPrimitiveComponent* C : Comps) { Bounds = (Bounds.SphereRadius <= 0) ? C->Bounds : (Bounds + C->Bounds); }
		if (Bounds.SphereRadius <= 0)
		{
			return;
		}

		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>();
		RT->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA16f;
		RT->InitAutoFormat(512, 512);
		RT->UpdateResourceImmediate(true);

		ASceneCapture2D* Cap = World->SpawnActor<ASceneCapture2D>();
		Cap->SetFlags(RF_Transient);
		USceneCaptureComponent2D* SCC = Cap->GetCaptureComponent2D();
		SCC->TextureTarget = RT;
		SCC->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		SCC->ShowOnlyComponents.Empty();
		for (UPrimitiveComponent* C : Comps) SCC->ShowOnlyComponents.Add(C);
		SCC->bCaptureEveryFrame = false;
		SCC->bAlwaysPersistRenderingState = true;
		const float Far = Bounds.SphereRadius * 4.f + 1000.f;

		// Build view directions on an upper hemisphere (Z up).
		TArray<FVector> Dirs;
		const int32 Rows = FMath::Max(1, FMath::RoundToInt(FMath::Sqrt((float)Views)));
		const int32 Cols = FMath::Max(1, FMath::RoundToInt((float)Views / (float)Rows));
		for (int32 a = 0; a < Rows; a++)
		{
			const float Elev = (Rows == 1) ? PI * 0.25f : (PI * 0.5f) * ((float)a / (float)(Rows - 1));
			for (int32 b = 0; b < Cols; b++)
			{
				const float Azim = (Cols == 1) ? 0.f : 2.f * PI * ((float)b / (float)(Cols - 1));
				Dirs.Add(FVector(FMath::Sin(Elev) * FMath::Cos(Azim), FMath::Sin(Elev) * FMath::Sin(Azim), FMath::Cos(Elev)));
			}
		}

		const FIntPoint Size(RT->SizeX, RT->SizeY);
		const FIntRect Region(0, 0, Size.X, Size.Y);
		const FReadSurfaceDataFlags Flags(RCM_MinMax);
		const float Dist = Bounds.SphereRadius * 2.2f + 100.f;
		// Build the perspective projection ourselves (USceneCaptureComponent2D has no GetProjectionMatrix in 5.8).
		const float HalfFOV = FMath::DegreesToRadians(SCC->FOVAngle) * 0.5f;
		const FMatrix InvProj = FPerspectiveMatrix(HalfFOV, HalfFOV, 1.f, 1.f, 1.f, Far).Inverse();

		for (const FVector& Dir : Dirs)
		{
			const FVector Pos = Bounds.Origin + Dir * Dist;
			Cap->SetActorLocationAndRotation(Pos, FRotationMatrix::MakeFromX(-Dir).Rotator());
			FakeEngineTick(World);

			SCC->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
			SCC->CaptureScene();
			TArray<FLinearColor> Depth;
			RT->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Depth, Flags, Region);

			SCC->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
			SCC->CaptureScene();
			TArray<FLinearColor> Color;
			RT->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Color, Flags, Region);

			const FTransform CamT = SCC->GetComponentToWorld();
			const FVector Eye = CamT.GetLocation();

			for (int32 y = 0; y < Size.Y; y++)
			{
				for (int32 x = 0; x < Size.X; x++)
				{
					const int32 Px = y * Size.X + x;
					const float d = Depth[Px].R;          // linear eye-space depth (world units)
					if (d <= 0.f || d >= Far * 0.999f)
					{
						continue;
					}
					const float ndcx = 2.f * ((x + 0.5f) / (float)Size.X) - 1.f;
					const float ndcy = 1.f - 2.f * ((y + 0.5f) / (float)Size.Y);
					const FVector4 Clip(ndcx, ndcy, 1.f, 1.f);
					const FVector4 V = InvProj.TransformFVector4(Clip);
					FVector DirView = FVector(V.X, V.Y, V.Z) / V.W;
					DirView.Normalize();
					const FVector WorldDir = CamT.TransformVector(DirView);
					const FVector WorldPos = Eye + WorldDir * d;
					FLinearColor C = Color[Px];
					C.A = 1.f;
					Out.Add(FOpenSplat4DPoint(FVector3f(WorldPos), FQuat4f::Identity, FVector3f(PointScale), C));
				}
			}
		}
		Cap->Destroy();
	}
}

UOpenSplat4DPointCloud* UOpenSplat4DEditorLibrary::CreatePointCloudFromActors(
	UWorld* World, const TArray<AActor*>& Actors, EOpenSplat4DScanMode ScanMode, int32 Density, float PointScale, const FString& SaveContentDir)
{
	TArray<AActor*> TargetActors = Actors;
	if (TargetActors.Num() == 0 && World)
	{
		UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), TargetActors);
	}

	TArray<UPrimitiveComponent*> Comps;
	for (AActor* A : TargetActors)
	{
		if (!A) continue;
		for (UActorComponent* C : A->GetComponents())
		{
			if (UPrimitiveComponent* PC = Cast<UPrimitiveComponent>(C))
			{
				Comps.Add(PC);
			}
		}
	}

	TArray<FOpenSplat4DPoint> Points;
	if (ScanMode == EOpenSplat4DScanMode::MeshSurface)
	{
		for (UPrimitiveComponent* PC : Comps)
		{
			if (UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(PC))
			{
				SampleStaticMeshComponent(SMC, Density, PointScale, Points);
			}
		}
	}
	else
	{
		ScanComponentsByCamera(World, Comps, FMath::Clamp(Density * Density, 16, 256), PointScale, Points);
	}

	if (Points.Num() == 0)
	{
		return nullptr;
	}

	FString BaseName = TEXT("Scan");
	if (TargetActors.Num() > 0 && TargetActors[0])
	{
		BaseName = FString::Printf(TEXT("Scan_%s"), *TargetActors[0]->GetActorLabel());
	}
	const bool bSave = !SaveContentDir.IsEmpty();
	UPackage* Pkg = bSave ? CreatePackage(*FString::Printf(TEXT("%s/%s"), *SaveContentDir, *BaseName)) : GetTransientPackage();
	UOpenSplat4DPointCloud* Cloud = NewObject<UOpenSplat4DPointCloud>(
		Pkg, UOpenSplat4DPointCloud::StaticClass(), *BaseName, bSave ? (RF_Public | RF_Standalone) : RF_Transient);

	Cloud->SetPoints(Points, /*bReorder=*/false);
	Cloud->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Cloud);

	if (bSave)
	{
		FPackagePath PkgPath = FPackagePath::FromPackageNameChecked(Pkg->GetName());
		const FString LocalPath = PkgPath.GetLocalFullPath();
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		SaveArgs.bWarnOfLongFilename = false;
		UPackage::SavePackage(Pkg, Cloud, *LocalPath, SaveArgs);
	}
	return Cloud;
}

#undef LOCTEXT_NAMESPACE
