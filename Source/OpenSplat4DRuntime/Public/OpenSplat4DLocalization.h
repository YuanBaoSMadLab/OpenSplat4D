#pragma once

#include "CoreMinimal.h"
#include "OpenSplat4DLocalization.generated.h"

/**
 * UI language selector for the OpenSplat4D plugin.
 *
 * Default is Chinese; the editor settings module overrides this at startup and
 * whenever the user flips the "UISLanguage" option (see UOpenSplat4DSettings).
 * Kept in the Runtime module (rather than Editor) so both modules can read the
 * current language without a circular dependency.
 */
UENUM(BlueprintType)
enum class EOpenSplat4DUILanguage : uint8
{
	Chinese = 0 UMETA(DisplayName = "中文"),
	English = 1 UMETA(DisplayName = "English"),
};

namespace OpenSplat4DLocalization
{
	/** Override the active UI language for the whole plugin. */
	OPENSPLAT4DRUNTIME_API void SetUILanguage(EOpenSplat4DUILanguage InLanguage);

	/** The currently active UI language (defaults to Chinese). */
	OPENSPLAT4DRUNTIME_API EOpenSplat4DUILanguage GetUILanguage();

	/**
	 * Register a bilingual UI string pair. Safe to call multiple times
	 * (idempotent: the last Chinese value wins). Call this from the editor
	 * module's StartupModule before any widget is built.
	 */
	OPENSPLAT4DRUNTIME_API void RegisterString(const TCHAR* InEnglish, const TCHAR* InChinese);

	/**
	 * Resolve a UI string for the current language. Returns the Chinese
	 * translation when the language is Chinese and a translation exists,
	 * otherwise the original English source string.
	 */
	OPENSPLAT4DRUNTIME_API FText GetText(const TCHAR* InEnglish);
}

// Convenience macro replacing LOCTEXT / NSLOCTEXT for any user-facing string
// that should follow the plugin's UI language setting.
//   OS4D_TEXT("Capture")  -> "捕获" when Chinese, "Capture" when English.
#define OS4D_TEXT(English) OpenSplat4DLocalization::GetText(TEXT(English))
