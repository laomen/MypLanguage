// runtime_rocm.c — AMD ROCm/HIP GPU offload runtime（§6.5 P4 跨厂商）
//
// 镜像 runtime_gpu.c（CUDA driver FFI），用 HIP API（dlopen libamdhip64）。
// 与 codegen 的 myp_gpu_* ABI 完全一致 → 同一份 @gpu for 内核字节（GCN code
// object，MYP_GPU_TARGET=amdgcn 交叉编译发射）可在此加载执行。
//
// 构建：-DMYP_ENABLE_ROCM=ON（需 ROCm 运行期 + AMD 硬件）。无 AMD 硬件时保持
// OFF —— 跨厂商验证走"MYP_GPU_TARGET=amdgcn 编译期 GCN 发射 + CPU 回退语义"
//（tests/cross_compile_amd.sh，§9.5 ⑤）。本文件在未启用时不被编译。
//
// HIP 函数映射（§6.5 cu→hip 表）：
//   cuModuleLoadData      → hipModuleLoadData
//   cuModuleGetFunction   → hipModuleGetFunction
//   cuLaunchKernel        → hipLaunchKernel
//   cuCtxSynchronize      → hipDeviceSynchronize
//   cuMemAlloc/cuMemFree  → hipMalloc/hipFree
//   cuMemcpyHtoD/DtoH/DtoD→ hipMemcpy(+方向)
//   cuMemcpy*Async        → hipMemcpyAsync
//   cuStreamCreate/…      → hipStreamCreate/…
//   cuEventCreate/…       → hipEventCreate/…
//   cuDeviceGetAttribute  → hipGetDeviceProperties
#ifdef MYP_ENABLE_ROCM

#include "mylang/runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
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
#endif

// HIP 句柄类型（不依赖 hip_runtime.h，用前向声明 + 不透明指针）
typedef struct hipCtx_t* hipContext;
typedef struct hipModule_t* hipModule;
typedef struct hipFunction_t* hipFunction;
typedef struct hipStream_t* hipStream;
typedef struct hipEvent_t* hipEvent;
typedef struct hipDeviceProp_t* hipDeviceProp;

#define HIP_MEMCPY_HOST_TO_DEVICE 0
#define HIP_MEMCPY_DEVICE_TO_HOST 1
#define HIP_MEMCPY_DEVICE_TO_DEVICE 2

typedef int (*hipInit_t)(unsigned int);
typedef int (*hipGetDeviceCount_t)(int*);
typedef int (*hipDeviceGetAttribute_t)(int*, int, int);
typedef int (*hipDeviceProp_tfn)(hipDeviceProp, int);
typedef int (*hipModuleGetGlobal_t)(void**, size_t*, hipModule, const char*);
typedef int (*hipMalloc_t)(void**, size_t);
typedef int (*hipFree_t)(void*);
typedef int (*hipMemcpy_t)(void*, const void*, size_t, int);
typedef int (*hipMemcpyAsync_t)(void*, const void*, size_t, int, hipStream);
typedef int (*hipDeviceSynchronize_t)(void);
typedef int (*hipModuleLoadData_t)(hipModule*, const void*);
typedef int (*hipModuleGetFunction_t)(hipFunction*, hipModule, const char*);
typedef int (*hipLaunchKernel_t)(hipFunction, unsigned, unsigned, unsigned,
    unsigned, unsigned, unsigned, unsigned, void**, void**);
typedef int (*hipStreamCreate_t)(hipStream*);
typedef int (*hipStreamSynchronize_t)(hipStream);
typedef int (*hipStreamDestroy_t)(hipStream);
typedef int (*hipEventCreate_t)(hipEvent*);
typedef int (*hipEventRecord_t)(hipEvent, hipStream);
typedef int (*hipEventSynchronize_t)(hipEvent);
typedef int (*hipEventElapsedTime_t)(float*, hipEvent, hipEvent);
typedef int (*hipEventDestroy_t)(hipEvent);
typedef int (*hipStreamWaitEvent_t)(hipStream, hipEvent, unsigned);

static void* lib = NULL;
static hipInit_t p_hipInit = NULL;
static hipGetDeviceCount_t p_hipGetDeviceCount = NULL;
static hipDeviceGetAttribute_t p_hipDeviceGetAttribute = NULL;
static hipMalloc_t p_hipMalloc = NULL;
static hipFree_t p_hipFree = NULL;
static hipMemcpy_t p_hipMemcpy = NULL;
static hipMemcpyAsync_t p_hipMemcpyAsync = NULL;
static hipDeviceSynchronize_t p_hipDeviceSynchronize = NULL;
static hipModuleLoadData_t p_hipModuleLoadData = NULL;
static hipModuleGetFunction_t p_hipModuleGetFunction = NULL;
static hipModuleGetGlobal_t p_hipModuleGetGlobal = NULL;
static hipLaunchKernel_t p_hipLaunchKernel = NULL;
static hipStreamCreate_t p_hipStreamCreate = NULL;
static hipStreamSynchronize_t p_hipStreamSynchronize = NULL;
static hipStreamDestroy_t p_hipStreamDestroy = NULL;
static hipEventCreate_t p_hipEventCreate = NULL;
static hipEventRecord_t p_hipEventRecord = NULL;
static hipEventSynchronize_t p_hipEventSynchronize = NULL;
static hipEventElapsedTime_t p_hipEventElapsedTime = NULL;
static hipEventDestroy_t p_hipEventDestroy = NULL;
static hipStreamWaitEvent_t p_hipStreamWaitEvent = NULL;

static int avail = 0;

// HIP 错误码 → 可读字符串（§5.2 错误友好化；GCN code object 加载/launch 诊断）。
static const char* rocm_err_str(int e) {
    switch (e) {
        case 0: return "hipSuccess";
        case 1: return "hipErrorInvalidValue";
        case 2: return "hipErrorOutOfMemory";
        case 3: return "hipErrorNotInitialized";
        case 101: return "hipErrorInvalidDevice";
        case 209: return "hipErrorNoBinaryForGpu";
        case 700: return "hipErrorInvalidKernelFile";
        default: return "hipErrorUnknown";
    }
}

int myp_gpu_init(void) {
    if (avail) return 1;
    if (getenv("MYP_GPU") && getenv("MYP_GPU")[0] != '1') return 0;  // 显式禁用
#if defined(_WIN32)
    const char* hip_lib = "amdhip64.dll";
    int dlflags = 0;
#else
    const char* hip_lib = "libamdhip64.so";
    int dlflags = RTLD_NOW | RTLD_GLOBAL;
#endif
    lib = dlopen(hip_lib, dlflags);
    if (!lib) {
        fprintf(stderr, "[myp ROCm] MYP_GPU=1 but cannot load libamdhip64.so "
                "(ROCm not installed) — falling back to CPU\n");
        return 0;
    }
#define LOAD(name) p_##name = (name##_t)dlsym(lib, #name)
    LOAD(hipInit); LOAD(hipGetDeviceCount); LOAD(hipDeviceGetAttribute);
    LOAD(hipMalloc); LOAD(hipFree);
    LOAD(hipMemcpy); LOAD(hipMemcpyAsync); LOAD(hipDeviceSynchronize);
    LOAD(hipModuleLoadData); LOAD(hipModuleGetFunction); LOAD(hipModuleGetGlobal); LOAD(hipLaunchKernel);
    LOAD(hipStreamCreate); LOAD(hipStreamSynchronize); LOAD(hipStreamDestroy);
    LOAD(hipEventCreate); LOAD(hipEventRecord); LOAD(hipEventSynchronize);
    LOAD(hipEventElapsedTime); LOAD(hipEventDestroy); LOAD(hipStreamWaitEvent);
#undef LOAD
    if (!p_hipInit || !p_hipMalloc || !p_hipModuleLoadData || !p_hipLaunchKernel) {
        fprintf(stderr, "[myp ROCm] HIP symbols missing — falling back to CPU\n");
        return 0;
    }
    if (p_hipInit(0) != 0) {
        fprintf(stderr, "[myp ROCm] hipInit failed — falling back to CPU\n");
        return 0;
    }
    int nd = 0;
    if (p_hipGetDeviceCount(&nd) != 0 || nd <= 0) {
        fprintf(stderr, "[myp ROCm] no AMD device found — falling back to CPU\n");
        return 0;
    }
    avail = 1;
    fprintf(stderr, "[myp ROCm] ROCm initialized (%d device(s))\n", nd);
    return 1;
}

typedef struct { hipFunction fn; } kernel_t;

void* myp_gpu_alloc(size_t sz) {
    if (!avail || sz == 0) return NULL;
    void* p = NULL;
    if (p_hipMalloc(&p, sz) != 0) return NULL;
    return p;
}
void myp_gpu_free(void* p) { if (avail && p) p_hipFree(p); }
void myp_gpu_to_device(void* d, const void* s, size_t sz) {
    if (avail && sz > 0) p_hipMemcpy(d, s, sz, HIP_MEMCPY_HOST_TO_DEVICE);
}
void myp_gpu_to_host(void* d, const void* s, size_t sz) {
    if (avail && sz > 0) p_hipMemcpy(d, s, sz, HIP_MEMCPY_DEVICE_TO_HOST);
}
void myp_gpu_to_host_async(void* d, const void* s, size_t sz, long stream) {
    if (avail && p_hipMemcpyAsync && sz > 0)
        p_hipMemcpyAsync(d, s, sz, HIP_MEMCPY_DEVICE_TO_HOST,
                         (hipStream)(intptr_t)stream);
}

// GCN code object（ELF relocatable，MYP_GPU_TARGET=amdgcn 交叉编译发射）加载。
// hipModuleLoadData 接受 ELF code object（与 cuModuleLoadData 吃 PTX 不同——
// AMD 需预编译架构专属 object，§6.4 策略 (a)）。
void* myp_gpu_load_kernel(const void* code, const char* name) {
    if (!avail || !code) return NULL;
    hipModule mod = NULL;
    int r = p_hipModuleLoadData(&mod, code);
    if (r != 0) {
        fprintf(stderr, "[myp ROCm] hipModuleLoadData failed: %s (code object "
                "架构需与设备 gfx 匹配，§6.4)\n", rocm_err_str(r));
        return NULL;
    }
    hipFunction fn = NULL;
    r = p_hipModuleGetFunction(&fn, mod, name);
    if (r != 0) {
        fprintf(stderr, "[myp ROCm] kernel '%s' not found: %s — falling back to CPU\n",
                name, rocm_err_str(r));
        return NULL;
    }
    kernel_t* k = (kernel_t*)calloc(1, sizeof(kernel_t));
    k->fn = fn;
    return k;
}

// stream==0 保持同步；stream!=0 异步排队（同 runtime_gpu.c）。
int myp_gpu_launch(void* kctx, unsigned int gx, unsigned int bx, void** args,
                   unsigned int n, long stream) {
    if (!avail || !kctx) return 0;
    kernel_t* k = (kernel_t*)kctx;
    (void)n;
    struct timespec t0, t1;
    static int prof = -1;
    if (prof == -1) { const char* e = getenv("MYP_PROF_GPU"); prof = (e && e[0]=='1') ? 1 : 0; }
    if (prof && stream == 0) clock_gettime(CLOCK_MONOTONIC, &t0);
    fprintf(stderr, "[myp ROCm] launching kernel grid=%u block=%u stream=%ld\n",
            gx, bx, stream);
    int r = p_hipLaunchKernel(k->fn, gx, 1, 1, bx, 1, 1, 0,
                              (hipStream)(intptr_t)stream, args, NULL);
    if (r != 0) { fprintf(stderr, "[myp ROCm] hipLaunchKernel failed: %s\n",
                          rocm_err_str(r)); return 0; }
    if (stream == 0) {
        r = p_hipDeviceSynchronize();
        if (r != 0) { fprintf(stderr, "[myp ROCm] hipDeviceSynchronize failed: %s\n",
                              rocm_err_str(r)); return 0; }
        if (prof) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 +
                        (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
            fprintf(stderr, "[myp ROCm] kernel done: %.3f ms\n", ms);
        } else {
            fprintf(stderr, "[myp ROCm] kernel done\n");
        }
    } else {
        fprintf(stderr, "[myp ROCm] kernel queued async on stream\n");
    }
    return 1;
}

void myp_gpu_destroy_kernel(void* kctx) {
    if (!avail || !kctx) return;
    kernel_t* k = (kernel_t*)kctx;
    free(k);
}

// §8.4 unique 模式索引校验失败（ABI 一致，供 codegen 调用）
__attribute__((noreturn)) void myp_gpu_scatter_check_fail(const char* msg) {
    fprintf(stderr, "[myp ROCm] @gpu scatter(unique) index check failed: %s\n",
            msg ? msg : "unknown");
    exit(1);
}

// §P5 ② kernel printk/assert staging（HIP 镜像，ABI 与 runtime_gpu.c 一致）：
// runtime 分配设备缓冲 + 计数器，kernel 以附加参数持指针，宿主回读打印。
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
    if (p_hipMalloc(&gpu_pf_buf, (size_t)MYP_PF_MAX * MYP_PF_REC * 8) != 0) gpu_pf_buf = NULL;
    if (p_hipMalloc(&gpu_pf_cnt, 8) != 0) gpu_pf_cnt = NULL;
    if (p_hipMalloc(&gpu_pf_fail, 8) != 0) gpu_pf_fail = NULL;
}
long myp_gpu_printf_buf(void)  { myp_gpu_printf_ensure(); return (long)(intptr_t)gpu_pf_buf; }
long myp_gpu_printf_cnt(void)  { myp_gpu_printf_ensure(); return (long)(intptr_t)gpu_pf_cnt; }
long myp_gpu_printf_fail(void) { myp_gpu_printf_ensure(); return (long)(intptr_t)gpu_pf_fail; }

void myp_gpu_flush_printf(long pbuf, long pcnt, long pfail,
                          const char** fmts, int nfmt) {
    if (!avail || !pcnt) return;
    long long cnt = 0, fail = 0;
    p_hipMemcpy(&cnt, (void*)(intptr_t)pcnt, 8, HIP_MEMCPY_DEVICE_TO_HOST);
    if (pfail) p_hipMemcpy(&fail, (void*)(intptr_t)pfail, 8, HIP_MEMCPY_DEVICE_TO_HOST);
    if (cnt < 0) cnt = 0;
    if (cnt > MYP_PF_MAX) cnt = MYP_PF_MAX;
    if (cnt > 0 && pbuf) {
        long long* rec = (long long*)calloc((size_t)cnt * MYP_PF_REC, sizeof(long long));
        if (rec) {
            p_hipMemcpy(rec, (void*)(intptr_t)pbuf,
                        (size_t)cnt * MYP_PF_REC * sizeof(long long),
                        HIP_MEMCPY_DEVICE_TO_HOST);
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
                    if (*p == 'l') p++;
                    if (ai > 2) { fputs("%?", stdout); continue; }
                    if ((int)((mask >> ai) & 1LL)) {
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
    if (pcnt) { long long z = 0; p_hipMemcpy((void*)(intptr_t)pcnt, &z, 8, HIP_MEMCPY_HOST_TO_DEVICE); }
    if (pfail) { long long z = 0; p_hipMemcpy((void*)(intptr_t)pfail, &z, 8, HIP_MEMCPY_HOST_TO_DEVICE); }
    if (fail) {
        fprintf(stderr, "[myp ROCm] @gpu kernel ASSERT FAILED — aborting (§P5 ②)\n");
        exit(1);
    }
}

// ---- §7.4 厂商探测 + 能力查询（HIP 镜像，ABI 与 runtime_gpu.c 一致）----
// 无 AMD 硬件：编译期验证为主（MYP_ENABLE_ROCM=ON 时本文件参与构建）。
// 属性 ID 与 CUDA CU_DEVICE_ATTRIBUTE_* 对齐（HIP 复用同一套枚举值）。

const char* myp_gpu_vendor(void) { return myp_strdup("amd"); }

// GCN 架构字符串（如 "gfx1030"）：完整实现需 hipGetDeviceProperties 的
// gcnArchName 字段（需 hipDeviceProp_t 布局）。无硬件前返回空串。
const char* myp_gpu_gfx_arch(void) { return myp_strdup(""); }

static int rocm_attr(int id) {
    if (!avail || !p_hipDeviceGetAttribute) return 0;
    int n = 0;
    return (p_hipDeviceGetAttribute(&n, id, 0) == 0) ? n : 0;
}
int myp_gpu_shared_per_block(void)   { return rocm_attr(8); }   // MaxSharedMemoryPerBlock
int myp_gpu_regs_per_block(void)     { return rocm_attr(12); }  // MaxRegistersPerBlock
int myp_gpu_max_grid_dim(void)       { return rocm_attr(5); }   // MaxGridDimX
int myp_gpu_max_block_dim(void)      { return rocm_attr(2); }   // MaxBlockDimX
int myp_gpu_clock_mhz(void)          { return rocm_attr(13) / 1000; }  // ClockRate(kHz)
int myp_gpu_concurrent_kernels(void) { return rocm_attr(31); }  // ConcurrentKernels
int myp_gpu_mem_alignment(void)      { return 16; }             // float4/double2 对齐
int myp_gpu_double_precision(void)   { return 1; }              // CDNA/RDNA FP64 常规
int myp_gpu_atomics64(void)          { return 1; }              // GCN 全局 64 位原子

#endif  // MYP_ENABLE_ROCM
