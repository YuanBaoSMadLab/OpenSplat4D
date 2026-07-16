#include "SOpenSplat4DPointCloudFeatureEditor.h"
#include "OpenSplat4DPointCloud.h"
#include "OpenSplat4DPointCloudEditor.h"

#define MAKE_PAINT_GEOMETRY_RC(Geometry, X, Y, W, H) Geometry.ToPaintGeometry(FVector2D(W, H), FSlateLayoutTransform(1.0f, FVector2D(X, Y)))

class SOpenSplat4DPointCloudHistogram : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SOpenSplat4DPointCloudHistogram) {}
		SLATE_ARGUMENT(TObjectPtr<UOpenSplat4DPointCloud>, PointCloud)
	SLATE_END_ARGS()
public:
	void Construct(const FArguments& InArgs)
	{
		PointCloud = InArgs._PointCloud;
		PointCloud->OnPointsChanged.AddSP(this, &SOpenSplat4DPointCloudHistogram::RefreshData);
		RefreshData();
		SetPixelSnapping(EWidgetPixelSnapping::Disabled);
	}

	int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
	{
		static const FSlateBrush* WhiteBrush = FAppStyle::GetBrush("WhiteBrush");
		const bool bEnabled = ShouldBeEnabled(bParentEnabled);
		const ESlateDrawEffect DrawEffects = bEnabled ? ESlateDrawEffect::NoPixelSnapping : ESlateDrawEffect::DisabledEffect;
		const int32 ViewWidth = AllottedGeometry.GetLocalSize().X;
		const int32 ViewHeight = AllottedGeometry.GetLocalSize().Y;
		const float BarWidth = ViewWidth / (float)HistogramData.Num();
		for (int32 i = 0; i < HistogramData.Num(); i++)
		{
			const int32 Count = HistogramData[i];
			const float Factor = FMath::Clamp(Count / (float)MaxBarCount, 0.0f, 1.0f);
			const float BarHeight = ViewHeight * Factor;
			const FLinearColor Color = SelectBar.Contains(i) ? FLinearColor(0.368f, 0.262f, 0.560f) : FLinearColor(0.1f, 0.5f, 0.9f);
			FSlateDrawElement::MakeBox(OutDrawElements, LayerId, MAKE_PAINT_GEOMETRY_RC(AllottedGeometry, i * BarWidth, ViewHeight - BarHeight, BarWidth - 1, BarHeight), WhiteBrush, DrawEffects, Color);
		}
		if (!MouseDownPosition.IsZero())
		{
			float StartPosX = MouseDownPosition.X;
			float EndPosX = MouseMovePosition.X;
			if (StartPosX > EndPosX)
			{
				Swap(StartPosX, EndPosX);
			}
			FSlateDrawElement::MakeBox(OutDrawElements, LayerId, MAKE_PAINT_GEOMETRY_RC(AllottedGeometry, StartPosX, 0, EndPosX - StartPosX, ViewHeight), WhiteBrush, DrawEffects, FLinearColor(0.368f, 0.262f, 0.560f, 0.5f));
		}
		return LayerId;
	}

	FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.IsMouseButtonDown(EKeys::LeftMouseButton))
		{
			MouseDownPosition = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
			MouseMovePosition = MouseDownPosition;
			return FReply::Handled().CaptureMouse(SharedThis(this));
		}
		return FReply::Handled();
	}

	FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.IsMouseButtonDown(EKeys::LeftMouseButton))
		{
			MouseMovePosition = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
		}
		return FReply::Handled();
	}

	FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (!MouseEvent.IsAltDown())
		{
			SelectBar.Reset();
		}
		const int32 ViewWidth = MyGeometry.GetLocalSize().X;
		float StartPosX = MouseDownPosition.X;
		float EndPosX = MouseMovePosition.X;
		if (StartPosX > EndPosX)
		{
			Swap(StartPosX, EndPosX);
		}
		const float BarWidth = ViewWidth / (float)HistogramData.Num();
		for (int32 i = 0; i < HistogramData.Num(); i++)
		{
			const float BarLeft = i * BarWidth;
			const float BarRight = BarLeft + BarWidth;
			if ((BarLeft >= StartPosX && BarLeft < EndPosX) || (BarRight >= StartPosX && BarRight < EndPosX))
			{
				SelectBar.Add(i);
			}
		}
		OnSelectChanged.ExecuteIfBound();
		MouseDownPosition = FVector2D::ZeroVector;
		MouseMovePosition = FVector2D::ZeroVector;
		return FReply::Handled().ReleaseMouseCapture();
	}

	void RefreshData()
	{
		const TArray<FOpenSplat4DPoint>& Points = PointCloud->GetPoints();
		HistogramData.Reset();
		SelectBar.Reset();
		MaxBarCount = 0;
		if (Points.IsEmpty())
		{
			return;
		}
		float MaxSize = 0.f;
		float MinSize = MAX_flt;
		for (const FOpenSplat4DPoint& P : Points)
		{
			const float S = P.Scale.Length();
			MaxSize = FMath::Max(MaxSize, S);
			MinSize = FMath::Min(MinSize, S);
		}
		if (MaxSize <= MinSize)
		{
			MaxSize = MinSize + 1e-3f;
		}

		HistogramData.AddZeroed(FMath::Min(NumOfBar, Points.Num()));
		for (int32 i = 0; i < Points.Num(); i++)
		{
			const float Size = Points[i].Scale.Length();
			int32 Index = FMath::Clamp((int32)(((Size - MinSize) / (MaxSize - MinSize)) * (HistogramData.Num() - 1)), 0, HistogramData.Num() - 1);
			HistogramData[Index]++;
			MaxBarCount = FMath::Max(MaxBarCount, HistogramData[Index]);
		}
	}

	TArray<uint32> GetSelectIndices()
	{
		TArray<uint32> Indices;
		if (SelectBar.IsEmpty())
		{
			return Indices;
		}
		const TArray<FOpenSplat4DPoint>& Points = PointCloud->GetPoints();
		int32 StartIndex = 0;
		for (int32 i = HistogramData.Num() - 1; i >= 0; i--)
		{
			const int32 BarCount = HistogramData[i];
			if (SelectBar.Contains(i))
			{
				for (int32 j = 0; j < BarCount; j++)
				{
					Indices.Add(StartIndex + j);
				}
			}
			StartIndex += BarCount;
		}
		Indices.Sort();
		return Indices;
	}

public:
	TObjectPtr<UOpenSplat4DPointCloud> PointCloud;
	const int32 NumOfBar = 128;
	TArray<int32> HistogramData;
	TSet<uint32> SelectBar;
	int32 MaxBarCount = 0;
	FVector2D MouseDownPosition = FVector2D::ZeroVector;
	FVector2D MouseMovePosition = FVector2D::ZeroVector;
	FSimpleDelegate OnSelectChanged;
};

void SOpenSplat4DPointCloudFeatureEditor::Construct(const FArguments& InArgs)
{
	Editor = InArgs._Editor;

	ChildSlot
		[
			SNew(SBox)
			.HeightOverride(100)
			[
				SAssignNew(Histogram, SOpenSplat4DPointCloudHistogram)
				.PointCloud(Editor.Pin()->PointCloud)
			]
		];

	Histogram->OnSelectChanged.BindSPLambda(this, [this]()
	{
		if (TSharedPtr<FOpenSplat4DPointCloudEditor> EditorPtr = Editor.Pin())
		{
			EditorPtr->SelectPointsByIndex(Histogram->GetSelectIndices());
		}
	});
}

void SOpenSplat4DPointCloudFeatureEditor::ClearSelection()
{
	if (Histogram.IsValid())
	{
		Histogram->SelectBar.Reset();
	}
}

FReply SOpenSplat4DPointCloudFeatureEditor::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Delete)
	{
		if (TSharedPtr<FOpenSplat4DPointCloudEditor> EditorPtr = Editor.Pin())
		{
			EditorPtr->RemoveSelectedPoints();
		}
		return FReply::Handled();
	}
	return FReply::Unhandled();
}
