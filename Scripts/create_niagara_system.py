"""
Create NS_OpenSplat4D Niagara System from scratch using UE Python API.
Called from OpenSplat4DNiagaraSetup when the bundled template is incompatible.
Target: UE 5.8
"""
import unreal

SYSTEM_PATH = "/Game/OpenSplat4D/Niagara/NS_OpenSplat4D"
EMITTER_NAME = "NE_OpenSplat4D"
MATERIAL_PATH = "/Game/OpenSplat4D/Materials/M_OpenSplat4DSprite"


def create_niagara_system():
    """Create a valid GPU Compute Niagara System for OpenSplat4D point cloud rendering."""

    # Check if already exists and is valid
    if unreal.EditorAssetLibrary.does_asset_exist(SYSTEM_PATH):
        sys_obj = unreal.EditorAssetLibrary.load_asset(SYSTEM_PATH)
        if sys_obj and isinstance(sys_obj, unreal.NiagaraSystem):
            # Check that the DI is correct
            try:
                store = sys_obj.get_exposed_parameters()
                di = store.get_data_interface(
                    unreal.NiagaraVariable(
                        unreal.NiagaraTypeDefinition(unreal.OpenSplat4DPointCloud),
                        "User.PointCloud"))
                if di:
                    unreal.log("OpenSplat4D Python: NS_OpenSplat4D already exists and is valid, skipping.")
                    return True
            except:
                pass
        unreal.log("OpenSplat4D Python: Regenerating NS_OpenSplat4D...")

    # 1. Create package and NiagaraSystem
    pkg = unreal.EditorAssetLibrary.load_asset("/Game/OpenSplat4D")
    if not pkg:
        unreal.EditorAssetLibrary.make_directory("/Game/OpenSplat4D")

    # Create the system using the factory
    system_factory = unreal.NiagaraSystemFactoryNew()
    system = unreal.EditorAssetLibrary.load_asset(SYSTEM_PATH)
    if not system:
        # Create new empty system
        asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
        system = asset_tools.create_asset(
            "NS_OpenSplat4D",
            "/Game/OpenSplat4D/Niagara",
            unreal.NiagaraSystem,
            unreal.NiagaraSystemFactoryNew()
        )
        if not system:
            unreal.log_error("OpenSplat4D Python: Failed to create NiagaraSystem.")
            return False

    unreal.NiagaraSystemFactoryNew.initialize_system(system, True)
    unreal.log(f"OpenSplat4D Python: Created system: {system.get_name()}")

    # 2. Create Emitter
    emitter = unreal.EditorAssetLibrary.load_asset(
        f"/Game/OpenSplat4D/Niagara/{EMITTER_NAME}")
    if not emitter:
        emitter_factory = unreal.NiagaraEmitterFactoryNew()
        emitter = asset_tools.create_asset(
            EMITTER_NAME,
            "/Game/OpenSplat4D/Niagara",
            unreal.NiagaraEmitter,
            emitter_factory
        )
        if not emitter:
            unreal.log_error("OpenSplat4D Python: Failed to create Emitter.")
            return False

    unreal.NiagaraEmitterFactoryNew.initialize_emitter(emitter, False)
    unreal.log(f"OpenSplat4D Python: Created emitter: {emitter.get_name()}")

    # 3. Configure emitter for GPU Compute
    emitter_data = emitter.get_latest_emitter_data()
    emitter_data.set_editor_property("sim_target", unreal.NiagaraSimTarget.GPU_COMPUTE_SIM)
    emitter_data.set_editor_property("b_local_space", False)
    emitter_data.set_editor_property("b_determinism", True)
    emitter_data.set_editor_property("fixed_bounds", unreal.Box(
        unreal.Vector(-500000, -500000, -500000),
        unreal.Vector(500000, 500000, 500000)))

    # 4. Add a minimal GPU Compute Script
    # The GPU compute script needs at minimum:
    # - An output node (ParticleGPUComputeScript)
    # - Connected to an input node via parameter map
    gpu_script = unreal.NiagaraScript()
    gpu_script.set_usage(unreal.NiagaraScriptUsage.PARTICLE_GPU_COMPUTE_SCRIPT)

    source = unreal.NiagaraScriptSource()
    graph = unreal.NiagaraGraph()
    source.set_editor_property("node_graph", graph)
    gpu_script.set_latest_source(source)

    # Create output node
    output_node = unreal.NiagaraNodeOutput()
    output_node.set_usage(unreal.NiagaraScriptUsage.PARTICLE_GPU_COMPUTE_SCRIPT)
    graph.add_node(output_node)
    output_node.allocate_default_pins()

    # Create input node
    input_node = unreal.NiagaraNodeInput()
    input_var = unreal.NiagaraVariable(
        unreal.NiagaraTypeDefinition(unreal.FNiagaraTypeDefinition.get_parameter_map_def()),
        "InputMap")
    input_node.set_editor_property("input", input_var)
    graph.add_node(input_node)
    input_node.allocate_default_pins()

    # Connect Input.Output -> Output.InputMap
    for ipin in input_node.get_all_pins():
        if ipin.pin_name == "Output" and ipin.direction == unreal.EEdGraphPinDirection.EGPD_Output:
            for opin in output_node.get_all_pins():
                if opin.pin_name == "InputMap" and opin.direction == unreal.EEdGraphPinDirection.EGPD_Input:
                    ipin.make_link_to(opin)
                    unreal.log("OpenSplat4D Python: Connected Input.Output -> Output.InputMap")
                    break

    emitter.set_editor_property("gpu_compute_script", gpu_script)
    unreal.log("OpenSplat4D Python: GPU Compute Script assigned.")

    # 5. Add Sprite Renderer
    sprite_renderer = unreal.NiagaraSpriteRendererProperties()
    material = unreal.EditorAssetLibrary.load_asset(MATERIAL_PATH)
    if material:
        sprite_renderer.set_editor_property("material", material)
    emitter.add_renderer(sprite_renderer, emitter.get_exact_version())

    # 6. Add emitter to system
    handle = unreal.FNiagaraEmitterHandle()
    handle.set_editor_property("instance", emitter)
    handle.set_editor_property("id", unreal.Guid.new_guid())
    handle.set_editor_property("name", unreal.Name("Emitter"))
    system.get_emitter_handles().add(handle)

    # 7. Set exposed parameters
    param_store = system.get_exposed_parameters()
    # DI will be wired at runtime by AOpenSplat4DPointCloudActor

    # 8. System settings
    system.set_editor_property("b_fixed_bounds", True)
    system.set_fixed_bounds(unreal.Box(
        unreal.Vector(-500000, -500000, -500000),
        unreal.Vector(500000, 500000, 500000)))

    # 9. Compile and save
    unreal.NiagaraSystemFactoryNew.initialize_system(system, True)
    system.request_compile(False)
    system.ensure_fully_loaded()
    system.post_edit_change()

    saved = unreal.EditorAssetLibrary.save_asset(SYSTEM_PATH)
    if saved:
        unreal.log("OpenSplat4D Python: NS_OpenSplat4D created and saved successfully.")
        return True
    else:
        unreal.log_error("OpenSplat4D Python: Failed to save NS_OpenSplat4D.")
        return False


if __name__ == "__main__":
    create_niagara_system()
