// runtime_lib.c — §P6 ③ BYOC 厂商库 hook：cuBLAS SGEMM（dlopen 惰性加载）。
//
// 设计：
//   · 与 runtime_gpu.c 解耦——这里只做「外部厂商库」互操作演示，不绑定编译期依赖。
//   · dlopen libcublas（尝试常见版本名），全部符号 dlsym；库缺失/无 GPU 时
//     myp_cublas_available() 返回 0，调用方走 CPU 回退，不崩溃。
//   · cublasSgemm 为列主序（column-major）：C[m×n] = alpha·A[m×k]·B[k×n] + beta·C，
//     lda=m, ldb=k, ldc=m。A/B/C 均为设备指针（float 数据）。
//   · handle 缓存在进程内（首次调用 cublasCreate），用毕由进程回收。
//
// FFI（MYP 侧范式库 stdlib/gpu/byoc.myp 使用）：
//   int    myp_cublas_available(void)
//   int    myp_cublas_sgemm(long devA, long devB, long devC,
//                           int m, int n, int k, double alpha, double beta)

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void* cublasHandle_t;
typedef int cublasStatus_t;
typedef int cublasOperation_t;

typedef cublasStatus_t (*cublasCreate_t)(cublasHandle_t*);
typedef cublasStatus_t (*cublasSgemm_t)(cublasHandle_t, cublasOperation_t, cublasOperation_t,
                                        int, int, int,
                                        const float*, const void*, int,
                                        const void*, int,
                                        const float*, void*, int);
typedef cublasStatus_t (*cublasDestroy_t)(cublasHandle_t);

static void* g_cublas_lib = NULL;
static cublasCreate_t  p_cublasCreate  = NULL;
static cublasSgemm_t   p_cublasSgemm   = NULL;
static cublasDestroy_t p_cublasDestroy = NULL;
static cublasHandle_t  g_cublas_handle = NULL;
static int g_cublas_avail = -1;  // -1 未探测

static int cublas_ensure(void) {
    if (g_cublas_avail != -1) return g_cublas_avail;
    g_cublas_avail = 0;
    const char* names[] = {
        "libcublas.so.12", "libcublas.so.13", "libcublas.so", NULL
    };
    for (int i = 0; names[i]; i++) {
        g_cublas_lib = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
        if (g_cublas_lib) break;
    }
    if (!g_cublas_lib) {
        fprintf(stderr, "[myp cublas] libcublas not found — cuBLAS hook disabled\n");
        return 0;
    }
    p_cublasCreate  = (cublasCreate_t)dlsym(g_cublas_lib, "cublasCreate_v2");
    if (!p_cublasCreate) p_cublasCreate = (cublasCreate_t)dlsym(g_cublas_lib, "cublasCreate");
    p_cublasSgemm   = (cublasSgemm_t)dlsym(g_cublas_lib, "cublasSgemm_v2");
    if (!p_cublasSgemm) p_cublasSgemm = (cublasSgemm_t)dlsym(g_cublas_lib, "cublasSgemm");
    p_cublasDestroy = (cublasDestroy_t)dlsym(g_cublas_lib, "cublasDestroy_v2");
    if (!p_cublasDestroy) p_cublasDestroy = (cublasDestroy_t)dlsym(g_cublas_lib, "cublasDestroy");
    if (!p_cublasCreate || !p_cublasSgemm) {
        fprintf(stderr, "[myp cublas] cublasCreate/Sgemm symbols missing — cuBLAS disabled\n");
        return 0;
    }
    if (p_cublasCreate(&g_cublas_handle) != 0) {
        fprintf(stderr, "[myp cublas] cublasCreate failed — cuBLAS disabled\n");
        return 0;
    }
    g_cublas_avail = 1;
    return 1;
}

// 厂商库是否可用（1 = 可用，0 = 不可用）。
int myp_cublas_available(void) {
    return cublas_ensure();
}

// cuBLAS SGEMM（列主序）：C[m×n] = alpha·A[m×k]·B[k×n] + beta·C。
// devA/devB/devC 为设备指针（float 数据）。返回 1 成功 / 0 失败。
// cublasSgemm 默认在设备上同步执行（用默认流），调用返回即可读回。
int myp_cublas_sgemm(long devA, long devB, long devC,
                     int m, int n, int k, double alpha, double beta) {
    if (!cublas_ensure()) return 0;
    if (!devA || !devB || !devC || m <= 0 || n <= 0 || k <= 0) return 0;
    float a = (float)alpha, b = (float)beta;
    // 0 = CUBLAS_OP_N
    cublasStatus_t st = p_cublasSgemm(g_cublas_handle, 0, 0,
                                      m, n, k,
                                      &a, (const void*)(intptr_t)devA, m,
                                      (const void*)(intptr_t)devB, k,
                                      &b, (void*)(intptr_t)devC, m);
    return (st == 0) ? 1 : 0;
}
