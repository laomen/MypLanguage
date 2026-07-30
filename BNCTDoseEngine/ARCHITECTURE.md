# BNCT Dose Engine — 架构设计 v3.0 (完整模块化 + mapping 事件驱动)

## 文件总览

```
BNCTDoseEngine/
├── partical.myp      # 粒子数据结构
├── nuclide.myp       # 核素微观截面
├── material.myp      # 材料定义
├── mesh.myp          # 网格 + 体素
├── random.myp        # 随机数生成器
├── physics.myp       # 物理模型 (碰撞/散射/能谱)
├── xs_loader.myp     # HDF5 截面加载 + 对数插值
├── hdf5.myp          # HDF5 FFI 声明 (MYP 侧)
├── hdf5_bridge.c     # HDF5 FFI C 桥接层
├── xs_data.myp       # 截面数据结构
├── cross_section_db.myp  # @static 类: 全局截面数据库
├── data_manager.myp      # 数据管理器
├── source.myp        # 粒子源 (事件发射器)
├── transport.myp     # 输运引擎 (事件消费者/源)
├── tally.myp         # 累计器 (事件消费者)
├── logger.myp        # 日志模块
├── photon.myp        # 光子数据结构
├── simulation.myp    # 模拟管理器
├── material_loader.myp   # 材料加载器
├── mapping_demo.myp  # 主程序: mapping 事件驱动版
├── main.myp          # 旧版主程序: @parallel for 版
└── build/
    └── sim           # 编译产物 (链接 HDF5 bridge)
```

## 构建与运行

```bash
cd BNCTDoseEngine/build
cmake .. && make -j$(nproc)
./sim
```

## 编译依赖

- MYP 编译器 (`mypc`) — 来自 `MYPLanguage/build`
- HDF5 serial 库 — `pkg-config hdf5-serial`
- pthread — 运行时线程池

## 运行输出示例 (65 ppm B-10)

```
  depth  | dose     | cap| B10
0.5      | 12581.4  | 5635 | 652
1.5      | 8828.48  | 3954 | 460
2.5      | 4876.31  | 2184 | 253
...
  total: cap=13451 B10=1546 dose=30031.3 MeV H/B=7.70052
```
```

---

## 1. 数据结构层 (Struct)

### 1.1 `partical.myp` — 粒子

```
Vec3 { x, y, z }
ParticleType enum: NEUTRON | PROTON | ELECTRON | PHOTON
Particle {
    position:  Vec3        # 空间位置
    direction: Vec3        # 飞行方向
    weight:    double      # 粒子权重
    E:         double      # 能量 (eV)
    alive:     bool        # 存活状态
    type:      ParticleType
}
```

### 1.2 `nuclide.myp` — 核素

```
MicroscopicNeutronXs  — 弹性/非弹性/热散射/俘获/裂变微观截面
MicroscopicPhotonXs   — 相干/非相干/光电/对产生微观截面
NeutronXsTableSet     — 多组能量-截面表 + 角度分布
```

### 1.3 `material.myp` — 材料

```
MaterialNuclideComponent  { nuclide, atom_density }
MacroscopicNeutronXs      { 弹性/俘获/裂变宏观截面 }
MacroscopicPhotonXs       { 相干/光电/对产生宏观截面 }
Material                  { id, name, density, nuclides[], xs }
```

### 1.4 `mesh.myp` + `voxel.myp` — 网格体素

```
Mesh        { nx, ny, nz, dx, dy, dz }
VoxelIndex  { i, j, k }
VoxelCell   { index, material_id, density_scale, active }
```

---

## 2. 事件驱动层 (mapping)

### 2.1 核心数据流

```
                     ╔═══════════════╗
                     ║   mapping()   ║
                     ╚═══════════════╝
                           │
        ┌──────────────────┼──────────────────┐
        ▼                  ▼                  ▼
   ┌─────────┐      ┌────────────┐      ┌─────────┐
   │ Source  │ ───► │ Transport │ ───► │ Tally   │
   │ (事件源) │      │ (消费者+源) │      │ (累计器) │
   └─────────┘      └────────────┘      └─────────┘
        │                                     ▲
        │  tallyInit                          │
        └─────────────────────────────────────┘
```

### 2.2 事件表

| 事件 | 源 → 目标 | 数据 |
|------|-----------|------|
| `tallyInit(nBatches)` | Source → Tally.init | 总批次数 |
| `batchReady(batchId, size)` | Source → Transport.runBatch | 批次号, 大小 |
| `batchDone(cap, bcap, dose)` | Transport → Tally.addBatch | 俘获数, 剂量 |

### 2.3 mapping 声明

```myp
mapping() {
    Source.tallyInit    -> Tally.init;          // ① 初始化累计器
    Source.batchReady   -> Transport.runBatch;  // ② 分发批次到输运
    Transport.batchDone -> Tally.addBatch;      // ③ 汇总结果到累计器
}
```

### 2.4 组件职责

| 组件 | 类型 | 角色 |
|------|------|------|
| **Source** | class (事件源) | `@startup` → 发 `tallyInit` + 循环发 `batchReady` |
| **Transport** | class (消费者+源) | 接收 `batchReady` → `@parallel for` 输运 → 发 `batchDone` |
| **Tally** | class (累计器) | 接收 `tallyInit` 设总数 → 接收 `batchDone` 累加 → 完成时输出 |

### 2.5 `@static class` — 全局共享数据

截面数据（能量网格、微观截面）、材料组分等是所有组件共同依赖的**只读型全局数据**。
使用 `@static class` 可将这些数据声明为进程级全局变量，所有实例直接访问，无需传参。

```myp
@static class CrossSectionDB {
    property:
        double[] eGrid;      // 能量网格 (eV)
        double[] h1El;       // H-1 弹性截面 (barn)
        double[] h1Cap;      // H-1 俘获截面
        double[] o16El;      // O-16 弹性截面
        double[] b10Cap;     // B-10 俘获截面
        long nE;             // 网格点数

    static:
        void load(string dataDir) {
            CrossSectionDB.eGrid = XSInterp.loadData(
                dataDir + "/H1.h5", "/H1/energy/294K");
            CrossSectionDB.h1El = XSInterp.loadData(
                dataDir + "/H1.h5", "/H1/reactions/reaction_002/294K/xs");
            CrossSectionDB.nE = 631;
        }

        double getSigma(double E, string nuclide, int mt) {
            // 任意组件直接调用，无需实例
            return XSInterp.interp(E, CrossSectionDB.eGrid,
                                   CrossSectionDB.h1El, CrossSectionDB.nE);
        }
}
```

**设计要点：**

| 特性 | 说明 |
|------|------|
| **零开销访问** | 编译为 LLVM `GlobalVariable`，直接寻址，无需 GEP 链 |
| **跨线程共享** | `@thread` 实例自动可见同一份全局数据 |
| **类名访问** | 通过 `CrossSectionDB.eGrid` 读写，语义清晰 |
| **不可实例化** | `@static class` 不允许 `new`，强制全局语义 |

**使用场景判断：**

```
用 @static class:   截面库、物理常量、配置参数（只读/少写）
用 mapping 事件:    运行时控制流、批处理分发、结果累计
用 实例 property:   体素剂量数组、运行时计数器（需 Atomic）
```

**初始化方式：**

```myp
// main() 中初始化一次，之后所有组件直接读取
CrossSectionDB.load("/data/endfb80/");

// Transport 中直接使用
class Transport {
    action:
        void runBatch(int batchId, int size) {
            double sigma = CrossSectionDB.getSigma(E, "H1", 2);
        }
}
```

---

## 3. 物理层

### 3.1 `physics.myp` (static)

```
rngStep(state)       → long          PCG 64-bit RNG
toDouble(x)          → double        i64 → [0,1)
sampleEnergy(state)  → double        BNCT 能谱 (热/超热/快)
sampleMu(state)      → double        散射角余弦
sampleDistance(xi,Σ) → double        自由程
isCapture(xi,σc,σt)  → bool          反应类型
boronCaptureMeV()    → double        2.33 MeV
h1CaptureMeV()       → double        2.22 MeV

collide(Particle&, Particle&)         碰撞处理
scatter(Particle&, mu)                散射
capture(Particle&)                    俘获
```

### 3.2 `xs_loader.myp` (static)

```
loadData(filePath, datasetPath) → double[]   从 HDF5 读取截面
interp(E, energies[], values[], n) → double  对数-对数插值
```

---

## 4. 控制流

### 4.1 初始化顺序 (重要!)

```myp
int main() {
    Tally tally  = new Tally();     // ① 先创建累计器
    Transport t  = new Transport(); // ② 再创建输运引擎
    Source s     = new Source();    // ③ 最后创建粒子源 (@startup 触发事件)
    return 0;
}
```

Source 最后创建，因为 `@startup` 在构造函数中立即发射事件，
此时 mapping 已注册、所有接收者已就绪。

### 4.2 运行流程

```
Source.@startup run()
  ├── tallyInit(nBatches)  ──mapping──► Tally.init()
  ├── loop b=0..nBatches:
  │     batchReady(b,size) ──mapping──► Transport.runBatch()
  │                                     ├── @parallel for 输运粒子
  │                                     └── batchDone(cap,dose) ──mapping──► Tally.addBatch()
  └── Tally 在所有 batch 完成后自动输出
```

---

## 5. HDF5 胶水层 (外部数据集成)

HDF5 功能不是编译器的功能，而是通过 FFI 桥接层独立接入。
分为三层：C 桥接 → MYP FFI 声明 → 上层数据加载。

### 5.1 三层架构

```
┌─────────────────────────────────────────────────────────────┐
│                    应用层 (xs_loader.myp)                      │
│  XSInterp.loadData(path, dataset) → double[]                │
│  XSInterp.interp(E, energies, values, n) → double           │
│                ↓ 调用 H5 类的静态方法                         │
├─────────────────────────────────────────────────────────────┤
│                    FFI 封装层 (hdf5.myp)                      │
│  class H5 (static):                                          │
│    open(path)          → long      包装 myp_h5_open          │
│    close(fileId)       → void      包装 myp_h5_close         │
│    datasetSize(fid,path) → long    包装 myp_h5_dataset_size  │
│    readDouble(fid,path,buf,size) → long 包装 myp_h5_read_double│
│    loadDataset(path, dataset) → double[] 便捷方法             │
│                ↓ ffi 声明                                     │
├─────────────────────────────────────────────────────────────┤
│                  FFI 声明层 (hdf5.myp)                        │
│  ffi long myp_h5_open(string path);                          │
│  ffi void myp_h5_close(long fileId);                         │
│  ffi long myp_h5_dataset_size(long fileId, string path);      │
│  ffi long myp_h5_read_double(long, string, double[], long);   │
│                ↓ C 链接                                       │
├─────────────────────────────────────────────────────────────┤
│                  C 桥接层 (hdf5_bridge.c)                     │
│  int64_t myp_h5_open(const char* path)                       │
│  void    myp_h5_close(int64_t file_id)                       │
│  int64_t myp_h5_dataset_size(int64_t, const char*)            │
│  int64_t myp_h5_read_double(int64_t, const char*, double*,    │
│                             int64_t)                          │
│                ↓ 链接 HDF5 库                                  │
├─────────────────────────────────────────────────────────────┤
│                  HDF5 C API (系统安装)                         │
│  H5Fopen / H5Dopen2 / H5Dread / H5Dget_space / H5Fclose     │
└─────────────────────────────────────────────────────────────┘
```

### 5.2 C 桥接层 (`hdf5_bridge.c`)

四个函数，每个对应一个 HDF5 操作：

| C 函数 | 调用 HDF5 API | 返回值 |
|--------|---------------|--------|
| `myp_h5_open(path)` | `H5Fopen(path, RDONLY)` | `int64_t` handle (或 -1) |
| `myp_h5_close(fid)` | `H5Fclose(fid)` | `void` |
| `myp_h5_dataset_size(fid, path)` | `H5Dopen2` → `H5Dget_space` → `H5Sget_simple_extent_dims` | `int64_t` 元素数 |
| `myp_h5_read_double(fid, path, buf, size)` | `H5Dopen2` → `H5Dread` → 写入 `double*` 缓冲区 | `int64_t` 实际读取数 |

关键细节：
- `hid_t` 在 64 位系统上是 `int64_t`，所以用 `int64_t` 传递文件句柄，避免截断
- `double* buffer` 由 MYP 侧预先分配（`new double[n]`），C 侧只填充数据
- 所有函数返回 -1 表示错误

### 5.3 FFI 封装层 (`hdf5.myp`)

```
ffi long myp_h5_open(string path);
ffi void myp_h5_close(long fileId);
ffi long myp_h5_dataset_size(long fileId, string path);
ffi long myp_h5_read_double(long fileId, string path, double[] buffer, long size);
```

`class H5` 将 FFI 函数包装为易用的静态方法，并提供 `loadDataset` 便捷方法：
1. `H5.open(path)` — 打开文件
2. `H5.datasetSize(fid, path)` — 查询数据集大小
3. `new double[n]` — 在 MYP 侧分配内存
4. `H5.readDouble(fid, path, data, n)` — 读取数据到预分配缓冲区
5. `H5.close(fid)` — 关闭文件

`H5.loadDataset` 将上述步骤组合为一个调用：打开 → 读大小 → 分配 → 读取 → 关闭 → 返回数组。

### 5.4 上层使用 (`xs_loader.myp`)

```myp
class XSInterp {
    static:
        // 从 HDF5 加载截面数据
        double[] loadData(string filePath, string datasetPath) {
            long fid = H5.open(filePath);
            if (fid < 0L) { double[] e; return e; }
            long n = H5.datasetSize(fid, datasetPath);
            if (n <= 0L) { H5.close(fid); double[] e; return e; }
            double[] data = new double[n];
            H5.readDouble(fid, datasetPath, data, n);
            H5.close(fid);
            return data;
        }

        // 对数-对数线性插值
        double interp(double E, double[] energies, double[] values, long n) {
            // 二分查找 + log-log Lerp
        }
}
```

典型用法：
```myp
double[] eGrid = XSInterp.loadData(dataDir + "/H1.h5", "/H1/energy/294K");
double[] h1El = XSInterp.loadData(dataDir + "/H1.h5", "/H1/reactions/reaction_002/294K/xs");
// ...
double sigma = XSInterp.interp(E, eGrid, h1El, nE) * 1.0e-24;  // barn → cm²
```

### 5.5 构建集成 (CMakeLists.txt)

```
hdf5_bridge.c  ──编译──► hdf5_bridge.o ──┐
                                         ├──► sim (可执行)
main.myp ──mypc──► main.myp.o ───────────┤
                                         │
runtime.c ──编译──► runtime.o ────────────┘
                              │
                    链接 libhdf5 (PkgConfig::HDF5)
```

关键点：
- C 桥接层和 MYP 代码独立编译，最后链接
- HDF5 库通过 `pkg-config` 查找（`hdf5-serial`）
- `hdf5_bridge.c` 的 include 路径包含 `mylang/runtime.h` 和 HDF5 头文件

---

## 6. 文件依赖图

```
mapping_demo.myp
  ├── source.myp       (独立)
  ├── transport.myp    (依赖 atomic, math)
  └── tally.myp        (独立)

main.myp (HDF5 版)
  ├── xs_loader.myp ──► hdf5.myp ──► (FFI → hdf5_bridge.c → libhdf5)
  ├── physics.myp   ──► partical.myp
  ├── atomic (stdlib)
  └── env (stdlib)

数据结构:
  nuclide.myp ──► material.myp
  mesh.myp    ──► voxel.myp
  random.myp  ──► physics.myp
```
```

### 3.3 切换方式的含义

| 模式 | 事件总数 | 数据方式 | 适用场景 |
|------|---------|---------|---------|
| 逐粒子 | N 个 | 数组索引 | 调试, < 1e5 |
| 批处理 | N / batchSize 个 | 流式累计 | 大规模, ≥ 1e6 |

**只改 mapping() 和一行 batchSize 就能切换，不改任何组件内部逻辑。**

---

## 4. 物理模块

Physics 是纯函数集合，被 Transport 内部调用：

```
Transport.runBatch()
  └── for i in 0..batchSize:
        └── Physics.transportOne()
              ├── Physics.rngStep() / toDouble()  — RNG
              ├── Physics.sampleDistance()         — 自由程
              ├── Physics.captureProb()            — 反应类型
              ├── Physics.boronDose()              — 能量沉积
              └── 继续或终止 → 局部累加器
```

所有 RNG 状态以 `long` 传递（纯函数），无全局状态。

---

## 5. 对比: 传统 vs Mapping 方式

| 方面 | 传统 (v1) | Mapping (v2) |
|------|-----------|-------------|
| 数据传递 | 函数参数硬编码 | 事件自动分发 |
| 控制流 | for/while 循环 | 事件链 + 自循环 |
| 组件耦合 | `Transport` 调 `Tally` | 互不知道对方存在 |
| 扩展 | 改代码 | 改一行 mapping |
| 可测试 | mock 复杂 | 事件注入即可 |
| 并行 | 手动 @threadpool | Worker + mapping 自动 |

---

## 6. 文件结构

```
BNCTDoseEngine/
├── main.myp          # mapping() 连接 + 启动
├── source.myp        # Source class
├── transport.myp     # Transport class
├── physics.myp       # Physics static class (含 RNG + 输运)
├── tally.myp         # Tally class
```

---

## 7. 当前限制

| 限制 | 影响 | 当前方案 |
|------|------|---------|
| 事件参数类型有限 | 无法传 class/struct | 批模式只传基本类型 (int, double) |
| 没有 `ref` 参数 | 无法 out 多个返回值 | 用 struct 封装返回值 (或分多次传) |
| 没有逐粒子通量分布 | 无法算空间分布 | 批模式下用数组索引辅助模式 |
| 并行 Worker 的 mapping | 多实例分发 | 多个 Worker + Tally 汇总 |
