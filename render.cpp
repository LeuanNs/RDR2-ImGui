/*
	RDR2 ImGui Mod Template - Render Implementation
	================================================
	
	Example render implementation. Replace this with your own UI code.
*/

#include "render.h"
#include "script.h"
#include "imgui/imgui.h"

namespace Render
{
	void Draw()
	{
		// Example: Show ImGui Demo Window
		static bool showDemo = false;
		if (g_showDemoWindow.load() && !showDemo)
		{
			showDemo = true;
		}
		else if (!g_showDemoWindow.load() && showDemo)
		{
			showDemo = false;
		}
		
		if (showDemo)
		{
			ImGui::ShowDemoWindow(&showDemo);
			g_showDemoWindow.store(showDemo);
		}

		// Example: Simple test window
		static bool showTestWindow = true;
		if (showTestWindow)
		{
			ImGui::Begin("RDR2 ImGui Mod Template", &showTestWindow);
			ImGui::Text("Hello from RDR2!");
			ImGui::Text("This is a test window.");
			ImGui::Separator();
			ImGui::Text("Use this as a starting point for your mod.");
			ImGui::End();
		}
	}

	bool ShouldDrawMouse()
	{
		// Return true if any of your UI windows are open
		return true; // Always show mouse for demo
	}
}
