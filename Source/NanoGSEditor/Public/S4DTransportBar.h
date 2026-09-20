// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class UGaussianSplatAsset;
class UGaussianSplatComponent;
class SSlider;
class STextBlock;
class SButton;

/**
 * 4D 播放器底部控制条（类似视频/动画播放器 transport bar）：
 *
 *   [|◀ 上一帧] [▶ 播放 / ⏸ 暂停] [■ 停止] [▶| 下一帧]
 *   ├──────────────●──────────────┤
 *   t = 0.342 [−1.0, 1.0] (32%)
 *   [☑ 循环]  [速度 ×1.0 ▾]
 *
 * 直接驱动预览视口的 UGaussianSplatComponent（Play4D/Pause4D/Stop4D/
 * SetPlaybackTime），渲染管线零改动。资产不含 4D 数据时整条灰显并提示。
 */
class S4DTransportBar : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(S4DTransportBar) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, UGaussianSplatAsset* InAsset, UGaussianSplatComponent* InComponent);

	/** 预览 actor 重建后重新绑定组件（Nanite 切换等场景） */
	void BindComponent(UGaussianSplatAsset* InAsset, UGaussianSplatComponent* InComponent);

	//~ SWidget：每帧同步时间滑块/时间码/播放按钮文案（组件在 Tick 驱动 CurrentTime）
	virtual void Tick(const FGeometry& AllottedGeometry, double InCurrentTime, float InDeltaTime) override;

private:
	UGaussianSplatAsset* GetAsset() const;
	UGaussianSplatComponent* GetComponent() const;
	float GetStepSize() const;
	bool Is4DAvailable() const;

	FReply OnPrevFrameClicked();
	FReply OnPlayPauseClicked();
	FReply OnStopClicked();
	FReply OnNextFrameClicked();
	void OnSliderValueChanged(float NewValue);
	void OnSliderCaptureBegin();
	void OnSliderCaptureEnd();
	void OnLoopToggled(ECheckBoxState NewState);
	void OnSpeedPicked(TSharedPtr<float> InSpeed, ESelectInfo::Type SelectInfo);
	FText GetTimecodeText() const;

	/** 数据源 */
	TWeakObjectPtr<UGaussianSplatAsset> Asset;
	TWeakObjectPtr<UGaussianSplatComponent> Component;

	/** 缓存的子控件引用（Tick 中更新） */
	TSharedPtr<SSlider> TimeSlider;
	TSharedPtr<STextBlock> TimecodeText;
	TSharedPtr<STextBlock> PlayPauseLabel;
	TSharedPtr<SButton> PlayPauseButton;

	/** 拖动滑块前是否在播放（松手后恢复） */
	bool bPlayingBeforeScrub = false;

	/** 是否正在拖动时间轴（Tick 期间不覆盖滑块值，让用户控值优先） */
	bool bScrubbing = false;

	/** 当前选中的播放速度（下拉框显示用） */
	TSharedPtr<float> SelectedSpeed;
};
