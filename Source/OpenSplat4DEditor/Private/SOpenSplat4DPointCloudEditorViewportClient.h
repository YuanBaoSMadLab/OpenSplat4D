#pragma once

#include "CoreMinimal.h"
#include "EditorViewportClient.h"

class FOpenSplat4DPointCloudEditor;

class FOpenSplat4DPointCloudEditorViewportClient : public FEditorViewportClient
{
public:
	using FEditorViewportClient::FEditorViewportClient;
	void SetEditor(TWeakPtr<FOpenSplat4DPointCloudEditor> InEditor);
	FOpenSplat4DPointCloudEditor* GetEditor();
protected:
	void Tick(float DeltaSeconds) override;
	void DrawCanvas(FViewport& InViewport, FSceneView& View, FCanvas& Canvas) override;
	bool InputKey(const FInputKeyEventArgs& EventArgs) override;
	TSharedPtr<FDragTool> MakeDragTool(EDragTool::Type DragToolType) override;

private:
	TWeakPtr<FOpenSplat4DPointCloudEditor> Editor;
};
