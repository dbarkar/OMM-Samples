# OMM Sample

*Opacity Micro-Map sample* demostrates usage of Opacity Micro-Maps in Raytracing. Sample details:
- Opacity Micro-Map SDK is used for OMM baking. Both CPU baker and GPU baker are supported in the sample: https://github.com/NVIDIA-RTX/OMM
- Rendering and Denoising are based on NRDSample: https://github.com/NVIDIAGameWorks/NRDSample
- The sample primarily uses the DXR 1.2 API, with NvAPI available as a legacy fallback option.

**Note:** The DXR 1.2 API requires a preview driver, available at: https://developer.nvidia.com/downloads/shadermodel6-9-preview-driver

## Build instructions

- Install [*Cmake*](https://cmake.org/download/) 4.1+
- Install on
    - Windows: latest *WindowsSDK* (22000+), *VulkanSDK* (1.3.231+)
- Build (variant 1) - using *Git* and *CMake* explicitly
    - Clone project and init submodules
    - Generate and build project using *CMake*
- Build (variant 2) - by running scripts:
    - Run `1-Deploy`
    - Run `2-Build`

### CMake options

- `USE_MINIMAL_DATA=ON` - download minimal resource package (90MB); run with `--scene=ShaderBalls/ShaderBalls.gltf`
- `RTXCR_INTEGRATION=OFF` - disable the optional hair and skin rendering integration and Claire assets
- `SHADERMAKE_FIND_DXC=OFF` with `SHADERMAKE_DXC_PATH=C:/path/to/dxc.exe` - use a custom DXIL compiler
- `SHADERMAKE_FIND_DXC_VK=OFF` with `SHADERMAKE_DXC_VK_PATH=C:/path/to/dxc.exe` - use a custom SPIR-V compiler
- `DXR_OMM=OFF` - use legacy NvAPI implementation

## How to run

- Run `3-Run OMM sample.bat` from the repository root and select the runtime options.
- Executables are in `_Bin/Release` or `_Bin/Debug`. When launching them directly, set the working directory to the repository root so they can load assets from `_Data`.

## Command Line Arguments
- If [Smart Command Line Arguments extension for Visual Studio](https://marketplace.visualstudio.com/items?itemName=MBulli.SmartCommandlineArguments) is installed, all command line arguments will be loaded into corresponding window
- `--api=D3D12`, `--api=VULKAN` for graphics API selection 
- `--width=x`, `--height=y` for setting window size
- `--scene=*path*` for scene selection
- `--help` to print all the available commands

## Minimum Requirements

Any RTX GPU:
- RTX 5000 series
- RTX 4000 series
- RTX 3000 series
- RTX 2000 series

## Usage

OMM:
- Set baker settings in the UI and press Bake OMMs
- For CPU baker it is recommended to use cache

Navigation:
- Right mouse button + W/S/A/D - move camera
- Mouse scroll - accelerate / decelerate
- F1 - toggle "gDebug" (can be useful for debugging and experiments)
- Tab - UI toggle
- Space - animation toggle

Notes:
- Check "Show all settings" in the UI to see rendering setting.

## OMM Workflow

1. Bake OMM data with Opacity Micro-Map SDK
2. Use OMM data to build OMM with corresponding API functions
3. Build BLAS using OMM
