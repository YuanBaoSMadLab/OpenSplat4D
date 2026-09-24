// Copyright OpenSplat4D 2026 All Rights Reserved.
#include "OpenSplat4DLocalization.h"

#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"

namespace OpenSplat4DLocalization
{
	// Active language. Defaults to Chinese; the editor module calls
	// SyncWithEditorLanguage() at startup so it normally follows the editor.
	static EOpenSplat4DUILanguage GCurrentUILanguage = EOpenSplat4DUILanguage::Chinese;

	// Bilingual string pairs, indexed in both directions so GetText() accepts
	// either the English or the Chinese side of a pair. Populated once by the
	// editor module's StartupModule.
	static TMap<FString, FString> GEnglishToChinese;
	static TMap<FString, FString> GChineseToEnglish;

	FOnUILanguageChanged& OnUILanguageChanged()
	{
		static FOnUILanguageChanged Delegate;
		return Delegate;
	}

	void SetUILanguage(EOpenSplat4DUILanguage InLanguage)
	{
		if (GCurrentUILanguage == InLanguage)
		{
			return;
		}
		GCurrentUILanguage = InLanguage;
		OnUILanguageChanged().Broadcast();
	}

	EOpenSplat4DUILanguage GetUILanguage()
	{
		return GCurrentUILanguage;
	}

	bool SyncWithEditorLanguage()
	{
		// Follow Editor Preferences > Region & Language: any "zh*" language
		// (zh-Hans, zh-CN, ...) selects Chinese, everything else English.
		const FCultureRef Culture = FInternationalization::Get().GetCurrentLanguage();
		const FString CultureName = Culture->GetName();
		const EOpenSplat4DUILanguage Detected = CultureName.StartsWith(TEXT("zh"), ESearchCase::IgnoreCase)
			? EOpenSplat4DUILanguage::Chinese
			: EOpenSplat4DUILanguage::English;

		if (Detected == GCurrentUILanguage)
		{
			return false;
		}
		GCurrentUILanguage = Detected;
		OnUILanguageChanged().Broadcast();
		return true;
	}

	void RegisterString(const TCHAR* InEnglish, const TCHAR* InChinese)
	{
		if (!InEnglish)
		{
			return;
		}
		const FString English(InEnglish);
		const FString Chinese = InChinese ? FString(InChinese) : FString();
		GEnglishToChinese.Add(English, Chinese);
		if (!Chinese.IsEmpty() && Chinese != English)
		{
			GChineseToEnglish.Add(Chinese, English);
		}
	}

	FText GetText(const TCHAR* InSource)
	{
		const FString Source(InSource ? InSource : TEXT(""));
		if (GCurrentUILanguage == EOpenSplat4DUILanguage::Chinese)
		{
			// Accepts the English source (gets translated) or text that is
			// already Chinese (returned as-is).
			const FString* Chinese = GEnglishToChinese.Find(Source);
			return FText::FromString(Chinese ? *Chinese : Source);
		}
		// English: accept the Chinese source (gets translated back) or text that
		// is already English.
		const FString* English = GChineseToEnglish.Find(Source);
		return FText::FromString(English ? *English : Source);
	}
}