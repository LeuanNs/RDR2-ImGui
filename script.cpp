/*
	RDR2 ImGui Mod Template - Script Thread
	========================================
	
	This is the ScriptHook thread. Use this file for:
	- Game natives (PLAYER::, ENTITY::, PED::, etc.)
	- Key input detection (GetAsyncKeyState)
	- Game state polling
	
	RULES:
	- Natives ONLY run here (ScriptHook thread)
	- ImGui ONLY in Render() (render thread)
	- Use std::atomic<> for cross-thread communication
	- Use WAIT() to yield in this thread
*/

#include "script.h"
#include "keyboard.h"
#include "Hook/Manager.h"
#include <atomic>

// Example: Shared state between script and render threads
std::atomic<bool> g_showDemoWindow{false};
std::atomic<bool> g_modActive{false};

// Example: Toggle key (F5)
static constexpr DWORD TOGGLE_KEY = VK_F5;

void WaitAndRender(unsigned ms)
{
	scriptWait(ms);
}

void ScriptMain()
{
	// Main script loop
	while (true)
	{
		// Example: Toggle mod with F5
		if (IsKeyJustUp(TOGGLE_KEY))
		{
			bool current = g_modActive.load();
			g_modActive.store(!current);
			g_showDemoWindow.store(!current);
		}

		// Example: Use natives here
		if (g_modActive)
		{
			// Example: Get player coordinates
			// Ped playerPed = PLAYER::PLAYER_PED_ID();
			// Vector3 pos = ENTITY::GET_ENTITY_COORDS(playerPed, true, true);
			// Use pos.x, pos.y, pos.z...
		}

		// Always yield at the end of each frame
		WAIT(0);
	}
}

// Accessors for render thread
bool IsModActive() { return g_modActive; }
bool ShouldShowDemoWindow() { return g_showDemoWindow; }
void SetShowDemoWindow(bool show) { g_showDemoWindow = show; }
