// Copyright Epic Games, Inc. All Rights Reserved.

#include "S4DTransportBar.h"
#include "GaussianSplatAsset.h"
#include "NanoGSGaussianSplatComponent.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "S4DTransportBar"

void S4DTransportBar::Construct(const FArguments& InArgs, UGaussianSplatAsset* InAsset, UGaussianSplatComponent* InComponent)
{
	Asset = InAsset;
	Component = InComponent;

	if (!Is4DAvailable())
	{
		// 3D 资产/无时间数据：整条灰显提示，不提供任何控件
		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Toolbox.GroupBorder"))
			.Padding(8.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("No4DData", "此资产不含 4D 时序数据（导入带 t / scale_t 属性的 4DGS PLY 后可用播放器）"))
				.ColorAndOpacity(FSlateColor(FLinearColor::Gray))
			]
		];
		return;
	}

	// 速度选项：0.25 / 0.5 / 1 / 2 / 4
	const TArray<float> Speeds = { 0.25f, 0.5f, 1.0f, 2.0f, 4.0f };
	TArray<TSharedPtr<float>> SpeedOptions;
	for (float S : Speeds)
	{
		SpeedOptions.Add(MakeShared<float>(S));
	}
	SelectedSpeed = MakeShared<float>(GetComponent() ? GetComponent()->PlayRate : 1.0f);

	const FSlateFontInfo ButtonFont = FAppStyle::GetFontStyle("NormalFont");

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Toolbox.GroupBorder"))
		.Padding(FMargin(8.0f, 4.0f))
		[
			SNew(SVerticalBox)

			// ---------------------------------------------------------------
			// 第一行：传输按钮 + 时间码
			// ---------------------------------------------------------------
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f)
			[
				SNew(SHorizontalBox)

				// |◀ 上一帧
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("PrevFrame", "|◀"))
					.ToolTipText(LOCTEXT("PrevFrameTooltip", "上一帧（自动暂停）"))
					.OnClicked(this, &S4DTransportBar::OnPrevFrameClicked)
				]

				// ▶ 播放 / ⏸ 暂停
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.0f, 0.0f)
				[
					SAssignNew(PlayPauseButton, SButton)
					.OnClicked(this, &S4DTransportBar::OnPlayPauseClicked)
					.ToolTipText(LOCTEXT("PlayPauseTooltip", "播放 / 暂停"))
					[
						SAssignNew(PlayPauseLabel, STextBlock)
						.Font(ButtonFont)
						.Text(LOCTEXT("Play", "▶ 播放"))
					]
				]

				// ■ 停止
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("Stop", "■ 停止"))
					.ToolTipText(LOCTEXT("StopTooltip", "停止并回到时间轴起点"))
					.OnClicked(this, &S4DTransportBar::OnStopClicked)
				]

				// ▶| 下一帧
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("NextFrame", "▶|"))
					.ToolTipText(LOCTEXT("NextFrameTooltip", "下一帧（自动暂停）"))
					.OnClicked(this, &S4DTransportBar::OnNextFrameClicked)
				]

				// 时间码（右侧伸展）
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				.Padding(16.0f, 0.0f)
				[
					SAssignNew(TimecodeText, STextBlock)
					.Font(ButtonFont)
					.Text(this, &S4DTransportBar::GetTimecodeText)
				]
			]

			// ---------------------------------------------------------------
			// 第二行：时间轴滑条
			// ---------------------------------------------------------------
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f)
			[
				SAssignNew(TimeSlider, SSlider)
				.MinValue(0.0f)
				.MaxValue(1.0f)
				.Value(0.0f)
				.OnValueChanged(this, &S4DTransportBar::OnSliderValueChanged)
				.OnMouseCaptureBegin(this, &S4DTransportBar::OnSliderCaptureBegin)
				.OnMouseCaptureEnd(this, &S4DTransportBar::OnSliderCaptureEnd)
			]

			// ---------------------------------------------------------------
			// 第三行：循环 + 播放速度
			// ---------------------------------------------------------------
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 2.0f)
			[
				SNew(SHorizontalBox)

				// 循环开关
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.IsChecked(GetComponent() && GetComponent()->bLooping ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
					.OnCheckStateChanged(this, &S4DTransportBar::OnLoopToggled)
					[
						SNew(STextBlock)
						.Font(ButtonFont)
						.Text(LOCTEXT("Looping", "循环"))
					]
				]

				// 播放速度下拉
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(16.0f, 0.0f)
				[
					SNew(SComboBox<TSharedPtr<float>>)
					.OptionsSource(&SpeedOptions)
					.InitiallySelectedItem(SelectedSpeed)
					.OnSelectionChanged(this, &S4DTransportBar::OnSpeedPicked)
					[
						SNew(STextBlock)
						.Font(ButtonFont)
						.Text(LOCTEXT("Speed", "播放速度"))
					]
					.OnGenerateWidget_Lambda([](TSharedPtr<float> InSpeed)
					{
						return SNew(STextBlock)
							.Text(FText::FromString(FString::Printf(TEXT("速度 ×%.2f"), *InSpeed)));
					})
					.Content()
					[
						SNew(STextBlock)
						.Font(ButtonFont)
						.Text_Lambda([this]()
						{
							return FText::FromString(FString::Printf(TEXT("速度 ×%.2f"), SelectedSpeed.IsValid() ? *SelectedSpeed : 1.0f));
						})
					]
				]
			]
		]
	];
}

void S4DTransportBar::BindComponent(UGaussianSplatAsset* InAsset, UGaussianSplatComponent* InComponent)
{
	Asset = InAsset;
	Component = InComponent;
}

void S4DTransportBar::Tick(const FGeometry& AllottedGeometry, double InCurrentTime, float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

	UGaussianSplatComponent* Comp = GetComponent();
	if (!Comp || !TimeSlider.IsValid() || !TimecodeText.IsValid() || !PlayPauseLabel.IsValid())
	{
		return;
	}

	UGaussianSplatAsset* Splats = GetAsset();
	const float TimeStart = Splats ? Splats->TimeStart : 0.0f;
	const float TimeEnd = Splats ? Splats->TimeEnd : 1.0f;
	const float Duration = TimeEnd - TimeStart;

	// 滑块位置：0..1 映射 TimeStart..TimeEnd（拖动中让用户控值优先）
	if (!bScrubbing && Duration > 0.0f)
	{
		TimeSlider->SetValue(FMath::GetMappedRangeValueClamped(FVector2f(TimeStart, TimeEnd), FVector2f(0.0f, 1.0f), Comp->CurrentTime));
	}

	// 播放按钮文案随状态切换
	PlayPauseLabel->SetText(Comp->bPlaying ? LOCTEXT("Pause", "⏸ 暂停") : LOCTEXT("Play", "▶ 播放"));
}

UGaussianSplatAsset* S4DTransportBar::GetAsset() const
{
	return Asset.Get();
}

UGaussianSplatComponent* S4DTransportBar::GetComponent() const
{
	return Component.Get();
}

float S4DTransportBar::GetStepSize() const
{
	const UGaussianSplatAsset* Splats = GetAsset();
	if (!Splats || Splats->TimeEnd <= Splats->TimeStart)
	{
		return 0.0f;
	}
	// 整段时间约 120 步
	return (Splats->TimeEnd - Splats->TimeStart) / 120.0f;
}

bool S4DTransportBar::Is4DAvailable() const
{
	const UGaussianSplatComponent* Comp = GetComponent();
	return Comp && Comp->Supports4DPlayback();
}

FReply S4DTransportBar::OnPrevFrameClicked()
{
	UGaussianSplatComponent* Comp = GetComponent();
	if (Comp)
	{
		Comp->Pause4D();
		Comp->SetPlaybackTime(Comp->CurrentTime - GetStepSize());
	}
	return FReply::Handled();
}

FReply S4DTransportBar::OnPlayPauseClicked()
{
	UGaussianSplatComponent* Comp = GetComponent();
	if (Comp)
	{
		if (Comp->bPlaying)
		{
			Comp->Pause4D();
		}
		else
		{
			// 播完（非循环停在终点）后再按播放 → 从头开始
			const UGaussianSplatAsset* Splats = GetAsset();
			if (Splats && !Comp->bLooping && Comp->CurrentTime >= Splats->TimeEnd)
			{
				Comp->SetPlaybackTime(Splats->TimeStart);
			}
			Comp->Play4D();
		}
	}
	return FReply::Handled();
}

FReply S4DTransportBar::OnStopClicked()
{
	UGaussianSplatComponent* Comp = GetComponent();
	if (Comp)
	{
		Comp->Stop4D();
	}
	return FReply::Handled();
}

FReply S4DTransportBar::OnNextFrameClicked()
{
	UGaussianSplatComponent* Comp = GetComponent();
	if (Comp)
	{
		Comp->Pause4D();
		Comp->SetPlaybackTime(Comp->CurrentTime + GetStepSize());
	}
	return FReply::Handled();
}

void S4DTransportBar::OnSliderValueChanged(float NewValue)
{
	// 拖动中直接 scrub：值已在 CaptureBegin 暂停后由 SetPlaybackTime 驱动画面
	if (UGaussianSplatComponent* Comp = GetComponent())
	{
		const UGaussianSplatAsset* Splats = GetAsset();
		const float TimeStart = Splats ? Splats->TimeStart : 0.0f;
		const float TimeEnd = Splats ? Splats->TimeEnd : 0.0f;
		Comp->SetPlaybackTime(FMath::Lerp(TimeStart, TimeEnd, NewValue));
	}
}

void S4DTransportBar::OnSliderCaptureBegin()
{
	UGaussianSplatComponent* Comp = GetComponent();
	if (Comp)
	{
		bScrubbing = true;
		bPlayingBeforeScrub = Comp->bPlaying;
		Comp->Pause4D();
	}
}

void S4DTransportBar::OnSliderCaptureEnd()
{
	bScrubbing = false;
	UGaussianSplatComponent* Comp = GetComponent();
	if (Comp && bPlayingBeforeScrub)
	{
		Comp->Play4D();
	}
	bPlayingBeforeScrub = false;
}

void S4DTransportBar::OnLoopToggled(ECheckBoxState NewState)
{
	UGaussianSplatComponent* Comp = GetComponent();
	if (Comp)
	{
		Comp->bLooping = (NewState == ECheckBoxState::Checked);
	}
}

void S4DTransportBar::OnSpeedPicked(TSharedPtr<float> InSpeed, ESelectInfo::Type SelectInfo)
{
	UGaussianSplatComponent* Comp = GetComponent();
	if (Comp && InSpeed.IsValid())
	{
		Comp->PlayRate = *InSpeed;
	}
	SelectedSpeed = InSpeed;
}

FText S4DTransportBar::GetTimecodeText() const
{
	const UGaussianSplatComponent* Comp = GetComponent();
	const UGaussianSplatAsset* Splats = GetAsset();
	if (!Comp || !Splats)
	{
		return FText::GetEmpty();
	}

	const float Duration = Splats->TimeEnd - Splats->TimeStart;
	const int32 Percent = Duration > 0.0f ? FMath::RoundToInt(100.0f * (Comp->CurrentTime - Splats->TimeStart) / Duration) : 0;
	return FText::FromString(FString::Printf(TEXT("t = %.3f  [%.2f, %.2f]  (%d%%)"),
		Comp->CurrentTime, Splats->TimeStart, Splats->TimeEnd, Percent));
}

#undef LOCTEXT_NAMESPACE
