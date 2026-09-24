# Third-Party Licenses and Notices

OpenSplat4D's own source code is released under the **Apache-2.0** license (see [LICENSE](LICENSE)).
The project integrates and references third-party software, listed below with its license terms.
**Not every component is under Apache-2.0** — but every component that ships in this repository
**is** under a permissive license, and the repository is cleared for commercial use.

For convenience the inventory is split into two groups:

- **Section 1 — permissive licenses (MIT / BSD-3-Clause):** compatible with Apache-2.0 and with
  commercial use. This is everything that ships.
- **Section 2 — non-commercial licenses:** nothing in this repository. The section records the
  Inria-derived items that earlier revisions carried, and how they were removed or replaced, so the
  history is auditable.

---

## Section 1 — Permissive components (commercially usable)

| Component | Copyright holder | License | Where it is used in this repository | Upstream |
|---|---|---|---|---|
| GaussianSplattingForUnrealEngine | Italink, 2023 | MIT | (a) Editor-mode layer, ported from the reference plugin: `Source/OpenSplat4DEditor/` (`OpenSplat4DEdMode*`, `OpenSplat4DEdModePanel`, `OpenSplat4DSettings`, `OpenSplat4DEditorLibrary`, `OpenSplat4DStepBase`, `OpenSplat4DActorFactory`); (b) helper scripts `Content/Python/OpenSplat4D/clip_model.py`, `Content/Python/OpenSplat4D/make_depth_scale.py`, `Content/Python/OpenSplat4D/openplat4d_helper.py`, `Content/Python/OpenSplat4D/gaussian_splatting_helper.py`; (c) **binary UE assets derived from it**: `Content/Niagara/NE_OpenSplat4D.uasset` (patched by `Content/Python/OpenSplat4D/patch_niagara_template.py` from the upstream `NE_GaussianSplatting.uasset`) and `Content/Niagara/Templates/UE5_3/{M_GaussianSplattingPoint,NE_GaussianSplatting,NS_GaussianSplattingPointCloud}.uasset` (**shipped verbatim, under their original upstream names**) | https://github.com/Italink/GaussianSplattingForUnrealEngine |
| NanoGaussianSplatting | TimChen, 2026 | MIT | **Direct derivative of the entire renderer.** `Source/NanoGS/`, `Source/NanoGSEditor/` and `Shaders/Private/` are a fork of `Plugins/NanoGS/` from upstream. Files that are still **byte-identical** to upstream: `Shaders/Private/{CalcDistances.usf, CompactSplats.usf, GaussianSplatRendering.usf, GaussianSplatting.ush, GlobalAccumulatorPrefixSum.usf, PrepareIndirectArgs.usf, RadixSort.usf, SphericalHarmonics.ush}`, `Source/NanoGS/{Public/GaussianClusterBuilder.h, Public/GaussianClusterTypes.h, Public/GaussianSplatViewExtension.h, Private/GaussianGlobalAccumulator.cpp, Private/GaussianSplatShaders.cpp, Private/GaussianSplatViewExtension.cpp, NanoGS.Build.cs}` and `Source/NanoGSEditor/Private/{ActorFactoryGaussianSplat.h, GaussianSplatThumbnailRenderer.cpp, GaussianSplatThumbnailRenderer.h}`, `Source/NanoGSEditor/Public/NanoGSEditorModule.h`. Everything else (`CalcViewData.usf`, `ClusterCulling.usf`, asset/component/proxy/PLY readers, editor factories) is an extension of the same files — the 4D pipeline, Nanite precision, visibility range and sort-interval work were added on top. | https://github.com/TimChen1383/NanoGaussianSplatting |
| Splatshop | Markus Schütz, 2025 | MIT | **Design correspondence only, no code taken.** The edit-tool enum in `Source/NanoGSEditor/Public/GaussianSplatEditorData.h` documents that its tool set (rect / sphere / brush select, gizmo) was modelled on Splatshop's `actions/` directory (RectSelectAction.h, SphereSelectAction.h, BrushSelectAction.h, GizmoAction.h). The implementation (a transient `UGaussianSplatEditorData` holding `TBitArray` selection/visibility flags plus static selection algorithms) is original and does not correspond to Splatshop's class layout. Credited for the interface design. | https://github.com/m-schuetz/Splatshop |
| 4D Gaussian Splatting (training repository) | Fudan Zhang Vision Group, 2024 / fork by ueoo | MIT (top level) | Training workflow integration; the PLY attribute layout parsed by `Source/NanoGS/Private/PLYFileReader.cpp` and `Source/NanoGS/Private/FudanPthReader.cpp`; `Content/Python/OpenSplat4D/export_4dgs.py` | https://github.com/ueoo/4d-gaussian-splatting |
| 4D Gaussian Splatting (native 4D primitives, ICLR 2024) | Fudan Zhang Vision Group, 2024 | MIT (top level) | Published reference for the native 4D math (dual-quaternion 4D rotation, temporal conditioning of the 4D covariance, 4D spherindrical harmonics). OpenSplat4D's implementation in `Shaders/Private/CalcViewData.usf` is written from those papers' equations, not transposed from upstream source; no upstream code line was copied. The PLY attribute layout and the `C ∈ {1,6,16,32,33,48}` channel convention follow the Fudan format so its files import as-is | https://github.com/fudan-zvg/4d-gaussian-splatting |
| SPZ — compressed Gaussian splat format | Niantic Labs, 2024 | MIT | `Source/OpenSplat4DRuntime/ThirdParty/Compression/Spz.cpp` / `Spz.h` (full MIT text kept at `Source/OpenSplat4DRuntime/ThirdParty/Compression/LICENSE`) | Niantic Labs |
| fused-ssim | Rahul Goel, 2024 | MIT | Optional training-side operator. **Not bundled with this repository**; `Content/Python/OpenSplat4D/openplat4d_helper.py` probes for the package and builds it on demand from the user's own 3DGS checkout | https://github.com/rahul-goel/fused-ssim |
| COLMAP — model file I/O | ETH Zurich and UNC Chapel Hill, 2023 | BSD-3-Clause | `Content/Python/Lib/site-packages/read_write_model.py` (keeps its original copyright and BSD-3-Clause header) | https://github.com/colmap/colmap |

These components may be redistributed and used commercially provided their copyright notices and
license texts are retained. Where a component's full license text is not stored in this repository,
it is the upstream file listed above.

### Attribution status (read this before redistributing)

Source files derived from **NanoGaussianSplatting** and
**GaussianSplattingForUnrealEngine** currently carry only the Unreal Engine boilerplate header
(`// Copyright Epic Games, Inc. All Rights Reserved.`) — the upstream MIT copyright notices are
**not embedded in the derived files themselves**. MIT requires that the copyright notice and
permission notice accompany all copies or substantial portions of the software. This is satisfied
for the released packages only because this file is bundled with them; if you fork OpenSplat4D,
strip this file, or re-license the tree, you must re-add the two upstream MIT notices
(Italink, 2023 and TimChen, 2026) yourself.

The `Content/Niagara/Templates/UE5_3/*.uasset` files are redistributed **unmodified and under their
original upstream asset names**; they are Italink's assets, not OpenSplat4D's.

---

## Section 2 — Non-commercial components (NOT bundled)

This repository contains **no** component under a non-commercial license. The two Inria-derived
items that an earlier revision of this project carried have both been removed or replaced:

### 2.1 CUDA training extensions (`diff_gaussian_rasterization`, `simple_knn`) — removed

Earlier releases shipped prebuilt training-side operators under `Extensions/`:

| Removed path | Upstream | License |
|---|---|---|
| `Extensions/diff_gaussian_rasterization/` (`__init__.py` + `_C.cp310-win_amd64.pyd`) | https://github.com/graphdeco-inria/diff-gaussian-rasterization | Inria/MPII Gaussian-Splatting License (non-commercial) |
| `Extensions/simple_knn/` (`__init__.py` + `_C.cp310-win_amd64.pyd`) | https://gitlab.inria.fr/bkerbl/simple-knn | Inria/MPII Gaussian-Splatting License (non-commercial) |

Those binaries are **no longer part of the repository and are excluded from the released packages**.
The `Extensions/` folder was first retired in favour of the Fab-prescribed location for third-party
Python libraries, `Content/Python/Lib/site-packages/`, and the extensions themselves were then dropped
from the distribution entirely (this also covers `fused_ssim`, which is MIT — see Section 1 — but is
likewise not shipped). That folder is now a **local-only build output**: it is git-ignored and is
explicitly excluded from the package by `Config/FilterPlugin.ini`, so no compiled extension binary is
ever redistributed. The extensions were never used by the Unreal Engine plugin at runtime — only by the
optional training workflow, which imports them by package name.

**The plugin ships no Python runtime, no COLMAP and no training repository either.** The training
pipeline (COLMAP reconstruction + the Inria 3DGS / Fudan 4DGS training repos) is entirely the user's
own installation; it is configured under *Project Settings > OpenSplat4D*. Rendering, importing and
playing back splat assets need none of it.

**If you want to run the in-editor training workflow**, you must obtain those extensions yourself
from their upstream projects and accept Inria's terms directly, then compile them locally.
`Content/Python/OpenSplat4D/openplat4d_helper.py` probes for them by import; when they are absent it
prints the failed probe, then compiles them from your own 3DGS checkout if a CUDA toolchain is
available, or skips them with an explicit message.

### 2.2 Native-4D evaluation path in `Shaders/Private/CalcViewData.usf` — independently reimplemented

An earlier revision built this branch by transposing `computeCov3D_conditional` and
`computeColorFromSH_4D` out of `forward.cu` (the Inria-headed rasterizer that ships inside the Fudan
4DGS training repository). That code has been **replaced with a clean implementation written from
the published equations**:

- the 4D covariance is accumulated directly as `Sigma4 = sum_k s_k^2 * c_k (x) c_k^T` from the four
  Hamilton products `c_k = q_l (x) e_k (x) q_r`, instead of forming three 4x4 matrices and
  multiplying them;
- the temporal conditioning keeps the same standard Gaussian marginal identity
  (`w(t) = exp(-0.5 dt^2 / (prefilter + Sigma_tt))`, `mu_x|t = mu_x + Sigma_xt dt / Sigma_tt`,
  `Sigma_xx|t = Sigma_xx - Sigma_xt (x) Sigma_xt / Sigma_tt`) expressed directly on those columns;
- the spherindrical-harmonic colour now reuses the project's own real-SH basis from
  `Shaders/Private/SphericalHarmonics.ush` (MIT), evaluated in the same band-grouped form as
  `EvaluateSH()`.

The rewrite is behaviour-preserving: on the project's own `fudan_test.ply` asset the old and new
formulations agree to `0.000e+00` in double precision for the rotation matrix, the 4D covariance and
every quantity the shader consumes, and to `9.3e-08` (one float32 ULP) under step-by-step float32
evaluation. The harness is `TestAssets/verify_native4d_rewrite.py`.

No GPL, LGPL or AGPL licensed code has been identified in this repository.

---

## Not distributed

None of the following ships with the plugin — not in the repository, not in the released packages:

- `teachers/` and `ThirdParty/` — local development reference material only (Python runtime,
  COLMAP build, Inria 3DGS / Fudan 4DGS training checkouts).
- `Content/Python/Lib/site-packages/` — local self-build output for the CUDA training extensions.
  Git-ignored and excluded from the package by `Config/FilterPlugin.ini`.

The plugin's own orchestrator scripts under `Content/Python/OpenSplat4D/` **are** shipped: they are
OpenSplat4D code (MIT-derived, see Section 1) and are what the editor steps invoke.

---

## Reporting

If you believe a component is missing from this inventory or is misattributed, please open an issue:
https://github.com/YuanBaoSMadLab/OpenSplat4D/issues