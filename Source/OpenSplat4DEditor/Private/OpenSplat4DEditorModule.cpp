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
	// --- Register bilingual UI strings (Chinese by default) ----------------
	// Must run before any widget / asset action / editor mode is constructed,
	// so OS4D_TEXT(...) resolves to the Chinese translation immediately.
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
	OpenSplat4DLocalization::RegisterString(TEXT("Execution failed\n there is currently a running task"),
		TEXT("执行失败\n当前已有任务正在运行"));
	OpenSplat4DLocalization::RegisterString(TEXT("0. Create from Scene (Scan)"), TEXT("0. 从场景创建（扫描）"));
	OpenSplat4DLocalization::RegisterString(TEXT("Camera Depth Scan (off = Mesh Surface)"), TEXT("相机深度扫描（关闭 = 网格表面）"));
	OpenSplat4DLocalization::RegisterString(TEXT("Density"), TEXT("密度"));
	OpenSplat4DLocalization::RegisterString(TEXT("Point Size"), TEXT("点尺寸"));
	OpenSplat4DLocalization::RegisterString(TEXT("Scan Selected -> Point Cloud"), TEXT("扫描选中对象 → 点云"));
	OpenSplat4DLocalization::RegisterString(TEXT("Select one or more actors in the viewport, then scan. Mesh Surface samples static-mesh geometry directly; Camera Depth places a camera rig and reconstructs depth. No colmap / python needed -- you can create a point cloud and test rendering immediately."),
		TEXT("在视口中选择一个或多个Actor，然后扫描。网格表面模式直接采样静态网格几何体；相机深度模式放置相机阵列并重建深度。无需 colmap / python —— 你可以立即创建点云并测试渲染。"));
	OpenSplat4DLocalization::RegisterString(TEXT("1. Import"), TEXT("1. 导入"));
	OpenSplat4DLocalization::RegisterString(TEXT("Import .ply / .4dgs ..."), TEXT("导入 .ply / .4dgs ..."));
	OpenSplat4DLocalization::RegisterString(TEXT("Or drag a .ply / .4dgs file into the Content Browser. The asset is created under /Game/OpenSplat4D/."),
		TEXT("或将 .ply / .4dgs 文件拖入内容浏览器。资产将创建在 /Game/OpenSplat4D/ 下。"));
	OpenSplat4DLocalization::RegisterString(TEXT("2. Place in level"), TEXT("2. 放入关卡"));
	OpenSplat4DLocalization::RegisterString(TEXT("Spawn Actor in Level"), TEXT("在关卡中生成角色"));
	OpenSplat4DLocalization::RegisterString(TEXT("Or drag the asset from the Content Browser into the viewport. You can also right-click the asset -> 'Create OpenSplat4D Actor'."),
		TEXT("或将资产从内容浏览器拖入视口。你也可以右键资产 → “创建 OpenSplat4D 角色”。"));
	OpenSplat4DLocalization::RegisterString(TEXT("3. 4D Playback"), TEXT("3. 4D 播放"));
	OpenSplat4DLocalization::RegisterString(TEXT("Use Selected Actor"), TEXT("使用选中的角色"));
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
	// Scan-to-point-cloud feedback (shown as Slate notifications).
	OpenSplat4DLocalization::RegisterString(TEXT("ScanSelectFirst"),
		TEXT("请先在视口中选中一个或多个物体，然后再点击“扫描到点云”。"));
	OpenSplat4DLocalization::RegisterString(TEXT("ScanNoPoints"),
		TEXT("扫描未生成任何点。网格表面模式需选中含静态网格(StaticMesh)的物体；若选中的是角色/地形/蓝图，请改用“相机深度扫描”模式。"));
	OpenSplat4DLocalization::RegisterString(TEXT("ScanDone {0} {1}"),
		TEXT("扫描完成：{0} 个点，已保存为 UE 资产到 {1}（内容浏览器已定位；注意这是 .uasset 资产，不是捕获图片目录）。"));

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

	UE_LOG(LogOpenSplat4DEditor, Log, TEXT("OpenSplat4D editor module shut down."));
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
