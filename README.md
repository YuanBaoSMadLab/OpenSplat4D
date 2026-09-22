# OpenSplat4D

**Unreal Engine plugin for rendering 3D and 4D Gaussian Splatting (3DGS / 4DGS) in real time.**

OpenSplat4D brings Gaussian Splatting into Unreal Engine 5 as a native C++ plugin. It renders
static 3D Gaussian Splatting models (`.ply`) **and** time-varying 4D Gaussian Splatting models —
Spacetime Gaussians, keyframe/sequence 4DGS, and Fudan native 4DGS (dual-quaternion 4D rotation
+ 4D spherindrical harmonics) — with a GPU compute pipeline, Nanite-style cluster LODs, indirect
draw, and a dedicated 4D player editor.

- Version: **0.2**
- Engine: **Unreal Engine 5.5 – 6.0** (built and verified on UE 5.7, 5.8, 6.0 — Windows / Win64)
- License: **Apache-2.0**
- Download: [GitHub Releases](https://github.com/YuanBaoSMadLab/OpenSplat4D/releases) (prebuilt per UE version)

---

## What is this project?

Gaussian Splatting is a radiance-field representation that reconstructs photorealistic scenes from
photos as millions of 3D Gaussian primitives. Its 4D extension (4DGS / dynamic Gaussian Splatting)
additionally models **time**, so the scene moves: people walk, cloth deforms, flames flicker.

OpenSplat4D is the Unreal Engine side of that pipeline. It solves three practical problems:

1. **Rendering** — a self-contained GPU renderer for Gaussian Splatting inside UE, with LOD,
   sorting, culling, and shading, without needing a Niagara System asset.
2. **4D playback** — a time axis for the point cloud, evaluated on the GPU, driven by a
   level-sequencer-style 4D player editor.
3. **Production workflow** — import, train (capture → COLMAP → 3DGS/4DGS training), and
   World Partition HLOD generation, all from inside the editor.

### Keywords

Gaussian Splatting, 3DGS, 4DGS, 4D Gaussian Splatting, dynamic Gaussian Splatting, Unreal Engine 5
plugin, UE5 plugin, real-time rendering, Nanite cluster LOD, compute shader splatting, PLY import,
PyTorch checkpoint import, Spacetime Gaussians, spherindrical harmonics, dual quaternion, World
Partition HLOD, COLMAP, novel view synthesis, point cloud.

---

## Supported input formats

The importer **auto-detects the model type from the file itself** — no manual mode selection.

| Format | Extension | Detected as | Notes |
|---|---|---|---|
| Standard 3DGS PLY (INRIA / Postshot / most trainers) | `.ply` | Static 3D | Position, rotation, scale, opacity, SH |
| SPZ compressed 3DGS (Niantic) | `.spz` | Static 3D | Runtime point-cloud path |
| Spacetime Gaussians (STG) | `.ply` | **4D — temporal marginalization** | Needs `t` + `scale_t`; `motion_0..2` adds velocity extrapolation |
| Fudan 4DGS (native 4D primitives) | `.ply` | **4D — native 4D** | Needs `rot_0..7` (dual quaternion) + `scale_3` + `f_rest` |
| Fudan 4DGS training checkpoint | `.pth` | **4D — native 4D** | `torch.save` checkpoint, parsed directly (no export step) |
| Keyframe / baked 4DGS (4DGaussians, Deformable-3DGS, Ex4DGS) | `.ply` sequence | **4D — keyframe** | Content Browser → *Import PLY Sequence (Keyframe 4D)* |
| OpenSplat4D native container | `.o4d` | 3D or 4D | Bundles splats + keyframes + timeline; save/load from Blueprint |

### 4D model-structure support matrix

| 4DGS model / training pipeline | Runtime representation | Supported |
|---|---|---|
| **Spacetime Gaussians (STG)** | single PLY: `t` / `scale_t` + polynomial motion | Yes — temporal marginalization + first-order velocity extrapolation. Higher-order `motion_3..8` terms are not evaluated (declared limitation, import-time warning) |
| **4DGaussians / Deformable-3DGS / Ex4DGS** (MLP / HexPlane deformation) | per-frame baked PLY (`export_perframe`) | Yes — via *Import PLY Sequence (Keyframe 4D)*, with GPU interpolation between frames (slerp for rotation) |
| **Fudan 4DGS** (native 4D primitives, ICLR 2024) | single PLY or `.pth`: dual-quaternion 4D rotation + 4D spherindrical harmonics | Yes — native 4D pipeline: 4D covariance temporal conditioning + 4D SH color evaluation |
| **Postshot 4D**, **Volinga `.nvol`** | proprietary | No — per-frame vertex counts/order do not match, and the formats are closed |
| Other native 4D variants (e.g. non-dual-quaternion 4D rotations) | — | No — the native 4D pipeline implements the dual-quaternion variant only |

---

## Requirements

- **Unreal Engine 5.5 – 6.0** (verified on 5.7, 5.8, 6.0).
- **DirectX 12 / Shader Model 6 is required.** DX11 is not supported: the cluster-culling compute
  shader uses 9 UAVs, above the SM5 (feature level 11.0) limit of 8, and the editor will fail while
  compiling `ClusterCulling.usf` (`Shader is using too many UAVs: 9 (only 8 supported)`).
  Set *Project Settings → Platforms → Windows → Default RHI* to **DirectX 12**.
- **English-only (ASCII) paths are mandatory.** Project path, plugin path, working directory,
  COLMAP path, Python path, and image directories must not contain Chinese characters, kana, or
  other non-ASCII symbols. Non-ASCII paths cause asset-registry crashes (`String is too long`),
  COLMAP command-line encoding problems, and hard-to-diagnose "file not found" errors.
  - Correct: `C:/Projects/MyProject/Plugins/OpenSplat4D`, `D:/Colmap/colmap.exe`
  - Wrong: `C:/项目/我的工程/插件/OpenSplat4D`, `D:/工具/colmap.exe`
- The plugin declares the **Niagara** plugin as a dependency, but you do **not** need to author a
  Niagara System — splatting is done by the plugin's own GPU pipeline.
- Building from source additionally requires **Visual Studio 2022/2026** with the C++ game
  development workload (toolset v143).

---

## Installation

### Option A — Prebuilt release (recommended)

1. Download the archive matching your engine version from
   [Releases](https://github.com/YuanBaoSMadLab/OpenSplat4D/releases):
   `OpenSplat4D-0.2-UE5.7.zip`, `OpenSplat4D-0.2-UE5.8.zip`, or `OpenSplat4D-0.2-UE6.0.zip`.
2. Extract it into your project so the layout is
   `<YourProject>/Plugins/OpenSplat4D/OpenSplat4D.uplugin`.
3. Restart the editor, then enable **OpenSplat4D** in *Edit → Plugins → Rendering*.

### Option B — Build from source

```powershell
& "C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat" BuildPlugin ^
  -Plugin="<Path>\OpenSplat4D\OpenSplat4D.uplugin" ^
  -Package="<OutputPath>" -CreateSubFolder -TargetPlatforms=Win64
```

Alternatively, copy the `OpenSplat4D` folder into `Plugins/`, then right-click the `.uproject` →
*Generate Visual Studio project files* and build.

---

## Quick start

1. **Import** — drag a `.ply` (or `.pth`) file into the Content Browser. An *OpenSplat4D* Gaussian
   Splat asset is created. 4D files are detected automatically.
2. **Place** — drag the asset into the level viewport. A Gaussian Splat actor is spawned with the
   component already assigned.
3. **Play** — for 4D assets, double-click the asset to open the **4D player editor** and press play;
   or select the actor in the level and press *Play* (PIE) to see the animation.

---

## 4D playback

Every component exposes 4D controls (Details panel, Blueprint, or the 4D player editor):

| Property / Function | Description |
|---|---|
| `Auto Play` / `bAutoPlay` | Start playback automatically when a 4D asset is registered |
| `Loop` / `bLooping` | Loop the time axis |
| `Play Rate` | Playback speed multiplier |
| `Current Time` | Read-only current time on the time axis |
| `Play` / `Pause` / `Stop` | `Play4D()` / `Pause4D()` / `Stop4D()` (BlueprintCallable) |
| `Set Playback Time` | `SetPlaybackTime(float)` — scrub / seek |
| `Supports 4D Playback` | `Supports4DPlayback()` — whether the assigned asset has temporal data |

Time-axis semantics depend on the model type: for temporal-marginalization models (STG) time is in
the training time units; for keyframe models time is a frame number (`0 … N-1`).

### 4D player editor

4D assets open in a **dedicated player-style editor** (3D assets keep the standard editor):

- Left: 3D/4D preview viewport; right: Details panel; bottom: transport bar.
- Transport bar: step-back / play / pause / stop / step-forward, timeline scrub (drag to seek),
  time code readout, loop toggle, and speed selector (×0.25 – ×4).
- Preview settings (auto-play, preview play rate, preview Nanite precision, preview view distance,
  preview fade-out distance, enable Nanite) apply live without rebuilding the preview actor.

Routing is automatic: the asset editor opens the 4D player editor when `Is4D()` is true, otherwise
the standard 3D asset editor.

---

## Performance tuning for large scenes

Large models (tens of millions of splats) are fill-rate and sort bound. The component exposes a
**Gaussian Splat | Performance** category, mirrored in the asset editor's preview settings and in
Blueprint:

| Setting | Effect |
|---|---|
| Nanite precision (LOD error threshold) | Cluster-LOD aggressiveness; lower value = fewer splats drawn |
| Max view distance | Distance culling; whole clusters outside the distance are skipped before compaction |
| Fade-out start distance | Soft fade of alpha between the fade distance and the max distance |
| Sort interval (frames) | Depth sorting runs only every N frames while the camera moves — largest single win |
| Frustum culling | Toggle view-frustum cluster culling |
| SH order | Spherical-harmonics order used for shading |
| Opacity scale / Splat size scale / Sharpness | Visual tuning |

BlueprintCallable: `SetNanitePrecision`, `SetVisibilityRange`, `SetFadeOutStart`, `SetSortInterval`.

Measured case: a 57-million-splat scene on an RTX 4090 went from ~60 FPS to roughly +60% after the
sort interval was wired into the pipeline (a previously inert property). Recommended starting point
for such scenes: sort interval 3, Nanite precision 0.1–0.3, max view distance ~200 m, fade-out 0.8×
max distance — and make sure Nanite cluster LODs were actually built for the asset.

---

## Training pipeline (capture → COLMAP → train) and HLOD

The plugin ships an editor mode (`EM_OpenSplat4D`, Modes panel → **OpenSplat4D**) with tabs for the
whole authoring workflow:

| Tab | Purpose |
|---|---|
| Capture | Place a capture rig over selected geometry, or scan geometry into a point cloud |
| Sparse | Run COLMAP sparse reconstruction, view and edit the result |
| Gaussian | Train the model; the **Train 4D** checkbox selects the 4DGS repo (dynamic) instead of 3DGS (static) |
| Settings | Paths for Python, COLMAP, and the training repositories (`UOpenSplat4DSettings`) |

Training is orchestrated by `Scripts/openplat4d_helper.py` (with a `--4d` branch) and
`Scripts/train_enhanced.py`; mask clipping uses `Scripts/clip_model.py`.

**World Partition HLOD.** `UOpenSplat4DHLODBuilder` runs capture → sparse → train on a cluster of
source components and replaces them with a single Gaussian Splat component. Its `bTrain4D` switch
produces either static 3DGS HLODs or dynamic 4DGS HLODs, so cells can mix both. Passing `-UseCache`
reuses an already-trained PLY instead of re-running the pipeline.

---

## Architecture

| Module | Type | Responsibility |
|---|---|---|
| `NanoGS` | Runtime | The active rendering pipeline: `UGaussianSplatAsset` (v8, 3D + three 4D modes), `UGaussianSplatComponent` (UPrimitiveComponent), `AGaussianSplatActor`, cluster builder, GPU renderer, scene proxy, global accumulator, NaNite-style LOD, view extension |
| `NanoGSEditor` | Editor | Asset factories (PLY / `.pth` / PLY-sequence), asset type actions, 3D asset editor, 4D player editor, transport bar (Slate), thumbnail renderer |
| `OpenSplat4DRuntime` | Runtime | Reference-derived pipeline: `UOpenSplat4DPointCloud` + `AOpenSplat4DSplatActor` (instanced static mesh rendering), SPZ compression, capture set, training dataset |
| `OpenSplat4DEditor` | Editor | Editor mode and panel, actor/asset factories, HLOD builder, settings, commandlet, reimport, Python step integration |

### Rendering pipeline (NanoGS)

```
GaussianSplatAsset (GPU buffers)
  -> SceneProxy -> RenderData / GPUResources
     -> ClusterCulling.usf        (cluster frustum + distance culling, indirect args)
     -> CompactSplats.usf         (Nanite LOD compaction)
     -> CalcDistances.usf + RadixSort.usf   (depth sort, gated by sort interval)
     -> CalcViewData.usf          (per-splat view data; time evaluation happens here)
     -> GaussianSplatRendering.usf / Composite (splat rasterization + resolve)
```

Time evaluation is a branch inside `CalcViewData.usf`, so all modes share one pipeline:

| Mode | GPU evaluation |
|---|---|
| Temporal marginalization | `alpha *= exp(-0.5 * (t - T)^2 / sigma_t^2)`, with `sigma_t = exp(scale_t)`; `pos(t) = pos + v * (t - anchor)` when velocity is present |
| Keyframe 4D | Interpolate 64 B/frame/splat records (position, quaternion, scale, opacity, color) between the two neighbouring frames; slerp for rotation |
| Native 4D (Fudan) | Build the 4D covariance from the dual quaternion, apply Schur temporal conditioning to obtain the equivalent 3D Gaussian (`mu_x' = mu_x + Sigma_xt * dt / sigma_t^2`, `Sigma_xx' = Sigma_xx - Sigma_xt Sigma_xt^T / sigma_t^2`), then evaluate 4D spherindrical harmonics for color |

Assets are serialized with a versioned format (currently **v8**); v5 – v7 assets load unchanged.
The `.o4d` container (magic `O4D2`) bundles splat data, optional keyframes, and the timeline.

---

## Repository layout

```
OpenSplat4D/
├── OpenSplat4D.uplugin
├── Source/
│   ├── NanoGS/                 # active renderer: asset, component, cluster LOD, GPU pipeline
│   ├── NanoGSEditor/           # importers, asset editors (3D + 4D), transport bar
│   ├── OpenSplat4DRuntime/     # point-cloud model, splat actor, SPZ, capture set
│   └── OpenSplat4DEditor/      # editor mode, HLOD builder, settings, commandlet
├── Shaders/Private/            # HLSL: CalcViewData, CalcDistances, RadixSort, ClusterCulling, ...
├── Scripts/                    # Python: COLMAP/training helpers, export_4dgs.py, clip_model.py
├── Extensions/                 # prebuilt CUDA extensions for the training side
├── Content/                    # Niagara templates (legacy path), docs
├── Docs/                       # additional documentation
├── LICENSE                     # Apache-2.0
├── README.md                   # this file
└── README-zh.md                # Chinese version
```

---

## Limitations and known issues

- **DirectX 12 / SM6 only.** DX11 cannot run the pipeline (UAV limit).
- **ASCII-only paths.** Non-ASCII paths break asset registry and COLMAP invocation.
- **4D model coverage** — see the support matrix above. Postshot 4D and Volinga `.nvol` are not
  supported. Spacetime Gaussians higher-order motion terms (`motion_3..8`) are not evaluated
  (first-order linear approximation, declared with an import-time warning).
- **`.pth` import** supports the modern `torch.save` zip format (PyTorch ≥ 1.6) with float32
  tensors. Legacy pickle checkpoints (PyTorch < 1.6) are rejected with an explicit error.
- **Per-splat temporal budget** is 16 B for temporal-marginalization models and 80 B for native 4D
  models; this bounds which attributes can be evaluated at runtime.
- Native 4D and keyframe pipelines have been validated with synthetic data and cross-checked against
  the reference CUDA math, but not yet against a full real-world trained model produced by every
  listed trainer.

---

## FAQ / troubleshooting

**The editor crashes while compiling `ClusterCulling.usf`.**
DX11 is active. Switch *Default RHI* to DirectX 12 (requires SM6 hardware).

**The scene renders black or nothing appears.**
Check that the GPU supports SM6 / DX12, that the asset's Nanite clusters exist, and that the actor's
component has the asset assigned. For very large models, raise the max view distance.

**Frame rate is low with tens of millions of splats.**
See *Performance tuning* above — start with the sort interval, then Nanite precision and max view
distance. Verify with `stat gpu` whether the bottleneck is splat draw (fill rate) or sorting.

**Importing a 2–3 GB PLY crashes or reports `stride is 0`.**
Large-file support includes whitespace-agnostic header parsing, type-aware property reads, and a
pre-import memory guard. Ensure you are on version 0.2; a 57 M-vertex file needs roughly 14 GB of
available physical memory.

**A 4D asset opened a normal editor / the transport bar is greyed out.**
The asset has no temporal data (`Is4D()` is false). Confirm the source PLY actually contains the 4D
attributes (`t`/`scale_t`, or `rot_0..7`/`scale_3`), or import per-frame PLYs through
*Import PLY Sequence (Keyframe 4D)*.

---

## Credits and license

OpenSplat4D's own source code is released under the **Apache-2.0** license (see [LICENSE](LICENSE)).
The project integrates third-party components whose terms differ; the full inventory is in
[THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md).

| Role | Source | License |
|---|---|---|
| 3DGS Unreal Engine rendering base | [Italink/GaussianSplattingForUnrealEngine](https://github.com/Italink/GaussianSplattingForUnrealEngine) | MIT |
| UE-side cluster LOD / compute pipeline reference | [TimChen1383/NanoGaussianSplatting](https://github.com/TimChen1383/NanoGaussianSplatting) | MIT |
| Native 4D Gaussians with spherindrical harmonics (ICLR 2024) | [fudan-zvg/4d-gaussian-splatting](https://github.com/fudan-zvg/4d-gaussian-splatting) | MIT (top level) |
| 4DGS training repository | [ueoo/4d-gaussian-splatting](https://github.com/ueoo/4d-gaussian-splatting) | MIT (top level) |
| Compressed splat format (SPZ) | Niantic Labs | MIT |
| Training-side SSIM operator | [rahul-goel/fused-ssim](https://github.com/rahul-goel/fused-ssim) | MIT |
| COLMAP model I/O (`Scripts/read_write_model.py`) | [colmap/colmap](https://github.com/colmap/colmap) | BSD-3-Clause |

### Non-commercial components — read before commercial use

Two parts of this repository come from the **Inria / MPII Gaussian-Splatting** code, which is
licensed for **non-commercial research and evaluation use only**:

- `Extensions/diff_gaussian_rasterization/` and `Extensions/simple_knn/` — prebuilt training-side
  CUDA extensions that carry the original Inria copyright headers
  ([diff-gaussian-rasterization](https://github.com/graphdeco-inria/diff-gaussian-rasterization),
  [simple-knn](https://gitlab.inria.fr/bkerbl/simple-knn)).
- `Shaders/Private/CalcViewData.usf` — the native-4D evaluation path is a port of
  `computeCov3D_conditional` / `computeColorFromSH_4D` from that codebase's `forward.cu`.

The Inria license states: *"THE USER CANNOT USE, EXPLOIT OR DISTRIBUTE THE SOFTWARE FOR COMMERCIAL
PURPOSES WITHOUT PRIOR AND EXPLICIT CONSENT OF LICENSORS."*

**The Apache-2.0 license therefore covers OpenSplat4D's own code only.** Commercial use of the
plugin as currently shipped requires either a commercial license from Inria
(stip-sophia.transfert@inria.fr) or replacing the two components above with independent
implementations — the native 4D math is fully specified in the published papers and can be
reimplemented without transposing Inria source. See
[THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md) for the complete statement.

`teachers/` and `ThirdParty/` are local development references only; they are not part of this
repository or of the released packages. No GPL, LGPL or AGPL code has been identified.

---

## Roadmap

- Broader native 4D variants beyond the dual-quaternion formulation.
- Higher-order Spacetime Gaussians motion evaluation.
- Validation against real trained models from each supported trainer.
- Point-cloud asset editor (size histogram, selection, deletion, undo).

## Links

- GitHub: https://github.com/YuanBaoSMadLab/OpenSplat4D
- Releases: https://github.com/YuanBaoSMadLab/OpenSplat4D/releases
- Issues: https://github.com/YuanBaoSMadLab/OpenSplat4D/issues
- Gitee mirror (for users in mainland China): https://gitee.com/YuanBaoSMadLab/OpenSplat4D
- Chinese README: [README-zh.md](README-zh.md)