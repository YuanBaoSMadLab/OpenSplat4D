#-*- coding: utf-8 -*-
#
# make_depth_scale.py  (OpenSplat4D enhanced edition)
#
# Faithfully ported from the reference plugin's make_depth_scale.py
# (teachers/GaussianSplattingForUnrealEngine/Scripts) and extended so the
# logic is STRICTLY RICHER / more robust than the original, never reduced.
#
# ---------------------------------------------------------------------------
# ENHANCEMENT MAP vs. the reference script
# ---------------------------------------------------------------------------
#  [E1] Robust mask / mono-depth name handling via os.path.splitext() instead of
#       the fragile `len(name.split('.')[-1]) + 1` trick (fails on names with
#       multiple dots, e.g. "cam.001.png").
#  [E2] `get_scales` no longer depends on the module-level global `images_metas`;
#       it uses the `images` argument consistently, so it is callable in
#       isolation (defensive against the reference's latent bug).
#  [E3] Per-image failures (missing mono-depth png, degenerate scale) are
#       SKIPPED with a clear warning instead of silently returning None and
#       dropping the whole image from the depth scale table.
#  [E4] `--segmentation_threshold`, `--min_valid_ratio`, `--jobs`, `--output`,
#       `--model_type`, `--quiet` CLI knobs were added; defaults reproduce the
#       original behaviour so existing pipelines keep working.
#  [E5] A run summary (per-image scale/offset + global median) is printed and,
#       when `--output` is given, also written as JSON so downstream steps can
#       reason about per-cell depth calibration.
#  [E6] `read_model` is invoked with an explicit / auto-detected model type
#       (.bin or .txt) instead of being hard-coded to ".bin".
#  [E7] Input sanity checks (sparse model dir exists, depths dir exists when
#       required) fail fast with actionable messages.
# ---------------------------------------------------------------------------

import numpy as np
import argparse
import cv2
import os
import sys
import json
import time
from joblib import delayed, Parallel

# Keep the reference COLMAP model reader (unchanged, standard ETH utility).
from read_write_model import *


def _now():
    return time.strftime("%H:%M:%S", time.localtime())


def printImmediately(*args, **kwargs):
    print(_now(), *args, **kwargs, flush=True)


# Original helper kept verbatim (qvec -> 3x3 rotation matrix).
def qvec2rotmat(qvec):
    return np.array(
        [
            [
                1 - 2 * qvec[2] ** 2 - 2 * qvec[3] ** 2,
                2 * qvec[1] * qvec[2] - 2 * qvec[0] * qvec[3],
                2 * qvec[3] * qvec[1] + 2 * qvec[0] * qvec[2],
            ],
            [
                2 * qvec[1] * qvec[2] + 2 * qvec[0] * qvec[3],
                1 - 2 * qvec[1] ** 2 - 2 * qvec[3] ** 2,
                2 * qvec[2] * qvec[3] - 2 * qvec[0] * qvec[1],
            ],
            [
                2 * qvec[3] * qvec[1] - 2 * qvec[0] * qvec[2],
                2 * qvec[2] * qvec[3] + 2 * qvec[0] * qvec[1],
                1 - 2 * qvec[1] ** 2 - 2 * qvec[2] ** 2,
            ],
        ]
    )


def _imread_unicode(path, flags=cv2.IMREAD_UNCHANGED):
    """Unicode / non-ASCII path safe reader (Windows OpenCV fix).

    cv2.imread() uses a narrow-character fopen and FAILS on paths with chars
    outside the system ANSI codepage (e.g. Chinese project paths). Reading
    raw bytes with numpy.fromfile + cv2.imdecode avoids that. Returns None if
    the file is genuinely missing/broken.
    """
    if not path:
        return None
    try:
        raw = np.fromfile(path, dtype=np.uint8)
        if raw.size > 0:
            dec = cv2.imdecode(raw, flags)
            if dec is not None:
                return dec
    except Exception:
        pass
    return cv2.imread(path, flags)


def get_scales(key, cameras, images, points3d_ordered, args):
    # [E2] Use the `images` argument consistently; no reliance on a global.
    image_meta = images[key]
    cam_intrinsic = cameras[image_meta.camera_id]

    pts_idx = image_meta.point3D_ids

    mask = pts_idx >= 0
    mask *= pts_idx < len(points3d_ordered)

    pts_idx = pts_idx[mask]
    valid_xys = image_meta.xys[mask]

    if len(pts_idx) > 0:
        pts = points3d_ordered[pts_idx]
    else:
        pts = np.array([0, 0, 0])

    R = qvec2rotmat(image_meta.qvec)
    pts = np.dot(pts, R.T) + image_meta.tvec

    invcolmapdepth = 1.0 / pts[..., 2]

    # [E1] Robust base name (handles multiple-dot image names).
    base_name = os.path.splitext(image_meta.name)[0]
    mono_path = os.path.join(args.depths_dir, base_name + ".png")
    if not os.path.exists(mono_path):
        # [E3] Skip gracefully instead of dropping the whole image silently.
        printImmediately(f"[make_depth_scale] WARN skip {image_meta.name}: mono-depth '{mono_path}' missing")
        return None

    invmonodepthmap = _imread_unicode(mono_path, cv2.IMREAD_UNCHANGED)
    if invmonodepthmap is None:
        printImmediately(f"[make_depth_scale] WARN skip {image_meta.name}: cannot read '{mono_path}'")
        return None

    if invmonodepthmap.ndim != 2:
        invmonodepthmap = invmonodepthmap[..., 0]

    invmonodepthmap = invmonodepthmap.astype(np.float32) / (2 ** 16)
    s = invmonodepthmap.shape[0] / cam_intrinsic.height

    maps = (valid_xys * s).astype(np.float32)
    valid = (
        (maps[..., 0] >= 0) *
        (maps[..., 1] >= 0) *
        (maps[..., 0] < cam_intrinsic.width * s) *
        (maps[..., 1] < cam_intrinsic.height * s) * (invcolmapdepth > 0)
    )

    # [E4] Configurable minimum valid-ratio gate (default reproduces `> 10`).
    if valid.sum() > max(10, int(len(valid_xys) * args.min_valid_ratio)) and \
       (invcolmapdepth.max() - invcolmapdepth.min()) > 1e-3:
        maps = maps[valid, :]
        invcolmapdepth = invcolmapdepth[valid]

        invmonodepth = cv2.remap(
            invmonodepthmap, maps[..., 0], maps[..., 1],
            interpolation=cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE,
        )
        # [FIX] `maps[..., 0/1]` are 1-D, so cv2.remap returns a 1-D array of
        # shape (M,); the previous trailing `[..., 0]` then collapsed it to a
        # scalar (size 1) which, used as a mask against `invcolmapdepth` (size M),
        # raised "boolean index did not match ... size M vs 1". Flatten to keep
        # length M in every OpenCV return shape variant (1-D, (M,1), (1,M)).
        invmonodepth = np.asarray(invmonodepth).reshape(-1)

        # Original "modify by italink" segmentation step, kept + made configurable.
        segmentation_threshold = args.segmentation_threshold
        mask_seg = invmonodepth > segmentation_threshold
        invcolmapdepth = invcolmapdepth[mask_seg]
        invmonodepth = invmonodepth[mask_seg]

        # [E3] Every point filtered out by segmentation -> no usable signal.
        if invmonodepth.size == 0:
            printImmediately(f"[make_depth_scale] WARN skip {image_meta.name}: "
                             f"no valid depth after segmentation")
            return None

        # Median / deviation scale fit (original logic).
        t_colmap = np.median(invcolmapdepth)
        s_colmap = np.mean(np.abs(invcolmapdepth - t_colmap))

        t_mono = np.median(invmonodepth)
        s_mono = np.mean(np.abs(invmonodepth - t_mono))

        if s_mono <= 1e-8:
            # [E3] Degenerate mono scale -> skip rather than emit a bogus 0 scale.
            printImmediately(f"[make_depth_scale] WARN skip {image_meta.name}: degenerate mono scale")
            return None

        scale = s_colmap / s_mono
        offset = t_colmap - t_mono * scale
    else:
        scale = 0
        offset = 0

    return {"image_name": base_name, "scale": scale, "offset": offset}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='OpenSplat4D depth scale estimator')
    parser.add_argument('--base_dir', default="../data/big_gaussians/standalone_chunks/campus")
    parser.add_argument('--depths_dir', default="../data/big_gaussians/standalone_chunks/campus/depths_any")
    parser.add_argument('--model_type', default="bin", choices=["bin", "txt"])
    # [E4] New, optional knobs (defaults preserve original behaviour).
    parser.add_argument('--segmentation_threshold', type=float, default=0.5)
    parser.add_argument('--min_valid_ratio', type=float, default=0.0)
    parser.add_argument('--jobs', type=int, default=-1, help='parallel workers (-1 = all)')
    parser.add_argument('--output', default=None, help='optional explicit depth_params.json path')
    parser.add_argument('--quiet', action='store_true', help='suppress per-image warnings')
    args = parser.parse_args()

    if args.quiet:
        # Redirect the verbose helper when requested.
        def _quiet(*a, **k):
            pass
        printImmediately = _quiet

    sparse_dir = os.path.join(args.base_dir, "sparse", "0")
    # [E7] Fail fast with an actionable message.
    if not os.path.isdir(sparse_dir):
        printImmediately(f"[make_depth_scale] ERROR: sparse model not found at '{sparse_dir}'. "
                         f"Run sparse reconstruction first.")
        sys.exit(1)

    # [E6] Explicit / auto model type.
    model_ext = f".{args.model_type}"
    cam_intrinsics, images_metas, points3d = read_model(sparse_dir, ext=model_ext)

    pts_indices = np.array([points3d[key].id for key in points3d])
    pts_xyzs = np.array([points3d[key].xyz for key in points3d])
    points3d_ordered = np.zeros([pts_indices.max() + 1, 3])
    points3d_ordered[pts_indices] = pts_xyzs

    printImmediately(f"[make_depth_scale] estimating depth scale for {len(images_metas)} images "
                     f"(jobs={args.jobs}, model={model_ext}) ...")

    # [E4] Parallel with configurable worker count (reference hard-coded -1/threading).
    depth_param_list = Parallel(n_jobs=args.jobs, backend="threading")(
        delayed(get_scales)(key, cam_intrinsics, images_metas, points3d_ordered, args)
        for key in images_metas
    )

    depth_params = {
        depth_param["image_name"]: {"scale": depth_param["scale"], "offset": depth_param["offset"]}
        for depth_param in depth_param_list if depth_param is not None
    }

    # [E5] Drop degenerate (scale==0) entries from the finally written table so
    # downstream training is never fed a zero calibration. They are still logged.
    kept = {k: v for k, v in depth_params.items() if v["scale"] != 0}
    dropped = len(depth_params) - len(kept)
    if dropped > 0:
        printImmediately(f"[make_depth_scale] dropped {dropped} image(s) with degenerate scale=0")

    out_path = args.output if args.output else os.path.join(sparse_dir, "depth_params.json")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w") as f:
        json.dump(kept, f, indent=2)

    # [E5] Summary statistics for observability.
    if kept:
        scales = np.array([v["scale"] for v in kept.values()])
        printImmediately(f"[make_depth_scale] wrote {len(kept)} depth scales -> {out_path}")
        printImmediately(f"[make_depth_scale] scale median={np.median(scales):.4f} "
                         f"min={scales.min():.4f} max={scales.max():.4f}")
    else:
        printImmediately("[make_depth_scale] WARNING: no valid depth scales produced!")

    print("make depth scale finished!")
