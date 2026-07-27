#-*- coding: utf-8 -*-
"""
train_enhanced.py — OpenSplat4D 增强训练入口
============================================

为什么需要这个文件
------------------
teachers/4d-gaussian-splatting/train.py 是只读的官方训练脚本，它的 loss 只包含
L1 + SSIM + (可选) opa_mask/rigid/motion，没有 depth loss，也没有针对过曝像素
的降权处理。这导致用户在 UE 中捕获的 mask/depth 资产无法发挥几何约束作用，
高斯点会在朝阳光方向、过曝区域堆积形成 "floaters"。

本文件作为 overlay，复用 teachers 的 GaussianModel / Scene / render，但替换
训练循环中的 loss 计算：

  1. depth_loss   — 把 UE 捕获的 metric depth 加入 L1 约束，把高斯点限制在表面
  2. overexp_downweight — 检测 gt_image 中 >=0.95 的饱和像素，在 L1 中降权 0.2
  3. mask_aware_l1 — 用 soft mask 对 L1 加权，让背景像素 (mask≈0) 不参与 loss
  4. sky_entropy  — alpha 在背景区域应接近 0，用 BCE 约束 (替代原 opa_mask)

使用方法
--------
UE 编辑器【设置】→ OpenSplat4D → GaussianTrainingParams 中把 train.py 路径
改为本文件，或在 openplat4d_helper.py 调用时通过 `--gaussian` 指向本目录。

  python train_enhanced.py -s <workDir> -m ./output --depths ./depths \
      --lambda_depth 0.05 --overexp_threshold 0.95 --overexp_weight 0.2 \
      --iterations 30000 --resolution 1

新增 CLI 参数
-------------
  --lambda_depth        float, default 0.05
                        depth loss 权重。0 = 关闭（行为同原版 train.py）。
                        推荐范围 0.02~0.1。过大会让颜色变模糊。
  --overexp_threshold   float, default 0.95
                        像素亮度超过此值视为过曝，触发降权。
  --overexp_weight      float, default 0.2
                        过曝像素在 L1 中的权重 (0=完全忽略，1=不降权)。
  --mask_aware_l1       int, default 1
                        1 = 用 gt_alpha_mask 对 L1 加权 (背景像素权重→0)。
                        0 = 不加权（行为同原版）。
  --resolution_coarse   int, default 1024
                        Coarse-to-Fine 第一阶段分辨率（长边）。0 = 关闭。
  --coarse_until_iter   int, default 5000
                        前多少迭代用 coarse 分辨率，之后切到全分辨率。
  --densify_grad_threshold_2k  float, default 0.0004
                        2K 图像推荐的致密化梯度阈值（默认 0.0002 对 2K 过于激进）。

depth 文件格式
--------------
UE 端 (OpenSplat4DStepCapture.cpp) 写出的 depth 是 16-bit PNG，值域 [0, 65535]
对应 [0, 1] 归一化距离（0=背景/天空，1=全局最远距离 GlobalMaxDistCm）。
本脚本读取后直接作为 inverse depth 使用（值越大越近，0=无效）。
"""

import os
import sys
import math
import argparse
import random
import uuid
from copy import deepcopy

import numpy as np
import torch
import torch.nn.functional as F
import cv2
from PIL import Image
from tqdm import tqdm
from torch.utils.data import DataLoader

# --- 把 teachers 路径加到 sys.path，复用其模块 ---
# 本文件位于 OpenSplat4D/Scripts/，teachers 在 OpenSplat4D/../teachers/4d-gaussian-splatting/
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
TEACHERS_DIR = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "teachers", "4d-gaussian-splatting"))
if TEACHERS_DIR not in sys.path:
    sys.path.insert(0, TEACHERS_DIR)

# 现在可以 import teachers 的模块
from arguments import ModelParams, OptimizationParams, PipelineParams
from gaussian_renderer import render
from scene import GaussianModel, Scene
from utils.general_utils import safe_state, knn
from utils.image_utils import psnr
from utils.loss_utils import l1_loss, ssim

try:
    from torch.utils.tensorboard import SummaryWriter
    TENSORBOARD_FOUND = True
except ImportError:
    TENSORBOARD_FOUND = False


# ============================================================================
# 工具：Unicode 安全的图像读取（Windows OpenCV 兼容中文路径）
# ============================================================================
def imread_unicode(path, flags=cv2.IMREAD_UNCHANGED):
    if not path or not os.path.exists(path):
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


# ============================================================================
# 增强的 Loss 函数
# ============================================================================
def mask_aware_l1(rendered, gt, mask, overexp_threshold=0.95, overexp_weight=0.2):
    """
    带 soft mask 加权和过曝降权的 L1 loss。

    Parameters
    ----------
    rendered : Tensor [3, H, W]  渲染图
    gt       : Tensor [3, H, W]  ground truth
    mask     : Tensor [1, H, W]  soft mask（1=前景，0=背景）或 None
    overexp_threshold : float    像素亮度 >= 此值视为过曝
    overexp_weight    : float    过曝像素的 L1 权重

    Returns
    -------
    Tensor scalar  加权 L1 均值
    """
    pixel_l1 = torch.abs(rendered - gt)              # [3, H, W]
    pixel_l1 = pixel_l1.mean(dim=0, keepdim=True)    # [1, H, W]

    # 基础权重：soft mask（背景 0，前景 1，边缘 0~1）
    weight = torch.ones_like(pixel_l1)
    if mask is not None:
        weight = weight * mask

    # 过曝降权：gt 平均亮度 >= threshold 的像素权重 *= overexp_weight
    gt_brightness = gt.mean(dim=0, keepdim=True)     # [1, H, W]
    overexp_mask = (gt_brightness >= overexp_threshold).float()
    weight = weight * ((1.0 - overexp_mask) + overexp_mask * overexp_weight)

    return (pixel_l1 * weight).sum() / (weight.sum() + 1e-8)


def depth_loss(rendered_depth, gt_depth, gt_alpha_mask=None, valid_thresh=1e-3):
    """
    深度 L1 loss，只在有效像素处计算。

    UE 写出的 depth 是 16-bit PNG 归一化到 [0, 1]，0 = 背景/天空（无效）。
    rendered_depth 来自 rasterizer 的 depth 输出，单位是世界距离的归一化值。

    Parameters
    ----------
    rendered_depth : Tensor [1, H, W]  渲染深度
    gt_depth       : Tensor [1, H, W]  ground truth 深度
    gt_alpha_mask  : Tensor [1, H, W]  可选 soft mask，进一步限制前景区域
    valid_thresh   : float             gt_depth >= 此值才视为有效
    """
    valid = (gt_depth > valid_thresh).float()
    if gt_alpha_mask is not None:
        valid = valid * gt_alpha_mask
    if valid.sum() < 1e-3:
        return torch.tensor(0.0, device=rendered_depth.device)
    diff = torch.abs(rendered_depth - gt_depth) * valid
    return diff.sum() / (valid.sum() + 1e-8)


def sky_bce_loss(alpha, gt_alpha_mask):
    """
    背景区域 alpha 应趋近 0，前景区域不强约束。
    用 BCE 替代原 opa_mask loss，更稳定。
    alpha: [1, H, W] 渲染 alpha
    gt_alpha_mask: [1, H, W] ground truth mask (1=前景, 0=背景)
    """
    if gt_alpha_mask is None:
        return torch.tensor(0.0, device=alpha.device)
    sky = 1.0 - gt_alpha_mask
    eps = 1e-6
    a = alpha.clamp(eps, 1 - eps)
    # 只在 sky 区域推 alpha→0；前景区域不推（让 splat 自由覆盖）
    bce = -sky * torch.log(1 - a)
    return bce.mean()


# ============================================================================
# Coarse-to-Fine resolution 切换
# ============================================================================
def maybe_coarse_resolution(cam, coarse_res, full_res):
    """
    在 coarse 阶段把相机分辨率降到 coarse_res（长边），保留宽高比。
    通过修改 cam.image_width/height 和 image tensor 实现。
    返回 (image, gt_alpha_mask, depth) 的降采样版本（如果在 coarse 阶段）。
    """
    if coarse_res <= 0 or coarse_res >= full_res:
        return None  # 不降采样
    scale = float(coarse_res) / float(max(cam.image_width, cam.image_height))
    if scale >= 1.0:
        return None
    new_w = int(cam.image_width * scale)
    new_h = int(cam.image_height * scale)
    return (new_w, new_h, scale)


def resize_tensor(tensor_3c, new_w, new_h):
    """[3, H, W] or [1, H, W] → [C, new_h, new_w] using bilinear."""
    if tensor_3c is None:
        return None
    return F.interpolate(
        tensor_3c.unsqueeze(0).float(),
        size=(new_h, new_w),
        mode="bilinear",
        align_corners=False,
    ).squeeze(0)


# ============================================================================
# 加载 depth 图（UE 写出的 16-bit PNG，归一化到 [0, 1]）
# ============================================================================
def load_depth_png(depth_path, target_h, target_w):
    """
    读取 UE 捕获的 depth PNG（16-bit，值域 [0, 65535] 对应 [0, 1] 距离）。
    返回 [1, H, W] float32 tensor on CPU，0 = 背景/无效。
    """
    if not os.path.exists(depth_path):
        return None
    arr = imread_unicode(depth_path, cv2.IMREAD_UNCHANGED)
    if arr is None:
        return None
    if arr.ndim == 3:
        arr = arr[..., 0]
    arr = arr.astype(np.float32) / 65535.0
    # 如果尺寸不匹配（coarse 阶段），用 bilinear 重采样
    if arr.shape[0] != target_h or arr.shape[1] != target_w:
        arr = cv2.resize(arr, (target_w, target_h), interpolation=cv2.INTER_LINEAR)
    return torch.from_numpy(arr).unsqueeze(0)  # [1, H, W]


# ============================================================================
# 主训练循环
# ============================================================================
def training_enhanced(
    dataset,
    opt,
    pipe,
    testing_iterations,
    saving_iterations,
    checkpoint,
    debug_from,
    gaussian_dim,
    time_duration,
    num_pts,
    num_pts_ratio,
    rot_4d,
    force_sh_3d,
    batch_size,
    # 新增参数
    lambda_depth=0.05,
    overexp_threshold=0.95,
    overexp_weight=0.2,
    mask_aware=1,
    sky_bce=0.0,
    resolution_coarse=0,
    coarse_until_iter=5000,
):
    if dataset.frame_ratio > 1:
        time_duration = [time_duration[0] / dataset.frame_ratio, time_duration[1] / dataset.frame_ratio]

    first_iter = 0
    tb_writer = prepare_output_and_logger(dataset)
    gaussians = GaussianModel(
        dataset.sh_degree,
        gaussian_dim=gaussian_dim,
        time_duration=time_duration,
        rot_4d=rot_4d,
        force_sh_3d=force_sh_3d,
        sh_degree_t=2 if pipe.eval_shfs_4d else 0,
    )
    scene = Scene(dataset, gaussians, num_pts=num_pts, num_pts_ratio=num_pts_ratio, time_duration=time_duration)
    gaussians.training_setup(opt)

    if checkpoint:
        (model_params, first_iter) = torch.load(checkpoint)
        gaussians.restore(model_params, opt)

    bg_color = [1, 1, 1] if dataset.white_background else [0, 0, 0]
    background = torch.tensor(bg_color, dtype=torch.float32, device="cuda")

    iter_start = torch.cuda.Event(enable_timing=True)
    iter_end = torch.cuda.Event(enable_timing=True)

    best_psnr = 0.0
    ema_loss_for_log = 0.0
    ema_l1loss_for_log = 0.0
    ema_ssimloss_for_log = 0.0
    ema_depth_loss_for_log = 0.0

    progress_bar = tqdm(range(first_iter, opt.iterations), desc="Training (enhanced)")
    first_iter += 1

    if pipe.env_map_res:
        env_map = nn.Parameter(
            torch.zeros((3, pipe.env_map_res, pipe.env_map_res), dtype=torch.float, device="cuda").requires_grad_(True)
        )
        env_map_optimizer = torch.optim.Adam([env_map], lr=opt.feature_lr, eps=1e-15)
    else:
        env_map = None

    gaussians.env_map = env_map

    training_dataset = scene.getTrainCameras()
    training_dataloader = DataLoader(
        training_dataset,
        batch_size=batch_size,
        shuffle=True,
        num_workers=12 if dataset.dataloader else 0,
        collate_fn=lambda x: x,
        drop_last=True,
    )

    # 检测是否有 depth 目录
    depths_dir = os.path.join(dataset.source_path, "depths")
    has_depths = os.path.isdir(depths_dir)
    if lambda_depth > 0 and not has_depths:
        print(f"[train_enhanced] WARN --lambda_depth={lambda_depth} 但 {depths_dir} 不存在，depth loss 会被跳过")
        lambda_depth = 0.0

    iteration = first_iter
    while iteration < opt.iterations + 1:
        for batch_data in training_dataloader:
            iteration += 1
            if iteration > opt.iterations:
                break

            iter_start.record()
            gaussians.update_learning_rate(iteration)

            if iteration % opt.sh_increase_interval == 0:
                gaussians.oneupSHdegree()

            if (iteration - 1) == debug_from:
                pipe.debug = True

            # Coarse-to-Fine: 在 coarse_until_iter 之前用降采样分辨率
            in_coarse = resolution_coarse > 0 and iteration <= coarse_until_iter

            batch_point_grad = []
            batch_visibility_filter = []
            batch_radii = []

            for batch_idx in range(batch_size):
                gt_image, viewpoint_cam = batch_data[batch_idx]
                gt_image = gt_image.cuda()
                viewpoint_cam = viewpoint_cam.cuda()

                # Coarse-to-Fine: 降采样 gt_image 和相机分辨率
                if in_coarse:
                    new_size = maybe_coarse_resolution(
                        viewpoint_cam, resolution_coarse,
                        max(viewpoint_cam.image_width, viewpoint_cam.image_height)
                    )
                    if new_size is not None:
                        new_w, new_h, _ = new_size
                        # 临时修改相机分辨率（render 会用这个）
                        # 注意：这不是优雅做法，但 teachers Camera 没有 resize 方法
                        orig_w, orig_h = viewpoint_cam.image_width, viewpoint_cam.image_height
                        viewpoint_cam.image_width = new_w
                        viewpoint_cam.image_height = new_h
                        # 重新计算 FoV（保持焦距，分辨率变化不影响 FoV）
                        # 但 rasterizer 用 image_width/height 输出，所以直接 resize gt
                        gt_image = resize_tensor(gt_image, new_w, new_h).to(gt_image.device)

                render_pkg = render(viewpoint_cam, gaussians, pipe, background)
                image, viewspace_point_tensor, visibility_filter, radii = (
                    render_pkg["render"],
                    render_pkg["viewspace_points"],
                    render_pkg["visibility_filter"],
                    render_pkg["radii"],
                )
                depth = render_pkg["depth"]
                alpha = render_pkg["alpha"]

                # 恢复相机分辨率（coarse 阶段后）
                if in_coarse and new_size is not None:
                    viewpoint_cam.image_width = orig_w
                    viewpoint_cam.image_height = orig_h

                # 获取 soft mask 和 gt depth
                gt_mask = viewpoint_cam.gt_alpha_mask  # [1, H, W] or None
                if gt_mask is not None and gt_mask.shape[-1] != image.shape[-1]:
                    gt_mask = resize_tensor(gt_mask, image.shape[-1], image.shape[-2])
                    gt_mask = gt_mask.to(image.device)

                gt_depth = None
                if lambda_depth > 0:
                    depth_path = os.path.join(depths_dir, viewpoint_cam.image_name + ".png")
                    gt_depth_cpu = load_depth_png(depth_path, image.shape[-2], image.shape[-1])
                    if gt_depth_cpu is not None:
                        gt_depth = gt_depth_cpu.to(image.device)

                # ============ Loss 计算 ============
                if mask_aware:
                    Ll1 = mask_aware_l1(image, gt_image, gt_mask, overexp_threshold, overexp_weight)
                else:
                    Ll1 = l1_loss(image, gt_image)
                Lssim = 1.0 - ssim(image, gt_image)
                loss = (1.0 - opt.lambda_dssim) * Ll1 + opt.lambda_dssim * Lssim

                # opa mask loss (原版保留)
                if opt.lambda_opa_mask > 0:
                    o = alpha.clamp(1e-6, 1 - 1e-6)
                    if gt_mask is not None:
                        sky = 1.0 - gt_mask
                    else:
                        # 无 mask 时 opa_mask loss 退化为 0（无 sky 区域可约束）
                        sky = torch.zeros_like(alpha)
                    Lopa_mask = (-sky * torch.log(1 - o)).mean()
                    loss = loss + opt.lambda_opa_mask * Lopa_mask

                # sky BCE loss (新增，可选)
                if sky_bce > 0:
                    Lsky = sky_bce_loss(alpha, gt_mask)
                    loss = loss + sky_bce * Lsky

                # depth loss (新增)
                Ldepth = torch.tensor(0.0, device=image.device)
                if lambda_depth > 0 and gt_depth is not None:
                    # rendered_depth: [1, H, W] or [H, W]
                    rendered_depth_1c = depth if depth.dim() == 3 else depth.unsqueeze(0)
                    if rendered_depth_1c.shape[-1] != gt_depth.shape[-1]:
                        rendered_depth_1c = resize_tensor(rendered_depth_1c, gt_depth.shape[-1], gt_depth.shape[-2])
                    Ldepth = depth_loss(rendered_depth_1c, gt_depth, gt_mask)
                    loss = loss + lambda_depth * Ldepth

                # rigid / motion (原版保留)
                if opt.lambda_rigid > 0:
                    k = 20
                    xyz_mean = gaussians.get_xyz
                    xyz_cur = xyz_mean
                    idx, dist = knn(xyz_cur[None].contiguous().detach(), xyz_cur[None].contiguous().detach(), k)
                    _, velocity = gaussians.get_current_covariance_and_mean_offset(1.0, gaussians.get_t + 0.1)
                    weight = torch.exp(-100 * dist)
                    vel_dist = torch.norm(velocity[idx] - velocity[None, :, None], p=2, dim=-1)
                    Lrigid = (weight * vel_dist).sum() / k / xyz_cur.shape[0]
                    loss = loss + opt.lambda_rigid * Lrigid

                if opt.lambda_motion > 0:
                    _, velocity = gaussians.get_current_covariance_and_mean_offset(1.0, gaussians.get_t + 0.1)
                    Lmotion = velocity.norm(p=2, dim=1).mean()
                    loss = loss + opt.lambda_motion * Lmotion

                loss = loss / batch_size
                loss.backward()
                batch_point_grad.append(torch.norm(viewspace_point_tensor.grad[:, :2], dim=-1))
                batch_radii.append(radii)
                batch_visibility_filter.append(visibility_filter)

            if batch_size > 1:
                visibility_count = torch.stack(batch_visibility_filter, 1).sum(1)
                visibility_filter = visibility_count > 0
                radii = torch.stack(batch_radii, 1).max(1)[0]
                batch_viewspace_point_grad = torch.stack(batch_point_grad, 1).sum(1)
                batch_viewspace_point_grad[visibility_filter] = (
                    batch_viewspace_point_grad[visibility_filter] * batch_size / visibility_count[visibility_filter]
                )
                batch_viewspace_point_grad = batch_viewspace_point_grad.unsqueeze(1)
                if gaussians.gaussian_dim == 4:
                    batch_t_grad = gaussians._t.grad.clone()[:, 0].detach()
                    batch_t_grad[visibility_filter] = (
                        batch_t_grad[visibility_filter] * batch_size / visibility_count[visibility_filter]
                    )
                    batch_t_grad = batch_t_grad.unsqueeze(1)
            else:
                if gaussians.gaussian_dim == 4:
                    batch_t_grad = gaussians._t.grad.clone().detach()

            iter_end.record()
            loss_dict = {"Ll1": Ll1, "Lssim": Lssim, "Ldepth": Ldepth}

            with torch.no_grad():
                psnr_for_log = psnr(image, gt_image).mean().double()
                ema_loss_for_log = 0.4 * loss.item() + 0.6 * ema_loss_for_log
                ema_l1loss_for_log = 0.4 * Ll1.item() + 0.6 * ema_l1loss_for_log
                ema_ssimloss_for_log = 0.4 * Lssim.item() + 0.6 * ema_ssimloss_for_log
                ema_depth_loss_for_log = 0.4 * Ldepth.item() + 0.6 * ema_depth_loss_for_log

                if iteration % 10 == 0:
                    postfix = {
                        "Loss": f"{ema_loss_for_log:.{7}f}",
                        "PSNR": f"{psnr_for_log:.{2}f}",
                        "Ll1": f"{ema_l1loss_for_log:.{4}f}",
                        "Lssim": f"{ema_ssimloss_for_log:.{4}f}",
                        "Ldepth": f"{ema_depth_loss_for_log:.{4}f}",
                        "Coarse": "Y" if in_coarse else "N",
                    }
                    progress_bar.set_postfix(postfix)
                    progress_bar.update(10)
                if iteration == opt.iterations:
                    progress_bar.close()

                # Densification (原版逻辑，参数可被 --densify_grad_threshold_2k 覆盖)
                if iteration < opt.densify_until_iter and (
                    opt.densify_until_num_points < 0 or gaussians.get_xyz.shape[0] < opt.densify_until_num_points
                ):
                    gaussians.max_radii2D[visibility_filter] = torch.max(
                        gaussians.max_radii2D[visibility_filter], radii[visibility_filter]
                    )
                    if batch_size == 1:
                        gaussians.add_densification_stats(
                            viewspace_point_tensor,
                            visibility_filter,
                            batch_t_grad if gaussians.gaussian_dim == 4 else None,
                        )
                    else:
                        gaussians.add_densification_stats_grad(
                            batch_viewspace_point_grad,
                            visibility_filter,
                            batch_t_grad if gaussians.gaussian_dim == 4 else None,
                        )

                    if iteration > opt.densify_from_iter and iteration % opt.densification_interval == 0:
                        size_threshold = 20 if iteration > opt.opacity_reset_interval else None
                        gaussians.densify_and_prune(
                            opt.densify_grad_threshold,
                            opt.thresh_opa_prune,
                            scene.cameras_extent,
                            size_threshold,
                            opt.densify_grad_t_threshold,
                        )

                    if iteration % opt.opacity_reset_interval == 0 or (
                        dataset.white_background and iteration == opt.densify_from_iter
                    ):
                        gaussians.reset_opacity()

                # Optimizer step
                if iteration < opt.iterations:
                    gaussians.optimizer.step()
                    gaussians.optimizer.zero_grad(set_to_none=True)
                    if pipe.env_map_res and iteration < pipe.env_optimize_until:
                        env_map_optimizer.step()
                        env_map_optimizer.zero_grad(set_to_none=True)


def prepare_output_and_logger(args):
    if not args.model_path:
        if os.getenv("OAR_JOB_ID"):
            unique_str = os.getenv("OAR_JOB_ID")
        else:
            unique_str = str(uuid.uuid4())
        args.model_path = os.path.join("./output/", unique_str[0:10])
    print("Output folder: {}".format(args.model_path))
    os.makedirs(args.model_path, exist_ok=True)
    with open(os.path.join(args.model_path, "cfg_args"), "w") as cfg_log_f:
        cfg_log_f.write(str(argparse.Namespace(**vars(args))))
    tb_writer = None
    if TENSORBOARD_FOUND:
        tb_writer = SummaryWriter(args.model_path)
    else:
        print("Tensorboard not available: not logging progress")
    return tb_writer


def setup_seed(seed):
    import random as _random
    torch.manual_seed(seed)
    torch.cuda.manual_seed_all(seed)
    np.random.seed(seed)
    _random.seed(seed)
    torch.backends.cudnn.deterministic = True


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="OpenSplat4D enhanced training")
    lp = ModelParams(parser)
    op = OptimizationParams(parser)
    pp = PipelineParams(parser)
    parser.add_argument("--config", type=str)
    parser.add_argument("--debug_from", type=int, default=-1)
    parser.add_argument("--detect_anomaly", action="store_true", default=False)
    parser.add_argument("--test_iterations", nargs="+", type=int, default=[7_000, 30_000])
    parser.add_argument("--save_iterations", nargs="+", type=int, default=[7_000, 30_000])
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--start_checkpoint", type=str, default=None)

    parser.add_argument("--gaussian_dim", type=int, default=3)
    parser.add_argument("--time_duration", nargs=2, type=float, default=[-0.5, 0.5])
    parser.add_argument("--num_pts", type=int, default=100_000)
    parser.add_argument("--num_pts_ratio", type=float, default=1.0)
    parser.add_argument("--rot_4d", action="store_true")
    parser.add_argument("--force_sh_3d", action="store_true")
    parser.add_argument("--batch_size", type=int, default=1)
    parser.add_argument("--seed", type=int, default=6666)
    parser.add_argument("--exhaust_test", action="store_true")

    # ---- OpenSplat4D 增强参数 ----
    parser.add_argument("--lambda_depth", type=float, default=0.05,
                        help="depth loss 权重。0=关闭。推荐 0.02~0.1")
    parser.add_argument("--overexp_threshold", type=float, default=0.95,
                        help="过曝像素亮度阈值")
    parser.add_argument("--overexp_weight", type=float, default=0.2,
                        help="过曝像素在 L1 中的权重 (0=完全忽略, 1=不降权)")
    parser.add_argument("--mask_aware", type=int, default=1,
                        help="1=用 soft mask 加权 L1, 0=不加权")
    parser.add_argument("--sky_bce", type=float, default=0.0,
                        help="sky BCE loss 权重，约束背景 alpha→0")
    parser.add_argument("--resolution_coarse", type=int, default=0,
                        help="Coarse-to-Fine 第一阶段长边分辨率。0=关闭")
    parser.add_argument("--coarse_until_iter", type=int, default=5000,
                        help="前多少迭代用 coarse 分辨率")
    parser.add_argument("--densify_grad_threshold_2k", type=float, default=0.0004,
                        help="2K 图像推荐的致密化梯度阈值（默认 0.0004）")

    args = parser.parse_args(sys.argv[1:])
    args.save_iterations.append(args.iterations)

    # 用 2K 推荐阈值覆盖默认 densify_grad_threshold
    # (仅当用户没显式传 --densify_grad_threshold 时生效)
    # 简化处理：直接覆盖（用户可以显式传 --densify_grad_threshold 0.0002 走原版）
    if args.densify_grad_threshold == 0.0002:  # 默认值
        args.densify_grad_threshold = args.densify_grad_threshold_2k

    if args.config:
        from omegaconf import OmegaConf
        from omegaconf.dictconfig import DictConfig
        cfg = OmegaConf.load(args.config)
        def recursive_merge(key, host):
            if isinstance(host[key], DictConfig):
                for key1 in host[key].keys():
                    recursive_merge(key1, host[key])
            else:
                assert hasattr(args, key), key
                setattr(args, key, host[key])
        for k in cfg.keys():
            recursive_merge(k, cfg)

    if args.exhaust_test:
        args.test_iterations = args.test_iterations + [i for i in range(0, args.iterations, 500)]

    setup_seed(args.seed)
    print("Optimizing (enhanced) " + args.model_path)
    safe_state(args.quiet)
    torch.autograd.set_detect_anomaly(args.detect_anomaly)

    training_enhanced(
        lp.extract(args),
        op.extract(args),
        pp.extract(args),
        args.test_iterations,
        args.save_iterations,
        args.start_checkpoint,
        args.debug_from,
        args.gaussian_dim,
        args.time_duration,
        args.num_pts,
        args.num_pts_ratio,
        args.rot_4d,
        args.force_sh_3d,
        args.batch_size,
        # 增强参数
        args.lambda_depth,
        args.overexp_threshold,
        args.overexp_weight,
        args.mask_aware,
        args.sky_bce,
        args.resolution_coarse,
        args.coarse_until_iter,
    )

    print("\nTraining (enhanced) complete.")
