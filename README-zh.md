# OpenSplat4D

**面向 Unreal Engine 的 3D / 4D 高斯泼溅（3DGS / 4DGS）实时渲染插件。**

OpenSplat4D 把高斯泼溅带入 Unreal Engine 5：以原生 C++ 插件的形式，实时渲染静态 3D 高斯泼溅模型
（`.ply`），以及随时间变化的 4D 高斯泼溅模型 —— 支持 Spacetime Gaussians、关键帧/序列 4DGS、
以及复旦原生 4DGS（双四元数 4D 旋转 + 4D 球柱谐）。渲染走 GPU compute 管线，具备 Nanite 式簇级 LOD、
间接绘制，并提供专属的 4D 播放器编辑器。

- 版本：**0.2**
- 引擎：**Unreal Engine 5.5 – 6.0**（已在 UE 5.7 / 5.8 / 6.0 上编译验证，Windows / Win64）
- 许可：**Apache-2.0**
- 下载：[GitHub Releases](https://github.com/YuanBaoSMadLab/OpenSplat4D/releases)（按引擎版本预编译）

---

## 这是什么项目

高斯泼溅（Gaussian Splatting）是一种辐射场表示方法，把照片重建为以百万级 3D 高斯图元组成的逼真场景。
它的 4D 扩展（4DGS / 动态高斯泼溅）进一步对**时间**建模，于是场景能"动起来"：人物行走、布料形变、火焰闪烁。

OpenSplat4D 是这条流水线的 Unreal Engine 侧实现，解决三个实际问题：

1. **渲染** —— UE 内自包含的高斯泼溅 GPU 渲染器，带 LOD、深度排序、剔除与着色，无需手工制作 Niagara System 资产。
2. **4D 播放** —— 点云的时间轴在 GPU 上求值，由"关卡序列式"的 4D 播放器编辑器驱动。
3. **生产流程** —— 导入、训练（采集 → COLMAP → 3DGS/4DGS 训练）、World Partition HLOD 生成，全部在编辑器内完成。

### 关键词

高斯泼溅、3DGS、4DGS、4D 高斯泼溅、动态高斯泼溅、Unreal Engine 5 插件、UE5 插件、实时渲染、
Nanite 簇级 LOD、compute shader 泼溅、PLY 导入、PyTorch checkpoint 导入、Spacetime Gaussians、
球柱谐函数、双四元数、World Partition HLOD、COLMAP、新视角合成、点云。

---

## 支持的输入格式

导入器**从文件本身自动判定模型类型**，无需手动选择模式。

| 格式 | 扩展名 | 判定为 | 说明 |
|---|---|---|---|
| 标准 3DGS PLY（INRIA / Postshot / 多数训练器） | `.ply` | 静态 3D | 位置、旋转、尺度、不透明度、SH |
| SPZ 压缩 3DGS（Niantic） | `.spz` | 静态 3D | 运行时点云路径 |
| Spacetime Gaussians（STG） | `.ply` | **4D —— 时间边缘化** | 需要 `t` + `scale_t`；含 `motion_0..2` 时启用速度外推 |
| 复旦 4DGS（原生 4D 图元） | `.ply` | **4D —— 原生 4D** | 需要 `rot_0..7`（双四元数）+ `scale_3` + `f_rest` |
| 复旦 4DGS 训练 checkpoint | `.pth` | **4D —— 原生 4D** | 直接解析 `torch.save` checkpoint，无需额外导出步骤 |
| 关键帧 / 逐帧烘焙 4DGS（4DGaussians、Deformable-3DGS、Ex4DGS） | `.ply` 序列 | **4D —— 关键帧** | 内容浏览器右键 → *导入 PLY 序列（关键帧 4D）* |
| OpenSplat4D 专属容器 | `.o4d` | 3D 或 4D | 打包 splat 数据 + 关键帧 + 时间轴；蓝图可保存/加载 |

### 4D 模型结构支持矩阵

| 4DGS 模型 / 训练流程 | 运行时形态 | 是否支持 |
|---|---|---|
| **Spacetime Gaussians（STG）** | 单 PLY：`t` / `scale_t` + 多项式运动 | 支持 —— 时间边缘化 + 一阶速度外推。高阶运动项 `motion_3..8` 不求值（已声明限制，导入时日志警告） |
| **4DGaussians / Deformable-3DGS / Ex4DGS**（MLP / HexPlane 形变） | 逐帧烘焙 PLY（`export_perframe`） | 支持 —— 走 *导入 PLY 序列（关键帧 4D）*，帧间由 GPU 插值（旋转用 slerp） |
| **复旦 4DGS**（原生 4D 图元，ICLR 2024） | 单 PLY 或 `.pth`：双四元数 4D 旋转 + 4D 球柱谐 | 支持 —— 原生 4D 管线：4D 协方差时间条件化 + 4D 球柱谐颜色求值 |
| **Postshot 4D**、**Volinga `.nvol`** | 封闭格式 | 不支持 —— 逐帧顶点数/顺序不一致，且格式封闭 |
| 其他原生 4D 变体（如非双四元数的 4D 旋转） | — | 不支持 —— 原生 4D 管线只实现双四元数这一变体 |

---

## 环境要求

- **Unreal Engine 5.5 – 6.0**（已在 5.7 / 5.8 / 6.0 验证）。
- **必须使用 DirectX 12 / Shader Model 6。** 不支持 DX11：簇剔除 compute shader 使用了 9 个 UAV，
  超过 SM5（特性级 11.0）的 8 个上限，在 DX11 下编辑器会在编译 `ClusterCulling.usf` 时报错
  （`Shader is using too many UAVs: 9 (only 8 supported)`）。
  请在 *项目设置 → 平台 → Windows → 默认 RHI* 中选择 **DirectX 12**。
- **路径必须全英文（ASCII）。** 工程路径、插件路径、工作目录、COLMAP 路径、Python 路径、图片目录
  都不得包含中文、日文假名或其他非 ASCII 字符。否则会出现资产注册表崩溃（`String is too long`）、
  COLMAP 命令行编码（GBK）乱码、"文件找不到"等难以排查的问题。
  - ✅ 正确：`C:/Projects/MyProject/Plugins/OpenSplat4D`、`D:/Colmap/colmap.exe`
  - ❌ 错误：`C:/项目/我的工程/插件/OpenSplat4D`、`D:/工具/colmap.exe`
- 插件声明依赖 **Niagara** 插件，但**不需要**你手工制作 Niagara System —— 泼溅渲染由插件自身的 GPU 管线完成。
- 若从源码构建，还需要 **Visual Studio 2022/2026**，并安装 C++ 游戏开发工作负载（工具集 v143）。

---

## 安装

### 方式 A —— 使用预编译包（推荐）

1. 到 [Releases](https://github.com/YuanBaoSMadLab/OpenSplat4D/releases) 下载与引擎版本对应的压缩包：
   `OpenSplat4D-0.2-UE5.7.zip`、`OpenSplat4D-0.2-UE5.8.zip` 或 `OpenSplat4D-0.2-UE6.0.zip`。
2. 解压到你的工程中，使目录结构为
   `<你的工程>/Plugins/OpenSplat4D/OpenSplat4D.uplugin`。
3. 重启编辑器，在 *编辑 → 插件 → 渲染* 中勾选启用 **OpenSplat4D**。

### 方式 B —— 从源码构建

```powershell
& "C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat" BuildPlugin ^
  -Plugin="<路径>\OpenSplat4D\OpenSplat4D.uplugin" ^
  -Package="<输出路径>" -CreateSubFolder -TargetPlatforms=Win64
```

也可以把 `OpenSplat4D` 目录复制到 `Plugins/` 下，右键 `.uproject` → *生成 Visual Studio 工程文件* 后编译。

---

## 快速上手

1. **导入** —— 把 `.ply`（或 `.pth`）文件拖入内容浏览器，生成 OpenSplat4D 高斯泼溅资产；4D 文件会自动识别。
2. **放置** —— 把资产拖入关卡视口，自动生成高斯泼溅 Actor，组件已自动关联该资产。
3. **播放** —— 4D 资产双击打开 **4D 播放器编辑器**并点播放；或在关卡中选中 Actor 后直接 *播放*（PIE）查看动画。

---

## 4D 播放

每个组件都暴露 4D 控制项（Details 面板、蓝图、或 4D 播放器编辑器）：

| 属性 / 函数 | 说明 |
|---|---|
| `自动播放`（bAutoPlay） | 注册 4D 资产时自动开始播放 |
| `循环播放`（bLooping） | 时间轴循环 |
| `播放速度`（PlayRate） | 播放倍速 |
| `当前时间`（CurrentTime） | 只读，当前时间轴位置 |
| `播放` / `暂停` / `停止` | `Play4D()` / `Pause4D()` / `Stop4D()`（蓝图可调用） |
| `设置播放时间` | `SetPlaybackTime(float)` —— 拖拽定位 / 跳转 |
| `支持 4D 播放` | `Supports4DPlayback()` —— 当前资产是否含时序数据 |

时间轴语义随模型类型不同：时间边缘化模型（STG）的时间是训练时间单位；关键帧模型的时间是**帧号**（`0 … N-1`）。

### 4D 播放器编辑器

4D 资产会打开**专属的播放器式编辑器**（3D 资产仍用原编辑器）：

- 左侧 3D/4D 预览视口，右侧 Details 面板，底部播放器条。
- 播放器条：上一帧 / 播放 / 暂停 / 停止 / 下一帧，时间轴拖拽定位，时间码显示，循环开关，倍速选择（×0.25 – ×4）。
- 预览设置（预览自动播放、预览播放速度、预览 Nanite 精度、预览最大可视距离、预览淡出起始距离、启用 Nanite）
  实时生效，不会重建预览 Actor。

分流是自动的：`Is4D()` 为真时资产编辑器打开 4D 播放器编辑器，否则打开标准 3D 资产编辑器。

---

## 大场景性能调优

大模型（数千万 splat）主要受填充率与排序开销限制。组件提供 **高斯泼溅 | 性能** 分类，
并且在资产编辑器预览设置与蓝图中同步暴露：

| 设置 | 作用 |
|---|---|
| Nanite 精度（LOD 误差阈值） | 簇级 LOD 激进程度；值越小绘制的 splat 越少 |
| 最大可视距离 | 距离剔除；整簇超出距离的在压缩前就被跳过 |
| 淡出起始距离 | 在淡出距离与最大距离之间对 alpha 线性衰减 |
| 排序间隔（帧） | 相机移动时每 N 帧才做一次深度排序 —— 单项收益最大 |
| 视锥剔除 | 视锥体簇剔除开关 |
| SH 阶数 | 着色使用的球谐阶数 |
| 不透明度缩放 / Splat 尺寸缩放 / 高斯锐度 | 观感调节 |

蓝图可调用：`SetNanitePrecision`、`SetVisibilityRange`、`SetFadeOutStart`、`SetSortInterval`。

实测案例：5700 万 splat 场景在 RTX 4090 上，把"排序间隔"接通管线（此前该属性形同虚设）后帧率提升约 60%。
此类场景的推荐起点：排序间隔 3、Nanite 精度 0.1–0.3、最大可视距离约 200 m、淡出取最大距离的 0.8 倍
—— 并确认该资产的 Nanite 簇层级确实已构建。

---

## 训练流程（采集 → COLMAP → 训练）与 HLOD

插件提供编辑器模式（`EM_OpenSplat4D`，Modes 面板 → **OpenSplat4D**），覆盖完整制作流程：

| 页签 | 用途 |
|---|---|
| Capture（采集） | 在选中几何体上布置采集相机阵列，或直接把几何体扫描为点云 |
| Sparse（稀疏） | 运行 COLMAP 稀疏重建，并可查看与编辑结果 |
| Gaussian（高斯） | 训练模型；**Train 4D** 勾选框选择 4DGS 仓库（动态）而非 3DGS 仓库（静态） |
| Settings（设置） | 配置 Python、COLMAP 与训练仓库路径（`UOpenSplat4DSettings`） |

训练由 `Scripts/openplat4d_helper.py`（含 `--4d` 分支）与 `Scripts/train_enhanced.py` 编排，
遮罩裁剪使用 `Scripts/clip_model.py`。

**World Partition HLOD。** `UOpenSplat4DHLODBuilder` 对一组源组件执行 采集 → 稀疏 → 训练，
并用单个高斯泼溅组件替换它们。其 `bTrain4D` 开关可生成静态 3DGS HLOD 或动态 4DGS HLOD，
因此不同格子可以混用两种模式。命令行加 `-UseCache` 可复用已训练好的 PLY，跳过整条流水线。

---

## 架构

| 模块 | 类型 | 职责 |
|---|---|---|
| `NanoGS` | Runtime | 当前使用的渲染管线：`UGaussianSplatAsset`（v8，3D + 三种 4D 模式）、`UGaussianSplatComponent`（UPrimitiveComponent）、`AGaussianSplatActor`、簇构建器、GPU 渲染器、场景代理、全局累加器、Nanite 式 LOD、视图扩展 |
| `NanoGSEditor` | Editor | 资产工厂（PLY / `.pth` / PLY 序列）、资产类型操作、3D 资产编辑器、4D 播放器编辑器、播放器条（Slate）、缩略图渲染器 |
| `OpenSplat4DRuntime` | Runtime | 参考实现派生的管线：`UOpenSplat4DPointCloud` + `AOpenSplat4DSplatActor`（实例化静态网格渲染）、SPZ 压缩、采集集、训练数据集 |
| `OpenSplat4DEditor` | Editor | 编辑器模式与面板、Actor/资产工厂、HLOD 构建器、设置、Commandlet、重新导入、Python 步骤集成 |

### 渲染管线（NanoGS）

```
GaussianSplatAsset（GPU 缓冲）
  -> SceneProxy -> RenderData / GPUResources
     -> ClusterCulling.usf        （簇级视锥 + 距离剔除，间接绘制参数）
     -> CompactSplats.usf         （Nanite LOD 压缩）
     -> CalcDistances.usf + RadixSort.usf   （深度排序，由排序间隔门控）
     -> CalcViewData.usf          （逐 splat 视图数据；时间求值在此处）
     -> GaussianSplatRendering.usf / Composite（splat 光栅化与合成）
```

时间求值是 `CalcViewData.usf` 内的一个分支，因此所有模式共用同一条管线：

| 模式 | GPU 求值 |
|---|---|
| 时间边缘化 | `alpha *= exp(-0.5 * (t - T)^2 / sigma_t^2)`，其中 `sigma_t = exp(scale_t)`；含速度时 `pos(t) = pos + v * (t - anchor)` |
| 关键帧 4D | 在相邻两帧之间插值 64 B/帧/splat 记录（位置、四元数、尺度、不透明度、颜色）；旋转用 slerp |
| 原生 4D（复旦） | 由双四元数构建 4D 协方差，做 Schur 时间条件化得到等效 3D 高斯（`mu_x' = mu_x + Sigma_xt * dt / sigma_t^2`，`Sigma_xx' = Sigma_xx - Sigma_xt Sigma_xt^T / sigma_t^2`），再用 4D 球柱谐求颜色 |

资产使用带版本的序列化格式（当前 **v8**），v5 – v7 资产可原样加载。
`.o4d` 容器（magic `O4D2`）打包 splat 数据、可选关键帧块与时间轴。

---

## 仓库结构

```
OpenSplat4D/
├── OpenSplat4D.uplugin
├── Source/
│   ├── NanoGS/                 # 当前渲染器：资产、组件、簇级 LOD、GPU 管线
│   ├── NanoGSEditor/           # 导入器、资产编辑器（3D + 4D）、播放器条
│   ├── OpenSplat4DRuntime/     # 点云模型、泼溅 Actor、SPZ、采集集
│   └── OpenSplat4DEditor/      # 编辑器模式、HLOD 构建器、设置、Commandlet
├── Shaders/Private/            # HLSL：CalcViewData、CalcDistances、RadixSort、ClusterCulling 等
├── Scripts/                    # Python：COLMAP/训练辅助、export_4dgs.py、clip_model.py
├── Extensions/                 # 训练侧预编译 CUDA 扩展
├── Content/                    # Niagara 模板（历史路径）与文档
├── Docs/                       # 补充文档
├── LICENSE                     # Apache-2.0
├── README.md                   # 英文版
└── README-zh.md                # 本文件
```

---

## 限制与已知问题

- **仅支持 DirectX 12 / SM6。** DX11 无法运行该管线（UAV 数量限制）。
- **路径必须全 ASCII。** 非 ASCII 路径会破坏资产注册表与 COLMAP 调用。
- **4D 模型覆盖范围** —— 见上方支持矩阵。Postshot 4D 与 Volinga `.nvol` 不支持；
  Spacetime Gaussians 的高阶运动项（`motion_3..8`）不求值，仅做一阶线性近似（导入时日志警告声明）。
- **`.pth` 导入**支持新版 `torch.save` zip 格式（PyTorch ≥ 1.6）且张量为 float32；
  旧版 pickle 格式（PyTorch < 1.6）会给出明确报错。
- **每 splat 时序预算**：时间边缘化模型 16 B，原生 4D 模型 80 B —— 这决定了运行时能求值哪些属性。
- 原生 4D 与关键帧管线已用合成数据验证并与参考 CUDA 数学对拍，但尚未针对上述每个训练器产出的
  真实完整模型逐一验证。

---

## 常见问题与排查

**编译器在编译 `ClusterCulling.usf` 时报错/崩溃。**
当前是 DX11。把 *默认 RHI* 切到 DirectX 12（需要支持 SM6 的显卡）。

**场景全黑或什么都看不到。**
确认显卡支持 SM6 / DX12、资产已构建 Nanite 簇层级、且 Actor 组件已关联资产。超大模型请提高最大可视距离。

**数千万 splat 时帧率很低。**
见上方"大场景性能调优"：先调排序间隔，再调 Nanite 精度与最大可视距离。
用 `stat gpu` 判断瓶颈在 splat 绘制（填充率）还是排序。

**导入 2–3 GB PLY 崩溃，或报 `stride is 0`。**
大文件支持已包含表头空白无关解析、类型感知属性读取与导入前内存守卫。请确认版本为 0.2；
5700 万顶点的文件约需 14 GB 可用物理内存。

**4D 资产打开的是普通编辑器 / 播放器条灰显。**
该资产没有时序数据（`Is4D()` 为假）。确认源 PLY 确实带 4D 属性（`t`/`scale_t`，或 `rot_0..7`/`scale_3`），
或用 *导入 PLY 序列（关键帧 4D）* 导入逐帧 PLY。

---

## 致谢与许可

以 **Apache-2.0** 许可证发布。本项目建立在以下开源工作之上：

| 角色 | 来源 |
|---|---|
| 3DGS 的 Unreal Engine 渲染基础 | `GaussianSplattingForUnrealEngine`（by Italink） |
| UE 侧簇级 LOD / compute 管线参考 | `NanoGaussianSplatting` |
| 4DGS 时序模型（训练 / 推理） | `4d-gaussian-splatting`（Wu et al.，INRIA GraphDeco 谱系） |
| 原生 4D 高斯与球柱谐 | 复旦大学 `4d-gaussian-splatting`（Yang et al., ICLR 2024） |
| 压缩 splat 格式（SPZ） | Niantic SPZ |

训练所用 CUDA 算子（`pointops2`、`simple-knn`、`diff-gaussian-rasterization`）位于**训练侧**，未做移植
—— Unreal Engine 侧只需要前向推理，已用 C++ 与 HLSL 实现。

---

## 路线图

- 支持双四元数之外的原生 4D 变体。
- Spacetime Gaussians 高阶运动项求值。
- 针对每个受支持训练器的真实模型做验证。
- 点云资产编辑器（尺寸直方图、选择、删除、撤销）。

## 链接

- GitHub 主仓库：https://github.com/YuanBaoSMadLab/OpenSplat4D
- 发布页：https://github.com/YuanBaoSMadLab/OpenSplat4D/releases
- 问题反馈：https://github.com/YuanBaoSMadLab/OpenSplat4D/issues
- Gitee 镜像（国内访问）：https://gitee.com/YuanBaoSMadLab/OpenSplat4D
- 英文版说明：[README.md](README.md)