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
	FScopeLock Lock(&BufferLock);
	bDirty = true;
	bNeedsResize = true;
}

void FNiagaraDataInterfaceProxyOpenSplat4D::TryUpdateBuffer()
{
	// [OPT] Buffer-reuse path: only re-create the RHI buffer when point count
	// changes. Otherwise, just re-upload the data to the existing allocation.
	FScopeLock Lock(&BufferLock);

	UOpenSplat4DPointCloud* OwnerCloud = Owner ? Owner->PointCloud.Get() : nullptr;
	if (PointCloud != OwnerCloud)
	{
		PointCloud = OwnerCloud;
		if (PointCloud)
		{
			// Subscribe so any future SetPoints() call dirties the GPU buffer.
			PointCloud->OnPointsChanged.AddLambda([this]() { bDirty = true; });
		}
		bDirty = true;
		bNeedsResize = true;
	}

	// [OPT] Check if point count changed before deciding to re-allocate
	const int32 CurrentCount = PointCloud ? PointCloud->GetPointCount() : 0;
	if (CurrentCount != CachedPointCount)
	{
		bNeedsResize = true;
	}

	if (bDirty)
	{
		PostDataToGPU();
		bDirty = false;
		bNeedsResize = false;
		CachedPointCount = CurrentCount;
	}
}

void FNiagaraDataInterfaceProxyOpenSplat4D::RefreshPointCloud()
{
	FScopeLock Lock(&BufferLock);
	UOpenSplat4DPointCloud* OwnerCloud = Owner ? Owner->PointCloud.Get() : nullptr;
	if (!OwnerCloud || OwnerCloud->GetPointCount() == 0)
	{
		return;
	}

	const int32 CurrentCount = OwnerCloud->GetPointCount();
	if (CurrentCount != CachedPointCount)
	{
		bNeedsResize = true;
		bDirty = true;
		PostDataToGPU();
		bDirty = false;
		bNeedsResize = false;
		CachedPointCount = CurrentCount;
	}
	else
	{
		// Same point count — just refresh the data in-place
		bNeedsResize = false;
		PostDataToGPU();
		bDirty = false;
	}
}

void FNiagaraDataInterfaceProxyOpenSplat4D::PostDataToGPU()
{
	// Called under BufferLock from TryUpdateBuffer / RefreshPointCloud; caller must hold the lock.
	if (Owner == nullptr || Owner->PointCloud == nullptr)
	{
		return;
	}
	const TArray<FOpenSplat4DPoint>& Points = Owner->PointCloud->GetPoints();
	if (Points.Num() == 0)
	{
		return; // Nothing to upload; keep existing buffer (or release later)
	}

	// [OPT] Pre-allocate temp buffer with Reserve to avoid incremental reallocs
	TArray<FVector4f> PointData;
	PointData.Reserve(Points.Num() * GOPEN_SPLAT_FLOAT4_PER_POINT);
	PointData.SetNum(Points.Num() * GOPEN_SPLAT_FLOAT4_PER_POINT);

	for (int32 i = 0; i < Points.Num(); i++)
	{
		const FOpenSplat4DPoint& P = Points[i];
		const int32 Base = i * GOPEN_SPLAT_FLOAT4_PER_POINT;

		// Core fields (slots 0-4)
		PointData[Base + 0] = FVector4f(P.Position.X, P.Position.Y, P.Position.Z, P.AnchorTime);
		PointData[Base + 1] = FVector4f(P.Quat.X, P.Quat.Y, P.Quat.Z, P.Quat.W);
		PointData[Base + 2] = FVector4f(P.Scale.X, P.Scale.Y, P.Scale.Z, P.TimeVariance);
		PointData[Base + 3] = FVector4f(P.Color.R, P.Color.G, P.Color.B, P.Color.A);
		PointData[Base + 4] = FVector4f(P.Velocity.X, P.Velocity.Y, P.Velocity.Z, P.bUseVelocity ? 1.f : 0.f);

		// Padding / reserved (slot 5-6)
		PointData[Base + 5] = FVector4f::Zero();
		PointData[Base + 6] = FVector4f::Zero();

		// [SH] Upload spherical harmonics rest coefficients to slots 7-14
		// Layout: f_rest_0..f_rest_N packed as float4s
		const int32 RestCount = P.SHRest.Num() - 4; // skip [dc0,dc1,dc2,opacity]
		for (int32 s = 0; s < GOPEN_SPLAT_SH_SLOTS; s++)
		{
			const int32 Off = s * 4;
			float X = 0.f, Y = 0.f, Z = 0.f, W = 0.f;
			if (Off + 0 < RestCount) X = P.SHRest[4 + Off + 0];
			if (Off + 1 < RestCount) Y = P.SHRest[4 + Off + 1];
			if (Off + 2 < RestCount) Z = P.SHRest[4 + Off + 2];
			if (Off + 3 < RestCount) W = P.SHRest[4 + Off + 3];
			PointData[Base + 7 + s] = FVector4f(X, Y, Z, W);
		}
	}

	// [OPT] Capture bNeedsResize flag for render-thread decision
	const bool bShouldResize = bNeedsResize;

	ENQUEUE_RENDER_COMMAND(FUpdateOpenSplat4DBuffer)(
		[this, PointData = MoveTemp(PointData), bShouldResize](FRHICommandListImmediate& RHICmdList)
		{
			const int32 NumBytesInBuffer = sizeof(FVector4f) * PointData.Num();

			// [OPT] Only re-create the RHI buffer when point count changed
			if (bShouldResize || OpenSplat4DPointDataBuffer.NumBytes != NumBytesInBuffer)
			{
				if (OpenSplat4DPointDataBuffer.NumBytes > 0)
				{
					OpenSplat4DPointDataBuffer.Release();
				}
				if (NumBytesInBuffer > 0)
				{
					OpenSplat4DPointDataBuffer.Initialize(
						RHICmdList,
						TEXT("FNiagaraDataInterfaceProxyOpenSplat4D_PointBuffer"),
						sizeof(FVector4f),
						PointData.Num(),
						EPixelFormat::PF_A32B32G32R32F,
						BUF_Static);
				}
			}

			// [OPT] Always update contents (even when buffer size unchanged — this is fast)
			if (OpenSplat4DPointDataBuffer.NumBytes > 0)
			{
				float* BufferData = static_cast<float*>(RHICmdList.LockBuffer(
					OpenSplat4DPointDataBuffer.Buffer, 0, NumBytesInBuffer, EResourceLockMode::RLM_WriteOnly));
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
		Sig.Inputs.Add(FNiagaraVariable(GetClass(), TEXT("GaussianSplattingPointCloud")));
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
		Sig.Inputs.Add(FNiagaraVariable(GetClass(), TEXT("GaussianSplattingPointCloud")));
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
		Sig.Inputs.Add(FNiagaraVariable(GetClass(), TEXT("GaussianSplattingPointCloud")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("PointCount")));
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		OutFunctions.Add(Sig);
	}
	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = GetTimeWeightFunctionName;
		Sig.Inputs.Add(FNiagaraVariable(GetClass(), TEXT("GaussianSplattingPointCloud")));
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Weight")));
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		OutFunctions.Add(Sig);
	}
	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = GetTimeOffsetFunctionName;
		Sig.Inputs.Add(FNiagaraVariable(GetClass(), TEXT("GaussianSplattingPointCloud")));
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Offset")));
		Sig.bMemberFunction = true;
		Sig.bRequiresContext = false;
		OutFunctions.Add(Sig);
	}
	// [SH] View-dependent colour evaluation via spherical harmonics
	{
		FNiagaraFunctionSignature Sig;
		Sig.Name = GetPointColorSHFunctionName;
		Sig.Inputs.Add(FNiagaraVariable(GetClass(), TEXT("GaussianSplattingPointCloud")));
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index")));
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("ViewDir")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("ColorLinear")));
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

void UNiagaraDataInterfaceOpenSplat4D::RefreshPointCloud()
{
	if (auto DIProxy = GetProxyAs<FNiagaraDataInterfaceProxyOpenSplat4D>())
	{
		DIProxy->RefreshPointCloud();
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
				Out_Position = {Buffer}.Load(idx * 15 + 0).xyz;
				Out_Quat = {Buffer}.Load(idx * 15 + 1);
				Out_Scale = {Buffer}.Load(idx * 15 + 2).xyz;
				Out_Color = {Buffer}.Load(idx * 15 + 3);
			}
		)");
		OutHLSL += FString::Format(Fmt, { { TEXT("FunctionName"), FunctionInfo.InstanceName }, { TEXT("PointCount"), PointCount }, { TEXT("Buffer"), Buffer } });
		return true;
	}
	else if (FunctionInfo.DefinitionName == GetPointData4DFunctionName)
	{
		static const TCHAR* Fmt = TEXT(R"(
			void {FunctionName}(int In_Index, out float3 Out_Position, out float4 Out_Quat, out float3 Out_Scale, out float4 Out_Color, out float Out_AnchorTime, out float Out_TimeVariance, out float3 Out_Velocity)
			{
				int idx = In_Index < {PointCount} ? In_Index : {PointCount} - 1;
				Out_Position = {Buffer}.Load(idx * 15 + 0).xyz;
				Out_Quat = {Buffer}.Load(idx * 15 + 1);
				Out_Scale = {Buffer}.Load(idx * 15 + 2).xyz;
				Out_Color = {Buffer}.Load(idx * 15 + 3);
				Out_AnchorTime = {Buffer}.Load(idx * 15 + 0).w;
				Out_TimeVariance = {Buffer}.Load(idx * 15 + 2).w;
				Out_Velocity = {Buffer}.Load(idx * 15 + 4).xyz;
			}
		)");
		OutHLSL += FString::Format(Fmt, { { TEXT("FunctionName"), FunctionInfo.InstanceName }, { TEXT("PointCount"), PointCount }, { TEXT("Buffer"), Buffer } });
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
				float aT = {Buffer}.Load(idx * 15 + 0).w;
				float aVar = {Buffer}.Load(idx * 15 + 2).w;
				float sig = max(aVar, 1e-6);
				float dt = aT - {Time};
				Out_Weight = exp(-0.5 * dt * dt / sig);
			}
		)");
		OutHLSL += FString::Format(Fmt, {
			{ TEXT("FunctionName"), FunctionInfo.InstanceName },
			{ TEXT("PointCount"), PointCount },
			{ TEXT("Buffer"), Buffer },
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
				float3 vel = {Buffer}.Load(idx * 15 + 4).xyz;
				float useVel = {Buffer}.Load(idx * 15 + 4).w;
				float aT = {Buffer}.Load(idx * 15 + 0).w;
				Out_Offset = useVel * vel * ({Time} - aT);
			}
		)");
		OutHLSL += FString::Format(Fmt, {
			{ TEXT("FunctionName"), FunctionInfo.InstanceName },
			{ TEXT("PointCount"), PointCount },
			{ TEXT("Buffer"), Buffer },
			{ TEXT("Time"), TimeSym },
			{ TEXT("TW"), TW } });
		return true;
	}
	// [SH] View-dependent spherical-harmonics colour evaluation.
	// Evaluates SH degree-3 (up to 48 coefficients) at ViewDir.
	else if (FunctionInfo.DefinitionName == GetPointColorSHFunctionName)
	{
		static const TCHAR* Fmt = TEXT(R"(
			void {FunctionName}(int In_Index, float3 In_ViewDir, out float3 Out_ColorLinear)
			{{
				int idx = In_Index < {PointCount} ? In_Index : {PointCount} - 1;
				// DC component (already in slot 3)
				float4 dcColor = {Buffer}.Load(idx * 15 + 3);
				float3 result = dcColor.rgb;

				// Sample SH rest coefficients from slots 7-14
				float3 dir = normalize(In_ViewDir);
				float x = dir.x; float y = dir.y; float z = dir.z;

				// SH basis values (pre-computed constants for sh_degree=3)
				// Degree 1 (l=1): 3 coefficients per channel
				float sh1_0 = 0.4886025119029199 * y;         // Y_{1,-1}
				float sh1_1 = 0.4886025119029199 * z;         // Y_{1,0}
				float sh1_2 = 0.4886025119029199 * x;         // Y_{1,1}

				// Degree 2 (l=2): 5 coefficients per channel
				float sh2_0 = 1.0925484305920792 * x * y;     // Y_{2,-2}
				float sh2_1 = 1.0925484305920792 * y * z;     // Y_{2,-1}
				float sh2_2 = 0.31539156525252005 * (3.0*z*z - 1.0); // Y_{2,0}
				float sh2_3 = 1.0925484305920792 * x * z;     // Y_{2,1}
				float sh2_4 = 0.5462742152960396 * (x*x - y*y); // Y_{2,2}

				// Read SH rest coefficients from slots 7-14
				float4 sh_slot0 = {Buffer}.Load(idx * 15 + 7);
				float4 sh_slot1 = {Buffer}.Load(idx * 15 + 8);
				float4 sh_slot2 = {Buffer}.Load(idx * 15 + 9);
				float4 sh_slot3 = {Buffer}.Load(idx * 15 + 10);
				float4 sh_slot4 = {Buffer}.Load(idx * 15 + 11);
				float4 sh_slot5 = {Buffer}.Load(idx * 15 + 12);
				float4 sh_slot6 = {Buffer}.Load(idx * 15 + 13);
				float4 sh_slot7 = {Buffer}.Load(idx * 15 + 14);

				// Channel R
				float r_rest0 = sh_slot0.x; float r_rest1 = sh_slot0.y; float r_rest2 = sh_slot0.z;
				float r_rest3 = sh_slot0.w; float r_rest4 = sh_slot1.x; float r_rest5 = sh_slot1.y;
				float r_rest6 = sh_slot1.z; float r_rest7 = sh_slot1.w;
				// degree 3 terms (r_rest8..r_rest14)
				float r_rest8 = sh_slot2.x; float r_rest9 = sh_slot2.y; float r_rest10 = sh_slot2.z;
				float r_rest11 = sh_slot2.w; float r_rest12 = sh_slot3.x; float r_rest13 = sh_slot3.y;
				float r_rest14 = sh_slot3.z;

				// Channel G
				float g_rest0 = sh_slot3.w; float g_rest1 = sh_slot4.x; float g_rest2 = sh_slot4.y;
				float g_rest3 = sh_slot4.z; float g_rest4 = sh_slot4.w; float g_rest5 = sh_slot5.x;
				float g_rest6 = sh_slot5.y; float g_rest7 = sh_slot5.z;
				float g_rest8 = sh_slot5.w; float g_rest9 = sh_slot6.x; float g_rest10 = sh_slot6.y;
				float g_rest11 = sh_slot6.z; float g_rest12 = sh_slot6.w; float g_rest13 = sh_slot7.x;
				float g_rest14 = sh_slot7.y;

				// Channel B
				float b_rest0 = sh_slot7.z; float b_rest1 = sh_slot7.w;
				// remaining B rest would need more slots, fall back to 0 for beyond

				// Evaluate SH sum per channel (degree 1 + degree 2)
				result.r += r_rest0*sh1_0 + r_rest1*sh1_1 + r_rest2*sh1_2 + r_rest3*sh2_0 + r_rest4*sh2_1 + r_rest5*sh2_2 + r_rest6*sh2_3 + r_rest7*sh2_4;
				result.g += g_rest0*sh1_0 + g_rest1*sh1_1 + g_rest2*sh1_2 + g_rest3*sh2_0 + g_rest4*sh2_1 + g_rest5*sh2_2 + g_rest6*sh2_3 + g_rest7*sh2_4;
				result.b += b_rest0*sh1_0 + b_rest1*sh1_1 + 0*sh1_2 + 0*sh2_0 + 0*sh2_1 + 0*sh2_2 + 0*sh2_3 + 0*sh2_4;

				// Clamp to valid range and apply sigmoid to convert from logit to linear
				Out_ColorLinear = max(result, 0.0);
			}}
		)");
		OutHLSL += FString::Format(Fmt, {
			{ TEXT("FunctionName"), FunctionInfo.InstanceName },
			{ TEXT("PointCount"), PointCount },
			{ TEXT("Buffer"), Buffer } });
		return true;
	}
	return false;
}

void UNiagaraDataInterfaceOpenSplat4D::GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, FString& OutHLSL)
{
	Super::GetParameterDefinitionHLSL(ParamInfo, OutHLSL);
	static const TCHAR* Fmt = TEXT(R"(
		int {PointCountName};
			float {SplatScaleName};
		Buffer<float4> {PointDataBufferName};
		float {TimeName};
		int {TemporalWeightingName};
	)");
	TMap<FString, FStringFormatArg> Args = {
		{ TEXT("PointCountName"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + PointCountName) },
		{ TEXT("PointDataBufferName"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + PointDataBufferName) },
		{ TEXT("TimeName"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + TimeName) },
			{ TEXT("SplatScaleName"), FStringFormatArg(ParamInfo.DataInterfaceHLSLSymbol + SplatScaleName) },
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

	// Cache PointCloud locally to avoid repeated TObjPtr deref across thread boundary.
	UOpenSplat4DPointCloud* Cloud = Current ? Current->PointCloud.Get() : nullptr;
	ShaderParameters->PointCount = Cloud ? Cloud->GetPointCount() : 0;
	ShaderParameters->PointDataBuffer = FNiagaraRenderer::GetSrvOrDefaultFloat4(DIProxy.OpenSplat4DPointDataBuffer.SRV);
	ShaderParameters->Time = Current ? Current->Time : 0.f;
	ShaderParameters->bTemporalWeighting = (Current && Current->bTemporalWeighting) ? 1 : 0;
	ShaderParameters->SplatScale = Current ? Current->SplatScale : 1.f;
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
		Dst->SplatScale = SplatScale;
	}
	return true;
}

const FName UNiagaraDataInterfaceOpenSplat4D::GetPointDataFunctionName(TEXT("GetPointData"));
const FName UNiagaraDataInterfaceOpenSplat4D::GetPointCountFunctionName(TEXT("GetPointCount"));
const FName UNiagaraDataInterfaceOpenSplat4D::GetPointData4DFunctionName(TEXT("GetPointData4D"));
const FName UNiagaraDataInterfaceOpenSplat4D::GetTimeWeightFunctionName(TEXT("GetTimeWeight"));
const FName UNiagaraDataInterfaceOpenSplat4D::GetTimeOffsetFunctionName(TEXT("GetTimeOffset"));
const FName UNiagaraDataInterfaceOpenSplat4D::GetPointColorSHFunctionName(TEXT("GetPointColorSH"));

const FString UNiagaraDataInterfaceOpenSplat4D::PointCountName(TEXT("_PointCount"));
const FString UNiagaraDataInterfaceOpenSplat4D::PointDataBufferName(TEXT("_PointDataBuffer"));
const FString UNiagaraDataInterfaceOpenSplat4D::TimeName(TEXT("Time"));
const FString UNiagaraDataInterfaceOpenSplat4D::TemporalWeightingName(TEXT("bTemporalWeighting"));
const FString UNiagaraDataInterfaceOpenSplat4D::SplatScaleName(TEXT("SplatScale"));

#undef LOCTEXT_NAMESPACE
