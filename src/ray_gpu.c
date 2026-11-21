#include "ray.h"

#include <stdint.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#define CL_CALL __stdcall
typedef HMODULE Library;
static Library open_loader(void)
{
    return LoadLibraryExW(L"OpenCL.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}
#define SYMBOL GetProcAddress
#define UNLOAD FreeLibrary
#else
#include <dlfcn.h>
#define CL_CALL
typedef void *Library;
static Library open_loader(void)
{
    return dlopen("libOpenCL.so.1", RTLD_NOW | RTLD_LOCAL);
}
#define SYMBOL dlsym
#define UNLOAD dlclose
#endif

/* OpenCL 1.2 loader ABI for the API subset used here. Handles are opaque. */
typedef uint32_t U32;
typedef int32_t I32;
typedef uint64_t Bits;
typedef struct _cl_platform_id *Platform;
typedef struct _cl_device_id *Device;
typedef struct _cl_context *Context;
typedef struct _cl_command_queue *Queue;
typedef struct _cl_program *Program;
typedef struct _cl_kernel *Kernel;
typedef struct _cl_mem *Buffer;
typedef struct _cl_event *Event;
enum { CL_GPU = 1 << 2, CL_WRITE_ONLY = 1 << 1, CL_READ_ONLY = 1 << 2, CL_COPY_HOST = 1 << 5 };

#define OPENCL_API(F) \
    F(I32, GetPlatformIDs, (U32, Platform *, U32 *)) \
    F(I32, GetDeviceIDs, (Platform, Bits, U32, Device *, U32 *)) \
    F(Context, CreateContext, (const intptr_t *, U32, const Device *, \
      void (CL_CALL *)(const char *, const void *, size_t, void *), void *, I32 *)) \
    F(Queue, CreateCommandQueue, (Context, Device, Bits, I32 *)) \
    F(Program, CreateProgramWithSource, (Context, U32, const char **, const size_t *, I32 *)) \
    F(I32, BuildProgram, (Program, U32, const Device *, const char *, void (CL_CALL *)(Program, void *), void *)) \
    F(Kernel, CreateKernel, (Program, const char *, I32 *)) \
    F(Buffer, CreateBuffer, (Context, Bits, size_t, void *, I32 *)) \
    F(I32, SetKernelArg, (Kernel, U32, size_t, const void *)) \
    F(I32, EnqueueNDRangeKernel, (Queue, Kernel, U32, const size_t *, const size_t *, const size_t *, U32, const Event *, Event *)) \
    F(I32, EnqueueReadBuffer, (Queue, Buffer, U32, size_t, size_t, void *, U32, const Event *, Event *)) \
    F(I32, ReleaseMemObject, (Buffer)) \
    F(I32, ReleaseKernel, (Kernel)) \
    F(I32, ReleaseProgram, (Program)) \
    F(I32, ReleaseCommandQueue, (Queue)) \
    F(I32, ReleaseContext, (Context))

#define DECLARE(result, name, args) typedef result (CL_CALL *Fn##name) args;
OPENCL_API(DECLARE)
#undef DECLARE

static struct {
    Library library;
    Context context;
    Queue queue;
    Kernel kernel;
    bool attempted;
#define FIELD(result, name, args) Fn##name name;
    OPENCL_API(FIELD)
#undef FIELD
} gpu;

#define STRINGIFY_INNER(...) #__VA_ARGS__
#define STRINGIFY(...) STRINGIFY_INNER(__VA_ARGS__)
static const char kernel_source[] =
    "static float slab(global const float *row) {" STRINGIFY(RAY_SLAB_BODY) "}\n"
    "kernel void trace_rays(global const float *rows, global float *hits) {"
    "  size_t i = get_global_id(0); hits[i] = slab(rows + i * 12);"
    "}\n";

static void close_gpu(void)
{
    if (gpu.kernel) gpu.ReleaseKernel(gpu.kernel);
    if (gpu.queue) gpu.ReleaseCommandQueue(gpu.queue);
    if (gpu.context) gpu.ReleaseContext(gpu.context);
    if (gpu.library) UNLOAD(gpu.library);
    gpu.kernel = nullptr;
}

/* Simulation callers run on the session's TUI thread. The context and
   compiled kernel stay warm between tools in the same session. */
static bool init_gpu(void)
{
    if (gpu.attempted) return gpu.kernel != nullptr;
    gpu.attempted = true;
    gpu.library = open_loader();
    if (!gpu.library) return false;
#define LOAD(result, name, args) \
    do { \
        union { Fn##name typed; \
            typeof(SYMBOL(gpu.library, "cl" #name)) symbol; } entry; \
        entry.symbol = SYMBOL(gpu.library, "cl" #name); \
        gpu.name = entry.typed; \
        if (!gpu.name) goto failed; \
    } while (0);
    OPENCL_API(LOAD)
#undef LOAD
    U32 count = 0;
    if (gpu.GetPlatformIDs(0, nullptr, &count) || !count || count > 256) goto failed;
    Platform platforms[256];
    if (gpu.GetPlatformIDs(count, platforms, nullptr)) goto failed;
    Device device = nullptr;
    for (U32 i = 0; i < count; ++i) {
        if (!gpu.GetDeviceIDs(platforms[i], CL_GPU, 1, &device, nullptr)) break;
        device = nullptr;
    }
    if (!device) goto failed;
    gpu.context = gpu.CreateContext(nullptr, 1, &device, nullptr, nullptr, nullptr);
    if (!gpu.context) goto failed;
    gpu.queue = gpu.CreateCommandQueue(gpu.context, device, 0, nullptr);
    if (!gpu.queue) goto failed;
    const char *code = kernel_source;
    Program program = gpu.CreateProgramWithSource(gpu.context, 1, &code, nullptr, nullptr);
    if (!program) goto failed;
    if (!gpu.BuildProgram(program, 1, &device, "-cl-std=CL1.2", nullptr, nullptr))
        gpu.kernel = gpu.CreateKernel(program, "trace_rays", nullptr);
    gpu.ReleaseProgram(program);
    if (!gpu.kernel || atexit(close_gpu)) goto failed;
    return true;
failed:
    close_gpu();
    return false;
}

bool ray_gpu(const float *rays, size_t count, float *hits)
{
    if (!count || !init_gpu()) return false;
    Buffer input = gpu.CreateBuffer(gpu.context, CL_READ_ONLY | CL_COPY_HOST,
                                    count * RAY_VALUES * sizeof(float), (void *)rays, nullptr);
    Buffer output = input ? gpu.CreateBuffer(gpu.context, CL_WRITE_ONLY, count * sizeof(float), nullptr, nullptr) : nullptr;
    bool ok = output &&
        !gpu.SetKernelArg(gpu.kernel, 0, sizeof input, &input) &&
        !gpu.SetKernelArg(gpu.kernel, 1, sizeof output, &output) &&
        !gpu.EnqueueNDRangeKernel(gpu.queue, gpu.kernel, 1, nullptr, &count, nullptr, 0, nullptr, nullptr) &&
        !gpu.EnqueueReadBuffer(gpu.queue, output, 1, 0, count * sizeof(float), hits, 0, nullptr, nullptr);
    if (output) gpu.ReleaseMemObject(output);
    if (input) gpu.ReleaseMemObject(input);
    return ok;
}
