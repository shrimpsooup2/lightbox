# LightBox Progress

## Current position

- **Stage:** 0 (0.7–0.8 open, 0.6 mostly done) and 1 (import) in progress.
- **Done:** 0.1–0.5, 1.1, 1.3, 2.1a, 2.2, 3.8a; partial: 0.6 (no project-file tests yet), 1.2, 1.4 (FBX,
  Collada/KMZ, 3DS, STL, PLY; real-export checks open), 1.9, 1.10 (online textures), 2.3, 2.4 (walk,
  orbit modes, lens), 2.5 (click select, move/rotate/scale gizmo; no undo yet), 2.8, 2.10 (start
  screen), 3.1, 3.2a (HDR + composite, AgX, auto exposure), 3.3 (glass, emission), 3.4 (lights, sky
  images), 3.8d (lights, emission, sky image sampling); interim lighting/look/camera (D-020)
- **Renderer focus (owner):** photoreal quality like `render quality standard.png` (repo root; delete it
  once reached). The owner judged the first ray-traced glTF test "not even close to photoreal"; the
  reference replica (`.lightbox-tools/pavilion.cpp`, see the latest entry) is the benchmark scene now.
  Next for quality: denoiser (3.8c), path tracer speed (wavefront kernels), cut-out opacity for leaves,
  a dedicated render screen (3.12). The owner said not to work on environment/nature for now.
- **Owner backlog, in the owner's words:** "better viewport scaling" (not yet pinned down: zoom
  toward the cursor? model size?), then the UI problems the owner deferred ("can focus on ui problems
  later"): the Materials panel scrolls sideways when a material is selected (its editor is wider than
  the default dock), the start screen logo has a dark box behind it. Also open: project-file tests
  (0.6), undo for gizmo/inspector edits (0.7), bounced light/AO for a "rendered" look (3.6), Rhino/
  SketchUp/IFC (1.5–1.6).
- **Known gaps carried forward:** Vulkan validation run (moved to 0.8 CI), MSVC and Linux presets not yet
  built anywhere (0.8 CI).

## How to add an entry

Newest entry first. Keep it short and factual:

```
### YYYY-MM-DD · <agent/model> · substage(s)
- Did: …
- Verified: <preset> build warning-free; ran …; screenshot …
- Decisions: D-0XX (or none)
- Open issues / next: …
```

## Session log

### 2026-10-21 · Claude (omp) · Similar objects also by material name (D-030)

- **Did:** owner: "it should match things named like similar materials". `withSimilarObjects` also
  brings objects using a selected object's material or a material named alike (same similarity key;
  generic names like "Material14" only within one model; default clay never matches). The Materials
  note names both: "Includes N more objects: named like “fin”; with materials like “black steel”;
  copies."
- **Verified:** warning-free `ninja -j 2` build; 95 test cases pass (new: same material, similarly
  named material, default clay not grouping, generic material names limited to one model). In the
  app (pavilion): clicking a fin gives "Apply to 44 objects" (all Black steel objects) and the note
  above. Desktop copy refreshed.
- **Decisions:** D-030 updated.

### 2026-10-21 · Claude (omp) · Apply to similar objects, selection outline, Shift-select (D-030, D-031)

- **Did:** `core/scene/similar.*` (name keys, same-size geometry copies); Materials ▸ *Include
  similar objects* (on by default) with "Apply to N objects" and a line saying what was matched;
  the texture library applies to the same objects (`materialTargets`, shared setting).
- **Also:** a **selection outline** (selected objects drawn into an R8 mask, outlined in the
  accent color by the composite; every mode but Render, never in saved images) and **Shift/Ctrl+click**
  to add or remove objects (`Selection::toggle`), D-031.
- **Verified:** warning-free `ninja -j 2` build; 94 test cases pass (new: similarity keys for
  Blender/SketchUp/Revit/copy names, generic names keep numbers, multi-material parts, same-name and
  same-size-copy matching, scaled unit-cube instances not matched, Sketchfab "Material14" limited to
  one model; Selection::toggle). In the app (pavilion scene): clicking a fin shows "Apply to 23
  objects" / "Includes 22 more objects named like “fin”"; Shift+click on the deck adds it (Apply to 24),
  and both get the blue outline (checked with a temporary local build whose screenshot kept overlays;
  external screen grabs can't see the Vulkan view on this Intel laptop at the moment, even with
  older builds: the view sits on a hardware overlay plane).
- **Decisions:** D-030, D-031.

### 2026-10-21 · Claude (omp) · Render screen: resolution and DPI (3.12)

- **Did:** the render screen's size buttons became **Resolution**: presets (same as the view, HD,
  Full HD, QHD, 4K UHD, square 2048², portrait 4:5) plus editable width × height (16–8192 px, typing
  switches to Custom) with *Keep proportions*; **Print resolution** in DPI (quick 72/150/300) written
  into saved PNG (pHYs) and JPEG (JFIF density); a line with megapixels and print size in cm and inches.
  All remembered in QSettings (`renderScreen/*`).
- **Verified:** warning-free `ninja -j 2` build. Scripted run: wheel on the width field 1920 → 1925
  moved the height to 1083 and switched the preset to Custom; 72 DPI changed the print size to
  67.9 × 38.2 cm; Draft render done in 1:18; the saved PNG read back as 1925 × 1083 at 72 DPI (PIL).
  Standalone copy in `C:/Users/30HamghalamA/LightBox App` refreshed.
- **Decisions:** D-029 updated.

### 2026-10-21 · Claude (omp) · Renderer: lights, glass, HDR, path tracer (3.8a), sky images (D-027, D-028)

- **Did:**
  - **Scale gizmo** handles (per axis and uniform, Shift snaps 10 %).
  - **HDR pipeline (3.2a):** raster renders into RGBA16F; `composite.frag` applies the look (new **AgX**
    tone mapping, **auto exposure** via `exposure.comp`) into the swapchain; overlays draw afterwards.
  - **Lights:** `Light` (bulb/spot/panel; lumens, Kelvin, size, beam) on entities, saved in projects;
    Light ▸ Lights adds them at the view focus; clickable icons and the gizmo in Import/Build/Light
    modes (not in Render mode). Raster: unshadowed; path tracer: next-event estimation + MIS.
  - **Glass and emission** in the material model (Materials ▸ Glass & glow; presets Clear/Frosted/
    Tinted/Solid); sorted transparent raster pass; glTF see-through materials (alpha blend,
    KHR_materials_transmission/ior/volume) import as glass.
  - **Path tracer (3.8a, D-027):** binned-SAH BVH (`core/bvh.*`) built off the UI thread, stackless
    traversal, `pathtrace.comp` sharing `material.glsl` with the raster shader (parallax relief on
    camera/mirror hits), thin and solid glass, sun/sky/light sampling, Russian roulette, time budget
    from GPU timestamps (Gentle/Balanced/Fast), quality presets (Draft/Good/Final/Ultra), async pipeline
    compile + `pipeline-cache.bin`. Fixed `VK_ERROR_DEVICE_LOST` (texture set not visible to compute).
  - **Progress bar and time left** for ray tracing (Render panel and status bar; "Refining: 39 of 256
    samples · 1:09 · about 6:24 left" finished at 7:35).
  - **Sky images (D-028):** `.hdr` panoramas in Light ▸ Sky (or dropped on the viewport), turnable; the
    sun is extracted from the image and drives both engines' sun; importance-sampled sky light in the
    path tracer; blurred-mip sky light and reflections in the raster view; loaded on a worker thread.
  - **Exposure-relative firefly clamp** in the path tracer (dusk scenes were left full of speckles).
  - **Render screen (3.12, D-029):** File ▸ Render Image… (Ctrl+Shift+R) / Render ▸ Render image…;
    offscreen presenter (`Renderer::createOffscreenPresenter`), sizes View/HD/Full HD/4K, quality,
    speed, live preview, progress and time left, Save image, Back/Esc. Made after a replica render in
    the viewport stalled at 40 samples in 65 min while the window was covered.
  - Test scenes (throwaway, `.lightbox-tools`): the owner's `modern_house.glb` (1.19 M triangles;
    owner: "not even close to photoreal"; the model itself is low-detail) and `pavilion.cpp`, a replica
    of the reference image built from boxes with CC0 ambientCG textures (WoodSiding008, Concrete031,
    Wood066, WoodFloor051, Grass004) and Poly Haven's `industrial_sunset_puresky` (CC0).
- **Verified:** warning-free `local-mingw-debug` build; 86 test cases pass (new: BVH, ray scene
  traversal, gizmo scale, glTF glass import, sky image directions/sun extraction/sampling/broken pixels,
  project round trip of lights, glass, emission, sky image, look). Screenshots: modern house raster vs.
  256-sample ray traced; pavilion replica raster and ray traced (256 samples in 2:19).
- **Decisions:** D-027 (implementation notes added), D-028, D-029.
- **Open issues / next:** path tracer speed (~1 M pixel-samples/s on Iris Xe; wavefront split),
  denoiser (3.8c), cut-out opacity (leaves render as solid cards), raster grid shows through ground
  meshes at z ≈ 0, sky image brightness is uncalibrated (dusk needs *Sky light* ≈ 0.01), linked sky
  images are not collected with the project, render screen: Views and results gallery (3.12).
- **Build note (this laptop):** `cmake --build --preset local-mingw-debug` (pip's cmake wrapper) hung
  twice at ninja's "Re-checking globbed directories" and a full parallel build lagged the whole machine.
  What works: run `ninja -j 2` in `build/local-mingw-debug` with `Qt/Tools/mingw1310_64/bin` on PATH at
  below-normal priority (~2 min for an app-only rebuild). Verified the render screen with it: HD, Draft,
  64 samples in 0:34, Save image enabled. The saved window geometry (2018 px) is wider than the
  1920 px screen, so the render screen's sidebar is clipped on the right until the window is resized.

### 2026-10-21 · Claude (omp) · Owner backlog: click-select, walk, random tiles, orbit modes, gizmos, placement, formats

- **Did:**
  - **Click-select** in the viewport (a click without dragging; CPU raycast `core/raycast.*`, D-024).
    Materials and Properties follow, which covers "pick individual objects in Materials".
  - **Walk mode** (viewport toolbar *Walk*, footprints icon): click to stand there (floor: on the spot;
    wall: 1.5 m in front of it), W A S D / arrows, drag to look, Shift runs, Q/E down/up, wheel steps,
    Esc stops; slides along walls. `OrbitCamera::look`/`walk`.
  - **Rotate each tile randomly** (Materials ▸ Mapping, off by default, saved as `randomTileRotation`):
    every tile corner gets a random quarter turn and shift, blended across tile middles; color,
    roughness, normal (tilt turned back with the tile) and height maps use the same lookup. Replaces the
    far-away bombing for that material.
  - **Orbit modes** (viewport toolbar + View menu, remembered): around the model, around the point
    under the cursor (`OrbitCamera::orbitAround`, pivot stays put on screen), look around.
  - **Gizmo** in Import and Build modes (`core/gizmo.*`, new overlay pass `overlay.vert/.frag`): X/Y/Z
    arrows, ground square, X/Y/Z rings at the bottom center of the selection, constant screen size,
    hover highlight, Shift snaps 0.1 m / 15°; groups move once with their children. Build panel quick
    reference updated. Not in saved images.
  - **Import placement** (`core/placement.*`): lands on the ground at the drop point, else beside the
    existing content, else at the origin; OBJ/STL/PLY (no axis information) are stood upright when
    their flat base says so. `modelBounds`/`moveBy` moved from the Import panel into core
    (`worldBoundsOf`, `moveBy`, `outermostEntities`).
  - **Formats** via Assimp 5.4.3 (Collada, 3DS, STL, PLY importers only): `.dae` (SketchUp's free
    export; units and up axis from the file), `.kmz` (unzipped with miniz), `.3ds`, `.stl`, `.ply`.
- **Verified:** warning-free `local-mingw-debug` build; 73 tests pass (new: raycast; orbit pivot stays
  on screen; gizmo size/picking, axis-only moves through a scaled parent, snapping, ring turn past
  180°; upright guess on a gabled house lying on its back/upside down vs. ambiguous boxes; ground
  placement; Collada Z-up inches, KMZ, ASCII STL). Scripted runs (PrintWindow captures):
  - click on the brick wall → *Main volume* selected, Materials shows Bricks113A;
  - Walk → click on the plinth → standing at eye height facing the wall; held W stops at the wall,
    held A slides along it;
  - all materials with random tiles: no repeating grid, relief consistent, soft tile borders;
  - orbit around the cursor: the grabbed wall point stays under the mouse;
  - Z-up house OBJ placed 250 m away and 12 m up: imported upright (−90° X), on the ground at the
    origin; dragging the Y arrow moved it along Y;
  - Assimp's sample duck.dae (textured) and Spider_binary.stl import upright on the ground.
- **Decisions:** D-024 (CPU raycast before the 0.7 ID buffer), D-025 (Assimp for more formats),
  D-026 (orbit modes, overlay lines, gizmo, placement, upright guess).
- **Next:** ask what "better viewport scaling" means; UI problems (Materials panel width, start screen
  logo); undo (0.7); real SketchUp/Revit exports through Collada/FBX.

### 2026-10-20 · Claude (omp) · Textures from the web (owner top priority), distance quality, projects

- **Did:**
  - Project file and start screen (written by two agents before the rate limit; built and smoke-tested
    the start screen).
  - Texture sets from zip archives and folders (miniz) and Poliigon naming. The classifier was
    reading `…_COL_8K_METALNESS` as a metal map.
  - **Texture library** (Materials ▸ Texture library…): ambientCG and Poly Haven search, categories,
    1K/2K/4K, download into the local library with JSON records, material at real-world size,
    applied to the selection. The shared `applyMaterial`/`meshEntitiesIn` helpers moved into core.
  - Tile-size quick buttons; Materials panel width fixes (stack sized by its widest page; Render lens
    row); Poly Haven WebP thumbnails switched to PNG.
  - Distance quality, after the owner said distant brick looked terrible: sRGB-correct mips for color
    maps, Toksvig specular AA, per-material Variation (macro noise plus distance-faded texture
    bombing).
- **Verified:** warning-free build; 61 tests pass (Poliigon/ambientCG classification, zip and folder
  packs, SceneSync color-upload flag). Scripted runs (PrintWindow captures of LightBox only):
  - ambientCG Bricks 113 A (2K zip) and Poly Haven Plywood/Red Brick (separate maps) downloaded and
    applied to all 7 objects at real size;
  - library reuse ("✓");
  - the far view of Red Brick has no tile grid after the fixes.
- **Decisions:** D-023 (online texture library; Poliigon via drops only, since it has no public API).
- **Next:** the owner backlog above, in order of the owner's choosing.

### 2026-10-15 · Claude (omp) · Import & textures (owner priority): 1.1, 1.3, FBX, 1.9 partial

- **Did:** New Qt-free `src/import/` (lb_import): glTF/GLB (cgltf), OBJ/MTL (tinyobjloader), FBX (ufbx),
  normals/degenerate cleanup, texture sets (ambientCG/Poly Haven names, ARM/ORM), `addToScene` (root
  group with unit scale, far-origin recentering). Core: `MeshData::uvs`, `core/image.*` (stb_image),
  `TextureResource`, `Material` texture slots + UV/Box mapping. Renderer: 32-byte vertices with UVs,
  mipmapped textures with sRGB/UNORM views, bindless texture set + per-frame material buffer, textured
  PBR + normal/AO maps, triplanar box mapping in meters. App: Import panel (queue, progress/cancel,
  Models, Placement, Report), drag & drop, File ▸ Import (Ctrl+I), `--import`, first import replaces
  the sample house; Materials panel rework (texture slots, presets, mapping, texture grid, apply to
  groups). Four parallel agents built renderer/importers/Import UI/Materials UI against fixed contracts.
- **Verified:** warning-free build; 55 test cases pass (image decode, texture resources, importers,
  texture sets, scene sync textures). Smoke with real files: Khronos BoxTextured/WaterBottle/Lantern
  (textures, PBR), ufbx Maya/Blender FBX (units from file), a textured cm OBJ (V flip, Y→Z, units
  guess warning), ambientCG Bricks076C set (4 maps used, DX/displacement/preview skipped with reasons,
  applied to the selected model).
- **Decisions:** D-021 (import/texture stack, bindless textures).
- **Next:** 0.6 project file + start screen with projects (owner request).

### 2026-10-15 · Claude (omp) · Lighting, look, camera, dense panels (owner-directed)

- **Did:** Owner asked for Lumion-level power instead of "childish" panels → D-019 property widgets
  (`ui::Section`, `ui::SliderField`, `ui::SegmentedControl`, `ui::scrollPage`). Core: `core/solar.*`
  (NOAA sun position, black-body color), `Environment` rewritten (manual / location sun, site, date/time,
  intensity, Kelvin, sky light, shadows), `Look` (exposure, tone mapping, contrast, saturation, warmth,
  vignette, clay), material roughness/metalness, `Scene::duplicate`, camera focal length, two-point
  perspective and eye level. Renderer: sun shadow map pass with descriptor set 0, GGX shading, sky driven
  by the sun, `applyLook` (Filmic/Neutral), mesh bounds for shadow fitting. UI: Light panel (smart
  presets, Sun, Location with cities, Sunlight, Shadows), Render panel (look presets, Camera, Image, Save
  image), Materials roughness/metalness. Owner: Environments (white studio, natural…) will own the sky
  later → no sky color controls; roadmap 5.7 rewritten.
- **Verified:** warning-free build; 32 test cases pass (new: solar positions vs astral for 5 cities/
  seasons, camera lens/two-point). Screenshots of Light, Render, Materials. Scripted clicks: *Location
  & time* → *Golden hour* set 19:14 on 21 June in London, readout "Sunrise 03:44 · Sunset 20:22" (UTC,
  i.e. 04:44/21:22 BST, matching published times); Render: *Dramatic* look, 18 mm lens and two-point
  perspective applied live with vertical walls.
- **Decisions:** D-019 (dense panels), D-020 (interim lighting, sky owned by Environments).
- **Next:** import & textures (owner priority).

### 2026-10-14/15 · Claude (omp) · 2.2 workflow + mode bar, then panel simplification (owner feedback)

- **Did:** `docs/WORKFLOW.md` (principles, screen structure + wireframe, six modes, workspaces, start
  screen, six user journeys). Header bar with the mode bar (Import · Build · Materials · Site · Light ·
  Render, Ctrl+1–6, `--mode`), per-mode layout memory. Real features: **Materials** (project materials
  with usage, rename, color, apply to selected objects, select users, new, delete unused), **Light** (sun
  presets + compass direction/height, stored in `Scene::environment()`, drives the viewport), **Render**
  (save PNG/JPEG, copy to clipboard). Core: `Environment`/`Sun`, resource rename, `userCount`,
  `forEachMaterial`.
  Owner feedback ("left panel confusing, hard to navigate") → D-018: left = one mode panel (single
  title, one-sentence intro), right = Scene above Properties, no tabs, no roadmap lists, short
  "coming soon" cards for Import/Build/Site, "All" mode removed. This also removed a bug where switching
  modes stacked every mode dock on the left and grew the window off-screen.
- **Verified:** warning-free build; 24 test cases pass (new: sun direction, environment notifications,
  resource names/usage). Screenshots of every mode (dark + light). Scripted clicks: select *Main volume*
  in Scene → switch to Materials → pick *Plinth* → *Apply*: the object turned plinth grey, counts updated
  (Clay 2, Plinth 2), window size unchanged, no Qt warnings. Clicking *Evening* lowered the sun live.
- **Decisions:** D-017 (workflow), D-018 (simpler panels).
- **Next:** 2.1b or 0.6/0.7.

### 2026-10-14 · Claude (omp) · 2.1a design system + 2.3 layout (owner-directed)

- **Did:** Token-based design system (`src/app/theme.*`: dark/light palettes, generated stylesheet, Inter
  4.1 + Lucide 1.57.0 bundled with licenses, live-recoloring SVG icon engine), `--gallery`, `--theme`,
  `--screenshot-delay`. Main window rebuilt: viewport toolbar (frame, reset, grid, theme), **Scene**
  outliner dock (live tree model, search, rename, multi-select), **Properties** inspector dock (name,
  transform, shared material base color via sRGB↔linear, mesh info, empty state), layout persistence +
  Reset Layout, theme persistence. Core: `Selection` (pulled from 0.7) and `core/color.*`.
- **Verified:** warning-free `local-mingw-debug`; 21 test cases pass (new: selection, sRGB). Gallery
  screenshots in both themes; app screenshots in both themes; scripted Win32 clicks selected an object
  (inspector filled, outliner highlighted) and a scripted edit of Position Z moved the object in the
  viewport.
- **Decisions:** D-016.
- **Next:** 2.1b help system, or 0.6/0.7. Panels write to the Scene directly until 0.7.

### 2026-10-14 · Claude (omp) · 0.5 Scene document core

- **Did:** `core/scene/` (`Scene`, `Entity`, `Transform`, `MeshInstance`, mesh/material resources, random
  64-bit IDs, change listeners), `core/sample_scene.*` (launch massing built through the document, one
  instanced cube), `render::SceneSync` (lazy mesh upload, release on resource removal, draw-list rebuild),
  `Renderer::releaseMesh` with slot reuse, viewport redraws on document changes, deleted
  `src/app/test_scene.*`. Pulled the doctest harness forward from 0.8: `tests/` with 17 tests.
- **Verified:** `local-mingw-debug` warning-free; `ctest --preset local-mingw-debug` passes (17 cases,
  10,073 assertions); a deliberately injected SceneSync bug (ignoring transform changes) made the suite
  fail; app screenshot identical to 0.4; a temporary hook that rotated/lifted the house group and deleted
  the stair tower 1 s after launch showed both changes in the viewport (hook removed afterwards).
- **Decisions:** D-014 (scene storage and IDs), D-015 (doctest).
- **Next:** owner asked to focus on the UI next.

### 2026-10-14 · Claude (omp) · 0.1–0.4 + branding

- **Did:**
  - Wrote the AI documentation set: `AGENTS.md` (AI readme), `CLAUDE.md`, `README.md`, `docs/ROADMAP.md`
    (stages 0–6 with substages and acceptance criteria), `docs/ARCHITECTURE.md`, `docs/DECISIONS.md`
    (D-001…D-012), `docs/DOMAIN.md`, `docs/BUILDING.md`, this file.
  - Stack chosen by the owner: C++20 + Qt 6 Widgets + Vulkan 1.3, Cycles for final renders (D-001).
  - Build system: CMake presets, pinned FetchContent deps (Vulkan-Headers/volk/glslang
    `vulkan-sdk-1.4.363.0`, VMA 3.4.0, GLM 1.0.3), build-time GLSL→SPIR-V headers, windeployqt post-build.
  - `lb_core`: logger (stderr + file), Z-up orbit camera with infinite reverse-Z projection, mesh data,
    box primitive.
  - `lb_render`: GPU context (device selection with feature checks, VMA, BDA), pipelines, presenter
    (swapchain, 2 frames in flight, 4× MSAA, resize/minimize handling, frame readback), shaders (sky
    gradient, infinite AA grid, two-sided clay shading).
  - `LightBox` app: dark baseline theme with brand accent, main window, embedded Vulkan viewport, status
    bar (GPU, frame time), menus (Quit, Frame Scene F, Reset View Home, Show Grid G, About), app/exe icon,
    `--screenshot`, `--quit-after`, `--validation`.
  - Branding: redesigned the owner's logo concept; the owner then picked the lower-camera variant as the
    primary (`branding/`, D-013), with the isometric original kept in `branding/alt/`.
  - Design additions requested by the owner: contextual how-to help (Fusion-style rich tooltips + ⓘ info
    buttons, toggle in settings) → new substage 2.1b and D-012.
- **Verified:** `local-mingw-debug` (MinGW 13.1, Qt 6.10.3) builds warning-free; app starts on Intel Iris
  Xe (Vulkan 1.4.323, driver 101.7088), renders the test massing with grid and sky
  (`LightBox --screenshot` output inspected); scripted resize ×4, minimize, restore, maximize, mouse drag:
  swapchain recreated each time, clean exit code 0.
- **Toolchain on the dev laptop (no admin):** CMake 4.4.4 + Ninja 1.13.2 + aqtinstall via `pip --user`,
  Qt 6.10.3 MinGW + MinGW 13.1 in `C:/Users/30HamghalamA/Qt`; machine paths live in the git-ignored
  `CMakeUserPresets.json` (presets `local-mingw-debug` / `local-mingw-release`).
- **Open issues / next:**
  - 0.5: introduce `lb::Scene` and a renderer scene-sync layer; delete `src/app/test_scene.*`.
  - Validation layers unavailable locally; CI job planned in 0.8.
  - The Stage 0 sky/clay look is a placeholder by design; Stage 3 replaces it.
