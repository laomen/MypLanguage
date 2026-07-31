// runtime_gpu.c — CUDA GPU offload runtime
#include "mylang/runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
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
typedef int (*cuCtxSynchronize_t)(void);

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
static cuCtxSynchronize_t p_cuCtxSynchronize = NULL;
static int avail = 0;
static CUcontext ctx = NULL;

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
    p_cuCtxSynchronize = (cuCtxSynchronize_t)dlsym(lib,"cuCtxSynchronize");
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
    if (p_cuCtxCreate(&ctx,0,0)!=0) { dlclose(lib);lib=NULL; return 0; }
    avail = 1;
    fprintf(stderr,"[myp GPU] CUDA initialized (%d device(s))\n", nd);
    return 1;
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

int myp_gpu_launch(void* kctx, unsigned int gx, unsigned int bx, void** args, unsigned int n) {
    if (!avail||!kctx) return 0;
    kernel_t* k = (kernel_t*)kctx;
    (void)n;
    fprintf(stderr, "[myp GPU] launching kernel grid=%u block=%u\n", gx, bx);
    int r = p_cuLaunchKernel(k->fn, gx,1,1, bx,1,1, 0,NULL, args,NULL);
    if (r!=0) { fprintf(stderr,"[myp GPU] cuLaunchKernel failed: %d\n", r); return 0; }
    r = p_cuCtxSynchronize();
    if (r!=0) { fprintf(stderr,"[myp GPU] cuCtxSynchronize failed: %d\n", r); return 0; }
    fprintf(stderr, "[myp GPU] kernel done\n");
    return 1;
}

void myp_gpu_destroy_kernel(void* kctx) {
    if (!avail||!kctx) return;
    kernel_t* k = (kernel_t*)kctx;
    free(k);
}
