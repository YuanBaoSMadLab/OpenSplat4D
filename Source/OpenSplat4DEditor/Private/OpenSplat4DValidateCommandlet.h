#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "OpenSplat4DValidateCommandlet.generated.h"

/**
 * Runs BuildNiagaraSystem() headlessly and reports success/failure via return
 * code.  Invoke with:
 *   UnrealEditor.exe "Project.uproject" -run=OpenSplat4DValidate -stdout -unattended -nullrhi
 */
UCLASS()
class UOpenSplat4DValidateCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
