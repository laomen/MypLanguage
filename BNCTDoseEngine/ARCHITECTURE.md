# BNCT Dose Engine — 架构设计 v4.0

## 文件总览

```
BNCTDoseEngine/
├── mapping_demo.myp      # 主入口: mapping 事件驱动 + @parallel for
├── source.myp            # 粒子源 (事件发射器)
├── transport.myp         # 输运引擎 (@parallel for 多线程)
├── tally.myp             # 累计器 (TallyData @static 全局数组)
├── data_manager.myp      # 数据管理器 + DataLoader (HDF5 加载线程)
├── cross_section_db.myp  # @static 截面数据库
├── xs_loader.myp         # HDF5 截面加载 + 对数插值
├── hdf5.myp              # HDF5 FFI 声明
├── hdf5_bridge.c         # HDF5 C 桥接层
├── physics.myp           # 物理模型 (能谱/散射/俘获)
├── random.myp            # 随机数生成器
├── nuclide.myp           # 核素数据结构
├── material.myp          # 材料定义
├── mesh.myp              # 网格 + 体素
├── partical.myp          # 粒子数据结构
├── photon.myp            # 光子数据结构
├── xs_data.myp           # 截面数据结构
├── logger.myp            # 日志模块
├── CMakeLists.txt        # 构建配置
└── build/
    └── sim               # 编译产物
```

## 构建与运行

```bash
cd BNCTDoseEngine/build
cmake .. && make -j$(nproc)
./sim                     # 5M 粒子 (默认配置, ~3s 16线程)
```

修改粒子数: 编辑 `mapping_demo.myp` 中 `nP` 值。

### 依赖

- MYP 编译器 (`mypc`) — 来自 `MYPLanguage/build`
- HDF5 serial — `pkg-config hdf5-serial`
- pthread — 运行时线程池

---

## 1. 事件驱动架构

### 1.1 核心流程

```
mapping() 注册事件链:

  Main.prepareData ──► DataLoader.load     (独立 @thread 线程)
  DataLoader.DataReady ──► Source.start     (主线程)
  Source.batchReady ──► Transport.runBatch  (顺序事件)
  Source.allDone ──► Tally.writeResults     (完成后输出)
```

### 1.2 详细事件流

```
@startup Main.run()
  └─ prepareData(dataDir, nb, bs) ──mapping──► DataLoader.load()
      [DataLoader @thread 异步加载 HDF5]
      └─ DataReady(nb, bs) ──mapping──► Source.start()
          └─ for b=0..nBatches:
               batchReady(b, size) ──mapping──► Transport.runBatch()
                 └─ @parallel for: 线程池 (16线程) 并行处理粒子
                    ├── 源采样 → 能量采样 → 飞行 → 碰撞
                    ├── Atomic.addDouble: 剂量/通量/KERMA 累加
                    └── Atomic.addInt: 俘获数累加
          └─ allDone() ──mapping──► Tally.writeResults()
```

### 1.3 组件职责

| 组件 | 类型 | 角色 |
|------|------|------|
| **Main** | class (事件源) | `@startup run()` 初始化参数，触发数据加载 |
| **DataLoader** | class @thread | 独立线程加载 HDF5 截面数据 |
| **Source** | class (事件源) | 收到 DataReady 后分批次发射 batchReady 事件 |
| **Transport** | class (消费者) | `@parallel for` 多线程粒子输运 |
| **Tally** | class (累计器) | 收到 allDone 后输出深度剂量分布 |

---

## 2. 多线程并行 (@parallel for)

### 2.1 线程池架构

```myp
// transport.myp — 在 runBatch 内部使用 @parallel for
class Transport {
    action:
        void runBatch(int batchId, int size) {
            // ... 加载截面数据到局部变量 ...
            @parallel for (long i = 0L; i < size; i = i + 1L) {
                // 每个粒子独立处理，无数据依赖
                long state = (batchId * size + i) * 152917L + 1L;
                // ... 源采样 → 输运 → 碰撞 → 剂量累加 ...
                Atomic.addDouble(depthDose, iz, energy);
            }
            // 写回 @static 数组
            TallyData.depthDose = depthDose;
        }
}
```

### 2.2 线程安全机制

| 机制 | 说明 |
|------|------|
| **每粒子独立 RNG** | 每个粒子根据 `(batchId * size + i)` 初始化唯一 `state`，无竞争 |
| **Atomic 累加** | `Atomic.addDouble` / `Atomic.addInt` 保护 Tally 数组 |
| **@static 全局只读** | 截面数据 (`CrossSectionDB.*`) 全局只读，无需同步 |
| **局部变量拷贝** | 每批开始时将 @static 数组指针拷贝到局部变量，减少全局访问 |

### 2.3 变量捕获机制 (编译时)

`@parallel for` 的 codegen 自动捕获外层作用域的所有变量到结构体，通过 `void* arg` 传递给线程池工作函数。支持以下类型:

- `int`/`long`/`double` — 值捕获
- `double[]`/`int[]` — 指针捕获 (堆数组，所有线程共享)
- `class` 实例 — 指针捕获
- 静态方法调用 (`Physics.*`, `Random.*`) — 直接函数调用

---

## 3. @static class — 全局共享数据

### 3.1 截面数据库

```myp
@static class CrossSectionDB {
    property:
        double[] eGrid;       // 能量网格 (eV)
        double[] h1El;        // H-1 弹性截面 (barn)
        double[] h1Cap;       // H-1 俘获截面
        double[] o16El;       // O-16 弹性截面
        double[] b10Cap;      // B-10 俘获截面
        long nE;              // 网格点数
        double nB, nH, nO;    // 原子数密度 (cm⁻³)
        double phantomSize;   // 水模厚度 (cm)

    static:
        void load(string dataDir);       // 从 HDF5 加载截面
        void setBDensity(double ppm);    // 设置硼浓度
}
```

### 3.2 网格数据

```myp
class Mesh {
    property:
        long nx, ny, nz;        // 体素网格维度
        double dx, dy, dz;      // 体素大小 (cm)
        double origin_x, origin_y, origin_z;  // 原点坐标
}
```

### 3.3 Tally 数据

```myp
@static class TallyData {
    property:
        double[] depthDose;       // [nz] 每层沉积能量 (MeV)
        int[] depthCap;           // [nz] 每层俘获数
        int[] depthBCap;          // [nz] 每层 B-10 俘获数
        double[] depthFlux;       // [nz] 每层跟踪长度通量 (cm)
        double[] depthDoseKerma;  // [nz] 每层 KERMA 剂量 (MeV)
        int[] pendingBatches;     // [1] 剩余批次数
        int nz;                   // 深度层数
        double phantomSize;       // 水模厚度 (cm)
}
```

---

## 4. 物理模型

### 4.1 截面来源

数据来自 ENDF/B-VIII.0，通过 HDF5 格式读取。支持的核素:

| 核素 | 截面类型 | 来源文件 |
|------|---------|---------|
| H-1 | 弹性 (MT=2), 俘获 (MT=102) | `H1.h5` |
| O-16 | 弹性 (MT=2) | `O16.h5` |
| B-10 | 俘获 (MT=102) | `B10.h5` |

### 4.2 输运算法

```
每粒子循环:
  1. 源采样: BNCT 束 (高斯径向分布, 三区能谱)
     - 热区 (0-0.1): Maxwell 峰 0.0253 eV
     - 超热区 (0.1-0.8): 1 eV ~ 10 keV 均匀对数
     - 快区 (0.8-1.0): 10 keV ~ 1 MeV 均匀对数
  2. 飞行距离: d = -ln(ξ) / Σt
  3. 碰撞处理:
     - H-1 弹性: 质心各向同性, E' = E·(1+μ_cm)/2
     - O-16 弹性: 质心各向同性, E' 减小公式
     - H-1 俘获: 2.22 MeV 瞬发伽马
     - B-10 俘获: 2.33 MeV (α+Li 反冲)
  4. 隐式俘获: wgt *= Σel/Σt (权重衰减)
  5. 俄式轮盘: wgt < wMin 时赌命
  6. 通量估计: Track-Length Flux = d·wgt
  7. KERMA: 弹性能量转移 + 俘获能量
```

### 4.3 能谱采样

```
xi = Random.prn(state)
xi < 0.10: Maxwell 热谱 (0.0253 eV, 裂变谱温度)
xi < 0.80: 1 eV ~ 10 keV 均匀对数分布
else:      10 keV ~ 1 MeV 均匀对数分布
```

---

## 5. HDF5 数据加载

### 5.1 三层架构

```
MYP 应用层 (xs_loader.myp)
  XSInterp.loadData(path, dataset) → double[]
  XSInterp.interp(E, energies, values, n) → double
        ↓
FFI 封装层 (hdf5.myp)
  class H5: open / close / datasetSize / readDouble / loadDataset
        ↓ ffi
C 桥接层 (hdf5_bridge.c)
  myp_h5_open / myp_h5_close / myp_h5_dataset_size / myp_h5_read_double
        ↓ 链接
HDF5 C API (系统安装)
  H5Fopen / H5Dopen2 / H5Dread / H5Fclose
```

### 5.2 初始化顺序

```
DataLoader.load (在 @thread 线程异步执行)
  ├── CrossSectionDB.load(dataDir)
  │     ├── H5.loadDataset("H1.h5", energy)     → eGrid
  │     ├── H5.loadDataset("H1.h5", elastic)    → h1El
  │     ├── H5.loadDataset("H1.h5", capture)    → h1Cap
  │     ├── H5.loadDataset("O16.h5", elastic)   → o16El
  │     └── H5.loadDataset("B10.h5", capture)   → b10Cap
  ├── DataManager.initNuclides()    → 注册 H-1, O-16, B-10
  ├── DataManager.initWaterMaterial() → 构建水材料
  ├── DataManager.initMesh(nx,ny,nz,dx,dy,dz) → 网格
  └── CrossSectionDB.setBDensity(ppm) → 硼浓度
```

---

## 6. 性能参考

### 6.1 运行时间 (16 核, 16 线程池)

| 粒子数 | 时间 | 说明 |
|--------|------|------|
| 5×10⁶ | ~3s | 快速验证 |
| 1×10⁸ | ~57s | 中等规模 (~1min) |
| 1×10⁹ | ~9.5min | 全规模运行 |

### 6.2 物理结果验证 (1e9 粒子, 水模 30cm, B-10=0ppm)

```
 depth  | dose (MeV)  | flux (cm)   | kerma (MeV)
 0.5    | 1.35e+13    | 8.85e+08    | 8.30e+12
 5.5    | 7.87e+10    | 1.91e+07    | 4.42e+10
10.5    | 5.08e+09    | 4.80e+05    | 2.80e+09
15.5    | 4.44e+08    | 3.24e+04    | 2.29e+08
20.5    | 4.07e+07    | 2.91e+03    | 2.05e+07
25.5    | 4.21e+06    | 2.55e+02    | 3.31e+06
29.5    | 1.59e+06    | 7.38e+01    | 4.90e+05
```

中子通量和剂量随深度指数衰减，在 20cm 后衰减约 6 个数量级，符合水模中子输运物理。
