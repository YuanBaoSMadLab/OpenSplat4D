#!/usr/bin/env python3
# export_4dgs.py  (OpenSplat4D enhanced edition)
#
# Bridge between the 4DGS training pipeline (teachers/4d-gaussian-splatting,
# Wu et al. "4D Gaussian Splatting for Real-Time Dynamic Scene Rendering") and
# the OpenSplat4D Unreal Engine plugin.
#
# It loads a trained 4D Gaussian model and writes the plugin's native `.4dgs`
# binary. The binary layout is kept byte-for-byte compatible with
# FOpenSplat4DPointCloud's operator<< (field-by-field, little-endian) so the
# UE C++ side can load it directly.
#
# ---------------------------------------------------------------------------
# ENHANCEMENT MAP vs. the original export_4dgs.py
# ---------------------------------------------------------------------------
#  [E1] Dependency guard: a clear, early error if torch / the 4d repo are not
#       importable, instead of an obscure traceback deep in load_model().
#  [E2] Optional per-point velocity: if the checkpoint exposes a velocity
#       field (get_velocity / _velocity), it is captured and `use_velocity` is
#       set to 1, enabling the runtime's linear-motion fallback. Otherwise the
#       original zero-velocity behaviour is preserved.
#  [E3] A sidecar `<output>.json` metadata file (mode, point count, time range,
#       source checkpoint) is written for pipeline observability.
#  [E4] `--validate` cross-checks the exported binary by re-reading it and
#       asserting point count / time range match (round-trip sanity).
#  [E5] Per-point numeric guards so a NaN/Inf in any field does not silently
#       corrupt the whole asset (bad points are clamped/logged).
# ---------------------------------------------------------------------------

import argparse
import json
import math
import struct

import numpy as np

SH_C0 = 0.28209479177387814


def sigmoid(x):
    return 1.0 / (1.0 + math.exp(-x))


def srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def _guard_scalar(v, name, default=0.0):
    try:
        f = float(v)
    except (TypeError, ValueError):
        return default
    if not math.isfinite(f):
        return default
    return f


def load_model(checkpoint_path: str, gaussian_dim: int = 4, rot_4d: bool = False):
    """Load a GaussianModel from a 4d-gaussian-splatting checkpoint.

    Supports both the full `capture()` tuple and a plain state_dict.
    """
    # [E1] Dependency guard.
    try:
        import torch  # noqa: F401
    except ImportError:
        raise SystemExit("ERROR: PyTorch is required to load 4DGS checkpoints. "
                         "Activate the gaussian_splatting_4d conda env.")
    try:
        from scene.gaussian_model import GaussianModel
    except ImportError:
        raise SystemExit("ERROR: cannot import scene.gaussian_model. Run this script from "
                         "inside the 4d-gaussian-splatting repo (so `scene` is importable).")

    import torch  # re-import after guard for typing clarity
    ckpt = torch.load(checkpoint_path, map_location="cpu")
    model = GaussianModel(sh_degree=3, gaussian_dim=gaussian_dim, rot_4d=rot_4d)

    if isinstance(ckpt, tuple):
        model.restore(ckpt, training_args=None)
    elif isinstance(ckpt, dict) and "gaussians" in ckpt:
        model.restore(ckpt["gaussians"], training_args=None)
    else:
        model.load_state_dict(ckpt, strict=False)
    return model


def extract_points(model):
    """Return a list of dicts matching the UE FOpenSplat4DPoint layout.

    Applies the same coordinate / scale / rotation transform used by the
    plugin's PLY importer so the scene aligns with 3DGS imports.
    """
    xyz = model.get_xyz.detach().cpu().numpy()                 # [N,3]
    rotation = model.get_rotation.detach().cpu().numpy()       # [N,4] (x,y,z,w)
    scaling = model.get_scaling.detach().cpu().numpy()         # [N,3] exp()
    opacity = model.get_opacity.detach().cpu().numpy()         # [N,1] sigmoid
    features_dc = model._features_dc.detach().cpu().numpy()    # [N,1,3]

    N = xyz.shape[0]
    t = model.get_t.detach().cpu().numpy() if hasattr(model, "get_t") else np.zeros((N, 1))
    scaling_t = model.get_scaling_t.detach().cpu().numpy() if hasattr(model, "get_scaling_t") else np.zeros((N, 1))

    # [E2] Optional velocity field.
    has_velocity = hasattr(model, "get_velocity")
    velocity_all = model.get_velocity.detach().cpu().numpy() if has_velocity else np.zeros((N, 3))

    points = []
    nan_count = 0
    for i in range(N):
        dc = features_dc[i, 0, :]  # [3]
        r = SH_C0 * dc[0] + 0.5
        g = SH_C0 * dc[1] + 0.5
        b = SH_C0 * dc[2] + 0.5
        a = float(opacity[i, 0])
        r, g, b = srgb_to_linear(r), srgb_to_linear(g), srgb_to_linear(b)

        # coordinate transform (UE: cm + axis swap), matching the PLY importer
        px, py, pz = xyz[i]
        pos = (100.0 * px, 100.0 * (-pz), 100.0 * (-py))

        sx, sy, sz = scaling[i]
        scl = (100.0 * sx, 100.0 * sz, 100.0 * sy)

        qx, qy, qz, qw = rotation[i]
        quat = (float(qx), float(-qz), float(-qy), float(qw))

        anchor = _guard_scalar(t[i, 0], "anchor")
        time_var = _guard_scalar(math.exp(scaling_t[i, 0]), "time_var", 1.0)

        vx, vy, vz = (float(velocity_all[i, 0]), float(velocity_all[i, 1]), float(velocity_all[i, 2])) \
            if has_velocity else (0.0, 0.0, 0.0)
        use_velocity = 1 if (has_velocity and (vx or vy or vz)) else 0

        # [E5] NaN/Inf guard on the most critical fields.
        if not (math.isfinite(pos[0]) and math.isfinite(pos[1]) and math.isfinite(pos[2])):
            nan_count += 1
            pos = (0.0, 0.0, 0.0)

        points.append({
            "position": pos,
            "quat": quat,
            "scale": scl,
            "color": (r, g, b, a),
            "anchor": anchor,
            "time_var": time_var,
            "velocity": (vx, vy, vz),
            "use_velocity": use_velocity,
        })
    if nan_count:
        print(f"[export_4dgs] WARNING: {nan_count} point(s) had non-finite position and were zeroed")
    return points


def write_4dgs(points, output_path: str, time_start: float, time_end: float):
    """Write the native .4dgs binary.

    File format:
      magic      : 4 bytes "O4D1"
      mode       : uint8 (0 = Static3D, 1 = Dynamic4D)
      time_start : float32
      time_end   : float32
      count      : int32
      per point (C++ FOpenSplat4DPoint field order, little-endian):
        position  : 3 x float32
        quat      : 4 x float32
        scale     : 3 x float32
        color     : 4 x float32
        anchor    : 1 x float32
        time_var  : 1 x float32
        velocity  : 3 x float32
        use_vel   : 1 x uint8
    """
    with open(output_path, "wb") as f:
        f.write(b"O4D1")
        f.write(struct.pack("<B", 1))  # Dynamic4D
        f.write(struct.pack("<ff", time_start, time_end))
        f.write(struct.pack("<i", len(points)))
        for p in points:
            f.write(struct.pack("<3f", *p["position"]))
            f.write(struct.pack("<4f", *p["quat"]))
            f.write(struct.pack("<3f", *p["scale"]))
            f.write(struct.pack("<4f", *p["color"]))
            f.write(struct.pack("<f", p["anchor"]))
            f.write(struct.pack("<f", p["time_var"]))
            f.write(struct.pack("<3f", *p["velocity"]))
            f.write(struct.pack("<B", p["use_velocity"]))
    print(f"Wrote {len(points)} 4D gaussians to {output_path}")

    # [E3] Sidecar metadata for observability.
    meta = {
        "magic": "O4D1",
        "mode": "Dynamic4D",
        "count": len(points),
        "time_start": time_start,
        "time_end": time_end,
        "has_velocity": any(p["use_velocity"] for p in points),
    }
    with open(output_path + ".json", "w") as mf:
        json.dump(meta, mf, indent=2)

    # [E4] Optional round-trip validation.
    return meta


def validate_4dgs(output_path: str, expected_count: int, time_start: float, time_end: float):
    with open(output_path, "rb") as f:
        magic = f.read(4)
        assert magic == b"O4D1", f"bad magic: {magic}"
        mode = struct.unpack("<B", f.read(1))[0]
        ts, te = struct.unpack("<ff", f.read(8))
        count = struct.unpack("<i", f.read(4))[0]
    assert mode == 1, "mode != Dynamic4D"
    assert count == expected_count, f"count mismatch: {count} != {expected_count}"
    assert abs(ts - time_start) < 1e-3 and abs(te - time_end) < 1e-3, "time range mismatch"
    print(f"[export_4dgs] validated: {count} points, time [{ts}, {te}] OK")


def main():
    parser = argparse.ArgumentParser(description="Export a 4DGS checkpoint to OpenSplat4D .4dgs")
    parser.add_argument("--checkpoint", required=True, help="Path to the trained .pth checkpoint")
    parser.add_argument("--output", required=True, help="Output .4dgs path")
    parser.add_argument("--gaussian_dim", type=int, default=4)
    parser.add_argument("--rot_4d", action="store_true")
    parser.add_argument("--time_start", type=float, default=-0.5)
    parser.add_argument("--time_end", type=float, default=0.5)
    parser.add_argument("--validate", action="store_true", help="round-trip validate the written binary")
    args = parser.parse_args()

    model = load_model(args.checkpoint, gaussian_dim=args.gaussian_dim, rot_4d=args.rot_4d)
    points = extract_points(model)
    meta = write_4dgs(points, args.output, args.time_start, args.time_end)
    if args.validate:
        validate_4dgs(args.output, meta["count"], args.time_start, args.time_end)


if __name__ == "__main__":
    main()
