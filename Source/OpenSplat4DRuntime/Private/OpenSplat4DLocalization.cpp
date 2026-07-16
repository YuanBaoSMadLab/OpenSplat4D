#include "OpenSplat4DLocalization.h"

namespace OpenSplat4DLocalization
{
	// Active language. Defaults to Chinese per project requirement.
	static EOpenSplat4DUILanguage GCurrentUILanguage = EOpenSplat4DUILanguage::Chinese;

	// English -> Chinese translation table. Populated once by the editor module.
	static TMap<FString, FString> GTranslations;

	void SetUILanguage(EOpenSplat4DUILanguage InLanguage)
	{
		GCurrentUILanguage = InLanguage;
	}

	EOpenSplat4DUILanguage GetUILanguage()
	{
		return GCurrentUILanguage;
	}

	void RegisterString(const TCHAR* InEnglish, const TCHAR* InChinese)
	{
		if (InEnglish)
		{
			GTranslations.Add(FString(InEnglish), InChinese ? FString(InChinese) : FString());
		}
	}

	FText GetText(const TCHAR* InEnglish)
	{
		const FString English(InEnglish);
		if (GCurrentUILanguage == EOpenSplat4DUILanguage::Chinese)
		{
			if (const FString* Chinese = GTranslations.Find(English))
			{
				return FText::FromString(*Chinese);
			}
		}
		// Fallback: original English source string.
		return FText::FromString(English);
	}
}
