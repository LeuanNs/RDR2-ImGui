// Code modified from: https://github.com/bruhmoment21/UniversalHookX/blob/main/UniversalHookX/src/hooks/backend/vulkan/hook_vulkan.cpp
// Big thanks to bruhmoment21

#include "Manager.h"
#include "../render.h"
#include <vector>
#include <vulkan/vulkan.h>
#pragma comment(lib, "vulkan-1.lib")

static VkAllocationCallbacks* g_Allocator = NULL;
static VkInstance g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice g_TempDevice = VK_NULL_HANDLE;
static VkDevice g_Device = VK_NULL_HANDLE;
 
static uint32_t g_QueueFamily = (uint32_t)-1;
static std::vector<VkQueueFamilyProperties> g_QueueFamilies;
 
static VkPipelineCache g_PipelineCache = VK_NULL_HANDLE;
static VkDescriptorPool g_DescriptorPool = VK_NULL_HANDLE;
static uint32_t g_MinImageCount = 2;
static VkRenderPass g_RenderPass = VK_NULL_HANDLE;
static ImGui_ImplVulkanH_Frame g_Frames[8] = {};
static ImGui_ImplVulkanH_FrameSemaphores g_FrameSemaphores[8] = {};
static VkExtent2D g_ImageExtent = {};

static volatile bool g_ResizePending = false;
static volatile bool g_ImGuiNeedsReinit = false;
static volatile bool g_FullResetPending = false;

// Contexto unico compartido entre hilos (ImGui es thread-local por defecto;
// sin esto cada hilo de present crearia su propio contexto invisible).
static ImGuiContext* g_ImGuiContext = nullptr;
static HWND g_LastKnownHwnd = nullptr;

// ---------------------------------------------------------------------
//  Tracking de swapchains: el juego presenta en varios (la ventana
//  principal + overlays de Social Club/Steam, cada uno en su hilo).
//  Solo dibujamos ImGui en el swapchain de la ventana principal.
// ---------------------------------------------------------------------
static CRITICAL_SECTION g_SwapchainCS;

struct SwapchainInfo
{
	VkSwapchainKHR handle;
	uint32_t width;
	uint32_t height;
};

static SwapchainInfo g_Swapchains[16] = {};
static uint32_t g_SwapchainCount = 0;
static VkSwapchainKHR g_MainSwapchain = VK_NULL_HANDLE;

static void TrackSwapchainLocked(VkSwapchainKHR sc, uint32_t w, uint32_t h)
{
	for (uint32_t i = 0; i < g_SwapchainCount; ++i) {
		if (g_Swapchains[i].handle == sc) {
			g_Swapchains[i].width = w;
			g_Swapchains[i].height = h;
			return;
		}
	}
	if (g_SwapchainCount < 16) {
		g_Swapchains[g_SwapchainCount].handle = sc;
		g_Swapchains[g_SwapchainCount].width = w;
		g_Swapchains[g_SwapchainCount].height = h;
		g_SwapchainCount++;
	}
}

static void UntrackSwapchainLocked(VkSwapchainKHR sc)
{
	for (uint32_t i = 0; i < g_SwapchainCount; ++i) {
		if (g_Swapchains[i].handle == sc) {
			g_Swapchains[i] = g_Swapchains[g_SwapchainCount - 1];
			g_Swapchains[g_SwapchainCount - 1] = {};
			g_SwapchainCount--;
			return;
		}
	}
}

static bool GetSwapchainExtent(VkSwapchainKHR sc, uint32_t* w, uint32_t* h)
{
	bool found = false;
	EnterCriticalSection(&g_SwapchainCS);
	for (uint32_t i = 0; i < g_SwapchainCount; ++i) {
		if (g_Swapchains[i].handle == sc) {
			if (w) *w = g_Swapchains[i].width;
			if (h) *h = g_Swapchains[i].height;
			found = true;
			break;
		}
	}
	LeaveCriticalSection(&g_SwapchainCS);
	return found;
}

static bool ExtentMatchesGameWindow(uint32_t w, uint32_t h)
{
	HWND hWnd = FindWindowA("sgaWindow", "Red Dead Redemption 2");
	if (!hWnd)
		hWnd = FindWindowA(NULL, "Red Dead Redemption 2");

	if (!hWnd)
		return false;

	RECT rect{};
	if (GetClientRect(hWnd, &rect))
	{
		const uint32_t clientW = (uint32_t)(rect.right - rect.left);
		const uint32_t clientH = (uint32_t)(rect.bottom - rect.top);

		// Tolerancia de ±2 píxeles para modo ventana/borderless (DPI scaling, bordes)
		const uint32_t tolerance = 2;
		const bool clientMatch = (w >= clientW - tolerance && w <= clientW + tolerance) &&
		                         (h >= clientH - tolerance && h <= clientH + tolerance);

		if (clientMatch)
			return true;

		RECT windowRect{};
		if (GetWindowRect(hWnd, &windowRect))
		{
			HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
			MONITORINFO mi{};
			mi.cbSize = sizeof(mi);
			if (GetMonitorInfo(hMonitor, &mi))
			{
				const uint32_t monW = mi.rcMonitor.right - mi.rcMonitor.left;
				const uint32_t monH = mi.rcMonitor.bottom - mi.rcMonitor.top;

				if ((windowRect.left == mi.rcMonitor.left && windowRect.top == mi.rcMonitor.top &&
				     windowRect.right == mi.rcMonitor.right && windowRect.bottom == mi.rcMonitor.bottom) &&
				    (w == monW && h == monH))
				{
					return true;
				}
			}
		}
	}
	return false;
}


static bool CreateDeviceVK()
{
	// Create Vulkan Instance
	{
		VkInstanceCreateInfo create_info = {};
		constexpr const char* instance_extension = VK_KHR_SURFACE_EXTENSION_NAME;

		create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		create_info.enabledExtensionCount = 1;
		create_info.ppEnabledExtensionNames = &instance_extension;

		// Create Vulkan Instance without any debug feature
		if (vkCreateInstance(&create_info, g_Allocator, &g_Instance) != VK_SUCCESS) {
			Log("[!] Vulkan: vkCreateInstance() FAILED for g_Instance");
			return false;
		}
	}

	// Select GPU
	{
		uint32_t gpu_count{};
		vkEnumeratePhysicalDevices(g_Instance, &gpu_count, NULL);
		IM_ASSERT(gpu_count > 0);

		VkPhysicalDevice* gpus = new VkPhysicalDevice[sizeof(VkPhysicalDevice) * gpu_count];
		vkEnumeratePhysicalDevices(g_Instance, &gpu_count, gpus);

		// If a number >1 of GPUs got reported, find discrete GPU if present, or use first one available. This covers
		// most common cases (multi-gpu/integrated+dedicated graphics). Handling more complicated setups (multiple
		// dedicated GPUs) is out of scope of this sample.
		int use_gpu = 0;
		for (int i = 0; i < (int)gpu_count; ++i) {
			VkPhysicalDeviceProperties properties{};
			vkGetPhysicalDeviceProperties(gpus[i], &properties);
			if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
				use_gpu = i;
				break;
			}
		}

		g_PhysicalDevice = gpus[use_gpu];
		delete[] gpus;
	}

	// Select graphics queue family
	{
		uint32_t count{};
		vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &count, NULL);
		g_QueueFamilies.resize(count);
		vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &count, g_QueueFamilies.data());
		for (uint32_t i = 0; i < count; ++i) {
			if (g_QueueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
				g_QueueFamily = i;
				break;
			}
		}
		IM_ASSERT(g_QueueFamily != (uint32_t)-1);
	}

	// Create Logical Device (with no queue)
	{
		constexpr const char* device_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
		constexpr float queue_priority = 1.0f;

		VkDeviceQueueCreateInfo queue_info = {};
		queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queue_info.queueFamilyIndex = g_QueueFamily;
		queue_info.queueCount = 1;
		queue_info.pQueuePriorities = &queue_priority;

		VkDeviceCreateInfo create_info = {};
		create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		create_info.queueCreateInfoCount = 1;
		create_info.pQueueCreateInfos = &queue_info;
		create_info.enabledExtensionCount = 1;
		create_info.ppEnabledExtensionNames = &device_extension;

		VkResult result = vkCreateDevice(g_PhysicalDevice, &create_info, g_Allocator, &g_TempDevice);
		if (result != VK_SUCCESS) {
			Log("[!] Vulkan: vkCreateDevice() FAILED for g_TempDevice");
			return false;
		}
	}

	return true;
}

static void CreateRenderTarget(VkDevice device, VkSwapchainKHR swapchain)
{
	uint32_t uImageCount;
	vkGetSwapchainImagesKHR(device, swapchain, &uImageCount, NULL);

	VkImage backbuffers[8] = {};
	vkGetSwapchainImagesKHR(device, swapchain, &uImageCount, backbuffers);

	for (uint32_t i = 0; i < uImageCount; ++i) {
		g_Frames[i].Backbuffer = backbuffers[i];

		ImGui_ImplVulkanH_Frame* fd = &g_Frames[i];
		ImGui_ImplVulkanH_FrameSemaphores* fsd = &g_FrameSemaphores[i];
		{
			VkCommandPoolCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
			info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			info.queueFamilyIndex = g_QueueFamily;

			vkCreateCommandPool(device, &info, g_Allocator, &fd->CommandPool);
		}
		{
			VkCommandBufferAllocateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			info.commandPool = fd->CommandPool;
			info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			info.commandBufferCount = 1;

			vkAllocateCommandBuffers(device, &info, &fd->CommandBuffer);
		}
		{
			VkFenceCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
			info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			vkCreateFence(device, &info, g_Allocator, &fd->Fence);
		}
		{
			VkSemaphoreCreateInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
			vkCreateSemaphore(device, &info, g_Allocator, &fsd->ImageAcquiredSemaphore);
			vkCreateSemaphore(device, &info, g_Allocator, &fsd->RenderCompleteSemaphore);
		}
	}

	// Create the Render Pass
	{
		VkAttachmentDescription attachment = {};
		attachment.format = VK_FORMAT_B8G8R8A8_UNORM;
		attachment.samples = VK_SAMPLE_COUNT_1_BIT;
		attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkAttachmentReference color_attachment = {};
		color_attachment.attachment = 0;
		color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass = {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &color_attachment;

		VkRenderPassCreateInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		info.attachmentCount = 1;
		info.pAttachments = &attachment;
		info.subpassCount = 1;
		info.pSubpasses = &subpass;

		vkCreateRenderPass(device, &info, g_Allocator, &g_RenderPass);
	}

	// Create The Image Views
	{
		VkImageViewCreateInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		info.format = VK_FORMAT_B8G8R8A8_UNORM;

		info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		info.subresourceRange.baseMipLevel = 0;
		info.subresourceRange.levelCount = 1;
		info.subresourceRange.baseArrayLayer = 0;
		info.subresourceRange.layerCount = 1;

		for (uint32_t i = 0; i < uImageCount; ++i) {
			ImGui_ImplVulkanH_Frame* fd = &g_Frames[i];
			info.image = fd->Backbuffer;
			vkCreateImageView(device, &info, g_Allocator, &fd->BackbufferView);
		}
	}

	// Create Framebuffer
	{
		VkImageView attachment[1];
		VkFramebufferCreateInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		info.renderPass = g_RenderPass;
		info.attachmentCount = 1;
		info.pAttachments = attachment;
		info.width = g_ImageExtent.width;
		info.height = g_ImageExtent.height;
		info.layers = 1;

		for (uint32_t i = 0; i < uImageCount; ++i) {
			ImGui_ImplVulkanH_Frame* fd = &g_Frames[i];
			attachment[0] = fd->BackbufferView;
			vkCreateFramebuffer(device, &info, g_Allocator, &fd->Framebuffer);
		}
	}

	// Create Descriptor Pool.
	if (!g_DescriptorPool)
	{
		constexpr VkDescriptorPoolSize pool_sizes[] =
		{
			{VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
			{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
			{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
			{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
			{VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
			{VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
			{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
			{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
			{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
			{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
			{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000}
		};

		VkDescriptorPoolCreateInfo pool_info = {};
		pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		pool_info.maxSets = 1000 * IM_ARRAYSIZE(pool_sizes);
		pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
		pool_info.pPoolSizes = pool_sizes;

		vkCreateDescriptorPool(device, &pool_info, g_Allocator, &g_DescriptorPool);
	}
}

static void CleanupRenderTarget()
{
	for (uint32_t i = 0; i < RTL_NUMBER_OF(g_Frames); ++i) {
		if (g_Frames[i].Fence) {
			vkDestroyFence(g_Device, g_Frames[i].Fence, g_Allocator);
			g_Frames[i].Fence = VK_NULL_HANDLE;
		}
		if (g_Frames[i].CommandBuffer) {
			vkFreeCommandBuffers(g_Device, g_Frames[i].CommandPool, 1, &g_Frames[i].CommandBuffer);
			g_Frames[i].CommandBuffer = VK_NULL_HANDLE;
		}
		if (g_Frames[i].CommandPool) {
			vkDestroyCommandPool(g_Device, g_Frames[i].CommandPool, g_Allocator);
			g_Frames[i].CommandPool = VK_NULL_HANDLE;
		}
		if (g_Frames[i].BackbufferView) {
			vkDestroyImageView(g_Device, g_Frames[i].BackbufferView, g_Allocator);
			g_Frames[i].BackbufferView = VK_NULL_HANDLE;
		}
		if (g_Frames[i].Framebuffer) {
			vkDestroyFramebuffer(g_Device, g_Frames[i].Framebuffer, g_Allocator);
			g_Frames[i].Framebuffer = VK_NULL_HANDLE;
		}
	}

	for (uint32_t i = 0; i < RTL_NUMBER_OF(g_FrameSemaphores); ++i) {
		if (g_FrameSemaphores[i].ImageAcquiredSemaphore) {
			vkDestroySemaphore(g_Device, g_FrameSemaphores[i].ImageAcquiredSemaphore, g_Allocator);
			g_FrameSemaphores[i].ImageAcquiredSemaphore = VK_NULL_HANDLE;
		}
		if (g_FrameSemaphores[i].RenderCompleteSemaphore) {
			vkDestroySemaphore(g_Device, g_FrameSemaphores[i].RenderCompleteSemaphore, g_Allocator);
			g_FrameSemaphores[i].RenderCompleteSemaphore = VK_NULL_HANDLE;
		}
	}
}

static void CleanupDeviceVulkan()
{
	if (g_Device) {
		vkDeviceWaitIdle(g_Device);
	}

	CleanupRenderTarget();

	if (g_DescriptorPool) {
		vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);
		g_DescriptorPool = VK_NULL_HANDLE;
	}
	
	// See #5 on GitHub (https://github.com/Halen84/ImGuiRDR2Hook/issues/5)
	//if (g_Instance) {
	//	vkDestroyInstance(g_Instance, g_Allocator);
	//	g_Instance = NULL;
	//}

	g_ImageExtent = {};
	g_Device = VK_NULL_HANDLE;
}

static bool DoesQueueSupportGraphic(VkQueue queue, VkQueue* pGraphicQueue)
{
	for (uint32_t i = 0; i < g_QueueFamilies.size(); ++i) {
		const VkQueueFamilyProperties& family = g_QueueFamilies[i];
		for (uint32_t j = 0; j < family.queueCount; ++j) {
			VkQueue it = VK_NULL_HANDLE;
			vkGetDeviceQueue(g_Device, i, j, &it);

			if (pGraphicQueue && family.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
				if (*pGraphicQueue == VK_NULL_HANDLE) {
					*pGraphicQueue = it;
				}
			}

			if (queue == it && family.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
				return true;
			}
		}
	}

	return false;
}

static VkSwapchainKHR PickMainSwapchain(const VkPresentInfoKHR* pPresentInfo)
{
	if (g_MainSwapchain != VK_NULL_HANDLE)
	{
		for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
			if (pPresentInfo->pSwapchains[i] == g_MainSwapchain)
				return g_MainSwapchain;
		}
		// Este frame solo presenta overlays: no dibujar nada
		return VK_NULL_HANDLE;
	}

	// Fallback: el swapchain principal no se ha identificado todavia
	// (su vkCreateSwapchainKHR no se intercepto o el tamano no coincidia
	// con la ventana en ese momento). Usamos el mas grande presentado.
	VkSwapchainKHR best = VK_NULL_HANDLE;
	uint32_t bestArea = 0, bw = 0, bh = 0;
	for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
	{
		uint32_t w = 0, h = 0;
		if (GetSwapchainExtent(pPresentInfo->pSwapchains[i], &w, &h) && w * h > bestArea)
		{
			bestArea = w * h;
			best = pPresentInfo->pSwapchains[i];
			bw = w; bh = h;
		}
	}

	if (best != VK_NULL_HANDLE)
	{
		g_MainSwapchain = best;
		Log("[+] Vulkan: main swapchain fallback (largest) = %p (%ux%u)", (void*)best, bw, bh);
	}
	return best;
}

// Reset completo de ImGui: destruye contexto + backends y recrea todo desde cero.
// Se usa cuando el juego cambia de ventana (alt+tab, cambio de resolución, toggle fullscreen).
static void FullImGuiReset(HWND newHwnd)
{
	Log("[+] Vulkan: Starting full ImGui reset (newHwnd=%p)", newHwnd);

	// 1. Esperar a que la GPU termine de usar los recursos
	if (g_Device) {
		for (uint32_t i = 0; i < RTL_NUMBER_OF(g_Frames); ++i) {
			if (g_Frames[i].Fence != VK_NULL_HANDLE) {
				vkWaitForFences(g_Device, 1, &g_Frames[i].Fence, VK_TRUE, 1000000000);
			}
		}
	}

	// 2. Destruir el contexto ImGui y backends si existen
	if (g_ImGuiContext) {
		ImGui::SetCurrentContext(g_ImGuiContext);
		
		if (ImGui::GetIO().BackendRendererUserData) {
			ImGui_ImplVulkan_Shutdown();
		}
		if (ImGui::GetIO().BackendPlatformUserData) {
			ImGui_ImplWin32_Shutdown();
		}
		
		ImGui::DestroyContext();
		g_ImGuiContext = nullptr;
	}

	// 3. Limpiar render targets
	CleanupRenderTarget();

	// 4. Actualizar ventana del juego
	CImGuiHookManager::SetGameWindow(newHwnd);
	g_LastKnownHwnd = newHwnd;

	// 5. Recrear contexto ImGui desde cero
	ImGui::CreateContext();
	g_ImGuiContext = ImGui::GetCurrentContext();
	ImGui_ImplWin32_Init(newHwnd);

	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = io.LogFilename = NULL;
	io.SetClipboardTextFn = nullptr;
	io.GetClipboardTextFn = nullptr;
	ImGui::StyleColorsDark();

	// Cargar fuentes
	io.Fonts->AddFontDefault();
	ImFontConfig config;
	config.SizePixels = 34.0f;
	ImFont* mvBoli = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\mvboli.ttf", 34.0f, &config);
	if (!mvBoli) {
		io.Fonts->AddFontDefault();
		Log("[!] Vulkan: mvboli.ttf not found, using default font");
	}
	io.Fonts->AddFontDefault();

	unsigned char* pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

	// 6. Re-subclasear WndProc
	CImGuiHookManager::GetWin32().Hook();

	// 7. Reset flags
	g_ImGuiNeedsReinit = true;
	g_FullResetPending = false;

	Log("[+] Vulkan: Full ImGui reset completed (hWnd=%p)", newHwnd);
}

static void RenderImGui_Vulkan(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
	if (!g_Device || CImGuiHookManager::IsShutdownRequested())
		return;

	// Filtro: solo dibujamos en el swapchain de la ventana principal del
	// juego. Los overlays (Social Club/Steam) presentan en otros
	// swapchains, normalmente en otro hilo, y hay que ignorarlos.
	const VkSwapchainKHR targetSwapchain = PickMainSwapchain(pPresentInfo);
	if (targetSwapchain == VK_NULL_HANDLE)
		return;

	// Detectar cambio de ventana del juego (alt+tab, cambio de resolución, toggle fullscreen)
	// Si la ventana cambió, hacer reset completo de ImGui
	HWND currentGameHwnd = CImGuiHookManager::GetGameWindow();
	if (currentGameHwnd && currentGameHwnd != g_LastKnownHwnd)
	{
		EnterCriticalSection(&g_SwapchainCS);
		if (currentGameHwnd != g_LastKnownHwnd) // re-check dentro del lock
		{
			FullImGuiReset(currentGameHwnd);
		}
		LeaveCriticalSection(&g_SwapchainCS);
	}

	if (g_ResizePending)
	{
		g_ResizePending = false;
		
		// 1. ESPERAR a que la GPU termine de usar los recursos actuales antes de destruirlos.
		// Usamos fences individuales en vez de vkDeviceWaitIdle para evitar hangs.
		for (uint32_t i = 0; i < RTL_NUMBER_OF(g_Frames); ++i) {
			if (g_Frames[i].Fence != VK_NULL_HANDLE) {
				vkWaitForFences(g_Device, 1, &g_Frames[i].Fence, VK_TRUE, 1000000000); // 1 segundo timeout
			}
		}

		// 2. DESTRUIR los ImageViews y Framebuffers viejos.
		for (uint32_t i = 0; i < RTL_NUMBER_OF(g_Frames); ++i) {
			if (g_Frames[i].BackbufferView != VK_NULL_HANDLE) {
				vkDestroyImageView(g_Device, g_Frames[i].BackbufferView, g_Allocator);
				g_Frames[i].BackbufferView = VK_NULL_HANDLE;
			}
			if (g_Frames[i].Framebuffer != VK_NULL_HANDLE) {
				vkDestroyFramebuffer(g_Device, g_Frames[i].Framebuffer, g_Allocator);
				g_Frames[i].Framebuffer = VK_NULL_HANDLE;
			}
		}
		
		g_ImGuiNeedsReinit = true;
	}

	{
		uint32_t w = 0, h = 0;
		if (GetSwapchainExtent(targetSwapchain, &w, &h) && w > 0 && h > 0)
		{
			g_ImageExtent.width = w;
			g_ImageExtent.height = h;
		}
		else if (g_ImageExtent.width == 0 || g_ImageExtent.height == 0)
		{
			HWND gameHwnd = CImGuiHookManager::GetGameWindow();
			if (gameHwnd)
			{
				RECT rect{};
				if (GetClientRect(gameHwnd, &rect) && rect.right > rect.left && rect.bottom > rect.top)
				{
					g_ImageExtent.width = (uint32_t)(rect.right - rect.left);
					g_ImageExtent.height = (uint32_t)(rect.bottom - rect.top);
					Log("[+] Vulkan: g_ImageExtent fallback from GetClientRect: %ux%u",
						g_ImageExtent.width, g_ImageExtent.height);
				}
				else
				{
					HMONITOR hMonitor = MonitorFromWindow(gameHwnd, MONITOR_DEFAULTTONEAREST);
					MONITORINFO mi{};
					mi.cbSize = sizeof(mi);
					if (GetMonitorInfo(hMonitor, &mi))
					{
						g_ImageExtent.width = (uint32_t)(mi.rcMonitor.right - mi.rcMonitor.left);
						g_ImageExtent.height = (uint32_t)(mi.rcMonitor.bottom - mi.rcMonitor.top);
						Log("[+] Vulkan: g_ImageExtent fallback from Monitor: %ux%u",
							g_ImageExtent.width, g_ImageExtent.height);
					}
				}
			}
		}
	}

	VkQueue graphicQueue = VK_NULL_HANDLE;
	const bool queueSupportsGraphic = DoesQueueSupportGraphic(queue, &graphicQueue);

	// Un unico contexto global (los contextos ImGui son thread-local; si
	// usamos GetCurrentContext() cada hilo de present crearia el suyo).
	if (!g_ImGuiContext) {
		EnterCriticalSection(&g_SwapchainCS);
		if (!g_ImGuiContext) {
			CImGuiHookManager::SetGameWindow(FindWindowA("sgaWindow", "Red Dead Redemption 2"));
			if (!CImGuiHookManager::GetGameWindow()) {
				// Fallback: buscar solo por titulo (la clase puede variar)
				CImGuiHookManager::SetGameWindow(FindWindowA(NULL, "Red Dead Redemption 2"));
				Log("[!] Vulkan: FindWindowA(\"sgaWindow\") failed, title-only lookup hWnd=%p", CImGuiHookManager::GetGameWindow());
			}

			ImGui::CreateContext();
			g_ImGuiContext = ImGui::GetCurrentContext();
			ImGui_ImplWin32_Init(CImGuiHookManager::GetGameWindow());
			g_LastKnownHwnd = CImGuiHookManager::GetGameWindow();

			static bool s_win32Hooked = false;
			if (!s_win32Hooked) {
				CImGuiHookManager::GetWin32().Hook();
				s_win32Hooked = true;
			}

			ImGuiIO& io = ImGui::GetIO();
			io.IniFilename = io.LogFilename = NULL;
			// TODO p2#8: Desactivar clipboard para reducir falsos positivos de AV
			io.SetClipboardTextFn = nullptr;
			io.GetClipboardTextFn = nullptr;
			ImGui::StyleColorsDark();

			// Cargar fuentes: indice 0 = default, indice 1 = MV Boli (manuscrita), indice 2 = default (alternativa)
			io.Fonts->AddFontDefault(); // Indice 0: fuente por defecto (fallback)
			ImFontConfig config;
			config.SizePixels = 34.0f;
			ImFont* mvBoli = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\mvboli.ttf", 34.0f, &config);
			if (!mvBoli)
			{
				// Fallback si MV Boli no esta disponible
				io.Fonts->AddFontDefault();
				Log("[!] Vulkan: mvboli.ttf not found, using default font");
			}
			io.Fonts->AddFontDefault(); // Indice 2: fuente alternativa para toggle F

			unsigned char* pixels;
			int width, height;
			io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

			Log("[+] Vulkan: ImGui context created (hWnd=%p, extent=%ux%u)",
				CImGuiHookManager::GetGameWindow(), g_ImageExtent.width, g_ImageExtent.height);
		}
		LeaveCriticalSection(&g_SwapchainCS);
	}
	ImGui::SetCurrentContext(g_ImGuiContext);

	// Si el juego creo el swapchain ANTES de instalar los hooks, nunca vimos
	// vkCreateSwapchainKHR y g_ImageExtent quedo en {0,0}: los framebuffers se
	// crearian con tamano 0x0 y no se dibujaria absolutamente nada. En ese
	// caso deducimos el tamano de la ventana del juego.
	// TODO #8: En pantalla completa exclusiva, GetClientRect puede fallar,
	// asi que tambien intentamos obtener la resolucion del monitor.
	if (g_ImageExtent.width == 0 || g_ImageExtent.height == 0) {
		HWND gameHwnd = CImGuiHookManager::GetGameWindow();
		if (gameHwnd)
		{
			RECT rect{};
			if (GetClientRect(gameHwnd, &rect) &&
			    rect.right > rect.left && rect.bottom > rect.top) {
				g_ImageExtent.width = (uint32_t)(rect.right - rect.left);
				g_ImageExtent.height = (uint32_t)(rect.bottom - rect.top);
				Log("[+] Vulkan: g_ImageExtent fallback from GetClientRect: %ux%u",
					g_ImageExtent.width, g_ImageExtent.height);
			}
			else
			{
				// Fullscreen exclusivo: usar la resolucion del monitor
				HMONITOR hMonitor = MonitorFromWindow(gameHwnd, MONITOR_DEFAULTTONEAREST);
				MONITORINFO mi{};
				mi.cbSize = sizeof(mi);
				if (GetMonitorInfo(hMonitor, &mi))
				{
					g_ImageExtent.width = (uint32_t)(mi.rcMonitor.right - mi.rcMonitor.left);
					g_ImageExtent.height = (uint32_t)(mi.rcMonitor.bottom - mi.rcMonitor.top);
					Log("[+] Vulkan: g_ImageExtent fallback from Monitor (fullscreen): %ux%u",
						g_ImageExtent.width, g_ImageExtent.height);
				}
			}
		}
	}

	for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
	{
		VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[i];
		if (swapchain != targetSwapchain)
			continue; // Overlay: no dibujar aqui

		// Check si necesitamos recrear el render target
		if (g_Frames[0].Framebuffer == VK_NULL_HANDLE) {
			CreateRenderTarget(g_Device, swapchain);
		}

		// TODO FASE7#3: el chequeo de foco se movio ACA, antes de tocar
		// fences/command buffer. Antes se hacia mas abajo (despues de
		// vkResetFences + vkBeginCommandBuffer + vkCmdBeginRenderPass), y al
		// perder el foco se hacia "continue" sin nunca llamar a
		// vkQueueSubmit: el fence quedaba reseteado pero jamas señalado, asi
		// que la proxima vez que se reusaba ese mismo indice de imagen,
		// vkWaitForFences(..., VK_TRUE, ~0ull) se colgaba para siempre
		// (freeze). Esto explica los crashes/cuelgues en ventana y ventana
		// sin bordes, donde perder el foco (alt-tab, click afuera) es mucho
		// mas comun que en pantalla completa exclusiva.
		// Check de foco más leniente para ventana sin bordes:
		// En borderless windowed, el foreground window puede ser diferente
		// (wrapper del juego, overlay, etc). Verificamos que la ventana del
		// juego esté visible y no minimizada.
		{
			HWND gameHwndFocusCheck = CImGuiHookManager::GetGameWindow();
			bool hasFocusEarly = false;
			if (gameHwndFocusCheck)
			{
				// Verificar si la ventana es visible y no minimizada
				const bool isVisible = IsWindowVisible(gameHwndFocusCheck);
				const bool isMinimized = IsIconic(gameHwndFocusCheck);
				const bool isForeground = (GetForegroundWindow() == gameHwndFocusCheck);
				
				// Aceptar si es foreground, O si está visible y no minimizada
				// (esto cubre el caso de borderless windowed)
				hasFocusEarly = isForeground || (isVisible && !isMinimized);
			}
			
			if (!hasFocusEarly)
			{
				static DWORD s_lastFocusLog = 0;
				DWORD now = GetTickCount();
				if (now - s_lastFocusLog > 2000)
				{
					HWND fg = GetForegroundWindow();
					Log("[+] Vulkan: Focus check failed (gameHwnd=%p, foreground=%p, visible=%d, minimized=%d), skipping frame",
						gameHwndFocusCheck, fg, 
						gameHwndFocusCheck ? IsWindowVisible(gameHwndFocusCheck) : 0,
						gameHwndFocusCheck ? IsIconic(gameHwndFocusCheck) : 0);
					s_lastFocusLog = now;
				}
				continue;
			}
		}

		ImGui_ImplVulkanH_Frame* fd = &g_Frames[pPresentInfo->pImageIndices[i]];
		ImGui_ImplVulkanH_FrameSemaphores* fsd = &g_FrameSemaphores[pPresentInfo->pImageIndices[i]];
		{
			vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, ~0ull);
			vkResetFences(g_Device, 1, &fd->Fence);
		}
		{
			vkResetCommandBuffer(fd->CommandBuffer, 0);

			VkCommandBufferBeginInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

			vkBeginCommandBuffer(fd->CommandBuffer, &info);
		}

		// Debe hacerse FUERA del render pass: la subida de la fuente usa
		// comandos de transferencia (vkCmdCopyBufferToImage) que no son
		// validos dentro de un subpass solo de graficos.
		if (!ImGui::GetIO().BackendRendererUserData || g_ImGuiNeedsReinit)
		{
			// Si ya estaba inicializado, no llamar Shutdown (destruye el descriptor pool)
			// Solo re-inicializar con el nuevo render pass
			ImGui_ImplVulkan_InitInfo init_info = {};
			init_info.Instance = g_Instance;
			init_info.PhysicalDevice = g_PhysicalDevice;
			init_info.Device = g_Device;
			init_info.QueueFamily = g_QueueFamily;
			init_info.Queue = graphicQueue;
			init_info.PipelineCache = g_PipelineCache;
			init_info.DescriptorPool = g_DescriptorPool;
			init_info.Subpass = 0;
			init_info.MinImageCount = g_MinImageCount;
			init_info.ImageCount = g_MinImageCount;
			init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
			init_info.Allocator = g_Allocator;
			ImGui_ImplVulkan_Init(&init_info, g_RenderPass);
			ImGui_ImplVulkan_CreateFontsTexture(fd->CommandBuffer);

			g_ImGuiNeedsReinit = false;
		}

		{
			VkRenderPassBeginInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			info.renderPass = g_RenderPass;
			info.framebuffer = fd->Framebuffer;
			if (g_ImageExtent.width == 0 || g_ImageExtent.height == 0) {
				// We don't know the window size the first time. So we just set it to 4K.
				// TODO: Maybe default to 1920x1080
				info.renderArea.extent.width = 3840;
				info.renderArea.extent.height = 2160;
			}
			else {
				info.renderArea.extent = g_ImageExtent;
			}

			vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
		}

		// TODO FASE7#3: el chequeo de foco ya se hizo mas arriba, antes de
		// resetear el fence / grabar el command buffer. Ver comentario ahi.
		ImGuiIO& io = ImGui::GetIO();
		if (Render::ShouldDrawMouse()) {
			io.WantCaptureMouse = true;
			io.MouseDrawCursor = true;
			io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
		}
		else {
			io.WantCaptureMouse = false;
			io.MouseDrawCursor = false;
			io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
		}

		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		Render::Draw();
		ImGui::Render();
		ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), fd->CommandBuffer);

		vkCmdEndRenderPass(fd->CommandBuffer);
		vkEndCommandBuffer(fd->CommandBuffer);

		uint32_t waitSemaphoresCount = i == 0 ? pPresentInfo->waitSemaphoreCount : 0;
		if (waitSemaphoresCount == 0 && !queueSupportsGraphic)
		{
			constexpr VkPipelineStageFlags stages_wait = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			{
				VkSubmitInfo info = {};
				info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

				info.pWaitDstStageMask = &stages_wait;

				info.signalSemaphoreCount = 1;
				info.pSignalSemaphores = &fsd->RenderCompleteSemaphore;

				vkQueueSubmit(queue, 1, &info, VK_NULL_HANDLE);
			}
			{
				VkSubmitInfo info = {};
				info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
				info.commandBufferCount = 1;
				info.pCommandBuffers = &fd->CommandBuffer;

				info.pWaitDstStageMask = &stages_wait;
				info.waitSemaphoreCount = 1;
				info.pWaitSemaphores = &fsd->RenderCompleteSemaphore;

				info.signalSemaphoreCount = 1;
				info.pSignalSemaphores = &fsd->ImageAcquiredSemaphore;

				vkQueueSubmit(graphicQueue, 1, &info, fd->Fence);
			}
		}
		else
		{
			std::vector<VkPipelineStageFlags> stages_wait(waitSemaphoresCount, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

			VkSubmitInfo info = {};
			info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			info.commandBufferCount = 1;
			info.pCommandBuffers = &fd->CommandBuffer;

			info.pWaitDstStageMask = stages_wait.data();
			info.waitSemaphoreCount = waitSemaphoresCount;
			info.pWaitSemaphores = pPresentInfo->pWaitSemaphores;

			info.signalSemaphoreCount = waitSemaphoresCount;
			info.pSignalSemaphores = pPresentInfo->pWaitSemaphores;

			vkQueueSubmit(graphicQueue, 1, &info, fd->Fence);
		}
	}
}

static std::add_pointer_t<VkResult VKAPI_CALL(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*)> oAcquireNextImageKHR;
static VkResult VKAPI_CALL hk_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex)
{
	g_Device = device;

	return oAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
}

static std::add_pointer_t<VkResult VKAPI_CALL(VkDevice, const VkAcquireNextImageInfoKHR*, uint32_t*)> oAcquireNextImage2KHR;
static VkResult VKAPI_CALL hk_vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR* pAcquireInfo, uint32_t* pImageIndex)
{
	g_Device = device;

	return oAcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);
}

static VkQueue m_queue;
static const VkPresentInfoKHR* m_pPresentInfo;

static std::add_pointer_t<VkResult VKAPI_CALL(VkQueue, const VkPresentInfoKHR*)> oQueuePresentKHR;
static VkResult VKAPI_CALL hk_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
	m_queue = queue;
	m_pPresentInfo = pPresentInfo;
	RenderImGui_Vulkan(queue, pPresentInfo);

	return oQueuePresentKHR(queue, pPresentInfo);
}

static std::add_pointer_t<VkResult VKAPI_CALL(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*)> oCreateSwapchainKHR;
static VkResult VKAPI_CALL hk_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
	const bool oldWasMain = (pCreateInfo->oldSwapchain != VK_NULL_HANDLE &&
	                          pCreateInfo->oldSwapchain == g_MainSwapchain);

	const VkResult res = oCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
	if (res != VK_SUCCESS)
		return res;

	EnterCriticalSection(&g_SwapchainCS);
	if (pCreateInfo->oldSwapchain != VK_NULL_HANDLE)
		UntrackSwapchainLocked(pCreateInfo->oldSwapchain);
	TrackSwapchainLocked(*pSwapchain, pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height);
	LeaveCriticalSection(&g_SwapchainCS);

	// TODO FASE7#3: antes se marcaba resize pending para CUALQUIER swapchain
	// recreado (incluidos overlays de Social Club/Steam), tirando abajo los
	// render targets del swapchain PRINCIPAL sin necesidad. Acotarlo a
	// oldWasMain evita ese churn innecesario, sobre todo durante cambios de
	// resolucion (momento en que hay mas actividad de swapchains a la vez).
	if (oldWasMain)
	{
		g_ResizePending = true;
		Log("[+] Vulkan: main swapchain recreated (old=%p -> new=%p), resize deferred to render thread",
			(void*)pCreateInfo->oldSwapchain, (void*)*pSwapchain);
	}

	// Es el swapchain principal si su tamaño coincide con la ventana del
	// juego, o si reemplaza directamente al swapchain principal anterior
	// (el juego lo recrea al cambiar resolucion/entrar a pantalla completa).
	const bool extentMatches = ExtentMatchesGameWindow(pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height);
	
	if (oldWasMain || extentMatches)
	{
		if (g_MainSwapchain != *pSwapchain)
		{
			// Solo hacer resize si el extent realmente cambió
			const bool extentChanged = (g_ImageExtent.width != pCreateInfo->imageExtent.width ||
			                            g_ImageExtent.height != pCreateInfo->imageExtent.height);
			
			if (g_MainSwapchain != VK_NULL_HANDLE && extentChanged)
			{
				g_ResizePending = true;
				Log("[+] Vulkan: main swapchain changed (old=%p -> new=%p), extent %ux%u -> %ux%u, resize deferred",
					(void*)g_MainSwapchain, (void*)*pSwapchain,
					g_ImageExtent.width, g_ImageExtent.height,
					pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height);
			}
			else if (g_MainSwapchain != VK_NULL_HANDLE)
			{
				Log("[+] Vulkan: main swapchain recreated (old=%p -> new=%p), same extent %ux%u, no resize needed",
					(void*)g_MainSwapchain, (void*)*pSwapchain,
					pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height);
			}
			g_MainSwapchain = *pSwapchain;
			g_ImageExtent = pCreateInfo->imageExtent;
			if (g_MainSwapchain == *pSwapchain && g_ImageExtent.width == 0)
			{
				Log("[+] Vulkan: main swapchain = %p (%ux%u)", (void*)*pSwapchain,
					pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height);
			}
		}
	}
	else
	{
		// Fallback: si no hay main swapchain identificado y este es grande, marcarlo como candidato
		if (g_MainSwapchain == VK_NULL_HANDLE && pCreateInfo->imageExtent.width >= 1280 && pCreateInfo->imageExtent.height >= 720)
		{
			g_MainSwapchain = *pSwapchain;
			g_ImageExtent = pCreateInfo->imageExtent;
			Log("[+] Vulkan: main swapchain fallback (large swapchain) = %p (%ux%u)", (void*)*pSwapchain,
				pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height);
		}
		else
		{
			Log("[+] Vulkan: ignoring overlay swapchain = %p (%ux%u)", (void*)*pSwapchain,
				pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height);
		}
	}

	return res;
}

void CImGuiHookManager::sVK::Present()
{
	RenderImGui_Vulkan(m_queue, m_pPresentInfo);
}

void CImGuiHookManager::sVK::Hook()
{
	// El juego puede tardar en cargar vulkan-1.dll; esperamos en lugar de
	// abandonar de inmediato (esto se ejecuta en un hilo propio).
	{
		const DWORD start = GetTickCount();
		while (GetModuleHandleA("vulkan-1.dll") == NULL && GetTickCount() - start < 15000)
			Sleep(100);
	}

	if (GetModuleHandleA("vulkan-1.dll") == NULL) {
		Log("[!] Vulkan: vulkan-1.dll is not loaded.");
		return;
	}

	if (!CreateDeviceVK()) {
		Log("[!] Vulkan: CreateDeviceVK() failed.");
		return;
	}

	InitializeCriticalSection(&g_SwapchainCS);

	MH_Initialize();

	void* fpAcquireNextImageKHR  = reinterpret_cast<void*>(vkGetDeviceProcAddr(g_TempDevice, "vkAcquireNextImageKHR"));
	void* fpAcquireNextImage2KHR = reinterpret_cast<void*>(vkGetDeviceProcAddr(g_TempDevice, "vkAcquireNextImage2KHR"));
	void* fpQueuePresentKHR      = reinterpret_cast<void*>(vkGetDeviceProcAddr(g_TempDevice, "vkQueuePresentKHR"));
	void* fpCreateSwapchainKHR   = reinterpret_cast<void*>(vkGetDeviceProcAddr(g_TempDevice, "vkCreateSwapchainKHR"));

	// TODO: Calling vkDestroyDevice() anywhere will causes RDR2 to hang and or crash - Unfixable?
	// Does this cause a memory leak then?
	/*if (g_TempDevice) {
		vkDestroyDevice(g_TempDevice, g_Allocator);
		g_TempDevice = NULL;
	}*/

	if (fpAcquireNextImageKHR)
	{
		MH_STATUS aniStatus  = MH_CreateHook(reinterpret_cast<void**>(fpAcquireNextImageKHR),  &hk_vkAcquireNextImageKHR,  reinterpret_cast<void**>(&oAcquireNextImageKHR));
		MH_STATUS ani2Status = MH_CreateHook(reinterpret_cast<void**>(fpAcquireNextImage2KHR), &hk_vkAcquireNextImage2KHR, reinterpret_cast<void**>(&oAcquireNextImage2KHR));
		MH_STATUS qpStatus   = MH_CreateHook(reinterpret_cast<void**>(fpQueuePresentKHR),      &hk_vkQueuePresentKHR,      reinterpret_cast<void**>(&oQueuePresentKHR));
		MH_STATUS csStatus   = MH_CreateHook(reinterpret_cast<void**>(fpCreateSwapchainKHR),   &hk_vkCreateSwapchainKHR,   reinterpret_cast<void**>(&oCreateSwapchainKHR));
		Log("[+] Vulkan: MH_CreateHook -> AcquireNextImage=%s, AcquireNextImage2=%s, QueuePresent=%s, CreateSwapchain=%s",
			hooks::MHStatusToString(aniStatus), hooks::MHStatusToString(ani2Status),
			hooks::MHStatusToString(qpStatus), hooks::MHStatusToString(csStatus));

		aniStatus  = MH_EnableHook(fpAcquireNextImageKHR);
		ani2Status = MH_EnableHook(fpAcquireNextImage2KHR);
		qpStatus   = MH_EnableHook(fpQueuePresentKHR);
		csStatus   = MH_EnableHook(fpCreateSwapchainKHR);
		Log("[+] Vulkan: MH_EnableHook -> AcquireNextImage=%s, AcquireNextImage2=%s, QueuePresent=%s, CreateSwapchain=%s",
			hooks::MHStatusToString(aniStatus), hooks::MHStatusToString(ani2Status),
			hooks::MHStatusToString(qpStatus), hooks::MHStatusToString(csStatus));
	}
	else
	{
		Log("[!] Vulkan: vkGetDeviceProcAddr() returned no function pointers.");
	}
}

void CImGuiHookManager::sVK::Unhook()
{
	m_shutdownRequested = true;

	g_MainSwapchain = VK_NULL_HANDLE;
	g_SwapchainCount = 0;

	if (g_ImGuiContext)
	{
		ImGui::SetCurrentContext(g_ImGuiContext);

		if (ImGui::GetIO().BackendRendererUserData)
			ImGui_ImplVulkan_Shutdown();

		if (ImGui::GetIO().BackendPlatformUserData)
			ImGui_ImplWin32_Shutdown();

		ImGui::DestroyContext();
		g_ImGuiContext = nullptr;
	}

	MH_DisableHook(MH_ALL_HOOKS);
	CleanupDeviceVulkan();
}
