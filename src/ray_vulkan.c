#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "ray.h"
#include "neutrino_ray_spv.h"

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
#define DECLARE(name) PFN_vk##name name;
    INSTANCE_FUNCTIONS(DECLARE)
    DEVICE_FUNCTIONS(DECLARE)
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

static VkPhysicalDevice compute_device(void)
{
    uint32_t count = 0;
    if (vk.EnumeratePhysicalDevices(vk.instance, &count, NULL) != VK_SUCCESS || !count || count > 64)
        return VK_NULL_HANDLE;
    VkPhysicalDevice devices[64], best = VK_NULL_HANDLE;
    if (vk.EnumeratePhysicalDevices(vk.instance, &count, devices) != VK_SUCCESS) return VK_NULL_HANDLE;
    vk.rank = -1;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties properties;
        vk.GetPhysicalDeviceProperties(devices[i], &properties);
        VkPhysicalDeviceType type = properties.deviceType;
        int rank = type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 :
            type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 :
            type == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 1 : 0;
        if (rank <= vk.rank) continue;
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
    VkInstanceCreateInfo instance = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    if (!create || create(&instance, NULL, &vk.instance) != VK_SUCCESS) goto fail;
#define LOAD_INSTANCE(name) vk.name = (PFN_vk##name)proc(vk.instance, "vk" #name); if (!vk.name) goto fail;
    INSTANCE_FUNCTIONS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
    VkPhysicalDevice physical = compute_device();
    if (!physical) goto fail;
    vk.GetPhysicalDeviceMemoryProperties(physical, &vk.memory);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = vk.family, .queueCount = 1, .pQueuePriorities = &priority};
    VkDeviceCreateInfo device = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
    if (vk.CreateDevice(physical, &device, NULL, &vk.device) != VK_SUCCESS) goto fail;
#define LOAD_DEVICE(name) vk.name = (PFN_vk##name)vk.GetDeviceProcAddr(vk.device, "vk" #name); if (!vk.name) goto fail;
    DEVICE_FUNCTIONS(LOAD_DEVICE)
#undef LOAD_DEVICE
    vk.GetDeviceQueue(vk.device, vk.family, 0, &vk.queue);
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
