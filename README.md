# RDR2 ImGui Mod Template

A modern, clean skeleton for building Red Dead Redemption 2 mods with [ImGui](https://github.com/ocornut/imgui) rendering capabilities. Built on top of the original [ImGuiRDR2Hook](https://github.com/Halen84/ImGuiRDR2Hook) by Halen84, with improvements and a focus on **Vulkan** support.

---

## Overview

This template provides a complete foundation for creating RDR2 mods that require custom UI rendering. It handles all the complex hooking infrastructure so you can focus on building your mod's features.

**Key Features:**
- Dual API support: **Vulkan** (primary, fully tested) and **DirectX 12** (functional but untested)
- Clean separation between game logic (ScriptHook thread) and UI rendering (Render thread)
- Thread-safe communication using `std::atomic<>`
- Ready-to-use project structure with Visual Studio 2022 solution
- Includes all dependencies: ImGui 1.89.5, kiero, MinHook, ScriptHookRDR2 SDK

---

## Architecture

The mod operates on two main threads:

### Script Thread (`script.cpp`)
- Execute game natives (`PLAYER::`, `ENTITY::`, `PED::`, etc.)
- Handle keyboard input detection
- Poll game state
- Use `WAIT(0)` to yield control

### Render Thread (`render.cpp`)
- Draw ImGui UI elements
- **No natives allowed here**
- Communicate with script thread via `std::atomic<>`

**Critical Rule:** Never mix threads. Natives only in `script.cpp`, ImGui only in `render.cpp`.

---

## Project Structure

```
RDR2ImGuiMod/
├── Hook/                    # Graphics API hooks
│   ├── Manager.h/cpp        # Hook manager (Vulkan/DX12)
│   ├── Vulkan.cpp           # Vulkan hook (kiero + MinHook)
│   ├── DX12.cpp             # DirectX 12 hook
│   └── Win32.cpp            # WndProc subclass for input
├── imgui/                   # ImGui 1.89.5 (do not modify)
├── kiero/                   # Graphics API hooking library
├── minhook/                 # MinHook library
├── inc/                     # ScriptHookRDR2 SDK (natives, types, enums)
├── lib/                     # Precompiled libraries
├── main.cpp                 # DLL entry point
├── script.h/cpp             # ScriptHook thread (game interaction)
├── render.h/cpp             # Render thread (UI drawing)
├── keyboard.h/cpp           # Input handling (from SDK)
└── RDR2ImGuiMod.sln         # Visual Studio 2022 solution
```

---

## Getting Started

### Prerequisites

- **Visual Studio 2022** with C++ workload
- **Vulkan SDK** installed (`VULKAN_SDK` environment variable set)
- **Red Dead Redemption 2** (Steam or Epic Games)
- **ScriptHookRDR2** (included in `lib/`)

### Building

1. Open `RDR2ImGuiMod.sln` in Visual Studio 2022
2. Select **Release | x64** configuration
3. Build the solution (Ctrl+Shift+B)
4. Output: `build/RDR2ImGuiMod.asi`

**Command-line build:**
```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" `
  "RDR2ImGuiMod.sln" /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
```

### Installation

1. Copy `RDR2ImGuiMod.asi` to your RDR2 game directory
2. Copy `RDR2ImGuiMod_config.txt` to your RDR2 game directory
3. Launch the game
4. Press **F5** to toggle the mod UI

---

## Configuration

Edit `RDR2ImGuiMod_config.txt` in your game directory:

```ini
# Graphics API: Vulkan or DX12
# If not specified, auto-detects from system.xml
HookType=Vulkan
```

---

## Creating Your Mod

### Adding Game Logic

Edit `script.cpp` to interact with the game:

```cpp
void ScriptMain()
{
    while (true)
    {
        // Example: Get player position
        Ped playerPed = PLAYER::PLAYER_PED_ID();
        Vector3 pos = ENTITY::GET_ENTITY_COORDS(playerPed, true, true);
        
        // Example: Check key press
        if (IsKeyJustUp(VK_F6))
        {
            // Do something
        }
        
        WAIT(0);
    }
}
```

### Adding UI

Edit `render.cpp` to draw your UI:

```cpp
namespace Render
{
    void Draw()
    {
        ImGui::Begin("My Mod");
        ImGui::Text("Hello from RDR2!");
        ImGui::End();
    }
}
```

### Adding New Systems

Create new `.cpp/.h` files and add them to the Visual Studio project:

1. Right-click project → **Add → New Item**
2. Implement your system
3. Integrate in `script.cpp` (game logic) or `render.cpp` (UI)

---

## Example Mods Built With This Template

- **[Write Your Journey](https://www.nexusmods.com/reddeadredemption2/mods/XXXX)** - A complete journal system with custom books, letters, and discoverable sheets
- Your mod here? Let me know!

---

## API Compatibility

| API | Status | Notes |
|-----|--------|-------|
| **Vulkan** | ✅ Fully tested | Primary development target, stable |
| **DirectX 12** | ⚠️ Functional but untested | Should work, but not regularly tested |

If you encounter issues with DX12, please report them. Vulkan is the recommended API.

---

## Credits

- **Original Base:** [ImGuiRDR2Hook](https://github.com/Halen84/ImGuiRDR2Hook) by Halen84
- **DX12 Hook:** [Sh0ckFR/Universal-Dear-ImGui-Hook](https://github.com/Sh0ckFR/Universal-Dear-ImGui-Hook)
- **Vulkan Hook:** [bruhmoment21/UniversalHookX](https://github.com/bruhmoment21/UniversalHookX)
- **ImGui:** [ocornut/imgui](https://github.com/ocornut/imgui)
- **ScriptHookRDR2:** Alexander Blade

---

## License

This template is provided as-is for educational and modding purposes. Respect Rockstar Games' modding policy and terms of service.

---

## Support

For issues, questions, or contributions, please visit the [GitHub repository](https://github.com/YOUR_USERNAME/RDR2-ImGui).

**Happy modding!**
