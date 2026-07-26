"""
在 GPU Compute Script 图中添加 DI 函数调用节点。
"""
import unreal

SYSTEM_PATH = "/Game/OpenSplat4D/Niagara/NS_OpenSplat4D"
EMITTER_PATH = "/Game/OpenSplat4D/Niagara/NE_OpenSplat4D"

system = unreal.EditorAssetLibrary.load_asset(SYSTEM_PATH)
if system:
    print(f"System: {system.get_name()}")
else:
    print("System not found")

emitter = unreal.EditorAssetLibrary.load_asset(EMITTER_PATH)
if emitter:
    print(f"Emitter: {emitter.get_name()}")
else:
    print("Emitter not loaded as separate asset (may be embedded)")

# Try accessing emitter through system properties
print("\n=== System editor properties ===")
for attr in sorted(dir(system)):
    if not attr.startswith('_') and ('emitter' in attr.lower() or 'graph' in attr.lower() or 'script' in attr.lower()):
        print(f"  {attr}")

# Try get_editor_property
print("\n=== System get_editor_property ===")
for prop_name in ['EmitterHandles', 'emitter_handles', 'SystemEmitterDataSet', 'exposed_parameters']:
    try:
        val = system.get_editor_property(prop_name)
        print(f"  {prop_name}: {type(val).__name__} = {val}")
    except:
        pass

# Try accessing emitter's GPU compute script
if emitter:
    print("\n=== Emitter editor properties ===")
    for attr in sorted(dir(emitter)):
        if not attr.startswith('_') and ('script' in attr.lower() or 'gpu' in attr.lower() or 'graph' in attr.lower() or 'source' in attr.lower()):
            print(f"  {attr}")
    
    for prop_name in ['GPUComputeScript', 'gpu_compute_script', 'GraphSource', 'latest_source']:
        try:
            val = emitter.get_editor_property(prop_name)
            print(f"\n  {prop_name}: {type(val).__name__}")
            if val and hasattr(val, 'get_editor_property'):
                for attr in sorted(dir(val)):
                    if not attr.startswith('_') and ('graph' in attr.lower() or 'node' in attr.lower() or 'source' in attr.lower()):
                        print(f"    {attr}")
        except Exception as e:
            print(f"  {prop_name}: {e}")

# Try NiagaraNodeFunctionCall
print("\n=== NiagaraNodeFunctionCall ===")
try:
    node = unreal.NiagaraNodeFunctionCall()
    print(f"  Created: {type(node).__name__}")
    for attr in sorted(dir(node)):
        a = attr.lower()
        if not attr.startswith('_') and ('function' in a or 'script' in a or 'signature' in a or 'di' in a or 'data' in a or 'interface' in a or 'pin' in a or 'add' in a or 'alloc' in a):
            print(f"  {attr}")
except Exception as e:
    print(f"  failed: {e}")
