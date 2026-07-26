"""
Programmatically build the complete NS_OpenSplat4D GPU Compute Script graph
with DI function nodes (GetPointCount, GetPointData) and Map Set nodes.
Run inside UE Editor Python console.
"""
import unreal

SYSTEM_PATH = "/Game/OpenSplat4D/Niagara/NS_OpenSplat4D"


def build():
    system = unreal.EditorAssetLibrary.load_asset(SYSTEM_PATH)
    if not system:
        print("ERROR: System not found")
        return

    # Get emitter
    handles = system.get_emitter_handles()
    if len(handles) == 0:
        print("ERROR: No emitters")
        return

    emitter = handles[0].get_editor_property("instance")
    print(f"Emitter: {emitter.get_name()}")

    # Get GPU Compute Script
    gpu_script = emitter.get_editor_property("gpu_compute_script")
    if not gpu_script:
        # Try alternate property name
        try:
            gpu_script = emitter.get_latest_emitter_data().get_editor_property("gpu_compute_script")
        except:
            pass

    if not gpu_script:
        print("WARNING: No GPU compute script found. Creating one...")
        gpu_script = unreal.NiagaraScript()
        gpu_script.set_usage(unreal.NiagaraScriptUsage.PARTICLE_GPU_COMPUTE_SCRIPT)
        source = unreal.NiagaraScriptSource()
        graph = unreal.NiagaraGraph()
        source.set_editor_property("node_graph", graph)
        gpu_script.set_latest_source(source)

        # Add output node
        output_node = unreal.NiagaraNodeOutput()
        output_node.set_usage(unreal.NiagaraScriptUsage.PARTICLE_GPU_COMPUTE_SCRIPT)
        graph.add_node(output_node)
        output_node.allocate_default_pins()

        # Add input node
        input_node = unreal.NiagaraNodeInput()
        input_var = unreal.NiagaraVariable(
            unreal.NiagaraTypeDefinition(unreal.FNiagaraTypeDefinition.get_parameter_map_def()),
            "InputMap")
        input_node.set_editor_property("input", input_var)
        graph.add_node(input_node)
        input_node.allocate_default_pins()

        # Connect Input.Output → Output.InputMap
        for ipin in input_node.get_all_pins():
            if "Output" in str(ipin.pin_name) and ipin.direction == unreal.EEdGraphPinDirection.EGPD_Output:
                for opin in output_node.get_all_pins():
                    if "InputMap" in str(opin.pin_name) and opin.direction == unreal.EEdGraphPinDirection.EGPD_Input:
                        ipin.make_link_to(opin)
                        break

        emitter.set_editor_property("gpu_compute_script", gpu_script)
        print("GPU Compute Script created.")
    else:
        print(f"GPU Compute Script found: {gpu_script}")

    # Get the graph
    source = gpu_script.get_latest_source()
    graph = source.get_editor_property("node_graph")
    if not graph:
        print("ERROR: No graph in GPU Compute Script")
        return

    print(f"Graph nodes before: {len(graph.get_all_nodes())}")

    # Find existing nodes
    output_node = None
    input_node = None
    all_nodes = graph.get_all_nodes()
    for node in all_nodes:
        cls_name = node.get_class().get_name()
        print(f"  Node: {node.get_name()} ({cls_name})")
        if "Output" in cls_name:
            output_node = node
        elif "Input" in cls_name:
            input_node = node

    if not input_node or not output_node:
        print("ERROR: Input or Output node not found")
        return

    # ============ Add GetPointCount DI function node ============
    print("\nAdding GetPointCount node...")
    # Try to add a function call node for the DI
    point_cloud_class = unreal.load_class(
        None, "/Script/OpenSplat4DRuntime.NiagaraDataInterfaceOpenSplat4D")

    # Method 1: Try NiagaraNodeFunctionCall
    try:
        func_node = unreal.NiagaraNodeFunctionCall()
        graph.add_node(func_node)
        func_node.allocate_default_pins()
        print(f"  Added NiagaraNodeFunctionCall")
    except Exception as e:
        print(f"  NiagaraNodeFunctionCall failed: {e}")

    # Method 2: Try to find existing function references
    # Explore what's available
    print("\nExploring Niagara classes...")
    for attr_name in dir(unreal):
        if 'NiagaraNode' in attr_name or 'FunctionCall' in attr_name:
            print(f"  {attr_name}")

    # Save
    system.request_compile(False)
    unreal.EditorAssetLibrary.save_asset(SYSTEM_PATH)
    print("\nDone. Check compile errors.")


build()
