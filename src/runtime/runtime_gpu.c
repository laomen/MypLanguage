// runtime_gpu.c — CUDA GPU offload runtime
#define _POSIX_C_SOURCE 200809L   // clock_gettime(CLOCK_MONOTONIC) — §5.1 per-kernel profiling
#include "mylang/runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#if defined(_WIN32)
/* dlfcn → LoadLibrary/GetProcAddress（CUDA driver = nvcuda.dll） */
#include <windows.h>
#include <malloc.h>   /* _alloca */
#define RTLD_LAZY 0
#define RTLD_LOCAL 0
#define RTLD_NOW 0
#define RTLD_GLOBAL 0
#define dlopen(name, flags) ((void*)LoadLibraryA(name))
#define dlsym(handle, sym) ((void*)GetProcAddress((HMODULE)(handle), (sym)))
#define dlclose(handle) (FreeLibrary((HMODULE)(handle)) ? 0 : -1)
#define dlerror() "LoadLibrary failed"
#else
#include <dlfcn.h>
#include <alloca.h>
#endif

typedef int CUresult;
typedef struct CUctx_st* CUcontext;
typedef struct CUmod_st* CUmodule;
typedef struct CUfunc_st* CUfunction;

typedef int (*cuInit_t)(unsigned int);
typedef int (*cuCtxCreate_t)(CUcontext*, unsigned int, int);
typedef int (*cuModuleLoadData_t)(CUmodule*, const void*);
typedef int (*cuModuleUnload_t)(CUmodule);
typedef int (*cuModuleGetFunction_t)(CUfunction*, CUmodule, const char*);
typedef int (*cuModuleGetGlobal_t)(unsigned long long*, size_t*, CUmodule, const char*);
typedef int (*cuLaunchKernel_t)(CUfunction, unsigned int,unsigned int,unsigned int,
    unsigned int,unsigned int,unsigned int,unsigned int,void*,void**,void**);
typedef int (*cuMemAlloc_t)(void**, size_t);
typedef int (*cuMemFree_t)(void*);
typedef int (*cuMemcpyHtoD_t)(void*, const void*, size_t);
typedef int (*cuMemcpyDtoH_t)(void*, const void*, size_t);
typedef int (*cuMemcpyDtoD_t)(void*, const void*, size_t);
typedef int (*cuCtxSynchronize_t)(void);
typedef int (*cuCtxSetCurrent_t)(CUcontext);
typedef int (*cuGetErrorName_t)(int, const char**);
typedef int (*cuCtxGetCurrent_t)(CUcontext*);
typedef int CUdevice;
typedef int (*cuDeviceGet_t)(CUdevice*, int);
typedef int (*cuDeviceGetName_t)(char*, int, CUdevice);
typedef int (*cuDeviceGetAttribute_t)(int*, int, CUdevice);
typedef int (*cuDeviceTotalMem_t)(size_t*, CUdevice);
typedef struct CUstream_st* CUstream;
typedef int (*cuStreamCreate_t)(CUstream*, unsigned int);
typedef int (*cuStreamSynchronize_t)(CUstream);
typedef int (*cuStreamDestroy_t)(CUstream);
typedef struct CUevent_st* CUevent;
typedef int (*cuEventCreate_t)(CUevent*, unsigned int);
typedef int (*cuEventRecord_t)(CUevent, CUstream);
typedef int (*cuEventSynchronize_t)(CUevent);
typedef int (*cuEventElapsedTime_t)(float*, CUevent, CUevent);
typedef int (*cuEventDestroy_t)(CUevent);
typedef int (*cuStreamWaitEvent_t)(CUstream, CUevent, unsigned int);
typedef int (*cuMemcpyHtoDAsync_t)(void*, const void*, size_t, CUstream);
typedef int (*cuMemcpyDtoHAsync_t)(void*, const void*, size_t, CUstream);
typedef int (*cuMemcpyDtoDAsync_t)(void*, const void*, size_t, CUstream);
// §P6 ② CUDA Graph（图内存）：流捕获 → 图 → 实例化 → 重放。
typedef struct CUgraph_st* CUgraph;
typedef struct CUgraphExec_st* CUgraphExec;
typedef int (*cuStreamBeginCapture_t)(CUstream, unsigned int);
typedef int (*cuStreamEndCapture_t)(CUstream, CUgraph*);
typedef int (*cuGraphInstantiate_t)(CUgraphExec*, CUgraph, unsigned long long);
typedef int (*cuGraphLaunch_t)(CUgraphExec, CUstream);
typedef int (*cuGraphDestroy_t)(CUgraph);
typedef int (*cuGraphExecDestroy_t)(CUgraphExec);
typedef int (*cuStreamIsCapturing_t)(CUstream, unsigned int*);

static void* lib = NULL;
static cuInit_t p_cuInit = NULL;
static cuCtxCreate_t p_cuCtxCreate = NULL;
static cuModuleLoadData_t p_cuModuleLoadData = NULL;
static cuModuleUnload_t p_cuModuleUnload = NULL;
static cuModuleGetFunction_t p_cuModuleGetFunction = NULL;
static cuModuleGetGlobal_t p_cuModuleGetGlobal = NULL;
static cuLaunchKernel_t p_cuLaunchKernel = NULL;
static cuMemAlloc_t p_cuMemAlloc = NULL;
static cuMemFree_t p_cuMemFree = NULL;
static cuMemcpyHtoD_t p_cuMemcpyHtoD = NULL;
static cuMemcpyDtoH_t p_cuMemcpyDtoH = NULL;
static cuMemcpyDtoD_t p_cuMemcpyDtoD = NULL;
static cuCtxSynchronize_t p_cuCtxSynchronize = NULL;
static cuCtxSetCurrent_t p_cuCtxSetCurrent = NULL;
static cuGetErrorName_t p_cuGetErrorName = NULL;
static cuCtxGetCurrent_t p_cuCtxGetCurrent = NULL;
static int avail = 0;
static CUcontext ctx = NULL;
static CUdevice dev = 0;
static int dev_count = 0;
static int dev_initialized = 0;
static cuDeviceGet_t p_cuDeviceGet = NULL;
static cuDeviceGetName_t p_cuDeviceGetName = NULL;
static cuDeviceGetAttribute_t p_cuDeviceGetAttribute = NULL;
static cuDeviceTotalMem_t p_cuDeviceTotalMem = NULL;
static cuStreamCreate_t p_cuStreamCreate = NULL;
static cuStreamSynchronize_t p_cuStreamSynchronize = NULL;
static cuStreamDestroy_t p_cuStreamDestroy = NULL;
static cuEventCreate_t p_cuEventCreate = NULL;
static cuEventRecord_t p_cuEventRecord = NULL;
static cuEventSynchronize_t p_cuEventSynchronize = NULL;
static cuEventElapsedTime_t p_cuEventElapsedTime = NULL;
static cuEventDestroy_t p_cuEventDestroy = NULL;
static cuStreamWaitEvent_t p_cuStreamWaitEvent = NULL;
static cuMemcpyHtoDAsync_t p_cuMemcpyHtoDAsync = NULL;
static cuMemcpyDtoHAsync_t p_cuMemcpyDtoHAsync = NULL;
static cuMemcpyDtoDAsync_t p_cuMemcpyDtoDAsync = NULL;
// §P6 ② CUDA Graph 函数指针
static cuStreamBeginCapture_t p_cuStreamBeginCapture = NULL;
static cuStreamEndCapture_t p_cuStreamEndCapture = NULL;
static cuGraphInstantiate_t p_cuGraphInstantiate = NULL;
static cuGraphLaunch_t p_cuGraphLaunch = NULL;
static cuGraphDestroy_t p_cuGraphDestroy = NULL;
static cuGraphExecDestroy_t p_cuGraphExecDestroy = NULL;
static cuStreamIsCapturing_t p_cuStreamIsCapturing = NULL;

typedef int (*cuDeviceGetCount_t)(int*);

// §5.2 错误友好化：CUDA driver error code → 可读字符串。无 cuGetErrorName 硬依赖
//（静态表覆盖常见错误码，避免多一次 dlsym 与符号差异）。
static const char* gpu_err_str(CUresult r) {
    switch (r) {
        case 0:   return "success";
        case 1:   return "invalid value";
        case 2:   return "out of memory";
        case 3:   return "driver not initialized";
        case 4:   return "driver deinitialized";
        case 46:  return "device unavailable";
        case 100: return "no CUDA device";
        case 101: return "invalid device";
        case 201: return "invalid context";
        case 202: return "context already current";
        case 209: return "no binary image is applicable for the device";
        case 218: return "invalid PTX (compile error)";
        case 300: return "invalid source";
        case 301: return "file not found";
        case 400: return "invalid handle";
        case 500: return "not found";
        case 600: return "not ready";
        case 700: return "illegal memory access";
        case 701: return "launch out of resources";
        case 702: return "launch timed out";
        case 709: return "context is destroyed";
        case 710: return "device-side assert triggered";
        case 719: return "launch failed";
        case 999: return "unknown error";
        default:  return "unknown error code";
    }
}

// §R0 GPU 运行时止血：全局「强制 CPU」标志 + 错误记录。
// 任何 kernel 加载/启动失败（OOB、非法访问、launch 失败等）都会：
//   1) 记录 kernel 名 + 错误码（g_fail_kernel / g_fail_code）；
//   2) 置 g_force_cpu —— 之后 myp_gpu_init() 返回 0，整条管线一致回退 CPU；
//   3) 打印明确诊断（不再静默混算）。
static int g_force_cpu = 0;
static char g_fail_kernel[128] = "";
static int g_fail_code = 0;

// 统一 CUDA 错误检查：r==0 返回 1；否则记录 + 打印 + 置 force_cpu，返回 0。
static int gpu_check_err(CUresult r, const char* what, const char* kernel) {
    if (r == 0) return 1;
    g_fail_code = (int)r;
    if (kernel && kernel[0]) {
        strncpy(g_fail_kernel, kernel, sizeof(g_fail_kernel) - 1);
        g_fail_kernel[sizeof(g_fail_kernel) - 1] = '\0';
    }
    if (!g_force_cpu) {
        g_force_cpu = 1;
        fprintf(stderr,
                "[myp GPU] %s failed: %s (%d)%s%s — forcing CPU for the whole pipeline\n",
                what, gpu_err_str(r), (int)r,
                (kernel && kernel[0]) ? " kernel=" : "",
                (kernel && kernel[0]) ? kernel : "");
    }
    return 0;
}

// 应用可查询：GPU 是否已在运行期失败（应整管线回退 CPU）。
int myp_gpu_force_cpu(void) { return g_force_cpu; }

int myp_gpu_init(void) {
    if (g_force_cpu) return 0;
    if (avail) return 1;
    // GPU offload 默认关闭：须 MYP_GPU=1 环境变量显式启用（CPU 为一等后端，
    // 无 GPU 时正常回退；启用但失败时下方给出明确诊断）。
    static int check_env = -1;
    if (check_env == -1) {
        const char* env = getenv("MYP_GPU");
        check_env = (env && env[0] == '1') ? 1 : 0;
    }
    if (!check_env) return 0;
    // MYP_GPU=1 显式要求 GPU 但初始化失败 → 明确诊断（不再静默回退）。
#if defined(_WIN32)
    const char* cuda_lib = "nvcuda.dll";
    int dlflags = 0;
    (void)dlflags;   /* dlopen 宏忽略 flags */
#else
    const char* cuda_lib = "libcuda.so.1";
    int dlflags = RTLD_LAZY | RTLD_LOCAL;
#endif
    lib = dlopen(cuda_lib, dlflags);
    if (!lib) {
        fprintf(stderr, "[myp GPU] MYP_GPU=1 but cannot load %s "
                        "(CUDA driver missing?) — falling back to CPU\n", cuda_lib);
        return 0;
    }
    p_cuInit = (cuInit_t)dlsym(lib,"cuInit");
    p_cuCtxCreate = (cuCtxCreate_t)dlsym(lib,"cuCtxCreate_v2");
    p_cuModuleLoadData = (cuModuleLoadData_t)dlsym(lib,"cuModuleLoadData");
    p_cuModuleUnload = (cuModuleUnload_t)dlsym(lib,"cuModuleUnload");
    p_cuModuleGetFunction = (cuModuleGetFunction_t)dlsym(lib,"cuModuleGetFunction");
    p_cuModuleGetGlobal = (cuModuleGetGlobal_t)dlsym(lib,"cuModuleGetGlobal");
    p_cuLaunchKernel = (cuLaunchKernel_t)dlsym(lib,"cuLaunchKernel");
    p_cuMemAlloc = (cuMemAlloc_t)dlsym(lib,"cuMemAlloc_v2");
    p_cuMemFree = (cuMemFree_t)dlsym(lib,"cuMemFree_v2");
    p_cuMemcpyHtoD = (cuMemcpyHtoD_t)dlsym(lib,"cuMemcpyHtoD_v2");
    p_cuMemcpyDtoH = (cuMemcpyDtoH_t)dlsym(lib,"cuMemcpyDtoH_v2");
    p_cuMemcpyDtoD = (cuMemcpyDtoD_t)dlsym(lib,"cuMemcpyDtoD_v2");
    if (!p_cuMemcpyDtoD) p_cuMemcpyDtoD = (cuMemcpyDtoD_t)dlsym(lib,"cuMemcpyDtoD");
    p_cuCtxSynchronize = (cuCtxSynchronize_t)dlsym(lib,"cuCtxSynchronize");
    p_cuCtxSetCurrent = (cuCtxSetCurrent_t)dlsym(lib,"cuCtxSetCurrent");
    p_cuGetErrorName = (cuGetErrorName_t)dlsym(lib,"cuGetErrorName");
    p_cuCtxGetCurrent = (cuCtxGetCurrent_t)dlsym(lib,"cuCtxGetCurrent");
    p_cuDeviceGet = (cuDeviceGet_t)dlsym(lib,"cuDeviceGet");
    p_cuDeviceGetName = (cuDeviceGetName_t)dlsym(lib,"cuDeviceGetName");
    p_cuDeviceGetAttribute = (cuDeviceGetAttribute_t)dlsym(lib,"cuDeviceGetAttribute");
    p_cuDeviceTotalMem = (cuDeviceTotalMem_t)dlsym(lib,"cuDeviceTotalMem_v2");
    p_cuStreamCreate = (cuStreamCreate_t)dlsym(lib,"cuStreamCreate");
    p_cuStreamSynchronize = (cuStreamSynchronize_t)dlsym(lib,"cuStreamSynchronize");
    p_cuStreamDestroy = (cuStreamDestroy_t)dlsym(lib,"cuStreamDestroy");
    p_cuEventCreate = (cuEventCreate_t)dlsym(lib,"cuEventCreate");
    p_cuEventRecord = (cuEventRecord_t)dlsym(lib,"cuEventRecord");
    p_cuEventSynchronize = (cuEventSynchronize_t)dlsym(lib,"cuEventSynchronize");
    p_cuEventElapsedTime = (cuEventElapsedTime_t)dlsym(lib,"cuEventElapsedTime");
    p_cuEventDestroy = (cuEventDestroy_t)dlsym(lib,"cuEventDestroy");
    p_cuStreamWaitEvent = (cuStreamWaitEvent_t)dlsym(lib,"cuStreamWaitEvent");
    p_cuMemcpyHtoDAsync = (cuMemcpyHtoDAsync_t)dlsym(lib,"cuMemcpyHtoDAsync_v2");
    if (!p_cuMemcpyHtoDAsync) p_cuMemcpyHtoDAsync = (cuMemcpyHtoDAsync_t)dlsym(lib,"cuMemcpyHtoDAsync");
    p_cuMemcpyDtoHAsync = (cuMemcpyDtoHAsync_t)dlsym(lib,"cuMemcpyDtoHAsync_v2");
    if (!p_cuMemcpyDtoHAsync) p_cuMemcpyDtoHAsync = (cuMemcpyDtoHAsync_t)dlsym(lib,"cuMemcpyDtoHAsync");
    p_cuMemcpyDtoDAsync = (cuMemcpyDtoDAsync_t)dlsym(lib,"cuMemcpyDtoDAsync_v2");
    if (!p_cuMemcpyDtoDAsync) p_cuMemcpyDtoDAsync = (cuMemcpyDtoDAsync_t)dlsym(lib,"cuMemcpyDtoDAsync");
    // §P6 ② CUDA Graph
    p_cuStreamBeginCapture = (cuStreamBeginCapture_t)dlsym(lib,"cuStreamBeginCapture");
    p_cuStreamEndCapture = (cuStreamEndCapture_t)dlsym(lib,"cuStreamEndCapture");
    p_cuGraphInstantiate = (cuGraphInstantiate_t)dlsym(lib,"cuGraphInstantiate");
    p_cuGraphLaunch = (cuGraphLaunch_t)dlsym(lib,"cuGraphLaunch");
    p_cuGraphDestroy = (cuGraphDestroy_t)dlsym(lib,"cuGraphDestroy");
    p_cuGraphExecDestroy = (cuGraphExecDestroy_t)dlsym(lib,"cuGraphExecDestroy");
    p_cuStreamIsCapturing = (cuStreamIsCapturing_t)dlsym(lib,"cuStreamIsCapturing");
    if (!p_cuInit||!p_cuCtxCreate||!p_cuModuleLoadData||!p_cuModuleGetFunction||
        !p_cuLaunchKernel||!p_cuMemAlloc||!p_cuMemFree||!p_cuMemcpyHtoD||
        !p_cuMemcpyDtoH||!p_cuCtxSynchronize) { dlclose(lib);lib=NULL; return 0; }
    int ir = p_cuInit(0);
    if (ir != 0) {
        fprintf(stderr, "[myp GPU] cuInit failed: %s — falling back to CPU\n",
                gpu_err_str(ir));
        dlclose(lib);lib=NULL; return 0;
    }
    // Check that at least one CUDA device is available
    cuDeviceGetCount_t p_cuDeviceGetCount = (cuDeviceGetCount_t)dlsym(lib,"cuDeviceGetCount");
    int nd = 0;
    if (!p_cuDeviceGetCount || p_cuDeviceGetCount(&nd)!=0 || nd <= 0) {
        fprintf(stderr, "[myp GPU] no CUDA-capable device found — falling back to CPU\n");
        dlclose(lib); lib = NULL; return 0;
    }
    if (p_cuDeviceGet) p_cuDeviceGet(&dev, 0);
    int cr = p_cuCtxCreate(&ctx,0,0);
    if (cr != 0) {
        fprintf(stderr, "[myp GPU] cuCtxCreate failed: %s — falling back to CPU\n",
                gpu_err_str(cr));
        dlclose(lib);lib=NULL; return 0;
    }
    avail = 1;
    dev_count = nd;
    dev_initialized = 1;
    fprintf(stderr,"[myp GPU] CUDA initialized (%d device(s))\n", nd);
    return 1;
}

// ---- Device info (valid only after myp_gpu_init succeeds) ----
int myp_gpu_device_count(void) {
    if (!dev_initialized) myp_gpu_init();
    return dev_count;
}
const char* myp_gpu_device_name(void) {
    static char name[256];
    if (!dev_initialized) myp_gpu_init();
    if (!avail || !p_cuDeviceGetName) { name[0]=0; return myp_strdup(name); }
    if (p_cuDeviceGetName(name, 256, dev) != 0) name[0] = 0;
    // M8 约定：返回 string 的 FFI 必须交回“带 ARC 头的计数副本”（myp_strdup），
    // MYP 侧会把返回值当作自有字符串在作用域结束时 myp_release 释放；
    // 直接返回 static 缓冲区会让 myp_release 读到坏 header / free 静态内存 → 崩溃。
    return myp_strdup(name);
}
long myp_gpu_device_memory(void) {
    if (!dev_initialized) myp_gpu_init();
    if (!avail || !p_cuDeviceTotalMem) return 0;
    size_t bytes = 0;
    if (p_cuDeviceTotalMem(&bytes, dev) != 0) return 0;
    return (long)bytes;
}
int myp_gpu_compute_capability(void) {
    if (!dev_initialized) myp_gpu_init();
    if (!avail || !p_cuDeviceGetAttribute) return 0;
    int major = 0, minor = 0;
    // CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR=75, MINOR=76
    p_cuDeviceGetAttribute(&major, 75, dev);
    p_cuDeviceGetAttribute(&minor, 76, dev);
    return major * 100 + minor;
}
int myp_gpu_multi_processors(void) {
    if (!dev_initialized) myp_gpu_init();
    if (!avail || !p_cuDeviceGetAttribute) return 0;
    int n = 0;
    // CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT=16
    if (p_cuDeviceGetAttribute(&n, 16, dev) != 0) return 0;
    return n;
}
int myp_gpu_max_threads_per_block(void) {
    if (!dev_initialized) myp_gpu_init();
    if (!avail || !p_cuDeviceGetAttribute) return 0;
    int n = 0;
    // CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK=1
    if (p_cuDeviceGetAttribute(&n, 1, dev) != 0) return 0;
    return n;
}
int myp_gpu_warp_size(void) {
    if (!dev_initialized) myp_gpu_init();
    if (!avail || !p_cuDeviceGetAttribute) return 0;
    int n = 0;
    // CU_DEVICE_ATTRIBUTE_WARP_SIZE=10
    if (p_cuDeviceGetAttribute(&n, 10, dev) != 0) return 0;
    return n;
}

// ---- §7.4 厂商探测 + 能力查询 ----
// 通用属性查询：CU_DEVICE_ATTRIBUTE_* 的整数 ID（值见下方各函数注释，
// 与 CUDA driver cuda.h 一致）。无 GPU / 查询失败 → 0。
static int gpu_attr(int id) {
    if (!dev_initialized) myp_gpu_init();
    if (!avail || !p_cuDeviceGetAttribute) return 0;
    int n = 0;
    return (p_cuDeviceGetAttribute(&n, id, dev) == 0) ? n : 0;
}

// 厂商名：CUDA 运行期设备恒为 NVIDIA；无 GPU（MYP_GPU 未开/无驱动）→ "cpu"。
// 与 runtime_rocm.c 的 "amd" 组成完整厂商探测（同 ABI，链接期二选一）。
const char* myp_gpu_vendor(void) {
    if (!dev_initialized) myp_gpu_init();
    return avail ? myp_strdup("nvidia") : myp_strdup("cpu");
}

// AMD 专属：GCN 架构字符串（如 "gfx1030"）。CUDA 运行期无此概念 → 空串。
const char* myp_gpu_gfx_arch(void) {
    return myp_strdup("");
}

// 每块最大共享内存（字节）。CU_DEVICE_ATTRIBUTE_MAX_SHARED_MEMORY_PER_BLOCK=8
int myp_gpu_shared_per_block(void) { return gpu_attr(8); }
// 每块最大寄存器数。CU_DEVICE_ATTRIBUTE_MAX_REGISTERS_PER_BLOCK=12
int myp_gpu_regs_per_block(void) { return gpu_attr(12); }
// 最大 grid 尺寸（x 维）。CU_DEVICE_ATTRIBUTE_MAX_GRID_DIM_X=5
int myp_gpu_max_grid_dim(void) { return gpu_attr(5); }
// 最大 block 尺寸（x 维）。CU_DEVICE_ATTRIBUTE_MAX_BLOCK_DIM_X=2
int myp_gpu_max_block_dim(void) { return gpu_attr(2); }
// 核心时钟（MHz）。CU_DEVICE_ATTRIBUTE_CLOCK_RATE=13 单位为 kHz → /1000
int myp_gpu_clock_mhz(void) { return gpu_attr(13) / 1000; }
// 并发内核支持（1=可并发）。CU_DEVICE_ATTRIBUTE_CONCURRENT_KERNELS=31
int myp_gpu_concurrent_kernels(void) { return gpu_attr(31); }
// 向量化访问所需对齐字节（float4/double2=16B，NVIDIA/AMD 均满足）
int myp_gpu_mem_alignment(void) { return 16; }
// 双精度算力：capability>=800（如 A100/H100）视为常规 FP64 → 1；
// sm_70/75 消费卡（RTX）FP64 为 1/32 慢速 → 0（启发式，按 compute capability）。
int myp_gpu_double_precision(void) { return myp_gpu_compute_capability() >= 800 ? 1 : 0; }
// 64 位原子（原子加/交换）：sm_60+ 全局 64 位原子 → 1。
int myp_gpu_atomics64(void) { return myp_gpu_compute_capability() >= 600 ? 1 : 0; }

void* myp_gpu_alloc(size_t sz) {
    if (!avail) return NULL;
    void* p; return (p_cuMemAlloc(&p,sz)==0) ? p : NULL;
}
void myp_gpu_free(void* p) { if(avail&&p) p_cuMemFree(p); }
void myp_gpu_to_device(void* d,const void* s,size_t sz) { if(avail) p_cuMemcpyHtoD(d,s,sz); }
void myp_gpu_to_host(void* d,const void* s,size_t sz) { if(avail) p_cuMemcpyDtoH(d,s,sz); }

// Locate the CUDA libdevice bitcode file (provides __nv_* device math functions).
// Search: $MYP_CUDA_LIBDEVICE, then common CUDA toolkit install paths.
typedef struct { CUmodule mod; CUfunction fn; const char* name; } kernel_t;

// Kernel 缓存：每个唯一的 (PTX, name) 只在首次 launch 时 cuModuleLoadData 一次，
// 之后复用同一模块/函数句柄。
// 背景：编译器为每个 @gpu for 的每次执行都调用 myp_gpu_load_kernel。若每次 launch
// 都新加载模块且从不 cuModuleUnload，设备端模块内存（kernel 代码）会逐次累积——
// 推理只启动几次无感，但训练每样本启动 ~13 个 kernel × 数十万样本 = 数百万次模块
// 加载且永不卸载 → 显存持续暴涨直至 OOM（"显存一直在涨"）。
// 方案：按 (ptx, name) 指针身份缓存（嵌入 PTX 为程序常量，地址稳定），命中即复用；
// 缓存满则回退为「加载 + 用完即卸」，保证不泄漏。缓存条目在进程生命周期内保持有效。
#define MYP_GPU_KERNEL_CACHE_MAX 128
typedef struct { const char* ptx; const char* name; kernel_t* k; } kernel_cache_entry_t;
static kernel_cache_entry_t g_kcache[MYP_GPU_KERNEL_CACHE_MAX];
static int g_kcache_n = 0;

void* myp_gpu_load_kernel(const char* ptx, const char* name) {
    if (!avail) return NULL;
    // cuModuleLoadData 依赖当前线程上下文 TLS；@thread/协程上下文可能因 cuCtxCreate
    // 所在线程而异——强制把 ctx 置为当前（同 cuModuleGetGlobal / cuGraphInstantiate
    // 教训：避免依赖隐式当前上下文）。
    if (p_cuCtxSetCurrent) p_cuCtxSetCurrent(ctx);
    // 缓存命中：嵌入 PTX/name 均为全局常量，指针身份比较即可；strcmp 复核防同址误配。
    for (int i = 0; i < g_kcache_n; i++) {
        if (g_kcache[i].ptx == ptx && g_kcache[i].name == name &&
            strcmp(g_kcache[i].name, name) == 0) {
            return g_kcache[i].k;
        }
    }
    kernel_t* k = (kernel_t*)malloc(sizeof(kernel_t));
    if (!k) return NULL;
    k->name = name;

    // The compiler JIT-links CUDA libdevice into the PTX at compile time, so the
    // PTX is fully self-contained (no external __nv_* references). Just load it.
    CUresult lr = p_cuModuleLoadData(&k->mod, ptx);
    if (!gpu_check_err(lr, "PTX module load", name)) {
        free(k); return NULL;
    }
    CUresult fr = p_cuModuleGetFunction(&k->fn, k->mod, name);
    if (!gpu_check_err(fr, "kernel function lookup", name)) {
        p_cuModuleUnload(k->mod);
        free(k); return NULL;
    }
    // 插入缓存（满了则本 kernel 走「用完即卸」路径，destroy 时 unload）。
    if (g_kcache_n < MYP_GPU_KERNEL_CACHE_MAX) {
        g_kcache[g_kcache_n].ptx = ptx;
        g_kcache[g_kcache_n].name = name;
        g_kcache[g_kcache_n].k = k;
        g_kcache_n++;
    }
    return (void*)k;
}

// 启动 kernel。stream 为 CUstream 句柄（0 = 默认流）。
// stream==0 时保持同步（launch 后 cuCtxSynchronize，兼容现状）；
// stream!=0 时异步排队到该流（调用方须在读取结果前 streamSync）。
int myp_gpu_launch(void* kctx, unsigned int gx, unsigned int bx, void** args, unsigned int n, long stream) {
    if (!avail||!kctx) return 0;
    kernel_t* k = (kernel_t*)kctx;
    (void)n;
    // §5.1 per-kernel 计时：MYP_PROF_GPU=1 时用单调时钟量同步 launch 耗时（
    // stream==0 同步路径 launch 阻塞到完成，host 时钟已足够精确）。
    static int prof = -1;
    if (prof == -1) { const char* e = getenv("MYP_PROF_GPU"); prof = (e && e[0]=='1') ? 1 : 0; }
    // 逐 kernel 日志默认静默（异步流模式 216 kernel/步，打印会成为 host 瓶颈）；
    // MYP_GPU_LOG=1 时输出，供调试。
    static int logv = -1;
    if (logv == -1) { const char* e = getenv("MYP_GPU_LOG"); logv = (e && e[0]=='1') ? 1 : 0; }
    struct timespec t0, t1;
    if (prof && stream == 0) clock_gettime(CLOCK_MONOTONIC, &t0);
    if (logv) fprintf(stderr, "[myp GPU] launching kernel grid=%u block=%u stream=%ld\n", gx, bx, stream);
    int r = p_cuLaunchKernel(k->fn, gx,1,1, bx,1,1, 0, (CUstream)(intptr_t)stream, args, NULL);
    if (r != 0) {
        char what[96];
        snprintf(what, sizeof(what), "cuLaunchKernel grid=%u block=%u", gx, bx);
        gpu_check_err(r, what, k->name);
        return 0;
    }
    if (stream == 0) {
        r = p_cuCtxSynchronize();
        if (!gpu_check_err(r, "cuCtxSynchronize", k->name)) return 0;
        if (prof) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 +
                        (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
            if (logv) fprintf(stderr, "[myp GPU] kernel done: %.3f ms\n", ms);
        } else if (logv) {
            fprintf(stderr, "[myp GPU] kernel done\n");
        }
    } else if (logv) {
        fprintf(stderr, "[myp GPU] kernel queued async on stream\n");
    }
    return 1;
}

// 异步设备 → 主机（排队到流；须 streamSync 后取回）。@gpu for/tile stream 模式
// 的捕获数组回拷用（kernel 异步时同步 D2H 会读未完成数据）。
void myp_gpu_to_host_async(void* d, const void* s, size_t sz, long stream) {
    if (!avail || !p_cuMemcpyDtoHAsync || sz == 0) return;
    p_cuMemcpyDtoHAsync(d, s, sz, (CUstream)(intptr_t)stream);
}

void myp_gpu_destroy_kernel(void* kctx) {
    if (!avail||!kctx) return;
    kernel_t* k = (kernel_t*)kctx;
    // 缓存条目进程生命周期内保持有效（复用同一模块，避免重复加载与泄漏）。
    for (int i = 0; i < g_kcache_n; i++) {
        if (g_kcache[i].k == k) return;
    }
    // 未缓存（缓存满的回退路径）：必须 cuModuleUnload 释放设备模块内存，
    // 否则每次 launch 都泄漏模块显存（旧版只 free 宿主结构 → 训练显存暴涨）。
    if (p_cuModuleUnload) p_cuModuleUnload(k->mod);
    free(k);
}

// ============================================================================
// §P6 ③ BYOC：自定义 PTX 内核加载与启动（宿主侧 FFI）。
// 约定：PTX 必须自包含（编译期已 JIT 链接 libdevice）；参数以 long[] 传递，
// 每项 8 字节——设备指针直接放指针值，标量放数值（double 按位型放 long）。
// ============================================================================

// 加载自定义 PTX 模块中的 kernel，返回内核句柄（0 = 失败）。
long myp_gpu_byoc_load(const char* ptx, const char* name) {
    if (!avail || !ptx || !name) return 0;
    void* k = myp_gpu_load_kernel(ptx, name);
    return (long)(intptr_t)k;
}

// 启动自定义内核：args 为 host long[]（每项 8B），n 为参数个数。
// 内部构造 void** 指向各 8B 参数存储后交给 cuLaunchKernel。
// stream==0 同步，!=0 异步排队。返回 1 成功 / 0 失败。
int myp_gpu_byoc_launch(long kctx, int grid, int block, const long* args, int n, long stream) {
    if (!avail || kctx == 0 || !args || n <= 0) return 0;
    if (n > 64) return 0;  // cuLaunchKernel 参数上限
    void** ptrs = (void**)alloca((size_t)n * sizeof(void*));
    for (int i = 0; i < n; i++) ptrs[i] = (void*)&args[i];
    return myp_gpu_launch((void*)(intptr_t)kctx, (unsigned int)grid, (unsigned int)block,
                          ptrs, (unsigned int)n, stream);
}

// §P5 ② kernel printk/assert staging：runtime 分配设备缓冲 + 计数器，kernel 以
// 附加参数（i64 设备指针）直接持有——避免 cuModuleGetGlobal（其依赖当前上下文
// TLS，在 MYP 协程/@thread 下不可靠 → CUDA_ERROR_INVALID_CONTEXT）。
//
// 记录布局（7×i64 = 56B）：[0]type(0=printk/1=assert) [1]fmt_id [2]gid
//   [3..5]a0..a2（int=zext i64 / double=位型 i64） [6]mask（bit_i=1 表示 a_i 为 double）
// 宿主 mini-printf：按格式串文本 + 每转换符消费一个 arg（int/double 由 mask 定）。
// assert 失败 → 打印并 exit(1)（契约违约，同 scatter unique）。
#define MYP_PF_REC 7
#define MYP_PF_MAX 1024
static void* gpu_pf_buf = NULL;
static void* gpu_pf_cnt = NULL;
static void* gpu_pf_fail = NULL;
static int gpu_pf_inited = 0;
static void myp_gpu_printf_ensure(void) {
    if (gpu_pf_inited) return;
    gpu_pf_inited = 1;
    if (!avail) return;
    if (p_cuMemAlloc(&gpu_pf_buf, (size_t)MYP_PF_MAX * MYP_PF_REC * 8) != 0) gpu_pf_buf = NULL;
    if (p_cuMemAlloc(&gpu_pf_cnt, 8) != 0) gpu_pf_cnt = NULL;
    if (p_cuMemAlloc(&gpu_pf_fail, 8) != 0) gpu_pf_fail = NULL;
}
long myp_gpu_printf_buf(void)  { myp_gpu_printf_ensure(); return (long)(intptr_t)gpu_pf_buf; }
long myp_gpu_printf_cnt(void)  { myp_gpu_printf_ensure(); return (long)(intptr_t)gpu_pf_cnt; }
long myp_gpu_printf_fail(void) { myp_gpu_printf_ensure(); return (long)(intptr_t)gpu_pf_fail; }

void myp_gpu_flush_printf(long pbuf, long pcnt, long pfail,
                          const char** fmts, int nfmt) {
    if (!avail || !pcnt) return;
    long long cnt = 0, fail = 0;
    p_cuMemcpyDtoH(&cnt, (void*)(intptr_t)pcnt, 8);
    if (pfail) p_cuMemcpyDtoH(&fail, (void*)(intptr_t)pfail, 8);
    if (cnt < 0) cnt = 0;
    if (cnt > MYP_PF_MAX) cnt = MYP_PF_MAX;
    if (cnt > 0 && pbuf) {
        long long* rec = (long long*)calloc((size_t)cnt * MYP_PF_REC, sizeof(long long));
        if (rec) {
            p_cuMemcpyDtoH(rec, (void*)(intptr_t)pbuf,
                           (size_t)cnt * MYP_PF_REC * sizeof(long long));
            for (long long i = 0; i < cnt; i++) {
                long long* r = rec + i * MYP_PF_REC;
                long long type = r[0], fid = r[1], gid = r[2], mask = r[6];
                const char* fmt = (fid >= 0 && fid < (long long)nfmt) ? fmts[fid] : "(?)";
                if (type == 1)
                    fprintf(stdout, "kernel[gid=%lld] ASSERT FAIL: ", (long long)gid);
                else
                    fprintf(stdout, "kernel[gid=%lld] ", (long long)gid);
                int ai = 0;
                const long long* av = &r[3];
                for (const char* p = fmt; *p; p++) {
                    if (*p != '%') { fputc(*p, stdout); continue; }
                    p++;
                    if (*p == '%') { fputc('%', stdout); continue; }
                    if (*p == 'l') p++;   // %ld/%lf → 跳过长修饰
                    if (ai > 2) { fputs("%?", stdout); continue; }
                    int is_double = (int)((mask >> ai) & 1LL);
                    if (is_double) {
                        double d; memcpy(&d, &av[ai], 8);
                        fprintf(stdout, "%g", d);
                    } else {
                        fprintf(stdout, "%lld", (long long)av[ai]);
                    }
                    ai++;
                }
                fputc('\n', stdout);
            }
            free(rec);
        }
    }
    // 重置计数器与失败标志（下个 kernel 从 0 起）
    if (pcnt) { long long z = 0; p_cuMemcpyHtoD((void*)(intptr_t)pcnt, &z, 8); }
    if (pfail) { long long z = 0; p_cuMemcpyHtoD((void*)(intptr_t)pfail, &z, 8); }
    if (fail) {
        fprintf(stderr, "[myp GPU] @gpu kernel ASSERT FAILED — aborting "
                        "(device-side assert, §P5 ②)\n");
        exit(1);
    }
}

// §8.4 @gpu scatter(unique) 索引预扫失败（越界 / 重复）→ 打印并退出。
// 契约违约（unique 模式要求 idx 无重复且 0≤idx<len(b)），而非可回退的降级，
// 故直接终止而非返回（返回会静默产生未定义结果，违背"冲突必须显式声明"）。
__attribute__((noreturn)) void myp_gpu_scatter_check_fail(const char* msg) {
    fprintf(stderr, "[myp GPU] @gpu scatter(unique) index check failed: %s\n",
            msg ? msg : "unknown");
    exit(1);
}

// ============================================================================
// M1 显式显存 / 流 FFI（范式库 stdlib/gpu 使用）
// 约定：设备内存句柄统一用 long（指针按位宽安全地转成整数），MYP 侧不暴露裸指针。
// ============================================================================

// 分配设备内存，返回句柄（0 = 失败 / GPU 不可用）。
long myp_gpu_alloc_handle(long bytes) {
    if (!avail || bytes <= 0) return 0;
    void* p = myp_gpu_alloc((size_t)bytes);
    if (!p) return 0;
    return (long)(intptr_t)p;
}

// 释放设备内存（0 = 失败 / 不可用）。
int myp_gpu_free_handle(long dev) {
    if (!avail || dev == 0) return 0;
    p_cuMemFree((void*)(intptr_t)dev);
    return 1;
}

// 主机 double[] → 设备（元素偏移；len 为元素个数）。返回 0 失败。
int myp_gpu_copy_h2d_d(long dev, const double* host, int srcOff, int dstOff, int len) {
    if (!avail || dev == 0 || !host || len <= 0) return 0;
    void* d = (char*)(intptr_t)dev + (size_t)dstOff * sizeof(double);
    const void* s = (const char*)host + (size_t)srcOff * sizeof(double);
    p_cuMemcpyHtoD(d, s, (size_t)len * sizeof(double));
    return 1;
}

// 设备 → 主机 double[]（元素偏移）。返回 0 失败。
int myp_gpu_copy_d2h_d(double* host, long dev, int srcOff, int dstOff, int len) {
    if (!avail || dev == 0 || !host || len <= 0) return 0;
    void* s = (char*)(intptr_t)dev + (size_t)srcOff * sizeof(double);
    void* d = (char*)host + (size_t)dstOff * sizeof(double);
    p_cuMemcpyDtoH(d, s, (size_t)len * sizeof(double));
    return 1;
}

// 主机 float[] → 设备（float 变体，元素 4 字节）。
int myp_gpu_copy_h2d_f(long dev, const float* host, int srcOff, int dstOff, int len) {
    if (!avail || dev == 0 || !host || len <= 0) return 0;
    void* d = (char*)(intptr_t)dev + (size_t)dstOff * sizeof(float);
    const void* s = (const char*)host + (size_t)srcOff * sizeof(float);
    p_cuMemcpyHtoD(d, s, (size_t)len * sizeof(float));
    return 1;
}

// 设备 → 主机 float[]（float 变体）。
int myp_gpu_copy_d2h_f(float* host, long dev, int srcOff, int dstOff, int len) {
    if (!avail || dev == 0 || !host || len <= 0) return 0;
    void* s = (char*)(intptr_t)dev + (size_t)srcOff * sizeof(float);
    void* d = (char*)host + (size_t)dstOff * sizeof(float);
    p_cuMemcpyDtoH(d, s, (size_t)len * sizeof(float));
    return 1;
}

// 设备 → 设备（按字节偏移）。返回 0 失败。
int myp_gpu_copy_d2d(long dst, long dstOff, long src, long srcOff, long bytes) {
    if (!avail || dst == 0 || src == 0 || bytes <= 0) return 0;
    if (!p_cuMemcpyDtoD) return 0;
    void* d = (char*)(intptr_t)dst + (size_t)dstOff;
    void* s = (char*)(intptr_t)src + (size_t)srcOff;
    p_cuMemcpyDtoD(d, s, (size_t)bytes);
    return 1;
}

// 同步整个上下文（阻塞直到所有内核完成）。返回 0 失败。
int myp_gpu_sync_all(void) {
    if (!avail) return 0;
    return (p_cuCtxSynchronize() == 0) ? 1 : 0;
}

// 创建流，返回句柄（0 = 失败）。流的默认/异步拷贝、异步启动留给 M2。
long myp_gpu_stream_create_h(void) {
    if (!avail || !p_cuStreamCreate) return 0;
    CUstream s = NULL;
    if (p_cuStreamCreate(&s, 0) != 0) return 0;
    return (long)(intptr_t)s;
}

// 同步流。返回 0 失败。
int myp_gpu_stream_sync_h(long s) {
    if (!avail || s == 0 || !p_cuStreamSynchronize) return 0;
    return (p_cuStreamSynchronize((CUstream)(intptr_t)s) == 0) ? 1 : 0;
}

// 销毁流。返回 0 失败。
int myp_gpu_stream_destroy_h(long s) {
    if (!avail || s == 0 || !p_cuStreamDestroy) return 0;
    p_cuStreamDestroy((CUstream)(intptr_t)s);
    return 1;
}

// ============================================================================
// §P6 ② CUDA Graph（图内存）FFI
// 范式：流捕获（cuStreamBeginCapture/EndCapture）→ 图 → 实例化（Exec）→ 重放。
// 捕获模式用 THREAD_LOCAL(1)，与 @thread/协程上下文兼容（同线程内的流操作被捕获）。
// 捕获期间该流上的内核启动与异步拷贝会被记录成图；重放时无需再经过启动路径。
// 注意：图捕获只能记录「持久化」的资源操作；内核若每次 launch 重新 new[] 分配，
// 其指针会在重放时失效。故图测试须配合 resident() 内核 + GpuBuffer（devicePtr 持久）。
// ============================================================================

// 开始捕获流 s 上的后续操作。返回 1 成功 / 0 失败。
int myp_gpu_graph_capture_begin(long stream) {
    if (!avail || stream == 0 || !p_cuStreamBeginCapture) return 0;
    if (p_cuCtxSetCurrent) p_cuCtxSetCurrent(ctx);
    return (p_cuStreamBeginCapture((CUstream)(intptr_t)stream, 1 /*THREAD_LOCAL*/) == 0) ? 1 : 0;
}

// 结束捕获，返回图句柄（0 = 失败）。
long myp_gpu_graph_capture_end(long stream) {
    if (!avail || stream == 0 || !p_cuStreamEndCapture) return 0;
    if (p_cuCtxSetCurrent) p_cuCtxSetCurrent(ctx);
    CUgraph g = NULL;
    int r = p_cuStreamEndCapture((CUstream)(intptr_t)stream, &g);
    if (r != 0) {
        fprintf(stderr, "[myp GPU] cuStreamEndCapture failed: %s\n", gpu_err_str(r));
        return 0;
    }
    if (g == NULL) {
        fprintf(stderr, "[myp GPU] cuStreamEndCapture returned null graph\n");
        return 0;
    }
    return (long)(intptr_t)g;
}

// 实例化图 → 可执行图（exec），返回 exec 句柄（0 = 失败）。
long myp_gpu_graph_instantiate(long graph) {
    if (!avail || graph == 0 || !p_cuGraphInstantiate) return 0;
    // cuGraphInstantiate 依赖当前线程上下文 TLS；协程/@thread 上下文可能因
    // cuCtxCreate 所在线程而异——强制把 ctx 置为当前再实例化（同 cuModuleGetGlobal
    // 教训：避免依赖隐式当前上下文）。
    if (p_cuCtxSetCurrent) p_cuCtxSetCurrent(ctx);
    CUgraphExec e = NULL;
    int r = p_cuGraphInstantiate(&e, (CUgraph)(intptr_t)graph, 0ULL);
    if (r != 0) {
        fprintf(stderr, "[myp GPU] cuGraphInstantiate failed: %s\n", gpu_err_str(r));
        return 0;
    }
    return (long)(intptr_t)e;
}

// 在流上重放可执行图。返回 1 成功 / 0 失败。
int myp_gpu_graph_launch(long exec, long stream) {
    if (!avail || exec == 0 || !p_cuGraphLaunch) return 0;
    if (p_cuCtxSetCurrent) p_cuCtxSetCurrent(ctx);
    return (p_cuGraphLaunch((CUgraphExec)(intptr_t)exec, (CUstream)(intptr_t)stream) == 0) ? 1 : 0;
}

// 销毁图（重复调用安全）。
void myp_gpu_graph_destroy(long graph) {
    if (graph == 0 || !p_cuGraphDestroy) return;
    p_cuGraphDestroy((CUgraph)(intptr_t)graph);
}

// 销毁可执行图（重复调用安全）。
void myp_gpu_graph_exec_destroy(long exec) {
    if (exec == 0 || !p_cuGraphExecDestroy) return;
    p_cuGraphExecDestroy((CUgraphExec)(intptr_t)exec);
}

// ============================================================================
// M2 异步拷贝 / 事件 FFI（范式库 stdlib/gpu/stream.myp 使用）
// 约定：流/事件句柄用 long；异步拷贝在指定流上排队，需 streamSync 后才可靠取回
// （主机数组须在流执行完成前保持有效）。
// ============================================================================

// 主机 double[] → 设备（异步，指定流）
int myp_gpu_copy_h2d_async_d(long dev, const double* host, int srcOff, int dstOff, int len, long stream) {
    if (!avail || dev == 0 || !host || len <= 0 || !p_cuMemcpyHtoDAsync) return 0;
    void* d = (char*)(intptr_t)dev + (size_t)dstOff * sizeof(double);
    const void* s = (const char*)host + (size_t)srcOff * sizeof(double);
    p_cuMemcpyHtoDAsync(d, s, (size_t)len * sizeof(double), (CUstream)(intptr_t)stream);
    return 1;
}

// 设备 → 主机 double[]（异步，指定流）
int myp_gpu_copy_d2h_async_d(double* host, long dev, int srcOff, int dstOff, int len, long stream) {
    if (!avail || dev == 0 || !host || len <= 0 || !p_cuMemcpyDtoHAsync) return 0;
    void* s = (char*)(intptr_t)dev + (size_t)srcOff * sizeof(double);
    void* d = (char*)host + (size_t)dstOff * sizeof(double);
    p_cuMemcpyDtoHAsync(d, s, (size_t)len * sizeof(double), (CUstream)(intptr_t)stream);
    return 1;
}

// 主机 float[] → 设备（异步）
int myp_gpu_copy_h2d_async_f(long dev, const float* host, int srcOff, int dstOff, int len, long stream) {
    if (!avail || dev == 0 || !host || len <= 0 || !p_cuMemcpyHtoDAsync) return 0;
    void* d = (char*)(intptr_t)dev + (size_t)dstOff * sizeof(float);
    const void* s = (const char*)host + (size_t)srcOff * sizeof(float);
    p_cuMemcpyHtoDAsync(d, s, (size_t)len * sizeof(float), (CUstream)(intptr_t)stream);
    return 1;
}

// 设备 → 主机 float[]（异步）
int myp_gpu_copy_d2h_async_f(float* host, long dev, int srcOff, int dstOff, int len, long stream) {
    if (!avail || dev == 0 || !host || len <= 0 || !p_cuMemcpyDtoHAsync) return 0;
    void* s = (char*)(intptr_t)dev + (size_t)srcOff * sizeof(float);
    void* d = (char*)host + (size_t)dstOff * sizeof(float);
    p_cuMemcpyDtoHAsync(d, s, (size_t)len * sizeof(float), (CUstream)(intptr_t)stream);
    return 1;
}

// 设备 → 设备（异步，字节偏移）
int myp_gpu_copy_d2d_async(long dst, long dstOff, long src, long srcOff, long bytes, long stream) {
    if (!avail || dst == 0 || src == 0 || bytes <= 0 || !p_cuMemcpyDtoDAsync) return 0;
    void* d = (char*)(intptr_t)dst + (size_t)dstOff;
    void* s = (char*)(intptr_t)src + (size_t)srcOff;
    p_cuMemcpyDtoDAsync(d, s, (size_t)bytes, (CUstream)(intptr_t)stream);
    return 1;
}

// 创建事件（计时可用，flags=0 = CU_EVENT_DEFAULT 开启计时）。返回句柄（0 = 失败）。
long myp_gpu_event_create_h(void) {
    if (!avail || !p_cuEventCreate) return 0;
    CUevent e = NULL;
    if (p_cuEventCreate(&e, 0) != 0) return 0;
    return (long)(intptr_t)e;
}

// 在流上记录事件（流为 0 表示默认流）。返回 0 失败。
int myp_gpu_event_record_h(long ev, long stream) {
    if (!avail || ev == 0 || !p_cuEventRecord) return 0;
    return (p_cuEventRecord((CUevent)(intptr_t)ev, (CUstream)(intptr_t)stream) == 0) ? 1 : 0;
}

// 让流等待事件（跨流依赖）。返回 0 失败。
int myp_gpu_event_wait_h(long ev, long stream) {
    if (!avail || ev == 0 || stream == 0 || !p_cuStreamWaitEvent) return 0;
    return (p_cuStreamWaitEvent((CUstream)(intptr_t)stream, (CUevent)(intptr_t)ev, 0) == 0) ? 1 : 0;
}

// 阻塞等待事件完成。返回 0 失败。
int myp_gpu_event_sync_h(long ev) {
    if (!avail || ev == 0 || !p_cuEventSynchronize) return 0;
    return (p_cuEventSynchronize((CUevent)(intptr_t)ev) == 0) ? 1 : 0;
}

// 两事件间隔毫秒（需均已完成）。返回毫秒。
double myp_gpu_event_elapsed_ms(long evStart, long evEnd) {
    if (!avail || evStart == 0 || evEnd == 0 || !p_cuEventElapsedTime) return 0.0;
    float ms = 0.0f;
    if (p_cuEventElapsedTime(&ms, (CUevent)(intptr_t)evStart, (CUevent)(intptr_t)evEnd) != 0) return 0.0;
    return (double)ms;
}

// 销毁事件。返回 0 失败。
int myp_gpu_event_destroy_h(long ev) {
    if (!avail || ev == 0 || !p_cuEventDestroy) return 0;
    p_cuEventDestroy((CUevent)(intptr_t)ev);
    return 1;
}
