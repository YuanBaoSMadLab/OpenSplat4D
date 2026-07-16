#pragma once

#include "CoreMinimal.h"
#include "Widgets/SWidget.h"
#include "SEditorViewport.h"
#include "AssetEditorViewportLayout.h"
#include "OpenSplat4DBillboardComponent.h"
#include "OpenSplat4DPointCloudActor.h"
#include "SAssetEditorViewport.h"

class FEditorViewportClient;
class FViewportTabContent;
class FMenuBuilder;
class FAdvancedPreviewScene;
class FOpenSplat4DPointCloudEditor;

/**
 * Editor viewport for the OpenSplat4D point cloud asset editor. Hosts a preview
 * AOpenSplat4DPointCloudActor inside an FAdvancedPreviewScene so the self-contained
 * Billboard renderer draws the splats without needing a Niagara System asset.
 */
class SOpenSplat4DPointCloudEditorViewport : public SAssetEditorViewport
{
	SLATE_BEGIN_ARGS(SOpenSplat4DPointCloudEditorViewport) {}
		SLATE_ARGUMENT(TWeakPtr<FOpenSplat4DPointCloudEditor>, Editor)
	SLATE_END_ARGS()
public:
	void Construct(const FArguments& InArgs);
	UOpenSplat4DBillboardComponent* GetPreviewComponent() { return PreviewComponent; }
	TSharedRef<FAdvancedPreviewScene> GetPreviewScene() { return PreviewScene.ToSharedRef(); }
protected:
	virtual void BindCommands() override {}
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;
private:
	TWeakPtr<FOpenSplat4DPointCloudEditor> Editor;
	TSharedPtr<FAdvancedPreviewScene> PreviewScene;
	UOpenSplat4DBillboardComponent* PreviewComponent = nullptr;
	TObjectPtr<AOpenSplat4DPointCloudActor> PreviewActor = nullptr;
};
