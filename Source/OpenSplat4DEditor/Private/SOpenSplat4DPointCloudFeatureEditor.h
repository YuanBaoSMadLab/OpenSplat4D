#pragma once

#include "Widgets/SCompoundWidget.h"

class FOpenSplat4DPointCloudEditor;

class SOpenSplat4DPointCloudFeatureEditor : public SCompoundWidget
{
	SLATE_BEGIN_ARGS(SOpenSplat4DPointCloudFeatureEditor) {}
		SLATE_ARGUMENT(TWeakPtr<FOpenSplat4DPointCloudEditor>, Editor)
	SLATE_END_ARGS()
public:
	void Construct(const FArguments& InArgs);
	void ClearSelection();
protected:
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
private:
	TWeakPtr<FOpenSplat4DPointCloudEditor> Editor;
	TSharedPtr<class SOpenSplat4DPointCloudHistogram> Histogram;
};
