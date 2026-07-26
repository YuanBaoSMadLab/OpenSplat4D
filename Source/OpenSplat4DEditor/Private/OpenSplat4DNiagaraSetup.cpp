#include "OpenSplat4DNiagaraSetup.h"
#include "OpenSplat4DEditorModule.h"

#include "UObject/SavePackage.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionLength.h"
#include "Materials/MaterialExpressionOneMinus.h"

#include "HAL/FileManager.h"

// Niagara core & editor
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraDataInterface.h"
#include "NiagaraDataInterfaceCurve.h"
#include "NiagaraConstants.h"
#include "NiagaraEditorSettings.h"
#include "NiagaraEditorUtilities.h"
#include "EdGraphSchema_Niagara.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "NiagaraNodeAssignment.h"
#include "EdGraphUtilities.h"	// FGraphNodeCreator

// OpenSplat4D DI
#include "OpenSplat4DDataInterface.h"

#define LOCTEXT_NAMESPACE "OpenSplat4DNiagaraSetup"

// ============================================================================
// Local replacements for FNiagaraStackGraphUtilities internals (not exported)
// ============================================================================
//
// Verified against UE 5.8 NiagaraEditor source (NiagaraStackGraphUtilities.h):
//   - ResetGraphForOutput    : NOT exported (no NIAGARAEDITOR_API)
//   - MakeLinkTo             : NOT exported
//   - GetParameterMapInputPin: NOT exported
//   - RelayoutGraph          : NOT exported
//   - AddScriptModuleToStack : EXPORTED (NIAGARAEDITOR_API on UNiagaraScript* overload)
//   - AddParameterModuleToStack: EXPORTED
// UE 5.8 factory code (NiagaraEmitterFactoryNew.cpp) confirms 4 Input nodes per
// graph is standard — one per script usage, each created by ResetGraphForOutput.

/** Make a pin-to-pin link and notify both owning nodes. */
static void GS_MakeLinkTo(UEdGraphPin* PinA, UEdGraphPin* PinB)
{
	PinA->MakeLinkTo(PinB);
	PinA->GetOwningNode()->PinConnectionListChanged(PinA);
	PinB->GetOwningNode()->PinConnectionListChanged(PinB);
}

/** Find the ParameterMap input pin on a Niagara node (public-equivalent). */
static UEdGraphPin* GS_GetParameterMapInputPin(UNiagaraNode& Node)
{
	TArray<UEdGraphPin*> InputPins;
	Node.GetInputPins(InputPins);
	const UEdGraphSchema_Niagara* NiagaraSchema = CastChecked<UEdGraphSchema_Niagara>(Node.GetSchema());
	for (UEdGraphPin* Pin : InputPins)
	{
		FNiagaraTypeDefinition PinDef = NiagaraSchema->PinToTypeDefinition(Pin);
		if (PinDef == FNiagaraTypeDefinition::GetParameterMapDef())
		{
			return Pin;
		}
	}
	return nullptr;
}

/** Find the ParameterMap output pin on a Niagara node. */
static UEdGraphPin* GS_GetParameterMapOutputPin(UNiagaraNode& Node)
{
	TArray<UEdGraphPin*> OutputPins;
	Node.GetOutputPins(OutputPins);
	const UEdGraphSchema_Niagara* NiagaraSchema = CastChecked<UEdGraphSchema_Niagara>(Node.GetSchema());
	for (UEdGraphPin* Pin : OutputPins)
	{
		FNiagaraTypeDefinition PinDef = NiagaraSchema->PinToTypeDefinition(Pin);
		if (PinDef == FNiagaraTypeDefinition::GetParameterMapDef())
		{
			return Pin;
		}
	}
	return nullptr;
}

/** Simple auto-layout for a Niagara graph: stack nodes left-to-right, top-to-bottom. */
static void GS_RelayoutGraph(UEdGraph& Graph)
{
	TArray<UNiagaraNodeOutput*> OutputNodes;
	Graph.GetNodesOfClass(OutputNodes);
	if (OutputNodes.Num() == 0)
	{
		return;
	}

	// Place output nodes at the right edge
	const int32 OutX = 800;
	int32 OutY = 0;
	for (UNiagaraNodeOutput* OutNode : OutputNodes)
	{
		OutNode->NodePosX = OutX;
		OutNode->NodePosY = OutY;
		OutY += 150;
	}

	// Walk from each output through param-map chain, place at decreasing X
	TMap<UEdGraphNode*, int32> PlacedNodes;
	int32 StackLevel = 1;
	const int32 XStep = 350;
	TArray<UEdGraphNode*> CurrentLevel;
	for (UNiagaraNodeOutput* OutNode : OutputNodes)
	{
		CurrentLevel.Add(OutNode);
	}
	while (CurrentLevel.Num() > 0)
	{
		TArray<UEdGraphNode*> NextLevel;
		int32 YOff = 0;
		for (UEdGraphNode* Node : CurrentLevel)
		{
			if (PlacedNodes.Contains(Node))
			{
				continue;
			}
			PlacedNodes.Add(Node, StackLevel);
			if (!Node->IsA<UNiagaraNodeOutput>())
			{
				Node->NodePosX = OutX - StackLevel * XStep;
				Node->NodePosY = YOff;
				YOff += 120;
			}
			UNiagaraNode* NiaNode = Cast<UNiagaraNode>(Node);
			if (NiaNode)
			{
				UEdGraphPin* MapPin = GS_GetParameterMapInputPin(*NiaNode);
				if (MapPin)
				{
					for (UEdGraphPin* Linked : MapPin->LinkedTo)
					{
						UEdGraphNode* LinkedNode = Linked->GetOwningNode();
						if (LinkedNode && !PlacedNodes.Contains(LinkedNode))
						{
							NextLevel.Add(LinkedNode);
						}
					}
				}
			}
		}
		CurrentLevel = NextLevel;
		StackLevel++;
	}

	Graph.NotifyGraphChanged();
}

/** Walk backward from OutputNode through the parameter-map chain to find the
 *  Input node, then insert NodeToInsert between Input and the rest of the
 *  chain.  Returns true on success.
 *
 *  Target topology after insertion:
 *    Input.OutputMap  →  NodeToInsert.InputMap
 *    NodeToInsert.OutputMap  →  (rest of chain)  →  OutputNode.InputMap
 */
static bool GS_InsertNodeAfterInput(UNiagaraNodeOutput& OutputNode, UNiagaraNode* NodeToInsert)
{
	UEdGraphPin* DiIn  = GS_GetParameterMapInputPin(*NodeToInsert);
	UEdGraphPin* DiOut = GS_GetParameterMapOutputPin(*NodeToInsert);
	if (!DiIn || !DiOut)
	{
		return false;
	}

	// Walk backward from OutputNode.InputMap through LinkedTo[0] (OutputMap of
	// the preceding node) until we hit the UNiagaraNodeInput that feeds the
	// entire chain.
	UEdGraphPin* ChainInPin = GS_GetParameterMapInputPin(OutputNode);
	if (!ChainInPin || ChainInPin->LinkedTo.Num() == 0)
	{
		return false;   // nothing connected yet — bail
	}

	UNiagaraNodeInput* FoundInput  = nullptr;
	UEdGraphPin*       FirstModuleInPin = nullptr;   // InputMap of first non-Input node

	while (ChainInPin)
	{
		if (ChainInPin->LinkedTo.Num() == 0)
		{
			break; // dead end (should not happen in a valid stack)
		}

		UEdGraphPin* PrevOutPin = ChainInPin->LinkedTo[0];     // previous node's OutputMap
		UNiagaraNode* PrevNode = Cast<UNiagaraNode>(PrevOutPin->GetOwningNode());
		if (!PrevNode)
		{
			return false;
		}

		if (UNiagaraNodeInput* CheckInput = Cast<UNiagaraNodeInput>(PrevNode))
		{
			FoundInput = CheckInput;
			FirstModuleInPin = ChainInPin;   // this pin belongs to the node right after Input
			break;
		}

		// Step one more node backward
		ChainInPin = GS_GetParameterMapInputPin(*PrevNode);
	}

	if (!FoundInput || !FirstModuleInPin)
	{
		return false;
	}

	UEdGraphPin* InputOutPin = GS_GetParameterMapOutputPin(*FoundInput);
	if (!InputOutPin)
	{
		return false;
	}

	// Break: Input.OutputMap  →  FirstModule.InputMap
	// Insert: Input.OutputMap  →  DiIn,   DiOut  →  FirstModule.InputMap
	InputOutPin->BreakLinkTo(FirstModuleInPin);
	GS_MakeLinkTo(DiIn, InputOutPin);
	GS_MakeLinkTo(FirstModuleInPin, DiOut);
	return true;
}

/**
 * Create or reset the output+input node pair for a Niagara script usage.
 * Mirrors FNiagaraStackGraphUtilities::ResetGraphForOutput using public API.
 */
static UNiagaraNodeOutput* GS_ResetGraphForOutput(
	UNiagaraGraph& NiagaraGraph,
	ENiagaraScriptUsage ScriptUsage,
	FGuid ScriptUsageId)
{
	NiagaraGraph.Modify();

	// Reuse existing output node or create new one.
	UNiagaraNodeOutput* OutputNode = NiagaraGraph.FindEquivalentOutputNode(ScriptUsage, ScriptUsageId);
	UEdGraphPin* OutputInputPin = OutputNode ? GS_GetParameterMapInputPin(*OutputNode) : nullptr;

	if (OutputNode && !OutputInputPin)
	{
		NiagaraGraph.RemoveNode(OutputNode);
		OutputNode = nullptr;
	}

	if (!OutputNode)
	{
		FGraphNodeCreator<UNiagaraNodeOutput> NodeCreator(NiagaraGraph);
		OutputNode = NodeCreator.CreateNode();
		OutputNode->SetUsage(ScriptUsage);
		OutputNode->SetUsageId(ScriptUsageId);
		OutputNode->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Out")));
		NodeCreator.Finalize();
		OutputInputPin = GS_GetParameterMapInputPin(*OutputNode);
	}

	// Ensure input node exists

	FGraphNodeCreator<UNiagaraNodeInput> InputCreator(NiagaraGraph);
	UNiagaraNodeInput* InputNode = InputCreator.CreateNode();
	InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
	InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
	InputCreator.Finalize();

	UEdGraphPin* InputOutputPin = GS_GetParameterMapOutputPin(*InputNode);
	if (OutputInputPin && InputOutputPin)
	{
		OutputInputPin->BreakAllPinLinks(true);
		GS_MakeLinkTo(OutputInputPin, InputOutputPin);
	}

	return OutputNode;
}

static const FString kPluginMount = TEXT("/OpenSplat4D");

// ---- helpers ----------------------------------------------------------------

static bool SaveAsset(UPackage* Pkg, UObject* Asset, const FString& PkgName)
{
	Pkg->MarkPackageDirty();
	const FString Path = FPackageName::LongPackageNameToFilename(PkgName, FPackageName::GetAssetPackageExtension());

	// Commandlets: if an old .uasset exists on disk but the package was created
	// fresh (CreatePackage), the save pipeline rejects "partially loaded" pkgs.
	// Delete old file first so the fresh package saves cleanly.
	if (FPaths::FileExists(Path))
	{
		IFileManager::Get().Delete(*Path, false, true, true);
	}

	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;

	FSavePackageResultStruct Result = UPackage::Save(Pkg, Asset, *Path, Args);
	if (Result.Result != ESavePackageResult::Success)
	{
		UE_LOG(LogOpenSplat4DEditor, Error,
			TEXT("OpenSplat4DSetup: Save failed for %s (Result=%d)"), *PkgName, (int32)Result.Result);
		return false;
	}
	return true;
}

// ---- Material (C++) --------------------------------------------------------

static UMaterial* CreateSplatMaterial()
{
	const FString PkgName = kPluginMount + TEXT("/Materials/M_OpenSplat4DSprite");
	// Always recreate to ensure correct usage flags (old builds may lack NiagaraSprites).
	if (FPackageName::DoesPackageExist(PkgName))
	{
		UE_LOG(LogOpenSplat4DEditor, Log,
			TEXT("OpenSplat4DSetup: Material exists — will overwrite to ensure NiagaraSprites usage flag."));
		// Fall through — CreatePackage below will get a fresh package.
	}

	UPackage* Pkg = CreatePackage(*PkgName);
	if (!Pkg) return nullptr;
	UMaterial* Mat = NewObject<UMaterial>(Pkg, TEXT("M_OpenSplat4DSprite"), RF_Public | RF_Standalone | RF_Transactional);
	if (!Mat) return nullptr;

	Mat->MaterialDomain = MD_Surface;
	Mat->BlendMode = BLEND_Translucent;
	Mat->SetShadingModel(MSM_Unlit);
	Mat->TwoSided = true;
	// [FIX] Niagara Sprite Renderer requires this usage flag in UE 5.8.
	// Without it, the material won't compile the correct shader permutation
	// and sprites render invisible (log: "needed to set usage flag NiagaraSprites").
	Mat->SetMaterialUsage(EMaterialUsage::MATUSAGE_NiagaraSprites);

	UMaterialEditorOnlyData* Ed = Mat->GetEditorOnlyData();
	auto& Expr = Ed->ExpressionCollection.Expressions;

	auto* PC = NewObject<UMaterialExpressionVectorParameter>(Mat);
	PC->ParameterName = TEXT("ParticleColor"); PC->DefaultValue = FLinearColor::White;
	PC->MaterialExpressionEditorX = -600; Expr.Add(PC);
	auto* EmRGB = NewObject<UMaterialExpressionComponentMask>(Mat);
	EmRGB->R = EmRGB->G = EmRGB->B = true; EmRGB->A = false;
	EmRGB->Input.Connect(0, PC); EmRGB->MaterialExpressionEditorX = -400; Expr.Add(EmRGB);
	Ed->EmissiveColor.Connect(0, EmRGB);

	auto* TC = NewObject<UMaterialExpressionTextureCoordinate>(Mat);
	TC->MaterialExpressionEditorX = -600; TC->MaterialExpressionEditorY = 160; Expr.Add(TC);
	auto* C2 = NewObject<UMaterialExpressionConstant>(Mat); C2->R = 2.0f;
	C2->MaterialExpressionEditorX = -500; C2->MaterialExpressionEditorY = 200; Expr.Add(C2);
	auto* C1 = NewObject<UMaterialExpressionConstant>(Mat); C1->R = 1.0f;
	C1->MaterialExpressionEditorX = -500; C1->MaterialExpressionEditorY = 240; Expr.Add(C1);
	auto* Mul2 = NewObject<UMaterialExpressionMultiply>(Mat);
	Mul2->A.Connect(0, TC); Mul2->B.Connect(0, C2);
	Mul2->MaterialExpressionEditorX = -350; Expr.Add(Mul2);
	auto* Sub1 = NewObject<UMaterialExpressionSubtract>(Mat);
	Sub1->A.Connect(0, Mul2); Sub1->B.Connect(0, C1);
	Sub1->MaterialExpressionEditorX = -200; Expr.Add(Sub1);
	auto* Len = NewObject<UMaterialExpressionLength>(Mat);
	Len->Input.Connect(0, Sub1); Len->MaterialExpressionEditorX = -50; Expr.Add(Len);
	auto* Sq = NewObject<UMaterialExpressionMultiply>(Mat);
	Sq->A.Connect(0, Len); Sq->B.Connect(0, Len);
	Sq->MaterialExpressionEditorX = 100; Expr.Add(Sq);
	auto* Om = NewObject<UMaterialExpressionOneMinus>(Mat);
	Om->Input.Connect(0, Sq); Om->MaterialExpressionEditorX = 250; Expr.Add(Om);
	Ed->Opacity.Connect(0, Om);
	Ed->OpacityMask.Connect(0, Om);

	// Force material compilation with all usage flags before saving.
	// SetMaterialUsage alone marks the intent; PostEditChange + recompile
	// ensures the shader permutation for NiagaraSprites is actually baked.
	Mat->PostEditChange();
	Mat->ForceRecompileForRendering();

	if (SaveAsset(Pkg, Mat, PkgName))
	{
		UE_LOG(LogOpenSplat4DEditor, Log, TEXT("OpenSplat4DSetup: Material M_OpenSplat4DSprite created."));
		return LoadObject<UMaterial>(nullptr, *PkgName);
	}
	return nullptr;
}

// ============================================================================
// Niagara System & Emitter — programmatic creation
// ============================================================================
//
// UE 5.8 Niagara source code confirms the following APIs are fully functional:
//
//   1. NewObject<UNiagaraEmitter>()  automatically initialises the version
//      system through PostInitProperties → CheckVersionDataAvailable →
//      FVersionedNiagaraEmitterData::PostInitProperties, which creates all
//      script objects (SpawnScript, UpdateScript, GPUComputeScript, etc.).
//
//   2. UNiagaraEmitter::CheckVersionDataAvailable() adds the first
//      FVersionedNiagaraEmitterData entry and copies legacy DEPRECATED
//      fields when upgrading pre-versioning assets.
//
//   3. FVersionedNiagaraEmitterData::PostInitProperties() calls
//      NewObject<UNiagaraScript>(...) for each script slot, including
//      GPUComputeScript (usage ENiagaraScriptUsage::ParticleGPUComputeScript).
//
//   4. FNiagaraStackGraphUtilities::ResetGraphForOutput() sets up output
//      nodes for each script usage in the graph.
//
//   5. FNiagaraEditorUtilities::AddEmitterToSystem() handles all the
//      handle-creation / version-copy logic.
//
// DI function nodes use the Signature field (FNiagaraFunctionSignature)
// from UNiagaraDataInterface::GetFunctionSignatures() when FunctionScript
// is null.  This is confirmed in EdGraphSchema_Niagara.cpp lines 817-826.
//
// ===== Spawn Count / GPU Compute Script wiring =====
//
// In GPU particle emitters, the Spawn stage is embedded in the GPU
// compute script context (no separate spawn/update like CPU mode).
// Niagara exposes an ENiagaraScriptUsage::ParticleSpawnScript that is
// used as the output node type for the spawn section of the graph.
// 
// GetPointCount → Assignment node → Emitter.SpawnNum controls how
// many particles the GPU emit stage creates per frame.
//
// GetPointData is called per-particle and outputs the attributes
// (Position, Color, SpriteSize, SpriteRotation) via a MapSet.

static bool CreateEmitterGraph(FVersionedNiagaraEmitterData* EmitterData, UNiagaraEmitter* Emitter)
{
	// --- Create the script source & graph ---
	UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(Emitter, NAME_None, RF_Transactional);
	if (!Source) return false;
	UNiagaraGraph* Graph = NewObject<UNiagaraGraph>(Source, NAME_None, RF_Transactional);
	if (!Graph) return false;
	Source->NodeGraph = Graph;

	EmitterData->GraphSource = Source;
	EmitterData->SpawnScriptProps.Script->SetLatestSource(Source);
	EmitterData->UpdateScriptProps.Script->SetLatestSource(Source);
	EmitterData->EmitterSpawnScriptProps.Script->SetLatestSource(Source);
	EmitterData->EmitterUpdateScriptProps.Script->SetLatestSource(Source);
	EmitterData->GetGPUComputeScript()->SetLatestSource(Source);

	// --- Purge stale nodes from previous builds ---
	// The script source may be reused across builds (SetLatestSource reuses the
	// old UNiagaraScriptSource).  Stale UNiagaraNodeFunctionCall nodes left in
	// the graph trigger a fatal Cast<NiagaraNodeParameterMapSet> inside
	// UNiagaraNodeAssignment::GenerateScript when AddParameterModuleToStack runs.
	Graph->Modify();
	TArray<UEdGraphNode*> OldNodes = Graph->Nodes;
	for (UEdGraphNode* N : OldNodes)
	{
		if (N) { Graph->RemoveNode(N); }
	}

	// --- Set up output nodes ---
	UNiagaraNodeOutput* EmitterSpawnOut = GS_ResetGraphForOutput(
		*Graph, ENiagaraScriptUsage::EmitterSpawnScript,
		EmitterData->EmitterSpawnScriptProps.Script->GetUsageId());
	UNiagaraNodeOutput* EmitterUpdateOut = GS_ResetGraphForOutput(
		*Graph, ENiagaraScriptUsage::EmitterUpdateScript,
		EmitterData->EmitterUpdateScriptProps.Script->GetUsageId());
	UNiagaraNodeOutput* ParticleSpawnOut = GS_ResetGraphForOutput(
		*Graph, ENiagaraScriptUsage::ParticleSpawnScript,
		EmitterData->SpawnScriptProps.Script->GetUsageId());
	UNiagaraNodeOutput* ParticleUpdateOut = GS_ResetGraphForOutput(
		*Graph, ENiagaraScriptUsage::ParticleUpdateScript,
		EmitterData->UpdateScriptProps.Script->GetUsageId());
	// GPU 模拟需要 ParticleGPUComputeScript 输出节点，否则 GPU 端无脚本、粒子不 spawn。
	// 引擎编译时会从 ParticleSpawnScript + ParticleUpdateScript 合并生成 GPU 计算逻辑。
	UNiagaraNodeOutput* GPUComputeOut = GS_ResetGraphForOutput(
		*Graph, ENiagaraScriptUsage::ParticleGPUComputeScript,
		EmitterData->GetGPUComputeScript()->GetUsageId());

	if (!EmitterSpawnOut || !EmitterUpdateOut || !ParticleSpawnOut || !ParticleUpdateOut || !GPUComputeOut)
	{
		UE_LOG(LogOpenSplat4DEditor, Error, TEXT("OpenSplat4DSetup: Failed to create output nodes."));
		return false;
	}

	// ---- helper: add a Niagara script module to a stack output node ----
	auto AddModule = [](const TCHAR* Path, UNiagaraNodeOutput& OutNode) -> UNiagaraNodeFunctionCall*
	{
		FSoftObjectPath Ref(Path);
		UNiagaraScript* Script = Cast<UNiagaraScript>(Ref.TryLoad());
		if (Script)
		{
			return FNiagaraStackGraphUtilities::AddScriptModuleToStack(Script, OutNode);
		}
		UE_LOG(LogOpenSplat4DEditor, Warning, TEXT("OpenSplat4DSetup: Module not found: %s"), Path);
		return nullptr;
	};

	// ========================================================================
	// ParticleGPUComputeScript: InitializeParticle + Assignment（固定默认值）
	// GPU 模拟下粒子 spawn+update 在 ParticleGPUComputeScript 执行（GPU 端）。
	// 当前阶段：先用 Assignment 写固定默认值，让整套渲染管线跑通。
	//   - Position: 原点
	//   - Color: 白色
	//   - SpriteSize: 10 单位方块
	//   - Lifetime: 100000（不死）
	// 验证目标：原点出现白色方块 = 编译/加载/GPU模拟/Sprite渲染/材质全链路通。
	//
	// 后续阶段：把点云数据接入粒子。两条可选路径：
	//   A) 修改 DI，让 GetPointData 内部用 Niagara 内置 ExecutionIndex，去掉 Index 输入
	//   B) 用 ParameterMapSet 把 DI 输入引脚作为 override 关联到 Assignment 的 input
	//      （需研究 Stack override 机制，避免 SetDataInterfaceValueForFunctionInput 的
	//       CastChecked<UNiagaraNodeParameterMapSet> 限制 —— 它要求 pin owner 是
	//       Assignment 节点而非 DI 函数节点）
	// ========================================================================
	{
		AddModule(TEXT("/Niagara/Modules/Spawn/Initialization/InitializeParticle.InitializeParticle"), *GPUComputeOut);

		TArray<FNiagaraVariable> AttrTargets;
		AttrTargets.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), FName(TEXT("Particles.Position"))));
		AttrTargets.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetColorDef(), FName(TEXT("Particles.Color"))));
		AttrTargets.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec2Def(), FName(TEXT("Particles.SpriteSize"))));
		AttrTargets.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), FName(TEXT("Particles.Lifetime"))));

		TArray<FString> AttrDefaults;
		AttrDefaults.Add(TEXT("0.0,0.0,0.0"));                  // Position: 原点
		AttrDefaults.Add(TEXT("(R=1.0,G=0.0,B=0.0,A=1.0)"));   // Color: 红色（FLinearColor::ToString 格式）
		AttrDefaults.Add(TEXT("X=500.000000 Y=500.000000"));   // SpriteSize: 500 单位方块（绝对可见）
		AttrDefaults.Add(TEXT("100000.0"));                     // Lifetime: 大值，粒子不死亡

		UNiagaraNodeFunctionCall* AssignNode = FNiagaraStackGraphUtilities::AddParameterModuleToStack(
			AttrTargets, *GPUComputeOut, INDEX_NONE, AttrDefaults);

		if (AssignNode)
		{
			UE_LOG(LogOpenSplat4DEditor, Log,
				TEXT("OpenSplat4DSetup: Assignment 节点已创建（Position=原点, Color=红色, Size=500, Lifetime=100000）。")
				TEXT("预期：原点出现红色大方块 = 渲染管线全通。点云数据接入为后续任务。"));
		}
		else
		{
			UE_LOG(LogOpenSplat4DEditor, Warning,
				TEXT("OpenSplat4DSetup: Assignment 节点创建失败。"));
		}
	}

	// ========================================================================
	// EmitterUpdate: EmitterState + SpawnBurst（一次性 spawn，替换持续 SpawnRate）
	// SpawnBurst 一次性生成大量粒子，配合 GPU 模拟与 DI clamp 保护越界索引。
	// 数量设为 1000000；点云点数通常 <100 万，多余粒子 clamp 到最后一个点。
	// ========================================================================
	AddModule(TEXT("/Niagara/Modules/Emitter/EmitterState.EmitterState"), *EmitterUpdateOut);
	{
		UNiagaraNodeFunctionCall* SpawnBurstNode = AddModule(
			TEXT("/Niagara/Modules/Emitter/SpawnBurst_Instantaneous.SpawnBurst_Instantaneous"), *EmitterUpdateOut);
		if (SpawnBurstNode)
		{
			// SpawnBurst_Instantaneous 的数量引脚名可能为 SpawnCount/Count/Number，
			// 遍历所有输入引脚按名匹配设值。
			for (UEdGraphPin* Pin : SpawnBurstNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input)
				{
					const FName PinName = Pin->PinName;
					if (PinName == TEXT("SpawnCount") || PinName == TEXT("Count") ||
						PinName == TEXT("Number") || PinName == TEXT("SpawnBurst"))
					{
						Pin->DefaultValue = TEXT("1000000");
						break;
					}
				}
			}
		}
	}

	// ========================================================================
	// ParticleUpdate (GPUComputeScript): UpdateAge（Lifetime 递减）
	// 移除 Color/SolveForcesAndVelocity：点云粒子是静态的，无需逐帧运动/颜色更新。
	// ========================================================================
	AddModule(TEXT("/Niagara/Modules/Update/Lifetime/UpdateAge.UpdateAge"), *GPUComputeOut);

	GS_RelayoutGraph(*Graph);
	return true;
}

static UNiagaraEmitter* CreateNE_OpenSplat4D(UPackage* OuterPkg)
{
	// NewObject auto-initialises: PostInitProperties → CheckVersionDataAvailable
	// → FVersionedNiagaraEmitterData::PostInitProperties creates all scripts.
	UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(
		OuterPkg, TEXT("NE_OpenSplat4D"),
		RF_Public | RF_Standalone | RF_Transactional);

	if (!Emitter)
	{
		UE_LOG(LogOpenSplat4DEditor, Error, TEXT("OpenSplat4DSetup: Failed to create NE_OpenSplat4D emitter."));
		return nullptr;
	}

	// PostInitProperties already called CheckVersionDataAvailable() and
	// PostInitProperties for the version data.  But to be safe:
	Emitter->CheckVersionDataAvailable();
	FVersionedNiagaraEmitterData* Data = Emitter->GetLatestEmitterData();
	if (!Data)
	{
		UE_LOG(LogOpenSplat4DEditor, Error, TEXT("OpenSplat4DSetup: No version data after NewObject."));
		return nullptr;
	}

	// --- Configure emitter ---
	// GPU 模拟：点云动辄数十万点（demo.ply=271123），CPU 模拟不可行。
	// GPU 模拟下粒子由 GPUComputeScript 驱动，DI 数据直接在 GPU 端读取。
	Data->SimTarget = ENiagaraSimTarget::GPUComputeSim;
	Data->CalculateBoundsMode = ENiagaraEmitterCalculateBoundMode::Fixed;
	Data->FixedBounds = FBox(FVector(-5000), FVector(5000));
	Data->bDeterminism = false;
	// 一次性 SpawnBurst 不需要 interpolated spawn；GPU 模式下 spawn 在 GPUComputeScript。
	Data->InterpolatedSpawnMode = ENiagaraInterpolatedSpawnMode::NoInterpolation;
	Data->bLocalSpace = false;
	Data->SpawnScriptProps.Script->SetUsage(ENiagaraScriptUsage::ParticleSpawnScript);

	// --- Sprite Renderer ---
	UNiagaraSpriteRendererProperties* SpriteRenderer = NewObject<UNiagaraSpriteRendererProperties>(
		Emitter, TEXT("SpriteRenderer"), RF_Transactional);
	if (SpriteRenderer)
	{
		SpriteRenderer->Alignment = ENiagaraSpriteAlignment::Unaligned;
		SpriteRenderer->FacingMode = ENiagaraSpriteFacingMode::FaceCamera;
		// Bind material — use the plugin-relative path
		SpriteRenderer->Material = CreateSplatMaterial();
		if (!SpriteRenderer->Material)
		{
			UE_LOG(LogOpenSplat4DEditor, Warning,
				TEXT("OpenSplat4DSetup: Sprite renderer material not found at %s/Materials/M_OpenSplat4DSprite.  ")
				TEXT("It will be created when EnsureAssetsExist runs."),
				*kPluginMount);
		}
		Emitter->AddRenderer(SpriteRenderer, Data->Version.VersionGuid);
	}

	// --- Build the graph ---
	if (!CreateEmitterGraph(Data, Emitter))
	{
		UE_LOG(LogOpenSplat4DEditor, Warning,
			TEXT("OpenSplat4DSetup: Emitter graph creation partially failed — ")
			TEXT("DI function nodes may need manual wiring in the Editor."));
	}

	return Emitter;
}

static UNiagaraSystem* CreateNS_OpenSplat4D(UPackage* Pkg, UNiagaraEmitter* Emitter)
{
	UNiagaraSystem* System = NewObject<UNiagaraSystem>(
		Pkg, TEXT("NS_OpenSplat4D"),
		RF_Public | RF_Standalone | RF_Transactional);

	if (!System)
	{
		UE_LOG(LogOpenSplat4DEditor, Error, TEXT("OpenSplat4DSetup: Failed to create NS_OpenSplat4D system."));
		return nullptr;
	}

	// --- Initialize System graph (same pattern as UNiagaraSystemFactoryNew) ---
	UNiagaraScript* SystemSpawnScript = System->GetSystemSpawnScript();
	UNiagaraScript* SystemUpdateScript = System->GetSystemUpdateScript();

	UNiagaraScriptSource* SysSource = NewObject<UNiagaraScriptSource>(
		SystemSpawnScript, TEXT("SystemScriptSource"), RF_Transactional);
	if (SysSource)
	{
		SysSource->NodeGraph = NewObject<UNiagaraGraph>(SysSource, TEXT("SystemScriptGraph"), RF_Transactional);
	}

	SystemSpawnScript->SetLatestSource(SysSource);
	SystemUpdateScript->SetLatestSource(SysSource);

	if (SysSource && SysSource->NodeGraph)
	{
		GS_ResetGraphForOutput(
			*SysSource->NodeGraph, ENiagaraScriptUsage::SystemSpawnScript,
			SystemSpawnScript->GetUsageId());
		GS_ResetGraphForOutput(
			*SysSource->NodeGraph, ENiagaraScriptUsage::SystemUpdateScript,
			SystemUpdateScript->GetUsageId());
		GS_RelayoutGraph(*SysSource->NodeGraph);
	}

	// --- Add emitter to system ---
	const FGuid VersionGuid = Emitter->GetExposedVersion().VersionGuid;
	const FGuid HandleGuid = FNiagaraEditorUtilities::AddEmitterToSystem(
		*System, *Emitter, VersionGuid, /*bCreateCopy=*/false);

	if (!HandleGuid.IsValid())
	{
		UE_LOG(LogOpenSplat4DEditor, Error,
			TEXT("OpenSplat4DSetup: AddEmitterToSystem returned invalid handle. System will not be saved."));
		return nullptr;
	}

	// [CRITICAL] Register the DataInterfaces as system-level User parameters.
	// Without this, UNiagaraFunctionLibrary::GetDataInterface() cannot find
	// User.PointCloud / User.FeatureCurve because the runtime queries
	// System->ExposedParameters, which is NOT automatically populated from
	// the graph nodes alone.
	{
		UNiagaraDataInterfaceOpenSplat4D* SysDI = NewObject<UNiagaraDataInterfaceOpenSplat4D>(
			System, TEXT("PointCloud"), RF_Transactional);
		System->GetExposedParameters().AddParameter(
			FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceOpenSplat4D::StaticClass()),
				TEXT("PointCloud")));

		UNiagaraDataInterfaceCurve* CurveDI = NewObject<UNiagaraDataInterfaceCurve>(
			System, TEXT("FeatureCurve"), RF_Transactional);
		System->GetExposedParameters().AddParameter(
			FNiagaraVariable(FNiagaraTypeDefinition(UNiagaraDataInterfaceCurve::StaticClass()),
				TEXT("FeatureCurve")));
	}

	return System;
}

// ============================================================================
// Public API
// ============================================================================

bool OpenSplat4DNiagaraSetup::BuildNiagaraSystem()
{
	const FString SysPkgName = kPluginMount + TEXT("/Niagara/Templates/UE5_8/NS_OpenSplat4D");
	if (FPackageName::DoesPackageExist(SysPkgName))
	{
		UE_LOG(LogOpenSplat4DEditor, Log,
			TEXT("OpenSplat4DSetup: NS_OpenSplat4D exists — will overwrite with fresh build."));
	}
	else
	{
		UE_LOG(LogOpenSplat4DEditor, Log,
			TEXT("OpenSplat4DSetup: Template not found — building programmatically..."));
	}

	// --- 1. Create emitter package ---
	const FString EmitterPkgName = kPluginMount + TEXT("/Niagara/Templates/UE5_8/NE_OpenSplat4D");
	UPackage* EmitterPkg = CreatePackage(*EmitterPkgName);
	if (!EmitterPkg)
	{
		UE_LOG(LogOpenSplat4DEditor, Error,
			TEXT("OpenSplat4DSetup: Failed to create emitter package %s."), *EmitterPkgName);
		return false;
	}

	UNiagaraEmitter* Emitter = CreateNE_OpenSplat4D(EmitterPkg);
	if (!Emitter)
	{
		return false;
	}

	// --- 2. Create system package ---
	UPackage* SysPkg = CreatePackage(*SysPkgName);
	if (!SysPkg)
	{
		UE_LOG(LogOpenSplat4DEditor, Error,
			TEXT("OpenSplat4DSetup: Failed to create system package %s."), *SysPkgName);
		return false;
	}

	UNiagaraSystem* System = CreateNS_OpenSplat4D(SysPkg, Emitter);
	if (!System)
	{
		return false;
	}

	// --- 4. Compile the system (cascades to emitter scripts) ---
	// Without compilation, the saved .uasset lacks cooked script runtime data,
	// and UE 5.8 rejects it as an invalid Niagara asset at load time.
	UE_LOG(LogOpenSplat4DEditor, Log,
		TEXT("OpenSplat4DSetup: Compiling Niagara system (this may take a moment)..."));
	System->RequestCompile(false);
	System->WaitForCompilationComplete(false, false);

	// Verify compilation succeeded before saving
	if (System->NeedsRequestCompile())
	{
		UE_LOG(LogOpenSplat4DEditor, Error,
			TEXT("OpenSplat4DSetup: Niagara compilation failed for NS_OpenSplat4D. Assets will not be saved."));
		return false;
	}
	UE_LOG(LogOpenSplat4DEditor, Log,
		TEXT("OpenSplat4DSetup: Niagara compilation complete."));

	// --- 5. Save both assets (now with valid compiled data) ---
	if (!SaveAsset(EmitterPkg, Emitter, EmitterPkgName))
	{
		UE_LOG(LogOpenSplat4DEditor, Error,
			TEXT("OpenSplat4DSetup: Failed to save NE_OpenSplat4D."));
		return false;
	}
	UE_LOG(LogOpenSplat4DEditor, Log,
		TEXT("OpenSplat4DSetup: NE_OpenSplat4D saved to %s."), *EmitterPkgName);

	if (!SaveAsset(SysPkg, System, SysPkgName))
	{
		UE_LOG(LogOpenSplat4DEditor, Error,
			TEXT("OpenSplat4DSetup: Failed to save NS_OpenSplat4D."));
		return false;
	}
	UE_LOG(LogOpenSplat4DEditor, Log,
		TEXT("OpenSplat4DSetup: NS_OpenSplat4D saved to %s."), *SysPkgName);

	UE_LOG(LogOpenSplat4DEditor, Log,
		TEXT("OpenSplat4DSetup: BuildNiagaraSystem completed successfully."));
	return true;
}

bool OpenSplat4DNiagaraSetup::EnsureAssetsExistFromTeacherTemplate()
{
	return BuildNiagaraSystem();
}

bool OpenSplat4DNiagaraSetup::EnsureAssetsExistFromBundledTemplate()
{
	return BuildNiagaraSystem();
}

bool OpenSplat4DNiagaraSetup::EnsureAssetsExist()
{
	UE_LOG(LogOpenSplat4DEditor, Log, TEXT("OpenSplat4DSetup: starting..."));

	FModuleManager::Get().LoadModule(TEXT("OpenSplat4DRuntime"));

	// Material is created first — BuildNiagaraSystem also calls CreateSplatMaterial
	// internally (via CreateNE_OpenSplat4D → SpriteRenderer), but we call it
	// explicitly here to ensure it exists before any fallback code.
	if (!CreateSplatMaterial())
	{
		UE_LOG(LogOpenSplat4DEditor, Error,
			TEXT("OpenSplat4DSetup: FAILED creating material M_OpenSplat4DSprite."));
		return false;
	}

	if (!BuildNiagaraSystem())
	{
		UE_LOG(LogOpenSplat4DEditor, Error,
			TEXT("OpenSplat4DSetup: FAILED creating NS_OpenSplat4D + NE_OpenSplat4D.  ")
			TEXT("Check preceding log lines for details."));
		return false;
	}

	UE_LOG(LogOpenSplat4DEditor, Log, TEXT("OpenSplat4DSetup: done."));
	return true;
}

#undef LOCTEXT_NAMESPACE
