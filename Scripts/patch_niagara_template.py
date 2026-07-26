#!/usr/bin/env python3
"""
Patch reference plugin .uasset files for OpenSplat4D.
Replaces class paths, module names, and asset paths (same-length via NUL padding).
"""
import sys
from pathlib import Path

def pad(new: bytes, target_len: int) -> bytes:
    """Return *new* NUL-padded to *target_len* bytes."""
    if len(new) > target_len:
        raise ValueError(f"Replacement longer than original: {len(new)} > {target_len} for {new!r}")
    return new + b'\x00' * (target_len - len(new))

def mk_repl(old: bytes, new: bytes):
    """Create a length-matched (old, new) pair."""
    return (old, pad(new, len(old)))

# Ordered longest-first to avoid substring corruption.
RAW = [
    # --- class/module (standalone) ---
    (b'/Script/GaussianSplattingRuntime.NiagaraDataInterfaceGaussianSplattingPointCloud',
     b'/Script/OpenSplat4DRuntime.NiagaraDataInterfaceOpenSplat4D'),

    (b'NiagaraDataInterfaceGaussianSplattingPointCloud',
     b'NiagaraDataInterfaceOpenSplat4D'),

    # --- full object paths with colon (sub-object variants) ---
    (b'/GaussianSplattingForUnrealEngine/Niagara/NS_GaussianSplattingPointCloud.NS_GaussianSplattingPointCloud:',
     b'/OpenSplat4D/Niagara/NS_OpenSplat4D.NS_OpenSplat4D:'),

    (b'/GaussianSplattingForUnrealEngine/Niagara/NE_GaussianSplatting.NE_GaussianSplatting:',
     b'/OpenSplat4D/Niagara/NE_OpenSplat4D.NE_OpenSplat4D:'),

    # --- full asset paths (bare) ---
    (b'/GaussianSplattingForUnrealEngine/Niagara/NS_GaussianSplattingPointCloud',
     b'/OpenSplat4D/Niagara/NS_OpenSplat4D'),

    (b'/GaussianSplattingForUnrealEngine/Niagara/NE_GaussianSplatting',
     b'/OpenSplat4D/Niagara/NE_OpenSplat4D'),

    (b'/GaussianSplattingForUnrealEngine/Materials/M_GaussianSplattingPoint',
     b'/OpenSplat4D/Materials/M_OpenSplat4DSprite'),

    (b'/GaussianSplattingForUnrealEngine/Materials/M_GaussianSplatting',
     b'/OpenSplat4D/Materials/M_OpenSplat4DSprite'),

    # --- module ---
    (b'/Script/GaussianSplattingRuntime',
     b'/Script/OpenSplat4DRuntime'),

    # --- material short names ---
    (b'M_GaussianSplattingPoint',
     b'M_OpenSplat4DSprite'),

    (b'M_GaussianSplatting',
     b'M_OpenSplat4DSprite'),
]

# Build replacements with correct NUL padding
REPLACEMENTS = [mk_repl(o, n) for o, n in RAW]

FILE_MAP = {
    'NE_GaussianSplatting.uasset': 'Niagara/NE_OpenSplat4D.uasset',
    'NS_GaussianSplattingPointCloud.uasset': 'Niagara/NS_OpenSplat4D.uasset',
}


def patch_file(src: Path, dst: Path):
    data = src.read_bytes()
    for old, new in REPLACEMENTS:
        cnt = data.count(old)
        if cnt:
            data = data.replace(old, new)
            print(f"  [{len(old)}→{len(new)}]  {old[:80]!r}  ×{cnt}")
        else:
            print(f"  (skip) {old[:60]!r}")
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_bytes(data)
    print(f"  → {dst}")


def main():
    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir.parent.parent
    src_base = repo_root / 'teachers' / 'GaussianSplattingForUnrealEngine' / 'Content'
    dst_base = repo_root / 'OpenSplat4D' / 'Content'

    if not src_base.is_dir():
        print(f"ERROR: {src_base} not found")
        sys.exit(1)

    for src_name, dst_rel in FILE_MAP.items():
        src = src_base / 'Niagara' / src_name
        dst = dst_base / dst_rel
        if not src.is_file():
            print(f"WARNING: {src} missing, skipping")
            continue
        print(f"\n=== {src_name} → {dst_rel} ===")
        patch_file(src, dst)

    print("\n✓ Done.")


if __name__ == '__main__':
    main()
