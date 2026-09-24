// Copyright OpenSplat4D 2026 All Rights Reserved.
#include "OpenSplat4DEditorModule.h"
#include "AssetToolsModule.h"
#include "OpenSplat4DAssetFactory.h"
#include "OpenSplat4DActorFactory.h"
#include "OpenSplat4DEdMode.h"
#include "OpenSplat4DPointCloud.h"
#include "OpenSplat4DPointCloudActor.h"
#include "NanoGSGaussianSplatComponent.h"
#include "OpenSplat4DSplatActor.h"
#include "OpenSplat4DSettings.h"
#include "OpenSplat4DLocalization.h"
#include "OpenSplat4DPointCloudEditor.h"
#include "NiagaraComponent.h"
#include "SOpenSplat4DPointCloudEditorViewport.h"
#include "OpenSplat4DNiagaraSetup.h"
#include "HAL/IConsoleManager.h"
#include "Containers/Ticker.h"
#include "Internationalization/Internationalization.h"

#include "EditorModeRegistry.h"
#include "ToolMenus.h"
#include "ContentBrowserModule.h"
#include "ContentBrowserMenuContexts.h"
#include "IContentBrowserSingleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "ISettingsModule.h"
#include "ISettingsSection.h"

DEFINE_LOG_CATEGORY(LogOpenSplat4DEditor);

#define LOCTEXT_NAMESPACE "OpenSplat4DEditor"

void FOpenSplat4DEditorModule::StartupModule()
{
	// The plugin UI follows the editor language (Editor Preferences > Region &
	// Language): any "zh*" culture selects Chinese, everything else English. Do
	// this before the string table below is used by any OS4D_TEXT(...) call.
	OpenSplat4DLocalization::SyncWithEditorLanguage();
	FInternationalization::Get().OnCultureChanged().AddRaw(this, &FOpenSplat4DEditorModule::HandleEditorCultureChanged);

	// --- Register bilingual UI strings -------------------------------------
	// Must run before any widget / asset action / editor mode is constructed.
	OpenSplat4DLocalization::RegisterString(TEXT("OpenSplat4D"), TEXT("OpenSplat4D"));
	OpenSplat4DLocalization::RegisterString(TEXT("Paths to colmap / python / training repositories used by the capture -> reconstruct -> train pipeline."),
		TEXT("捕获 → 重建 → 训练流水线所需的 colmap / python / 训练仓库路径。"));
	OpenSplat4DLocalization::RegisterString(TEXT("Create OpenSplat4D Actor"), TEXT("创建 OpenSplat4D 角色"));
	OpenSplat4DLocalization::RegisterString(TEXT("Spawn an OpenSplat4DPointCloudActor using this cloud"),
		TEXT("使用此点云生成 OpenSplat4DPointCloudActor"));
	OpenSplat4DLocalization::RegisterString(TEXT("OpenSplat4D Point Cloud"), TEXT("OpenSplat4D 点云"));
	OpenSplat4DLocalization::RegisterString(TEXT("OpenSplat4D Point Cloud Editor"), TEXT("OpenSplat4D 点云编辑器"));
	OpenSplat4DLocalization::RegisterString(TEXT("Remove Points"), TEXT("删除点"));
	OpenSplat4DLocalization::RegisterString(TEXT("Viewport"), TEXT("视口"));
	OpenSplat4DLocalization::RegisterString(TEXT("Details"), TEXT("细节"));
	OpenSplat4DLocalization::RegisterString(TEXT("Preview Scene Settings"), TEXT("预览场景设置"));
	OpenSplat4DLocalization::RegisterString(TEXT("Features"), TEXT("特征"));
	OpenSplat4DLocalization::RegisterString(TEXT("Capture"), TEXT("捕获"));
	OpenSplat4DLocalization::RegisterString(TEXT("Sparse"), TEXT("稀疏重建"));
	OpenSplat4DLocalization::RegisterString(TEXT("Gaussian"), TEXT("高斯训练"));
	OpenSplat4DLocalization::RegisterString(TEXT("Settings"), TEXT("设置"));
	OpenSplat4DLocalization::RegisterString(TEXT("Usage"), TEXT("用法"));
	OpenSplat4DLocalization::RegisterString(TEXT("Open the working directory"), TEXT("打开工作目录"));
	OpenSplat4DLocalization::RegisterString(TEXT("Cancel Current Task"), TEXT("取消当前任务"));
	OpenSplat4DLocalization::RegisterString(TEXT("Execution failed: a task is already running. Wait for it to finish, or cancel it first."),
		TEXT("执行失败：当前已有任务在运行，请等待完成或取消后再试"));
	OpenSplat4DLocalization::RegisterString(TEXT("0. Create from Scene (Scan)"), TEXT("0. 从场景创建（扫描）"));
	OpenSplat4DLocalization::RegisterString(TEXT("Camera Depth Scan (off = Mesh Surface)"), TEXT("相机深度扫描（关闭=网格表面采样）"));
	OpenSplat4DLocalization::RegisterString(TEXT("Density"), TEXT("密度"));
	OpenSplat4DLocalization::RegisterString(TEXT("Point Size"), TEXT("点尺寸"));
	OpenSplat4DLocalization::RegisterString(TEXT("Scan Selected -> Point Cloud"), TEXT("扫描选中对象 → 点云"));
	OpenSplat4DLocalization::RegisterString(TEXT("Select one or more actors in the viewport, then scan. Mesh Surface samples static-mesh geometry directly; Camera Depth places a camera rig and reconstructs depth. No colmap / python needed -- you can create a point cloud and test rendering immediately."),
		TEXT("在视口中选中一个或多个 Actor 后点击扫描。网格表面模式直接采样静态网格几何体；相机深度模式放置相机阵列并重建深度。无需 colmap / python，可立即生成点云并测试渲染。"));
	OpenSplat4DLocalization::RegisterString(TEXT("1. Import"), TEXT("1. 导入"));
	OpenSplat4DLocalization::RegisterString(TEXT("Import .ply / .4dgs ..."), TEXT("导入 .ply / .4dgs ..."));
	OpenSplat4DLocalization::RegisterString(TEXT("Or drag a .ply / .4dgs file into the Content Browser. The asset is created under /Game/OpenSplat4D/."),
		TEXT("也可将 .ply / .4dgs 文件拖入内容浏览器。资产会创建在 /Game/OpenSplat4D/ 下。"));
	OpenSplat4DLocalization::RegisterString(TEXT("2. Place in level"), TEXT("2. 放入关卡"));
	OpenSplat4DLocalization::RegisterString(TEXT("Spawn Actor in Level"), TEXT("在关卡中生成 Actor"));
	OpenSplat4DLocalization::RegisterString(TEXT("Or drag the asset from the Content Browser into the viewport. You can also right-click the asset -> 'Create OpenSplat4D Actor'."),
		TEXT("也可将资产从内容浏览器拖入视口。或右键资产 → '创建 OpenSplat4D Actor'。"));
	OpenSplat4DLocalization::RegisterString(TEXT("3. 4D Playback"), TEXT("3. 4D 回放"));
	OpenSplat4DLocalization::RegisterString(TEXT("Use Selected Actor"), TEXT("使用选中 Actor"));
	OpenSplat4DLocalization::RegisterString(TEXT("Play"), TEXT("播放"));
	OpenSplat4DLocalization::RegisterString(TEXT("Pause"), TEXT("暂停"));
	OpenSplat4DLocalization::RegisterString(TEXT("Time"), TEXT("时间"));
	OpenSplat4DLocalization::RegisterString(TEXT("Speed"), TEXT("速度"));
	OpenSplat4DLocalization::RegisterString(TEXT("Loop"), TEXT("循环"));
	OpenSplat4DLocalization::RegisterString(TEXT("Pick a point cloud asset first."), TEXT("请先选择一个点云资产。"));
	OpenSplat4DLocalization::RegisterString(TEXT("Scan produced no points. Select static-mesh actor(s) for Mesh Surface, or ensure visible geometry for Camera Depth."),
		TEXT("扫描未产生任何点。网格表面模式请选择静态网格Actor；相机深度模式请确保存在可见几何体。"));
	OpenSplat4DLocalization::RegisterString(TEXT("A valid OpenSplat4D point cloud must be specified."),
		TEXT("必须指定有效的 OpenSplat4D 点云。"));
	// (Brand / technical ids kept identical in both languages.)
	OpenSplat4DLocalization::RegisterString(TEXT("OpenSplat4DPointCloud"), TEXT("OpenSplat4DPointCloud"));
	// Runtime / dialog strings that are not part of the Slate panel layout.
	OpenSplat4DLocalization::RegisterString(TEXT("Save As"), TEXT("另存为"));
	OpenSplat4DLocalization::RegisterString(TEXT("Failed to import '{0}'. Not a valid .ply / .4dgs."),
		TEXT("导入 '{0}' 失败：不是有效的 .ply / .4dgs 文件。"));
	OpenSplat4DLocalization::RegisterString(TEXT("Import OpenSplat4D Point Cloud"), TEXT("导入 OpenSplat4D 点云"));
	// Scan-to-point-cloud feedback (shown as Slate notifications).
	OpenSplat4DLocalization::RegisterString(TEXT("Select one or more actors in the viewport first, then click 'Scan to Point Cloud'."),
		TEXT("请先在视口中选中一个或多个物体，然后再点击“扫描到点云”。"));
	OpenSplat4DLocalization::RegisterString(TEXT("The scan produced no points. Mesh Surface needs static-mesh actors selected; for characters / terrain / blueprints switch to 'Camera Depth Scan'."),
		TEXT("扫描未生成任何点。网格表面模式需选中含静态网格(StaticMesh)的物体；若选中的是角色/地形/蓝图，请改用“相机深度扫描”模式。"));
	OpenSplat4DLocalization::RegisterString(TEXT("Scan finished: {0} points, saved as a UE asset to {1} (the Content Browser has been synced; note this is a .uasset asset, not the captured-images folder)."),
		TEXT("扫描完成：{0} 个点，已保存为 UE 资产到 {1}（内容浏览器已定位；注意这是 .uasset 资产，不是捕获图片目录）。"));

	// Capture-set picker (panel header) and the "open index" tooltip.
	OpenSplat4DLocalization::RegisterString(TEXT("Capture Set:"), TEXT("捕获组:"));
	OpenSplat4DLocalization::RegisterString(TEXT("New Capture Set"), TEXT("新建捕获组"));
	OpenSplat4DLocalization::RegisterString(TEXT("Open Index"), TEXT("打开索引"));
	OpenSplat4DLocalization::RegisterString(TEXT("Open the currently selected capture-set (index) asset to view / edit the file locations and ranges it records."),
		TEXT("打开当前选中的【捕获组】索引资产，查看 / 编辑其记录的文件位置与范围。"));
	OpenSplat4DLocalization::RegisterString(TEXT("Failed to create the capture-set asset."), TEXT("创建捕获组资产失败。"));
	OpenSplat4DLocalization::RegisterString(TEXT("Pick an index asset in the 'Capture Set' drop-down at the top of the panel first, then click 'Open Index'."),
		TEXT("请先在顶部的【捕获组】下拉中选择一个索引资产，再点击【打开索引】。"));

	// Dependency pre-flight (OnRequestTaskStart).
	OpenSplat4DLocalization::RegisterString(TEXT("COLMAP executable"), TEXT("Colmap 可执行文件"));
	OpenSplat4DLocalization::RegisterString(TEXT("3DGS training repository directory"), TEXT("3DGS 训练仓库目录"));
	OpenSplat4DLocalization::RegisterString(TEXT("4DGS training repository directory"), TEXT("4DGS 训练仓库目录"));
	OpenSplat4DLocalization::RegisterString(TEXT("{0}: not found in the optional local ThirdParty folder and not specified in Project Settings > OpenSplat4D. The plugin does not ship these tools: set the path in Settings, or drop your own copy into the plugin's ThirdParty folder."),
		TEXT("{0}：未在可选的本地 ThirdParty 目录中找到，也未在【项目设置 → OpenSplat4D】中指定。插件不随包携带这些工具：请在设置里指定路径，或把你自己的副本放入插件 ThirdParty 目录。"));
	OpenSplat4DLocalization::RegisterString(TEXT("{0}: the specified path does not exist or is invalid: {1}"),
		TEXT("{0}：已指定的路径不存在或无效：{1}"));
	OpenSplat4DLocalization::RegisterString(TEXT("No target capture set (index asset) selected: pick the capture set to reconstruct / train in 'Target Capture Set (Index Asset)' for this step (you can also pick it in the 'Capture Set' drop-down at the top of the panel -- the two stay in sync). The step is skipped when no target is selected, to avoid reconstructing the wrong asset."),
		TEXT("目标捕获组（索引资产）未选择：请先在对应步骤的【目标捕获组（索引资产）】中选择要重建 / 训练的捕获组（也可在面板顶部的【捕获组】下拉中选择，会自动同步）。未选择时不会执行，以避免重建错误的资产。"));

	// Usage tab: intro + help sections.
	OpenSplat4DLocalization::RegisterString(TEXT(
		"OpenSplat4D turns real objects / people into gaussian splat models that render in real time inside UE.\n"
		"The three tabs at the top -- Capture -> Sparse -> Gaussian -- are the three steps, executed in order. This page explains the workflow; the Settings tab configures the python / colmap / training-repository paths.\n"
		"Select the actor you want to process in the viewport first, then start."),
		TEXT(
		"OpenSplat4D 把真实物体 / 人物变成可在 UE 里实时渲染的高斯泼溅模型。\n"
		"顶部三个标签页【捕获】→【稀疏重建】→【高斯训练】是按顺序执行的三步；本页是用法说明；【设置】里配置 python / colmap / 训练仓库路径。\n"
		"先用视口选中要处理的角色，再开始。"));
	OpenSplat4DLocalization::RegisterString(TEXT("Full pipeline: train a real gaussian model (recommended)"),
		TEXT("完整流程：训练一个真正的高斯模型（推荐）"));
	OpenSplat4DLocalization::RegisterString(TEXT(
		"Step 1 - Capture: on the Capture tab, select the actor to scan (click it in the viewport), set the camera grid rows / columns and capture distance, then click Capture. "
		"The engine screenshots the actor from many angles into the working directory: images/ (colour), masks/, depths/ and cameras.txt (camera poses).\n"
		"Step 2 - Sparse: switch to the Sparse tab and click Sparse. COLMAP runs feature extraction -> matching -> mapping -> alignment, turning the images into a sparse point cloud plus camera poses (the input for training). "
		"Use 'View Colmap Result' to check the quality and 'Edit Colmap Config' to tune the parameters.\n"
		"Step 3 - Gaussian: switch to the Gaussian tab, adjust the iteration count if needed, then click Train. The network fits the sparse reconstruction with a large number of gaussian primitives (about 7000 iterations by default; the runtime depends on your GPU and image count). "
		"On completion it automatically reloads, and the real gaussian model appears in the viewport."),
		TEXT(
		"第 1 步 · 捕获：在【捕获】标签页选中要扫描的角色（视口里点选 Actor），设置相机阵列行列数、捕获距离等，点【捕获】。"
		"引擎从多个角度自动截图，保存到工作目录的 images/（彩色图）、masks/（遮罩）、depths/（深度）、cameras.txt（相机位姿）。\n"
		"第 2 步 · 稀疏重建：切到【稀疏重建】标签页点【稀疏重建】。COLMAP 会做特征提取→匹配→映射→对齐，把图片变成稀疏点云和相机位姿（这是训练的输入）。"
		"可用【查看 Colmap 结果】检查质量，用【编辑 Colmap 配置】调参。\n"
		"第 3 步 · 高斯训练：切到【高斯训练】标签页，按需调整迭代次数等，点【训练】。网络把稀疏重建拟合为大量高斯基元（默认约 7000 次迭代，耗时取决于显卡与图片数）。"
		"完成后自动【重新加载】，视口即可看到真正的高斯模型。"));
	OpenSplat4DLocalization::RegisterString(TEXT("Quick preview: scan to a point cloud (no python / colmap needed)"),
		TEXT("快速预览：扫描到点云（无需 python / colmap）"));
	OpenSplat4DLocalization::RegisterString(TEXT(
		"On the Capture tab click 'Scan to Point Cloud', or use 'Scan Selected -> Point Cloud' further down this page: the selected geometry is sampled straight into a point cloud that can be rendered in the viewport immediately, for a quick shape check.\n"
		"Note: it does NOT produce images and cannot be used for training -- it is an in-editor instant preview only. For a trainable / exportable final model, follow the full pipeline above (Capture -> Sparse -> Gaussian)."),
		TEXT(
		"在【捕获】标签页点【扫描到点云】，或本页下方【扫描选中对象 → 点云】：直接把选中几何体采样成点云，立刻能在视口渲染，用来快速看形状。\n"
		"注意：它【不生成图片】，也不能拿去训练，只是编辑器内的即时预览。想要可训练 / 可导出的最终模型，请走上面的完整流程（捕获→稀疏重建→高斯训练）。"));
	OpenSplat4DLocalization::RegisterString(TEXT("Where did my captured images go?"),
		TEXT("我捕获的图片去哪了？"));
	OpenSplat4DLocalization::RegisterString(TEXT(
		"Everything is stored in the plugin working directory:\n"
		"  <your project>/Intermediate/OpenSplat4D/OpenSplat4DEditor/\n"
		"containing images, masks, depths and cameras.txt.\n"
		"To open it: click the folder icon at the top of the Capture tab (browse working directory), or paste that path into your file manager.\n"
		"You can also search the Output Log for \"Capture Finished\" -- the log prints the exact full path of every image."),
		TEXT(
		"全部保存在插件工作目录：\n"
		"  <你的项目>/Intermediate/OpenSplat4D/OpenSplat4DEditor/\n"
		"其下包含 images、masks、depths、cameras.txt。\n"
		"打开方式：点【捕获】标签页顶部的文件夹图标（浏览工作目录）即可一键打开；或在文件管理器地址栏粘贴该路径。\n"
		"也可在 Output Log 搜索 \"Capture Finished\"，日志会打印每张图片的确切完整路径。"));
	OpenSplat4DLocalization::RegisterString(TEXT("Dependencies (configure these in Settings first)"),
		TEXT("前置依赖（必须先在【设置】配置）"));
	OpenSplat4DLocalization::RegisterString(TEXT(
		"- Python: install Python 3 and set the python executable path in Settings (leave it empty to use the python on your PATH).\n"
		"- COLMAP: install COLMAP and set the colmap executable in Settings (used for the sparse reconstruction and for viewing its result).\n"
		"- Training repositories: set the 3DGS (and optionally 4DGS) repository path to your local gaussian-splatting / 4d-gaussian-splatting source folder (Train calls the training script inside it).\n"
		"When a path is missing or wrong the corresponding step reports an error in the Output Log."),
		TEXT(
		"· Python：需安装 Python 3，并在【设置】指定 python 可执行文件路径（留空则用系统 PATH 里的 python）。\n"
		"· COLMAP：需安装 COLMAP，并在【设置】指定 colmap 可执行文件（用于稀疏重建与查看结果）。\n"
		"· 训练仓库：需填写 3DGS（以及可选的 4DGS）仓库路径，指向你本地的 gaussian-splatting / 4d-gaussian-splatting 代码目录（【训练】会调用其中的训练脚本）。\n"
		"未配置或路径错误时，对应步骤会在 Output Log 报错。"));
	OpenSplat4DLocalization::RegisterString(TEXT("FAQ"), TEXT("常见问题"));
	OpenSplat4DLocalization::RegisterString(TEXT(
		"- Step 2 (sparse reconstruction / training) errors out or does nothing: this is usually an external command path containing spaces (e.g. installed under \"C:/Program Files\", project under \"Unreal Projects\") or python / colmap not being installed. Spaces are now handled automatically -- make sure the dependencies are installed and the Settings paths are correct.\n"
		"- The model is a huge transparent blob / the frame rate is terrible: the scanned point cloud is oversized (every point is drawn as an enormous quad, redrawn over the whole screen). Select the point-cloud actor in the viewport -> Details -> OpenSplat4D -> lower \"Splat Scale\" to 0.05~0.1, or rescan (the default point size is now smaller). Real trained gaussian models have a much smaller scale and do not show this problem.\n"
		"- 'Edit Colmap Config' / 'View Colmap Result' do nothing: same external-command issue, now fixed; if it still does nothing, check the Output Log."),
		TEXT(
		"· 第二步（稀疏重建 / 训练）报错或没反应：通常是外部命令路径含空格（如装在 \"C:/Program Files\"、项目在 \"Unreal Projects\" 下）或 python / colmap 未安装。空格问题现已自动处理，请确保依赖已安装且【设置】路径正确。\n"
		"· 模型是一坨巨大透明的东西 / 帧率极低：这是扫描点云尺寸过大（每个点被画成巨大面片，满屏重复绘制）。在视口选中该点云 Actor → Details → OpenSplat4D → 把 \"Splat Scale\" 调到 0.05~0.1 即可缩小；或重新扫描（默认点尺寸已调小）。真正训练出的高斯模型尺度较小，不会出现此问题。\n"
		"· 【编辑 Colmap 配置】/【查看 Colmap 结果】点了没反应：同外部命令问题，现已修复；若仍无反应请检查 Output Log。"));

	// Asset / step strings outside the Slate panel layout.
	OpenSplat4DLocalization::RegisterString(TEXT("OpenSplat4D Asset"), TEXT("OpenSplat4D 资产"));
	OpenSplat4DLocalization::RegisterString(TEXT("Empty executable path: configure the Python / COLMAP paths under Settings > OpenSplat4D"),
		TEXT("可执行文件路径为空：请在【设置】→ OpenSplat4D 中配置 Python / COLMAP 路径"));

	// Subprocess lifecycle messages shown in the panel's log area.
	OpenSplat4DLocalization::RegisterString(TEXT("[ERROR] Failed to create the pipe; cannot launch the subprocess."),
		TEXT("[ERROR] 管道创建失败，无法启动子进程"));
	OpenSplat4DLocalization::RegisterString(TEXT("[ERROR] Failed to start the subprocess; check the executable path."),
		TEXT("[ERROR] 子进程启动失败，请检查可执行文件路径"));
	OpenSplat4DLocalization::RegisterString(TEXT("[CANCELLED] The task was cancelled by the user."),
		TEXT("[CANCELLED] 用户已取消任务"));
	OpenSplat4DLocalization::RegisterString(TEXT("[TIMEOUT] The task was terminated after {0} seconds."),
		TEXT("[TIMEOUT] 任务超过 {0} 秒被终止"));

	// About / Support card (usage tab).
	OpenSplat4DLocalization::RegisterString(TEXT("About / Support Us"), TEXT("关于 / 支持我们"));
	OpenSplat4DLocalization::RegisterString(TEXT("OpenSplat4D is developed and open-sourced by YuanBaoSMadLab under Apache-2.0. If it helps you, please consider supporting future development:"),
		TEXT("OpenSplat4D 由 YuanBaoSMadLab 开发并开源（Apache-2.0）。如果它帮到了你，欢迎赞助支持后续开发："));
	OpenSplat4DLocalization::RegisterString(TEXT("Afdian (China)"), TEXT("爱发电（国内）"));
	OpenSplat4DLocalization::RegisterString(TEXT("Quark Drive (China mirror, code UZ2J)"), TEXT("夸克网盘（国内加速，提取码 UZ2J）"));
	OpenSplat4DLocalization::RegisterString(TEXT("Project home"), TEXT("项目主页"));

	// Register the "OpenSplat4D" editor mode hosting the usage panel.
	FEditorModeRegistry::Get().RegisterMode<FOpenSplat4DEdMode>(
		FOpenSplat4DEdMode::EdID,
		OS4D_TEXT("OpenSplat4D"),
		FSlateIcon(),
		true,
		010200);

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FOpenSplat4DEditorModule::RegisterMenus));

	// Register the pipeline settings (colmap / python / training repos) under Project > Plugins.
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->RegisterSettings(
			"Project", "Plugins", "OpenSplat4D",
			OS4D_TEXT("OpenSplat4D"),
			OS4D_TEXT("Paths to colmap / python / training repositories used by the capture -> reconstruct -> train pipeline."),
			GetMutableDefault<UOpenSplat4DSettings>());
	}

	// --- Console command: reload a source file into the open point-cloud editor.
	// This is the fastest way to triage a blank preview: open the (empty) asset,
	// then run   OpenSplat4D.Reload C:/path/to/demo.ply   in the console. The
	// editor's billboard component reads the cloud live each frame, so the splats
	// appear immediately without re-importing or restarting.
	{
		static FAutoConsoleCommand CCmdReload(
			TEXT("OpenSplat4D.Reload"),
			TEXT("OpenSplat4D.Reload <path> : load a .ply/.4dgs/.spz file into the currently-open OpenSplat4D point cloud editor."),
			FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
			{
				if (Args.Num() < 1)
				{
					UE_LOG(LogOpenSplat4DEditor, Warning, TEXT("OpenSplat4D.Reload: usage: OpenSplat4D.Reload <full_path_to_file>"));
					return;
				}
				TSharedPtr<FOpenSplat4DPointCloudEditor> Editor = GActiveOpenSplatEditor.Pin();
				if (!Editor.IsValid())
				{
					UE_LOG(LogOpenSplat4DEditor, Warning, TEXT("OpenSplat4D.Reload: no OpenSplat4D point cloud editor is currently open."));
					return;
				}
				UOpenSplat4DPointCloud* Cloud = Editor->GetPointCloud();
				if (!Cloud)
				{
					UE_LOG(LogOpenSplat4DEditor, Warning, TEXT("OpenSplat4D.Reload: editor has no point cloud."));
					return;
				}
				const FString Path = Args[0];
				if (!FPaths::FileExists(Path))
				{
					UE_LOG(LogOpenSplat4DEditor, Warning, TEXT("OpenSplat4D.Reload: file not found: %s"), *Path);
					return;
				}
				Cloud->LoadFromFile(Path);
				Cloud->SourceFilePath = Path;
				Cloud->MarkPackageDirty();
				Cloud->OnPointsChanged.Broadcast();
				if (TSharedPtr<SOpenSplat4DPointCloudEditorViewport> VP = Editor->GetViewport())
				{
					// The DI's dirty tracking handles buffer rebuild automatically.
					// Just broadcast so the Niagara component picks up the new data.
					Cloud->OnPointsChanged.Broadcast();
				}
				UE_LOG(LogOpenSplat4DEditor, Log, TEXT("OpenSplat4D.Reload: loaded %s -> pointCount=%d"), *Path, Cloud->GetPointCount());
			}));
	}

	// Niagara assets are auto-created lazily — the first time an
	// AOpenSplat4DPointCloudActor tries to render (PushStateToNiagara),
	// the runtime checks and triggers creation via the Editor module.
	// This avoids DataValidation crashes during build/packaging.

// [DISABLED] 全面禁用 Niagara 路径：不再自动创建 Niagara 资产。
// 用户若需要 Niagara 路径（模式 2），可手动调用 OpenSplat4DNiagaraSetup::EnsureAssetsExist()。
// FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float) -> bool {
// 	OpenSplat4DNiagaraSetup::EnsureAssetsExist();
// 	return false;
// }), 0.5f);

UE_LOG(LogOpenSplat4DEditor, Log, TEXT("OpenSplat4D editor module started."));
}

void FOpenSplat4DEditorModule::ShutdownModule()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->UnregisterSettings("Project", "Plugins", "OpenSplat4D");
	}

	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);
	FEditorModeRegistry::Get().UnregisterMode(FOpenSplat4DEdMode::EdID);
	FInternationalization::Get().OnCultureChanged().RemoveAll(this);

	UE_LOG(LogOpenSplat4DEditor, Log, TEXT("OpenSplat4D editor module shut down."));
}

void FOpenSplat4DEditorModule::HandleEditorCultureChanged()
{
	// Re-derive the plugin UI language. SyncWithEditorLanguage broadcasts
	// OnUILanguageChanged when it actually changed, which live-rebuilds any open
	// OpenSplat4D panel.
	OpenSplat4DLocalization::SyncWithEditorLanguage();
}

void FOpenSplat4DEditorModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);
	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu.OpenSplat4DPointCloud");
	FToolMenuSection& Section = Menu->FindOrAddSection("GetAssetActions");
	Section.AddDynamicEntry("OpenSplat4D_CreateActor", FNewToolMenuSectionDelegate::CreateLambda(
		[this](FToolMenuSection& Section)
		{
			if (UContentBrowserAssetContextMenuContext* Context = Section.FindContext<UContentBrowserAssetContextMenuContext>())
			{
				if (Context->SelectedAssets.Num() == 1)
				{
					UOpenSplat4DPointCloud* Cloud = Cast<UOpenSplat4DPointCloud>(Context->SelectedAssets[0].GetAsset());
					if (Cloud)
				{
					Section.AddMenuEntry(
						"OpenSplat4D_CreateActor",
						OS4D_TEXT("Create OpenSplat4D Actor"),
						OS4D_TEXT("Spawn an OpenSplat4DPointCloudActor using this cloud"),
						FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda(
								[Cloud]()
								{
									if (GEditor)
									{
										UWorld* World = GEditor->GetEditorWorldContext().World();
										if (World)
										{
											AOpenSplat4DPointCloudActor* Actor = World->SpawnActor<AOpenSplat4DPointCloudActor>();
											if (Actor)
											{
												Actor->GaussianSplatComponent->SetSplatAsset(Cloud);
												GEditor->SelectActor(Actor, true, true);
											}
										}
									}
								})));

					// [DISABLED] 自研 ISMC 管线已停用，由 NanoGS 模块接管渲染。
				// Section.AddMenuEntry(
				// 	"OpenSplat4D_CreateSplatActor",
				// 	OS4D_TEXT("Create OpenSplat4D Splat Actor (主模式/自研管线)"),
				// 	OS4D_TEXT("使用此点云生成 SplatActor（基于 ISMC 的高 GPU 利用率自研渲染管线）"),
				// 	FSlateIcon(),
				// 	FUIAction(FExecuteAction::CreateLambda(
				// 		[Cloud]()
				// 		{
				// 			if (GEditor)
				// 			{
				// 				UWorld* World = GEditor->GetEditorWorldContext().World;
				// 				if (World)
				// 				{
				// 					AOpenSplat4DSplatActor* Actor = World->SpawnActor<AOpenSplat4DSplatActor>();
				// 					if (Actor)
				// 					{
				// 						Actor->GaussianSplatComponent->SetSplatAsset(Cloud);
				// 						GEditor->SelectActor(Actor, true, true);
				// 					}
				// 				}
				// 			}
				// 		})));
				}
				}
			}
		}));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FOpenSplat4DEditorModule, OpenSplat4DEditor)
