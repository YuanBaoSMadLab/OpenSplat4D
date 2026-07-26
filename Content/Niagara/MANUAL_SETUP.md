# 🌟 OpenSplat4D Niagara 系统 — 安装说明

> **UE 5.8 用户：文件已内置，直接复制即可，无需手动创建！**
> 文件位置：`Content/Niagara/Templates/UE5.8/NS_OpenSplat4D.uasset`

---

## 安装方式（二选一）

### 方式一：直接复制（推荐，UE 5.8 用户）

`NS_OpenSplat4D.uasset` 已经预置在插件中。将 `OpenSplat4D/` 文件夹复制到你的 UE 项目的 `Plugins/` 目录下即可。插件启动时会自动加载该文件。

### 方式二：手动创建（用于其他 UE 版本）

以下指南适用于需要为其他 UE 版本（如 UE 5.5、5.6、5.7）手动创建 Niagara System 的情况。步骤与 UE 5.8 相同，只是最终保存到对应的版本文件夹中。

---

## 目录

1. [准备工作](#1-准备工作)
2. [打开 Niagara Editor](#2-打开-niagara-editor)
3. [创建 System 和 Emitter](#3-创建-system-和-emitter)
4. [配置 Emitter 为 GPU 模式](#4-配置-emitter-为-gpu-模式)
5. [删除不需要的默认模块](#5-删除不需要的默认模块)
6. [创建 Scratch Pad 模块——核心步骤](#6-创建-scratch-pad-模块核心步骤)
7. [添加 Sprite Renderer](#7-添加-sprite-renderer)
8. [暴露 User 参数](#8-暴露-user-参数)
9. [编译和保存](#9-编译和保存)
10. [测试渲染效果](#10-测试渲染效果)

---

## 1. 准备工作

### 1.1 确认插件已启用

1. 打开你的 UE 5.8 项目
2. 顶部菜单栏 → **Edit** → **Plugins**
3. 搜索 `OpenSplat4D`
4. 确保它被勾选（Enabled）
5. 如果刚启用，可能需要重启编辑器

### 1.2 确认文件夹存在

1. 在编辑器底部的 **Content Browser**（内容浏览器）中
2. 左侧文件夹树找到 `Content` → `OpenSplat4D` → `Niagara` → `Templates` → `UE5.8`
3. 如果 `UE5.8` 文件夹不存在：
   - 右键 `Templates` 文件夹 → **New Folder** → 命名为 `UE5.8`
4. 我们一会儿创建的系统就保存在这个 `UE5.8` 文件夹里

---

## 2. 打开 Niagara Editor

### 2.1 创建 Niagara System

1. 在 Content Browser 中，导航到 `Content/OpenSplat4D/Niagara/Templates/UE5.8/`
2. 在右侧空白区域**右键** → 选择 **FX** → **Niagara System**
   - 会弹出一个窗口问你 "Choose a system template"
3. 选择 **"Empty"**（空白模板，最上面那个）
4. 点击窗口右下角的 **绿色 + 按钮**
5. 在弹出的"Pick a name"对话框中，输入：**`NS_OpenSplat4D`**
6. 点击 **Save**

现在应该自动弹出了 **Niagara Editor** 窗口。如果没有自动弹出：
- 在 Content Browser 中**双击**刚才创建的 `NS_OpenSplat4D` 图标

---

## 3. 创建 System 和 Emitter

在 Niagara Editor 中，你会看到一个分栏界面。

### 3.1 界面说明（新手必读）

| 面板位置 | 名称 | 用途 |
|----------|------|------|
| 左侧竖条 | **Scratch Pad**（草稿本） | 创建自定义模块的地方 |
| 中间上方 | **System Overview**（系统概览图） | 显示整个系统的结构，是一个节点图 |
| 中间下方 | **Emitter Properties** / **Module Graph** | 选中不同节点时显示不同内容 |
| 右侧 | **Parameters**（参数面板） | 显示和编辑所有参数 |
| 左上角 | **Toolbar**（工具栏） | 编译、保存按钮 |

### 3.2 确认 System 结构

在 **System Overview** 中间区域，你应该能看到：
- 一个蓝色盒子叫 **"NS_OpenSplat4D"**（System 节点）
- 下面连着一个盒子叫 **"Emitter"**（发射器节点）

如果没有 Emitter 节点：
1. 右键 System Overview 空白处
2. 选择 **Add Emitter** → **Empty**

---

## 4. 配置 Emitter 为 GPU 模式

### 4.1 选中 Emitter

在 System Overview 中**单击** Emitter 盒子（不是双击，是单击选中它）。

### 4.2 修改属性

选中 Emitter 后，下方会显示 **Emitter Properties** 面板。找到并修改以下设置：

1. **Sim Target**（模拟目标）
   - 当前值可能是 `CPUSim`
   - 点击下拉菜单 → 选择 **`GPUComputeSim`**

2. **Local Space**（本地空间）
   - 确保**取消勾选**（不打勾）

3. **Determinism**（确定性）
   - **勾选**它

4. **Fixed Bounds**（固定边界）
   - 展开这个选项
   - Min X/Y/Z 都设为 **`-500000`**
   - Max X/Y/Z 都设为 **`500000`**

---

## 5. 删除不需要的默认模块

### 5.1 找到 Emitter 的模块列表

在 System Overview 中，**双击** Emitter 盒子。下方会切换到 **模块区域**，你会看到几个默认的模块：

- `Spawn Rate`（生成速率）
- `Initialize Particle`（初始化粒子）
- `Add Velocity`（加速度）
- `Solve Forces and Velocity`（受力与速度）
- 可能还有其他模块

### 5.2 逐个删除

依次选中每个模块，按键盘上的 **Delete** 键删除。

**全部删除，一个不留。** 所有默认模块都删掉（它们是 CPU 模式用的，GPU 模式不需要）。

删除后模块区域应该是空的。

---

## 6. 创建 Scratch Pad 模块（核心步骤）

Scratch Pad 是左侧的竖条面板。这是我们创建自定义逻辑的地方。

**ℹ️ 什么是 Scratch Pad？** 它让你在 Emitter 内部编写自定义模块脚本，而不需要创建额外的资产文件。

### 6.1 找到 Scratch Pad 面板

看编辑器**左侧**，有一个竖条或标签页写着 **"Scratch Pad"**。

如果看不到 Scratch Pad：
- 顶部菜单 **Window** → **Scratch Pad**

### 6.2 创建第一个模块：GetPointCount

1. 在 Scratch Pad 面板中，点击 **+** 按钮
2. 选择 **New Module**
3. 在弹窗中输入模块名：**`GetPointCount`**
4. 点击 **OK**
5. 现在 Scratch Pad 中出现了 `GetPointCount` 条目，**单击它**选中它

#### 6.2.1 设置模块用途

在选中 `GetPointCount` 后，看下方的属性面板：
- **Module Usage**：改为 **`Emitter Spawn Script`**

> 这是发射器初始化阶段——告诉 Niagara 这个模块只在发射器第一次创建时执行。

#### 6.2.2 添加 DI 函数调用（关键步骤）

现在在中间下方的 **Graph** 区域（图编辑器）：

1. **右键**空白处
2. 在弹出菜单中搜索或找到：**Data Interface Function**
3. 子菜单中应该有 `OpenSplat4D Point Cloud`（或 `OpenSplat4D`）
4. 再下一层选择 **GetPointCount**
5. 一个 **函数节点**出现在图中（蓝色或绿色的盒子）

这个节点代表调用 `GetPointCount()` 来获取点云中的高斯数量。

#### 6.2.3 连接输出

现在图中有两个节点：
- **Output** 节点（右侧，Emitter Spawn 的输出节点）
- **GetPointCount** 节点（我们刚添加的）

1. 看 GetPointCount 节点的右侧，有一个输出 pin（小圆圈）标记为 **`PointCount`**（int 类型）
2. 看 Output 节点左侧，有输入 pins
3. 把 `PointCount` pin **拖拽连接**到 Output 节点的某个输入上

如果 Output 节点没有 `SpawnCount` 输入：
- 确保 Module Usage 已设为 `Emitter Spawn Script`
- Output 节点应该有一个输入 pin 叫 `SpawnNum` 或类似名称
- 如果实在找不到匹配的 pin，先跳过连接——最重要的是函数节点存在于图中

**按 Ctrl+S** 保存 Scratch Pad 模块。

### 6.3 创建第二个模块：GetPointData

1. 在 Scratch Pad 面板中，再次点击 **+**
2. 选择 **New Module**
3. 命名为：**`GetPointData`**
4. 点击 **OK**
5. 单击选中 `GetPointData` 条目

#### 6.3.1 设置模块用途

- **Module Usage**：改为 **`Particle Update Script`**

> 这是粒子更新阶段——每帧对每个粒子执行。

#### 6.3.2 添加 DI 函数调用

1. 在 Graph 空白处**右键**
2. 选择 **Data Interface Function** → **OpenSplat4D** → **GetPointData**
3. 函数节点出现

这个节点有这些输出小圆圈：

| Pin 名称 | 类型 | 含义 |
|----------|------|------|
| `Position` | Vector3 | 高斯中心位置（世界坐标） |
| `Quat` | Vector4 | 四元数旋转 |
| `Scale` | Vector3 | 缩放（X/Y/Z 表示椭球半径） |
| `Color` | LinearColor | RGBA 颜色（A 表示不透明度） |

#### 6.3.3 连接到粒子属性

现在要把这些输出连接到粒子上，让每个粒子携带对应的高斯数据。

**方式一：使用 Map Set 节点（推荐）**

对每个输出 pin：

1. 从 pin 拖出一条线
2. 在空白处松手
3. 弹出菜单 → 搜索 **"Map Set"**
4. 选择 **Map Set** → 然后再选择对应的粒子属性

具体映射关系：

| DI 输出 | 搜索关键词 | 连接的粒子属性 |
|---------|-----------|---------------|
| `Position` | 拖线 → Map Set → 搜索 `Position` | **Particles.Position** |
| `Color` | 拖线 → Map Set → 搜索 `Color` | **Particles.Color** |
| `Scale` | 拖线 → 搜索 `Length` → Length 的输出 → Map Set → `SpriteSize` | **Particles.SpriteSize** |
| `Quat` | 见下方 [6.4 节](#64-quat-转-sprite-rotation) | **Particles.SpriteRotation** |

**方式一的详细说明：**

- 从 `Position` pin 拖线 → 松手 → 弹窗搜索 `Map Set` → 选择后出现新节点
  → 在 Map Set 节点的 **Variable** 属性中选择 `Particles.Position`
  → DI 的 Position pin 连接到 Map Set 的输入
  → Map Set 的输出连接到 Output 节点

重复这个流程处理 `Color`。

对于 `Scale`：需要先添加一个 **Length** 节点（搜索 `Length`），把 Scale 输入进去，Length 输出一个 float 值。然后把这个 float 值通过 Map Set 写入 `Particles.SpriteSize`（分别连 X 和 Y）。

**💡 提示**：如果你不确定怎么连线，最低要求是节点存在于图中。运行时 Actor 代码会通过 DI 直接设置粒子数据。

#### 6.4 Quat 转 Sprite Rotation

四元数需要转换为旋转角度才能用于 Sprite。我们需要一个 **Custom HLSL Node**：

1. 在 Graph 空白处**右键** → 搜索 **"Custom HLSL"**
2. 添加一个 **Custom HLSL** 节点
3. 选中该节点，在右侧 Details 面板配置：
   - **Inputs**：点击 + 添加一个输入
     - Name: `Quat`
     - Type: `Vector4`
   - **Outputs**：点击 + 添加一个输出
     - Name: `Angle`
     - Type: `Float`
   - **Code**（HLSL 代码框）：粘贴以下代码：
```hlsl
float sinA = 2.0 * (Quat.w * Quat.z + Quat.x * Quat.y);
float cosA = 1.0 - 2.0 * (Quat.y * Quat.y + Quat.z * Quat.z);
return atan2(sinA, cosA);
```

4. 将 DI 的 `Quat` pin 连接到 Custom HLSL 的 `Quat` 输入
5. 将 `Angle` 输出通过 Map Set 连接到 `Particles.SpriteRotation`

**按 Ctrl+S** 保存 Scratch Pad 模块。

### 6.5 将 Scratch Pad 模块添加到 Emitter

1. 回到 **System Overview**
2. 在 Emitter 的 Spawn 阶段（或空白区域），**右键**
3. 搜索 `GetPointCount` → 添加它
4. 在 Emitter 的 Update 阶段，**右键**
5. 搜索 `GetPointData` → 添加它

现在你的 Emitter 有两个自定义模块：一个在生成阶段获取点数，一个在更新阶段获取每个点的数据。

---

## 7. 添加 Sprite Renderer

Sprite Renderer 负责把每个粒子画成一个面朝相机的方形卡片。

### 7.1 添加 Renderer

1. 在 Emitter 节点上找到 **Render** 区域（通常在底部）
2. 点击 **+** 按钮
3. 选择 **Sprite Renderer**

### 7.2 配置材质

1. 选中刚添加的 **Sprite Renderer** 节点
2. 在下方 Details 面板中：
   - **Material**：点击下拉菜单 → 搜索 `M_OpenSplat4DSprite`
   - 如果找不到这个材质，说明材质还没创建。回到 Content Browser → `Content/OpenSplat4D/Materials/` 确认有 `M_OpenSplat4DSprite`（插件启动时会自动创建）
   - 如果没有，关闭 Niagara Editor，回到主编辑器，等几秒让插件初始化，再打开 Niagara Editor

3. **Alignment**：设为 **Unaligned**
4. **Facing Mode**：设为 **Face Camera**

---

## 8. 暴露 User 参数

User 参数让外部 C++ 代码能够向 Niagara System 传入数据。

### 8.1 打开 User Parameters

在 System Overview 中**单击** System 节点（顶部的 `NS_OpenSplat4D` 蓝色盒子）。

### 8.2 添加参数

看右侧的 **Parameters** 面板，找到 **User Exposed** 分组。

1. 点击 **+** 按钮
2. 选择 **Data Interface**
3. 在弹出的搜索框中搜索 **`OpenSplat4D`**
4. 选择 **OpenSplat4D Point Cloud**
5. 参数名设为 **`PointCloud`**
6. 回车确认

同样的方式添加第二个参数：

1. 再次点击 **+** → **Data Interface**
2. 搜索 **`Curve`**
3. 选择 **Niagara Data Interface Curve**
4. 参数名设为 **`FeatureCurve`**
5. 回车确认

---

## 9. 编译和保存

### 9.1 编译

点击 Niagara Editor 左上角工具栏的 **Compile** 按钮（或按 **F6**）。

看底部的 **Message Log**（消息日志），应该没有红色错误。

⚠️ 如果出现编译错误，通常是连线不正确——检查 Map Set 节点是否正确配置了目标变量名。

### 9.2 保存

1. 点击工具栏的 **Save** 按钮（或 **Ctrl+S**）
2. 关闭 Niagara Editor

### 9.3 验证文件位置

1. 在 Content Browser 中导航到 `Content/OpenSplat4D/Niagara/Templates/UE5.8/`
2. 你应该能看到 `NS_OpenSplat4D` 图标
3. **右键** → **Asset Actions** → **Reload**（确保加载的是最新版本）

---

## 10. 测试渲染效果

### 10.1 导入测试点云

1. 在 Content Browser 中导航到 `Content/OpenSplat4D/`
2. 把鼠标悬停在右侧空白区
3. 从 Windows 资源管理器**拖拽**一个 `.ply` 文件到 Content Browser
4. 应该会自动创建一个 `UOpenSplat4DPointCloud` 资产

### 10.2 放置到关卡

1. 在 Content Browser 中选中刚创建的点云资产
2. **拖拽**它到关卡视口（3D 视图）中
3. 应该出现一个 Actor，并且能看到点云渲染

### 10.3 问题排查

| 问题 | 可能原因 | 解决方法 |
|------|----------|----------|
| 完全看不到东西 | NS_OpenSplat4D 未找到 | 确认文件在 Templates/UE5.8/ 下 |
| 能看到粒子但位置/大小不对 | Scale→Length→SpriteSize 连线有问题 | 重新检查 6.3.3 节连线 |
| 粒子卡住不动 | 正常（Static3D 模式） | 切换到 Dynamic4D 模式检查动画 |
| 编译报 "参数不匹配" | DI 函数节点签名问题 | 选择 DI 节点 → 右键 → Refresh Node |

---

## 附录 A：Content 目录结构说明

```
Content/
├── Materials/
│   └── M_OpenSplat4DSprite.uasset     ← C++ 自动创建
└── Niagara/
    ├── Templates/
    │   ├── UE5_3/                     ← UE 5.3/5.4 用
    │   │   └── NS_GaussianSplattingPointCloud.uasset
    │   └── UE5_8/                     ← UE 5.8+ 用
    │       └── NS_OpenSplat4D.uasset
    └── MANUAL_SETUP.md
```

> ⚠️ 文件夹名用下划线 `UE5_8` 而非 `UE5.8`——UE 会把点号当包名分隔符。

C++ 会自动按引擎版本查找：`Templates/UE5_8/` → `UE5_7/` → ... → `UE5_3/`

## 附录 B：故障排查

| 问题 | 原因 | 解决 |
|------|------|------|
| `LoadPackage can't find package 8/NS_OpenSplat4D` | 文件夹名用了 `.` → UE 解析错误 | 必须用下划线 `UE5_8` |
| `DataInterface(OpenSplat4D) was not found` | 系统中未暴露 `User.PointCloud` 参数 | 回第 8 步，添加 Data Interface 参数 |
| 编译错误 | 图结构问题 | 重新按步骤创建，确保删掉所有 CPU 模块 |
