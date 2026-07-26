#pragma once

#include "CoreMinimal.h"
#include "NiagaraDataInterface.h"
#include "Misc/EngineVersionComparison.h"
#include "RHIUtilities.h"
#include "OpenSplat4DPointCloud.h"
#include "OpenSplat4DDataInterface.generated.h"

/** Number of float4 slots packed per point in the GPU buffer.
 *  [OPT] Expanded to 15 for SH coefficients (degree-3 spherical harmonics).
 *  Layout per point:
 *    0: (PosX, PosY, PosZ, AnchorTime)
 *    1: (QuatX, QuatY, QuatZ, QuatW)
 *    2: (ScaleX, ScaleY, ScaleZ, TimeVariance)
 *    3: (ColorR, ColorG, ColorB, ColorA)
 *    4: (VelX, VelY, VelZ, bUseVelocity)
 *    5..6: reserved (padding for 16-byte alignment)
 *    7..14: SH rest coefficients (45 floats / 3 channels = max 8 float4 for d=3 SH)
 */
static constexpr int32 GOPEN_SPLAT_FLOAT4_PER_POINT = 15;

/** Number of SH rest float4 slots (after slot 7). Supports up to SH degree 3. */
static constexpr int32 GOPEN_SPLAT_SH_SLOTS = 8;
/** Maximum SH rest coefficient count (3 channels x 15 = 45 for degree 3). */
static constexpr int32 GOPEN_SPLAT_SH_MAX_REST = 45;

struct FNiagaraDataInterfaceProxyOpenSplat4D : public FNiagaraDataInterfaceProxy
{
	FNiagaraDataInterfaceProxyOpenSplat4D(class UNiagaraDataInterfaceOpenSplat4D* InOwner);

	virtual ~FNiagaraDataInterfaceProxyOpenSplat4D();

	void MakeBufferDirty();
	void TryUpdateBuffer();
	void PostDataToGPU();
	/** [OPT] Refreshes only the GPU buffer contents without full re-init.
	 *  Called from the render thread via SetShaderParameters. */
	void RefreshPointCloud();

	virtual int32 PerInstanceDataPassedToRenderThreadSize() const override { return 0; }

	TObjectPtr<class UNiagaraDataInterfaceOpenSplat4D> Owner = nullptr;
	TObjectPtr<class UOpenSplat4DPointCloud> PointCloud;
	bool bDirty = false;
	/** [OPT] True when point count changed and the RHI buffer must be re-allocated. */
	bool bNeedsResize = false;
	/** [OPT] Point count at last upload. Used to decide whether to resize. */
	int32 CachedPointCount = 0;
	FReadBuffer OpenSplat4DPointDataBuffer;
	/** Protects PointCloud pointer and buffer state across game/render threads. */
	mutable FCriticalSection BufferLock;
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
		SHADER_PARAMETER(float, SplatScale)
	END_SHADER_PARAMETER_STRUCT()
public:
	void SetPointCloud(UOpenSplat4DPointCloud* InPointCloud);
	UOpenSplat4DPointCloud* GetPointCloud() const { return PointCloud; }

	/** [OPT] Refresh GPU buffer without full re-init. Call after modifying points. */
	UFUNCTION(BlueprintCallable, Category = "OpenSplat4D")
	void RefreshPointCloud();

	/** Global playback time, pushed by AOpenSplat4DPointCloudActor each frame. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OpenSplat4D")
	float Time = 0.f;

	/** When true, the GPU applies the 4DGS temporal marginal; otherwise weight == 1. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OpenSplat4D")
	bool bTemporalWeighting = false;

	/** Runtime multiplier on every splat's size (default 1.0). */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "OpenSplat4D")
	float SplatScale = 1.f;

	// --- GPU / VM functions -------------------------------------------------
	void GetPointCount(FVectorVMExternalFunctionContext& Context);
	void GetPointData(FVectorVMExternalFunctionContext& Context);
	void GetPointData4D(FVectorVMExternalFunctionContext& Context);
	void GetTimeWeight(FVectorVMExternalFunctionContext& Context);
	void GetTimeOffset(FVectorVMExternalFunctionContext& Context);

	static const FName GetPointDataFunctionName;
	static const FName GetPointCountFunctionName;
	static const FName GetPointData4DFunctionName;
	static const FName GetTimeWeightFunctionName;
	static const FName GetTimeOffsetFunctionName;
	/** [SH] Evaluate spherical-harmonics colour at a view direction. */
	static const FName GetPointColorSHFunctionName;

	/** Exposed so OpenSplat4DNiagaraSetup can query the exact registered
	 *  function signatures from the CDO (avoids hardcoding mismatches). */
#if WITH_EDITORONLY_DATA
#if UE_VERSION_NEWER_THAN(5, 4, 0)
	virtual void GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& OutFunctions) const override;
#else
	void GetFunctions(TArray<FNiagaraFunctionSignature>& OutFunctions) override;
#endif
#endif

protected:
	friend struct FNiagaraDataInterfaceProxyOpenSplat4D;

	static const FString PointCountName;
	static const FString PointDataBufferName;
	static const FString TimeName;
	static const FString TemporalWeightingName;
	static const FString SplatScaleName;

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

	virtual bool CopyToInternal(UNiagaraDataInterface* Destination) const override;
};
