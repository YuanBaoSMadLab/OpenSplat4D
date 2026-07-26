#pragma once

#include "CoreMinimal.h"
#include "Widgets/SWidget.h"
#include "SEditorViewport.h"
#include "AssetEditorViewportLayout.h"
#include "OpenSplat4DSplatActor.h"
#include "SAssetEditorViewport.h"

class FEditorViewportClient;
class FViewportTabContent;
class FMenuBuilder;
class FAdvancedPreviewScene;
class FOpenSplat4DPointCloudEditor;

/**
 * Editor viewport for the OpenSplat4D point cloud asset editor. Hosts a preview
 * AOpenSplat4DSplatActor inside an FAdvancedPreviewScene. The actor's
 * UInstancedStaticMeshComponent renders the splats (自研管线，不走 Niagara)。
 *
 * [DISABLED] Niagara 路径已禁用，资产编辑器预览改用自研 SplatActor。
 */
class SOpenSplat4DPointCloudEditorViewport : public SAssetEditorViewport
{
	SLATE_BEGIN_ARGS(SOpenSplat4DPointCloudEditorViewport) {}
		SLATE_ARGUMENT(TWeakPtr<FOpenSplat4DPointCloudEditor>, Editor)
	SLATE_END_ARGS()
public:
	void Construct(const FArguments& InArgs);
	TSharedRef<FAdvancedPreviewScene> GetPreviewScene() { return PreviewScene.ToSharedRef(); }

	/** 获取预览 SplatActor（自研管线，ISMC 渲染） */
	AOpenSplat4DSplatActor* GetPreviewSplatActor() { return PreviewActor.Get(); }

protected:
	virtual void BindCommands() override {}
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;
private:
	TWeakPtr<FOpenSplat4DPointCloudEditor> Editor;
	TSharedPtr<FAdvancedPreviewScene> PreviewScene;
	// [DISABLED] 旧 NiagaraComponent 引用已移除，改用 SplatActor 的 ISMC
	TWeakObjectPtr<AOpenSplat4DSplatActor> PreviewActor;
};
