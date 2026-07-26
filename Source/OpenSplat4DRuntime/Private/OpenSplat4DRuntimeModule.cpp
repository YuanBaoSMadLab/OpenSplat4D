#include "OpenSplat4DRuntimeModule.h"
#include "UObject/CoreRedirects.h"

DEFINE_LOG_CATEGORY(LogOpenSplat4D);

#define LOCTEXT_NAMESPACE "OpenSplat4D"

// Multiple rendering-mode support:
//   - OpenSplat4D native:  Niagara assets created in pure C++ (no CoreRedirect
//     needed) — this is the default and the one used by EnsureAssetsExist().
//   - Teacher-compatible:  loads .uasset templates authored against the
//     reference GaussianSplattingForUnrealEngine plugin. Needs CoreRedirects.
//
// To activate teacher-compatible mode, call RegisterTeacherCoreRedirects()
// BEFORE any template .uasset is loaded. It is NOT called at module startup
// because the native path works without it and early CoreRedirect registration
// can trigger errors in some engine builds.

void FOpenSplat4DRuntimeModule::RegisterTeacherCoreRedirects()
{
	static bool bRegistered = false;
	if (bRegistered)
	{
		return;
	}
	bRegistered = true;

	TArray<FCoreRedirect> Redirects;
	Redirects.Emplace(ECoreRedirectFlags::Type_Class,
		TEXT("/Script/GaussianSplattingRuntime.NiagaraDataInterfaceGaussianSplattingPointCloud"),
		TEXT("/Script/OpenSplat4DRuntime.NiagaraDataInterfaceOpenSplat4D"));
	Redirects.Emplace(ECoreRedirectFlags::Type_Class,
		TEXT("/Script/GaussianSplattingRuntime.GaussianSplattingPointCloud"),
		TEXT("/Script/OpenSplat4DRuntime.OpenSplat4DPointCloud"));
	Redirects.Emplace(ECoreRedirectFlags::Type_Struct,
		TEXT("/Script/GaussianSplattingRuntime.GaussianSplattingPoint"),
		TEXT("/Script/OpenSplat4DRuntime.OpenSplat4DPoint"));
	FCoreRedirects::AddRedirectList(Redirects, TEXT("OpenSplat4D"));

	UE_LOG(LogOpenSplat4D, Log, TEXT("OpenSplat4D: teacher-compatible CoreRedirects registered."));
}

void FOpenSplat4DRuntimeModule::StartupModule()
{
	// CoreRedirects are NOT registered here — the OpenSplat4D native rendering
	// path creates Niagara assets in pure C++ and doesn't need them.
	// Call RegisterTeacherCoreRedirects() on-demand when the teacher-compatible
	// rendering mode (loading reference .uasset templates) is selected.
	UE_LOG(LogOpenSplat4D, Log, TEXT("OpenSplat4D runtime module started (native mode)."));
}

void FOpenSplat4DRuntimeModule::ShutdownModule()
{
	UE_LOG(LogOpenSplat4D, Log, TEXT("OpenSplat4D runtime module shut down."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FOpenSplat4DRuntimeModule, OpenSplat4DRuntime)
