// Copyright OpenSplat4D 2026 All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "OpenSplat4DLocalization.generated.h"

/**
 * UI language of the OpenSplat4D plugin.
 *
 * There is no plugin-level language switch: the plugin follows the editor's own
 * language automatically (Editor Preferences > Region & Language) via
 * OpenSplat4DLocalization::SyncWithEditorLanguage, which the editor module
 * calls at startup and whenever the culture changes.
 *
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
	/** Broadcast whenever the active UI language changes (widgets rebuild on it). */
	DECLARE_MULTICAST_DELEGATE(FOnUILanguageChanged);
	OPENSPLAT4DRUNTIME_API FOnUILanguageChanged& OnUILanguageChanged();

	/** Override the active UI language for the whole plugin. */
	OPENSPLAT4DRUNTIME_API void SetUILanguage(EOpenSplat4DUILanguage InLanguage);

	/** The currently active UI language (defaults to Chinese). */
	OPENSPLAT4DRUNTIME_API EOpenSplat4DUILanguage GetUILanguage();

	/**
	 * Derive the UI language from the editor's current culture ("zh*" -> Chinese,
	 * anything else -> English) and apply it. Broadcasts OnUILanguageChanged when
	 * the language actually changed. Safe to call repeatedly.
	 * @return true when the language changed.
	 */
	OPENSPLAT4DRUNTIME_API bool SyncWithEditorLanguage();

	/**
	 * Register a bilingual UI string pair. Both sides are indexed, so either the
	 * English or the Chinese text may be passed to GetText(). Call this from the
	 * editor module's StartupModule before any widget is built.
	 */
	OPENSPLAT4DRUNTIME_API void RegisterString(const TCHAR* InEnglish, const TCHAR* InChinese);

	/**
	 * Resolve a UI string for the current language. Accepts either side of a
	 * registered pair and returns the one matching the active language;
	 * unregistered strings are returned unchanged.
	 */
	OPENSPLAT4DRUNTIME_API FText GetText(const TCHAR* InSource);
}

// Convenience macro replacing LOCTEXT / NSLOCTEXT for any user-facing string
// that should follow the editor language. Pass the English source text:
//   OS4D_TEXT("Capture")  -> "捕获" when Chinese, "Capture" when English.
#define OS4D_TEXT(Source) OpenSplat4DLocalization::GetText(TEXT(Source))