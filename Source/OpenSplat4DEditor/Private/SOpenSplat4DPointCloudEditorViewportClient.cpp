#include "SOpenSplat4DPointCloudEditorViewportClient.h"
#include "SOpenSplat4DPointCloudEditorViewport.h"
#include "OpenSplat4DPointCloudEditor.h"
#include "OpenSplat4DPointCloud.h"
#include "OpenSplat4DSplatActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "CanvasTypes.h"
#include "CanvasItem.h"
#include "EditorDragTools.h"
#include "AdvancedPreviewScene.h"
#include "SceneView.h"

/**
 * Frustum/box select drag tool: projects every point through the preview
 * component's model-view-projection matrix and collects the ones whose screen
 * position falls inside the dragged rectangle. Selection is then forwarded to
 * the editor (used for deletion). Adapted from GaussianSplattingForUnrealEngine.
 *
 * [DISABLED] Niagara 路径已禁用：改用 SplatActor 的 ISMC transform 计算 MVP。
 */
class FDragTool_PointsFrustumSelect : public FDragTool
{
public:
	explicit FDragTool_PointsFrustumSelect(FOpenSplat4DPointCloudEditorViewportClient* InViewportClient)
		: FDragTool(InViewportClient->GetModeTools())
		, ViewportClient(InViewportClient)
	{
	}

	virtual void AddDelta(const FVector& InDelta) override
	{
		FIntPoint MousePos;
		ViewportClient->Viewport->GetMousePos(MousePos);

		EndWk = FVector(MousePos);
		End = EndWk;
	}

	virtual void StartDrag(FEditorViewportClient* InViewportClient, const FVector& InStart, const FVector2D& InStartScreen) override
	{
		FDragTool::StartDrag(InViewportClient, InStart, InStartScreen);

		FIntPoint MousePos;
		InViewportClient->Viewport->GetMousePos(MousePos);

		Start = FVector(InStartScreen.X, InStartScreen.Y, 0);
		End = EndWk = Start;
	}

	virtual void EndDrag() override
	{
		const int32 ViewportSizeX = ViewportClient->Viewport->GetSizeXY().X;
		const int32 ViewportSizeY = ViewportClient->Viewport->GetSizeXY().Y;
		if (Start.X > End.X)
		{
			Swap(Start.X, End.X);
		}
		if (Start.Y > End.Y)
		{
			Swap(Start.Y, End.Y);
		}

		FOpenSplat4DPointCloudEditor* Editor = ViewportClient->GetEditor();
		TSharedPtr<SOpenSplat4DPointCloudEditorViewport> Viewport = Editor->GetViewport();
		// [DISABLED] Niagara 路径：改用 SplatActor 的 ISMC transform
		AOpenSplat4DSplatActor* PreviewSplatActor = Viewport->GetPreviewSplatActor();
		USceneComponent* PreviewComp = PreviewSplatActor ? PreviewSplatActor->GetSplatComponent() : nullptr;
		UOpenSplat4DPointCloud* PointCloud = Editor->PointCloud;
		const TArray<FOpenSplat4DPoint>& Points = PointCloud->GetPoints();

		FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(ViewportClient->Viewport, Viewport->GetPreviewScene()->GetScene(), ViewportClient->EngineShowFlags));
		FSceneView* View = ViewportClient->CalcSceneView(&ViewFamily);
		const FMatrix ModelMatrix = PreviewComp ? PreviewComp->GetComponentTransform().ToMatrixWithScale() : FMatrix::Identity;
		const FMatrix MVPMatrix = ModelMatrix * View->ViewMatrices.GetWorldToClip();

		TArray<uint32> SelectedIndices;
		for (int32 i = 0; i < Points.Num(); i++)
		{
			FVector4 TransformedPoint = MVPMatrix.TransformFVector4(FVector4(FVector(Points[i].Position), 1.f));
			if (TransformedPoint.W != 0.0f)
			{
				TransformedPoint /= TransformedPoint.W;
			}
			FVector2D ViewportPoint;
			ViewportPoint.X = (TransformedPoint.X + 1.0f) * 0.5f * ViewportSizeX;
			ViewportPoint.Y = (1.0f - (TransformedPoint.Y + 1.0f) * 0.5f) * ViewportSizeY;
			bool bIsInside = (ViewportPoint.X >= Start.X) && (ViewportPoint.X <= End.X) &&
				(ViewportPoint.Y >= Start.Y) && (ViewportPoint.Y <= End.Y);
			if (bIsInside)
			{
				SelectedIndices.Add(i);
			}
		}
		Editor->ClearPropertyEditorSelection();
		Editor->SelectPointsByIndex(SelectedIndices);
		FDragTool::EndDrag();
	}

	virtual void Render(const FSceneView* View, FCanvas* Canvas) override
	{
		FCanvasBoxItem BoxItem(FVector2D(Start.X, Start.Y) / Canvas->GetDPIScale(), FVector2D(End.X - Start.X, End.Y - Start.Y) / Canvas->GetDPIScale());
		BoxItem.SetColor(FLinearColor::White);
		Canvas->DrawItem(BoxItem);
	}
private:
	FOpenSplat4DPointCloudEditorViewportClient* ViewportClient;
};


void FOpenSplat4DPointCloudEditorViewportClient::SetEditor(TWeakPtr<FOpenSplat4DPointCloudEditor> InEditor)
{
	Editor = InEditor;
}

FOpenSplat4DPointCloudEditor* FOpenSplat4DPointCloudEditorViewportClient::GetEditor()
{
	return Editor.Pin().Get();
}

void FOpenSplat4DPointCloudEditorViewportClient::Tick(float DeltaSeconds)
{
	FEditorViewportClient::Tick(DeltaSeconds);
	if (PreviewScene)
	{
		PreviewScene->GetWorld()->Tick(LEVELTICK_All, DeltaSeconds);
	}
}

void FOpenSplat4DPointCloudEditorViewportClient::DrawCanvas(FViewport& InViewport, FSceneView& View, FCanvas& Canvas)
{
	FEditorViewportClient::DrawCanvas(InViewport, View, Canvas);
}

bool FOpenSplat4DPointCloudEditorViewportClient::InputKey(const FInputKeyEventArgs& EventArgs)
{
	if (EventArgs.Key == EKeys::Delete && EventArgs.Event == IE_Pressed)
	{
		if (TSharedPtr<FOpenSplat4DPointCloudEditor> EditorPtr = Editor.Pin())
		{
			EditorPtr->RemoveSelectedPoints();
		}
		return true;
	}
	return FEditorViewportClient::InputKey(EventArgs);
}

TSharedPtr<FDragTool> FOpenSplat4DPointCloudEditorViewportClient::MakeDragTool(EDragTool::Type DragToolType)
{
	TSharedPtr<FDragTool> DragTool;
	switch (DragToolType)
	{
	case EDragTool::BoxSelect:
		DragTool = MakeShareable(new FDragTool_PointsFrustumSelect(this));
		break;
	case EDragTool::FrustumSelect:
		DragTool = MakeShareable(new FDragTool_PointsFrustumSelect(this));
		break;
	case EDragTool::Measure:
		break;
	case EDragTool::ViewportChange:
		break;
	};
	return DragTool;
}
