# OpenSplat4D

一个面向 **Unreal Engine 5.5 ~ 6.0**（已在 UE 5.8 上构建验证）的 **4D 高斯泼溅（4DGS）+ 3DGS 双模式渲染** 插件。

OpenSplat4D 渲染由 4DGS 流程训练得到的时间变化高斯点云，并与静态 3DGS 模式作为
**可切换双模式**共存；渲染由自包含的 Billboard 组件完成，**无需手动创建 Niagara System 资产**。

---

## 许可与致谢

本插件融合两个开源参考项目构建，以 **Apache-2.0** 许可证发布。

| 角色 | 来源 |
|------|------|
| ① 3DGS UE 渲染基础（可塑 C++ 插件） | `GaussianSplattingForUnrealEngine`（by Italink） |
| ② 4DGS 时序模型（训练 / 推理） | `4d-gaussian-splatting`（Wu et al.，基于 INRIA GraphDeco `gaussian-splatting`） |

CUDA 算子（`pointops2`、`simple-knn`、`diff-gaussian-rasterization`）属于②的**训练侧**，
**未移植**——UE 端只需要前向*推理*（时间边缘化），已用 C++/HLSL 实现。

---

> # ⛔ 关键警告 —— 使用前务必阅读
>
> > **请让对应的工程以及所需文件目录中全是英文，不包含任何中文或者其他语言。**
>
> ⚠️ **关键前提：路径与文件名必须全英文！**
>
> **工程路径、插件目录、工作目录（WorkDir）、COLMAP 可执行文件路径、Python 路径、图片目录等任何涉及插件读写的路径，都不得包含中文字符、日文假名、特殊符号或其他非 ASCII 字符。** 否则可能出现：资产注册表崩溃（`String is too long`）、COLMAP 命令行参数乱码（GBK 编码）、文件找不到、Python 脚本参数解析错误等一系列难以排查的问题。
>
> ✅ 正确：`C:/Projects/MyProject/Plugins/OpenSplat4D`、`D:/Colmap/colmap.exe`
> ❌ 错误：`C:/项目/我的工程/插件/OpenSplat4D`、`D:/工具/colmap.exe`、`E:/OpenSplat4D_副本`

## 环境要求

- **Unreal Engine 5.5+**（已在 5.8 验证）。
- **必须启用 DirectX 12（DX11/SM5 不支持）**：NanoGS 渲染管线的簇剔除等 compute shader
  使用的 UAV 数量（9 个）超过 SM5 特性级 11.0 的 8 个上限。若在 DX11 下启用插件，编辑器会在
  编译 `ClusterCulling.usf` 时直接崩溃（`Shader is using too many UAVs: 9 (only 8 supported)`）。
  请在 *Project Settings → Platforms → Windows → Default RHI* 中选择 **DirectX 12**，
  并确保显卡 / 驱动支持 DirectX 12（Shader Model 6）。
- 插件依赖 **Niagara** 插件，但**不需要**你手工制作 Niagara System——渲染由
  `UOpenSplat4DBillboardComponent` 完成。

---

## 安装与启用

1. 把 `OpenSplat4D` 文件夹复制（或软链）到工程的 `Plugins/` 目录下。
2. 重启编辑器，或右键 `.uproject` → *Generate Visual Studio project files*（需
   **Visual Studio 2022/2026** 的 C++ 游戏开发负载）后编译。
3. 在 *Edit → Plugins → Rendering* 中搜索并启用 **OpenSplat4D**。

> 若用 `RunUAT BuildPlugin` 构建，把产出的 `HostProject/Plugins/OpenSplat4D` 部署到工程
> 的 `Plugins/` 目录即可。

---

## 快速开始（3 步）

1. **导入** —— 把 `.ply`（3DGS）或 `.4dgs`（OpenSplat4D 原生）文件拖入 *Content Browser*，
   会在 `/Game/OpenSplat4D/` 下生成一个 `OpenSplat4D Point Cloud` 资产。
2. **放入场景** —— 把该资产从 Content Browser 拖进视口，会自动生成
   `OpenSplat4DPointCloudActor` 并把点云绑上去。
3. **播放（4D）** —— 选中该 Actor，打开 **OpenSplat4D** 编辑器模式（Modes 面板里），
   点 **Use Selected Actor** 再点 **Play**。时间轴在 *Play / Simulate*（PIE）下连续动画；
   在编辑器内拖动 **Time** 滑块即可实时刷新。

---

## 详细使用

### 导入点云

- **拖拽导入**：把 `.ply` 或 `.4dgs` 文件拖入 Content Browser（资产工厂
  `UOpenSplat4DPointCloudAssetFactory` 已注册）。
- 或打开 **OpenSplat4D** 编辑器模式面板 → **Import .ply / .4dgs ...**（文件对话框）。
- **重导入**：右键资产 → *Reimport*（由 `SourceFilePath` 驱动）。
- 代码方式：
  ```cpp
  UOpenSplat4DPointCloud* Cloud = NewObject<UOpenSplat4DPointCloud>(Parent, ...);
  Cloud->LoadFromFile(TEXT("path/to/model.ply"));      // 3DGS
  // 或
  Cloud->LoadFrom4DGS(TEXT("path/to/model.4dgs"));     // 原生（3D 或 4D）
  ```

### 放入场景

以下任一种都会生成 `AOpenSplat4DPointCloudActor` 并绑定点云：

- 把资产从 Content Browser 拖入视口（由 `UActorFactory_OpenSplat4DPointCloud` 处理）。
- 在 Content Browser 中右键资产 → **Create OpenSplat4D Actor**。
- **OpenSplat4D** 编辑器模式面板 → 选好点云 → **Spawn Actor in Level**。
- *Place Actor* 面板 → 搜索 `OpenSplat4DPointCloudActor` → 指定其 `PointCloud`。

### 编辑资产

选中 `OpenSplat4D Point Cloud` 资产（或 Actor 上的 `PointCloud` 属性）并设置：

- **Mode** —— `Static3D`（普通 3DGS）或 `Dynamic4D`（随时间变化的 4DGS）。
- **TimeStart / TimeEnd** —— 4D 时间轴（对应训练模型的 `time_duration`）。

### 点云资产编辑器

**双击** Content Browser 中的 `OpenSplat4D Point Cloud` 资产即可打开专用编辑器，包含四个标签页：

- **Viewport（预览视口）**：用自带的 Billboard 渲染器实时显示 splat（无需 Niagara）。
  在视口内按住 **Ctrl+Alt 拖框**（或 Box Select / Frustum Select 工具）可选中框内所有点；
  按 **Delete** 删除选中点。
- **Details（细节）**：编辑 `Mode` / `TimeStart` / `TimeEnd` / `CompressionMethod` 等属性。
- **Features（特征）**：底部**尺寸直方图**按高斯尺度分布展示；在直方图上**拖拽**可选中
  某一尺度区间对应的点（高亮为紫色），再按 **Delete** 删除。
- **Preview Scene Settings**：预览场景环境/光照设置。

所有删除操作都包裹在事务中，可用 **Ctrl+Z / Ctrl+Y** 撤销 / 重做。

### 4D 播放

针对 `AOpenSplat4DPointCloudActor`：

- **细节面板**：`bAutoPlay`、`bLooping`、`PlayRate`、`CurrentTime`。
- **OpenSplat4D 编辑器模式面板**（推荐）：点 **Use Selected Actor**，再 **Play / Pause**，
  拖动 **Time** 拖拽、设 **Speed**、勾选 **Loop**。
- `Dynamic4D` 模式下 GPU 会应用时间边缘化 `w(t) = exp(-0.5·(t−T)²/σ²)`；
  `Static3D` 模式下权重恒为 1。
- 连续动画需要 *Play / Simulate*（PIE）；在编辑器视口内用 **Time** 滑块拖动可实时刷新
  （渲染器每帧都读取 `Time`）。

### 渲染说明

渲染由 `UOpenSplat4DBillboardComponent`（一个 `UPrimitiveComponent`）完成：把点云上传到 GPU
结构化缓冲，通过全局 Shader `OpenSplat4DBillboard.usf` 为每个高斯画一个朝向相机的四边形，
且绘制发生在 base pass 的渲染目标内（因此在 D3D12/Vulkan 下能真正光栅化）。Splat 作为
深度只读、alpha 混合的半透明叠加层。

---

## 从训练好的 4DGS 模型导出

用 Python 桥把训练好的②检查点转成 `.4dgs`：

```bash
python Scripts/export_4dgs.py --checkpoint models/flame_7000.pth --output flame.4dgs
```

随后按上面方式导入 `flame.4dgs`。（该脚本需在提供模型定义的 `4d-gaussian-splatting`
环境中运行。）

---

## 控制台变量（LOD / 屏幕尺寸）

- `r.OpenSplat4D.ScreenSizeBias` —— LOD 裁剪的屏幕尺寸偏移（默认 `0`）。
- `r.OpenSplat4D.ScreenSizeScale` —— LOD 裁剪的屏幕尺寸缩放（默认 `1`）。
- `r.OpenSplat4D.MaxFeatureSize` —— 允许的最大屏幕尺寸（占屏高比例，`0` = 不限制）。
  投影尺寸超过该值的超大高斯会在 CPU 上传前被剔除（近处巨点 LOD 剔除）。

---

## 蓝图 / 编辑器函数库（`UOpenSplat4DEditorLibrary`）

编辑器内可用蓝图节点（与参考插件 `UGaussianSplattingEditorLibrary` 对齐，已去掉 Niagara 专属项）：

- `LoadSplatFile(FileName, Outer, AssetName)` —— 从 `.ply`(3DGS) 或 `.4dgs`(3D/4D) 载入为资产。
- `GetPointCount(Cloud)` / `GetFeatureCount(Cloud)` —— 高斯（特征）数量。
- `ImportPointClouds(World, SearchDir, SaveContentDir)` —— 递归导入目录下所有 `.ply`/`.4dgs`，
  存为资产（如 `/Game/OpenSplat4D`）并生成 Actor。
- `RepartitionPointClouds(World, PartitionBaseName, CellSize, SaveContentDir)` —— 把关卡内所有
  点云按 `CellSize` 世界单位栅格化分块，每格生成独立小云 + Actor（用于分块加载 / LOD）。
- `CreateStaticMeshFromPointCloud(Cloud, Outer, AssetName)` —— 烘焙静态网格：每个高斯一个定向
  quad，顶点色(RGBA) + 打包数据 UV（UV1=scale.xy，UV2=scale.z，UV3=quat.xy，UV4=quat.zw），
  供自定义材质在 Niagara 之外的管线里重建高斯（代理 / 碰撞 / 导出）。

---

## 捕获 → 稀疏重建 → 训练 管线（与参考插件对齐）

编辑器内提供三步工作流类（`UOpenSplat4DStep_*`，移植自 ① 的 `UGaussianSplattingStep_*`）：

- **`UOpenSplat4DStep_Capture`**（`CallInEditor` 按钮 `Capture`）—— 用 `SceneCapture2D` 从
  选中 Actor / 半球或球面布点 / 自定义相机阵列渲染多视角 RGB(+ 深度 / 遮罩) 图像到 `WorkDir/images`。
- **`UOpenSplat4DStep_SparseReconstruction`**（`ReconstructionSparse`）—— 调用外部 **colmap**
  做特征提取 / 匹配 / 稀疏重建，生成相机与稀疏点（参数：MaxNumFeatures / 各类 colmap 命令参数）。
- **`UOpenSplat4DStep_GaussianSplatting`**（`Train`）—— 调用外部 **python 训练脚本**（3DGS 或
  4DGS，`bTrain4D` 开关）训练出 `point_cloud.ply`；`Clip` 支持按遮罩裁剪，`LoadPly` 载入结果资产。

命令通过 `UOpenSplat4DStepBase::ExecuteCommand` 桥接到系统进程，外部工具路径在
**Project Settings → Plugins → OpenSplat4D**（`UOpenSplat4DSettings`）中配置：
`ColmapDir` / `PythonExecutable` / `PythonProjectDir`（训练 / 4D 仓库根）等。

### HLOD 构建器（`UOpenSplat4DHLODBuilder`）

已注册为 WorldPartition HLOD 构建器：在 HLOD 图层选用 **OpenSplat4D** 构建器后，一键对
源组件（静态网格 / 地形 / 其他）执行 **捕获 → 稀疏重建 → 训练**，生成
`UOpenSplat4DBillboardComponent`（含 `UOpenSplat4DPointCloud`），并写出
`point_cloud_meta.json`（位置 / 包围盒 / 源资产名）。支持 `-UseCache` 命令行跳过重训直接复用 ply。

---

## 从场景扫描生成点云（无需 colmap / python）

即使手上没有任何 `.ply` / `.4dgs`，也能**直接把关卡里的模型 / 场景变成可渲染的点云**来测试：

- 打开编辑器 **Modes → OpenSplat4D** 面板，最上方是 **"0. Create from Scene (Scan)"**：
  - 在视口选中一个或多个 Actor（通常是静态网格）。
  - **Camera Depth Scan** 勾选框：关闭 = **Mesh Surface**（直接按面积采样网格表面，最稳、即时）；勾选 = **Camera Depth**（自动半球相机阵列 + 深度反投影，适合任意几何体）。
  - `Density`（采样密度）、`Point Size`（高斯尺寸，cm）。
  - 点 **"Scan Selected → Point Cloud"**：自动在 `/Game/OpenSplat4D` 生成 `Scan_<Actor>` 资产并生成 Actor 选中，立刻可在视口看到高斯渲染。
- 也可在 `UOpenSplat4DStep_Capture` 的细节面板点 **`ScanToPointCloud`**（同一套逻辑）。
- 蓝图 / C++：`UOpenSplat4DEditorLibrary::CreatePointCloudFromActors(World, Actors, ScanMode, Density, PointScale, SaveContentDir)`。

> Mesh Surface 模式从静态网格 LOD0 顶点 / 法线 / 顶点色生成世界空间高斯（法线朝向，surfel 式），
> 不依赖任何外部工具；Camera Depth 模式用场景深度反投影，作为补充的相机扫描手段。

---

## “OpenSplat4D” 编辑器模式

打开 Modes 面板，选择 **OpenSplat4D**（注册为编辑器模式 `EM_OpenSplat4D`）。其面板提供
端到端工作流：

1. **Import** —— 选择 `.ply` / `.4dgs` 文件。
2. **Place in level** —— 选定点云并生成 Actor。
3. **4D Playback** —— 驱动选中 Actor 的时间 / 速度 / 循环。

---

## 项目结构

```
OpenSplat4D/
├── OpenSplat4D.uplugin
├── Source/
│   ├── OpenSplat4DRuntime/        # 数据模型 + Billboard 渲染器 + Actor + 4D 数学
│   │   ├── Public/
│   │   │   ├── OpenSplat4DPoint.h          # FOpenSplat4DPoint（4D 感知）
│   │   │   ├── OpenSplat4DPointCloud.h     # UOpenSplat4DPointCloud（资产）
│   │   │   ├── OpenSplat4DPointCloudActor.h
│   │   │   └── OpenSplat4DBillboardComponent.h
│   │   └── Private/  （实现 + zlib 压缩 + Shader/渲染器）
│   └── OpenSplat4DEditor/         # 资产工厂 + Actor 工厂 + 编辑器模式面板
├── Shaders/Private/OpenSplat4DBillboard.usf  # Billboard HLSL
├── Scripts/export_4dgs.py         # ② 检查点 -> .4dgs
├── LICENSE
└── README.md
```

---

## 状态与路线图

已完成：双模式数据模型、自包含 Billboard 渲染器、导入/导出、编辑器模式工作流面板、
拖拽生成 Actor 工厂、LOD 控制台变量、**点云资产编辑器**（双击打开：预览视口 +
尺寸直方图 + 框选/删除 + 撤销重做）。

规划 / 扩充中（移植自参考项目 `GaussianSplattingForUnrealEngine`）：

- **捕获 → 稀疏重建 → 训练** 流水线（复用参考项目的 `GaussianSplattingStep` +
  `EditorLibrary` + Python 辅助脚本），实现在编辑器内直接产出 3DGS/4DGS 资产。
