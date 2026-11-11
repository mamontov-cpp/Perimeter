# Perimeter Map Editor — Help

A standalone application for creating and editing Perimeter worlds and battle
maps. This manual describes the typical workflows. Technical details of the
formats are in `docs/map_editor.md` and `docs/architecture.md` of the
repository at https://github.com/mamontov-cpp/Perimeter/tree/map-editor.
The editor was built for maximum compatibility with the original game, so
many limitations here come from the game's own quirks.

A basic introduction: in the editor every brush, except a few special ones,
works in one of two modes — Geo and Dam.

- **Geo** — the base terrain. If its height is 0, it is dimmed in the game
  and treated as chaos (no movement, no building).
- **Dam** — the dam layer: essentially the same terrain, but always on top
  of Geo, and where it exists the surface is 100% not chaos.

This behaviour is a legacy of the game itself.

Map sizes are limited to 2^13 — Terra and the game crash with an assertion
on larger sizes.

## Quick start

1. **File → Select Game Directory...** — point at the game folder (the one
   containing `RESOURCE/`). Usually `build/Source/Debug` in the project tree.
2. **File → Load World...** (`Ctrl+O`) — pick a world from
   `RESOURCE/Worlds`. If the game folder has
   `Mods/<ModName>/RESOURCE/Worlds/`, mod worlds appear in the same list as
   `<ModName> / <World>` and are edited (and saved) right in the mod folder.
3. Edit the relief and colors with the brushes; view in 2D or 3D.
4. **File → Save** (`Ctrl+S`) — the world is saved to its folder (including
   `cache.tga`/`map.tga`, which the game reads when loading).

## Creating a map for Skirmish

A battle map (`.spg`) is the players' spawn points, cameras and player
settings bound to a world. The relief lives in the world itself.

The same applies to the Survival mode.

### Option A: a map on an existing world

1. Load the world (**Load World**).
2. Edit the relief/colors with the brushes and save the world.
3. **File → Open Battle Map (.spg)...** — open an existing map that uses
   this world (e.g. `RESOURCE/Battle/COMPASS.spg`), or create a new one
   (option B).
4. In the **Battle Map** panel (key `B`):
   - select a player in the list;
   - **Move to View Centre** — place the player's spawn group at the centre
     of the current view (frame + squad + units + camera relocate together,
     the height is auto-corrected to the relief);
   - **Edit Properties...** — name, player type (human/AI), faction, color,
     clan, difficulty, handicap, and the **spawn location (Location X/Y)**:
     frame + squad + units + camera move to the point, the height corrected
     to the relief exactly like Move to View Centre;
   - **Place Spawn** (the **button in the Battle Map panel**, the mode
     button on the Brush toolbar, Edit → Editor Mode, key `S`) — a click on
     the map moves the spawn of **the player selected in the list** to that
     cell: the relief does not change, the height is corrected to the
     relief, and the move is one undo entry (`Ctrl+Z` moves the spawn back).
     The mode is available only while a battle map is loaded; the selected
     row survives every move — you can place the spawn repeatedly;
   - **Remove Player** — remove the selected player from the list (the last
     player cannot be removed). The change is saved together with the map
     (File → Save Battle Map);
   - **Go To** (double click) — jump to the player's spawn.
   Spawns are visible on the map itself: in the 2D view — colored rings (no
   labels; names are only in the panel), in the 3D view — colored discs on
   the relief at the spawn point. The position updates on every move.

All map-click tools are mutually exclusive: **Place Spawn** and the
**Place / Select / Delete** tools of the Buildings, Scum Nests and
Nature panels. Activating one resets the others (exactly one button is
highlighted at a moment); clicking the active button again releases it
— and the click works as the normal brush again, as it does after
choosing any brush or mode (**Brush**, **View**, **Eyedropper**) on the
mode toolbar.

### Pre-placed buildings (Buildings panel)

The **Buildings** panel (tab next to Battle Map; active only when a battle
map is loaded/created) places neutral buildings of the world — they are
written to the `.spg` (`nobodysBuildings`), the relief does not change:

- a grid of 43 buildings (Core, Relay, Collector, plants, the command
  center, laboratories of all factions/levels, corridors, guns; the frame,
  scum spots and the black hole are excluded); the buttons show icons baked
  from the actual in-game M3D models (if the GPU viewport is not up yet —
  text labels; the icons appear the next time the panel is shown);
- tools: **Place** (a click places the selected building one level above the
  zeroplast — like in the game, `z = hZeroPlast − boxMin().z`; a green/red
  circle and the model preview follow the cursor; overlaps with other
  buildings are rejected), **Select** (a click selects a building; the
  **Rotation** and **HP** spins edit the selection), **Delete** (a click
  deletes);
- **Charge** (a 0–100 % spin, shown only for the superweapons — Scum
  Disruptor, Missile Gun (Ballistic), Filth Navigator): the initial shot
  charge (`weaponChargeLevel` 0..1 in the `.spg`). **Place** stamps the
  chosen value, **Select** edits the charge of a building already on the
  map; 100 % = the shot is available right away (the game's default),
  0 % = charging from empty;
- buildings are visible: in the 2D view and on the minimap as white dots
  (like the game's radar), in 3D as the actual M3D models with the game's
  textures; the selection is highlighted orange;
- saving is the regular **Save Battle Map** (buildings live in the `.spg`);
  Undo does not apply to buildings.

### Scum nests (Scum Nests panel)

The **Scum Nests** panel (tab next to Battle Map; active when a battle map
is loaded/created) places scum nests — pairs of "visible mound + invisible
spawner":

- 4 mound variants (SCUM_SPOT, SCUM_SPOT 2/3/4 — different models and HP;
  SPOT 4 also damages the foundations of neighbouring buildings);
- the tools are the same as for buildings: **Place** (a click places the
  mound on the zeroplast and a FILTH_SPOT spawner beneath it; the minimum
  distance between mounds is 150 units — violations are rejected; a ghost
  circle follows the cursor), **Select** (a click selects a nest; the
  **Rotation** and **HP** spins edit the mound), **Delete** (a click deletes
  the whole nest);
- the **Filth spawner** block edits the spawner parameters of the SELECTED
  nest: filth type (ants, wasps, dragon, crows, volcano...), creatures per
  spawn, sleep and the periods, what activates it (unit + radius), whom to
  attack; a new nest stamps the current block values;
- on **Save Battle Map** the editor writes
  `Scripts/Triggers/<world name>.scr` — triggers enable the spawner while
  its mound is alive and disable it when the mound is destroyed (like on
  03isoland) — and registers the file for every player in TriggerChainNames
  next to the mp-N.scr files; when there are no nests, the file and the
  registration are removed;
- the mounds are visible in 3D as the game's models, in 2D as white dots;
  they are not shown on the minimap or in the exported top-down preview.

Placing FILTH_SPOTs separately is not implemented: in the game an
unstoppable filth spawn is not very playable.
Note that some enemies behave oddly — for example ghosts attack scum nests
for some reason. That is a game quirk; the editor is not at fault.

### Nature (Nature panel)

The **Nature** panel (tab next to Battle Map; active when a battle map is
loaded/created) places static nature — trees, rocks, effects
(`SavePrm.environment`, each object stores its model):

- the model list is the content of `RESOURCE/Models/Environment` (as in the
  game);
- tools: **Place** (a click places an object with the parameters below; the
  base height is the zeroplast, the **Z** field edits it), **Select** (a
  click selects an object; **Rotation**, **Z**, **Radius** and the flags
  edit the selection), **Delete** (a click deletes);
- **Radius** — the model scale (the footprint radius in world units);
- flags: **Destroy** (can be destroyed), **Collisions** (real collisions),
  **Lighting** (lighting);
- HP is not editable — always full;
- objects are visible in 3D as their models, in 2D as green dots.
5. **File → Save Battle Map** — save; **Save Battle Map As...** — save
   under a new name into `RESOURCE/Battle` or `RESOURCE/Worlds`.
6. Validate the map: **Tools → Validate Battle Map...**
7. Launch the game — the map appears in the Skirmish list.

### Option B: a new map from scratch

1. **File → New World...** — the new-world wizard:
   - world name and a template world (palettes and textures are copied
     from it);
   - relief: **flat** (the geo layer height; it must be below the zeroplast
     thickness) or **generated** (the game's procedural relief);
   - map size (a power of two: **9 = 512×512 minimum** — the editor
     refuses to create anything smaller — up to 13 = 8192×8192; the game
     allows 14 and such worlds still load, but the creation dialog does
     not offer them), the zeroplast height (`hZeroPlast`).
   The created world is loaded and saved right away.
2. Shape the relief with the brushes; place zeroplast and base sites.
3. **File → Save**.
4. **File → New Battle Map...** — pick a map template and the player count
   (1–8). The map is created for the loaded world and saved right away as
   `<world name>.spg` (the game expects the battle map to share the
   world's name — the dialog has no name field); the spawns are placed on
   an ellipse around the map centre — move each to a suitable spot (the
   **New Battle Map...** button is also in the **Battle Map** panel,
   key `B`).
5. Save the map and check it in the game.

Tip: the battle map's `zeroLayerHeight` is taken from the world's
`hZeroPlast` parameter at creation; if the world was edited — **Tools →
Autofix: Set zeroLayerHeight from World**.

## Brushes

Pick a brush on the **Brush** panel (toolbar) or in the **Edit → Brush
Type / Mode** menu. The left mouse button paints; holding it down gives a
continuous stroke.

### Brush types (effects)

| Brush | Action |
|---|---|
| **Raise / Lower** | raise/lower the relief by *Strength* per stamp |
| **Level to Zeroplast** | level to the zeroplast height and mark the cells as zeroplast in one stamp; the brush strength has no effect (Dam mode only) |
| **Level to Height** | set the relief exactly to the chosen height in one stamp (the spinbox next to the brushes); overlapping stamps never rise above the target |
| **Color** | paint the surface with the exact 24-bit RGB from the color panel, without changing the height; the nearest palette entry of the layer is kept as the surface type |
| **Image Decal** | stamp a picture in exact 24-bit colors on the dam surface: the button immediately asks for a PNG/TGA/BMP (at most 2048 px per side), colors pass through unquantized (the nearest Dam palette entry is kept as the cell's surface type). Dam mode only; transparent pixels do not paint |
| **Pick Color** | eyedropper: take the color of the cell under the cursor |
| **Soot** | "soot" — a cosmetic dark mark |
| **Very Light Dam** | very light dam (a special dam palette entry) |

### Modes (layer)

- **Geo** — edits the geo layer (the main relief). Dams are not touched:
  raising under a dam raises the geo beneath it; paint lands only on
  non-dam cells.
- **Dam** — edits the dam layer (the zeroplast plateaus): raising grows the
  dam (never below geo — a game invariant), lowering removes it down to the
  geo. The zeroplast brushes and **Image Decal** are available in this mode
  only.

### Shapes (stamp)

- filled circle / square;
- circle / square outline;
- **regular polygon** (filled and outline): the vertices lie on the circle
  of the brush radius (like the circle); the **vertex count** (3–20) and the
  **first-vertex angle** (0–360°, wrapping) are set by the two spinboxes
  next to the radius spinbox — active only with a polygon shape selected.
  Angle 0° puts a vertex to the east (+x); the angle grows clockwise;
- line — two clicks (start, end), stamps along the segment;
- **image mask** (**Load Image**): any PNG/JPG/BMP/TGA becomes the stamp;
  transparent pixels do not paint; the mask scale is the spinbox next to it.
- **Image Decal** — its own geometry: the stamp is exactly the picture size
  (the scale is the same spinbox as the image mask).

For the unfilled shapes (circle outline, square outline, polygon outline,
line) a **thickness spinbox** is available next to the radius spinbox — the
strip width in cells. The strip grows INWARD from the outer edge, so a wide
stroke does not extend beyond the normal brush footprint. Thickness 1 is the
former one-cell border; for a line 1 it is exactly Bresenham's path. The
value is limited by the stamp extent (2·radius+1).

### Image Decal (a picture in dam colors)

The **Image Decal** button is both the brush type and the file picker. The
flow:

1. Pick a picture (PNG/TGA/BMP, at most 2048 px per side — otherwise the
   editor shows an error).
2. The picture is NOT quantized: every opaque pixel keeps its exact 24-bit
   RGB (the palette no longer limits the color count). The nearest Dam
   palette entry is remembered as the cell's surface type.
3. A click/drag places the picture onto the dam: every pixel paints its
   cell with its exact color. The height does not change; transparent (and
   "almost transparent") pixels do not paint at all; cells without a dam are
   skipped.
5. The scale spinbox next to it grows/shrinks the stamp (100 % is the
   original picture size; pixels are not smoothed).

One stamp is one Undo entry (stamps closer than 500 ms merge into a single
transaction — see Undo/Redo under "Parameters").

### Scripted Brush (JavaScript)

The **Scripted Brush** button (the "JS" icon) enables the brush type that
**runs a JavaScript script once per stamp** (the duktape engine) from the
click point. The pencil button next to it opens the **script editor
window** (OK/Cancel):

- a multi-line editor with highlighting: **API functions — bold blue**, JS
  keywords — purple italic, strings/numbers — brown, comments — gray; line
  numbers on the left;
- the **stamp preview shape** selector (circle/square) — the cursor outline
  while the scripted brush is active;
- **OK** compiles the script (syntax check): on error a message with the
  line number is shown and the window stays open. Cancel discards the edits.

Limitations and semantics:

- Works in **Dam mode only** (selecting the brush switches the mode
  automatically; the Geo button is disabled while the script is active);
- **one stamp = one Undo entry** — no matter how many operations the script
  performs, Ctrl+Z reverts its entire work at once (stamps closer than 500
  ms merge into one transaction, like the regular brushes);
- a **60 s time limit** per stamp — an infinite loop is aborted with an
  error;
- a runtime error (exception, timeout, empty script) is shown in a message
  box; the partial changes are reverted by the same Ctrl+Z;
- the script runs in the engine's sandbox without file access; from the UI
  it can only reach the message boxes `showInfoMessageBox` /
  `showErrorMessageBox` — plus the API below and standard ECMAScript.

**API** (`pos` — an `{x: int, y: int}` object in map cells; `radius` — a
number ≥ 1, the circle radius or the square half-side, up to 512; `square`
— boolean: true = filled square, false = filled circle):

| Function | Action (analogue of the editor brush) |
|---|---|
| `getCurrentPos()` | `{x, y}` — the current stamp point |
| `getCurrentRadius()` | the brush radius from the UI |
| `getCurrentThickness()` | the stroke thickness from the UI |
| `getCurrentHeight()` | the brush height from the UI (the "vox" spinbox, a number) |
| `raiseHeight(pos, height, radius, square)` | raise the dam by `height` voxels — whole or fractional (10.4 rounds to the nearest 1/32-voxel step, like in the game) |
| `lowerHeight(pos, height, radius, square)` | lower the dam by `height` voxels (fractional — like raiseHeight) |
| `setHeight(pos, height, radius, square)` | set the dam to height `height` (fractional — like raiseHeight) |
| `setToZP(pos, radius, square)` | level to the zeroplast + mark |
| `paintSoot(pos, radius, square)` | soot |
| `paintVeryLightDam(pos, radius, square)` | "very light dam" |
| `paintColor(pos, red, green, blue, radius, square)` | paint: the exact RGB (0..255), no palette quantization |
| `applyBlur(pos, radius, blurRadius, effect, square)` | Gaussian Blur: smooth the heights with a Gaussian kernel; `blurRadius` is clamped to `1..radius-1`, `effect` — the effect fraction 0..1 (1.0 = reach in one stamp) |
| `applySmooth(pos, radius, effect, square)` | Tricubic: smooth towards the tricubic (bicubic Catmull-Rom) interpolation of the lattice under the stamp — like the Tricubic brush on the Sculpting toolbar; `effect` — the fraction 0..1 |
| `applyHeightFn(pos, radius, fn, square)` | set the height **by a function**: `fn(x, y, h)` is called for every covered cell with its coordinates and the PRE-EDIT dam surface height `h` (fractional, 1/32 step; 0 where there is no dam) and returns the cell's new **absolute** height (fractional — like raiseHeight); all reads see the original surface before any write, so cells never influence each other |
| `getDamHeight(pos)` | the dam height at the cell in whole voxels (**0** when there is no dam) |
| `getGeoColor(pos)` | `{r, g, b}` — the cell's SurBuf index through the **Geo** palette |
| `getDamColor(pos)` | `{r, g, b}` — the cell's SurBuf index through the **Dam** palette |
| `setGeoPalette(index, {r, g, b})` | write color `index` (0..255) of the **Geo** palette |
| `setDamPalette(index, {r, g, b})` | write color `index` (0..255) of the **Dam** palette |
| `showInfoMessageBox(text)` | an information QMessageBox with the text |
| `showErrorMessageBox(text)` | an error QMessageBox with the text |

The same API is available from **Tools → Run Script...** — the "run script"
window (without the stamp preview settings; **Run** and **Cancel** buttons).
Running executes the text once: `getCurrentPos()` is the cell under the
cursor (the map centre when the cursor is outside it), the whole run is one
Undo step, the 60 s limit applies.

The getters validate the position: a `pos` outside the map raises an
exception with a message. Colors follow the game's render model: a cell
stores a single SurBuf index, and the palette (Geo or Dam) is selected by
the layer — so `getGeoColor` and `getDamColor` read the same index through
different palettes. The message boxes are the script's only access to the
UI; any JS value is converted to a string for them.

A note on `raiseHeight`/`lowerHeight`: these are **incremental** ops — a
radius-1 stamp covers a 3×3 area around the centre, so stamps at adjacent
cells (distance 1) always overlap and the overlapped cell receives, say,
+10 again — the sums pile up. Distinct heights in adjacent cells cannot be
placed that way. Options without mutual influence: `applyHeightFn` (one
operation: every cell is computed against the pre-edit surface first,
then all writes land — any seamless function of position and the original
height), `setHeight` (absolute set: overlaps do not add up, but the last
stamp overwrites the shared cells), or radius-1 `setHeight` stamps with
the centres at least 3 cells apart (no overlap at all).

Example — a smooth "pyramidal" slope without a single stamp (the height is
a function of position; the original height is available as `h` too):

```js
var p = getCurrentPos();
applyHeightFn(p, 10, function(x, y, h) {
    return 30 - Math.abs(x - p.x) - Math.abs(y - p.y);
}, false);
// h + 5 — raise the whole area by exactly 5, wherever it started
```

Example — a stepped pedestal around the cursor:

```js
var p = getCurrentPos();
var r = getCurrentRadius();
for (var i = 3; i >= 1; i--) {
    setHeight({x: p.x, y: p.y}, getCurrentHeight() + i * 8,
              Math.max(1, r - i * 3), false);
}
setToZP({x: p.x, y: p.y}, Math.max(1, r - 12), false);
```

### Sculpting (Smooth / Flatten / Tricubic / Inflate / Deflate / Gaussian Blur)

A separate **Sculpting** toolbar (its own row below the brush toolbar — a
toolbar break is placed before it): the sculptor brushes, **Dam mode only**
(selecting one switches the mode automatically; the Geo button is
disabled). Between the buttons sits the shared **effect fraction spinbox
(0..1)**: what part of the way to the target one stamp moves each cell.
1.0 reaches the target in one stamp; 0.2–0.5 gives a soft layer-by-layer
smoothing while dragging. The Gaussian Blur brush has its **blur radius
spinbox** next to it (strictly less than the stamp radius).

- **Smooth** — averages the surface height under the stamp and moves every
  covered cell by the given fraction towards the average. Slopes "flow"
  towards the middle of the footprint — use Flatten to preserve large
  shapes.
- **Flatten** — builds a **least-squares plane** through the heights under
  the stamp (z = a·dx + b·dy + c) and moves the cells towards that plane. A
  large slope survives (the plane follows it) while local bumps and dents
  disappear. Also works as "align to a slope": drag along the desired
  gradient.
- **Tricubic** — block-averages the heights under the stamp into a **coarse
  lattice** (stride 4 cells) and moves the cells towards the **bicubic
  interpolation** (Catmull-Rom) of that lattice. Features smaller than the
  lattice stride are erased, while large shapes — including curved ones (the
  cubic reproduces lines and parabolas exactly) — stay smooth; the stamp
  edges are not distorted (the lattice is extrapolated linearly). A middle
  ground between Smooth and Flatten.
- **Inflate / Deflate** — displace the surface **along its voxel normals**
  (central differences of the height gradient). Inflate moves every cell
  along the outward normal — bumps grow "up and out"; Deflate along the
  inward one (down and in). The displaced points rasterize back into the
  height field: where parts of the surface meet in one column, the HIGHEST
  height wins when inflating and the LOWEST when deflating (a steep slope
  "throws" its height onto the neighbouring columns). The full displacement
  step is 4 voxels at fraction 1.0.
- **Gaussian Blur** — a convolution of the heights under the stamp with a
  **normalized isotropic Gaussian kernel** (the kernel radius is the
  spinbox next to it, strictly less than the stamp radius; sigma =
  radius/2). Each cell's target is the weighted average of the PRE-stamp
  heights in the kernel window, sampling the real neighbouring cells (not
  the stamp's outer edge). Planes and slopes are reproduced exactly (the
  weights sum to 1), small noise dies, large shapes remain; the effect
  fraction is the shared spinbox (0..1).

### 3D brushes (Sculpting toolbar, Dam mode only)

Six solid-shape buttons: **Cone**, **Pyramid**, **Truncated Cone**,
**Truncated Pyramid**, **Half Torus** (the upper half of a torus — a ring
crest), **Half Ellipsoid** (half of an ellipsoid — a dome). The base is a
circle or a regular polygon of the brush radius; the preview in both views
draws exactly the base (for the torus — with the inner ring).

Every covered cell moves towards the target **Height + profile·Shape
Height**, where profile ∈ [0;1] is the solid's cross-section in that column
(1 at the apex, 0 at the base edge), with the result rounded to the nearest
1/32-voxel fixed-point step Terra supports:

- with **Raise / Lower Terrain** — cells approach the target from below /
  above only (a reached height is not touched; a repeated stamp over a
  finished shape is a no-op without an Undo entry);
- with **Set-to-Height** — **Height** is the base height, movement in both
  directions;
- **Shape Height = 0** + Lower / Set — erases the shape down to the base
  (the dam is removed where the relief reaches it).

Settings (spinboxes on the Sculpting toolbar; active only for the shapes
that use them):

- **Shape Height** — the shape amplitude (voxels);
- **Sides / Polygon Angle** — the base side count and the first-vertex
  angle (the shared spinboxes of the 2D polygon; pyramids);
- **Upper part X/Y** — the apex / upper base offset (an oblique axis; cone,
  pyramid, both truncated solids) — clamped strictly inside the base;
- **Upper part radius** — the top radius of the truncated cone / the torus
  INNER radius — less than the base radius (clamped);
- **Upper part side** — the top edge length of the truncated pyramid (the
  upper polygon is similar to the base) — clamped inside the base.

They work with the Apply Above / Below Height modifiers (the decision uses
the pre-stamp height) and refuse in Geo mode. One shape — one stamp — one
Undo entry (with the 500 ms window — one transaction).

Both brushes work in two passes: the target is computed from the state
BEFORE the stamp, then the cells move — so the result does not depend on
the traversal order. The heights are edited by the game's own mechanism
(Terra's alignment toolzer), the same as the Level to Height brush: the dam
is seeded where needed, never pushed below the geo layer, and the
zeroplast marks of moved cells are cleared. One stamp — one Undo entry
(stamps closer than 500 ms merge into one transaction).

### The Model brush (Model toolbar, Dam only)
A separate **Model** toolbar (its own row): **Import Model...** — imports a
PLY (ASCII or binary), **Use Model Brush** — enables the brush (after an
import with a world loaded), the **Place / Add / Subtract** mode buttons
and the **Height** (0–8191 voxels) and **X/Y scale** (0–1) spinboxes. The
preview is the placement rectangle in both views.

The import takes the model's vertex XY bounding rectangle and treats the
model as a solid: for each grid cell (up to 512 on the long side) it keeps
the TOP OF THE MODEL ABOVE THAT COLUMN — faces (including polygons, which
are triangulated) are sampled at the cell centre, the highest surface wins
with its (interpolated) color. Holes remain holes only where there is no
surface above the column at all. The maximum vertex height (the model's
z-range) is proportionally stretched onto 0..Height. The color comes from
the model's red/green/blue, or from the texture named in the `comment`
string of the header (`comment ... texture model_tex.tga`, a 24-bit TGA
next to the .ply), sampled by UV. The colors are exact 24-bit RGB — no
palette quantization, same as Image Decal. A model WITHOUT a texture and
without vertex colors is painted WHITE.
Import errors are shown as
`Unable to import from PLY the model <name>` (with a description).

Modes (the decision about painting/movement uses the pre-stamp state):

- **Place** — set to the model heights (like Set-to-Height, both
  directions) + paint the exact 24-bit colors on the cells with a dam; a
  cell whose model height is below the relief stays on geo without a dam;
- **Add** — the covered average height PLUS the local model height (cells
  move up only);
- **Subtract** — the covered average MINUS the local model height (down
  only; going below the relief removes the dam).

Works with Apply Above / Below Height; refuses in Geo.

### Difference mode (.gmp)

The `.gmp` is exactly how a battle map changes the relief: in Skirmish,
Survival and the scenarios the terrain differs from the base world while
the world itself stays untouched — the differences sit next to the `.spg`
in a `.gmp` and are applied by the game when loading the map.

The **Difference Mode** toggle (the View toolbar and the View menu, `F2`):

- **ON** — the terrain is shown as the world WITH the differences (what the
  game shows with a `.gmp` next to the battle map), and EVERY edit by a
  brush/import is written to the difference data, not to the world;
- **OFF** — the plain world; edits go to the world itself. The difference
  model is kept meanwhile and waits for the next ON.

Toggling changes the displayed terrain entirely and **resets the undo
history**. The differences are saved/loaded via **File → Save Difference
(.gmp)...** / **Load Difference (.gmp)...** — the file format is exactly
the game's (`vrtMap::loadGameMap`, as in `Universe.cpp`); by default the
`.gmp` sibling of the loaded battle map is used (and written). It can be
loaded in either toggle state — with the mode OFF the differences apply
after the next ON.

The preview pictures are **always from the base world**: `map.tga`/
`cache.tga` (on world save and autofix) and Export Top View do not include
the difference data — the game's map selection screen shows the world's
`map.tga` and does not apply a `.gmp`.

On exit the difference-mode edits do NOT prompt to save the world — the
world stays clean; the editor asks whether to save the difference data to
the `.gmp` (Save / Discard / Cancel).

**File → Save** always writes the BASE terrain of the world — the
differences are NEVER merged into it (they live only in the `.gmp`).
**Save/Load Difference (.gmp)** works in either toggle state: you can load
and edit the difference data while the mode is OFF (they apply on the next
ON).

Caution: **File → Save** while the mode is ON will save the output.vmp
WITH the differences merged — the editor asks for confirmation. To keep the
world clean, keep the differences in the `.gmp`.

### Height modifiers (Apply Above / Below Height)

A separate **Brush Modifiers** toolbar (its own row — a toolbar break
before it, like Sculpting) plus the **Edit → Brush Modifiers** menu (the
same actions + the threshold). Two mutually exclusive conditional buttons
set a **per-voxel** condition on top of any brush type (raise/dig, align to
height/zeroplast, color, soot, very light dam, sculpting, the scripted
brush — its operations inherit the params and pass the gate too):

- **Apply Above Height** — the effect applies to a voxel only if the dam
  height of THAT voxel is strictly above the threshold;
- **Apply Below Height** — the same, but strictly below the threshold.

The threshold is the **Gate Height** spinbox (0..8191 voxels) next to the
buttons (and in the menu); active while at least one modifier is on. Rules:

- the check is per **individual voxels**, not the stamp area: one stamp can
  raise some cells and skip their neighbours;
- the comparison is **strict**: a voxel exactly at the threshold is edited
  by neither condition;
- **Dam mode only**: enabling a modifier switches the layer to Dam itself;
  in Geo mode the condition is ignored;
- a stamp where **all** voxels fail the condition is rejected and does NOT
  create an Undo entry;
- for sculpting each voxel's decision uses the PRE-stamp height (two-pass
  semantics), while the target is computed over all covered cells.

### Parameters

- **Radius** — the stamp half-size in cells (0–512; a stamp up to 1025×1025
  cells). The limit is an editor choice; the game's radial tables in Terra
  do not rely on it.
- **Strength** — the Raise/Lower step per stamp. It has no effect on Level
  to Zeroplast: that brush reaches the zeroplast height in one stamp.
- **Height** — the target height of the Level to Height brush (0..8191
  voxels); the brush sets it absolutely in one stamp, strength does not
  affect it.
- **Sculpt effect** (0..1) — the fraction of the way to the target per
  stamp for the **Smooth / Flatten / Tricubic / Inflate / Deflate /
  Gaussian Blur** brushes that live on the **separate Sculpting toolbar**
  (its own row below the brush toolbar):
  - **Smooth** (Dam only): averages the surface height under the stamp and
    moves every cell by this fraction towards the average — bumps and dents
    smooth out, the overall level is preserved;
  - **Flatten** (Dam only): builds a least-squares plane through the
    heights under the stamp and moves the cells towards the plane — large
    slopes survive, local irregularities disappear.
  The heights are edited by the same Terra toolzer as the Level to Height
  brush.
- **Blur radius** (1..stamp radius − 1) — the Gaussian kernel radius of the
  **Gaussian Blur** brush; available only when that brush is selected. The
  spinbox upper bound follows the stamp radius and shrinks with it.
- **Gate Height** (0..8191 voxels) — the threshold of the **Apply Above /
  Below Height** modifiers from the **Brush Modifiers** toolbar (and Edit →
  Brush Modifiers); active while at least one of them is on. The brush acts
  only on voxels strictly above / strictly below the threshold.
- **Undo/Redo**: `Ctrl+Z` / `Ctrl+R` (or `Ctrl+Y`). Edits that follow one
  another with an interval below **500 ms** merge into **one history
  transaction** — one `Ctrl+Z` reverts the whole series at once (e.g. an
  entire brush stroke of dozens of stamps). A pause longer than 500 ms
  starts a new transaction. An image import onto the map is one step.

## Palettes and colors

The **Palette** panel: two grids of 256 colors — Geo (in `inGeo.act` +
`geoPal.xml`) and Dam (in `inDam.act`). Click — select the brush color,
double click — edit the RGB (changes apply immediately and are saved with
the world). The eyedropper (Pick Color) takes the index from the palette of
the cell's layer.

## Viewing and navigation

- **2D (TopDown)**: wheel — zoom (1:1…whole map), drag — pan.
- **3D (Oblique)**: the toolbar button or the View menu.
  Rotation/tilt/lift/focus — the toolbar buttons or the mouse (Alt+left —
  orbit, middle/right — pan, wheel — focus). The rotation/tilt keys are on
  the buttons. The absolute camera rotation is set by the **degree
  spinbox** next to the rotation buttons (0–360°, wrapping 360°→0°): the
  spinbox always shows the current azimuth (including the mouse orbit and
  the Camera Like in Game mode — there it is the game camera's psi), and
  editing it rotates the camera right away.
- **Camera Like in Game** (View menu) — switch the 3D view to the game's
  camera: the psi/theta/distance model with the game FOV
  (HardwareCameraFocus), aimed at the first player's camera of the loaded
  world (the "Camera0" spline from the world's `.spg`; for FORTOP — a tilted
  view onto the first player's spawn, for worlds without cameras — the
  game's default top view). The minimap's yellow frame follows the game
  frustum. Controls: wheel — distance (the game's zoom), Alt+left —
  psi/theta, middle/right — pan ("drag the relief").
- **Camera Like in Editor** (View menu) — back to the editor's 3D camera.
- **Chunky / Smooth** — the pixelated look or the smoothed "like in the
  game" one.
- **Grid** — a grid overlay; the step is set by the W/H spinboxes (in
  cells).
- **Dim Geo** — dimming of the SUNKEN relief (cells whose surface height is
  0 — the game engine sinks them to the -15 depth, hence the "grey floor").
  Raised geo without a dam is never dimmed.
- **Freeze Dam** — the scrolling background renders the geo layer only, the
  dam stays in place (handy for finding flat spots).
- **Minimap** — the minimap panel; a click moves the view, the frame shows
  the visible area. The picture updates immediately on terrain edits
  (brush, undo/redo) without saving the world.
- A click in the 3D view paints the cell under the cursor (the brush works
  in 3D too).

## World parameters

**File → World Parameters...**:

- `hZeroPlast` — the zeroplast height (applies immediately);
- the generation powers (GeoNet/Section/MinimalSquare) — written to
  `world.ini`, applied on world reload or relief generation;
- the **output.vmp format** — S2T0 (uncompressed) or S2T1 (zlib); the
  conversion happens right away and is supported on every save;
- the geo layer's 3D noise scale (`ScalingXYZ`, the function);
- the **zeroplast color** — stored in `Perimeter.ini` and shared by all
  maps of this game installation.

## Images

The **File → Images** menu:

- **Import Image onto Map...** — paint the surface with a picture (RGB →
  the nearest color of the cell's layer palette; an indexed TGA → indices
  as-is). Filters: paint geo, paint dam. One Undo step.
- **Import up.tga** — the world's 512×512 cover image (not used by the
  game).
- **Import leveledSurfaceTexture.tga** — the texture of the leveled zones
  (a palette TGA, a power of two, ≤ the map size).
- **Export Heightmap / Export Top View** — export the heights (8-bit
  grayscale TGA) and the top view (24-bit TGA).

## Validation

The **Tools** menu:

- **Validate World...** — the world's files, sizes, `hZeroPlast`, palettes,
  textures, `hardness.bin`; the volumetric soil balance relative to the
  zeroplast (an imbalance warns about a future soil import/export);
- **Validate Battle Map...** — the world's existence, spawns within bounds,
  unique player names, cameras;
- **Autofix** — normalize `playersShufflingIndices`, set `zeroLayerHeight`
  from the world, regenerate `cache.tga`/`map.tga`.

## Hotkeys

| Key | Action |
|---|---|
| `Ctrl+O` | load a world |
| `Ctrl+S` | save the world |
| `Ctrl+Shift+S` | save the world as |
| `Ctrl+Z` / `Ctrl+R`, `Ctrl+Y` | undo / redo |
| `F5` | rescan the world list |
| `Ctrl+W` | unload the world |
| `P` | show/hide the palette |
| `B` | show/hide the battle map panel |
| `S` | the Place Spawn mode (move a player's spawn by click) |
| `F1` | this help |

## Good to know

- All world edits are applied in memory; until **Save** is pressed the
  files on disk do not change (except the parameters marked as applying
  immediately).
- Changing the generation powers of a loaded world takes effect only after
  its reload.
- The `.gmp`/`.dat` files next to a `.spg`: `.gmp` — the terrain difference
  data, edited by the Difference Mode (see above); the editor never touches
  `.dat`.
- After saving a world it is useful to regenerate/check `cache.tga` — the
  editor does this automatically on every save.
- Always check a new map in the game itself (the Tools validator catches
  only structural problems).

## The MCP server (driving the editor from LLM agents)

The editor embeds an **MCP server** — through it an LLM agent (Claude,
Cursor, etc.) performs the same actions as the UI: opens worlds and maps,
edits the relief with brushes, places objects, works with palettes and
undo.

### Starting and default settings

- The server **starts automatically** with the editor — nothing else to
  launch. If the port is busy the server simply disables itself (a trace
  log entry); the editor is unaffected.
- Protocol: **HTTP + JSON-RPC 2.0**, the `POST /mcp` endpoint (the root `/`
  is accepted too), responses `application/json`; `GET /` — the server's
  info page.
- The default port: **10102**.

### The settings file and the port

You can place a `map_editor.ini` next to the editor executable
(`map_editor.exe`):

```ini
[mcp]
port=12345
```

`port` — any free port (0 or a missing/incorrect key → 10102 again).
Restart the editor after editing the ini. The server stops when the editor
exits; starting again on the same port is free of "port busy" errors.

### Connecting an agent

The server is stateless and compatible with the usual MCP clients
(streamable HTTP). A client configuration example:

```json
{
  "mcpServers": {
    "perimeter-map-editor": {
      "url": "http://127.0.0.1:10102/mcp"
    }
  }
}
```

By hand it looks like plain JSON-RPC:

```json
{"jsonrpc":"2.0","id":1,"method":"tools/list"}
{"jsonrpc":"2.0","id":2,"method":"tools/call",
 "params":{"name":"get_world_properties","arguments":{}}}
```

The agent's workflow: `set_game_path` → `list_worlds` → `open_world` /
`open_battle_map` → edits (`apply_brush`, the object tools) →
`save_world` / `save_battle_map`. As in the UI, edits go to the undo
history, and actions with an interval below **500 ms** merge into one
transaction (the `undo`/`redo` tools).

### The tool list

**World and session:** `set_game_path` (the game folder path),
`list_worlds`, `get_world_properties`, `create_world`, `open_world`,
`save_world`.

**Battle maps:** `list_battle_maps`, `open_battle_map`, `save_battle_map`,
`new_battle_map`.

**Map players:** `list_players`, `add_player`, `update_player` (properties
and the spawn coordinates), `remove_player`.

**Map objects:** `list_battle_map_objects` (buildings, scum nests, nature),
`place_building` / `update_building` / `delete_building`,
`place_scum_spot` / `update_scum_spot` / `delete_scum_spot`,
`place_nature_object` / `update_nature_object` / `delete_nature_object`.

**Brushes:** `apply_brush` — all effects (raise/lower/set height,
zeroplast, soot, very light dam, paintColor, the smooth/flatten/tricubic/
inflate/gaussianBlur sculpting), 3D solids (`shape`: cone, pyramid,
truncatedCone, truncatedPyramid, torus, halfEllipsoid), ImageDecal
(`load_decal_image` + `effect: "imageDecal"`), the image mask
(`load_brush_image` — the alpha channel defines the stamp shape: fully
transparent pixels do not stamp at all; the scale is the `maskScale`
argument of apply_brush, 10–800 %, like the Mask spin on the toolbar), the
model brush (`import_model` + `effect: "model"`), the stamp parameters
(radius, square, sides/angleOffset, fraction, blurRadius, layer dam/geo).
In the difference mode the stamp writes to the diff buffer.

**Scripts:** `run_script` (JavaScript in one undo step, the same API as the
Scripted Brush / Run Script), `set_scripted_brush_script`.

**Difference mode (.gmp):** `set_difference_mode`, `save_difference`,
`load_difference`.

**Terrain and palettes:** `get_terrain_at` (the Geo/Dam heights, the
surface index and colors, the cell attributes), `get_palette`,
`set_palette_color`.

**History:** `undo`, `redo`.

Errors arrive as `isError` with a text; notifications (without an `id`)
receive HTTP 202. The full description of every tool — with the JSON Schema
of the arguments — the agent gets via `tools/list`.
