# Building LightBox

## Requirements

| Tool | Version | Notes |
| --- | --- | --- |
| CMake | ≥ 3.25 | Presets v6 |
| Ninja | any recent | Generator used by all presets |
| C++20 compiler | MSVC 2022 (canonical on Windows), MinGW-w64 GCC 13.1 (Windows, no-admin), GCC 13+ / Clang 17+ (Linux) | Cycles (Stage 3.10) will require MSVC on Windows |
| Qt | 6.8+ (tested 6.10.3) | Widgets module; must be a build with Vulkan support (official binaries are) |
| GPU driver | Vulkan 1.3 | Any recent NVIDIA/AMD/Intel driver |
| Git | any | CMake fetches pinned dependencies on first configure |
| Vulkan SDK | optional | Provides validation layers (`--validation`) and a prebuilt `glslang`; without it glslang is built from source on first configure (a few minutes) |

Everything else (Vulkan headers, volk, VMA, GLM, glslang) is fetched by CMake at pinned tags; see
`cmake/LightBoxDependencies.cmake`.

## Machine-specific paths: `CMakeUserPresets.json`

The shared presets read two environment variables:

- `LIGHTBOX_QT_DIR`: the Qt kit root, e.g. `C:/Qt/6.10.3/mingw_64` or `/opt/Qt/6.10.3/gcc_64`
- `LIGHTBOX_MINGW_DIR`: MinGW root for the `mingw-*` presets, e.g. `C:/Qt/Tools/mingw1310_64`

Either set them in your environment, or create a git-ignored `CMakeUserPresets.json` next to
`CMakePresets.json`:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "local-mingw-base",
      "hidden": true,
      "inherits": "mingw-base",
      "environment": {
        "LIGHTBOX_QT_DIR": "C:/Users/you/Qt/6.10.3/mingw_64",
        "LIGHTBOX_MINGW_DIR": "C:/Users/you/Qt/Tools/mingw1310_64",
        "LIGHTBOX_PIP_SCRIPTS": "C:/Users/you/AppData/Roaming/Python/Python314/Scripts",
        "PATH": "$env{LIGHTBOX_MINGW_DIR}/bin;$env{LIGHTBOX_PIP_SCRIPTS};$penv{PATH}"
      }
    },
    { "name": "local-mingw-debug", "inherits": "local-mingw-base", "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" } },
    { "name": "local-mingw-release", "inherits": "local-mingw-base", "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo" } }
  ],
  "buildPresets": [
    { "name": "local-mingw-debug", "configurePreset": "local-mingw-debug" },
    { "name": "local-mingw-release", "configurePreset": "local-mingw-release" }
  ]
}
```

`LIGHTBOX_PIP_SCRIPTS` is only needed when CMake/Ninja were installed with `pip --user` and are not on PATH.

## Windows without admin rights (MinGW)

This is how the original development laptop is set up; nothing needs elevation.

```sh
python -m pip install --user cmake ninja aqtinstall
python -m aqt install-qt windows desktop 6.10.3 win64_mingw --outputdir %USERPROFILE%\Qt
python -m aqt install-tool windows desktop tools_mingw1310 --outputdir %USERPROFILE%\Qt
```

Create `CMakeUserPresets.json` as above, then:

```sh
cmake --preset local-mingw-debug
cmake --build --preset local-mingw-debug
build\local-mingw-debug\src\app\LightBox.exe
```

> aqtinstall 3.3.0 failed to fetch Qt 6.11+ metadata on 2026-10-14 (checksum error); 6.10.x works. The
> official Qt online installer works for any version.

## Windows with MSVC (canonical)

1. Install Visual Studio 2022 or Build Tools with the *Desktop development with C++* workload.
2. Install Qt for `msvc2022_64` (online installer or `aqt install-qt windows desktop 6.10.3 win64_msvc2022_64`).
3. Open an **x64 Native Tools Command Prompt for VS 2022**, set `LIGHTBOX_QT_DIR`, then:

```sh
cmake --preset msvc-debug
cmake --build --preset msvc-debug
build\msvc-debug\src\app\LightBox.exe
```

## Linux

```sh
# Debian/Ubuntu example
sudo apt install build-essential cmake ninja-build git qt6-base-dev libvulkan-dev mesa-vulkan-drivers
cmake --preset linux-debug      # set LIGHTBOX_QT_DIR only if Qt is not in a system path
cmake --build --preset linux-debug
./build/linux-debug/src/app/LightBox
```

## Running and verifying

- On Windows the build runs `windeployqt` after linking (option `LIGHTBOX_DEPLOY_QT_ON_BUILD`, default ON),
  so `LightBox.exe` starts straight from the build directory.
- Debug builds open a console window with the log; all builds log to
  `%LOCALAPPDATA%/LightBox/logs/lightbox.log` (Linux: `~/.local/share/LightBox/logs/`).
- `LightBox --screenshot shot.png` captures the main window ~2 s after startup and exits (exit code 0 on
  success). Use it to verify visual changes.
- `LightBox --validation` enables Vulkan validation (needs the Vulkan SDK). Errors appear in the log as
  `[vulkan]` lines.
- `LightBox --quit-after 5000` exits automatically, for smoke tests.

## Unit tests

```sh
ctest --preset local-mingw-debug      # or mingw-debug / msvc-debug / linux-debug
build/local-mingw-debug/tests/lb_tests.exe --help   # doctest options, e.g. -tc="*scene*"
```

Tests live in `tests/` (doctest), need no GPU and no Qt, and are built unless
`-DLIGHTBOX_BUILD_TESTS=OFF`.

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| `unable to find a build program corresponding to "Ninja"` | Put Ninja on PATH (or `LIGHTBOX_PIP_SCRIPTS` in your user preset) |
| `Could not find a package configuration file provided by "Qt6"` | Set `LIGHTBOX_QT_DIR` to the Qt kit root (the folder containing `lib/cmake/Qt6`) |
| App says *LightBox needs Vulkan 1.3* | Update the GPU driver; very old GPUs (pre-2016) are not supported |
| App starts without Qt DLLs on Windows | Re-enable `LIGHTBOX_DEPLOY_QT_ON_BUILD` or run `windeployqt LightBox.exe` |
| `glslang` build step is slow on first configure | Expected once; install the Vulkan SDK to skip it |
