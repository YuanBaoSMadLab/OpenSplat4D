#pragma once

#include "WorldPartition/HLOD/HLODBuilder.h"
#include "OpenSplat4DStep.h"
#include "OpenSplat4DHLODBuilder.generated.h"

class FHLODHashBuilder;

UCLASS()
class UOpenSplat4DHLODBuilderSettings : public UHLODBuilderSettings
{
	GENERATED_BODY()
public:
	UOpenSplat4DHLODBuilderSettings(const FObjectInitializer& ObjectInitializer);

	/** HLOD build hash: changes when any pipeline setting (or the 3D/4D mode) changes. */
	void ComputeHLODHash(FHLODHashBuilder& InHashBuilder) const override;

	UPROPERTY(VisibleAnywhere, Instanced, NoClear, meta = (EditInline), Category = "OpenSplat4D")
	TObjectPtr<UOpenSplat4DStep_Capture> CaptureSettings;

	UPROPERTY(VisibleAnywhere, Instanced, NoClear, meta = (EditInline), Category = "OpenSplat4D")
	TObjectPtr<UOpenSplat4DStep_SparseReconstruction> SparseReconstructionSettings;

	UPROPERTY(VisibleAnywhere, Instanced, NoClear, meta = (EditInline), Category = "OpenSplat4D")
	TObjectPtr<UOpenSplat4DStep_GaussianSplatting> GaussianSplattingSettings;

	/** When true the HLOD is trained as a dynamic 4DGS model (time-enabled); otherwise static 3DGS. */
	UPROPERTY(EditAnywhere, Category = "OpenSplat4D")
	bool bTrain4D = false;
};

UCLASS()
class UOpenSplat4DHLODBuilder : public UHLODBuilder
{
	GENERATED_BODY()
public:
	virtual TSubclassOf<UHLODBuilderSettings> GetSettingsClass() const override;
	virtual TArray<UActorComponent*> Build(const FHLODBuildContext& InHLODBuildContext, const TArray<UActorComponent*>& InSourceComponents) const override;
};
