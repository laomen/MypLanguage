// runtime_gpu.c — CUDA GPU offload runtime
#include "mylang/runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <dlfcn.h>

typedef int CUresult;
typedef struct CUctx_st* CUcontext;
typedef struct CUmod_st* CUmodule;
typedef struct CUfunc_st* CUfunction;

typedef int (*cuInit_t)(unsigned int);
typedef int (*cuCtxCreate_t)(CUcontext*, unsigned int, int);
typedef int (*cuModuleLoadData_t)(CUmodule*, const void*);
typedef int (*cuModuleGetFunction_t)(CUfunction*, CUmodule, const char*);
typedef int (*cuLaunchKernel_t)(CUfunction, unsigned int,unsigned int,unsigned int,
    unsigned int,unsigned int,unsigned int,unsigned int,void*,void**,void**);
typedef int (*cuMemAlloc_t)(void**, size_t);
typedef int (*cuMemFree_t)(void*);
typedef int (*cuMemcpyHtoD_t)(void*, const void*, size_t);
typedef int (*cuMemcpyDtoH_t)(void*, const void*, size_t);
typedef int (*cuMemcpyDtoD_t)(void*, const void*, size_t);
typedef int (*cuCtxSynchronize_t)(void);
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

static void* lib = NULL;
static cuInit_t p_cuInit = NULL;
static cuCtxCreate_t p_cuCtxCreate = NULL;
static cuModuleLoadData_t p_cuModuleLoadData = NULL;
static cuModuleGetFunction_t p_cuModuleGetFunction = NULL;
static cuLaunchKernel_t p_cuLaunchKernel = NULL;
static cuMemAlloc_t p_cuMemAlloc = NULL;
static cuMemFree_t p_cuMemFree = NULL;
static cuMemcpyHtoD_t p_cuMemcpyHtoD = NULL;
static cuMemcpyDtoH_t p_cuMemcpyDtoH = NULL;
static cuMemcpyDtoD_t p_cuMemcpyDtoD = NULL;
static cuCtxSynchronize_t p_cuCtxSynchronize = NULL;
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

typedef int (*cuDeviceGetCount_t)(int*);
int myp_gpu_init(void) {
    if (avail) return 1;
    // Allow enabling GPU via environment variable MYP_GPU=1
    // Default is CPU (disabled) due to PTX kernel parameter issues
    static int check_env = -1;
    if (check_env == -1) {
        const char* env = getenv("MYP_GPU");
        check_env = (env && env[0] == '1') ? 1 : 0;
    }
    if (!check_env) return 0;
    lib = dlopen("libcuda.so.1", RTLD_LAZY|RTLD_LOCAL);
    if (!lib) return 0;
    p_cuInit = (cuInit_t)dlsym(lib,"cuInit");
    p_cuCtxCreate = (cuCtxCreate_t)dlsym(lib,"cuCtxCreate_v2");
    p_cuModuleLoadData = (cuModuleLoadData_t)dlsym(lib,"cuModuleLoadData");
    p_cuModuleGetFunction = (cuModuleGetFunction_t)dlsym(lib,"cuModuleGetFunction");
    p_cuLaunchKernel = (cuLaunchKernel_t)dlsym(lib,"cuLaunchKernel");
    p_cuMemAlloc = (cuMemAlloc_t)dlsym(lib,"cuMemAlloc_v2");
    p_cuMemFree = (cuMemFree_t)dlsym(lib,"cuMemFree_v2");
    p_cuMemcpyHtoD = (cuMemcpyHtoD_t)dlsym(lib,"cuMemcpyHtoD_v2");
    p_cuMemcpyDtoH = (cuMemcpyDtoH_t)dlsym(lib,"cuMemcpyDtoH_v2");
    p_cuMemcpyDtoD = (cuMemcpyDtoD_t)dlsym(lib,"cuMemcpyDtoD_v2");
    if (!p_cuMemcpyDtoD) p_cuMemcpyDtoD = (cuMemcpyDtoD_t)dlsym(lib,"cuMemcpyDtoD");
    p_cuCtxSynchronize = (cuCtxSynchronize_t)dlsym(lib,"cuCtxSynchronize");
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
    if (!p_cuInit||!p_cuCtxCreate||!p_cuModuleLoadData||!p_cuModuleGetFunction||
        !p_cuLaunchKernel||!p_cuMemAlloc||!p_cuMemFree||!p_cuMemcpyHtoD||
        !p_cuMemcpyDtoH||!p_cuCtxSynchronize) { dlclose(lib);lib=NULL; return 0; }
    if (p_cuInit(0)!=0) { dlclose(lib);lib=NULL; return 0; }
    // Check that at least one CUDA device is available
    cuDeviceGetCount_t p_cuDeviceGetCount = (cuDeviceGetCount_t)dlsym(lib,"cuDeviceGetCount");
    int nd = 0;
    if (!p_cuDeviceGetCount || p_cuDeviceGetCount(&nd)!=0 || nd <= 0) {
        dlclose(lib); lib = NULL; return 0;
    }
    if (p_cuDeviceGet) p_cuDeviceGet(&dev, 0);
    if (p_cuCtxCreate(&ctx,0,0)!=0) { dlclose(lib);lib=NULL; return 0; }
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

void* myp_gpu_alloc(size_t sz) {
    if (!avail) return NULL;
    void* p; return (p_cuMemAlloc(&p,sz)==0) ? p : NULL;
}
void myp_gpu_free(void* p) { if(avail&&p) p_cuMemFree(p); }
void myp_gpu_to_device(void* d,const void* s,size_t sz) { if(avail) p_cuMemcpyHtoD(d,s,sz); }
void myp_gpu_to_host(void* d,const void* s,size_t sz) { if(avail) p_cuMemcpyDtoH(d,s,sz); }

// Locate the CUDA libdevice bitcode file (provides __nv_* device math functions).
// Search: $MYP_CUDA_LIBDEVICE, then common CUDA toolkit install paths.
typedef struct { CUmodule mod; CUfunction fn; } kernel_t;

void* myp_gpu_load_kernel(const char* ptx, const char* name) {
    if (!avail) return NULL;
    kernel_t* k = (kernel_t*)malloc(sizeof(kernel_t));
    if (!k) return NULL;

    // The compiler JIT-links CUDA libdevice into the PTX at compile time, so the
    // PTX is fully self-contained (no external __nv_* references). Just load it.
    if (p_cuModuleLoadData(&k->mod, ptx) != 0) { free(k); return NULL; }

    if (p_cuModuleGetFunction(&k->fn, k->mod, name)!=0) { free(k); return NULL; }
    return (void*)k;
}

// 启动 kernel。stream 为 CUstream 句柄（0 = 默认流）。
// stream==0 时保持同步（launch 后 cuCtxSynchronize，兼容现状）；
// stream!=0 时异步排队到该流（调用方须在读取结果前 streamSync）。
int myp_gpu_launch(void* kctx, unsigned int gx, unsigned int bx, void** args, unsigned int n, long stream) {
    if (!avail||!kctx) return 0;
    kernel_t* k = (kernel_t*)kctx;
    (void)n;
    fprintf(stderr, "[myp GPU] launching kernel grid=%u block=%u stream=%ld\n", gx, bx, stream);
    int r = p_cuLaunchKernel(k->fn, gx,1,1, bx,1,1, 0, (CUstream)(intptr_t)stream, args, NULL);
    if (r!=0) { fprintf(stderr,"[myp GPU] cuLaunchKernel failed: %d\n", r); return 0; }
    if (stream == 0) {
        r = p_cuCtxSynchronize();
        if (r!=0) { fprintf(stderr,"[myp GPU] cuCtxSynchronize failed: %d\n", r); return 0; }
        fprintf(stderr, "[myp GPU] kernel done\n");
    } else {
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
    free(k);
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
