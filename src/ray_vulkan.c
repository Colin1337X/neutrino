#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "ray.h"
#include "neutrino_ray_spv.h"
#ifdef NEUTRINO_VULKAN_RT
#include "neutrino_ray_query_spv.h"
#include <float.h>
#include <math.h>
#endif

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef HMODULE Loader;
#else
#include <dlfcn.h>
typedef void *Loader;
#endif

#define INSTANCE_FUNCTIONS(X) \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) X(CreateDevice) \
    X(GetDeviceProcAddr)
#define DEVICE_FUNCTIONS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
    X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateShaderModule) X(DestroyShaderModule) \
    X(CreateComputePipelines) X(DestroyPipeline) X(CreateBuffer) X(DestroyBuffer) \
    X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) \
    X(MapMemory) X(UnmapMemory) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
    X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateCommandPool) X(DestroyCommandPool) \
    X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdBindPipeline) \
    X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDispatch) X(CmdPipelineBarrier) \
    X(QueueSubmit) X(QueueWaitIdle)
#ifdef NEUTRINO_VULKAN_RT
#define RT_FUNCTIONS(X) \
    X(GetBufferDeviceAddress) X(CreateAccelerationStructureKHR) X(DestroyAccelerationStructureKHR) \
    X(GetAccelerationStructureBuildSizesKHR) X(GetAccelerationStructureDeviceAddressKHR) \
    X(CmdBuildAccelerationStructuresKHR) X(ResetCommandPool)
static const char *const rt_extensions[] = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME
};
static void rt_release_scene(void);
static void rt_release_dispatch(void);
#endif

static struct {
    Loader loader;
    VkInstance instance;
    VkDevice device;
    VkQueue queue;
    VkPhysicalDeviceMemoryProperties memory;
    VkDescriptorSetLayout descriptors;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    uint32_t family;
    int rank;
    bool attempted;
#ifdef NEUTRINO_VULKAN_RT
    bool use_rt;
    uint32_t scratch_alignment;
    VkDescriptorSetLayout rt_descriptors;
    VkPipelineLayout rt_layout;
    VkPipeline rt_pipeline;
    PFN_vkGetPhysicalDeviceFeatures2 features;
    PFN_vkGetPhysicalDeviceProperties2 properties;
    PFN_vkEnumerateDeviceExtensionProperties extensions;
#endif
#define DECLARE(name) PFN_vk##name name;
    INSTANCE_FUNCTIONS(DECLARE)
    DEVICE_FUNCTIONS(DECLARE)
#ifdef NEUTRINO_VULKAN_RT
    RT_FUNCTIONS(DECLARE)
#endif
#undef DECLARE
} vk;

static PFN_vkGetInstanceProcAddr open_loader(void)
{
#ifdef _WIN32
    vk.loader = LoadLibraryExW(L"vulkan-1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    union { FARPROC symbol; PFN_vkGetInstanceProcAddr entry; } loader;
    loader.symbol = vk.loader ? GetProcAddress(vk.loader, "vkGetInstanceProcAddr") : NULL;
    return loader.entry;
#else
    vk.loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    union { void *symbol; PFN_vkGetInstanceProcAddr entry; } loader;
    loader.symbol = vk.loader ? dlsym(vk.loader, "vkGetInstanceProcAddr") : NULL;
    return loader.entry;
#endif
}

static void close_backend(void)
{
#ifdef NEUTRINO_VULKAN_RT
    rt_release_dispatch();
    rt_release_scene();
    if (vk.rt_pipeline) vk.DestroyPipeline(vk.device, vk.rt_pipeline, NULL);
    if (vk.rt_layout) vk.DestroyPipelineLayout(vk.device, vk.rt_layout, NULL);
    if (vk.rt_descriptors) vk.DestroyDescriptorSetLayout(vk.device, vk.rt_descriptors, NULL);
#endif
    if (vk.pipeline) vk.DestroyPipeline(vk.device, vk.pipeline, NULL);
    if (vk.layout) vk.DestroyPipelineLayout(vk.device, vk.layout, NULL);
    if (vk.descriptors) vk.DestroyDescriptorSetLayout(vk.device, vk.descriptors, NULL);
    if (vk.device && vk.DestroyDevice) vk.DestroyDevice(vk.device, NULL);
    if (vk.instance && vk.DestroyInstance) vk.DestroyInstance(vk.instance, NULL);
    if (vk.loader) {
#ifdef _WIN32
        FreeLibrary(vk.loader);
#else
        dlclose(vk.loader);
#endif
    }
    vk.pipeline = VK_NULL_HANDLE;
}

#ifdef NEUTRINO_VULKAN_RT
static bool rt_supported(VkPhysicalDevice physical, uint32_t version)
{
    if (version < VK_API_VERSION_1_2 || !vk.features || !vk.properties || !vk.extensions) return false;
    uint32_t count = 0;
    if (vk.extensions(physical, NULL, &count, NULL) != VK_SUCCESS || !count || count > 4096) return false;
    VkExtensionProperties *extensions = calloc(count, sizeof *extensions);
    if (!extensions) return false;
    bool supported = vk.extensions(physical, NULL, &count, extensions) == VK_SUCCESS;
    for (size_t i = 0; supported && i < sizeof rt_extensions / sizeof *rt_extensions; ++i) {
        supported = false;
        for (uint32_t j = 0; j < count; ++j)
            if (!strcmp(rt_extensions[i], extensions[j].extensionName)) {
                supported = true;
                break;
            }
    }
    free(extensions);
    if (!supported) return false;
    VkPhysicalDeviceRayQueryFeaturesKHR query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR structure = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, .pNext = &query};
    VkPhysicalDeviceBufferDeviceAddressFeatures address = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, .pNext = &structure};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &address};
    vk.features(physical, &features);
    return query.rayQuery && structure.accelerationStructure && address.bufferDeviceAddress;
}
#endif

static VkPhysicalDevice compute_device(void)
{
    uint32_t count = 0;
    if (vk.EnumeratePhysicalDevices(vk.instance, &count, NULL) != VK_SUCCESS || !count || count > 64)
        return VK_NULL_HANDLE;
    VkPhysicalDevice devices[64], best = VK_NULL_HANDLE;
    if (vk.EnumeratePhysicalDevices(vk.instance, &count, devices) != VK_SUCCESS) return VK_NULL_HANDLE;
    vk.rank = -1;
    int best_score = -1;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties properties;
        vk.GetPhysicalDeviceProperties(devices[i], &properties);
        VkPhysicalDeviceType type = properties.deviceType;
        int rank = type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 :
            type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 :
            type == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 1 : 0;
        int score = rank;
#ifdef NEUTRINO_VULKAN_RT
        bool rt = rank >= 2 && rt_supported(devices[i], properties.apiVersion);
        if (rt) score += 4;
#endif
        if (score <= best_score) continue;
        uint32_t families = 0;
        vk.GetPhysicalDeviceQueueFamilyProperties(devices[i], &families, NULL);
        if (!families || families > 256) continue;
        VkQueueFamilyProperties queues[256];
        vk.GetPhysicalDeviceQueueFamilyProperties(devices[i], &families, queues);
        for (uint32_t j = 0; j < families; ++j) {
            if (!queues[j].queueCount || !(queues[j].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
            best = devices[i];
            vk.family = j;
            vk.rank = rank;
            best_score = score;
#ifdef NEUTRINO_VULKAN_RT
            vk.use_rt = rt;
#endif
            break;
        }
    }
    return best;
}

static bool init_backend(void)
{
    if (vk.attempted) return vk.pipeline != VK_NULL_HANDLE;
    vk.attempted = true;
    PFN_vkGetInstanceProcAddr proc = open_loader();
    if (!proc) goto fail;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)proc(VK_NULL_HANDLE, "vkCreateInstance");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "Neutrino", .apiVersion = VK_API_VERSION_1_0};
#ifdef NEUTRINO_VULKAN_RT
    PFN_vkEnumerateInstanceVersion loader_version =
        (PFN_vkEnumerateInstanceVersion)proc(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    uint32_t version;
    if (loader_version && loader_version(&version) == VK_SUCCESS && version >= VK_API_VERSION_1_2)
        app.apiVersion = VK_API_VERSION_1_2;
#endif
    VkInstanceCreateInfo instance = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    if (!create || create(&instance, NULL, &vk.instance) != VK_SUCCESS) goto fail;
#define LOAD_INSTANCE(name) vk.name = (PFN_vk##name)proc(vk.instance, "vk" #name); if (!vk.name) goto fail;
    INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
#ifdef NEUTRINO_VULKAN_RT
    if (app.apiVersion >= VK_API_VERSION_1_2) {
        vk.features = (PFN_vkGetPhysicalDeviceFeatures2)proc(vk.instance, "vkGetPhysicalDeviceFeatures2");
        vk.properties = (PFN_vkGetPhysicalDeviceProperties2)proc(vk.instance, "vkGetPhysicalDeviceProperties2");
        vk.extensions = (PFN_vkEnumerateDeviceExtensionProperties)proc(vk.instance, "vkEnumerateDeviceExtensionProperties");
    }
#endif
    VkPhysicalDevice physical = compute_device();
    if (!physical) goto fail;
    vk.GetPhysicalDeviceMemoryProperties(physical, &vk.memory);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = vk.family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
#ifdef NEUTRINO_VULKAN_RT
    VkPhysicalDeviceRayQueryFeaturesKHR query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, .rayQuery = VK_TRUE};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR structure = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
        .pNext = &query, .accelerationStructure = VK_TRUE};
    VkPhysicalDeviceBufferDeviceAddressFeatures address = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
        .pNext = &structure, .bufferDeviceAddress = VK_TRUE};
    if (vk.use_rt) {
        device.pNext = &address;
        device.enabledExtensionCount = sizeof rt_extensions / sizeof *rt_extensions;
        device.ppEnabledExtensionNames = rt_extensions;
        if (vk.CreateDevice(physical, &device, NULL, &vk.device) != VK_SUCCESS) {
            vk.use_rt = false;
            vk.device = VK_NULL_HANDLE;
            device.pNext = NULL;
            device.enabledExtensionCount = 0;
            device.ppEnabledExtensionNames = NULL;
        }
    }
#endif
    if (!vk.device && vk.CreateDevice(physical, &device, NULL, &vk.device) != VK_SUCCESS) goto fail;
#define LOAD_DEVICE(name) vk.name = (PFN_vk##name)vk.GetDeviceProcAddr(vk.device, "vk" #name); if (!vk.name) goto fail;
    DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
    vk.GetDeviceQueue(vk.device, vk.family, 0, &vk.queue);
#ifdef NEUTRINO_VULKAN_RT
    if (vk.use_rt) {
#define LOAD_RT(name) vk.name = (PFN_vk##name)vk.GetDeviceProcAddr(vk.device, "vk" #name); if (!vk.name) vk.use_rt = false;
        RT_FUNCTIONS(LOAD_RT)
#undef LOAD_RT
        VkPhysicalDeviceAccelerationStructurePropertiesKHR structure_properties = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 properties = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &structure_properties};
        vk.properties(physical, &properties);
        vk.scratch_alignment = structure_properties.minAccelerationStructureScratchOffsetAlignment;
        if (!vk.scratch_alignment) vk.use_rt = false;
    }
#endif
    VkDescriptorSetLayoutBinding binding = {.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo descriptors = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding};
    if (vk.CreateDescriptorSetLayout(vk.device, &descriptors, NULL, &vk.descriptors) != VK_SUCCESS) goto fail;
    VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &vk.descriptors, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    if (vk.CreatePipelineLayout(vk.device, &layout, NULL, &vk.layout) != VK_SUCCESS) goto fail;
    VkShaderModule shader;
    VkShaderModuleCreateInfo module = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof neutrino_ray_spv, .pCode = neutrino_ray_spv};
    if (vk.CreateShaderModule(vk.device, &module, NULL, &shader) != VK_SUCCESS) goto fail;
    VkComputePipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .layout = vk.layout, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main"}};
    VkResult result = vk.CreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipeline, NULL, &vk.pipeline);
    vk.DestroyShaderModule(vk.device, shader, NULL);
    if (result != VK_SUCCESS || atexit(close_backend)) goto fail;
    return true;
fail:
    close_backend();
    return false;
}

const char *ray_vulkan(const float *rays, size_t count, float *hits, bool hardware_only)
{
    if (!count || count > 4096 || !init_backend() || (hardware_only && !vk.rank)) return NULL;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    void *data = NULL;
    size_t input_bytes = count * RAY_VALUES * sizeof(float), output_bytes = count * sizeof(float);
    VkDeviceSize bytes = input_bytes + output_bytes;
    bool success = false;
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    if (vk.CreateBuffer(vk.device, &info, NULL, &buffer) != VK_SUCCESS) goto done;
    VkMemoryRequirements required;
    vk.GetBufferMemoryRequirements(vk.device, buffer, &required);
    uint32_t type = 0, needed = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (; type < vk.memory.memoryTypeCount; ++type)
        if ((required.memoryTypeBits & (1u << type)) &&
            (vk.memory.memoryTypes[type].propertyFlags & needed) == needed) break;
    if (type == vk.memory.memoryTypeCount) goto done;
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = required.size, .memoryTypeIndex = type};
    if (vk.AllocateMemory(vk.device, &allocation, NULL, &memory) != VK_SUCCESS ||
        vk.BindBufferMemory(vk.device, buffer, memory, 0) != VK_SUCCESS ||
        vk.MapMemory(vk.device, memory, 0, bytes, 0, &data) != VK_SUCCESS) goto done;
    memcpy(data, rays, input_bytes);
    VkDescriptorPoolSize capacity = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1};
    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &capacity};
    if (vk.CreateDescriptorPool(vk.device, &pool_info, NULL, &pool) != VK_SUCCESS) goto done;
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo descriptor = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &vk.descriptors};
    if (vk.AllocateDescriptorSets(vk.device, &descriptor, &set) != VK_SUCCESS) goto done;
    VkDescriptorBufferInfo storage = {.buffer = buffer, .range = bytes};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &storage};
    vk.UpdateDescriptorSets(vk.device, 1, &write, 0, NULL);
    VkCommandPoolCreateInfo command_pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = vk.family, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT};
    if (vk.CreateCommandPool(vk.device, &command_pool, NULL, &commands) != VK_SUCCESS) goto done;
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo command = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vk.AllocateCommandBuffers(vk.device, &command, &cmd) != VK_SUCCESS ||
        vk.BeginCommandBuffer(cmd, &begin) != VK_SUCCESS) goto done;
    vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk.pipeline);
    vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, vk.layout, 0, 1, &set, 0, NULL);
    uint32_t size = (uint32_t)count;
    vk.CmdPushConstants(cmd, vk.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof size, &size);
    vk.CmdDispatch(cmd, (size + 63) / 64, 1, 1);
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &barrier, 0, NULL, 0, NULL);
    if (vk.EndCommandBuffer(cmd) != VK_SUCCESS) goto done;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    success = vk.QueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS &&
        vk.QueueWaitIdle(vk.queue) == VK_SUCCESS;
    if (success) memcpy(hits, (unsigned char *)data + input_bytes, output_bytes);
done:
    if (commands) vk.DestroyCommandPool(vk.device, commands, NULL);
    if (pool) vk.DestroyDescriptorPool(vk.device, pool, NULL);
    if (data) vk.UnmapMemory(vk.device, memory);
    if (buffer) vk.DestroyBuffer(vk.device, buffer, NULL);
    if (memory) vk.FreeMemory(vk.device, memory, NULL);
    return success ? (vk.rank ? "Vulkan GPU" : "Vulkan software") : NULL;
}

#ifdef NEUTRINO_VULKAN_RT
typedef struct { VkBuffer buffer; VkDeviceMemory memory; VkDeviceAddress address; void *map; } RtBuffer;
typedef struct { RtBuffer storage; VkAccelerationStructureKHR handle; } RtStructure;
typedef struct {
    RtStructure blas, tlas;
    RtBuffer bounds, instance, bottom_scratch, top_scratch;
    RtBox boxes[16];
    size_t count, capacity;
    bool valid;
} RtScene;
static RtScene rt_scene;
static struct {
    RtBuffer data;
    VkDescriptorPool descriptors;
    VkDescriptorSet set;
    VkCommandPool commands;
    VkCommandBuffer command;
} rt_dispatch;

static void rt_free_buffer(RtBuffer *buffer)
{
    if (buffer->map) vk.UnmapMemory(vk.device, buffer->memory);
    if (buffer->buffer) vk.DestroyBuffer(vk.device, buffer->buffer, NULL);
    if (buffer->memory) vk.FreeMemory(vk.device, buffer->memory, NULL);
}
static void rt_free_structure(RtStructure *structure)
{
    if (structure->handle) vk.DestroyAccelerationStructureKHR(vk.device, structure->handle, NULL);
    rt_free_buffer(&structure->storage);
}
static void rt_free_scene(RtScene *scene)
{
    rt_free_structure(&scene->tlas);
    rt_free_structure(&scene->blas);
    rt_free_buffer(&scene->top_scratch);
    rt_free_buffer(&scene->bottom_scratch);
    rt_free_buffer(&scene->instance);
    rt_free_buffer(&scene->bounds);
    *scene = (RtScene){0};
}
static void rt_release_scene(void)
{
    rt_free_scene(&rt_scene);
}

static void rt_release_dispatch(void)
{
    if (rt_dispatch.commands) vk.DestroyCommandPool(vk.device, rt_dispatch.commands, NULL);
    if (rt_dispatch.descriptors) vk.DestroyDescriptorPool(vk.device, rt_dispatch.descriptors, NULL);
    rt_free_buffer(&rt_dispatch.data);
    memset(&rt_dispatch, 0, sizeof rt_dispatch);
}

static bool rt_buffer(RtBuffer *out, VkDeviceSize size, VkBufferUsageFlags usage, bool upload)
{
    const bool address = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    if (vk.CreateBuffer(vk.device, &info, NULL, &out->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements;
    vk.GetBufferMemoryRequirements(vk.device, out->buffer, &requirements);
    VkMemoryPropertyFlags flags = upload ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT :
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = 0;
    for (; type < vk.memory.memoryTypeCount; ++type)
        if ((requirements.memoryTypeBits & (1u << type)) &&
            (vk.memory.memoryTypes[type].propertyFlags & flags) == flags) break;
    if (type == vk.memory.memoryTypeCount) return false;
    VkMemoryAllocateFlagsInfo address_flag = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                             .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = address ? &address_flag : NULL, .allocationSize = requirements.size, .memoryTypeIndex = type};
    if (vk.AllocateMemory(vk.device, &allocation, NULL, &out->memory) != VK_SUCCESS ||
        vk.BindBufferMemory(vk.device, out->buffer, out->memory, 0) != VK_SUCCESS) return false;
    if (address) {
        VkBufferDeviceAddressInfo query = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = out->buffer};
        out->address = vk.GetBufferDeviceAddress(vk.device, &query);
        if (!out->address) return false;
    }
    return !upload || vk.MapMemory(vk.device, out->memory, 0, size, 0, &out->map) == VK_SUCCESS;
}

static bool rt_build_info(RtStructure *out, RtBuffer *scratch,
                          VkAccelerationStructureBuildGeometryInfoKHR *build, uint32_t primitives)
{
    if (!out->handle) {
        VkAccelerationStructureBuildSizesInfoKHR sizes = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vk.GetAccelerationStructureBuildSizesKHR(vk.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                build, &primitives, &sizes);
        VkDeviceSize scratch_size = sizes.buildScratchSize > sizes.updateScratchSize ?
            sizes.buildScratchSize : sizes.updateScratchSize;
        if (!rt_buffer(&out->storage, sizes.accelerationStructureSize,
                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false) ||
            !rt_buffer(scratch, scratch_size + vk.scratch_alignment,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false)) return false;
        VkAccelerationStructureCreateInfoKHR info = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = out->storage.buffer, .size = sizes.accelerationStructureSize, .type = build->type};
        if (vk.CreateAccelerationStructureKHR(vk.device, &info, NULL, &out->handle) != VK_SUCCESS) return false;
    }
    if (build->mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR) build->srcAccelerationStructure = out->handle;
    build->dstAccelerationStructure = out->handle;
    build->scratchData.deviceAddress = (scratch->address + vk.scratch_alignment - 1) &
                                      ~((VkDeviceAddress)vk.scratch_alignment - 1);
    return true;
}

static bool init_rt_pipeline(void)
{
    if (!init_backend() || !vk.use_rt) return false;
    if (vk.rt_pipeline) return true;
    VkDescriptorSetLayoutBinding bindings[] = {
        {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}
    };
    VkDescriptorSetLayoutCreateInfo descriptors = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings};
    if (vk.CreateDescriptorSetLayout(vk.device, &descriptors, NULL, &vk.rt_descriptors) != VK_SUCCESS) goto fail;
    VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 2 * sizeof(uint32_t)};
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &vk.rt_descriptors, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    if (vk.CreatePipelineLayout(vk.device, &layout, NULL, &vk.rt_layout) != VK_SUCCESS) goto fail;
    VkShaderModule shader;
    VkShaderModuleCreateInfo module = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof neutrino_ray_query_spv, .pCode = neutrino_ray_query_spv};
    if (vk.CreateShaderModule(vk.device, &module, NULL, &shader) != VK_SUCCESS) goto fail;
    VkComputePipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .layout = vk.rt_layout, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main"}};
    VkResult result = vk.CreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &pipeline, NULL, &vk.rt_pipeline);
    vk.DestroyShaderModule(vk.device, shader, NULL);
    if (result == VK_SUCCESS) return true;
fail:
    vk.use_rt = false;
    return false;
}

static bool init_rt_dispatch(void)
{
    if (rt_dispatch.command) return true;
    const size_t capacity = 4096*(sizeof(RtRay)+sizeof(RtHit)) + 16*sizeof(RtBox);
    if (!rt_buffer(&rt_dispatch.data, capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) goto fail;
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                    {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}};
    VkDescriptorPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes};
    if (vk.CreateDescriptorPool(vk.device, &pool, NULL, &rt_dispatch.descriptors) != VK_SUCCESS) goto fail;
    VkDescriptorSetAllocateInfo descriptor = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = rt_dispatch.descriptors, .descriptorSetCount = 1, .pSetLayouts = &vk.rt_descriptors};
    if (vk.AllocateDescriptorSets(vk.device, &descriptor, &rt_dispatch.set) != VK_SUCCESS) goto fail;
    VkCommandPoolCreateInfo commands = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = vk.family};
    if (vk.CreateCommandPool(vk.device, &commands, NULL, &rt_dispatch.commands) != VK_SUCCESS) goto fail;
    VkCommandBufferAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = rt_dispatch.commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vk.AllocateCommandBuffers(vk.device, &allocation, &rt_dispatch.command) == VK_SUCCESS) return true;
fail:
    rt_release_dispatch();
    return false;
}

bool ray_vulkan_rt(const RtRay *rays, size_t count, const RtBox *boxes, size_t box_count, RtHit *hits)
{
    /* Every submitted batch waits for the queue before returning. Reuse is serial. */
    if (!count || count > 4096 || !box_count || box_count > 16 || !init_rt_pipeline() ||
        !init_rt_dispatch() || vk.ResetCommandPool(vk.device, rt_dispatch.commands, 0) != VK_SUCCESS) return false;
    static_assert(sizeof(RtBox) == sizeof(VkAabbPositionsKHR), "Vulkan AABB layout");
    static_assert(sizeof(RtRay) == 6 * sizeof(float) && sizeof(RtHit) == 2 * sizeof(float), "RT shader buffer layout");
    RtScene replacement = {0};
    const RtBuffer *data = &rt_dispatch.data;
    const VkDescriptorSet set = rt_dispatch.set;
    const VkCommandBuffer command = rt_dispatch.command;
    bool success = false;
    const bool reuse = rt_scene.capacity >= box_count;
    RtScene *scene = reuse ? &rt_scene : &replacement;
    const bool same_count = scene->count == box_count;
    const bool build_scene = !same_count || !scene->valid ||
        memcmp(scene->boxes, boxes, box_count * sizeof *boxes) != 0;
    const VkBuildAccelerationStructureModeKHR mode = same_count && scene->valid ?
        VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    const VkBufferUsageFlags input = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    size_t input_size = count * sizeof *rays + box_count * sizeof *boxes;
    size_t size = input_size + count * sizeof *hits;
    memcpy(data->map, rays, count * sizeof *rays);
    memcpy((unsigned char *)data->map + count * sizeof *rays, boxes, box_count * sizeof *boxes);
    VkAccelerationStructureGeometryKHR geometry = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_AABBS_KHR, .geometry.aabbs = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
            .stride = sizeof(VkAabbPositionsKHR)}};
    VkAccelerationStructureBuildGeometryInfoKHR bottom = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, .mode = mode,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
        .geometryCount = 1, .pGeometries = &geometry};
    VkAccelerationStructureGeometryKHR instances = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
        .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR, .geometry.instances = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR}};
    VkAccelerationStructureBuildGeometryInfoKHR top = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, .mode = mode,
        .flags = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
        .geometryCount = 1, .pGeometries = &instances};
    if (build_scene) {
        /* Do not refit again until all of the updated bounds have completed successfully. */
        scene->valid = false;
        if (!reuse) {
            scene->capacity = 1;
            while (scene->capacity < box_count) scene->capacity *= 2;
            if (!rt_buffer(&scene->bounds, scene->capacity * sizeof(VkAabbPositionsKHR), input, true) ||
                !rt_buffer(&scene->instance, sizeof(VkAccelerationStructureInstanceKHR), input, true)) goto done;
        }
        RtBox *conservative = scene->bounds.map;
        for (size_t i = 0; i < box_count; ++i) {
            for (int axis = 0; axis < 3; ++axis) {
                float scale = fmaxf(1, fmaxf(fabsf(boxes[i].lo[axis]), fabsf(boxes[i].hi[axis])));
                float pad = 8 * FLT_EPSILON * scale;
                conservative[i].lo[axis] = boxes[i].lo[axis] - pad;
                conservative[i].hi[axis] = boxes[i].hi[axis] + pad;
            }
        }
        geometry.geometry.aabbs.data.deviceAddress = scene->bounds.address;
        instances.geometry.instances.data.deviceAddress = scene->instance.address;
        if (!rt_build_info(&scene->blas, &scene->bottom_scratch, &bottom, (uint32_t)scene->capacity) ||
            !rt_build_info(&scene->tlas, &scene->top_scratch, &top, 1)) goto done;
        if (!reuse) {
            VkAccelerationStructureDeviceAddressInfoKHR address = {
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
                .accelerationStructure = scene->blas.handle};
            VkDeviceAddress blas_address = vk.GetAccelerationStructureDeviceAddressKHR(vk.device, &address);
            if (!blas_address) goto done;
            VkAccelerationStructureInstanceKHR object = {
                .transform.matrix = {{1,0,0,0}, {0,1,0,0}, {0,0,1,0}}, .mask = 0xff,
                .accelerationStructureReference = blas_address};
            memcpy(scene->instance.map, &object, sizeof object);
        }
    }

    VkDescriptorBufferInfo storage = {.buffer = data->buffer, .range = size};
    VkWriteDescriptorSetAccelerationStructureKHR scene_descriptor = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
        .accelerationStructureCount = 1, .pAccelerationStructures = &scene->tlas.handle};
    VkWriteDescriptorSet writes[] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &storage},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = &scene_descriptor, .dstSet = set, .dstBinding = 1,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR}
    };
    vk.UpdateDescriptorSets(vk.device, 2, writes, 0, NULL);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                     .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vk.BeginCommandBuffer(command, &begin) != VK_SUCCESS) goto done;
    VkAccelerationStructureBuildRangeInfoKHR bottom_range = {.primitiveCount = (uint32_t)box_count};
    VkAccelerationStructureBuildRangeInfoKHR top_range = {.primitiveCount = 1};
    const VkAccelerationStructureBuildRangeInfoKHR *range = &bottom_range;
    VkMemoryBarrier build_barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR};
    if (build_scene) {
        if (reuse) {
            VkMemoryBarrier reuse_barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                 VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                .dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                 VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR};
            vk.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                  0, 1, &reuse_barrier, 0, NULL, 0, NULL);
        }
        vk.CmdBuildAccelerationStructuresKHR(command, 1, &bottom, &range);
        vk.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                              VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &build_barrier, 0, NULL, 0, NULL);
        range = &top_range;
        vk.CmdBuildAccelerationStructuresKHR(command, 1, &top, &range);
    }
    vk.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &build_barrier, 0, NULL, 0, NULL);
    vk.CmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, vk.rt_pipeline);
    vk.CmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, vk.rt_layout, 0, 1, &set, 0, NULL);
    uint32_t limits[] = {(uint32_t)count, (uint32_t)box_count};
    vk.CmdPushConstants(command, vk.rt_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof limits, limits);
    vk.CmdDispatch(command, (limits[0] + 63) / 64, 1, 1);
    VkMemoryBarrier readback = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vk.CmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &readback, 0, NULL, 0, NULL);
    if (vk.EndCommandBuffer(command) != VK_SUCCESS) goto done;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    success = vk.QueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS && vk.QueueWaitIdle(vk.queue) == VK_SUCCESS;
    if (success) memcpy(hits, (unsigned char *)data->map + input_size, count * sizeof *hits);

done:
    if (success && build_scene) {
        memcpy(scene->boxes, boxes, box_count * sizeof *boxes);
        scene->count = box_count;
        scene->valid = true;
        if (!reuse) {
            rt_release_scene();
            rt_scene = replacement;
            replacement = (RtScene){0};
        }
    }
    rt_free_scene(&replacement);
    return success;
}
#endif
