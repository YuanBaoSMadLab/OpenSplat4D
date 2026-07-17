#include "OpenSplat4DBillboardComponent.h"
#include "OpenSplat4DPointCloud.h"

#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "PipelineStateCache.h"
#include "RHI.h"
#include "RHIUtilities.h"
#include "RHIStaticStates.h"
#include "RenderingThread.h"
#include "PrimitiveSceneProxy.h"

#include "SceneView.h"
#include "SceneViewExtension.h"
#include "RenderGraphBuilder.h"
#include "SceneTexturesConfig.h"
#include "HAL/IConsoleManager.h"

// ----------------------------------------------------------------------------
// Shader parameters
// ----------------------------------------------------------------------------
// Packed GPU layout: 7 x float4 per point (see OpenSplat4DBillboard.usf).
BEGIN_SHADER_PARAMETER_STRUCT(FOpenSplat4DDrawParameters, )
	RENDER_TARGET_BINDING_SLOTS()
	SHADER_PARAMETER(FMatrix44f, ViewProjection)
	SHADER_PARAMETER(FVector4f, CameraRight)
	SHADER_PARAMETER(FVector4f, CameraUp)
	SHADER_PARAMETER(float, Time)
	SHADER_PARAMETER(int32, bTemporalWeighting)
	SHADER_PARAMETER(int32, PointCount)
	SHADER_PARAMETER(float, SplatScale)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Points)
END_SHADER_PARAMETER_STRUCT()

class FSplatVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSplatVS);
	SHADER_USE_PARAMETER_STRUCT(FSplatVS, FGlobalShader);
	using FParameters = FOpenSplat4DDrawParameters;
};

class FSplatPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSplatPS);
	SHADER_USE_PARAMETER_STRUCT(FSplatPS, FGlobalShader);
	using FParameters = FOpenSplat4DDrawParameters;
};

IMPLEMENT_GLOBAL_SHADER(FSplatVS, "/Plugin/OpenSplat4D/Private/OpenSplat4DBillboard.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FSplatPS, "/Plugin/OpenSplat4D/Private/OpenSplat4DBillboard.usf", "MainPS", SF_Pixel);

/** Maximum allowed screen-space feature size (0 = unlimited). Points whose projected
 *  size exceeds this are culled on the CPU before upload (LOD for very large gaussians). */
static TAutoConsoleVariable<float> CVarOpenSplat4DMaxFeatureSize(
	TEXT("r.OpenSplat4D.MaxFeatureSize"),
	0.0f,
	TEXT("Maximum allowed screen-space feature size (0 = unlimited). Gaussians whose projected size exceeds this are culled."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

// Empty vertex declaration for SV_VertexID-only draws.
static FVertexDeclarationRHIRef GOpenSplatEmptyVertexDeclaration;

// When 0, the splats draw as a pure overlay with no depth test (handy to confirm
// the pipeline is alive even if depth/occlusion is misbehaving). Default 1 = correct.
static TAutoConsoleVariable<int32> CVarOpenSplat4DDepthTest(
	TEXT("r.OpenSplat4D.DepthTest"),
	1,
	TEXT("0 = draw splats ignoring the depth buffer (overlay); 1 = normal depth test."),
	ECVF_RenderThreadSafe);

// One-time diagnostics so a missing preview is easy to triage in the Output Log.
static bool GOpenSplatLoggedEmpty = false;
static bool GOpenSplatLoggedDraw = false;

// ----------------------------------------------------------------------------
// Scene view extension -- draws the splats *inside* the base pass render target
// so the output is actually rasterized (draws outside a pass are dropped on
// D3D12/Vulkan).
// ----------------------------------------------------------------------------
class FOpenSplat4DSceneViewExtension : public FSceneViewExtensionBase
{
public:
	FOpenSplat4DSceneViewExtension(const FAutoRegister& AutoRegister, UOpenSplat4DBillboardComponent* InComponent)
		: FSceneViewExtensionBase(AutoRegister)
		, Component(InComponent)
	{
	}

	virtual void PostRenderBasePassDeferred_RenderThread(
		FRDGBuilder& GraphBuilder,
		FSceneView& InView,
		const FRenderTargetBindingSlots& RenderTargets,
		TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;

	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override
	{
		UOpenSplat4DBillboardComponent* Comp = Component.Get();
		if (!Comp || !Comp->PointCloud)
		{
			return false;
		}
		if (Comp->PointCloud->GetPointCount() <= 0)
		{
			// [FIX] Previously the "cloud is empty" warning lived inside
			// PostRenderBasePassDeferred, which is only invoked when this method
			// returns true -- so the warning could never fire for an empty cloud,
			// leaving the user with a silent blank viewport. Emit it here instead.
			static bool bLoggedEmptyOnce = false;
			if (!bLoggedEmptyOnce)
			{
				bLoggedEmptyOnce = true;
				UE_LOG(LogOpenSplat4D, Warning,
					TEXT("OpenSplat4D: point cloud has 0 points -> splat extension inactive (blank preview). Re-import the source .ply/.4dgs, or run 'OpenSplat4D.Reload <path>' in the console."));
			}
			return false;
		}
		// Stay active for every view that includes this extension (level viewport,
		// asset-editor preview, etc.). The per-component world transform keeps each
		// cloud in its own space, and PostRenderBasePassDeferred already skips scene
		// captures, so we no longer gate on Context.GetWorld() -- that check wrongly
		// disabled rendering inside the asset-editor's preview scene (whose context
		// world does not match the component's owning world).
		return true;
	}

private:
	TWeakObjectPtr<UOpenSplat4DBillboardComponent> Component;
};

void FOpenSplat4DSceneViewExtension::PostRenderBasePassDeferred_RenderThread(
	FRDGBuilder& GraphBuilder,
	FSceneView& InView,
	const FRenderTargetBindingSlots& RenderTargets,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures)
{
	UOpenSplat4DBillboardComponent* Comp = Component.Get();
	if (!Comp || !Comp->PointCloud || Comp->PointCloud->GetPointCount() <= 0)
	{
		if (!GOpenSplatLoggedEmpty)
		{
			GOpenSplatLoggedEmpty = true;
			UE_LOG(LogOpenSplat4D, Warning,
				TEXT("OpenSplat4D: 渲染管线已触发，但点云为空（0 点）——预览空白通常是资产未装入点，请重新导入/重导入源文件。"));
		}
		return;
	}

	// Skip splats for scene-capture renders (the image / depth capture used while
	// scanning an object). This is purely to keep the editor-only preview out of
	// the captured frames so it is never baked into training data. The 505 PSO
	// RT-descriptor crash is handled separately by ApplyCachedRenderTargets() in
	// the draw lambda, which makes the PSO match whatever RT is bound for *any*
	// view (including scene captures), so we do not need to bail out here for that.
	if (InView.bIsSceneCapture)
	{
		return;
	}

	const TArray<FOpenSplat4DPoint>& Points = Comp->PointCloud->GetPoints();
	const int32 PointCount = Points.Num();

	// Pack into the GPU layout (7 x float4 per point), transforming positions by
	// the component's world transform so actor placement is honoured. Points whose
	// projected screen size exceeds r.OpenSplat4D.MaxFeatureSize are culled (LOD).
	const FTransform LocalToWorld = Comp->GetComponentToWorld();
	const FVector ViewOrigin = InView.ViewMatrices.GetViewOrigin();
	const float MaxFeatureSize = CVarOpenSplat4DMaxFeatureSize.GetValueOnRenderThread();

	TArray<FVector4f> Packed;
	Packed.AddUninitialized(PointCount * 7);
	int32 OutCount = 0;
	for (int32 i = 0; i < PointCount; i++)
	{
		const FOpenSplat4DPoint& P = Points[i];
		const FVector WorldPos = LocalToWorld.TransformPosition(FVector(P.Position.X, P.Position.Y, P.Position.Z));

		// Screen-size LOD cull: drop gaussians that would project larger than the limit.
		if (MaxFeatureSize > 0.f)
		{
			const float MaxScale = FMath::Max3(P.Scale.X, P.Scale.Y, P.Scale.Z);
			const float Dist = FVector::Dist(WorldPos, ViewOrigin);
			const float ScreenFrac = (2.f * MaxScale) / FMath::Max(Dist, 1.f);
			if (ScreenFrac > MaxFeatureSize)
			{
				continue;
			}
		}

		const int32 Base = OutCount * 7;
		Packed[Base + 0] = FVector4f(WorldPos.X, WorldPos.Y, WorldPos.Z, P.AnchorTime);
		Packed[Base + 1] = FVector4f(P.Quat.X, P.Quat.Y, P.Quat.Z, P.Quat.W);
		Packed[Base + 2] = FVector4f(P.Scale.X, P.Scale.Y, P.Scale.Z, P.TimeVariance);
		Packed[Base + 3] = FVector4f(P.Color.R, P.Color.G, P.Color.B, P.Color.A);
		Packed[Base + 4] = FVector4f(P.Velocity.X, P.Velocity.Y, P.Velocity.Z, P.bUseVelocity ? 1.f : 0.f);
		Packed[Base + 5] = FVector4f::Zero();
		Packed[Base + 6] = FVector4f::Zero();
		OutCount++;
	}
	Packed.SetNum(OutCount * 7);

	// Camera basis in world space = first two rows of the (world->view) matrix.
	const FMatrix44f ViewProjection = FMatrix44f(InView.ViewMatrices.GetWorldToClip());
	const FMatrix ViewMatrix = InView.ViewMatrices.GetWorldToView();
	const FVector3f Right(ViewMatrix.M[0][0], ViewMatrix.M[0][1], ViewMatrix.M[0][2]);
	const FVector3f Up(ViewMatrix.M[1][0], ViewMatrix.M[1][1], ViewMatrix.M[1][2]);

	// Upload the packed points as an RDG structured buffer (re-built each frame so
	// cloud / transform changes are picked up live).
	FRDGBufferRef PointBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), Packed.Num()),
		TEXT("OpenSplat4DPoints"));
	GraphBuilder.QueueBufferUpload(PointBuffer, Packed.GetData(), Packed.Num() * sizeof(FVector4f));
	FRDGBufferSRVRef PointSRV = GraphBuilder.CreateSRV(PointBuffer);

	FOpenSplat4DDrawParameters* PassParameters = GraphBuilder.AllocParameters<FOpenSplat4DDrawParameters>();
	PassParameters->RenderTargets = RenderTargets;
	// Keep depth test but never write it (splats are a translucent overlay).
	if (PassParameters->RenderTargets.DepthStencil.GetTexture())
	{
		PassParameters->RenderTargets.DepthStencil.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
	}
	PassParameters->ViewProjection = ViewProjection;
	PassParameters->CameraRight = FVector4f(Right, 0.f);
	PassParameters->CameraUp = FVector4f(Up, 0.f);
	PassParameters->Time = Comp->Time;
	PassParameters->bTemporalWeighting = Comp->bTemporalWeighting ? 1 : 0;
	PassParameters->PointCount = OutCount;
	PassParameters->SplatScale = Comp->SplatScale;
	PassParameters->Points = PointSRV;

	const ERHIFeatureLevel::Type FeatureLevel = InView.GetFeatureLevel();
	TShaderMapRef<FSplatVS> VertexShader(GetGlobalShaderMap(FeatureLevel));
	TShaderMapRef<FSplatPS> PixelShader(GetGlobalShaderMap(FeatureLevel));

	if (!GOpenSplatEmptyVertexDeclaration)
	{
		GOpenSplatEmptyVertexDeclaration = RHICreateVertexDeclaration(FVertexDeclarationElementList());
	}

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("OpenSplat4DBillboard"),
		PassParameters,
		ERDGPassFlags::Raster,
		[PassParameters, VertexShader, PixelShader, PointCount, OutCount](FRHICommandList& RHICmdList)
		{
			FGraphicsPipelineStateInitializer PSOInit;
			PSOInit.PrimitiveType = PT_TriangleList;
			PSOInit.BoundShaderState.VertexDeclarationRHI = GOpenSplatEmptyVertexDeclaration;
			PSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			PSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
		PSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha>::GetRHI();
		if (CVarOpenSplat4DDepthTest.GetValueOnRenderThread() != 0)
		{
			PSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI();
		}
		else
		{
			PSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		}
		PSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();

		// Synchronise the PSO's render-target description with whatever RT the RDG
		// pass actually has bound (SceneColor + depth-stencil). UE5.8's PSO cache
		// validates the RT descriptor against the command list's current RTs
		// (PipelineStateCache.cpp:4767); if they disagree it asserts with code 505
		// and hard-crashes the editor. Pulling the live RT state from the command
		// list keeps the two in lock-step for every view (main viewport, scene
		// captures, etc.), so the billboard can draw without tripping the check.
		RHICmdList.ApplyCachedRenderTargets(PSOInit);

		SetGraphicsPipelineState(RHICmdList, PSOInit, false);

			SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), *PassParameters);
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);

			// 6 vertices (2 triangles) per point, instanced over all points.
			if (!GOpenSplatLoggedDraw)
			{
				GOpenSplatLoggedDraw = true;
				UE_LOG(LogOpenSplat4D, Log,
					TEXT("OpenSplat4D: 渲染管线已触发，正在绘制点云：点数=%d，深度测试=%s。"),
					OutCount, CVarOpenSplat4DDepthTest.GetValueOnRenderThread() != 0 ? TEXT("开") : TEXT("关(叠加)"));
			}
			RHICmdList.DrawPrimitive(0, 2, OutCount);
		});
}

// ----------------------------------------------------------------------------
// Scene proxy (bounds / relevance only; drawing is handled by the extension)
// ----------------------------------------------------------------------------
class FOpenSplat4DBillboardSceneProxy : public FPrimitiveSceneProxy
{
public:
	FOpenSplat4DBillboardSceneProxy(const UOpenSplat4DBillboardComponent& InComponent)
		: FPrimitiveSceneProxy(&InComponent)
	{
		// Bounds are derived automatically from the component's CalcBounds() by the
		// base FPrimitiveSceneProxy constructor (via FPrimitiveSceneProxyDesc).
	}

	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override
	{
		// No mesh elements: the actual splat draw happens in
		// FOpenSplat4DSceneViewExtension::PostRenderBasePassDeferred_RenderThread.
	}

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Relevance;
		Relevance.bDrawRelevance = true;
		Relevance.bDynamicRelevance = true;
		Relevance.bRenderInMainPass = true;
		Relevance.bRenderCustomDepth = false;
		Relevance.bNormalTranslucency = true;
		return Relevance;
	}

	virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }

	virtual SIZE_T GetTypeHash() const override
	{
		static const SIZE_T Hash = FCrc::StrCrc32(TEXT("FOpenSplat4DBillboardSceneProxy"));
		return Hash;
	}
};

// ----------------------------------------------------------------------------
// Component
// ----------------------------------------------------------------------------
UOpenSplat4DBillboardComponent::UOpenSplat4DBillboardComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void UOpenSplat4DBillboardComponent::OnRegister()
{
	Super::OnRegister();

	if (!OpenSplatViewExtension.IsValid())
	{
		OpenSplatViewExtension = FSceneViewExtensions::NewExtension<FOpenSplat4DSceneViewExtension>(this);
	}
}

void UOpenSplat4DBillboardComponent::OnUnregister()
{
	// Releasing the shared reference unregisters the extension automatically.
	OpenSplatViewExtension.Reset();

	Super::OnUnregister();
}

void UOpenSplat4DBillboardComponent::RebuildBuffer()
{
	// The view extension reads the cloud live each frame, so a rebuild here only
	// needs to refresh the scene proxy (bounds) if the cloud topology changed.
	MarkRenderStateDirty();
}

FPrimitiveSceneProxy* UOpenSplat4DBillboardComponent::CreateSceneProxy()
{
	if (!PointCloud || PointCloud->GetPointCount() <= 0)
	{
		return nullptr;
	}

	return new FOpenSplat4DBillboardSceneProxy(*this);
}

FBoxSphereBounds UOpenSplat4DBillboardComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (PointCloud)
	{
		FBoxSphereBounds B = PointCloud->CalcBounds();
		return B.TransformBy(LocalToWorld);
	}
	return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector::ZeroVector, 0.f);
}
