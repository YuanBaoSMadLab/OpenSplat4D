#pragma once

#include "CoreMinimal.h"
#include "NiagaraDataInterface.h"
#include "Misc/EngineVersionComparison.h"
#include "RHIUtilities.h"
#include "OpenSplat4DPointCloud.h"
#include "OpenSplat4DDataInterface.generated.h"

/** Number of float4 slots packed per point in the GPU buffer. */
static constexpr int32 GOPEN_SPLAT_FLOAT4_PER_POINT = 7;

struct FNiagaraDataInterfaceProxyOpenSplat4D : public FNiagaraDataInterfaceProxy
{
	FNiagaraDataInterfaceProxyOpenSplat4D(class UNiagaraDataInterfaceOpenSplat4D* InOwner);

	virtual ~FNiagaraDataInterfaceProxyOpenSplat4D();

	void MakeBufferDirty();
	void TryUpdateBuffer();
	void PostDataToGPU();

	virtual int32 PerInstanceDataPassedToRenderThreadSize() const override { return 0; }

	TObjectPtr<class UNiagaraDataInterfaceOpenSplat4D> Owner = nullptr;
	TObjectPtr<class UOpenSplat4DPointCloud> PointCloud;
	bool bDirty = false;
	FReadBuffer OpenSplat4DPointDataBuffer;
	FCriticalSection BufferLock;
};

/**
 * Niagara data interface that streams OpenSplat4D point clouds (3DGS or 4DGS)
 * to the GPU. The buffer packs GOPEN_SPLAT_FLOAT4_PER_POINT float4 per point:
 *   0: (PosX, PosY, PosZ, AnchorTime)
 *   1: (QuatX, QuatY, QuatZ, QuatW)
 *   2: (ScaleX, ScaleY, ScaleZ, TimeVariance)
 *   3: (ColorR, ColorG, ColorB, ColorA)
 *   4: (VelX, VelY, VelZ, bUseVelocity)
 *   5,6: reserved
 *
 * GPU functions expose the data plus the 4DGS temporal marginal
 * (GetTimeWeight / GetTimeOffset), ported from 4d-gaussian-splatting.
 */
UCLASS(EditInlineNew, Category = "Array", meta = (DisplayName = "OpenSplat4D Point Cloud", Experimental), Blueprintable, BlueprintType)
class OPENSPLAT4DRUNTIME_API UNiagaraDataInterfaceOpenSplat4D : public UNiagaraDataInterface
{
	GENERATED_UCLASS_BODY()

	BEGIN_SHADER_PARAMETER_STRUCT(FShaderParameters, )
		SHADER_PARAMETER(int, PointCount)
		SHADER_PARAMETER_SRV(Buffer<float4>, PointDataBuffer)
		SHADER_PARAMETER(float, Time)
		SHADER_PARAMETER(int, bTemporalWeighting)
	END_SHADER_PARAMETER_STRUCT()
public:
	void SetPointCloud(UOpenSplat4DPointCloud* InPointCloud);
	UOpenSplat4DPointCloud* GetPointCloud() const { return PointCloud; }

	/** Global playback time, pushed by AOpenSplat4DPointCloudActor each frame. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OpenSplat4D")
	float Time = 0.f;

	/** When true, the GPU applies the 4DGS temporal marginal; otherwise weight == 1. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OpenSplat4D")
	bool bTemporalWeighting = false;

	// --- GPU / VM functions -------------------------------------------------
	void GetPointCount(FVectorVMExternalFunctionContext& Context);
	void GetPointData(FVectorVMExternalFunctionContext& Context);
	void GetPointData4D(FVectorVMExternalFunctionContext& Context);
	void GetTimeWeight(FVectorVMExternalFunctionContext& Context);
	void GetTimeOffset(FVectorVMExternalFunctionContext& Context);

protected:
	friend struct FNiagaraDataInterfaceProxyOpenSplat4D;

	static const FName GetPointDataFunctionName;
	static const FName GetPointCountFunctionName;
	static const FName GetPointData4DFunctionName;
	static const FName GetTimeWeightFunctionName;
	static const FName GetTimeOffsetFunctionName;

	static const FString PointCountName;
	static const FString PointDataBufferName;
	static const FString TimeName;
	static const FString TemporalWeightingName;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OpenSplat4D")
	TObjectPtr<UOpenSplat4DPointCloud> PointCloud;

	virtual void GetVMExternalFunction(const FVMExternalFunctionBindingInfo& BindingInfo, void* InstanceData, FVMExternalFunction& OutFunc) override;
	virtual bool CanExecuteOnTarget(ENiagaraSimTarget Target) const override { return true; }

#if WITH_EDITORONLY_DATA
	virtual bool AppendCompileHash(FNiagaraCompileHashVisitor* InVisitor) const override;
	virtual bool GetFunctionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, const FNiagaraDataInterfaceGeneratedFunction& FunctionInfo, int FunctionInstanceIndex, FString& OutHLSL) override;
	virtual void GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, FString& OutHLSL) override;
#endif
	virtual void BuildShaderParameters(FNiagaraShaderParametersBuilder& ShaderParametersBuilder) const override;
	virtual void SetShaderParameters(const FNiagaraDataInterfaceSetShaderParametersContext& Context) const override;
	virtual bool Equals(const UNiagaraDataInterface* Other) const override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
	virtual void PostInitProperties() override;
	virtual void PostLoad() override;

#if WITH_EDITORONLY_DATA
#if UE_VERSION_NEWER_THAN(5, 4, 0)
	virtual void GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& OutFunctions) const override;
#else
	void GetFunctions(TArray<FNiagaraFunctionSignature>& OutFunctions) override;
#endif
#endif
	virtual bool CopyToInternal(UNiagaraDataInterface* Destination) const override;
};
