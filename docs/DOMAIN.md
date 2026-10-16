# LightBox Domain Primer: Architecture, Landscape and Visualization

Read this before making product or UX decisions. It explains who uses LightBox, what they produce, what
files they bring, and which words they expect. Engineering rules live in `AGENTS.md`; stages in
`docs/ROADMAP.md`. Numbers below are typical ranges, not standards. When in doubt, pick the
conservative, architect-friendly option.

---

## 1. Users & context

| User | Typical situation | What they need from LightBox |
|---|---|---|
| Small practice (1–10 people) | Mixed projects (houses, extensions, small commercial). No dedicated visualization staff. | Client-ready images fast, without a specialist. |
| Solo architect | Does design, drawings, planning and marketing alone. | One-click good results; minimal learning time. |
| Landscape architect | Gardens, public realm, parks, courtyards, water features. | Plants, ground cover, terrain, water, seasons. |
| Interior designer | Rooms, finishes, furniture, lighting schemes. | Accurate materials, artificial lighting, close-up quality. |
| Student | Studio projects, portfolio, deadlines, school laptop. | Free, runs on weak hardware, impressive output. |

Hardware reality:
- Many users run **laptops**, often with **integrated GPUs** (Intel/AMD iGPU) or entry/mid-range discrete
  GPUs. Workstation GPUs are the exception.
- Ray-tracing-capable GPUs are common but not universal. The non-RT path must look good on its own.
- Memory is limited: large site models plus vegetation can exhaust VRAM. Degrade gracefully, never crash.

Why they need LightBox: commercial real-time renderers are subscription products many small offices and
students cannot justify. Free alternatives (Blender) are powerful but take a long time to learn. LightBox
aims for the commercial-tool workflow at no cost.

---

## 2. Typical workflow

```
Design (SketchUp / Revit / ArchiCAD / Rhino / Vectorworks / AutoCAD)
   -> export (or save native file)
   -> LightBox: import, materials, lighting, entourage, views
   -> render iterations (Live engine for preview, Final engine for stills)
   -> deliverables: client presentation, planning application, competition, website, portfolio
   -> design changes -> re-export -> re-import -> re-render (repeat many times)
```

- The design model is **owned by the CAD/BIM app**. LightBox is downstream; users rarely model in it.
- Design changes continue until late. A project goes through **many re-import cycles**.
- **Re-import must preserve LightBox work**: material assignments, added lights, placed plants/people,
  saved views, and render settings. Match by stable keys (object/layer/material names, IFC GUIDs) and keep
  overrides. If users must redo materials after each change, they will abandon the tool.
- Views are reused across iterations: the same camera angles get re-rendered after each design change so
  the client can compare.
- Typical deadline pattern: images are needed the day before a meeting. Speed and predictability beat
  ultimate quality.

---

## 3. Deliverables architects produce

| Deliverable | What it is | What LightBox needs |
|---|---|---|
| Exterior hero shot | The main marketing/competition image of the building at eye level. | Photoreal Final render, sky/sun, entourage, two-point perspective, high resolution. |
| Aerial / bird's-eye | High view showing building in site context. | Large-scene performance, context buildings, terrain, scattered vegetation, distant LOD. |
| Interior view | Rooms with furniture and finishes. | Wide lenses, artificial lighting, GI, accurate materials, exposure control. |
| Day / dusk / night series | Same view at different times. | Sun position by time, sky models, interior lights glowing at dusk, light presets per view. |
| Orthographic elevation | Flat facade view, no perspective. | Orthographic camera aligned to facade, scale-correct output, optional shadows. |
| Rendered plan | Top-down orthographic, often with shadows and planting. | Ortho top view, cut at storey height, scale output (e.g. 1:100 at given DPI). |
| Section with poché | Building cut by a plane, cut material filled solid. | Section/clipping planes, cap fill on cut surfaces (poché), view-beyond rendering. |
| Axonometric | Parallel-projection 3D view (isometric or similar). | Orthographic camera at arbitrary angle, often clay or line style, exploded/cut options. |
| Sun / shadow study | Shadow casting at set dates/times, e.g. for planning applications. | Accurate sun from latitude/longitude, date, time, time zone, true north. Batch output per hour. Must be defensible. |
| 360° panorama / VR | Spherical image for web viewers or headsets. | Equirectangular output (mono, optionally stereo), eye-height camera. |
| Walkthrough / flythrough | Animated camera path video. | Camera paths, smooth easing, video encoding, consistent frame-to-frame lighting (no flicker). |
| Clay / white model | All surfaces in neutral white/grey, emphasizing form and light. | One-click material override, optional glass kept transparent. |
| Diagram / line style | Graphic styles: outlines, flat colors, hidden lines, ghosted context. | Edge/outline rendering, flat shading, per-layer style overrides. |
| Material board | Swatches of the specified materials. | Material preview/export, consistent material thumbnails at real scale. |

---

## 4. Input formats

License rule (from `AGENTS.md`): no GPL/AGPL code in-process. Proprietary SDKs only as optional plugins.

| Format | Source apps | Contains | Gotchas | Planned LightBox handling |
|---|---|---|---|---|
| DWG | AutoCAD, most CAD apps | 2D drawings, sometimes 3D solids | Closed, versioned format. GPL readers (LibreDWG) are forbidden. | Convert via user-installed **ODA File Converter** as external process (DWG -> DXF), then DXF reader. Guide user if absent. |
| DXF | AutoCAD and nearly all CAD | 2D lines, polylines, arcs, hatches, blocks, text, layers; some 3D | ASCII or binary; units in `$INSUNITS` often unset; huge coordinates. | Own reader. Plans as layer-toggleable underlays per storey. Ask units only if missing. |
| PDF plans | Any app, consultants | Vector/raster drawing pages | Scale unknown; vector data not reliable geometry. | Rasterize page as underlay (PDFium); scale calibration tool. |
| Raster plans (PNG/JPG/TIFF) | Scans, screenshots | Image of a plan | No scale. | Underlay + two-point scale calibration. |
| SKP | SketchUp | Meshes, components, groups, tags, materials with textures | Proprietary; official SDK has its own terms. | Optional SDK plugin. Fallback: guide user to export glTF/FBX/DAE. |
| RVT | Revit | Full BIM database | No public reader; not importable directly. | Guide user to export IFC, FBX or glTF from Revit. |
| PLN / PLA | ArchiCAD | BIM project | Proprietary. | Guide user to export IFC (or other mesh formats). |
| 3DM | Rhino | NURBS, meshes, blocks, layers, materials | NURBS need meshing; render meshes may be absent. | openNURBS: use stored render meshes; blocks -> instances; layers preserved. |
| IFC | Revit, ArchiCAD, Vectorworks, most BIM | Elements by type (wall, slab, window), spatial structure (site/building/storey), materials, GUIDs | Large files, inconsistent materials across exporters, georeferencing. | IfcOpenShell (LGPL, dynamic) or web-ifc (MPL). Storeys -> outliner groups; element type for bulk material assignment; GUIDs for re-import matching. |
| FBX | Revit, 3ds Max, SketchUp, many | Meshes, hierarchy, materials, cameras | Unit/axis scaling varies by exporter; material conversion lossy. | ufbx. Detect units/up-axis; keep material names. |
| OBJ | Universal | Meshes, UVs, MTL materials | No units, no hierarchy beyond groups; MTL paths often broken. | Assume meters unless dimensions suggest otherwise; ask with good default. Search for textures nearby. |
| glTF / GLB | Modern exporters, Blender, SketchUp plugins | Meshes, PBR materials, hierarchy, instancing | Meters and Y-up by spec; reliable. | Preferred interchange format. Recommend it in fallback guidance. |
| DAE (Collada) | SketchUp, older tools | Meshes, materials, hierarchy | Verbose, exporter variations. | Assimp or focused reader. |
| 3DS | Legacy 3ds Max, asset libraries | Meshes, basic materials | Old limits (names, vertex counts). | Assimp or focused reader. Mostly for old assets. |
| STL | 3D printing, some CAD | Triangles only | No materials, no units, no hierarchy. | Import as single material object; ask units. |
| USD | Pipeline tools, some newer exporters | Scenes, materials, variants | Complex; later stage. | Later. |

---

## 5. CAD/BIM geometry gotchas

| Problem | Cause | LightBox response |
|---|---|---|
| Missing UVs | CAD models rarely have texture coordinates. | Box/triplanar mapping in **world-scale** units (a 1 m brick texture = 1 m). Never stretch per object. |
| Flipped / inconsistent normals | SketchUp back faces, mixed winding. | Render **two-sided by default**; flip shading normal toward viewer. |
| Coplanar faces / z-fighting | Duplicated surfaces, overlapping finishes. | Reverse-Z depth, sensible near plane; optional tool to detect overlaps. |
| Huge coordinates | Georeferenced survey coordinates (km from origin). | Recenter with stored **origin offset**; keep offset for re-import and export alignment. |
| Mixed units | mm (UK/EU architecture), m (landscape/survey), ft-in (US). | Read units from file; sanity check (a 3,000 m tall door means mm read as m). Display in project units. |
| Nested blocks / components | Repeated windows, chairs, trees. | Map to **instances**; keep hierarchy for selection. |
| Materials named by layer | AutoCAD/Rhino workflow: color/material by layer. | Use layer name as material slot when no material exists; allow assignment by layer. |
| Very high triangle counts | Curved surfaces, detailed furniture, NURBS meshing. | Instancing, LOD, culling; warn but still import. |
| Walls with no thickness | Single-face walls in SketchUp/massing. | Two-sided rendering; do not assume closed solids. |
| Glass as single faces | Windows modeled as one plane. | Thin-glass material (no refraction offset) as default for glass. |
| Hidden/helper geometry | Section boxes, guides, 2D annotations in 3D files. | Import to separate hidden layers rather than delete. |

---

## 6. Vocabulary for the UI

Use the words architects use. Avoid game-engine and VFX jargon in user-facing text.

| Say this | Not this | Meaning |
|---|---|---|
| Plan | Top view map | Horizontal cut through a building seen from above. |
| Section | Slice, cross-cut | Vertical cut through a building. |
| Elevation | Side view | Flat orthographic view of a facade. |
| Storey / Level | Floor layer, game level | A floor of a building at a given height. Follow source app wording where possible. |
| Layer / Tag | Group channel | Organizing category from CAD (SketchUp calls them tags). |
| Massing | Blockout, greybox | Simple volumes showing building size and shape. |
| Entourage | Props, NPCs | People, cars, furniture, plants added for context and scale. |
| Hardscape | Static meshes | Paving, walls, steps, built landscape elements. |
| Softscape | Foliage | Planting: trees, shrubs, grass, groundcover. |
| Planting plan | Foliage paint map | Drawing/layout of what is planted where. |
| Groundcover | Grass painter | Low plants covering soil. |
| Contour | Height line | Line of equal elevation on terrain. |
| Grading | Terrain sculpt | Shaping ground levels to design. |
| Site | Map, world | The land the project sits on. |
| Context | Background meshes | Surrounding existing buildings/streets. |
| View | Camera actor, shot | A saved camera position and settings. |
| Sun study | Time-of-day cycle | Shadow analysis at real date/time/location. |
| Clay render / White model | Unlit, override mode | All-neutral material render. |
| Poché | Cap, fill | Solid fill on cut walls/floors in section or plan. |
| Axonometric | Iso camera | Parallel projection 3D view. |
| Two-point perspective | Vertical correction | Perspective with vertical lines kept vertical. |
| Material | Shader | Surface definition (color, roughness, texture). |
| Object / Component | Actor, prefab, blueprint | Placed element in the scene. |
| Place / Add | Spawn | Put something into the scene. |
| Project / Scene | Level, map | The document being worked on. |
| Render / Image | Frame capture | Produced output. |
| Exposure | Tonemapper gain | Image brightness in photographic terms. |

---

## 7. Photography & presentation conventions

- **Eye height** about 1.6 m for standing views (1.2 m seated). Default cameras to this.
- **Vertical lines vertical**: architectural photography uses shift lenses. Default to two-point perspective
  (vertical correction on); allow three-point for dramatic up/down shots.
- **Focal lengths** (full-frame equivalent):
  - Interiors: about 16–24 mm. Wider distorts edges; avoid going extreme by default.
  - Exteriors: about 24–50 mm. Longer (e.g. 70+ mm) for compressed street views.
  - Express in mm, not field-of-view degrees, with FOV shown secondary.
- **Moods**:
  - Golden hour: low warm sun, long shadows; flattering.
  - Overcast: soft, even light; good for materials and landscape.
  - Dusk / blue hour: deep blue sky, warm interior lights glowing; very popular for hero shots.
  - Night: artificial and landscape lighting only.
  - Midday: hard shadows; used for sun studies more than marketing.
- **People for scale**: a few people at natural positions, not crowds; avoid all looking at camera.
- **Composition**: rule of thirds, horizon placement, foreground framing (planting, trees), leading lines.
  Offer overlays (thirds, safe frames) and common aspect ratios (3:2, 16:9, 4:5, A-series paper).
- Avoid lens effects overload: subtle vignette/bloom are accepted; heavy chromatic aberration is not.

---

## 8. Lighting knowledge

Color temperature:

| Source | Approx. Kelvin |
|---|---|
| Candle / flame | ~1900 K |
| Warm residential LED / incandescent | 2700 K |
| Warm-neutral / hospitality | 3000 K |
| Neutral / retail | 3500–4000 K |
| Office | ~4000 K |
| Direct sun around midday | ~5000–5500 K |
| Daylight (sun + sky) | ~5500–6500 K |
| Overcast sky | cooler, ~6500–7500 K |
| Clear blue sky (shade) | cooler still, ~8000 K+ |

Illuminance (lux):

| Situation | Approx. lux |
|---|---|
| Direct sunlight | up to ~100,000 |
| Overcast day | ~1,000–10,000 |
| Office working plane | 300–500 |
| Living room | ~50–150 |
| Street lighting | ~5–30 |
| Full moon | <1 |

Luminous flux (lumens), typical:

| Fixture | Approx. lumens |
|---|---|
| Small LED downlight | 400–800 |
| Domestic LED bulb (60 W equivalent) | ~800 |
| Office panel / linear LED | 3,000–4,500 |
| LED strip | ~300–1,500 per metre |
| Garden spike / uplight | 100–500 |
| Street light | several thousand |

- **IES files**: manufacturers publish photometric files describing a fixture's light distribution. Users
  expect to drop an `.ies` file onto a light. Support both `.ies` and lumens/Kelvin entry.
- **Why physical units matter**: lighting designers and architects specify fixtures by lumens and Kelvin.
  Physical units make day and night, interior and exterior consistent with one exposure model, and let
  users trust results. Present friendly controls (presets, Kelvin slider) backed by physical values.
- Exposure is photographic: EV, with auto exposure as default and manual override.

---

## 9. Landscape & site knowledge

- **Planting layers** (top to bottom): trees (canopy), shrubs (mid layer), perennials/grasses,
  groundcover, lawn. Good scenes use all layers; a lawn plus trees alone looks sparse.
- **Seasons**: deciduous trees bare in winter, blossom in spring, autumn colors. Evergreens constant.
  Seasonal variants of the same plant are a valued feature.
- **Size at planting vs maturity**: clients see mature designs; planners may require planting-day sizes.
  Plants need a height parameter or age/growth control (e.g. 5 years, 15 years).
- **Hedges**: clipped linear planting; need path-based placement with height/width.
- **Paving**: stone, concrete, brick, gravel, decking; laid in patterns; joint width matters at close range.
- **Terraces**: level outdoor areas, often stepped down a slope.
- **Retaining walls**: walls holding back ground where levels change.
- **Swales / rain gardens**: shallow planted channels managing stormwater.
- **Contours & grading**: terrain shaped from contour lines or survey points; existing vs proposed levels.
  Import contours from DXF as terrain input.
- **Site context**: neighbouring buildings, often as simple massing; roads, kerbs, street furniture.
- **Variation**: real planting is irregular. Scatter tools need random scale, rotation and species mix.

---

## 10. Water feature vocabulary

| Term | Meaning |
|---|---|
| Weir | Edge over which water flows from one level to another. |
| Spillway | Channel or edge that carries overflow water down. |
| Scupper | Spout or opening that projects water out from a wall as a falling stream. |
| Runnel / Rill | Narrow shallow channel of flowing water set into paving. |
| Cascade | Series of steps or drops water flows down. |
| Reflecting pool | Still, shallow pool designed for mirror-like reflections. |
| Infinity / vanishing edge | Pool edge where water flows over and seems to disappear into the view. |
| Perimeter overflow | Water spills over all edges into a surrounding gutter; surface flush with deck. |
| Catch basin | Lower basin/trough that collects falling water. |
| Laminar jet | Smooth glass-like arc of water, no turbulence. |
| Bubbler | Low vertical jet producing a frothy dome. |
| Deck jet | Jet emerging from paving, often in plazas. |
| Mist / fogger | Fine water vapor effect. |
| Sheet fall | Thin, wide, smooth curtain of falling water. |
| Spill bowl | Raised bowl overflowing at its rim. |
| Koi pond | Ornamental fish pond, usually deeper with planted edges. |
| Natural swimming pond | Swimming pool cleaned by plants instead of chemicals; natural look. |

---

## 11. Competitive landscape

| Product | What users love | What LightBox should learn |
|---|---|---|
| Enscape | Live link inside the CAD app; very simple; good results with few settings. | Seamless re-import, minimal UI, instant decent result. |
| Lumion | Large asset library, effects stack, speed to good-looking images. | Asset depth, stackable presets/effects, fast workflow. |
| Twinmotion | Seasons, vegetation growth, paths with people and cars, weather. | Landscape-friendly tools, animated entourage, environment controls. |
| D5 Render | Real-time ray-traced GI, rich asset library, scatter tools. | RT tier quality, scatter, modern UI. |
| V-Ray / Corona | Photoreal final renders, light mix after render, interactive rendering. | Final-quality expectations, post-render light adjustments. |
| Blender + Cycles | Free and powerful. Steep learning curve. | LightBox should be the easy front end to Cycles-quality output. |
| KeyShot | Drag-and-drop materials, fast product-style results. | Drag materials onto objects; immediate feedback. |

---

## 12. What "easy" means for this audience

- **Drag and drop import**: drop a file, it appears at correct scale and orientation. Questions only when
  ambiguous, with a default selected.
- **Sensible defaults that look good instantly**: sun, sky, ground, exposure and materials configured on
  import. No black screen.
- **No node graphs required**: materials are edited with sliders, swatches and texture slots. Advanced
  options may exist but are never needed.
- **Real-world units everywhere**: meters/mm/ft-in, lux, lumens, Kelvin, mm focal length; texture size in
  real dimensions.
- **Presets over parameters**: "Golden hour", "Overcast", "Blue hour"; "Polished concrete", "Oak floor".
  Parameters available behind a preset.
- **Fast feedback**: viewport updates immediately; renders show progress; never freeze the UI.
- **Undo** for everything, including imports and material changes.
- **Familiar navigation**: navigation presets matching SketchUp/Revit/Rhino.
- **Safe re-import**: work survives model updates.
- **Plain language**: architect vocabulary (section 6), clear error messages with a next step.
