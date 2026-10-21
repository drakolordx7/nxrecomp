// SPDX-License-Identifier: GPL-3.0-or-later
// Vulkan device layer (declarations in include/nxrt/gfx.h): instance + validation, GPU choice, logical device, VMA, image-format
// checks, buffers, one-shot submits.
// Ideas taken from SuperMarioWonderRecomp (MIT) docs/nvn/DEBUG-2.md #69 (ASTC on NVIDIA desktop) and #70 (iGPU listed first):
// the checks and the GPU rule are written from scratch, see docs/S4b.md.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>

#include "nxrt/config.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"
#include "nxrt/platform.h"

#include <vk_mem_alloc.h>

namespace nxrt::gfx {
namespace {

constexpr const char* kTag = "gfx";
constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const char* type_name(int t) {
    static const char* const names[] = {"other", "integrated", "discrete", "virtual", "cpu"};
    return t >= 0 && t <= 4 ? names[t] : "?";
}

// Higher is better: discrete, integrated, virtual, other, cpu.
int type_rank(int t) {
    switch (t) {
        case 2: return 4;
        case 1: return 3;
        case 3: return 2;
        case 0: return 1;
        default: return 0;
    }
}

size_t best_of_rank(const std::vector<GpuCandidate>& g) {
    size_t best = 0;
    for (size_t i = 1; i < g.size(); ++i) {
        const int ri = type_rank(g[i].type), rb = type_rank(g[best].type);
        if (ri > rb || (ri == rb && g[i].vram_bytes > g[best].vram_bytes)) best = i;
    }
    return best;
}

size_t first_of_type(const std::vector<GpuCandidate>& g, int type) {
    size_t best = SIZE_MAX;
    for (size_t i = 0; i < g.size(); ++i)
        if (g[i].type == type && (best == SIZE_MAX || g[i].vram_bytes > g[best].vram_bytes)) best = i;
    return best;
}

bool is_bc(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }
bool is_etc2(VkFormat f) { return f >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && f <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK; }
bool is_astc(VkFormat f) {
    return (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) ||
           (f >= VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK && f <= VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK);
}

VkImageAspectFlags aspect_of(VkFormat f) {
    switch (f) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_X8_D24_UNORM_PACK32:
        case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);

}  // namespace

// ---- GPU rule ---------------------------------------------------------------------------------------------------------
size_t pick_gpu(const std::vector<GpuCandidate>& gpus, const char* want, bool* matched) {
    if (matched) *matched = true;
    if (gpus.empty()) return 0;
    const size_t fallback = best_of_rank(gpus);
    const std::string w = want ? lower(want) : "";
    if (w.empty()) return fallback;
    size_t k = SIZE_MAX;
    if (w == "discrete") {
        k = first_of_type(gpus, 2);
    } else if (w == "integrated") {
        k = first_of_type(gpus, 1);
    } else if (w.size() <= 2 && std::isdigit(static_cast<unsigned char>(w[0]))) {   // an index; "3080" is a name part
        char* end = nullptr;
        const unsigned long n = std::strtoul(w.c_str(), &end, 10);
        if (end && !*end && n < gpus.size()) k = static_cast<size_t>(n);
    } else {
        for (size_t i = 0; i < gpus.size() && k == SIZE_MAX; ++i)
            if (lower(gpus[i].name).find(w) != std::string::npos) k = i;
    }
    if (k != SIZE_MAX) return k;
    if (matched) *matched = false;
    return fallback;
}

// ---- private state ----------------------------------------------------------------------------------------------------
struct Device::Impl {
    // validation counters (written from any thread by the layer callback)
    std::atomic<uint64_t> errors{0}, warnings{0}, infos{0}, verbose{0};
    std::mutex first_error_mu;
    std::string first_error;

    // image-format cache
    struct Key {
        VkFormat format; VkImageType type; VkImageTiling tiling; VkImageUsageFlags usage; VkImageCreateFlags flags;
        bool operator<(const Key& o) const {
            return std::tie(format, type, tiling, usage, flags) < std::tie(o.format, o.type, o.tiling, o.usage, o.flags);
        }
    };
    struct Val { VkResult result; VkImageFormatProperties props; };
    std::mutex fmt_mu;
    std::map<Key, Val> fmt_cache;
    std::atomic<uint64_t> fmt_checks{0}, fmt_hits{0}, fmt_rejected{0};

    // one-shot submits
    std::mutex once_mu;
    VkCommandPool once_pool = VK_NULL_HANDLE;
    VkFence once_fence = VK_NULL_HANDLE;
};

namespace {
VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto* impl = static_cast<Device::Impl*>(user);
    const char* id = data && data->pMessageIdName ? data->pMessageIdName : "";
    const char* msg = data && data->pMessage ? data->pMessage : "";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        if (impl->errors.fetch_add(1) == 0) {
            std::lock_guard<std::mutex> lk(impl->first_error_mu);
            impl->first_error = std::string(id) + ": " + msg;
        }
        NXLOG_E("vulkan", "%s: %s", id, msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        impl->warnings.fetch_add(1);
        NXLOG_W("vulkan", "%s: %s", id, msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) {
        impl->infos.fetch_add(1);
        NXLOG_D("vulkan", "%s: %s", id, msg);
    } else {
        impl->verbose.fetch_add(1);
    }
    return VK_FALSE;
}
}  // namespace

// ---- creation ---------------------------------------------------------------------------------------------------------
std::unique_ptr<Device> Device::create(const DeviceDesc& desc, std::string* error) {
    std::unique_ptr<Device> d(new Device());
    d->impl_ = new Impl();
    std::string err;
    if (!d->init(desc, &err)) {
        NXLOG_E(kTag, "device creation failed: %s", err.c_str());
        if (error) *error = err;
        return nullptr;
    }
    return d;
}

bool Device::init(const DeviceDesc& desc, std::string* error) {
    auto fail = [&](const std::string& m) {
        *error = m;
        return false;
    };
#ifdef NDEBUG
    constexpr bool kDebugBuild = false;
#else
    constexpr bool kDebugBuild = true;
#endif
    const bool want_validation = desc.validation >= 0 ? desc.validation != 0 : config_get_bool("gfx", "validation", kDebugBuild);
    const bool headless = desc.headless >= 0 ? desc.headless != 0 : config_get_bool("gfx", "headless", false);
    std::string gpu_want = !desc.gpu.empty() ? desc.gpu : config_get_str("gfx", "gpu", "");

    if (volkInitialize() != VK_SUCCESS) return fail("no Vulkan loader found (vulkan-1.dll)");
    uint32_t inst_version = 0;
    vkEnumerateInstanceVersion(&inst_version);
    if (inst_version < VK_API_VERSION_1_3) return fail("Vulkan 1.3 is required; the loader offers less");

    if (!headless && !platform::has_window()) return fail("no platform window: create one (platform::create_window) or run headless");

    // layers and extensions
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> layers(n);
    vkEnumerateInstanceLayerProperties(&n, layers.data());
    auto has_layer = [&](const char* name) {
        for (auto& l : layers) if (!std::strcmp(l.layerName, name)) return true;
        return false;
    };
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> iexts(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, iexts.data());
    auto has_iext = [&](const char* name) {
        for (auto& e : iexts) if (!std::strcmp(e.extensionName, name)) return true;
        return false;
    };

    std::vector<const char*> inst_layers, inst_exts;
    if (want_validation) {
        if (has_layer(kValidationLayer)) {
            inst_layers.push_back(kValidationLayer);
            validation_ = true;
        } else {
            NXLOG_W(kTag, "validation requested but %s is not installed (Vulkan SDK): continuing without", kValidationLayer);
        }
    }
    if (!headless) {
        uint32_t cnt = 0;
        const char* const* pe = platform::vulkan_instance_extensions(&cnt);
        for (uint32_t i = 0; pe && i < cnt; ++i) inst_exts.push_back(pe[i]);
    }
    if (validation_ && has_iext(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        inst_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        debug_utils_ = true;
    }
    VkValidationFeatureEnableEXT enables[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
    VkValidationFeaturesEXT vfeat = {VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    vfeat.enabledValidationFeatureCount = 1;
    vfeat.pEnabledValidationFeatures = enables;
    const bool vfeat_ok = validation_ && has_iext(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    if (vfeat_ok) inst_exts.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);

    VkDebugUtilsMessengerCreateInfoEXT dbg = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT;
    dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dbg.pfnUserCallback = debug_callback;
    dbg.pUserData = impl_;
    if (vfeat_ok) vfeat.pNext = &dbg;   // messages from instance creation and destruction too

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = desc.app_name.c_str();
    app.applicationVersion = 1;
    app.pEngineName = "nxrt";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = static_cast<uint32_t>(inst_layers.size());
    ici.ppEnabledLayerNames = inst_layers.data();
    ici.enabledExtensionCount = static_cast<uint32_t>(inst_exts.size());
    ici.ppEnabledExtensionNames = inst_exts.data();
    ici.pNext = vfeat_ok ? static_cast<void*>(&vfeat) : (debug_utils_ ? static_cast<void*>(&dbg) : nullptr);
    VkResult r = vkCreateInstance(&ici, nullptr, &instance_);
    if (r != VK_SUCCESS) return fail("vkCreateInstance failed (" + std::to_string(r) + ")");
    volkLoadInstance(instance_);
    if (debug_utils_) vkCreateDebugUtilsMessengerEXT(instance_, &dbg, nullptr, &messenger_);

    if (!headless) {
        uint64_t s = 0;
        if (!platform::create_vulkan_surface(instance_, &s)) return fail("could not create the Vulkan surface for the window");
        surface_ = reinterpret_cast<VkSurfaceKHR>(s);
        owns_platform_surface_ = true;
    }

    // physical devices
    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(instance_, &ndev, nullptr);
    if (!ndev) return fail("no Vulkan physical device");
    std::vector<VkPhysicalDevice> devs(ndev);
    vkEnumeratePhysicalDevices(instance_, &ndev, devs.data());

    struct Cand { VkPhysicalDevice pd; uint32_t qfam; bool eligible; std::string why; };
    std::vector<Cand> cands;
    for (uint32_t i = 0; i < ndev; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        GpuInfo gi;
        gi.index = i;
        gi.name = p.deviceName;
        gi.vendor_id = p.vendorID;
        gi.device_id = p.deviceID;
        gi.type = static_cast<int>(p.deviceType);
        gi.api_version = p.apiVersion;
        gi.driver_version = p.driverVersion;
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(devs[i], &mp);
        for (uint32_t h = 0; h < mp.memoryHeapCount; ++h)
            if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) gi.vram_bytes = std::max<uint64_t>(gi.vram_bytes, mp.memoryHeaps[h].size);

        Cand c{devs[i], UINT32_MAX, true, ""};
        if (p.apiVersion < VK_API_VERSION_1_3) { c.eligible = false; c.why = "Vulkan 1.3 not supported"; }
        if (c.eligible) {
            VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
            vkGetPhysicalDeviceFeatures2(devs[i], &f2);
            if (!f13.dynamicRendering || !f13.synchronization2) { c.eligible = false; c.why = "dynamic rendering / synchronization2 missing"; }
        }
        if (c.eligible && !headless) {
            uint32_t ne = 0;
            vkEnumerateDeviceExtensionProperties(devs[i], nullptr, &ne, nullptr);
            std::vector<VkExtensionProperties> ex(ne);
            vkEnumerateDeviceExtensionProperties(devs[i], nullptr, &ne, ex.data());
            bool sw = false;
            for (auto& e : ex) if (!std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) sw = true;
            if (!sw) { c.eligible = false; c.why = "no VK_KHR_swapchain"; }
        }
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, qf.data());
        for (uint32_t q = 0; q < nq && c.eligible && c.qfam == UINT32_MAX; ++q) {
            if (!(qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !(qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
            if (!headless) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, surface_, &present);
                if (!present) continue;
            }
            c.qfam = q;
        }
        if (c.eligible && c.qfam == UINT32_MAX) { c.eligible = false; c.why = headless ? "no graphics+compute queue" : "no queue family that draws and presents"; }
        gi.can_present = c.eligible;
        gpus_.push_back(gi);
        cands.push_back(c);
        NXLOG_I(kTag, "GPU %u: %s (%s, Vulkan %u.%u.%u, driver 0x%08x, %.1f GiB)%s%s", i, p.deviceName, type_name(gi.type),
                VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
                p.driverVersion, static_cast<double>(gi.vram_bytes) / (1024.0 * 1024.0 * 1024.0),
                c.eligible ? "" : " NOT USABLE: ", c.eligible ? "" : c.why.c_str());
    }

    // The rule runs over the usable devices only; a numeric override is an index into the list logged above.
    std::vector<GpuCandidate> usable;
    std::vector<size_t> usable_to_all;
    for (size_t i = 0; i < gpus_.size(); ++i) {
        if (!cands[i].eligible) continue;
        usable.push_back({gpus_[i].name, gpus_[i].type, gpus_[i].vram_bytes});
        usable_to_all.push_back(i);
    }
    if (usable.empty()) return fail("no usable Vulkan 1.3 GPU");
    std::string want = lower(gpu_want);
    bool numeric = !want.empty() && want.size() <= 2 && std::isdigit(static_cast<unsigned char>(want[0]));
    if (numeric) {
        const size_t idx = static_cast<size_t>(std::strtoul(want.c_str(), nullptr, 10));
        auto it = std::find(usable_to_all.begin(), usable_to_all.end(), idx);
        want = it != usable_to_all.end() ? std::to_string(it - usable_to_all.begin()) : std::string("\x01no-such-gpu");
    }
    bool matched = true;
    const size_t pick = usable_to_all[pick_gpu(usable, want.c_str(), &matched)];
    gpu_override_matched_ = matched;
    if (!matched) NXLOG_W(kTag, "gpu override '%s' matches no usable GPU: using the default", gpu_want.c_str());
    gpu_index_ = pick;
    physical_ = cands[pick].pd;
    queue_family_ = cands[pick].qfam;
    NXLOG_I(kTag, "using GPU %zu: %s%s", pick, gpus_[pick].name.c_str(), gpu_want.empty() ? " (default rule)" : " (override)");
    vkGetPhysicalDeviceProperties(physical_, &props_);

    // features: enable what the device has of the set the NVN back end will want, and what Vulkan 1.3 rendering needs
    VkPhysicalDeviceVulkan13Features a13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features a12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &a13};
    VkPhysicalDeviceFeatures2 avail = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &a12};
    vkGetPhysicalDeviceFeatures2(physical_, &avail);
    VkPhysicalDeviceVulkan13Features e13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features e12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &e13};
    VkPhysicalDeviceFeatures2 en = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e12};
    e13.dynamicRendering = VK_TRUE;
    e13.synchronization2 = VK_TRUE;
    e12.timelineSemaphore = a12.timelineSemaphore;
    const VkPhysicalDeviceFeatures& af = avail.features;
    en.features.samplerAnisotropy = af.samplerAnisotropy;
    en.features.independentBlend = af.independentBlend;
    en.features.depthClamp = af.depthClamp;
    en.features.fillModeNonSolid = af.fillModeNonSolid;
    en.features.textureCompressionBC = af.textureCompressionBC;
    en.features.textureCompressionASTC_LDR = af.textureCompressionASTC_LDR;
    en.features.textureCompressionETC2 = af.textureCompressionETC2;
    features_.bc = af.textureCompressionBC;
    features_.astc_ldr = af.textureCompressionASTC_LDR;
    features_.etc2 = af.textureCompressionETC2;
    features_.sampler_anisotropy = af.samplerAnisotropy;
    features_.independent_blend = af.independentBlend;
    features_.depth_clamp = af.depthClamp;
    features_.fill_mode_non_solid = af.fillModeNonSolid;
    features_.timeline_semaphore = a12.timelineSemaphore;
    features_.dynamic_rendering = true;
    features_.synchronization2 = true;
    NXLOG_I(kTag, "texture compression on this GPU: BC %s, ASTC-LDR %s, ETC2 %s", features_.bc ? "yes" : "no",
            features_.astc_ldr ? "yes" : "no", features_.etc2 ? "yes" : "no");

    std::vector<const char*> dev_exts;
    if (!headless) dev_exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &en};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(dev_exts.size());
    dci.ppEnabledExtensionNames = dev_exts.data();
    r = vkCreateDevice(physical_, &dci, nullptr, &device_);
    if (r != VK_SUCCESS) return fail("vkCreateDevice failed (" + std::to_string(r) + ")");
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_, &nq, qf.data());
    timestamp_valid_bits_ = qf[queue_family_].timestampValidBits;

    VmaVulkanFunctions vf = {};
    vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci = {};
    aci.physicalDevice = physical_;
    aci.device = device_;
    aci.instance = instance_;
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    aci.pVulkanFunctions = &vf;
    r = vmaCreateAllocator(&aci, &vma_);
    if (r != VK_SUCCESS) return fail("vmaCreateAllocator failed (" + std::to_string(r) + ")");

    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = queue_family_;
    vkCreateCommandPool(device_, &cpi, nullptr, &impl_->once_pool);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(device_, &fci, nullptr, &impl_->once_fence);

    NXLOG_I(kTag, "device ready: %s, queue family %u, validation %s, %s", props_.deviceName, queue_family_, validation_ ? "on" : "off",
            headless ? "headless" : "window surface");
    return true;
}

Device::~Device() {
    if (device_) vkDeviceWaitIdle(device_);
    if (impl_) {
        if (impl_->once_fence) vkDestroyFence(device_, impl_->once_fence, nullptr);
        if (impl_->once_pool) vkDestroyCommandPool(device_, impl_->once_pool, nullptr);
    }
    if (vma_) vmaDestroyAllocator(vma_);
    if (device_) vkDestroyDevice(device_, nullptr);
    if (surface_ && owns_platform_surface_) platform::destroy_vulkan_surface(instance_, reinterpret_cast<uint64_t>(surface_));
    if (messenger_) vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
    delete impl_;
}

// ---- validation -------------------------------------------------------------------------------------------------------
ValidationStats Device::validation_stats() const {
    ValidationStats s;
    s.errors = impl_->errors.load();
    s.warnings = impl_->warnings.load();
    s.infos = impl_->infos.load();
    s.verbose = impl_->verbose.load();
    std::lock_guard<std::mutex> lk(impl_->first_error_mu);
    s.first_error = impl_->first_error;
    return s;
}

void Device::validation_reset() {
    impl_->errors = impl_->warnings = impl_->infos = impl_->verbose = 0;
    std::lock_guard<std::mutex> lk(impl_->first_error_mu);
    impl_->first_error.clear();
}

void Device::set_name(VkObjectType type, uint64_t handle, const char* name) {
    if (!debug_utils_ || !name) return;
    VkDebugUtilsObjectNameInfoEXT ni = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    ni.objectType = type;
    ni.objectHandle = handle;
    ni.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(device_, &ni);
}

// ---- images -----------------------------------------------------------------------------------------------------------
FormatCheckStats Device::format_check_stats() const {
    return {impl_->fmt_checks.load(), impl_->fmt_hits.load(), impl_->fmt_rejected.load()};
}

bool Device::image_supported(const ImageDesc& d, std::string* why) {
    auto no = [&](const std::string& m) {
        if (why) *why = m;
        return false;
    };
    char fmtbuf[48];
    std::snprintf(fmtbuf, sizeof fmtbuf, "format %d", static_cast<int>(d.format));
    if (d.format == VK_FORMAT_UNDEFINED) return no("VK_FORMAT_UNDEFINED");
    if (!d.width || !d.height || !d.depth || !d.mip_levels || !d.array_layers) return no("a zero image dimension");
    if (is_astc(d.format) && !features_.astc_ldr) return no(std::string(fmtbuf) + ": ASTC, textureCompressionASTC_LDR is not available on " + props_.deviceName);
    if (is_bc(d.format) && !features_.bc) return no(std::string(fmtbuf) + ": BC, textureCompressionBC is not available on " + props_.deviceName);
    if (is_etc2(d.format) && !features_.etc2) return no(std::string(fmtbuf) + ": ETC2, textureCompressionETC2 is not available on " + props_.deviceName);

    const Impl::Key key{d.format, d.type, d.tiling, d.usage, d.flags};
    Impl::Val val;
    {
        std::lock_guard<std::mutex> lk(impl_->fmt_mu);
        auto it = impl_->fmt_cache.find(key);
        if (it != impl_->fmt_cache.end()) {
            val = it->second;
            impl_->fmt_hits++;
        } else {
            VkPhysicalDeviceImageFormatInfo2 info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
            info.format = d.format;
            info.type = d.type;
            info.tiling = d.tiling;
            info.usage = d.usage;
            info.flags = d.flags;
            VkImageFormatProperties2 out = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
            val.result = vkGetPhysicalDeviceImageFormatProperties2(physical_, &info, &out);
            val.props = out.imageFormatProperties;
            impl_->fmt_checks++;
            impl_->fmt_cache.emplace(key, val);
        }
    }
    if (val.result != VK_SUCCESS) return no(std::string(fmtbuf) + ": vkGetPhysicalDeviceImageFormatProperties2 says " + std::to_string(val.result) + " for this type/tiling/usage/flags");
    if (d.width > val.props.maxExtent.width || d.height > val.props.maxExtent.height || d.depth > val.props.maxExtent.depth)
        return no(std::string(fmtbuf) + ": extent exceeds the device maximum");
    if (d.mip_levels > val.props.maxMipLevels) return no(std::string(fmtbuf) + ": more mip levels than the device allows");
    if (d.array_layers > val.props.maxArrayLayers) return no(std::string(fmtbuf) + ": more array layers than the device allows");
    if (!(val.props.sampleCounts & d.samples)) return no(std::string(fmtbuf) + ": sample count not supported");
    const uint32_t maxdim = std::max({d.width, d.height, d.type == VK_IMAGE_TYPE_3D ? d.depth : 1u});
    if (d.mip_levels > 1 + static_cast<uint32_t>(std::floor(std::log2(static_cast<double>(maxdim))))) return no(std::string(fmtbuf) + ": more mip levels than the image has");
    return true;
}

VkResult Device::create_image(const ImageDesc& d, Image* out, std::string* why) {
    std::string reason;
    if (!image_supported(d, &reason)) {
        impl_->fmt_rejected++;
        NXLOG_W(kTag, "image '%s' refused before creation: %s", d.name ? d.name : "?", reason.c_str());
        if (why) *why = reason;
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = d.flags;
    ci.imageType = d.type;
    ci.format = d.format;
    ci.extent = {d.width, d.height, d.depth};
    ci.mipLevels = d.mip_levels;
    ci.arrayLayers = d.array_layers;
    ci.samples = d.samples;
    ci.tiling = d.tiling;
    ci.usage = d.usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    Image img;
    VkImage handle = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    VkResult r = vmaCreateImage(vma_, &ci, &ai, &handle, &alloc, nullptr);
    if (r != VK_SUCCESS) {
        if (why) *why = "vmaCreateImage failed (" + std::to_string(r) + ")";
        return r;
    }
    img.image = handle;
    img.allocation = alloc;
    img.format = d.format;
    img.extent = ci.extent;
    img.mip_levels = d.mip_levels;
    img.array_layers = d.array_layers;
    if (d.create_view) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = handle;
        vi.format = d.format;
        if (d.type == VK_IMAGE_TYPE_3D) vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
        else if (d.type == VK_IMAGE_TYPE_1D) vi.viewType = d.array_layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        else if (d.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) vi.viewType = d.array_layers > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        else vi.viewType = d.array_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        vi.subresourceRange = {aspect_of(d.format), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        r = vkCreateImageView(device_, &vi, nullptr, &img.view);
        if (r != VK_SUCCESS) {
            vmaDestroyImage(vma_, handle, alloc);
            if (why) *why = "vkCreateImageView failed (" + std::to_string(r) + ")";
            return r;
        }
    }
    set_name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(handle), d.name);
    *out = img;
    return VK_SUCCESS;
}

void Device::destroy_image(Image& img) {
    if (img.view) vkDestroyImageView(device_, img.view, nullptr);
    if (img.image) vmaDestroyImage(vma_, img.image, img.allocation);
    img = Image{};
}

// ---- buffers ----------------------------------------------------------------------------------------------------------
VkResult Device::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryUse use, Buffer* out, const char* name) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (use == MemoryUse::Upload) ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    if (use == MemoryUse::Readback) ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info = {};
    Buffer b;
    VkResult r = vmaCreateBuffer(vma_, &bi, &ai, &b.buffer, &b.allocation, &info);
    if (r != VK_SUCCESS) return r;
    b.size = size;
    b.mapped = info.pMappedData;
    set_name(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(b.buffer), name);
    *out = b;
    return VK_SUCCESS;
}

void Device::destroy_buffer(Buffer& b) {
    if (b.buffer) vmaDestroyBuffer(vma_, b.buffer, b.allocation);
    b = Buffer{};
}

void Device::flush_buffer(const Buffer& b) {
    if (b.allocation) vmaFlushAllocation(vma_, b.allocation, 0, VK_WHOLE_SIZE);
}

void Device::invalidate_buffer(const Buffer& b) {
    if (b.allocation) vmaInvalidateAllocation(vma_, b.allocation, 0, VK_WHOLE_SIZE);
}

// ---- one-shot submit, idle, stats -------------------------------------------------------------------------------------
bool Device::submit_once(const std::function<void(VkCommandBuffer)>& fn) {
    std::lock_guard<std::mutex> lk(impl_->once_mu);
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = impl_->once_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device_, &ai, &cmd) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    fn(cmd);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    bool ok;
    {
        std::lock_guard<std::mutex> ql(queue_mutex_);
        ok = vkQueueSubmit(queue_, 1, &si, impl_->once_fence) == VK_SUCCESS;
    }
    if (ok) ok = vkWaitForFences(device_, 1, &impl_->once_fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    vkResetFences(device_, 1, &impl_->once_fence);
    vkFreeCommandBuffers(device_, impl_->once_pool, 1, &cmd);
    return ok;
}

void Device::wait_idle() {
    std::lock_guard<std::mutex> ql(queue_mutex_);
    vkDeviceWaitIdle(device_);
}

Device::MemoryStats Device::memory_stats() {
    VmaTotalStatistics st;
    vmaCalculateStatistics(vma_, &st);
    MemoryStats m;
    m.allocations = st.total.statistics.allocationCount;
    m.blocks = st.total.statistics.blockCount;
    m.bytes = st.total.statistics.allocationBytes;
    return m;
}

}  // namespace nxrt::gfx
