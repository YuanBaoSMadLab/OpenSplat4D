#include "OpenSplat4DStep.h"
#include "OpenSplat4DSettings.h"

#include <string>

#include "Misc/FileHelper.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"

#include "Editor.h"
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

DEFINE_LOG_CATEGORY(LogOpenSplat4DStep);

	FString OpenSplat4DQuoteArg(const FString& Arg)
{
	if (Arg.Contains(TEXT(" ")) || Arg.Contains(TEXT("\t")))
	{
		return FString::Printf(TEXT("\"%s\""), *Arg);
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
		const TArray<FString>& Images, const FString& MasksDir,
		const FString& DepthsDir, const FString& CamerasFile)
    {
        if (!Asset)
        {
            return;
        }
        Asset->WorkDirectory = WorkDir;
        Asset->Images = Images;
        Asset->ImageCount = Images.Num();
        Asset->MasksDir = MasksDir;
        Asset->DepthsDir = DepthsDir;
        Asset->CamerasFile = CamerasFile;
        Asset->CapturedAt = FDateTime::Now();
        Asset->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(Asset);
        SaveCaptureSetPackage(Asset->GetOutermost(), Asset);
        UE_LOG(LogOpenSplat4DStep, Log, TEXT("Capture set asset updated: %s (%d images)"), *Asset->GetPathName(), Images.Num());
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
            for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
            {
                if (AActor* A = Cast<AActor>(*It))
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
	TFunction<void()> FinishedCallback;
	FProcHandle ProcessHandle;

	FCommandExecuteRunnable(UOpenSplat4DStepBase* InStep, FString InExecutePath, FString InCommand, TFunction<void()> InFinishedCallback)
		: Step(InStep)
		, ExecutePath(MoveTemp(InExecutePath))
		, Command(MoveTemp(InCommand))
		, FinishedCallback(MoveTemp(InFinishedCallback))
	{
	}

	virtual uint32 Run() override
	{
		UE_LOG(LogOpenSplat4DStep, Warning, TEXT("Run Command: %s"), *Command);
		Step->bRequestCancelTask = false;
		int32 ReturnCode = -1;
		void* PipeStdOutRead = nullptr, *PipeStdOutWrite = nullptr;
		verify(FPlatformProcess::CreatePipe(PipeStdOutRead, PipeStdOutWrite));

		// Redirect both stdout and stderr to the same pipe so we capture
		// everything in one stream. The child does not need stdin, so pass
		// nullptr there: previously the stdout *read* handle was passed as the
		// stdin argument, which triggered the Windows
		// "PipeReadChild passed to CreateProc is not inheritable" warning.
		ProcessHandle = FPlatformProcess::CreateProc(*ExecutePath, *Command, true, true, true, nullptr, 0, nullptr, PipeStdOutWrite, nullptr, PipeStdOutWrite);
		if (ProcessHandle.IsValid())
		{
			FPlatformProcess::Sleep(0.01f);
			while (FPlatformProcess::IsProcRunning(ProcessHandle))
			{
				TArray<uint8> BinaryData;
				FPlatformProcess::ReadPipeToArray(PipeStdOutRead, BinaryData);
				if (!BinaryData.IsEmpty())
				{
					Step->ReceiveMessage(FString(std::string(reinterpret_cast<const char*>(BinaryData.GetData()), BinaryData.Num()).c_str()));
				}
				if (Step->bRequestCancelTask)
				{
					FPlatformProcess::CloseProc(ProcessHandle);
					FPlatformProcess::ClosePipe(PipeStdOutRead, PipeStdOutWrite);
					return 0;
				}
			}
			FPlatformProcess::GetProcReturnCode(ProcessHandle, &ReturnCode);
			TArray<uint8> BinaryData;
			FPlatformProcess::ReadPipeToArray(PipeStdOutRead, BinaryData);
			if (!BinaryData.IsEmpty())
			{
				Step->ReceiveMessage(FString(std::string(reinterpret_cast<const char*>(BinaryData.GetData()), BinaryData.Num()).c_str()));
			}
			FPlatformProcess::CloseProc(ProcessHandle);
		}
		else
		{
			Step->ReceiveMessage(TEXT("Failed to launch command"));
		}
		FPlatformProcess::ClosePipe(PipeStdOutRead, PipeStdOutWrite);
		ProcessHandle.Reset();
		return 0;
	}

	virtual void Stop() override { Step->WorkThread.Reset(); }

	virtual void Exit() override
	{
		AsyncTask(ENamedThreads::GameThread, [this]()
		{
			if (FinishedCallback) FinishedCallback();
		});
	}
};

void UOpenSplat4DStepBase::ExecuteCommand(FString ExecutePath, FString Command, bool bAsync, TFunction<void()> FinishedCallback)
{
	Worker = MakeShared<FCommandExecuteRunnable>(this, ExecutePath, Command, FinishedCallback);
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
