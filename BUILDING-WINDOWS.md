# Building on Windows (MinGW-w64 / MSYS2)

Phase 0 baseline: produce a runnable `client.exe` that reaches the server list.

MSVC is deferred; use MSYS2 MinGW64 only.

## Prerequisites

1. Install [MSYS2](https://www.msys2.org/) to the default path: `C:\msys64`
2. Open **MSYS2 MINGW64** (not UCRT64, not MSYS)

## Packages

In the MINGW64 shell:

```bash
pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-glfw mingw-w64-x86_64-glew mingw-w64-x86_64-openal \
  mingw-w64-x86_64-libdeflate mingw-w64-x86_64-enet mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-vulkan-loader mingw-w64-x86_64-vulkan-headers \
  mingw-w64-x86_64-vulkan-validation-layers mingw-w64-x86_64-shaderc
```

Vulkan notes:

- CMake uses `find_package(Vulkan REQUIRED)` and links `Vulkan::Vulkan` (the MinGW
  import library for `vulkan-1.dll`). This is the robust path under MSYS2/MinGW;
  do not hand-link `libvulkan-1.dll.a` unless FindVulkan fails.
- A GPU with a working ICD is required to run `client.exe --vulkan` (the Windows
  Vulkan Runtime / vendor driver). Headers + loader packages alone are not enough.
- **shaderc** provides `glslc`, required at configure/build time to compile
  `shaders/ui.vert` / `ui.frag` / `ui_nametag.frag` and `shaders/world.vert` /
  `world.frag`. CMake embeds the resulting SPIR-V as C arrays
  (`shaders_embedded.c`) via `cmake/EmbedSpirv.cmake` — no runtime shader
  files beside the exe.
- Validation layers: enabled automatically in Debug builds. Override with
  `BUTTERSPADES_VK_VALIDATION=0` (force off) or `=1` (force on, including Release).
- FetchContent pins (Phase 2): **vk-bootstrap `v1.4.356`**, **VMA `v3.3.0`**.

## Configure and build

From the repo root in MINGW64:

```bash
mkdir -p build && cd build
cmake -G Ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ..
ninja
```

Notes:

- CMake downloads FetchContent deps (`cglm`, `vxl`, etc.) and `bsresources.zip`; network access is required.
- `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` is required with CMake 4.x so the vendored `vxl` CMakeLists (minimum < 3.5) can configure. No source tree edits are needed for this.
- Output binary: `build/BetterSpades/client.exe`
- Post-build steps also unpack resources into `build/BetterSpades/`

Verified with:

- MSYS2 at `C:\msys64`
- `mingw-w64-x86_64-gcc` 16.1.0
- `mingw-w64-x86_64-cmake` 4.3.3
- Ninja generator

## Runtime DLLs

`client.exe` does not embed the MinGW/runtime shared libraries. Copy these from
`C:\msys64\mingw64\bin\` into `build/BetterSpades\` (beside `client.exe`):

| DLL | Package / notes |
|-----|-----------------|
| `glfw3.dll` | mingw-w64-x86_64-glfw |
| `glew32.dll` | mingw-w64-x86_64-glew |
| `libopenal-1.dll` | mingw-w64-x86_64-openal (not named `OpenAL32.dll` in current MSYS2) |
| `libdeflate.dll` | mingw-w64-x86_64-libdeflate |
| `libenet-7.dll` | mingw-w64-x86_64-enet |
| `libgcc_s_seh-1.dll` | mingw-w64-x86_64-gcc / gcc-libs |
| `libstdc++-6.dll` | mingw-w64-x86_64-gcc / gcc-libs (required; Vulkan TU is C++) |
| `libwinpthread-1.dll` | mingw-w64-x86_64-libwinpthread |
| `vulkan-1.dll` | Prefer leaving this on `PATH` (`C:\msys64\mingw64\bin`) rather than
  copying beside `client.exe`. A local copy can break validation-layer DLL loading
  (Win32 error 126) unless the full layer set is copied too. |

Validation layers (Debug / `BUTTERSPADES_VK_VALIDATION=1`):

```powershell
$env:VK_LAYER_PATH = "C:\msys64\mingw64\bin"
$env:PATH = "C:\msys64\mingw64\bin;" + $env:PATH
cd build\BetterSpades
.\client.exe --vulkan
```

Alternatively, keep `C:\msys64\mingw64\bin` on `PATH` when launching.

PowerShell one-liner (from repo root):

```powershell
$dest = "build\BetterSpades"
$src = "C:\msys64\mingw64\bin"
@(
  "glfw3.dll","glew32.dll","libopenal-1.dll","libdeflate.dll",
  "libenet-7.dll","libgcc_s_seh-1.dll","libstdc++-6.dll","libwinpthread-1.dll",
  "vulkan-1.dll"
) | ForEach-Object { Copy-Item -Force (Join-Path $src $_) (Join-Path $dest $_) }
```

## Run

```powershell
cd build\BetterSpades
.\client.exe              # OpenGL (default)
.\client.exe --vulkan     # Phase 3 Vulkan 2D/UI (menu / settings / server list)
```

Expected (default): window opens and the server list UI appears. Live server connection is a manual check.

Expected (`--vulkan`): same server-list / settings UI as GL (fonts, colors, scissor clipping).
World/3D draws remain stubbed. Resize / minimize / restore should keep presenting;
close with no validation errors.

## Source changes

Phase 2 adds Vulkan FetchContent deps and C++20 for `gfx_vk.cpp` only. Debug builds
disable IPO/LTO (Release still enables it when supported) so validation debug stacks stay useful.
