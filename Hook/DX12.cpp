// Code modified from https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook/blob/master/d3d12hook.cpp
// Big thanks to Sh0ckFR

#include "Manager.h"
#include "../render.h"
#include <d3d12.h>
#pragma comment(lib, "libMinHook.x64.lib")

static ID3D12Device* g_d3d12Device = nullptr;
static ID3D12DescriptorHeap* g_d3d12DescriptorHeapBackBuffers = nullptr;
static ID3D12DescriptorHeap* g_d3d12DescriptorHeapImGuiRender = nullptr;
static ID3D12GraphicsCommandList* g_d3d12CommandList = nullptr;
static ID3D12CommandQueue* g_d3d12CommandQueue = nullptr;

// string form of the IID for the ID3D12Device interface
struct __declspec(uuid("189819F1-1DB6-4B57-BE54-1821339B85F7")) ID3D12Device;

struct FrameContext
{
	ID3D12CommandAllocator* pCommandAllocator = nullptr;
	ID3D12Resource* pRenderTargetResource = nullptr;
	D3D12_CPU_DESCRIPTOR_HANDLE sRenderTargetDescriptor{};
};

static UINT uBuffersCounts = 0;
static FrameContext* pFrameContext;

static IDXGISwapChain3* m_pSwapChain;		
static UINT m_SyncInterval;
static UINT m_Flags;

static ID3D12Fence* g_d3d12ResizeFence = nullptr;
static HANDLE g_d3d12ResizeFenceEvent = nullptr;

static volatile bool g_ResizePending = false;
static UINT g_PendingBufferCount = 0;
static UINT g_PendingWidth = 0;
static UINT g_PendingHeight = 0;
static DXGI_FORMAT g_PendingFormat = DXGI_FORMAT_UNKNOWN;
static UINT g_PendingFlags = 0;

static void FlushCommandQueue()
{
	if (!g_d3d12CommandQueue || !g_d3d12Device)
		return;

	if (!g_d3d12ResizeFence)
	{
		g_d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_d3d12ResizeFence));
		g_d3d12ResizeFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	}

	g_d3d12CommandQueue->Signal(g_d3d12ResizeFence, 1);
	if (g_d3d12ResizeFence->GetCompletedValue() < 1)
		g_d3d12ResizeFence->SetEventOnCompletion(1, g_d3d12ResizeFenceEvent);
	WaitForSingleObject(g_d3d12ResizeFenceEvent, INFINITE);
}

typedef long(__fastcall* Present_t) (IDXGISwapChain*, UINT, UINT);
static Present_t og_Present{};
long __fastcall hk_Present(IDXGISwapChain3* pSwapChain, UINT SyncInterval, UINT Flags)
{
	static bool s_initialized;

	m_pSwapChain = pSwapChain;
	m_SyncInterval = SyncInterval;
	m_Flags = Flags;

	if (!s_initialized)
	{
		if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&g_d3d12Device)))
		{
			Log("[+] DX12: winerror.h SUCCEEDED() - Pass");
			CImGuiHookManager::SetGameWindow(FindWindowA("sgaWindow", "Red Dead Redemption 2"));
			if (!CImGuiHookManager::GetGameWindow()) {
				CImGuiHookManager::SetGameWindow(FindWindowA(NULL, "Red Dead Redemption 2"));
				Log("[!] DX12: FindWindowA(\"sgaWindow\") failed, title-only lookup hWnd=%p", CImGuiHookManager::GetGameWindow());
			}

			IMGUI_CHECKVERSION();
			ImGui::CreateContext();

			unsigned char* pixels;
			int width, height;
			ImGuiIO& io = ImGui::GetIO();
			(void)io;
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
				io.Fonts->AddFontDefault();
				Log("[!] DX12: mvboli.ttf not found, using default font");
			}
			io.Fonts->AddFontDefault(); // Indice 2: fuente alternativa para toggle F
			io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
			io.IniFilename = NULL;

			CreateEvent(nullptr, false, false, nullptr);

			DXGI_SWAP_CHAIN_DESC sdesc;
			pSwapChain->GetDesc(&sdesc);
			sdesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
			sdesc.OutputWindow = CImGuiHookManager::GetGameWindow();
			sdesc.Windowed = ((GetWindowLongPtr(CImGuiHookManager::GetGameWindow(), GWL_STYLE) & WS_POPUP) != 0) ? false : true;

			uBuffersCounts = sdesc.BufferCount;
			pFrameContext = new FrameContext[uBuffersCounts];

			D3D12_DESCRIPTOR_HEAP_DESC descriptorImGuiRender = {};
			descriptorImGuiRender.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			descriptorImGuiRender.NumDescriptors = uBuffersCounts;
			descriptorImGuiRender.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

			if (g_d3d12Device->CreateDescriptorHeap(&descriptorImGuiRender, IID_PPV_ARGS(&g_d3d12DescriptorHeapImGuiRender)) != S_OK) {
				Log("[!] DX12: CreateDescriptorHeap(descriptorImGuiRender) FAILED");
				return false;
			}

			ID3D12CommandAllocator* allocator;
			if (g_d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)) != S_OK) {
				Log("[!] DX12: CreateCommandAllocator() FAILED");
				return false;
			}

			for (size_t i = 0; i < uBuffersCounts; i++) {
				pFrameContext[i].pCommandAllocator = allocator;
			}

			if (g_d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL, IID_PPV_ARGS(&g_d3d12CommandList)) != S_OK ||
				g_d3d12CommandList->Close() != S_OK)
			{
				Log("[!] DX12: CreateCommandList() FAILED");
				return false;
			}


			D3D12_DESCRIPTOR_HEAP_DESC descriptorBackBuffers;
			descriptorBackBuffers.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			descriptorBackBuffers.NumDescriptors = uBuffersCounts;
			descriptorBackBuffers.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
			descriptorBackBuffers.NodeMask = 1;

			if (g_d3d12Device->CreateDescriptorHeap(&descriptorBackBuffers, IID_PPV_ARGS(&g_d3d12DescriptorHeapBackBuffers)) != S_OK) {
				Log("[!] DX12: CreateDescriptorHeap(descriptorBackBuffers) FAILED");
				return false;
			}

			const auto rtvDescriptorSize = g_d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_d3d12DescriptorHeapBackBuffers->GetCPUDescriptorHandleForHeapStart();

			for (size_t i = 0; i < uBuffersCounts; i++) {
				ID3D12Resource* pBackBuffer = nullptr;

				pFrameContext[i].sRenderTargetDescriptor = rtvHandle;
				pSwapChain->GetBuffer(i, IID_PPV_ARGS(&pBackBuffer));
				g_d3d12Device->CreateRenderTargetView(pBackBuffer, nullptr, rtvHandle);
				pFrameContext[i].pRenderTargetResource = pBackBuffer;
				rtvHandle.ptr += rtvDescriptorSize;
			}

			ImGui_ImplWin32_Init(CImGuiHookManager::GetGameWindow());
			ImGui_ImplDX12_Init
			(
				g_d3d12Device,
				uBuffersCounts,
				DXGI_FORMAT_R8G8B8A8_UNORM,
				g_d3d12DescriptorHeapImGuiRender,
				g_d3d12DescriptorHeapImGuiRender->GetCPUDescriptorHandleForHeapStart(),
				g_d3d12DescriptorHeapImGuiRender->GetGPUDescriptorHandleForHeapStart()
			);
			ImGui_ImplDX12_CreateDeviceObjects();
			CImGuiHookManager::GetWin32().Hook();
		}
		else
		{
			Log("[!] DX12: winerror.h SUCCEEDED() - FAILED");
		}

		s_initialized = true;
	}

	if (!CImGuiHookManager::IsShutdownRequested())
	{
		if (g_d3d12CommandQueue == nullptr) {
			return og_Present(pSwapChain, SyncInterval, Flags);
		}

		// Manejar resize pendiente en el hilo de render
		if (g_ResizePending)
		{
			FlushCommandQueue();

			// Liberar render targets viejos
			if (pFrameContext)
			{
				for (UINT i = 0; i < uBuffersCounts; i++)
				{
					if (pFrameContext[i].pRenderTargetResource)
					{
						pFrameContext[i].pRenderTargetResource->Release();
						pFrameContext[i].pRenderTargetResource = nullptr;
					}
				}
			}

			// Recrear los render targets con el nuevo tamano
			const auto rtvDescriptorSize = g_d3d12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_d3d12DescriptorHeapBackBuffers->GetCPUDescriptorHandleForHeapStart();

			for (UINT i = 0; i < g_PendingBufferCount; i++)
			{
				ID3D12Resource* pBackBuffer = nullptr;
				pFrameContext[i].sRenderTargetDescriptor = rtvHandle;
				pSwapChain->GetBuffer(i, IID_PPV_ARGS(&pBackBuffer));
				if (pBackBuffer)
				{
					g_d3d12Device->CreateRenderTargetView(pBackBuffer, nullptr, rtvHandle);
					pFrameContext[i].pRenderTargetResource = pBackBuffer;
				}
				rtvHandle.ptr += rtvDescriptorSize;
			}

			uBuffersCounts = g_PendingBufferCount;
			g_ResizePending = false;

			Log("[+] DX12: deferred resize completed (%ux%u), render targets recreated", g_PendingWidth, g_PendingHeight);
		}

		HWND gameHwnd = CImGuiHookManager::GetGameWindow();
		bool hasFocus = (gameHwnd && GetForegroundWindow() == gameHwnd);

		if (!hasFocus) {
			return og_Present(pSwapChain, SyncInterval, Flags);
		}

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

		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		Render::Draw();

		FrameContext& currentFrameContext = pFrameContext[pSwapChain->GetCurrentBackBufferIndex()];
		currentFrameContext.pCommandAllocator->Reset();

		D3D12_RESOURCE_BARRIER barrier;
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource = currentFrameContext.pRenderTargetResource;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

		g_d3d12CommandList->Reset(currentFrameContext.pCommandAllocator, nullptr);
		g_d3d12CommandList->ResourceBarrier(1, &barrier);
		g_d3d12CommandList->OMSetRenderTargets(1, &currentFrameContext.sRenderTargetDescriptor, FALSE, nullptr);
		g_d3d12CommandList->SetDescriptorHeaps(1, &g_d3d12DescriptorHeapImGuiRender);

		ImGui::Render();
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_d3d12CommandList);

		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

		g_d3d12CommandList->ResourceBarrier(1, &barrier);
		g_d3d12CommandList->Close();

		g_d3d12CommandQueue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList* const*>(&g_d3d12CommandList));
	}

	return og_Present(pSwapChain, SyncInterval, Flags);
}

typedef void(*ExecuteCommandLists_t) (ID3D12CommandQueue*, UINT, ID3D12CommandList*);
static ExecuteCommandLists_t og_ExecuteCommandLists{};
void hk_ExecuteCommandLists(ID3D12CommandQueue* pQueue, UINT NumCommandLists, ID3D12CommandList* ppCommandLists)
{
	if (!g_d3d12CommandQueue) {
		g_d3d12CommandQueue = pQueue;
	}

	og_ExecuteCommandLists(pQueue, NumCommandLists, ppCommandLists);
}

// TODO #8: Hook de ResizeBuffers para manejar cambios de resolucion y
// transiciones a pantalla completa exclusiva.
typedef long(__fastcall* ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static ResizeBuffers_t og_ResizeBuffers{};
long __fastcall hk_ResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
	// Solo marcar el flag y guardar parámetros; la limpieza real se hace en el hilo de render
	g_ResizePending = true;
	g_PendingBufferCount = BufferCount;
	g_PendingWidth = Width;
	g_PendingHeight = Height;
	g_PendingFormat = NewFormat;
	g_PendingFlags = SwapChainFlags;

	Log("[+] DX12: ResizeBuffers deferred (pending %ux%u)", Width, Height);

	// Llamar al ResizeBuffers original inmediatamente (el juego espera esto)
	return og_ResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

void CImGuiHookManager::sDX12::Present()
{
	hk_Present(m_pSwapChain, m_SyncInterval, m_Flags);
}

void CImGuiHookManager::sDX12::Hook()
{
	if (GetModuleHandleA("d3d12.dll") == NULL) {
		Log("[!] DX12: d3d12.dll is not loaded.");
		return;
	}

	kiero::Status::Enum initStatus = kiero::init(kiero::RenderType::D3D12);
	kiero::Status::Enum bindStatus;
	if (initStatus == kiero::Status::Success)
	{
		Log("[+] DX12: kiero::init(D3D12) - Success");

		bindStatus = kiero::bind(kiero::D3D12MT::ExecuteCommandLists, (void**)&og_ExecuteCommandLists, hk_ExecuteCommandLists);
		Log("[+] DX12: bind (ExecuteCommandLists) - %s", hooks::KieroStatusEnumToString(bindStatus));
		bindStatus = kiero::bind(kiero::D3D12MT::Present, (void**)&og_Present, &hk_Present);
		Log("[+] DX12: bind (Present) - %s", hooks::KieroStatusEnumToString(bindStatus));
		// TODO #8: Hook de ResizeBuffers para fullscreen
		bindStatus = kiero::bind(kiero::D3D12MT::ResizeBuffers, (void**)&og_ResizeBuffers, &hk_ResizeBuffers);
		Log("[+] DX12: bind (ResizeBuffers) - %s", hooks::KieroStatusEnumToString(bindStatus));

		Log("[+] DX12: kiero::bind() functions completed");
	}
	else
	{
		Log("[!] DX12: kiero::init(D3D12) - FAILED - %s", hooks::KieroStatusEnumToString(initStatus));
	}
}

void CImGuiHookManager::sDX12::Unhook()
{
	m_shutdownRequested = true;

	if (g_d3d12ResizeFenceEvent) { CloseHandle(g_d3d12ResizeFenceEvent); g_d3d12ResizeFenceEvent = nullptr; }
	if (g_d3d12ResizeFence) { g_d3d12ResizeFence->Release(); g_d3d12ResizeFence = nullptr; }
	if (g_d3d12Device) g_d3d12Device->Release();
	if (g_d3d12DescriptorHeapBackBuffers) g_d3d12DescriptorHeapBackBuffers->Release();
	if (g_d3d12DescriptorHeapImGuiRender) g_d3d12DescriptorHeapImGuiRender->Release();
	if (g_d3d12CommandList) g_d3d12CommandList->Release();
	if (g_d3d12CommandQueue) g_d3d12CommandQueue->Release();

	kiero::shutdown();
	if (ImGui::GetCurrentContext())
	{
		if (ImGui::GetIO().BackendRendererUserData)
			ImGui_ImplDX12_Shutdown();

		if (ImGui::GetIO().BackendPlatformUserData)
			ImGui_ImplWin32_Shutdown();

		ImGui::DestroyContext();
	}

	MH_DisableHook(MH_ALL_HOOKS);
}
