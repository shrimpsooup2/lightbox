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

### D-023 · 2026-10-20 · Accepted · Online texture library (ambientCG + Poly Haven), packs as zips/folders

- **Context:** The owner's top priority is making textures from texture websites quick to use. The
  owner also named Poliigon.
- **Decision:**
  - **Texture library dialog** (Materials ▸ Texture library…) searches **ambientCG** and **Poly Haven**
    through their public APIs. Both are CC0 and need no account or key.
  - One click downloads a set at 1K/2K/4K into `%LOCALAPPDATA%/LightBox/Library/Textures/<source>/`
    (an ambientCG zip, or a Poly Haven folder) with a small `<id>.json` record (name, source, URL,
    license, real size). It then becomes a material at the source's real-world size (ambientCG cm,
    Poly Haven mm; default 2 m when unknown) and is applied to the selection.
  - Downloads already made are reused offline. Uses **Qt Network** (part of Qt, LGPL, dynamic) with a
    disk cache for thumbnails and API responses.
  - Poly Haven's API terms: free including commercial use, the source must be named in the UI (the
    source switch and a credit line do this), and a unique User-Agent is required
    (`LightBox/<version>`).
  - **Poliigon** has no public download API: downloads need an account and the license is royalty-free
    rather than CC0. So it is supported by dropping its downloads instead.
  - Texture-set import accepts **zip archives and folders** as well as images, read with miniz (no
    unpacking). The classifier understands Poliigon's two naming schemes, including the trailing
    `_METALNESS`/`_SPECULAR` workflow marker, `NRM16`, `REFL`, `GLOSS` and `BaseColorOpacity`.
- **Consequences:**
  - LightBox stays fully usable offline. The library only needs the network for searching and for new
    downloads (AGENTS.md principle 9).
  - Asset library 1.10 can index the downloaded folder and its JSON records.
  - API changes on either site may need updates; both are called from `texture_browser.cpp` only.

### D-024 · 2026-10-21 · Accepted · CPU raycast for picking and walking, before the 0.7 GPU ID buffer

- **Context:** The owner asked to pick individual objects in Materials and for a simple walkthrough
  ("select the mode then click anywhere to start walking there"). The roadmap planned GPU picking via an ID
  buffer in 0.7.
- **Decision:**
  - `core/raycast.*`: brute-force ray–triangle test (Möller–Trumbore) against every mesh (object-space test
    after a bounds check), nearest hit with entity, point, normal (facing the ray) and distance. No acceleration structure.
  - The viewport builds the mouse ray from the inverse view-projection (reverse-Z: depth 1 is near).
  - A click without dragging selects the object under the cursor; Materials and Properties follow the
    selection. The **Walk** toolbar toggle turns clicks into "stand here": eye 1.6 m above a floor hit, or
    1.5 m in front of a wall on the floor below, facing it. W A S D / arrows, drag-to-look, Shift, Q/E, Esc.
    Moving casts rays at eye and waist height and removes the part of the move into the hit surface, so
    the walker slides along walls; no gravity or steps.
- **Consequences:**
  - Cost grows with triangle count (per click, and up to two rays × two passes per walk frame). Fine for
    house-sized models; a BVH (or the 0.7 ID buffer for clicks) replaces it when large sites make it slow.
  - The 0.7 selection outline and double-click → Properties remain open.

### D-025 · 2026-10-21 · Accepted · More model formats through Assimp (Collada, KMZ, 3DS, STL, PLY)

- **Context:** The owner asked for more standard house file formats. SketchUp's free version exports
  Collada (`.dae`); 3D Warehouse and Google Earth hand out Collada or KMZ; older libraries are 3DS; CAD
  and scans give STL/PLY. Roadmap 1.4 allowed Assimp or focused readers.
- **Decision:**
  - **Assimp 5.4.3** (BSD-3; its bundled zlib, pugixml, poly2tri and utf8cpp are permissive), static,
    built with only the Collada, 3DS, STL and PLY importers, no exporters, tools or tests
    (`cmake/LightBoxDependencies.cmake`, `src/import/assimp_importer.cpp`). One reader covers all five.
  - **KMZ** is unzipped with miniz (already a dependency) into a temporary folder; the Collada model in
    it is imported and the folder deleted (textures are in memory by then). Entries escaping the
    folder are skipped.
  - Axes: Assimp turns Collada (by `<up_axis>`) and 3DS into Y-up on the root node; LightBox adds its
    usual Y-up → Z-up turn there. STL and PLY carry no axis information: taken as Z-up and flagged
    (`ImportedModel::axesFromFile = false`) for the upright check of D-026. Units: Collada's `<unit>` is
    applied by Assimp; the others are guessed from the size like OBJ (`guessUnits`, now shared).
  - glTF, OBJ and FBX keep their dedicated readers (cgltf, tinyobjloader, ufbx): smaller, faster and
    already tested.
- **Consequences:**
  - A one-off longer first build (Assimp's common code); adding another Assimp importer later is one
    CMake line plus a dispatch entry.
  - **IFC stays open (1.6):** Assimp's IFC reader handles only IFC2x3 and its generated schema is very
    slow to build; IfcOpenShell or web-ifc remain the candidates. `.skp` stays an optional plugin (1.5).
  - Collada's units are baked into the model's root node, so the Import panel reports them as meters.

### D-026 · 2026-10-21 · Accepted · Orbit modes, gizmo overlay lines, import placement and upright guess

- **Context:** Owner backlog: "different rotate types", "easier move/rotation with gizmos", "better
  placement and auto-orientation when a model is imported". The renderer had no overlay pass; there is no
  command system yet (0.7).
- **Decision:**
  - **Orbit modes** (viewport toolbar and View menu, saved in settings): around the model (orbit target),
    around the cursor (pivot = surface or ground point under the cursor at drag start;
    `OrbitCamera::orbitAround` rotates eye and view rigidly so the pivot stays put on screen), and look
    around (turn in place).
  - **Overlay lines:** `FrameInput::overlay` carries `lb::OverlayLine`s (world ends, pixel widths that may
    taper, color). One pipeline (`overlay.vert/.frag`) draws each as a screen-space quad from a per-frame
    storage buffer (the push constants' second address points at it in that pass), alpha-blended, no
    depth test, after the grid. Saved images (`grabFrame`) leave overlays out.
  - **Gizmo** (`core/gizmo.*`, Qt-free and unit-tested): X/Y/Z arrows, a square for sliding on the
    ground, X/Y/Z rings, X/Y/Z scale squares past the rings and a white even-scale square in the middle
    (axis scaling follows the handle; even scaling doubles per gizmo length dragged right, halves left;
    a turned object stretches along its own axis closest to the dragged one); constant ~110 px on screen; at the bottom center of the selection. Picking is in
    screen space; drags keep the start transforms and apply the total change (no drift), to the outermost
    selected entities (a group and its child move once), through parent transforms. Shift snaps
    0.1 m / 15° / 10 %. Shown in Import and Build modes only (Materials clicks shouldn't move walls).
  - **Placement** (`core/placement.*`): every import is set on the ground (lowest point at z = 0),
    footprint centered on the drop point in the viewport; without one, beside the existing content
    (1 m apart along +X), or at the origin in an empty scene.
  - **Upright guess** for files without axis information (OBJ, STL, PLY): compare the flat area facing
    along each axis at the low end with that at the high end. Buildings and most objects have a broad
    flat base and a sloped or smaller top; lying on a side, both ends look alike. Only clear evidence
    turns the model (best score ≥ 2 % of all area and ≥ 2× the current up's score); ambiguous models
    (plain boxes, trees) stay as read.
- **Consequences:**
  - Gizmo edits are not undoable until 0.7 (Properties edits aren't either).
  - Separate imports that share CAD coordinates (site + building) no longer line up automatically: each
    is centered. Moving them with the gizmo or a future "keep file position" option covers that.
  - The upright guess can't help symmetric shapes; the Import panel's Up axis switch and the gizmo
    rings remain the manual fix.

### D-027 · 2026-10-21 · Accepted · Ray tracing is the quality renderer, on every GPU, with an adaptive time budget

- **Context:** The owner: "the renderer should also focus heavily on raytracing … build in glass too.
  Rather than freezing up on lower end machines, the renderer should still produce same quality if
  wanted, but take adaptably longer to not stress the machine. On higher end machines, the quality should
  be able to be improved or the render can be sped up." The development laptop (Iris Xe) has no hardware
  ray tracing; Cycles (3.10) needs MSVC, which isn't available here.
- **Decision:**
  - LightBox gets its **own progressive path tracer** in Vulkan compute (3.8a). It needs nothing beyond
    the Vulkan 1.3 baseline: triangles and a CPU-built BVH (binned SAH, built off the UI thread) in
    storage buffers via buffer device address; materials and textures through the existing per-frame
    material buffer and bindless table. Hardware ray tracing (`VK_KHR_ray_query`, acceleration
    structures) becomes a faster traversal backend where present (3.8b); the software BVH stays the
    reference and the fallback.
  - **Quality and speed are separate.** Quality = target samples per pixel and bounce depth (Draft /
    Good / Final). Speed = how much GPU time each frame may use (Gentle / Balanced / Fast). Every frame
    the renderer measures its GPU time with timestamp queries and sizes the next batch to the budget,
    split into tiles so no submission runs long (UI stays responsive, no driver timeouts, the laptop
    isn't pinned). Slow GPUs reach the same image later; fast GPUs finish sooner or are given higher
    targets. When the target is reached the GPU idles.
  - The **raster engine stays** for interaction (dragging, walking, editing). Any scene or camera change
    restarts accumulation. Both write the same linear HDR image; one composite pass applies the look
    (3.2a), so ray-traced and raster images match in exposure and tone.
  - **Glass** is part of the material model (transmission, IOR, thin-walled panes): refraction and
    Fresnel in the path tracer, a transparent approximation in the raster view.
  - Cycles (3.10) remains optional for MSVC builds; it is no longer the only route to photoreal images.
- **Consequences:**
  - A one-level BVH over world-space triangles is rebuilt after scene changes (fine for houses; a
    two-level BVH for heavy instancing, e.g. forests, comes with 3.8b/5.5).
  - Without denoising (3.8c) previews are noisy until enough samples accumulate.
  - Hardware ray tracing code can't be tested on the development machine; it must keep the software
    path's output and be verified on RT hardware before it is enabled by default.
  - Implementation notes (2026-10-21): the BVH is relaid out depth first with miss links, so traversal
    needs no stack (lower register pressure); the path tracer pipeline compiles on a worker thread and all
    pipelines persist in `pipeline-cache.bin` (the first compile took ~28 s on Iris Xe and blocked
    startup); the bindless texture set is visible to compute. Measured on Iris Xe: ~1 M pixel-samples/s
    with sun, sky and light sampling, i.e. 7½ min for 256 samples at 1259×770.

### D-028 · 2026-10-21 · Accepted · Sky images (HDRI): sun taken out of the image, linked file

- **Context:** The owner's honest verdict on a ray-traced test render: "does not even look close to
  photoreal". The largest single gap to the reference image was the sky: a flat procedural gradient
  against a photographed cloudy dusk sky that also lights the scene. Free CC0 sky panoramas (Poly
  Haven) are the standard source in archviz.
- **Decision:**
  - `Environment::sky.image` (absolute path of an equirectangular Radiance `.hdr`) and
    `sky.imageRotationDegrees`. With an image, both engines use it for the background, reflections and
    sky light; the path tracer samples it by brightness × solid angle (row/column CDFs over a 512 × 256
    grid, MIS with the BSDF). The raster preview uses a blurred mip for its ambient light and
    roughness-blurred mips for reflections.
  - **The sun is taken out of the image** (`makeEnvironmentMap`): the brightest spot is a sun when it
    is ≥ 50 × the median sky; its pixels above 20 × the median (or 1 % of its peak) are clamped and
    the removed light becomes the analytic sun's direction, color and strength. Both engines then draw
    a sharp sun with real shadows (shadow map, next-event estimation) instead of noise from a few
    very bright pixels. Turning the sky turns its sun; *Sun strength* still scales it; the Sun
    section's direction controls are replaced by a note while an image is used.
  - Image values are used as render units × *Sky light*. Poly Haven images are not calibrated: an
    image at 1 lights like a bright overcast day, so dusk scenes with lamps need *Sky light* around
    0.01–0.05.
  - The project stores the **path** (a linked file, like a texture source), not the pixels: a 4K
    `.hdr` is 16–25 MB. Moving a project to another computer needs the image too.
  - Loading (decode, sun extraction, sampling tables; ~1 s for 4K) runs on a worker thread; the
    procedural sky shows meanwhile. Only `.hdr` for now (stb_image); EXR needs another decoder.
- **Consequences:** physical sky (Hillaire) stays planned for 3.4; Environments (5.7) will bundle sky
  images with ground and backdrop. Embedding or collecting linked sky images on save is open work
  (ROADMAP 3.4). Calibrating image brightness (e.g. to measured lux) is open work.

### D-029 · 2026-10-21 · Accepted · Render screen with an offscreen presenter

- **Context:** The owner asked for a Lumion-style render screen instead of refining final images in
  the editing viewport. A ray-traced render in the viewport paused whenever the window was covered
  (frames only render while it is exposed), and its size was the viewport's.
- **Decision:** `Renderer::createOffscreenPresenter(w, h)`: the same `Presenter` (raster preview,
  path tracer, sky image, composite) with no surface: one sRGB target image, no acquire/present,
  read-back always possible, no MSAA (the path tracer antialiases; a 4K multisampled target would not
  fit an integrated GPU). `RenderScreen` is a third page of the main window next to the start screen
  and the workspace: size (View, HD, Full HD, 4K), quality, speed, Render/Stop, progress with time
  left, Save image (PNG/JPEG), Back/Esc. It renders the current view from
  `ViewportWindow::renderScreenInput`; the editing viewport is hidden meanwhile, so the GPU is the
  render's. A fresh presenter per render; it is freed when the render ends.
- **Consequences:** Views (2.9) will let the screen render saved views instead of only the current
  one; the gallery of results, batch queue, panoramas and passes remain 3.12 work.

