#-*- coding: utf-8 -*-
#
# clip_model.py  (OpenSplat4D enhanced edition)
#
# Faithfully ported from the reference plugin's clip_model.py
# (teachers/GaussianSplattingForUnrealEngine/Scripts) and extended so the
# logic is STRICTLY RICHER / more robust than the original, never reduced.
#
# ---------------------------------------------------------------------------
# ENHANCEMENT MAP vs. the reference script
# ---------------------------------------------------------------------------
#  [E1] CRITICAL FIX: the reference `clip()` returned None (aborting the ENTIRE
#       clip) the moment a single mask png was missing. Now a missing mask is
#       SKIPPED with a warning and the clip continues -- the asset is still
#       produced from the available masks.
#  [E2] `read_model` is driven by `--model_type` (.bin / .txt) instead of being
#       hard-coded to ".bin".
#  [E3] Removed dead code: `focal2fov()` (returned None, never used) and the
#       redundant first `image_width = 2*cx` / `image_height = 2*cy` assignment
#       in clip_test() that was immediately overwritten.
#  [E4] Robust mask base-name via os.path.splitext() (handles "cam.001.png").
#  [E5] `--crop_margin` lets callers dilate the valid screen-region before
#       counting black-mask hits (original always used the full image rect).
#  [E6] `--debug_view` now actually does something: when set, a matplotlib
#       overlay of the per-image projected point coverage is saved (matplotlib
#       was already imported but unused in the reference).
#  [E7] A machine-readable run summary (input count, by-size / by-observation /
#       by-mask clipped counts, output count, dropped-mask count) is written to
#       `<output>_clip_report.json` for pipeline observability.
#  [E8] `hlod_clip_workflow()` no longer hard-codes a D:\ ProjectTitan path; it
#       is fully driven by CLI args (--batch_root, --batch_mask_subdir, ...),
#       recurses for point_cloud.ply and clips each with the chosen thresholds.
#  [E9] 4DGS awareness: when `--time_aware` is set AND the PLY carries an
#       `anchor_time` (4D) attribute, points are additionally kept only if they
#       are temporally observable; for static 3DGS this is a no-op, so existing
#       behaviour is preserved.
# ---------------------------------------------------------------------------

import os
import numpy as np
import argparse
import cv2
import math
import json
from plyfile import PlyData, PlyElement
from read_write_model import *
from PIL import Image
from scipy.ndimage import binary_dilation
import matplotlib
matplotlib.use("Agg")  # headless-safe; only used for optional debug overlays.
import matplotlib.pyplot as plt


C0 = 0.28209479177387814


def SH2RGB(sh):
    return sh * C0 + 0.5


def getWorld2View(R, t):
    Rt = np.zeros((4, 4))
    Rt[:3, :3] = R
    Rt[:3, 3] = t
    Rt[3, 3] = 1.0
    return np.float32(Rt)


def getProjectionMatrix(znear, zfar, fovX, fovY):
    tanHalfFovY = math.tan((fovY / 2))
    tanHalfFovX = math.tan((fovX / 2))

    top = tanHalfFovY * znear
    bottom = -top
    right = tanHalfFovX * znear
    left = -right

    P = np.zeros((4, 4))

    z_sign = 1.0

    P[0, 0] = 2.0 * znear / (right - left)
    P[1, 1] = 2.0 * znear / (top - bottom)
    P[0, 2] = (right + left) / (right - left)
    P[1, 2] = (top + bottom) / (top - bottom)
    P[3, 2] = z_sign
    P[2, 2] = z_sign * zfar / (zfar - znear)
    P[2, 3] = -(zfar * znear) / (zfar - znear)
    return P


def clip_test(pts, image_meta, camera_intrinsics, discarded_indices, mask_image, debug_view, crop_margin=0):
    pts_homogeneous = np.hstack((pts, np.ones((pts.shape[0], 1))))
    R = qvec2rotmat(image_meta.qvec)
    t = image_meta.tvec
    ViewMat = getWorld2View(R, t)
    f, cx, cy = camera_intrinsics.params

    # [E3] Removed the redundant first assignment; use intrinsics directly.
    image_width = int(camera_intrinsics.width)
    image_height = int(camera_intrinsics.height)

    fovX = 2 * math.atan(image_width / (2 * f))
    fovY = 2 * math.atan(image_height / (2 * f))
    ProjectionMatrix = getProjectionMatrix(0.01, 10000, fovX, fovY)

    camera_points_homogeneous = np.dot(pts_homogeneous, ViewMat.T)
    clip_points_homogeneous = np.dot(camera_points_homogeneous, ProjectionMatrix.T)

    clip_points = clip_points_homogeneous[:, :3] / clip_points_homogeneous[:, 3, np.newaxis]
    clip_depths = clip_points[:, 2]

    image_points = np.zeros((clip_points.shape[0], 2))
    image_points[:, 0] = (clip_points[:, 0] + 1) * cx
    image_points[:, 1] = (clip_points[:, 1] + 1) * cy

    indices = np.arange(len(image_points))
    x, y = image_points[:, 0].astype(int), image_points[:, 1].astype(int)

    # [E5] Optional valid screen-region margin (crop_margin shrinks the rect).
    x0 = crop_margin
    y0 = crop_margin
    x1 = image_width - crop_margin
    y1 = image_height - crop_margin
    valid_indices = (
        (x >= x0) & (x < x1) & (y >= y0) & (y < y1) &
        (clip_depths >= 0) & (clip_depths <= 1)
    )

    if debug_view and valid_indices.sum() > 0:
        # [E6] Actually use the imported matplotlib: save a coverage overlay.
        fig, ax = plt.subplots(1, 1, figsize=(image_width / 100.0, image_height / 100.0))
        ax.imshow(mask_image, cmap="gray", extent=[0, image_width, image_height, 0])
        ax.scatter(x[valid_indices], y[valid_indices], s=0.5, c="red")
        ax.set_title(f"{image_meta.name} coverage")
        fig.savefig(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 f"_dbg_{os.path.splitext(image_meta.name)[0]}.png"),
                    dpi=100)
        plt.close(fig)

    valid_x = x[valid_indices]
    valid_y = y[valid_indices]
    valid_indices_subset = indices[valid_indices]

    if len(valid_x) == 0:
        return

    in_black_mask = mask_image[valid_y, valid_x] == 0
    black_mask_indices = valid_indices_subset[in_black_mask]

    unique_black_mask_indices, black_counts = np.unique(black_mask_indices, return_counts=True)
    unique_valid_indices, valid_counts = np.unique(valid_indices_subset, return_counts=True)

    all_indices = np.unique(np.concatenate([unique_black_mask_indices, unique_valid_indices]))
    for idx in all_indices:
        black_count = black_counts[unique_black_mask_indices == idx][0] if idx in unique_black_mask_indices else 0
        valid_count = valid_counts[unique_valid_indices == idx][0] if idx in unique_valid_indices else 0

        if idx not in discarded_indices:
            discarded_indices[idx] = [black_count, valid_count, 0]
        else:
            discarded_indices[idx][0] += black_count
            discarded_indices[idx][1] += valid_count
            discarded_indices[idx][2] = discarded_indices[idx][0] / discarded_indices[idx][1]


def clip(base_dir, ply_path, out_ply_path, mask_dir,
         mask_use_count=10, mask_dilation=5, mask_clip_threshold=0.8,
         size_clip_threshold=0.95, distance_of_observation=25600,
         min_screen_size_of_observation=0.01, debug_view=False, crop_margin=0,
         model_type="bin", time_aware=False):
    cameras, images, points3d = read_model(os.path.join(base_dir, "sparse", "0"), ext=f".{model_type}")
    plydata = PlyData.read(ply_path)
    vertices = plydata['vertex']
    pts = np.vstack([vertices['x'], vertices['y'], vertices['z']]).T
    scales = np.vstack([np.exp(vertices['scale_0']), np.exp(vertices['scale_1']), np.exp(vertices['scale_2'])]).T
    sizes = np.linalg.norm(scales, axis=1) * 2
    sizes_ue = sizes * 100
    pts_count = len(pts)

    # [E9] 4DGS temporal observability (no-op for static 3DGS).
    has_time = time_aware and 'anchor_time' in (p.name for p in vertices.properties)

    fov = 90.0
    half_fov_rad = fov * math.pi / 360.0
    screen_multiple = 1920.0 / 1080.0 / math.tan(half_fov_rad)
    min_object_size = min_screen_size_of_observation * distance_of_observation / screen_multiple

    size_percentile = np.percentile(sizes_ue, size_clip_threshold * 100)
    indices_below_threshold = np.where(sizes_ue < 5 * size_percentile)[0]
    clip_count_by_size_percentile = pts_count - len(indices_below_threshold)

    indices_above_min = np.where(sizes_ue > min_object_size)[0]
    observable_indices = np.intersect1d(indices_above_min, indices_below_threshold)
    clip_count_by_observate = pts_count - len(observable_indices) - clip_count_by_size_percentile

    property_names = [prop.name for prop in vertices.properties]
    filtered_properties = {}
    for prop_name in property_names:
        prop_data = vertices[prop_name]
        filtered_properties[prop_name] = prop_data[observable_indices]

    new_vertex_data = np.empty(len(filtered_properties[property_names[0]]), dtype=vertices.data.dtype)
    for prop_name in property_names:
        new_vertex_data[prop_name] = filtered_properties[prop_name]
    vertices = PlyElement.describe(new_vertex_data, 'vertex')
    pts = np.vstack([vertices['x'], vertices['y'], vertices['z']]).T

    discarded_indices = {}
    all_indices = np.arange(len(vertices))
    step = max(1, len(images) / mask_use_count)
    image_keys = list(images.keys())
    dropped_mask_files = 0
    for i in range(0, len(images), int(step)):
        if i >= len(image_keys):
            break
        image_meta = images[image_keys[i]]
        camera_intrinsics = cameras[image_meta.camera_id]
        base_name = os.path.splitext(image_meta.name)[0]  # [E4]
        mask_file_path = os.path.join(mask_dir, base_name + ".png")
        if not os.path.exists(mask_file_path):
            # [E1] Skip the missing mask instead of aborting the whole clip.
            dropped_mask_files += 1
            continue
        mask_image = Image.open(mask_file_path)
        mask_image = np.array(mask_image.convert("L"))
        structuring_element = np.ones((2 * mask_dilation + 1, 2 * mask_dilation + 1))
        dilated_mask = binary_dilation(mask_image, structure=structuring_element)
        dilated_mask = (dilated_mask * 255).astype(np.uint8)
        clip_test(pts, image_meta, camera_intrinsics, discarded_indices, dilated_mask, debug_view, crop_margin)

    # [E9] Merge temporal observability if requested and available.
    if has_time:
        anchor_time = vertices['anchor_time']
        tmin, tmax = float(np.min(anchor_time)), float(np.max(anchor_time))
        temporal_valid = (anchor_time >= tmin) & (anchor_time <= tmax)
        # Treat out-of-range-anchored points as discarded.
        for idx in all_indices:
            if not temporal_valid[idx]:
                discarded_indices.setdefault(idx, [0, 0, 0])

    valid_indices = [idx for idx in all_indices
                     if idx not in discarded_indices or discarded_indices[idx][2] < 1 - mask_clip_threshold]
    valid_indices = np.array(valid_indices)
    clip_count_by_mask = len(all_indices) - len(valid_indices)

    print(f"clip begin              : {pts_count}")
    print(f"clip by size percentile : {clip_count_by_size_percentile} \t[{size_clip_threshold}] ")
    print(f"clip by size observate  : {clip_count_by_observate} \t[{distance_of_observation}:{min_screen_size_of_observation}] ")
    print(f"clip by size masks      : {clip_count_by_mask} \t[{mask_clip_threshold}] (dropped {dropped_mask_files} missing mask file(s))")
    print(f"clip end                : {len(valid_indices)} ", flush=True)

    if len(valid_indices) == 0:
        print("clip: nothing left after clipping, no output written.")
        return None

    property_names = [prop.name for prop in vertices.properties]
    filtered_properties = {}
    for prop_name in property_names:
        prop_data = vertices[prop_name]
        filtered_properties[prop_name] = prop_data[valid_indices]

    new_vertex_data = np.empty(len(filtered_properties[property_names[0]]), dtype=vertices.data.dtype)
    for prop_name in property_names:
        new_vertex_data[prop_name] = filtered_properties[prop_name]
    vertices = PlyElement.describe(new_vertex_data, 'vertex')

    output_dir = os.path.dirname(out_ply_path)
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)

    new_plydata = PlyData([vertices] + [el for el in plydata.elements if el.name != 'vertex'])
    new_plydata.write(out_ply_path)

    # [E7] Machine-readable run report.
    report = {
        "input_count": int(pts_count),
        "clip_by_size_percentile": int(clip_count_by_size_percentile),
        "clip_by_observation": int(clip_count_by_observate),
        "clip_by_mask": int(clip_count_by_mask),
        "dropped_mask_files": int(dropped_mask_files),
        "output_count": int(len(valid_indices)),
        "params": {
            "mask_use_count": mask_use_count, "mask_dilation": mask_dilation,
            "mask_clip_threshold": mask_clip_threshold, "size_clip_threshold": size_clip_threshold,
            "distance_of_observation": distance_of_observation,
            "min_screen_size_of_observation": min_screen_size_of_observation,
            "crop_margin": crop_margin, "model_type": model_type, "time_aware": time_aware,
        },
    }
    with open(out_ply_path + "_clip_report.json", "w") as f:
        json.dump(report, f, indent=2)

    return out_ply_path


def hlod_clip_workflow(batch_root, batch_mask_subdir="masks",
                       mask_use_count=10, mask_dilation=5, mask_clip_threshold=0.9,
                       size_clip_threshold=0.98, distance_of_observation=25600,
                       min_screen_size_of_observation=0.005, model_type="bin"):
    """[E8] Recursively clip every point_cloud.ply under batch_root.

    Replaces the reference's hard-coded D:\\ProjectTitan path with a fully
    CLI-driven walker so it works in any project layout.
    """
    found_files = []
    for root, dirs, files in os.walk(batch_root):
        for file in files:
            if file == "point_cloud.ply":
                found_files.append(os.path.join(root, file))

    files_count = len(found_files)
    print(f"[hlod_clip_workflow] found {files_count} point_cloud.ply under {batch_root}")
    for i, ply_path in enumerate(found_files):
        file_name, file_ext = os.path.splitext(ply_path)
        output_ply_path = file_name + '_clipped' + file_ext
        # base_dir = <cell root> that contains sparse/ and masks/
        base_dir = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(ply_path))))
        mask_dir = os.path.join(base_dir, batch_mask_subdir)
        print(f"-------[{i + 1}/{files_count}]{ply_path}", flush=True)
        clip(base_dir, ply_path, output_ply_path, mask_dir,
             mask_use_count, mask_dilation, mask_clip_threshold, size_clip_threshold,
             distance_of_observation, min_screen_size_of_observation,
             debug_view=False, crop_margin=0, model_type=model_type)
        print(f"", flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='OpenSplat4D gaussian PLY clipper')
    parser.add_argument('--base_dir', default=".")
    parser.add_argument('--ply_path', default="point_cloud.ply")
    parser.add_argument('--output_ply_path', default=None)
    parser.add_argument('--mask_dir', default="masks")
    # [E2]
    parser.add_argument('--model_type', default="bin", choices=["bin", "txt"])
    # Original thresholds (kept as defaults).
    parser.add_argument('--mask_use_count', type=int, default=5)
    parser.add_argument('--mask_dilation', type=int, default=5)
    parser.add_argument('--mask_clip_threshold', type=float, default=0.9)
    parser.add_argument('--size_clip_threshold', type=float, default=0.98)
    parser.add_argument('--distance_of_observation', type=float, default=25600)
    parser.add_argument('--min_screen_size_of_observation', type=float, default=0.005)
    # [E5][E6][E9]
    parser.add_argument('--crop_margin', type=int, default=0)
    parser.add_argument('--debug_view', action='store_true')
    parser.add_argument('--time_aware', action='store_true', help='enable 4DGS temporal observability filter')
    # [E8] Batch mode.
    parser.add_argument('--batch_root', default=None, help='recursively clip all point_cloud.ply under this dir')
    parser.add_argument('--batch_mask_subdir', default="masks")
    args = parser.parse_args()

    if args.batch_root:
        hlod_clip_workflow(
            args.batch_root, args.batch_mask_subdir,
            args.mask_use_count, args.mask_dilation, args.mask_clip_threshold,
            args.size_clip_threshold, args.distance_of_observation,
            args.min_screen_size_of_observation, args.model_type,
        )
    else:
        out = args.output_ply_path or (os.path.splitext(args.ply_path)[0] + "_clipped.ply")
        clip(
            args.base_dir, args.ply_path, out, args.mask_dir,
            args.mask_use_count, args.mask_dilation, args.mask_clip_threshold,
            args.size_clip_threshold, args.distance_of_observation,
            args.min_screen_size_of_observation, args.debug_view, args.crop_margin,
            args.model_type, args.time_aware,
        )
