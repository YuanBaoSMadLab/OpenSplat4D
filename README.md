# OpenSplat4D

A **4D Gaussian Splatting (4DGS) + 3DGS dual-mode rendering** plugin for **Unreal Engine 5.5 ~ 6.0** (built & tested on UE 5.8).

OpenSplat4D renders time-varying Gaussian point clouds trained by 4DGS pipelines and
coexists with the static 3DGS mode as **switchable dual modes**, rendered by a
self-contained billboard component (no Niagara System asset required).

---

## Credits & License

This plugin is built by fusing two reference open-source projects and is released under
the **Apache-2.0** license.

| Role | Source |
|------|--------|
| ① 3DGS UE rendering base (moldable C++ plugin) | `GaussianSplattingForUnrealEngine` (by Italink) |
| ② 4DGS temporal model (training / inference) | `4d-gaussian-splatting` (Wu et al., based on INRIA GraphDeco `gaussian-splatting`) |

The CUDA operators (`pointops2`, `simple-knn`, `diff-gaussian-rasterization`) live in the
**training** side (②) and are **not** ported — UE only needs the forward *inference*
(time-marginal evaluation), implemented here in C++/HLSL.

---

## Requirements

- **Unreal Engine 5.5+** (tested on 5.8).
- The plugin uses the **Niagara** plugin (kept as a dependency) but does **not** require you
  to author a Niagara System — rendering is done by `UOpenSplat4DBillboardComponent`.

---

## Install & Enable

1. Copy (or symlink) the `OpenSplat4D` folder into your UE project's `Plugins/` directory.
2. Restart the editor / right-click the `.uproject` → *Generate Visual Studio project files*
   (requires **Visual Studio 2022/2026** with the C++ game-dev workload), then build.
3. Enable **OpenSplat4D** in *Edit → Plugins → Rendering* (search "OpenSplat4D").

> If you build the plugin with `RunUAT BuildPlugin`, deploy the produced
> `HostProject/Plugins/OpenSplat4D` into your project's `Plugins/` folder.

---

## Quick start (3 steps)

1. **Import** — drag a `.ply` (3DGS) or `.4dgs` (OpenSplat4D native) file into the
   *Content Browser*. An `OpenSplat4D Point Cloud` asset is created under `/Game/OpenSplat4D/`.
2. **Place** — drag that asset from the Content Browser into the level viewport. An
   `OpenSplat4DPointCloudActor` is spawned and the cloud is wired to it automatically.
3. **Play (4D)** — select the actor, open the **OpenSplat4D** editor mode (the Modes panel,
   or *Settings*), click **Use Selected Actor**, then **Play**. The time axis animates
   during *Play* / *Simulate* (PIE). In the editor you can scrub with the **Time** slider
   and the splats update live.

---

## Detailed usage

### Importing a point cloud

- **Drag & drop** a `.ply` or `.4dgs` file into the Content Browser (works because the
  asset factory `UOpenSplat4DPointCloudAssetFactory` is registered).
- Or use the **OpenSplat4D** editor mode panel → **Import .ply / .4dgs ...** (file dialog).
- Re-import: right-click the asset → *Reimport* (driven by `SourceFilePath`).
- Programmatically:
  ```cpp
  UOpenSplat4DPointCloud* Cloud = NewObject<UOpenSplat4DPointCloud>(Parent, ...);
  Cloud->LoadFromFile(TEXT("path/to/model.ply"));      // 3DGS
  // or
  Cloud->LoadFrom4DGS(TEXT("path/to/model.4dgs"));     // native (3D or 4D)
  ```

### Placing in the level

Any of these spawn an `AOpenSplat4DPointCloudActor` and assign the cloud:

- Drag the asset from the Content Browser into the viewport (handled by
  `UActorFactory_OpenSplat4DPointCloud`).
- Right-click the asset in the Content Browser → **Create OpenSplat4D Actor**.
- **OpenSplat4D** editor mode panel → pick the cloud → **Spawn Actor in Level**.
- *Place Actor* panel → search `OpenSplat4DPointCloudActor` → assign its `PointCloud`.

### Editing the asset

Select the `OpenSplat4D Point Cloud` asset (or its actor's `PointCloud` property) and set:

- **Mode** — `Static3D` (plain 3DGS) or `Dynamic4D` (time-varying 4DGS).
- **TimeStart / TimeEnd** — the 4D time axis (matches the trained model's `time_duration`).

### 4D playback

On the `AOpenSplat4DPointCloudActor`:

- **Details panel**: `bAutoPlay`, `bLooping`, `PlayRate`, `CurrentTime`.
- **OpenSplat4D editor mode panel** (recommended): click **Use Selected Actor**, then
  **Play** / **Pause**, drag **Time** to scrub, set **Speed**, toggle **Loop**.
- In **Dynamic4D** mode the GPU applies the temporal marginal
  `w(t) = exp(-0.5·(t−T)²/σ²)`; in **Static3D** mode weight is always 1.
- Continuous animation requires *Play* / *Simulate* (PIE). In the editor viewport the
  **Time** slider scrubs live (the renderer reads `Time` every frame).

### Rendering notes

Rendering is performed by `UOpenSplat4DBillboardComponent` (a `UPrimitiveComponent`):
it uploads the cloud to a GPU structured buffer and draws one camera-facing quad per
Gaussian through the global shader `OpenSplat4DBillboard.usf`, inside the base-pass render
target (so it is actually rasterized on D3D12/Vulkan). Splats are a depth-read, alpha-blended
translucent overlay.

---

## HLOD (World Partition) — one-click 3DGS / 4DGS

For large worlds (World Partition), OpenSplat4D ships an HLOD builder
(`UOpenSplat4DHLODBuilder`) that automatically runs the full
**capture → sparse reconstruction → train** pipeline on a cluster of source
components and replaces them with a single `UOpenSplat4DBillboardComponent`.

1. In the *World Partition* HLOD setup, choose the **OpenSplat4D** HLOD builder
   for a cell / cluster layer.
2. Its settings expose the same three step objects (Capture / Sparse /
   Gaussian) plus a **`bTrain4D`** dual-mode switch:
   - `bTrain4D = false` → a static **3DGS** HLOD (regular gaussian-splatting repo).
   - `bTrain4D = true`  → a dynamic **4DGS** HLOD (4d-gaussian-splatting repo).
3. Build HLODs (e.g. *Build → Build HLODs*). For each cluster the builder
   captures the source geometry, runs colmap + training (honoring `bTrain4D`),
   and emits a billboard component placed at the cluster origin, alongside a
   `point_cloud_meta.json` recording the bounds + source asset names.
4. Pass `-UseCache` on the command line to reuse an already-trained PLY instead
   of re-running the whole pipeline.

This is the same dual-mode switch used by the editor pipeline (see below), so
World Partition cells can mix static and dynamic gaussian HLODs.

### Dual-mode editor pipeline (Capture / Sparse / Gaussian)

The **OpenSplat4D** editor mode panel has four pipeline tabs in addition to the
Usage tab:

- **Capture** — place a capture rig over selected geometry (or scan it into a
  point cloud directly, see *Usage*).
- **Sparse** — run COLMAP sparse reconstruction / view / edit.
- **Gaussian** — train the model. The **`bTrain4D`** checkbox selects the
  **4DGS** repo (dynamic, time-enabled) vs the **3DGS** repo (static). Training
  is orchestrated by `Scripts/openplat4d_helper.py` (ported from the reference
  plugin, extended with a `--4d` branch); mask clipping uses `clip_model.py`.
- **Settings** — edit `UOpenSplat4DSettings` (python / colmap / repo paths).

---

## Exporting from a trained 4DGS model

Use the Python bridge to convert a trained ② checkpoint into `.4dgs`:

```bash
python Scripts/export_4dgs.py --checkpoint models/flame_7000.pth --output flame.4dgs
```

Then import `flame.4dgs` as above. (The script must run inside the `4d-gaussian-splatting`
environment that provides the model definition.)

---

## Console variables (LOD / screen size)

- `r.OpenSplat4D.ScreenSizeBias` — screen-size bias for LOD culling (default `0`).
- `r.OpenSplat4D.ScreenSizeScale` — screen-size scale for LOD culling (default `1`).

---

## The "OpenSplat4D" editor mode

Open the Modes panel and select **OpenSplat4D** (registered as editor mode
`EM_OpenSplat4D`). Its panel provides the end-to-end workflow:

1. **Import** — pick a `.ply` / `.4dgs` file.
2. **Place in level** — choose a cloud and spawn the actor.
3. **4D Playback** — drive the selected actor's time / speed / loop.

---

## Project layout

```
OpenSplat4D/
├── OpenSplat4D.uplugin
├── Source/
│   ├── OpenSplat4DRuntime/        # data model + billboard renderer + actor + 4D math
│   │   ├── Public/
│   │   │   ├── OpenSplat4DPoint.h          # FOpenSplat4DPoint (4D-aware)
│   │   │   ├── OpenSplat4DPointCloud.h     # UOpenSplat4DPointCloud (asset)
│   │   │   ├── OpenSplat4DPointCloudActor.h
│   │   │   └── OpenSplat4DBillboardComponent.h
│   │   └── Private/  (implementations + zlib compression + shader/renderer)
│   └── OpenSplat4DEditor/         # asset factory + actor factory + editor mode panel
├── Shaders/Private/OpenSplat4DBillboard.usf  # billboard HLSL
├── Scripts/export_4dgs.py         # ② checkpoint -> .4dgs
├── LICENSE
└── README.md
```

---

## Status & roadmap

Working: dual-mode data model, self-contained billboard renderer, importers/exporters,
editor-mode workflow panel, drag-to-scene actor factory, LOD console variables.

Planned / expanding (ported from the reference `GaussianSplattingForUnrealEngine`):
- A **point-cloud asset editor** (preview viewport + size histogram + select/delete + undo).
- The **capture → sparse reconstruction → training** pipeline (reusing the reference's
  `GaussianSplattingStep` + `EditorLibrary` + Python helpers) for producing 3DGS/4DGS
  assets without leaving the editor.
