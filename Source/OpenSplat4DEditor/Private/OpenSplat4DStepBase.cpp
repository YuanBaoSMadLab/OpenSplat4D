#include "OpenSplat4DStep.h"
#include "OpenSplat4DSettings.h"

#include <string>

#include "Misc/FileHelper.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"

#include "Editor.h"
#include "Selection.h"
#include "GameFramework/Actor.h"

// --- Headers required to faithfully drive the engine / render thread during an
//     offline ("fake") tick, ported from the reference GaussianSplattingEditor.
#include "Engine/Engine.h"
#include "Misc/App.h"
#include "AssetCompilingManager.h"
#include "Tickable.h"
#include "Misc/CoreDelegates.h"
#include "DynamicResolutionState.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/ThreadManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"

DEFINE_LOG_CATEGORY(LogOpenSplat4DStep);

	FString OpenSplat4DQuoteArg(const FString& Arg)
{
	if (Arg.Contains(TEXT(" ")) || Arg.Contains(TEXT("\t")))
	{
		return FString::Printf(TEXT("\"%s\""), *Arg);
	}
	return Arg;
}

	// Inverse of OpenSplat4DQuoteArg: remove a single pair of surrounding double
	// quotes. Used for the executable path we hand to CreateProc, which must not
	// be quoted (see ExecuteCommand / FCommandExecuteRunnable::Run).
	FString OpenSplat4DUnquoteArg(const FString& Arg)
{
	if (Arg.Len() >= 2 && Arg.StartsWith(TEXT("\"")) && Arg.EndsWith(TEXT("\"")))
	{
		return Arg.Mid(1, Arg.Len() - 2);
	}
	return Arg;
}

	bool OpenSplat4DIsExecutableResolvable(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return false;
		}
		// Absolute or relative path that exists on disk (handles "C:/Program Files/Colmap/colmap.exe").
		if (FPaths::FileExists(Path))
		{
			return true;
		}
		// Bare command name (e.g. "colmap" / "python"): probe the system PATH via "where".
		if (!Path.Contains(TEXT("/")) && !Path.Contains(TEXT("\\")))
		{
			FString StdOut, StdErr;
			int32 ReturnCode = -1;
			if (FPlatformProcess::ExecProcess(TEXT("where"), *Path, &ReturnCode, &StdOut, &StdErr) && ReturnCode == 0 && !StdOut.TrimStartAndEnd().IsEmpty())
			{
				return true;
			}
		}
		return false;
	}

	    // --- Capture set as a native UE DataAsset ----------------------------

    static FString SanitizeSetName(const FString& In)
    {
        FString Out = In;
        for (TCHAR& C : Out)
        {
            if (!FChar::IsAlnum(C) && C != TEXT('_') && C != TEXT('-') && C != TEXT('.'))
            {
                C = TEXT('_');
            }
        }
        return Out;
    }

    static void SaveCaptureSetPackage(UPackage* Package, UObject* Asset)
    {
        const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
        const FString Dir = FPaths::GetPath(FileName);
        IFileManager::Get().MakeDirectory(*Dir, true);
        FSavePackageArgs Args;
        Args.TopLevelFlags = RF_Standalone;
        Args.Error = GError;
        Args.bWarnOfLongFilename = true;
        UPackage::SavePackage(Package, Asset, *FileName, Args);
    }

    UOpenSplat4DCaptureSet* OpenSplat4DCreateCaptureSet(const FString& SetName, const FString& WorkDir)
    {
        const FString Safe = SanitizeSetName(SetName);
        const FString PackagePath = TEXT("/Game/OpenSplat4D/Captures/") + Safe;
        UPackage* Package = CreatePackage(*PackagePath);
        if (!Package)
        {
            UE_LOG(LogOpenSplat4DStep, Error, TEXT("Failed to create capture-set package: %s"), *PackagePath);
            return nullptr;
        }
        Package->FullyLoad();
        const FString AssetName = FPaths::GetBaseFilename(PackagePath);
        UOpenSplat4DCaptureSet* Asset = FindObject<UOpenSplat4DCaptureSet>(Package, *AssetName);
        if (!Asset)
        {
            Asset = NewObject<UOpenSplat4DCaptureSet>(Package, *AssetName, RF_Public | RF_Standalone);
        }
        Asset->SetName = SetName;
        Asset->WorkDirectory = WorkDir;
        Asset->CapturedAt = FDateTime::Now();
        Asset->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(Asset);
        SaveCaptureSetPackage(Package, Asset);
        UE_LOG(LogOpenSplat4DStep, Log, TEXT("Capture set asset created: %s (%s)"), *Asset->GetPathName(), *WorkDir);
        return Asset;
    }

	void OpenSplat4DUpdateCaptureSet(UOpenSplat4DCaptureSet* Asset, const FString& WorkDir,
		int32 ImageCount)
    {
        if (!Asset)
        {
            return;
        }
        Asset->WorkDirectory = WorkDir;
        Asset->ImageCount = ImageCount;
        Asset->CapturedAt = FDateTime::Now();
        Asset->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(Asset);
        SaveCaptureSetPackage(Asset->GetOutermost(), Asset);
        UE_LOG(LogOpenSplat4DStep, Log, TEXT("Capture set asset updated: %s (%d images)"), *Asset->GetPathName(), ImageCount);
    }

    void OpenSplat4DEnumerateCaptureSets(TArray<UOpenSplat4DCaptureSet*>& OutSets)
    {
        OutSets.Reset();
        FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        IAssetRegistry& AR = ARM.Get();
        FARFilter Filter;
        Filter.ClassPaths.Add(UOpenSplat4DCaptureSet::StaticClass()->GetClassPathName());
        Filter.PackagePaths.Add(FName(TEXT("/Game/OpenSplat4D/Captures")));
        Filter.bRecursivePaths = true;
        TArray<FAssetData> Assets;
        AR.GetAssets(Filter, Assets);
        for (const FAssetData& A : Assets)
        {
            if (UOpenSplat4DCaptureSet* C = Cast<UOpenSplat4DCaptureSet>(A.GetAsset()))
            {
                OutSets.Add(C);
            }
        }
    }

    // Build a capture-set name from the actor(s) selected in the editor. The
    // subject label keeps the capture tied to what was scanned; the sequence
    // number + timestamp stop two captures of the same subject from colliding.
    FString OpenSplat4DBuildCaptureSetName()
    {
        FString BaseName;
        if (GEditor)
        {
            USelection* SelectedActors = GEditor->GetSelectedActors();
            for (int32 SelIdx = 0; SelIdx < SelectedActors->Num(); ++SelIdx)
            {
                if (AActor* A = Cast<AActor>(SelectedActors->GetSelectedObject(SelIdx)))
                {
                    if (A->HasAnyFlags(RF_Transient))
                    {
                        continue;
                    }
                    const FString Label = A->GetActorLabel();
                    BaseName = Label.IsEmpty() ? A->GetName() : Label;
                    break;
                }
            }
        }
        if (BaseName.IsEmpty())
        {
            BaseName = TEXT("Capture");
        }
        BaseName = SanitizeSetName(BaseName);

        const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));

        int32 Seq = 1;
        TArray<UOpenSplat4DCaptureSet*> Existing;
        OpenSplat4DEnumerateCaptureSets(Existing);
        for (const UOpenSplat4DCaptureSet* S : Existing)
        {
            if (S && S->SetName.StartsWith(BaseName + TEXT("_")))
            {
                ++Seq;
            }
        }
        return FString::Printf(TEXT("%s_%02d_%s"), *BaseName, Seq, *Timestamp);
    }

// Local helpers (ported from the reference plugin's editor library)
//
// FakeEngineTick: advance the engine + render thread a fixed number of times so
// that offline capture / HLOD generation actually produces real pixels and
// finished assets. The previous OpenSplat4D implementation only called
//   InWorld->Tick(LEVELTICK_ViewportsOnly, InDelta)
// which is NOT enough: it never flushes the render thread and never waits for
// async texture/mesh compilation, so HLOD / headless scene capture could read
// empty render targets. The implementation below mirrors the reference
// GaussianSplattingEditor::FakeEngineTick exactly (commandlet path + Slate path
// with BeginFrame/EndFrame render commands and FinishAllCompilation). The
// commandlet branch is adapted for UE 5.8 (CommandletHelpers::TickEngine was
// removed), but produces the same offline-tick behaviour.
// ----------------------------------------------------------------------------
void FakeEngineTick(UWorld* InWorld, float InDelta /*= 0.03f*/, int InCount /*= 1*/)
{
	if (!InWorld)
	{
		return;
	}
	for (int i = 0; i < InCount; ++i)
	{
		if (IsRunningCommandlet())
		{
			// --- Headless / commandlet path ---
			// NOTE: the reference plugin used CommandletHelpers::TickEngine(), but
			// that helper no longer exists in UE 5.8's CommandletHelpers namespace
			// (it only exposes BuildCommandletProcessArguments). We reproduce the
			// same headless tick by driving GEngine + the world directly.
			GEngine->EmitDynamicResolutionEvent(EDynamicResolutionStateEvent::EndFrame);
			FApp::SetDeltaTime(InDelta);
			GEngine->Tick(FApp::GetDeltaTime(), false);
			InWorld->Tick(LEVELTICK_All, InDelta);
			GEngine->EmitDynamicResolutionEvent(EDynamicResolutionStateEvent::EndFrame);
		}
		else if (FSlateApplication::IsInitialized())
		{
			// --- Editor path: drive the full engine + render thread ----------
			FApp::SetDeltaTime(InDelta);
			const bool bIsTicking = FSlateApplication::Get().IsTicking();
			GEngine->EmitDynamicResolutionEvent(EDynamicResolutionStateEvent::EndFrame);
			GEngine->Tick(FApp::GetDeltaTime(), false);
			FSlateApplication::Get().PumpMessages();
			FSlateApplication::Get().Tick();
			GFrameCounter++;

			// If Slate is not already pumping frames, manually bracket the render
			// thread with BeginFrame/EndFrame and flush so capture render targets
			// are resolved. This is the key fix vs. the old simplified tick.
			if (!bIsTicking && GIsRHIInitialized)
			{
				if (FSceneInterface* Scene = InWorld->Scene)
				{
					ENQUEUE_RENDER_COMMAND(BeginFrame)([](FRHICommandListImmediate& RHICmdList)
					{
						GFrameNumberRenderThread++;
						GFrameCounterRenderThread++;
						FCoreDelegates::OnBeginFrameRT.Broadcast();
					});

					ENQUEUE_RENDER_COMMAND(EndFrame)([](FRHICommandListImmediate& RHICmdList)
					{
						FCoreDelegates::OnEndFrameRT.Broadcast();
						RHICmdList.EndFrame();
					});
					FlushRenderingCommands();
				}
			}

			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FSlateApplication::Get().GetRenderer()->Sync();
			FThreadManager::Get().Tick();
			FTSTicker::GetCoreTicker().Tick(FApp::GetDeltaTime());
			GEngine->TickDeferredCommands();
			GEngine->EmitDynamicResolutionEvent(EDynamicResolutionStateEvent::EndFrame);

			// Wait for any async asset (texture / mesh) compilation triggered by
			// the capture, so the next HLOD / export step reads real data.
			FAssetCompilingManager::Get().FinishAllCompilation();
		}
		else
		{
			// Fallback for a pure-headless world with no Slate: at least advance
			// the world so capture actors progress.
			InWorld->Tick(LEVELTICK_ViewportsOnly, InDelta);
		}
	}
}

FLinearColor LinearToSRGB(const FLinearColor& Color)
{
	auto ToSRGB = [](const float C) -> float
	{
		return C <= 0.0031308f ? C * 12.92f : 1.055f * FMath::Pow(C, 1.0f / 2.4f) - 0.055f;
	};
	return FLinearColor(ToSRGB(Color.R), ToSRGB(Color.G), ToSRGB(Color.B), Color.A);
}

FVector UVtoPyramid(FVector2D UV)
{
	// Full upper hemisphere: azimuth must sweep the full circle [0, 2π] so the
	// camera rig (and the actual capture) covers the whole top of the object.
	// (Using Phi = UV.X * PI only covered a quarter of the sphere — see user report.)
	const float Phi = UV.X * 2.f * PI;
	const float Theta = UV.Y * PI * 0.5f;
	return FVector(FMath::Sin(Theta) * FMath::Cos(Phi), FMath::Sin(Theta) * FMath::Sin(Phi), FMath::Cos(Theta));
}

FVector UVtoOctahedron(FVector2D UV)
{
	const float Phi = UV.X * 2.f * PI;
	const float CosTheta = 1.f - 2.f * UV.Y;
	const float SinTheta = FMath::Sqrt(FMath::Max(0.f, 1.f - CosTheta * CosTheta));
	return FVector(SinTheta * FMath::Cos(Phi), SinTheta * FMath::Sin(Phi), CosTheta);
}

// ----------------------------------------------------------------------------
// Command runner (faithful port of FCommandExecuteRunnable)
// ----------------------------------------------------------------------------
class FCommandExecuteRunnable final : public FRunnable
{
public:
	UOpenSplat4DStepBase* Step = nullptr;
	FString ExecutePath;
	FString Command;
	TFunction<void(bool bSuccess, int32 ReturnCode)> FinishedCallback;
	FProcHandle ProcessHandle;
	/** 任务退出原因：0=未结束 1=正常完成 2=取消 3=启动失败 4=超时 */
	std::atomic<int32> ExitReason{0};
	/** 超时秒数（<=0 表示不超时） */
	float TimeoutSeconds = 0.0f;

	FCommandExecuteRunnable(UOpenSplat4DStepBase* InStep, FString InExecutePath, FString InCommand, TFunction<void(bool, int32)> InFinishedCallback, float InTimeoutSeconds = 0.0f)
		: Step(InStep)
		, ExecutePath(MoveTemp(InExecutePath))
		, Command(MoveTemp(InCommand))
		, FinishedCallback(MoveTemp(InFinishedCallback))
		, TimeoutSeconds(InTimeoutSeconds)
	{
	}

	virtual uint32 Run() override
	{
		UE_LOG(LogOpenSplat4DStep, Warning, TEXT("Run Command: %s"), *Command);
		Step->bRequestCancelTask = false;
		int32 ReturnCode = -1;
		void* PipeStdOutRead = nullptr, *PipeStdOutWrite = nullptr;
		// Critical: 不使用 verify，管道创建失败时优雅返回而非崩溃
		if (!FPlatformProcess::CreatePipe(PipeStdOutRead, PipeStdOutWrite))
		{
			UE_LOG(LogOpenSplat4DStep, Error, TEXT("CreatePipe failed - cannot launch subprocess"));
			Step->ReceiveMessage(TEXT("[ERROR] 管道创建失败，无法启动子进程"));
			ExitReason.store(3);
			return 0;
		}

		// ENCODING / PARAMETER PASSING (2026-08 fix):
	// The old code base64-encoded the workDir here by counting the 3rd/4th
	// double quotes in the command string. That heuristic silently misfired
	// whenever some paths contain spaces and others don't (OpenSplat4DQuoteArg
	// only quotes args WITH spaces), replacing e.g. the --extractor value or
	// the colmap path with "b64:..." and corrupting the colmap invocation.
	//
	// Chinese paths are handled WITHOUT any encoding instead:
	//  - CreateProcessW passes the command line as UTF-16 verbatim, and
	//  - the helper script re-parses sys.argv via CommandLineToArgvW
	//    (Python 3's own sys.argv is Unicode-correct on Windows anyway).
	// => What we build below is exactly what Python's argparse receives.

	// Defensive: ensure the executable path is not wrapped in quotes, which
	// would trip WindowsPlatformProcess's `URL[0] != '"'` assertion.
	const FString ResolvedExecutePath = OpenSplat4DUnquoteArg(ExecutePath);
	UE_LOG(LogOpenSplat4DStep, Log, TEXT("Final Command: %s"), *Command);
		ProcessHandle = FPlatformProcess::CreateProc(*ResolvedExecutePath, *Command, true, true, true, nullptr, 0, nullptr, PipeStdOutWrite, nullptr, PipeStdOutWrite);
		if (!ProcessHandle.IsValid())
		{
			UE_LOG(LogOpenSplat4DStep, Error, TEXT("CreateProc failed for: %s"), *ResolvedExecutePath);
			Step->ReceiveMessage(TEXT("[ERROR] 子进程启动失败，请检查可执行文件路径"));
			FPlatformProcess::ClosePipe(PipeStdOutRead, PipeStdOutWrite);
			ExitReason.store(3);
			return 0;
		}

		// 主循环：带超时与取消检测
		const double StartTime = FPlatformTime::Seconds();
		constexpr float PollInterval = 0.05f;  // 50ms 轮询
		bool bTimedOut = false;

		while (FPlatformProcess::IsProcRunning(ProcessHandle))
		{
			TArray<uint8> BinaryData;
			FPlatformProcess::ReadPipeToArray(PipeStdOutRead, BinaryData);
			if (!BinaryData.IsEmpty())
			{
				// 修复编码：用 UTF-8→TCHAR 转换，避免 \0 截断和中文乱码
				Step->ReceiveMessageFromBinary(BinaryData);
			}
			if (Step->bRequestCancelTask)
			{
				FPlatformProcess::TerminateProc(ProcessHandle, true);
				ExitReason.store(2);
				Step->ReceiveMessage(TEXT("[CANCELLED] 用户已取消任务"));
				break;
			}
			if (TimeoutSeconds > 0.0f)
			{
				const double Elapsed = float(FPlatformTime::Seconds() - StartTime);
				if (Elapsed > TimeoutSeconds)
				{
					bTimedOut = true;
					ExitReason.store(4);
					FPlatformProcess::TerminateProc(ProcessHandle, true);
					Step->ReceiveMessage(FString::Printf(TEXT("[TIMEOUT] 任务超过 %.0f 秒被终止"), TimeoutSeconds));
					break;
				}
			}
			FPlatformProcess::Sleep(PollInterval);
		}

		// 读取剩余管道数据
		if (ExitReason.load() == 0 || ExitReason.load() == 1)
		{
			FPlatformProcess::GetProcReturnCode(ProcessHandle, &ReturnCode);
			TArray<uint8> BinaryData;
			FPlatformProcess::ReadPipeToArray(PipeStdOutRead, BinaryData);
			if (!BinaryData.IsEmpty())
			{
				Step->ReceiveMessageFromBinary(BinaryData);
			}
			if (ExitReason.load() == 0)
			{
				ExitReason.store(ReturnCode == 0 ? 1 : 3);
			}
		}
		else
		{
			ReturnCode = -1;
		}

		FPlatformProcess::CloseProc(ProcessHandle);
		FPlatformProcess::ClosePipe(PipeStdOutRead, PipeStdOutWrite);
		ProcessHandle.Reset();
		return 0;
	}

	virtual void Stop() override
	{
		// 设置取消标志，Run() 循环会检测并终止子进程
		Step->bRequestCancelTask = true;
	}

	virtual void Exit() override
	{
		const int32 Reason = ExitReason.load();
		const bool bSuccess = (Reason == 1);
		const int32 RetCode = (Reason == 1) ? 0 : -1;
		AsyncTask(ENamedThreads::GameThread, [this, bSuccess, RetCode]()
		{
			if (FinishedCallback) FinishedCallback(bSuccess, RetCode);
		});
	}
};

void UOpenSplat4DStepBase::ExecuteCommand(FString ExecutePath, FString Command, bool bAsync, TFunction<void()> FinishedCallback)
{
	// FPlatformProcess::CreateProc (Windows) asserts `URL[0] != '"'`: the
	// executable path passed as lpApplicationName must NOT be wrapped in quotes.
	// Paths with spaces are handled correctly by Windows as lpApplicationName
	// (only the command-line parameter string is space-delimited). Strip any
	// surrounding quotes the caller may have added via OpenSplat4DQuoteArg.
	ExecutePath = OpenSplat4DUnquoteArg(ExecutePath);

	// EARLY FALLBACK: If the executable path resolves to empty / whitespace,
	// the caller forgot to check it (or the bundled python/colmap is missing
	// and the user has not configured a path). Fail gracefully with a clear
	// log + UI notification instead of letting FPlatformProcess::CreateProc
	// return an invalid handle that the runnable would then spin on.
	if (ExecutePath.TrimStartAndEnd().IsEmpty())
	{
		UE_LOG(LogOpenSplat4DStep, Error,
			TEXT("ExecuteCommand 收到空的可执行路径，已跳过。命令字符串为: %s\n")
			TEXT("这通常意味着插件自带的 Python / COLMAP 未安装，且用户未在【设置】→ OpenSplat4D 中配置路径。"),
			*Command);

		// Surface the failure in the editor UI (not just the log) so users
		// who don't have the output log open still see what went wrong.
		if (FSlateApplication::IsInitialized())
		{
			FNotificationInfo NotifyInfo(FText::FromString(TEXT("可执行文件路径为空：请在【设置】→ OpenSplat4D 中配置 Python / COLMAP 路径")));
			NotifyInfo.ExpireDuration = 6.0f;
			NotifyInfo.bUseSuccessFailIcons = true;
			if (TSharedPtr<SNotificationItem> Notification = FSlateNotificationManager::Get().AddNotification(NotifyInfo))
			{
				Notification->SetCompletionState(SNotificationItem::CS_Fail);
			}
		}

		// Fire the callback so any UI waiting on completion still proceeds
		// (e.g. progress bars are hidden, buttons re-enabled).
		if (FinishedCallback)
		{
			AsyncTask(ENamedThreads::GameThread, [FinishedCallback]() { FinishedCallback(); });
		}
		return;
	}

	// Wrap the user-facing no-arg callback into the (bool, int32) signature
	// expected by FCommandExecuteRunnable. The bool/int32 (success, exit code)
	// are dropped here because all current callers of ExecuteCommand use the
	// no-arg form. If a caller ever needs the exit code, add an overload.
	TFunction<void(bool, int32)> WrappedCallback = [FinishedCallback](bool /*bSuccess*/, int32 /*ReturnCode*/)
	{
		if (FinishedCallback) FinishedCallback();
	};

	Worker = MakeShared<FCommandExecuteRunnable>(this, ExecutePath, Command, MoveTemp(WrappedCallback));
	if (bAsync)
	{
		WorkThread = TSharedPtr<FRunnableThread>(FRunnableThread::Create(Worker.Get(), TEXT("OpenSplat4D")));
	}
	else
	{
		Worker->Run();
		if (FinishedCallback) FinishedCallback();
	}
}

void UOpenSplat4DStepBase::ReceiveMessage(const FString& Message)
{
	UE_LOG(LogOpenSplat4DStep, Log, TEXT("%s"), *Message);
	if (FSlateApplication::IsInitialized())
	{
		TArray<FString> Lines;
		Message.ParseIntoArray(Lines, TEXT("\n"));
		LastTaskStatusText = FText::FromString(Lines.Last());
	}
}

void UOpenSplat4DStepBase::ReceiveMessageFromBinary(const TArray<uint8>& BinaryData)
{
	// Append new bytes to the leftover buffer from last call.
	PendingPipeBuffer.Append(BinaryData);

	// Walk the buffer and split on '\n'. Each complete line is decoded as
	// UTF-8 and forwarded to ReceiveMessage. The trailing partial line (if
	// any) stays in PendingPipeBuffer for the next call.
	int32 LineStart = 0;
	for (int32 i = 0; i < PendingPipeBuffer.Num(); ++i)
	{
		if (PendingPipeBuffer[i] == '\n')
		{
			// Skip trailing '\r' (CRLF line endings from Windows subprocesses).
			int32 LineEnd = i;
			if (LineEnd > LineStart && PendingPipeBuffer[LineEnd - 1] == '\r')
			{
				--LineEnd;
			}

			// Decode this line as UTF-8. UTF8_TO_TCHAR macro handles the
			// conversion correctly on all platforms; we just need a null-
			// terminated buffer. UTF8CHAR is ANSICHAR-sized on Windows.
			const int32 LineLen = LineEnd - LineStart;
			if (LineLen > 0)
			{
				UTF8CHAR LineBuf[4096];
				const int32 CopyLen = FMath::Min(LineLen, (int32)UE_ARRAY_COUNT(LineBuf) - 1);
				FMemory::Memcpy(LineBuf, PendingPipeBuffer.GetData() + LineStart, CopyLen * sizeof(UTF8CHAR));
				LineBuf[CopyLen] = 0;
				ReceiveMessage(FString(UTF8_TO_TCHAR(reinterpret_cast<const ANSICHAR*>(LineBuf))));
			}
			LineStart = i + 1;
		}
	}

	// Keep the tail (incomplete line) for next time. If we consumed everything
	// (LineStart == Num), Reset() the buffer to avoid unbounded growth from
	// repeated small allocations.
	if (LineStart > 0)
	{
		if (LineStart < PendingPipeBuffer.Num())
		{
			// Shift remaining bytes to the front of the array.
			const int32 Remaining = PendingPipeBuffer.Num() - LineStart;
			FMemory::Memmove(PendingPipeBuffer.GetData(), PendingPipeBuffer.GetData() + LineStart, Remaining * sizeof(uint8));
			PendingPipeBuffer.SetNum(Remaining, EAllowShrinking::No);
		}
		else
		{
			PendingPipeBuffer.Reset();
		}
	}
}
