#include "Manager.h"

IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static WNDPROC s_WndProc;

static LRESULT WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	// Forward input to ImGui when needed
	// Add your own state checks here (e.g., if (MyMod::IsUIOpen()))
	if (ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam))
		return true;

	return CallWindowProc(s_WndProc, hwnd, uMsg, wParam, lParam);
}

void CImGuiHookManager::sWIN32::Hook()
{
	s_WndProc = (WNDPROC)SetWindowLongPtr(m_hWnd, GWLP_WNDPROC, (__int3264)(LONG_PTR)WndProc);
}

void CImGuiHookManager::sWIN32::Unhook()
{
	SetWindowLongPtr(m_hWnd, GWLP_WNDPROC, (LONG_PTR)s_WndProc);
}
