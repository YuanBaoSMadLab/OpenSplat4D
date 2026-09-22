# Third-Party Licenses and Notices

OpenSplat4D's own source code is released under the **Apache-2.0** license (see [LICENSE](LICENSE)).
The project integrates and references third-party software, listed below with its license terms.
**Not every component is under Apache-2.0, and not every component is usable commercially.**

For convenience the inventory is split into two groups:

- **Section 1 — permissive licenses (MIT / BSD-3-Clause):** compatible with Apache-2.0 and with
  commercial use.
- **Section 2 — non-commercial licenses:** these components may **not** be used, exploited or
  distributed for commercial purposes without the licensor's prior written consent.

---

## Section 1 — Permissive components (commercially usable)

| Component | Copyright holder | License | Where it is used in this repository | Upstream |
|---|---|---|---|---|
| GaussianSplattingForUnrealEngine | Italink, 2023 | MIT | Ported/reference rendering and runtime code under `Source/`; several helper scripts under `Scripts/` (`make_depth_scale.py`, `clip_model.py`, `openplat4d_helper.py`) | https://github.com/Italink/GaussianSplattingForUnrealEngine |
| NanoGaussianSplatting | TimChen, 2026 | MIT | Cluster-LOD / compute pipeline reference for `Source/NanoGS/` and `Shaders/Private/` | https://github.com/TimChen1383/NanoGaussianSplatting |
| 4D Gaussian Splatting (training repository) | Fudan Zhang Vision Group, 2024 / fork by ueoo | MIT (top level) | Training workflow integration; the PLY attribute layout parsed by `Source/NanoGS/Private/PLYFileReader.cpp` and `Source/NanoGS/Private/FudanPthReader.cpp`; `Scripts/export_4dgs.py` | https://github.com/ueoo/4d-gaussian-splatting |
| 4D Gaussian Splatting (native 4D primitives, ICLR 2024) | Fudan Zhang Vision Group, 2024 | MIT (top level) | Reference implementation for the native 4D math (dual-quaternion 4D rotation, temporal conditioning, 4D spherindrical harmonics) | https://github.com/fudan-zvg/4d-gaussian-splatting |
| SPZ — compressed Gaussian splat format | Niantic Labs, 2024 | MIT | `Source/OpenSplat4DRuntime/Private/Compression/Spz.cpp` / `Spz.h` (full MIT text kept at `Source/OpenSplat4DRuntime/Private/Compression/LICENSE`) | Niantic Labs |
| fused-ssim | Rahul Goel, 2024 | MIT | Prebuilt training-side operator `Extensions/fused_ssim/` and `Extensions/fused_ssim_cuda.cp310-win_amd64.pyd` | https://github.com/rahul-goel/fused-ssim |
| COLMAP — model file I/O | ETH Zurich and UNC Chapel Hill, 2023 | BSD-3-Clause | `Scripts/read_write_model.py` (keeps its original copyright and BSD-3-Clause header) | https://github.com/colmap/colmap |

These components may be redistributed and used commercially provided their copyright notices and
license texts are retained. Where a component's full license text is not stored in this repository,
it is the upstream file listed above.

---

## Section 2 — Non-commercial components (NOT usable commercially as shipped)

The following parts of this repository originate from the **Gaussian-Splatting** software owned by
**Inria** and the **Max Planck Institut for Informatik (MPII)**, distributed under the
"Inria / MPII Gaussian-Splatting License". That license permits non-exclusive use **for research
and evaluation purposes only**, and states:

> **"THE USER CANNOT USE, EXPLOIT OR DISTRIBUTE THE *SOFTWARE* FOR COMMERCIAL PURPOSES WITHOUT
> PRIOR AND EXPLICIT CONSENT OF LICENSORS. YOU MUST CONTACT INRIA FOR ANY UNAUTHORIZED USE:
> stip-sophia.transfert@inria.fr. ANY SUCH ACTION WILL CONSTITUTE A FORGERY."**

### 2.1 Prebuilt CUDA extensions under `Extensions/`

| Path | Upstream | License |
|---|---|---|
| `Extensions/diff_gaussian_rasterization/__init__.py` and `Extensions/diff_gaussian_rasterization/_C.cp310-win_amd64.pyd` | https://github.com/graphdeco-inria/diff-gaussian-rasterization | Inria/MPII Gaussian-Splatting License (non-commercial) |
| `Extensions/simple_knn/__init__.py` and `Extensions/simple_knn/_C.cp310-win_amd64.pyd` | https://gitlab.inria.fr/bkerbl/simple-knn | Inria/MPII Gaussian-Splatting License (non-commercial) |

`Extensions/diff_gaussian_rasterization/__init__.py` carries the original header:

```
Copyright (C) 2023, Inria
GRAPHDECO research group, https://team.inria.fr/graphdeco
All rights reserved.
This software is free for non-commercial, research and evaluation use
under the terms of the LICENSE.md file.
For inquiries contact  george.drettakis@inria.fr
```

These are training-side operators. They are **not required by the Unreal Engine plugin at runtime**;
they exist so the in-editor training workflow can run locally.

### 2.2 Native-4D evaluation path in `Shaders/Private/CalcViewData.usf`

The native 4D (dual-quaternion) branch of `CalcViewData.usf` is a port of the functions
`computeCov3D_conditional` and `computeColorFromSH_4D` from `forward.cu` in the
`diff-gaussian-rasterization` tree that ships with the Fudan 4DGS training repository
(`teachers/4d-gaussian-splatting/diff-gaussian-rasterization/cuda_rasterizer/forward.cu`). The file
from which the math was transposed is headed:

```
Copyright (C) 2023, Inria
GRAPHDECO research group, https://team.inria.fr/graphdeco
All rights reserved.
This software is free for non-commercial, research and evaluation use
under the terms of the LICENSE.md file.
```

Note on the mixed provenance: the 4D-specific functions in that file were added by the Fudan
authors (the Fudan repository's own code is MIT), but the file itself is distributed under the Inria
header and is a derivative of the Inria rasterizer. The conservative reading — and the one this
notice follows — is that the non-commercial term applies to the ported code.

**Consequence:** as currently shipped, OpenSplat4D as a whole is **not cleared for commercial use**.
Two paths to resolve this:

1. Obtain commercial licensing from Inria (contact: stip-sophia.transfert@inria.fr), or
2. Replace the components in 2.1 and 2.2 with independent implementations — the native 4D math
   (4D rotation from a dual quaternion, Schur temporal conditioning of the 4D covariance, and 4D
   spherindrical harmonics) is fully specified in the published papers and can be reimplemented
   without transposing Inria source.

No GPL, LGPL or AGPL licensed code has been identified in this repository.

---

## Not distributed

`teachers/` and `ThirdParty/` exist only in the local development tree as read-only reference
material. They are not part of this repository and are not included in the released packages.

---

## Reporting

If you believe a component is missing from this inventory or is misattributed, please open an issue:
https://github.com/YuanBaoSMadLab/OpenSplat4D/issues