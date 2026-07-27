#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/RunnableThread.h"
#include "Components/SceneCaptureComponent.h"
#include "Engine/TriggerSphere.h"
#include "Engine/SkyLight.h"
#include "Engine/Scene.h"
#include "OpenSplat4DCaptureSet.h"
#include "OpenSplat4DEditorLibrary.h"
#include "OpenSplat4DTypes.h"
#include "GaussianSplatAsset.h"
#include "GaussianSplatActor.h"
#include "GaussianSplatComponent.h"
#include "PLYFileReader.h"
#include "OpenSplat4DStep.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOpenSplat4DStep, Log, All);

DECLARE_DELEGATE_RetVal(bool, FOnRequestTaskStart)

// Internal helpers shared across the step implementation files.
void FakeEngineTick(UWorld* InWorld, float InDelta = 0.03f, int InCount = 1);
FLinearColor LinearToSRGB(const FLinearColor& Color);
FVector UVtoPyramid(FVector2D UV);
FVector UVtoOctahedron(FVector2D UV);

/** Wrap a command-line token in double quotes if it contains spaces.
 *  Windows splits unquoted paths (e.g. "C:/Program Files/.../script.py" or a
 *  project dir under "Unreal Projects") on the space, so python receives
 *  "C:/Program" and fails with "can't open file". Quoting keeps the whole
 *  path as one argument. Harmless for space-free tokens. */
FString OpenSplat4DQuoteArg(const FString& Arg);

/** Remove a single pair of surrounding double quotes (inverse of OpenSplat4DQuoteArg).
 *  The executable path handed to FPlatformProcess::CreateProc must not be quoted. */
FString OpenSplat4DUnquoteArg(const FString& Arg);

/** Returns true if the given executable path can actually be launched: either an
 *  existing file on disk, or a bare command name resolvable on the system PATH.
 *  Used for a clear pre-flight dependency check (python / colmap) before the
 *  plugin shells out, so the user gets an actionable UE-side error instead of an
 *  obscure "executable not found" from the python helper. */
bool OpenSplat4DIsExecutableResolvable(const FString& Path);

// ----------------------------------------------------------------------------
// Capture-set asset (native UE DataAsset in the project's Content folder).
//
// Each capture session is recorded as a UOpenSplat4DCaptureSet asset saved to
//   Content/OpenSplat4D/Captures/<SetName>.uasset
// pointing at every captured image (absolute paths) plus masks / depths /
// cameras.txt. The asset is a first-class Content Browser asset: it can be
// dragged into Blueprints or the panel picker like a material, and it survives
// an editor restart (no manual JSON parsing). The sparse / training steps read
// from whichever set is currently selected in the panel.
// ----------------------------------------------------------------------------
class UOpenSplat4DCaptureSet;

/** Create (or reuse) a capture-set asset in /Game/OpenSplat4D/Captures/<Name>
 *  and save it to disk. Returns the asset (nullptr on failure). */
UOpenSplat4DCaptureSet* OpenSplat4DCreateCaptureSet(const FString& SetName, const FString& WorkDir);

/** Update an existing capture-set asset with the captured results, then save it.
 *  All sub-paths (images/, masks/, depths/, cameras.txt) are derived from
 *  WorkDirectory — the asset no longer stores individual image paths. */
void OpenSplat4DUpdateCaptureSet(UOpenSplat4DCaptureSet* Asset, const FString& WorkDir, int32 ImageCount);

/** Enumerate every existing capture-set asset under /Game/OpenSplat4D/Captures. */
void OpenSplat4DEnumerateCaptureSets(TArray<UOpenSplat4DCaptureSet*>& OutSets);

/** Build a human-friendly capture-set name from the actor(s) currently selected
 *  in the editor: "&lt;MeshOrActorLabel&gt;_&lt;NN&gt;_&lt;YYYYMMDD_HHMMSS&gt;". The subject label
 *  ties the capture to what was scanned; the zero-padded sequence number and the
 *  timestamp keep repeated captures of the same subject from colliding. */
FString OpenSplat4DBuildCaptureSetName();

UENUM()
enum class EOpenSplat4DCaptureSourceMode : uint8
{
	Select UMETA(DisplayName = "选择"),
	Locate UMETA(DisplayName = "定位"),
	Custom UMETA(DisplayName = "自定义"),
};

UENUM()
enum class EOpenSplat4DCaptureCameraMode : uint8
{
	Hemisphere UMETA(DisplayName = "半球"),
	Sphere UMETA(DisplayName = "球体"),
};

UCLASS(EditInlineNew, CollapseCategories, config = OpenSplat4D, defaultconfig, meta = (DisplayName = "OpenSplat4D 编辑器"))
class UOpenSplat4DStepBase : public UObject
{
	GENERATED_BODY()
public:
	virtual void Activate() {}

	virtual void Deactivate() {}

	void SetWorld(UWorld* InWorld) { World = InWorld; }

	virtual UWorld* GetWorld() const override { return World; }

	void SetWorkDir(FString InWorkDir) { WorkDir = InWorkDir; }

	void ExecuteCommand(FString ExecutePath, FString Command, bool bAsync = true, TFunction<void()> FinishedCallback = {});

	virtual void ReceiveMessage(const FString& Message);

	TObjectPtr<UWorld> World;

	FString WorkDir;

	TSharedPtr<FRunnable> Worker;

	TSharedPtr<FRunnableThread> WorkThread;

	float TaskProgressPercent = 0.0f;
	FText LastTaskStatusText = FText::FromString("");
	bool bRequestCancelTask = false;

	FOnRequestTaskStart OnRequestTaskStart;
	FSimpleMulticastDelegate OnTaskFinished;
};

UCLASS(EditInlineNew, CollapseCategories, config = OpenSplat4D, defaultconfig, meta = (DisplayName = "OpenSplat4D 捕获"))
class UOpenSplat4DStep_Capture : public UOpenSplat4DStepBase
{
	GENERATED_BODY()
public:
	void Activate() override;
	void Deactivate() override;

	UFUNCTION(CallInEditor, meta = (DisplayPriority = 1, DisplayName = "捕获", Tooltip = "从多个角度自动拍摄选中的角色，把彩色图(images)、遮罩(masks)、深度(depths)和相机位姿(cameras.txt)保存到工作目录。捕获完成后请依次点击【稀疏重建】和【高斯训练】才能得到一个真正的高斯模型。提示：点云面板顶部有文件夹图标可一键打开工作目录。"))
	void Capture();

	/** Self-contained scan of the selected level geometry into a point cloud (no colmap/python). */
	UFUNCTION(CallInEditor, meta = (DisplayPriority = 4, DisplayName = "扫描到点云", Tooltip = "快速预览：直接把选中的静态网格/场景采样成点云，无需 python/colmap，可立即渲染测试。注意：它【不会】生成图片，只是即时点云，适合先看效果或做编辑器内预览，不能拿去训练。"))
	void ScanToPointCloud();

	UPROPERTY(EditAnywhere, Config, Category = "扫描", DisplayName = "扫描模式")
	EOpenSplat4DScanMode ScanMode = EOpenSplat4DScanMode::MeshSurface;

	UPROPERTY(EditAnywhere, Config, Category = "扫描", meta = (UIMin = 1, ClampMin = 1, UIMax = 64), DisplayName = "扫描密度")
	int32 ScanDensity = 8;

	UPROPERTY(EditAnywhere, Config, Category = "扫描", meta = (UIMin = 0.1, ClampMin = 0.1, UIMax = 500), DisplayName = "点尺寸缩放")
	float ScanPointScale = 3.f;

	UPROPERTY(EditAnywhere, Config, Category = "扫描", DisplayName = "扫描保存目录")
	FString ScanSaveDir = TEXT("/Game/OpenSplat4D");

	UFUNCTION(CallInEditor, meta = (DisplayPriority = 2, DisplayName = "上一个相机", Tooltip = "在自动生成的捕获相机阵列中向前切换，预览某个拍摄机位看到的画面。"))
	void PrevCamera();

	UFUNCTION(CallInEditor, meta = (DisplayPriority = 3, DisplayName = "下一个相机", Tooltip = "在自动生成的捕获相机阵列中向后切换，预览某个拍摄机位看到的画面。"))
	void NextCamera();

	void SetSelectionByComponents(const TArray<UActorComponent*>& InSourceComponents);
	void OnActorSelectionChanged(const TArray<UObject*>& NewSelection, bool bForceRefresh);
	void OnComponentTransformChanged(USceneComponent* Component, ETeleportType TeleportType);
	void UpdateCameraMatrix();
	void SetCurrentCameraIndex(int InIndex);
	void SCC_ApplyCamera();
	void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

public:
	UPROPERTY(VisibleAnywhere, Transient, Category = "OpenSplat4D", DisplayName = "捕获组资产")
	TObjectPtr<UOpenSplat4DCaptureSet> CaptureSetAsset;

	UPROPERTY(EditAnywhere, Config, Category = "OpenSplat4D", DisplayName = "来源模式")
	EOpenSplat4DCaptureSourceMode SourceMode = EOpenSplat4DCaptureSourceMode::Select;

	UPROPERTY(EditAnywhere, Transient, meta = (EditCondition = "SourceMode == EOpenSplat4DCaptureSourceMode::Select", EditConditionHides), Category = "OpenSplat4D", DisplayName = "选中的角色")
	TArray<TObjectPtr<AActor>> SelectionActors;

	UPROPERTY(EditAnywhere, Config, meta = (EditCondition = "SourceMode != EOpenSplat4DCaptureSourceMode::Custom", EditConditionHides), Category = "OpenSplat4D", DisplayName = "相机模式")
	EOpenSplat4DCaptureCameraMode CameraMode = EOpenSplat4DCaptureCameraMode::Hemisphere;

	UPROPERTY(EditAnywhere, Config, meta = (EditCondition = "SourceMode != EOpenSplat4DCaptureSourceMode::Custom", EditConditionHides), Category = "OpenSplat4D", DisplayName = "相机阵列行列数")
	int FrameXY = 10;

	UPROPERTY(EditAnywhere, Config, meta = (UIMin = 0.01, ClampMin = 0.01, UIMax = 2), meta = (EditCondition = "SourceMode != EOpenSplat4DCaptureSourceMode::Custom", EditConditionHides), Category = "OpenSplat4D", DisplayName = "捕获距离缩放")
	float CaptureDistanceScale = 0.6f;

	UPROPERTY(VisibleAnywhere, Transient, meta = (EditCondition = "SourceMode == EOpenSplat4DCaptureSourceMode::Locate", EditConditionHides), Category = "OpenSplat4D", DisplayName = "定位辅助体")
	TObjectPtr<ATriggerSphere> LocateActor;

	UPROPERTY(EditAnywhere, Transient, meta = (EditCondition = "SourceMode != EOpenSplat4DCaptureSourceMode::Select", EditConditionHides), Category = "OpenSplat4D", DisplayName = "隐藏的角色")
	TArray<TObjectPtr<AActor>> HiddenActors;

	UPROPERTY(VisibleAnywhere, Transient, Category = "OpenSplat4D", DisplayName = "场景捕获组件")
	TObjectPtr<ASceneCapture2D> SceneCapture;

	UPROPERTY(VisibleAnywhere, Transient, Category = "OpenSplat4D", DisplayName = "渲染目标")
	TObjectPtr<UTextureRenderTarget2D> RenderTarget;

	UPROPERTY(EditAnywhere, Config, Category = "OpenSplat4D", DisplayName = "渲染目标分辨率",
		meta = (Tooltip = "捕获时每张照片的分辨率（宽=高）。2K(2048)是质量和速度的平衡点。增大分辨率能让 COLMAP 提取更多特征点，从而改善稀疏重建质量，但会显著增加捕获和训练时间。推荐：快速预览=1024，正式重建=2048，高质量=4096。"))
	int RenderTargetResolution = 2048;

	UPROPERTY(EditAnywhere, Config, Category = "OpenSplat4D", DisplayName = "捕获最终颜色")
	bool bCaptureFinalColor = false;

	UPROPERTY(EditAnywhere, Config, Category = "OpenSplat4D", DisplayName = "捕获深度")
	bool bCaptureDepth = true;

	UPROPERTY(EditAnywhere, Config, Category = "OpenSplat4D", DisplayName = "显示标志设置")
	TArray<FEngineShowFlagsSetting> ShowFlagSettings;

	UPROPERTY(EditAnywhere, Config, Category = "OpenSplat4D", DisplayName = "后处理设置")
	struct FPostProcessSettings PostProcessSettings;

	UPROPERTY(EditAnywhere, Transient, meta = (EditCondition = "SourceMode == EOpenSplat4DCaptureSourceMode::Custom", EditConditionHides), Category = "OpenSplat4D", DisplayName = "相机角色")
	TArray<AActor*> CameraActors;

	FBoxSphereBounds CurrentBounds;
	int CurrentCameraIndex = 0;
};

UCLASS(EditInlineNew, CollapseCategories, config = OpenSplat4D, defaultconfig, meta = (DisplayName = "OpenSplat4D 稀疏重建"))
class UOpenSplat4DStep_SparseReconstruction : public UOpenSplat4DStepBase
{
	GENERATED_BODY()
public:
	/** Which capture set (index asset) to reconstruct. Must be selected so the
	 *  step operates on the right images / cameras; the panel auto-fills it from
	 *  the top 【捕获组】 picker, but you can override it here. Null => blocked. */
	UPROPERTY(EditAnywhere, Category = "目标", DisplayName = "目标捕获组（索引资产）")
	TObjectPtr<UOpenSplat4DCaptureSet> TargetCaptureSet;

	void Activate() override;
	void Deactivate() override;

	UFUNCTION(CallInEditor, meta = (DisplayPriority = 1, DisplayName = "稀疏重建", Tooltip = "运行 COLMAP 稀疏重建：特征提取→穷举匹配→映射→模型对齐，把【捕获】生成的图片变成稀疏点云和相机位姿，供下一步【高斯训练】使用。必须先完成【捕获】，并在【目标捕获组（索引资产）】中选择要重建的捕获组。"))
	void Reconstruction();

	UFUNCTION(CallInEditor, meta = (DisplayPriority = 2, DisplayName = "编辑 Colmap 配置", Tooltip = "用系统默认文本编辑器打开 COLMAP 的配置文件，可手动修改特征提取/匹配/映射等参数并保存；下次【稀疏重建】会使用修改后的参数。"))
	void ColmapEdit();

	UFUNCTION(CallInEditor, meta = (DisplayPriority = 3, DisplayName = "查看 Colmap 结果", Tooltip = "弹出一个查看窗口，显示 COLMAP 稀疏重建的结果（相机位置与稀疏点云），用来检查重建质量：如果点云散乱或相机错乱，说明捕获角度/图片有问题，需重新捕获。"))
	void ColmapView();

	void ReconstructionSparse(bool bAsync, TFunction<void()> FinishedCallback = {});
	void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	void UpdateParams();

public:
	UPROPERTY(VisibleAnywhere, Category = "特征提取器", DisplayName = "特征提取参数")
	FString FeatureExtractorParams;

	UPROPERTY(EditAnywhere, Config, Category = "特征提取器", DisplayName = "最大特征数")
	int MaxNumFeatures = 8192;

	UPROPERTY(EditAnywhere, Config, Category = "特征提取器", DisplayName = "特征提取参数（自定义）")
	FString FeatureExtractorParamsCustom;

	UPROPERTY(VisibleAnywhere, Category = "穷举匹配", DisplayName = "穷举匹配参数")
	FString ExhaustiveMatcherParams;

	UPROPERTY(EditAnywhere, Config, Category = "穷举匹配", DisplayName = "穷举匹配参数（自定义）")
	FString ExhaustiveMatcherParamsCustom;

	UPROPERTY(VisibleAnywhere, Category = "映射", DisplayName = "映射参数")
	FString MapperParams;

	UPROPERTY(EditAnywhere, Config, Category = "映射", DisplayName = "最小内点数")
	int AbsPoseMinNumInliers = 1;

	UPROPERTY(EditAnywhere, Config, Category = "映射", DisplayName = "映射参数（自定义）")
	FString MapperParamsCustom;

	UPROPERTY(VisibleAnywhere, Category = "模型对齐", DisplayName = "模型对齐参数")
	FString ModelAlignerParams;

	UPROPERTY(EditAnywhere, Config, Category = "模型对齐", DisplayName = "模型对齐参数（自定义）")
	FString ModelAlignerParamsCustom;
};

UCLASS(EditInlineNew, CollapseCategories, config = OpenSplat4D, defaultconfig, meta = (DisplayName = "OpenSplat4D 高斯训练"))
class UOpenSplat4DStep_GaussianSplatting : public UOpenSplat4DStepBase
{
	GENERATED_BODY()
public:
	/** Which capture set (index asset) to train on. Must be selected so training
	 *  knows which reconstruction to fit; the panel auto-fills it from the top
	 *  【捕获组】 picker, but you can override it here. Null => blocked. */
	UPROPERTY(EditAnywhere, Category = "目标", DisplayName = "目标捕获组（索引资产）")
	TObjectPtr<UOpenSplat4DCaptureSet> TargetCaptureSet;

	void Activate() override;
	void Deactivate() override;

	UFUNCTION(CallInEditor, meta = (DisplayPriority = 1, DisplayName = "训练", Tooltip = "运行 3DGS/4DGS 训练网络，把【稀疏重建】的结果拟合为大量高斯基元（高斯系数）。耗时较长（默认约 7000 次迭代）。训练结束后会自动【重新加载】。勾选'训练 4D 模型'可训练带时间的动态模型。执行前必须在【目标捕获组（索引资产）】中选择要训练的捕获组。"))
	void Train();

	void Train(bool bAsync, TFunction<void()> FinishedCallback = {});

	FString Clip(FString PlyPath);

	UFUNCTION(CallInEditor, meta = (DisplayPriority = 2, DisplayName = "重新加载", Tooltip = "重新加载训练产出的 point_cloud.ply 到当前预览资产，让视口中的高斯模型更新为最新训练结果。训练完成后通常会自动调用，也可手动点击刷新。"))
	void Reload();

	UFUNCTION(CallInEditor, meta = (DisplayPriority = 3, DisplayName = "导出资产", Tooltip = "把训练好的高斯模型导出为可分发的资产文件，便于保存到磁盘或分享给其他人。"))
	void Export();

	UGaussianSplatAsset* LoadPly(UObject* Outer, FName AssetName);

	/** Automatically save the trained result as a native UE asset under
	 *  /Game/OpenSplat4D/Models/<timestamp> so it shows up in the Content
	 *  Browser as a first-class, re-usable asset.
	 *  Returns the created asset (nullptr on failure). */
	UGaussianSplatAsset* SaveToContent();

	/** Drop an AGaussianSplatActor carrying the given gaussian splat into
	 *  the current editor world (for quick preview after training). */
	void PlaceInLevel(UGaussianSplatAsset* SplatAsset);

	/** Fill capture-set data paths (images / sparse / trained). */
	void FillAssetPaths();

	void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	void UpdateParams();

public:
	/** When true, train a 4DGS model (uses the 4D repo + time-enabled flags); otherwise static 3DGS. */
	UPROPERTY(EditAnywhere, Config, Category = "OpenSplat4D", DisplayName = "训练 4D 模型")
	bool bTrain4D = false;

	/** After training finishes, automatically save the result as a native UE
	 *  asset under /Game/OpenSplat4D/Models/<timestamp> (and fill its data paths).
	 *  When off, the result only lives in the transient preview and you must
	 *  click 【导出资产】 to persist it. Default on. */
	UPROPERTY(EditAnywhere, Config, Category = "OpenSplat4D", DisplayName = "训练后自动导入到 Content")
	bool bAutoImportToContent = true;

	/** After training and import, automatically drop a SplatActor carrying the
	 *  trained point cloud into the current level (default off — safety first). */
	UPROPERTY(EditAnywhere, Config, Category = "OpenSplat4D", DisplayName = "训练后放入场景")
	bool bPlaceInLevel = false;

	UPROPERTY(VisibleAnywhere, Transient, Category = "输出", DisplayName = "训练结果")
	TObjectPtr<UGaussianSplatAsset> Result;

	UPROPERTY(DisplayName = "本地包")
	TObjectPtr<UPackage> LocalPackage;

	UPROPERTY(Config, DisplayName = "上次保存路径")
	FString LastSavePath;

	UPROPERTY(VisibleAnywhere, Category = "训练", DisplayName = "高斯训练参数")
	FString GaussianSplattingTrainParams;

	UPROPERTY(EditAnywhere, Config, Category = "训练", meta = (UIMin = 1, ClampMin = 1, UIMax = 8), DisplayName = "分辨率倍率")
	int Resolution = 1;

	UPROPERTY(EditAnywhere, Config, Category = "训练", meta = (Tooltip = "训练的总迭代次数。"), DisplayName = "迭代次数")
	int Iterations = 7000;

	/** Quality level for PLY import (compression precision). */
	UPROPERTY(EditAnywhere, Config, Category = "训练", DisplayName = "导入质量")
	EGaussianQualityLevel ImportQuality = EGaussianQualityLevel::Medium;

	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = "训练", meta = (Tooltip = "球谐特征的学习率。"), DisplayName = "特征学习率")
	float Feature_LR = 0.0025f;

	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = "训练", meta = (Tooltip = "不透明度学习率。"), DisplayName = "不透明度学习率")
	float Opacity_LR = 0.05f;

	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = "训练", meta = (Tooltip = "缩放学习率。"), DisplayName = "缩放学习率")
	float Scaling_LR = 0.005f;

	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = "训练", meta = (Tooltip = "旋转学习率。"), DisplayName = "旋转学习率")
	float Rotation_LR = 0.001f;

	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = "训练", meta = (Tooltip = "位置学习率从初始到最终所经过的步数（从 0 开始）。"), DisplayName = "位置学习率步数")
	int Position_LR_MaxSteps = 30000;

	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = "训练", meta = (Tooltip = "3D 位置初始学习率。"), DisplayName = "位置学习率初始值")
	float Position_LR_Init = 0.00016f;

	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = "训练", meta = (Tooltip = "3D 位置最终学习率。"), DisplayName = "位置学习率最终值")
	float Position_LR_Final = 0.0000016f;

	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = "训练", meta = (Tooltip = "位置学习率倍率。"), DisplayName = "位置学习率延迟倍率")
	float Position_LR_DelayMult = 0.01f;

	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = "训练", meta = (Tooltip = "开始稠密化的迭代次数。"), DisplayName = "稠密化起始迭代")
	int DensifyFromIter = 500;

	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = "训练", meta = (Tooltip = "停止稠密化的迭代次数。"), DisplayName = "稠密化结束迭代")
	int DensifyUntilIter = 15000;

	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = "训练", meta = (Tooltip = "基于 2D 位置梯度决定是否对点进行稠密化的阈值。"), DisplayName = "稠密化梯度阈值")
	float DensifyGradThreshold = 0.0002f;

	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = "训练", meta = (Tooltip = "稠密化的频率。"), DisplayName = "稠密化间隔")
	int DensificationInterval = 100;

	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = "训练", meta = (Tooltip = "重置不透明度的频率。"), DisplayName = "不透明度重置间隔")
	int OpacityResetInterval = 3000;

	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = "训练", meta = (UIMin = 0, ClampMin = 0, UIMax = 1), DisplayName = "深度 L1 权重初始值")
	float Depth_L1_WeightInit = 1.0f;

	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = "训练", meta = (UIMin = 0, ClampMin = 0, UIMax = 1), DisplayName = "深度 L1 权重最终值")
	float Depth_L1_WeightFinal = 0.01f;

	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = "训练", meta = (UIMin = 0, ClampMin = 0, UIMax = 1, Tooltip = "SSIM 对总损失的权重（0 到 1）。"), DisplayName = "SSIM 损失权重")
	float LambdaDssim = 0.2f;

	UPROPERTY(EditAnywhere, Config, AdvancedDisplay, Category = "训练", meta = (UIMin = 0, ClampMin = 0, UIMax = 1, Tooltip = "点必须超过场景范围的比例（0–1）才会被强制稠密化。"), DisplayName = "强制稠密化场景占比")
	float PercentDense = 0.01f;

	UPROPERTY(EditAnywhere, Config, Category = "加载", DisplayName = "压缩方式")
	EOpenSplat4DCompressionMethod CompressionMethod = EOpenSplat4DCompressionMethod::Zlib;

	UPROPERTY(EditAnywhere, Config, Category = "加载", DisplayName = "按遮罩裁剪")
	bool bClippingByMask = false;

	UPROPERTY(EditAnywhere, Config, meta = (EditCondition = "bClippingByMask", EditConditionHides, UIMin = 1, ClampMin = 0, UIMax = 20), Category = "加载", DisplayName = "遮罩膨胀")
	int MaskDilation = 5;

	UPROPERTY(EditAnywhere, Config, meta = (EditCondition = "bClippingByMask", EditConditionHides, UIMin = 1, ClampMin = 0.01, UIMax = 1), Category = "加载", DisplayName = "裁剪阈值")
	float ClipThreshold = 0.8f;

	UPROPERTY(EditAnywhere, Config, Category = "加载", DisplayName = "观测距离")
	float DistanceOfObservation = 0.0f;

	UPROPERTY(EditAnywhere, Config, Category = "加载", DisplayName = "最小观测屏幕占比")
	float MinScreenSizeOfObservation = 0.01f;

	FSimpleDelegate OnPlyLoadFinished;
};
