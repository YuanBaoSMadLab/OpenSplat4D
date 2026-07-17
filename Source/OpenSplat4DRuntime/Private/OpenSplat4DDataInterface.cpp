#include "OpenSplat4DDataInterface.h"
#include "NiagaraCompileHashVisitor.h"
#include "NiagaraShaderParametersBuilder.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraRenderer.h"
#include "RHIUtilities.h"

#define LOCTEXT_NAMESPACE "OpenSplat4D"

static TAutoConsoleVariable<float> CVarOpenSplat4DScreenSizeBias(
	TEXT("r.OpenSplat4D.ScreenSizeBias"),
	0.0f,
	TEXT("Screen-size bias used by the Gaussian splat LOD."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<float> CVarOpenSplat4DScreenSizeScale(
	TEXT("r.OpenSplat4D.ScreenSizeScale"),
	1.0f,
	TEXT("Screen-size scale used by the Gaussian splat LOD."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

// ----------------------------------------------------------------------------
// Proxy
// ----------------------------------------------------------------------------
FNiagaraDataInterfaceProxyOpenSplat4D::FNiagaraDataInterfaceProxyOpenSplat4D(class UNiagaraDataInterfaceOpenSplat4D* InOwner)
	: Owner(InOwner)
{
}

FNiagaraDataInterfaceProxyOpenSplat4D::~FNiagaraDataInterfaceProxyOpenSplat4D()
{
}

void FNiagaraDataInterfaceProxyOpenSplat4D::MakeBufferDirty()
{
	bDirty = true;
}

void FNiagaraDataInterfaceProxyOpenSplat4D::TryUpdateBuffer()
{
	if (PointCloud != Owner->PointCloud)
	{
		PointCloud = Owner->PointCloud;
		if (PointCloud)
		{
			PointCloud->OnPointsChanged.AddLambda([this]() { bDirty = true; });
		}
		bDirty = true;
	}
	if (bDirty)
	{
		PostDataToGPU();
		bDirty = false;
	}
}

void FNiagaraDataInterfaceProxyOpenSplat4D::PostDataToGPU()
{
	if (Owner == nullptr || Owner->PointCloud == nullptr)
	{
		return;
	}
	const TArray<FOpenSplat4DPoint>& Points = Owner->PointCloud->GetPoints();
	TArray<FVector4f> PointData;
	PointData.SetNum(Points.Num() * GOPEN_SPLAT_FLOAT4_PER_POINT);

	for (int32 i = 0; i < Points.Num(); i++)
	{
		const FOpenSplat4DPoint& P = Points[i];
		const int32 Base = i * GOPEN_SPLAT_FLOAT4_PER_POINT;
		PointData[Base + 0] = FVector4f(P.Position.X, P.Position.Y, P.Position.Z, P.AnchorTime);
		PointData[Base + 1] = FVector4f(P.Quat.X, P.Quat.Y, P.Quat.Z, P.Quat.W);
		PointData[Base + 2] = FVector4f(P.Scale.X, P.Scale.Y, P.Scale.Z, P.TimeVariance);
		PointData[Base + 3] = FVector4f(P.Color.R, P.Color.G, P.Color.B, P.Color.A);
		PointData[Base + 4] = FVector4f(P.Velocity.X, P.Velocity.Y, P.Velocity.Z, P.bUseVelocity ? 1.f : 0.f);
		// Slots 5..17: SH data (raw_f_dc + opacity + f_rest)
		const int32 SHLen = P.SHRest.Num();
		for (int32 si = 0; si < 13; si++)
		{
			float v0 = (si * 4 + 0 < SHLen) ? P.SHRest[si * 4 + 0] : 0.f;
			float v1 = (si * 4 + 1 < SHLen) ? P.SHRest[si * 4 + 1] : 0.f;
			float v2 = (si * 4 + 2 < SHLen) ? P.SHRest[si * 4 + 2] : 0.f;
			float v3 = (si * 4 + 3 < SHLen) ? P.SHRest[si * 4 + 3] : 0.f;
			PointData[Base + 5 + si] = FVector4f(v0, v1, v2, v3);
		}
	}

	ENQUEUE_RENDER_COMMAND(FUpdateOpenSplat4DBuffer)(
		[this, PointData](FRHICommandListImmediate& RHICmdList)
		{
			const int32 NumBytesInBuffer = sizeof(FVector4f) * PointData.Num();

			if (NumBytesInBuffer != OpenSplat4DPointDataBuffer.NumBytes)
			{
				if (OpenSplat4DPointDataBuffer.NumBytes > 0)
					OpenSplat4DPointDataBuffer.Release();
				if (NumBytesInBuffer > 0)
					OpenSplat4DPointDataBuffer.Initialize(
						RHICmdList,
						TEXT("FNiagaraDataInterfaceProxyOpenSplat4D_PointBuffer"),
						sizeof(FVector4f),
						PointData.Num(),
						EPixelFormat::PF_A32B32G32R32F,
						BUF_Static);
			}

			if (OpenSplat4DPointDataBuffer.NumBytes > 0)
			{
				float* BufferData = static_cast<float*>(RHICmdList.LockBuffer(
					OpenSplat4DPointDataBuffer.Buffer, 0, NumBytesInBuffer, EResourceLockMode::RLM_WriteOnly));
				FScopeLock ScopeLock(&BufferLock);
				FPlatformMemory::Memcpy(BufferData, PointData.GetData(), NumBytesInBuffer);
				RHICmdList.UnlockBuffer(OpenSplat4DPointDataBuffer.Buffer);
			}
		});
}

// ----------------------------------------------------------------------------
// Data interface
// ----------------------------------------------------------------------------
UNiagaraDataInterfaceOpenSplat4D::UNiagaraDataInterfaceOpenSplat4D(FObjectInitializer const& ObjectInitializer)
	: Super(ObjectInitializer)
{
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		Proxy = MakeUnique<FNiagaraDataInterfaceProxyOpenSplat4D>(this);
	}
}

#if WITH_EDITORONLY_DATA
#if UE_VERSION_NEWER_THAN(5, 4, 0)
void UNiagaraDataInterfaceOpenSplat4D::GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& OutFunctions) const
{
	Super::GetFunctionsInternal(OutFunctions);
#else
void UNiagaraDataInterfaceOpenSplat4D::GetFunctions(TArray<FNiagaraFunctionSignature>& OutFunctions)
{
	Super::GetFunctions(OutFunctions);
#endif
	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = GetPointDataFunctionName;
		Sig.Inputs.Add(FNiagaraVariable(GetClass(), TEXT("OpenSplat4D")));
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Position")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("Quat")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Scale")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetColorDef(), TEXT("Color")));
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		OutFunctions.Add(Sig);
	}
	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = GetPointData4DFunctionName;
		Sig.Inputs.Add(FNiagaraVariable(GetClass(), TEXT("OpenSplat4D")));
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Position")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec4Def(), TEXT("Quat")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Scale")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetColorDef(), TEXT("Color")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("AnchorTime")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("TimeVariance")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Velocity")));
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		OutFunctions.Add(Sig);
	}
	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = GetPointCountFunctionName;
		Sig.Inputs.Add(FNiagaraVariable(GetClass(), TEXT("OpenSplat4D")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("PointCount")));
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		OutFunctions.Add(Sig);
	}
	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = GetTimeWeightFunctionName;
		Sig.Inputs.Add(FNiagaraVariable(GetClass(), TEXT("OpenSplat4D")));
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Weight")));
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		OutFunctions.Add(Sig);
	}
	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = GetTimeOffsetFunctionName;
		Sig.Inputs.Add(FNiagaraVariable(GetClass(), TEXT("OpenSplat4D")));
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Offset")));
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		OutFunctions.Add(Sig);
	}
}
#endif

DEFINE_NDI_DIRECT_FUNC_BINDER(UNiagaraDataInterfaceOpenSplat4D, GetPointCount);
DEFINE_NDI_DIRECT_FUNC_BINDER(UNiagaraDataInterfaceOpenSplat4D, GetPointData);
DEFINE_NDI_DIRECT_FUNC_BINDER(UNiagaraDataInterfaceOpenSplat4D, GetPointData4D);
DEFINE_NDI_DIRECT_FUNC_BINDER(UNiagaraDataInterfaceOpenSplat4D, GetTimeWeight);
DEFINE_NDI_DIRECT_FUNC_BINDER(UNiagaraDataInterfaceOpenSplat4D, GetTimeOffset);

void UNiagaraDataInterfaceOpenSplat4D::SetPointCloud(UOpenSplat4DPointCloud* InPointCloud)
{
	PointCloud = InPointCloud;
	if (auto DIProxy = GetProxyAs<FNiagaraDataInterfaceProxyOpenSplat4D>())
	{
		DIProxy->MakeBufferDirty();
	}
}

void UNiagaraDataInterfaceOpenSplat4D::GetPointCount(FVectorVMExternalFunctionContext& Context)
{
	VectorVM::FExternalFuncRegisterHandler<int32> OutPointCount(Context);
	const int32 Count = PointCloud ? PointCloud->GetPointCount() : 0;
	for (int32 InstanceIdx = 0; InstanceIdx < Context.GetNumInstances(); ++InstanceIdx)
	{
		*OutPointCount.GetDestAndAdvance() = Count;
	}
}

void UNiagaraDataInterfaceOpenSplat4D::GetPointData(FVectorVMExternalFunctionContext& Context)
{
	VectorVM::FExternalFuncInputHandler<int32> InIndex(Context);
	VectorVM::FExternalFuncRegisterHandler<float> PosX(Context), PosY(Context), PosZ(Context);
	VectorVM::FExternalFuncRegisterHandler<float> QuatX(Context), QuatY(Context), QuatZ(Context), QuatW(Context);
	VectorVM::FExternalFuncRegisterHandler<float> ScaleX(Context), ScaleY(Context), ScaleZ(Context);
	VectorVM::FExternalFuncRegisterHandler<float> ColorR(Context), ColorG(Context), ColorB(Context), ColorA(Context);

	if (!PointCloud) return;
	const TArray<FOpenSplat4DPoint>& Points = PointCloud->GetPoints();
	for (int32 InstanceIdx = 0; InstanceIdx < Context.GetNumInstances(); ++InstanceIdx)
	{
		const FOpenSplat4DPoint& P = Points[FMath::Clamp(InIndex.Get(), 0, Points.Num() - 1)];
		*PosX.GetDestAndAdvance() = P.Position.X;
		*PosY.GetDestAndAdvance() = P.Position.Y;
		*PosZ.GetDestAndAdvance() = P.Position.Z;
		*QuatX.GetDestAndAdvance() = P.Quat.X;
		*QuatY.GetDestAndAdvance() = P.Quat.Y;
		*QuatZ.GetDestAndAdvance() = P.Quat.Z;
		*QuatW.GetDestAndAdvance() = P.Quat.W;
		*ScaleX.GetDestAndAdvance() = P.Scale.X;
		*ScaleY.GetDestAndAdvance() = P.Scale.Y;
		*ScaleZ.GetDestAndAdvance() = P.Scale.Z;
		*ColorR.GetDestAndAdvance() = P.Color.R;
		*ColorG.GetDestAndAdvance() = P.Color.G;
		*ColorB.GetDestAndAdvance() = P.Color.B;
		*ColorA.GetDestAndAdvance() = P.Color.A;
		InIndex.Advance();
	}
}

void UNiagaraDataInterfaceOpenSplat4D::GetPointData4D(FVectorVMExternalFunctionContext& Context)
{
	VectorVM::FExternalFuncInputHandler<int32> InIndex(Context);
	VectorVM::FExternalFuncRegisterHandler<float> PosX(Context), PosY(Context), PosZ(Context);
	VectorVM::FExternalFuncRegisterHandler<float> QuatX(Context), QuatY(Context), QuatZ(Context), QuatW(Context);
	VectorVM::FExternalFuncRegisterHandler<float> ScaleX(Context), ScaleY(Context), ScaleZ(Context);
	VectorVM::FExternalFuncRegisterHandler<float> ColorR(Context), ColorG(Context), ColorB(Context), ColorA(Context);
	VectorVM::FExternalFuncRegisterHandler<float> AnchorTime(Context);
	VectorVM::FExternalFuncRegisterHandler<float> TimeVariance(Context);
	VectorVM::FExternalFuncRegisterHandler<float> VelX(Context), VelY(Context), VelZ(Context);

	if (!PointCloud) return;
	const TArray<FOpenSplat4DPoint>& Points = PointCloud->GetPoints();
	for (int32 InstanceIdx = 0; InstanceIdx < Context.GetNumInstances(); ++InstanceIdx)
	{
		const FOpenSplat4DPoint& P = Points[FMath::Clamp(InIndex.Get(), 0, Points.Num() - 1)];
		*PosX.GetDestAndAdvance() = P.Position.X;
		*PosY.GetDestAndAdvance() = P.Position.Y;
		*PosZ.GetDestAndAdvance() = P.Position.Z;
		*QuatX.GetDestAndAdvance() = P.Quat.X;
		*QuatY.GetDestAndAdvance() = P.Quat.Y;
		*QuatZ.GetDestAndAdvance() = P.Quat.Z;
		*QuatW.GetDestAndAdvance() = P.Quat.W;
		*ScaleX.GetDestAndAdvance() = P.Scale.X;
		*ScaleY.GetDestAndAdvance() = P.Scale.Y;
		*ScaleZ.GetDestAndAdvance() = P.Scale.Z;
		*ColorR.GetDestAndAdvance() = P.Color.R;
		*ColorG.GetDestAndAdvance() = P.Color.G;
		*ColorB.GetDestAndAdvance() = P.Color.B;
		*ColorA.GetDestAndAdvance() = P.Color.A;
		*AnchorTime.GetDestAndAdvance() = P.AnchorTime;
		*TimeVariance.GetDestAndAdvance() = P.TimeVariance;
		*VelX.GetDestAndAdvance() = P.Velocity.X;
		*VelY.GetDestAndAdvance() = P.Velocity.Y;
		*VelZ.GetDestAndAdvance() = P.Velocity.Z;
		InIndex.Advance();
	}
}

void UNiagaraDataInterfaceOpenSplat4D::GetTimeWeight(FVectorVMExternalFunctionContext& Context)
{
	VectorVM::FExternalFuncInputHandler<int32> InIndex(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OutWeight(Context);
	if (!PointCloud) return;
	const TArray<FOpenSplat4DPoint>& Points = PointCloud->GetPoints();
	for (int32 InstanceIdx = 0; InstanceIdx < Context.GetNumInstances(); ++InstanceIdx)
	{
		const FOpenSplat4DPoint& P = Points[FMath::Clamp(InIndex.Get(), 0, Points.Num() - 1)];
		const float W = bTemporalWeighting
			? UOpenSplat4DPointCloud::GetTimeWeight(P.AnchorTime, P.TimeVariance, Time)
			: 1.f;
		*OutWeight.GetDestAndAdvance() = W;
		InIndex.Advance();
	}
}

void UNiagaraDataInterfaceOpenSplat4D::GetTimeOffset(FVectorVMExternalFunctionContext& Context)
{
	VectorVM::FExternalFuncInputHandler<int32> InIndex(Context);
	VectorVM::FExternalFuncRegisterHandler<float> OffX(Context), OffY(Context), OffZ(Context);
	if (!PointCloud) return;
	const TArray<FOpenSplat4DPoint>& Points = PointCloud->GetPoints();
	for (int32 InstanceIdx = 0; InstanceIdx < Context.GetNumInstances(); ++InstanceIdx)
	{
		const FOpenSplat4DPoint& P = Points[FMath::Clamp(InIndex.Get(), 0, Points.Num() - 1)];
		FVector3f Offset = FVector3f::ZeroVector;
		if (bTemporalWeighting && P.bUseVelocity)
		{
			Offset = P.Velocity * (Time - P.AnchorTime);
		}
		*OffX.GetDestAndAdvance() = Offset.X;
		*OffY.GetDestAndAdvance() = Offset.Y;
		*OffZ.GetDestAndAdvance() = Offset.Z;
		InIndex.Advance();
	}
}

void UNiagaraDataInterfaceOpenSplat4D::GetVMExternalFunction(const FVMExternalFunctionBindingInfo& BindingInfo, void* InstanceData, FVMExternalFunction& OutFunc)
{
	if (BindingInfo.Name == GetPointDataFunctionName)
		NDI_FUNC_BINDER(UNiagaraDataInterfaceOpenSplat4D, GetPointData)::Bind(this, OutFunc);
	else if (BindingInfo.Name == GetPointData4DFunctionName)
		NDI_FUNC_BINDER(UNiagaraDataInterfaceOpenSplat4D, GetPointData4D)::Bind(this, OutFunc);
	else if (BindingInfo.Name == GetPointCountFunctionName)
		NDI_FUNC_BINDER(UNiagaraDataInterfaceOpenSplat4D, GetPointCount)::Bind(this, OutFunc);
	else if (BindingInfo.Name == GetTimeWeightFunctionName)
		NDI_FUNC_BINDER(UNiagaraDataInterfaceOpenSplat4D, GetTimeWeight)::Bind(this, OutFunc);
	else if (BindingInfo.Name == GetTimeOffsetFunctionName)
		NDI_FUNC_BINDER(UNiagaraDataInterfaceOpenSplat4D, GetTimeOffset)::Bind(this, OutFunc);
	else
		ensureMsgf(false, TEXT("OpenSplat4D: function defined for this class but not bound."));
}

#if WITH_EDITORONLY_DATA
bool UNiagaraDataInterfaceOpenSplat4D::AppendCompileHash(FNiagaraCompileHashVisitor* InVisitor) const
{
	bool bSuccess = Super::AppendCompileHash(InVisitor);
	bSuccess &= InVisitor->UpdateShaderParameters<FShaderParameters>();
	return bSuccess;
}

bool UNiagaraDataInterfaceOpenSplat4D::GetFunctionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, const FNiagaraDataInterfaceGeneratedFunction& FunctionInfo, int FunctionInstanceIndex, FString& OutHLSL)
{
	bool ParentRet = Super::GetFunctionHLSL(ParamInfo, FunctionInfo, FunctionInstanceIndex, OutHLSL);
	if (ParentRet) return true;

	const FString Symbol = ParamInfo.DataInterfaceHLSLSymbol;
	const FString PointCount = Symbol + PointCountName;
	const FString Buffer = Symbol + PointDataBufferName;
	const FString TimeSym = Symbol + TimeName;
	const FString TW = Symbol + TemporalWeightingName;

	if (FunctionInfo.DefinitionName == GetPointDataFunctionName)
	{
		static const TCHAR* Fmt = TEXT(R"(
			void {FunctionName}(int In_Index, out float3 Out_Position, out float4 Out_Quat, out float3 Out_Scale, out float4 Out_Color)
			{
				int idx = In_Index < {PointCount} ? In_Index : {PointCount} - 1;
				Out_Position = {Buffer}.Load(idx * {Stride} + 0).xyz;
				Out_Quat = {Buffer}.Load(idx * {Stride} + 1);
				Out_Scale = {Buffer}.Load(idx * {Stride} + 2).xyz;
				Out_Color = {Buffer}.Load(idx * {Stride} + 3);
			}
		)");
		OutHLSL += FString::Format(Fmt, { { TEXT("FunctionName"), FunctionInfo.InstanceName }, { TEXT("PointCount"), PointCount }, { TEXT("Buffer"), Buffer }, { TEXT("Stride"), GOPEN_SPLAT_FLOAT4_PER_POINT } });
		return true;
	}
	else if (FunctionInfo.DefinitionName == GetPointData4DFunctionName)
	{
		static const TCHAR* Fmt = TEXT(R"(
			void {FunctionName}(int In_Index, out float3 Out_Position, out float4 Out_Quat, out float3 Out_Scale, out float4 Out_Color, out float Out_AnchorTime, out float Out_TimeVariance, out float3 Out_Velocity)
			{
				int idx = In_Index < {PointCount} ? In_Index : {PointCount} - 1;
				Out_Position = {Buffer}.Load(idx * {Stride} + 0).xyz;
				Out_Quat = {Buffer}.Load(idx * {Stride} + 1);
				Out_Scale = {Buffer}.Load(idx * {Stride} + 2).xyz;
				Out_Color = {Buffer}.Load(idx * {Stride} + 3);
				Out_AnchorTime = {Buffer}.Load(idx * {Stride} + 0).w;
				Out_TimeVariance = {Buffer}.Load(idx * {Stride} + 2).w;
				Out_Velocity = {Buffer}.Load(idx * {Stride} + 4).xyz;
			}
		)");
		OutHLSL += FString::Format(Fmt, { { TEXT("FunctionName"), FunctionInfo.InstanceName }, { TEXT("PointCount"), PointCount }, { TEXT("Buffer"), Buffer }, { TEXT("Stride"), GOPEN_SPLAT_FLOAT4_PER_POINT } });
		return true;
	}
	else if (FunctionInfo.DefinitionName == GetPointCountFunctionName)
	{
		static const TCHAR* Fmt = TEXT(R"(
			void {FunctionName}(out int Out_Val)
			{
				Out_Val = {PointCount};
			}
		)");
		OutHLSL += FString::Format(Fmt, { { TEXT("FunctionName"), FunctionInfo.InstanceName }, { TEXT("PointCount"), PointCount } });
		return true;
	}
	else if (FunctionInfo.DefinitionName == GetTimeWeightFunctionName)
	{
		static const TCHAR* Fmt = TEXT(R"(
			void {FunctionName}(int In_Index, out float Out_Weight)
			{
				if ({TW} == 0) { Out_Weight = 1.0f; return; }
				int idx = In_Index < {PointCount} ? In_Index : {PointCount} - 1;
				float aT = {Buffer}.Load(idx * {Stride} + 0).w;
				float aVar = {Buffer}.Load(idx * {Stride} + 2).w;
				float sig = max(aVar, 1e-6);
				float dt = aT - {Time};
				Out_Weight = exp(-0.5 * dt * dt / sig);
			}
		)");
		OutHLSL += FString::Format(Fmt, {
			{ TEXT("FunctionName"), FunctionInfo.InstanceName },
			{ TEXT("PointCount"), PointCount },
			{ TEXT("Buffer"), Buffer },
			{ TEXT("Stride"), GOPEN_SPLAT_FLOAT4_PER_POINT },
			{ TEXT("Time"), TimeSym },
			{ TEXT("TW"), TW } });
		return true;
	}
	else if (FunctionInfo.DefinitionName == GetTimeOffsetFunctionName)
	{
		static const TCHAR* Fmt = TEXT(R"(
			void {FunctionName}(int In_Index, out float3 Out_Offset)
			{
				if ({TW} == 0) { Out_Offset = float3(0, 0, 0); return; }
				int idx = In_Index < {PointCount} ? In_Index : {PointCount} - 1;
				float3 vel = {Buffer}.Load(idx * {Stride} + 4).xyz;
				float useVel = {Buffer}.Load(idx * {Stride} + 4).w;
				float aT = {Buffer}.Load(idx * {Stride} + 0).w;
				Out_Offset = useVel * vel * ({Time} - aT);
			}
		)");
		OutHLSL += FString::Format(Fmt, {
			{ TEXT("FunctionName"), FunctionInfo.InstanceName },
			{ TEXT("PointCount"), PointCount },
			{ TEXT("Buffer"), Buffer },
			{ TEXT("Stride"), GOPEN_SPLAT_FLOAT4_PER_POINT },
			{ TEXT("Time"), TimeSym },
			{ TEXT("TW"), TW } });
		return true;
	}
	return false;
}

void UNiagaraDataInterfaceOpenSplat4D::GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, FString& OutHLSL)
{
	Super::GetParameterDefinitionHLSL(ParamInfo, OutHLSL);
	static const TCHAR* Fmt = TEXT(R"(
		int {PointCountName};
		Buffer<float4> {PointDataBufferName};
		float {TimeName};
		int {TemporalWeightingName};
	)");
	TMap<FString, FStringFormatArg> Args = {
		{ TEXT("PointCountName"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + PointCountName) },
		{ TEXT("PointDataBufferName"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + PointDataBufferName) },
		{ TEXT("TimeName"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + TimeName) },
		{ TEXT("TemporalWeightingName"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + TemporalWeightingName) },
	};
	OutHLSL += FString::Format(Fmt, Args);
}
#endif

void UNiagaraDataInterfaceOpenSplat4D::BuildShaderParameters(FNiagaraShaderParametersBuilder& ShaderParametersBuilder) const
{
	ShaderParametersBuilder.AddNestedStruct<FShaderParameters>();
}

void UNiagaraDataInterfaceOpenSplat4D::SetShaderParameters(const FNiagaraDataInterfaceSetShaderParametersContext& Context) const
{
	FNiagaraDataInterfaceProxyOpenSplat4D& DIProxy = Context.GetProxy<FNiagaraDataInterfaceProxyOpenSplat4D>();
	DIProxy.TryUpdateBuffer();
	UNiagaraDataInterfaceOpenSplat4D* Current = DIProxy.Owner;
	FShaderParameters* ShaderParameters = Context.GetParameterNestedStruct<FShaderParameters>();
	ShaderParameters->PointCount = Current->PointCloud ? Current->PointCloud->GetPointCount() : 0;
	ShaderParameters->PointDataBuffer = FNiagaraRenderer::GetSrvOrDefaultFloat4(DIProxy.OpenSplat4DPointDataBuffer.SRV);
	ShaderParameters->Time = Current->Time;
	ShaderParameters->bTemporalWeighting = Current->bTemporalWeighting ? 1 : 0;
}

bool UNiagaraDataInterfaceOpenSplat4D::Equals(const UNiagaraDataInterface* Other) const
{
	if (!Super::Equals(Other))
	{
		return false;
	}
	const UNiagaraDataInterfaceOpenSplat4D* OtherDI = CastChecked<const UNiagaraDataInterfaceOpenSplat4D>(Other);
	return OtherDI->PointCloud == PointCloud;
}

#if WITH_EDITOR
void UNiagaraDataInterfaceOpenSplat4D::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	static const FName PointCloudFName = GET_MEMBER_NAME_CHECKED(UNiagaraDataInterfaceOpenSplat4D, PointCloud);
	if (!HasAnyFlags(RF_ClassDefaultObject) && PropertyChangedEvent.GetMemberPropertyName() == PointCloudFName)
	{
		GetProxyAs<FNiagaraDataInterfaceProxyOpenSplat4D>()->MakeBufferDirty();
	}
}
#endif

void UNiagaraDataInterfaceOpenSplat4D::PostInitProperties()
{
	Super::PostInitProperties();
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		ENiagaraTypeRegistryFlags Flags = ENiagaraTypeRegistryFlags::AllowAnyVariable | ENiagaraTypeRegistryFlags::AllowParameter;
		FNiagaraTypeRegistry::Register(FNiagaraTypeDefinition(GetClass()), Flags);
	}
	else
	{
		GetProxyAs<FNiagaraDataInterfaceProxyOpenSplat4D>()->MakeBufferDirty();
	}
}

void UNiagaraDataInterfaceOpenSplat4D::PostLoad()
{
	Super::PostLoad();
}

bool UNiagaraDataInterfaceOpenSplat4D::CopyToInternal(UNiagaraDataInterface* Destination) const
{
	if (!Super::CopyToInternal(Destination))
		return false;
	UNiagaraDataInterfaceOpenSplat4D* Dst = Cast<UNiagaraDataInterfaceOpenSplat4D>(Destination);
	if (Dst)
	{
		Dst->PointCloud = PointCloud;
		Dst->Time = Time;
		Dst->bTemporalWeighting = bTemporalWeighting;
	}
	return true;
}

const FName UNiagaraDataInterfaceOpenSplat4D::GetPointDataFunctionName(TEXT("GetPointData"));
const FName UNiagaraDataInterfaceOpenSplat4D::GetPointCountFunctionName(TEXT("GetPointCount"));
const FName UNiagaraDataInterfaceOpenSplat4D::GetPointData4DFunctionName(TEXT("GetPointData4D"));
const FName UNiagaraDataInterfaceOpenSplat4D::GetTimeWeightFunctionName(TEXT("GetTimeWeight"));
const FName UNiagaraDataInterfaceOpenSplat4D::GetTimeOffsetFunctionName(TEXT("GetTimeOffset"));

const FString UNiagaraDataInterfaceOpenSplat4D::PointCountName(TEXT("_PointCount"));
const FString UNiagaraDataInterfaceOpenSplat4D::PointDataBufferName(TEXT("_PointDataBuffer"));
const FString UNiagaraDataInterfaceOpenSplat4D::TimeName(TEXT("Time"));
const FString UNiagaraDataInterfaceOpenSplat4D::TemporalWeightingName(TEXT("bTemporalWeighting"));

#undef LOCTEXT_NAMESPACE
