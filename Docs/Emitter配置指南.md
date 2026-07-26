# OpenSplat4D Emitter 配置指南（一次性操作）

> 适用版本：UE 5.8  
> 前提：插件已编译并加载，已导入过至少一个 `.ply` 文件

---

## 在哪找到 NE_OpenSplat4D？

### 方法一：Content Browser 搜索（推荐）

1. 打开 UE 编辑器
2. 在底部 **Content Browser**（内容浏览器）面板中
3. 点击右上角的 **🔍 搜索框**，输入 `NE_OpenSplat4D`
4. 搜索结果中双击打开它

### 方法二：按路径找

1. Content Browser 左侧面板 → 点击 **Content** 文件夹
2. 找到并双击 **OpenSplat4D** 文件夹
3. 打开 **Niagara** 子文件夹
4. 双击 **NE_OpenSplat4D** 文件

### 方法三：如果找不到

如果没有找到，说明 Emitter 尚未创建。拖一个 `.ply` 文件到 Content Browser 完成一次导入，插件会自动创建所有资产。

---

## 第一步：打开 NE_OpenSplat4D

双击后，会打开 **Niagara Editor**（Niagara 编辑器），界面大致如下：

```
┌──────────────────────────────┐
│  工具栏（保存、编译等）       │
├────────────┬─────────────────┤
│  左侧面板   │  中间大区域      │
│  System     │  （视口预览）    │
│  Overview   │                 │
│             │                 │
│  参数列表   │                 │
│             │                 │
├────────────┴─────────────────┤
│  底部面板：Emitter Script     │
│  ┌─ Spawn Script ───────────┐│
│  │  (我们在这删除默认模块)    ││
│  ├─ Update Script ──────────┤│
│  │  (我们在这添加 DI 调用)    ││
│  ├─ Particle Renderer ──────┤│
│  │  SpriteRenderer (已经有了) ││
│  └──────────────────────────┘│
└──────────────────────────────┘
```

---

## 第二步：配置 Spawn Script（生成粒子）

找到底部面板的 **Spawn Script** 区域：

### 2.1 删除默认模块

Spawn Script 下面可能有一个或多个模块（Module），比如 "Spawn Rate"。全部删除：

1. 点击选中一个模块（会高亮）
2. 按键盘 **Delete** 键删除
3. 重复，直到 Spawn Script 下面**没有任何模块**

> 预期结果：Spawn Script 下面是空的（或者只剩下一个 "Emitter State"）

### 2.2 添加 Scratch Pad 模块

Scratch Pad 是一个特殊的脚本，它运行在 GPU 上，负责告诉引擎要生成多少个粒子。

1. 在 Spawn Script 区域，点击 **➕** 加号按钮
2. 在弹出的模块列表中，搜索 `Scratch Pad`
3. 选择 **Scratch Pad**（或 **Scratch Pad Module**）
4. 点击添加

此时 Spawn Script 下会出现一个 **Scratch Pad** 条目，双击打开它。

### 2.3 在 Scratch Pad 中调用 DI

Scratch Pad 打开后，中间是节点的蓝图样式的编辑区域：

1. **找到 Data Interface**：
   - 在左侧 **System Parameters** 面板中
   - 找到 **User** → **PointCloud**（这是我们数据接口的引用）
   - 把它**拖入**中间的图形编辑区

2. 弹出菜单后，选择 **GetPointCount**

3. 节点会自动生成。现在需要连接：
   - 把 `Out_Val`（整数输出）连接到 **Emitter.SpawnInfo.Count**（或类似的 Spawn Count 输入）
   - 如果没有直接的 Spawn Count 引脚，使用 **Set Parameter** 节点：
     - 右键空白处 → 添加 **Set Parameter**  
     - 参数名选择 **Emitter.SpawnInfo.Count**
     - 把 `Out_Val` 连接到这个节点的输入

4. 如果上述不成功，试试这个简单方法：
   - 不添加 Scratch Pad
   - 直接在 Spawn Script 添加 **Spawn Burst Instantaneous** 模块
   - 设置 Spawn Count 为 **10000000**（一千万）
   - 在 Update Script 中 kill 掉多余的粒子（后面会讲）

---

## 第三步：配置 Update Script（设置粒子属性）

找到底部面板的 **Particle Update**（或 Update Script）区域：

### 3.1 删除默认模块

Update Script 下的默认模块（Add Velocity、Apply Forces、Apply Drag、Curl Noise Force 等）全部删除（选中 → Delete）。只保留空的 Update Script。

### 3.2 添加 DI 函数调用

1. 在 Update Script 区域，点击 **➕** 加号按钮
2. 在弹出的模块列表中，搜索 `PointCloud` 或直接选择 **Scratch Pad**（对 update script 也可以用）
3. 添加一个模块

或者在更新脚本区域直接：
1. 从左侧 **System Parameters** → **User** → **PointCloud** 拖入图形编辑区
2. 选择 **GetPointData4D** 函数

### 3.3 连线（核心步骤）

`GetPointData4D` 节点有以下输出引脚：

| 引脚名 | 类型 | 含义 | 连接到 |
|--------|------|------|--------|
| `Position` | Vector3 | 粒子世界坐标 | `Particles.Position` |
| `Quat` | Vector4 | 旋转四元数(x,y,z,w) | 用于计算 SpriteRotation |
| `Scale` | Vector3 | 缩放 | 用于计算 SpriteSize |
| `Color` | LinearColor | RGBA颜色 | `Particles.Color` |
| `AnchorTime` | Float | 4D时间锚点 | （可忽略） |
| `TimeVariance` | Float | 4D时间方差 | （可忽略） |
| `Velocity` | Vector3 | 4D速度 | （可忽略） |

**输入引脚**：
| 引脚名 | 类型 | 含义 |
|--------|------|------|
| `Index` | Int | 粒子索引。连接到 `Particles.ID` 或 `Particles.UniqueID` |

**连线操作**：
1. 把 `Index` 连接到 `Particles.ID`（或 `Particles.UniqueID`）
2. 把 `Position` 连接到 `Particles.Position`
3. 把 `Color` 连接到 `Particles.Color`
4. 对于 Scale：添加一个 **Length** 节点（搜索 length），把 Scale 连进去，输出连到 `Particles.SpriteSize`（需要同时连 X 和 Y）
5. 对于 Quat 转 Rotation：
   - 添加 **Custom HLSL** 节点（搜索 "Custom"）
   - 在里面写：
     ```
     float sinA = 2.0 * (Quat.w * Quat.z + Quat.x * Quat.y);
     float cosA = 1.0 - 2.0 * (Quat.y * Quat.y + Quat.z * Quat.z);
     return atan2(sinA, cosA);
     ```
   - 把输出连到 `Particles.SpriteRotation`

### 3.4 处理多余粒子（重要！）

如果用固定 Spawn Count（1000万），需要 kill 掉超过实际点数的粒子：

1. 在 Update Script 开头添加一个 **Custom HLSL** 节点
2. 输入代码：
   ```
   int PointCount;
   PointCloud.GetPointCount(PointCount);
   if (Particles.ID >= PointCount) {
       // Kill particle
       Particles.Age = 999999.0;
   }
   ```

---

## 第四步：保存

配置完毕后：
1. 按 **Ctrl+S**（或工具栏 Save 按钮）
2. 关闭 Niagara Editor
3. 重新拖入一个 `.ply` 文件测试

---

## 极简方案（如果上面步骤太复杂）

如果你只想要一个**能用的版本**，这里是最简配置：

### Spawn Script
不用改，保留默认的 Spawn Rate 模块（设 SpawnRate = 1000000）

### Update Script
1. 删除所有默认模块
2. 在空白处右键 → **Scratch Module**
3. 从左侧 **User → PointCloud** 拖入 → 选 **GetPointData4D**
4. 连线：
   - Index ← `Particles.ID`
   - Position → `Particles.Position`
   - Color → `Particles.Color`

这样就够了。多余的粒子会自动飘走（不会影响主要点云）。

---

## 常见问题

**Q: 拖入 PointCloud 时没有弹出函数选择菜单？**
A: 先确认 System 的 User Parameters 中有 PointCloud 参数。在左侧面板 → System Overview → User Parameters 检查。如果没有，重新导入一个 `.ply` 文件。

**Q: `Particles.ID` 找不到？**
A: 在 Update Script 的 Map Get（模块输入）节点中，搜索 "ID" 或 "UniqueID"。

**Q: 保存后编译失败？**
A: 查看 Niagara Editor 底部的 Compile 输出。常见原因：连线类型不匹配。SpriteSize 需要 float2，Position 需要 float3。

**Q: 渲染还是空白？**
A: 检查 NE_OpenSplat4D 的 Sprite Renderer 中 Material 是否为 `M_OpenSplat4DSprite`。在底部 Particle Renderer 区域确认。

---

> 配置完一次后，所有 `.ply` / `.4dgs` 导入都会自动使用这个 Emitter，无需再次配置。
