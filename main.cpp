#include <jni.h>
#include <thread>
#include <chrono>
#include <android/log.h>
#include <dlfcn.h>
#include <vulkan/vulkan.h>
#include <android/native_activity.h>
#include <android/input.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <atomic>

#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_vulkan.h"
#include "Dobby.h"

#define LOG_TAG "Iptade"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define JNI_EXPORT __attribute__((visibility("default")))

static std::atomic<bool> g_running{true};
static bool g_menu_open = true;
static bool g_Initialized = false;

// Vulkan объекты
static VkDevice g_Device = VK_NULL_HANDLE;
static VkPhysicalDevice g_PhysicalDevice = VK_NULL_HANDLE;
static uint32_t g_QueueFamily = 0;
static VkQueue g_Queue = VK_NULL_HANDLE;
static VkSwapchainKHR g_Swapchain = VK_NULL_HANDLE;
static VkFormat g_SwapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
static VkExtent2D g_SwapchainExtent = {1920, 1080};

static VkDescriptorPool g_DescriptorPool = VK_NULL_HANDLE;
static VkRenderPass g_RenderPass = VK_NULL_HANDLE;
static VkCommandPool g_CommandPool = VK_NULL_HANDLE;

// Синхронизация
static const int MAX_FRAMES_IN_FLIGHT = 2;
static std::vector<VkCommandBuffer> g_CommandBuffers;
static std::vector<VkFence> g_Fences;
static std::vector<VkSemaphore> g_Semaphores; // Добавлено для стабильности
static int g_CurrentFrame = 0;

static std::vector<VkImage> g_SwapchainImages;
static std::vector<VkImageView> g_SwapchainImageViews;
static std::vector<VkFramebuffer> g_Framebuffers;

// --- Защита ---
static void HideFromMaps(const char* name) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, name)) {
            char* p = strchr(line, '/');
            if (p) memset(p, ' ', strlen(p) - 1);
        }
    }
    fclose(f);
}

static bool IsDebuggerAttached() {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return false;
    char buf[256];
    bool found = false;
    while (fgets(buf, sizeof(buf), f)) {
        if (strncmp(buf, "TracerPid:", 10) == 0) {
            found = atoi(buf + 10) != 0;
            break;
        }
    }
    fclose(f);
    return found;
}

// --- 1. ХУК КАСАНИЙ ---
typedef int32_t (*ANativeActivity_onInputEvent_t)(ANativeActivity*, AInputEvent*);
static ANativeActivity_onInputEvent_t orig_onInputEvent = nullptr;

int32_t hook_onInputEvent(ANativeActivity* activity, AInputEvent* event) {
    if (g_Initialized) {
        ImGui_ImplAndroid_HandleInputEvent(event);
    }
    return orig_onInputEvent(activity, event);
}

// --- 2. ХУК vkCreateDevice ---
typedef VkResult (*vkCreateDevice_t)(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
static vkCreateDevice_t orig_vkCreateDevice = nullptr;

VkResult hook_vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    VkResult result = orig_vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result == VK_SUCCESS) {
        g_Device = *pDevice;
        g_PhysicalDevice = physicalDevice;
        g_QueueFamily = pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex;
        vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);
        LOGI("[+] Device hooked: %p", g_Device);
    }
    return result;
}

// --- 3. ХУК vkCreateSwapchainKHR ---
typedef VkResult (*vkCreateSwapchainKHR_t)(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
static vkCreateSwapchainKHR_t orig_vkCreateSwapchainKHR = nullptr;

VkResult hook_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
    VkResult result = orig_vkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    if (result == VK_SUCCESS) {
        g_Swapchain = *pSwapchain;
        g_SwapchainFormat = pCreateInfo->imageFormat;
        g_SwapchainExtent = pCreateInfo->imageExtent;

        uint32_t imageCount = 0;
        vkGetSwapchainImagesKHR(g_Device, g_Swapchain, &imageCount, nullptr);
        g_SwapchainImages.resize(imageCount);
        vkGetSwapchainImagesKHR(g_Device, g_Swapchain, &imageCount, g_SwapchainImages.data());

        g_SwapchainImageViews.resize(imageCount);
        g_Framebuffers.resize(imageCount);

        for (uint32_t i = 0; i < imageCount; i++) {
            VkImageViewCreateInfo view_info = {};
            view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view_info.image = g_SwapchainImages[i];
            view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = g_SwapchainFormat;
            view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            view_info.subresourceRange.levelCount = 1;
            view_info.subresourceRange.layerCount = 1;
            vkCreateImageView(g_Device, &view_info, nullptr, &g_SwapchainImageViews[i]);
        }
        LOGI("[+] Swapchain hooked: %u images", imageCount);
    }
    return result;
}

// --- 4. Инициализация ImGui ---
void SetupImGui() {
    // DescriptorPool
    VkDescriptorPoolSize pool_sizes[] = { { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 }, { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 }, { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 }, { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 } };
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1000 * IM_ARRAYSIZE(pool_sizes);
    pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;
    vkCreateDescriptorPool(g_Device, &pool_info, nullptr, &g_DescriptorPool);

    // RenderPass
    VkAttachmentDescription attachment = {};
    attachment.format = g_SwapchainFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkRenderPassCreateInfo rp_info = {};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &attachment;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    vkCreateRenderPass(g_Device, &rp_info, nullptr, &g_RenderPass);

    // CommandPool & Buffers
    VkCommandPoolCreateInfo cp_info = {};
    cp_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp_info.queueFamilyIndex = g_QueueFamily;
    vkCreateCommandPool(g_Device, &cp_info, nullptr, &g_CommandPool);

    g_CommandBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    VkCommandBufferAllocateInfo cb_info = {};
    cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb_info.commandPool = g_CommandPool;
    cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_info.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
    vkAllocateCommandBuffers(g_Device, &cb_info, g_CommandBuffers.data());

    // Fences & Semaphores (полная синхронизация)
    g_Fences.resize(MAX_FRAMES_IN_FLIGHT);
    g_Semaphores.resize(MAX_FRAMES_IN_FLIGHT);
    VkFenceCreateInfo fence_info = {};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphoreCreateInfo sema_info = {};
    sema_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkCreateFence(g_Device, &fence_info, nullptr, &g_Fences[i]);
        vkCreateSemaphore(g_Device, &sema_info, nullptr, &g_Semaphores[i]);
    }

    // Создаем Framebuffers
    for (size_t i = 0; i < g_SwapchainImageViews.size(); i++) {
        VkFramebufferCreateInfo fb_info = {};
        fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.renderPass = g_RenderPass;
        fb_info.attachmentCount = 1;
        fb_info.pAttachments = &g_SwapchainImageViews[i];
        fb_info.width = g_SwapchainExtent.width;
        fb_info.height = g_SwapchainExtent.height;
        fb_info.layers = 1;
        vkCreateFramebuffer(g_Device, &fb_info, nullptr, &g_Framebuffers[i]);
    }

    // ImGui Init
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = VK_NULL_HANDLE;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = g_QueueFamily;
    init_info.Queue = g_Queue;
    init_info.DescriptorPool = g_DescriptorPool;
    init_info.MinImageCount = g_SwapchainImages.size();
    init_info.ImageCount = g_SwapchainImages.size();
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ImGui_ImplAndroid_Init(nullptr);
    init_info.RenderPass = g_RenderPass;
ImGui_ImplVulkan_Init(&init_info);
    

    g_Initialized = true;
    LOGI("[+] ImGui Vulkan initialized!");
}

// --- 5. ХУК vkQueuePresentKHR ---
typedef VkResult (*vkQueuePresentKHR_t)(VkQueue, const VkPresentInfoKHR*);
static vkQueuePresentKHR_t orig_vkQueuePresentKHR = nullptr;

VkResult hook_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    if (!g_Initialized && g_Device != VK_NULL_HANDLE && g_Swapchain != VK_NULL_HANDLE) {
        SetupImGui();
    }

    if (g_Initialized && pPresentInfo->swapchainCount > 0) {
        uint32_t imageIndex = pPresentInfo->pImageIndices[0];

        vkWaitForFences(g_Device, 1, &g_Fences[g_CurrentFrame], VK_TRUE, UINT64_MAX);
        vkResetFences(g_Device, 1, &g_Fences[g_CurrentFrame]);

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplAndroid_NewFrame();
        ImGui::NewFrame();
        if (g_menu_open) {
            ImGui::Begin("Iptade", &g_menu_open);
            ImGui::Text("Vulkan ESP Menu (Optimized)");
            ImGui::End();
        }
        ImGui::Render();

        VkCommandBuffer cmd = g_CommandBuffers[g_CurrentFrame];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cmd, &begin_info);

        VkRenderPassBeginInfo rp_begin = {};
        rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_begin.renderPass = g_RenderPass;
        rp_begin.framebuffer = g_Framebuffers[imageIndex];
        rp_begin.renderArea.extent = g_SwapchainExtent; // Используем реальный размер
        rp_begin.clearValueCount = 0;
        vkCmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);

        // Отправка команд с семафором
        VkSubmitInfo submit_info = {};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &cmd;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &g_Semaphores[g_CurrentFrame];
        
        vkQueueSubmit(queue, 1, &submit_info, g_Fences[g_CurrentFrame]);
        
        g_CurrentFrame = (g_CurrentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    return orig_vkQueuePresentKHR(queue, pPresentInfo);
}

// --- MAIN THREAD ---
static void* MainThread(void*) {
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Защита
    HideFromMaps("libiptade.so");
    if (IsDebuggerAttached()) {
        LOGE("[-] Debugger detected!");
        return nullptr;
    }

    // Хукаем Вулкан
    void* handle = dlopen("libvulkan.so", RTLD_NOW | RTLD_NOLOAD);
    if (!handle) handle = dlopen("libvulkan.so", RTLD_NOW);
    if (handle) {
        void* addr1 = DobbySymbolResolver("libvulkan.so", "vkCreateDevice");
        if (addr1) DobbyHook(addr1, (void*)hook_vkCreateDevice, (void**)&orig_vkCreateDevice);

        void* addr2 = DobbySymbolResolver("libvulkan.so", "vkCreateSwapchainKHR");
        if (addr2) DobbyHook(addr2, (void*)hook_vkCreateSwapchainKHR, (void**)&orig_vkCreateSwapchainKHR);

        void* addr3 = DobbySymbolResolver("libvulkan.so", "vkQueuePresentKHR");
        if (addr3) DobbyHook(addr3, (void*)hook_vkQueuePresentKHR, (void**)&orig_vkQueuePresentKHR);
        
        LOGI("[+] Vulkan hooks installed!");
    } else {
        LOGE("[-] libvulkan.so not found");
    }

    // Хукаем тачи (ввод)
    void* android_lib = dlopen("libandroid.so", RTLD_NOW);
    if (android_lib) {
        void* addr_input = DobbySymbolResolver("libandroid.so", "ANativeActivity_onInputEvent");
        if (addr_input) {
            DobbyHook(addr_input, (void*)hook_onInputEvent, (void**)&orig_onInputEvent);
            LOGI("[+] Input hook installed");
        }
    }

    return nullptr;
}

extern "C" JNI_EXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    pthread_t t;
    if (pthread_create(&t, nullptr, MainThread, nullptr) == 0)
        pthread_detach(t);
    return JNI_VERSION_1_6;
}

extern "C" JNI_EXPORT void JNI_OnUnload(JavaVM* vm, void* reserved) {
    g_running.store(false);
}
