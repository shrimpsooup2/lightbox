# Decision Records

Why LightBox is built the way it is. Newest decisions at the bottom; never rewrite history: supersede an
entry with a new one that references it (`Supersedes D-00X`).

**Format:** ID · date · status (Accepted / Superseded / Proposed) · context · decision · consequences.

---

### D-001 · 2026-10-14 · Accepted · Native stack: C++20 + Qt 6 + Vulkan 1.3, Cycles for finals

- **Context:** The owner wants an app that can compete with commercial renderers on quality. Options
  weighed: Electron + Three.js/WebGPU (fastest UI, no hardware RT in WebGPU), Rust + wgpu (high ceiling, API
  churn), Godot 4 host (fast start, lower lighting ceiling), C++ + Qt + Vulkan + Cycles.
- **Decision:** C++20 + Qt 6 Widgets + Vulkan 1.3 for the interactive *Live* engine, with Cycles
  (Apache-2.0) embedded as the *Final* path-traced engine. Chosen by the project owner.
- **Consequences:** Highest quality ceiling and direct access to hardware ray tracing; largest engineering
  effort. Sessions must stay small and well documented (`AGENTS.md`, `ROADMAP.md`). Do not propose a stack
  switch.

### D-002 · 2026-10-14 · Accepted · MIT license and dependency license policy

- **Context:** The repository is MIT. Blender is GPL, so building on Blender's codebase is excluded.
- **Decision:** Only permissive (MIT/BSD/Apache/zlib/ISC/Boost/MPL-2.0/public domain) code in-process; LGPL
  only dynamically linked (Qt); no GPL/AGPL in-process; proprietary SDKs only as optional plugins. Details in
  `AGENTS.md` §9.
- **Consequences:** DWG support goes through the user-installed ODA File Converter as an external process;
  DXF gets an in-house reader (libdxfrw/dxflib are GPL). Qt Quick 3D (GPL) is off limits.

### D-003 · 2026-10-14 · Accepted · Qt Widgets shell; viewport is a native Vulkan `QWindow`

- **Context:** The UI must feel premium and dense like professional tools; the renderer must own its
  Vulkan device (ray tracing extensions, headless batch rendering later).
- **Decision:** Qt Widgets for the application chrome. Each viewport is a `QWindow` with
  `QSurface::VulkanSurface` embedded via `QWidget::createWindowContainer`. `lb_render` creates and owns the
  `VkInstance`/`VkDevice`; Qt *adopts* the instance (`QVulkanInstance::setVkInstance`) solely to create
  surfaces portably. QML (`QQuickWidget`) may be used for individual rich panels if it clearly helps.
- **Consequences:** Widgets cannot be drawn over the native viewport, so viewport overlays (gizmos, view
  cube, HUD) are rendered by the engine (Stage 2.4). Rendering stays Qt-free and testable.

### D-004 · 2026-10-14 · Accepted · Vulkan 1.3 baseline; ray tracing is an optional tier

- **Decision:** Require Vulkan 1.3 with `dynamicRendering`, `synchronization2` and `bufferDeviceAddress`.
  No render pass or framebuffer objects; push constants carry a buffer device address to per-frame data.
  Hardware ray tracing (`VK_KHR_acceleration_structure` + `VK_KHR_ray_query`) is detected and reported
  but never required.
- **Consequences:** Supports GPUs from roughly 2016 onward on current drivers, including Intel Iris Xe
  (the dev machine, no hardware RT). Every RT feature needs a raster fallback.

### D-005 · 2026-10-14 · Accepted · World conventions

- **Decision:** Meters; right-handed, Z-up; +Y north by default; reverse-Z depth with an infinite far plane;
  Vulkan clip space with the Y flip in the projection matrix; linear Rec.709 scene color with sRGB output;
  two-sided geometry by default. Full table in `AGENTS.md` §6.
- **Consequences:** Importers convert into these conventions (glTF is Y-up, many CAD files are mm/inches).

### D-006 · 2026-10-14 · Accepted · Dependencies via pinned FetchContent; Qt from the system

- **Decision:** CMake `FetchContent` with exact tags (currently Vulkan-Headers, volk and glslang at
  `vulkan-sdk-1.4.363.0`, VMA `v3.4.0`, GLM `1.0.3`). volk loads Vulkan at runtime, so no Vulkan SDK is
  needed to build. glslang comes from the Vulkan SDK when installed, otherwise it is built from source.
  Qt is located through `CMAKE_PREFIX_PATH` (`LIGHTBOX_QT_DIR`).
- **Consequences:** A clean clone builds with only a compiler, CMake, Ninja, Git and Qt. vcpkg/Conan may be
  reconsidered when heavy dependencies arrive (Cycles, OpenImageIO, IfcOpenShell); record it as a new decision.

### D-007 · 2026-10-14 · Accepted · Toolchains: MSVC canonical on Windows, MinGW supported for core

- **Context:** The dev laptop has no admin rights, so Visual Studio cannot be installed there. Cycles,
  CUDA/OptiX and Blender's prebuilt libraries require MSVC on Windows.
- **Decision:** MSVC 2022 is the canonical Windows toolchain and the CI target. MinGW-w64 GCC 13.1 (from
  Qt's tool repository) is supported for everything except Cycles. Code stays standard C++20, warning-clean
  on both.
- **Consequences:** `LIGHTBOX_WITH_CYCLES` (Stage 3.10) will error out on MinGW with a clear message.

### D-008 · 2026-10-14 · Accepted · Qt version

- **Decision:** Minimum Qt 6.8; developed and tested with Qt 6.10.3 (aqtinstall could not fetch 6.11+ on
  the setup date).
- **Consequences:** Re-test and bump when moving to a newer Qt; note it here.

### D-009 · 2026-10-14 · Accepted · Stage plan: added Stage 0 and Stage 6

- **Context:** The owner's stages were 1 Import, 2 Interface, 3 Light & render, 4 Water, 5 Environment, and
  said they could change.
- **Decision:** Added **Stage 0 Foundation** (build system, shell, viewport, scene document, save/load, undo,
  CI) because importing needs an application to import into, and **Stage 6 Release & ecosystem**
  (installers, live-link plugins, docs). Stage 2 keeps the full UI/workflow design and the final project
  file format; Stage 0 only builds the minimal framework skeleton.

### D-010 · 2026-10-14 · Accepted · Shaders embedded as SPIR-V at build time

- **Decision:** GLSL sources in `src/render/shaders/` are compiled by glslang into generated headers
  (`shaders/<name>.h`, symbol `lb_spv_<name>_<stage>`) and linked into the binary. Shared declarations live
  in `*.glsl` include files; any change to them rebuilds all shaders.
- **Consequences:** No shader files to ship or locate at runtime. Hot reload for shader development can be
  added later behind a developer flag.

### D-011 · 2026-10-14 · Accepted (camera angle superseded by D-013) · Brand

- **Decision:** Logo = isometric cube lit from the left, shadow cast in front of the dark face, wordmark
  standing in the plane of the lit face ("Light" light weight, "Box" bold; Jost, OFL, converted to
  outlines). Primary assets in `branding/`; a lower-camera (22° elevation) alternative in `branding/alt/`.
  UI accent = logo top-face azure `#1FA7F0`.
- **Consequences:** The baseline theme (`src/app/theme.*`) and the Stage 2.1a design system derive their
  accent from the logo palette.

### D-012 · 2026-10-14 · Accepted · Contextual how-to help that can be turned off

- **Context:** The owner wants the UI to teach users how to do things, like Autodesk Fusion's rich
  tooltips, through out-of-the-way info buttons, and wants it easy to disable.
- **Decision:** A data-driven help registry (help IDs → summary, steps, shortcut, media) surfaced as
  rich tooltips that expand into how-to steps, plus small ⓘ info buttons on complex settings and panel
  headers. One preference, *How-to tips: Detailed / Basic / Off*, also toggled from the Help menu,
  applies live. Built in substage 2.1b, before the tools, so every later feature ships its help entry
  (`AGENTS.md` §10). Design in `ARCHITECTURE.md` §12.
- **Consequences:** Every new action/tool/setting needs a help ID and entry; a coverage check enforces it.
  Help text is translatable data, not code.

### D-013 · 2026-10-14 · Accepted · Low-camera logo is primary (supersedes the camera angle in D-011)

- **Context:** The owner preferred the lower-camera variant, closer to the original drawing.
- **Decision:** The 22°-elevation logo, icon and `.ico` are the primary brand assets in `branding/`; the
  isometric (35.26°) set moved to `branding/alt/` with `-iso` names. Everything else in D-011 stands.
- **Consequences:** App window icon, exe icon, About dialog and README use the low-camera assets.

### D-014 · 2026-10-14 · Accepted · Scene document storage and IDs

- **Context:** Substage 0.5 needed a document model that importers, the renderer, undo and the project file
  can all build on.
- **Decision:** `lb::Scene` stores entities and resources in hash maps keyed by random non-zero 64-bit IDs,
  with an explicit ordered hierarchy and `std::optional` components on `Entity`, not an ECS. Mutations emit
  synchronous `SceneChange` notifications; no-op edits are silent. The renderer mirrors the document through
  `render::SceneSync`, which rebuilds the draw list after changes.
- **Consequences:** Simple to read, serialize and undo. Very large instance counts (vegetation, scatter)
  must live inside dedicated components rather than as one entity per instance. Full draw-list rebuilds
  are fine at Stage 0 sizes; incremental GPU scene updates arrive with the frame graph (3.1).

### D-015 · 2026-10-14 · Accepted · Unit tests with doctest

- **Decision:** doctest (MIT, header-only, pinned `v2.5.3`) for unit tests in `tests/`, built by default
  (`LIGHTBOX_BUILD_TESTS`), run with `ctest --preset <preset>`. Tests must not need a GPU or Qt; GPU-facing
  logic is tested through injected fakes (e.g. `SceneSync`'s upload/release callbacks).
- **Consequences:** The harness from roadmap 0.8 exists early; 0.8 keeps the CI work.

### D-016 · 2026-10-14 · Accepted · Design system implementation, Inter and Lucide

- **Context:** The owner asked for UI work right after 0.5, ahead of 0.6–0.8.
- **Decision:** `src/app/theme.*` holds two palettes (dark default, light) as named tokens, generates the
  application stylesheet from a template, sets the Fusion style + matching `QPalette`, and bundles
  **Inter 4.1** (OFL-1.1, `resources/fonts/`) and selected **Lucide 1.57.0** icons (ISC,
  `resources/icons/`) with their license files. Icons are rendered by a custom `QIconEngine` that
  recolors the SVG from the current palette at paint time, so a live theme switch needs no widget
  refresh. Widgets use QSS roles (`QLabel[role=…]`, `QPushButton[accent=true]`, …) instead of colors.
  Qt Svg (LGPL, dynamic) is now a dependency. Add icons by copying the SVG from the pinned Lucide tag.
- **Consequences:** Stage 2.1a is largely done; 2.3 has started with plain `QDockWidget` docking
  (Qt Advanced Docking System can still replace it). The selection set from 0.7 was pulled forward
  into `core/scene/selection.*` because the outliner and inspector need it.

### D-017 · 2026-10-14 · Accepted (panel layout and "All" superseded by D-018) · Workflow: six modes with custom workspaces, start screen

- **Context:** Roadmap 2.2 workflow design, decided with the owner.
- **Decision:** Mode tabs guide the work — **Import · Build · Materials · Site · Light · Render** — plus
  an **All** workspace. Each mode is a remembered panel layout (custom workspaces, persisted per mode),
  not a separate screen: the viewport is always visible, the left dock shows the mode panel tabbed with
  the Scene outliner, the right dock is the Properties inspector. LightBox opens on a **start screen**
  (import, templates, recents, sample) once projects exist (0.6/2.10). The sun direction is document state
  (`Scene::environment()`), so it will be saved with the project. Full design: `docs/WORKFLOW.md`.
- **Consequences:** Every new tool/panel must belong to one mode and update `docs/WORKFLOW.md`. Modes
  whose features are not built yet show an honest "Coming to LightBox" list with roadmap substages,
  which shrinks as stages land. Adding or renaming docks requires bumping `kLayoutVersion`.

### D-018 · 2026-10-15 · Accepted · Simpler panels (supersedes the panel layout and "All" in D-017)

- **Context:** The owner found the left side confusing and hard to navigate: per-mode docks tabbed with
  Scene produced three stacked headers (tab bar, dock title, heading), long "Coming to…" roadmap lists,
  and a layout bug that stacked every mode dock vertically after switching.
- **Decision:** Exactly three panels in every mode: the **mode panel** on the left (one dock whose content
  is a stacked page per mode, single title, one-sentence intro), **Scene** above **Properties** on the
  right. No tabs inside the side areas, no close buttons (View ▸ Panels toggles them), no roadmap lists in
  the UI; modes without tools show a short "coming soon" card. The **All** mode is removed. Per-mode
  layout memory stays (`workspace/layout/<mode>`, `kLayoutVersion` 3); mode switches use `restoreState`
  with a default captured before the window is shown.
- **Consequences:** Simpler code (no per-mode docks) and a stable layout. Future features add pages to the
  mode panel rather than new docks; only panels needed in every mode become docks.

### D-019 · 2026-10-15 · Accepted · Dense, configurable mode panels (refines D-018)

- **Context:** The owner found the D-018 panels "a bit childish" and asked for Lumion-level power and
  configurability, while D-018's calm structure (one mode panel, Scene + Properties) stays.
- **Decision:** Mode panels keep D-018's structure but are built from shared property widgets
  (`src/app/property_widgets.*`): presets row at the top, then **collapsible sections** (`ui::Section`)
  with a shared label column, **slider + number field** (`ui::SliderField`), **segmented choices**
  (`ui::SegmentedControl`). Setters never emit "edited" signals, so panels refresh from the document
  without feedback loops. Long explanations move into tooltips; intro paragraphs are dropped.
- **Consequences:** New settings are one row of code each. The generated inspector (2.6) should produce
  these same widgets. Materials and Build panels move to this style when they are reworked.

### D-020 · 2026-10-15 · Accepted · Interim lighting before the Stage 3 renderer; sky belongs to Environments

- **Context:** The owner wanted real light controls now, before the frame graph/HDR work of Stage 3, and
  later asked for an **Environments** feature (put the house in a white studio, a natural landscape, …)
  that owns the sky, so the Light panel needs no sky color controls.
- **Decision:** Forward shading with GGX/Lambert + sky fill, a single 2048² sun shadow map fitted to the
  scene bounds with rotated-Poisson PCF, per-pass tone mapping and look (`applyLook`), sun position from
  the NOAA equations (`core/solar.*`, tested against astral to < 0.15°), black-body sun color. Sky colors
  are automatic (derived from sun height). Document state: `Environment` (sun mode/angles/site/date/time,
  intensity, Kelvin, sky light, shadows) and `Look` (exposure, tone mapping, contrast, saturation, warmth,
  vignette, clay). Environments are planned as roadmap 5.7.
- **Consequences:** Large sites lose shadow resolution (one map, no cascades) until 3.5. Tone mapping per
  pass is replaced by an HDR target + post chain in 3.2; `Look` and `Environment` stay as the document
  model and feed those passes.

### D-021 · 2026-10-15 · Accepted · Import and texture stack: cgltf, tinyobjloader, ufbx, stb_image; bindless textures

- **Context:** The owner made importing models and textures, and bringing them into the design
  quickly, the top priority. The roadmap listed fastgltf for glTF.
- **Decision:**
  - New Qt-free module `src/import/` (`lb_import`). Formats: glTF 2.0/GLB with **cgltf** (MIT; single
    C header, no simdjson/C++ exceptions issues on MinGW, chosen over fastgltf), OBJ/MTL with
    **tinyobjloader** (MIT), FBX with **ufbx** (MIT/PD; Autodesk SDK avoided because its license
    forbids redistribution in open source).
  - Images are decoded with **stb_image** (MIT/PD) in `core/image.*`, then capped at 4096 px.
    Textures are scene resources (`TextureResource`). Materials reference them through `TextureSlot`s
    (channel + invert, so packed glTF/ARM maps work unsplit). **Box mapping** at real-world size covers
    CAD geometry without UVs.
  - The importer converts axes to Z-up but keeps file units. The scale lives on the model's root
    entity, so units can be corrected after import without touching vertices.
  - Parsing runs on `QThreadPool` (`app/background.hpp`); only `addToScene` runs on the UI thread.
  - Renderer: textures are uploaded with mip chains and two views (sRGB and UNORM), and live in one
    bindless descriptor array (descriptor indexing is core in Vulkan 1.2/1.3). Material parameters go
    in a per-frame storage buffer.
  - stb has no releases, so a commit is pinned. The other libraries are pinned to tags.
- **Consequences:** No compressed-texture cache yet. Textures stay RGBA8 in RAM and VRAM: a 4K map is
  64 MB plus mips. BC7/KTX2 compression follows in 1.9. Imported data is not saved until the project file
  (0.6) exists.

### D-022 · 2026-10-15 · Accepted · Project files (.lbox v0) and the start screen; sample on demand

- **Context:** The owner asked for a start screen listing all projects, Lumion-style. That needs saving
  and opening projects (roadmap 0.6) first.
- **Decision:**
  - `.lbox` is a zip, read and written by **miniz** (MIT), with JSON from **nlohmann/json** (MIT). It
    lives in a Qt-free module `src/project/` (`lb_project`), so batch/CLI tools can use it later.
  - Package contents:
    - `project.json`: format version, project info, camera, and the document as canonical JSON. That
      means sorted IDs as hex strings and round-trip floats.
    - `meshes/<id>.bin` and `textures/<id>.rgba`: binary blobs.
    - `thumbnail.png`: preview for the start screen.
  - Saves are atomic: write a temp file, then rename. Newer files are refused with a clear message;
    older ones go through a migration step.
  - The UI thread installs a loaded document with `Scene::replaceContents`, which keeps IDs.
  - The app opens on a **start screen**: recent projects as thumbnail cards, plus New, Open and Sample.
    New projects start empty, and the sample house only appears when chosen. This replaces the earlier
    rule where the first import silently replaced the sample.
- **Consequences:**
  - Textures are stored as raw RGBA8, deflated rather than kept in their original encoding, so files get
    large with many 4K textures. Keeping the source bytes, or linking to them, comes with format v1 (2.10).
  - Saving runs on the UI thread for now.
  - Autosave, crash recovery and templates stay in 2.10.
