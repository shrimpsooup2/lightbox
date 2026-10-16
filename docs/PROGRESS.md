# LightBox Progress

## Current position

- **Stage:** 0 (0.6–0.8 open) and 1 (import) in progress; owner-directed UI, lighting and import work done.
- **Done:** 0.1–0.5, 1.1, 1.3, 2.1a, 2.2; partial: 1.2, 1.4 (FBX), 1.9, 2.3, 2.8, 3.1, 3.3; interim
  lighting/look/camera pulled forward from 2.4, 3.2, 3.4, 3.5 (D-020)
- **Next substage (owner's choice):** **start screen with projects**, which needs **0.6 project file**
  (save/load `.lbox`) first: build them together (start screen = the recents/templates/sample part of 2.10).
  Then 0.7 commands/undo.
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
