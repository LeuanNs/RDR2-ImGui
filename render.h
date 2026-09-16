/*
	RDR2 ImGui Mod Template - Render Interface
	============================================
	
	This file defines the interface between the hook system and your mod's render code.
	Implement these functions in your mod's main render file.
*/

#pragma once

namespace Render
{
	// Called every frame from the render thread
	// Put your ImGui drawing code here
	void Draw();

	// Return true if the mouse cursor should be visible
	bool ShouldDrawMouse();
}
