# snap5-book: books in native mode, the book's 3D scene (roadmap phase 5, "Menu scenes")

Lead `book`, branch `snap5/book` (base `org_integration` 8d5f6471b), stage `s7-book`. Starts from
[snap5-menus5.md](snap5-menus5.md) (its book findings), decisions 146-150 and 189-191.

## State

| Item | State |
|---|---|
| 1. The book crash (decision 191) | done: books open and read in native Vulkan and guest; shots match guest and the engine-drawn native |
| 2. The book's 3D scene (scheme 3) | done: observed, owned (CS computes every draw) |
| 3. The local map | not done (see open items) |
| 4. Delete `CS_MENU_SCENES_OBSERVE` | not done: left for the local map (and scheme 5, never seen) |

## How a book renders (RE, AE / SE)

- `BookMenu::PreDisplay` (AE 51052, SE 50121) runs for each page target i below `numRenderTargets` (+0x78): RenderEffect's
  texture form (AE 105680, SE 99029) with effect 0x22 (ISCopy) copies the page texture (+0x80, the page geometry's base texture) into
  kBOOK_TEXT_0 + i; `ShowPageAtOffset(i)`; `SetRenderTarget(0, kBOOK_TEXT_0 + i)`, `SetCurrentDepthStencilTarget(kBOOK_TEXT, 0,
  slice i)`, ApplyState; then the book movie's Display draws the page's text into that target.
- **PreDisplay is not called by DrawInterface.** Main::DrawFrame calls AE 82085 / SE 79948 (DrawFrame +0x1CD, before the world),
  which runs PreDisplay for every menu with flag 0x1000 (kRendersOffscreenTargets): BookMenu and MapMenu (MapMenu's PreDisplay jumps
  to its local map's, `this + 0x60`). That is why the copy came while no CS stage was open.
- `BookMenu::PostDisplay` (AE 51051) displays the menu's own movie, then tail-jumps to `UI3DSceneManager::Render(3, null, 0)`.
- The book's init (AE 0x1408f1ed0) sets each page geometry's `diffuseRenderTargetSourceIndex` (material +0x50) to kBOOK_TEXT_0 + i, so
  the page Lighting draws sample the targets through SetupMaterial's render-target path (DCLF's material port already reads it).
- The book model's shapes are skinned and have **no renderer data of their own**: their buffers are their skin partition's `buffData`
  (one partition each).

## What was built

- **The page copy as a UI-stage ImageSpace step** (`MenuSteps.inl`): a fourth client of the menu composites,
  `cs.engine.imagespace.book` (`PostPart::BookPages`), one entry per kBOOK_TEXT target, keyed `InOut(kLoadedSource = 0x7e, target)`.
  A detour on RenderEffect's texture form (`RenderTextureEffect`, ImageSpace.cpp) marks the call like RenderEffect's. t0 is the
  loaded page texture read by latched heap index: a new program permutation `kLoadedSourcePermutation` builds ISCopy with
  `ORG_LOADED_SOURCE` (`package/Shaders/ISCopy.hlsl`); `LatchedSlots(effect, permutation)` replaces the per-effect latched mask where
  programs and passes are made; `StageLoadedSources` (was `StageTintMasks`) stages any latched slots. The recipe puts the client in
  the UI stage's first ImageSpace step, beside the frozen composite, before the displays (the stage has its 21 steps already).
  `SlotTexture` now checks a loaded texture before the depth index (the texture form leaves +0xC at 0).
- **BookMenu::PreDisplay moves into the UI stage** (Scaleform `Renderer.cpp`, `BookPreDisplay`, a detour on AE 51052 / SE 50121): in
  graph mode (native and guest) the call from DrawFrame's offscreen loop records that it is due, and DrawInterface's detour runs it
  first, under the movie lock the loop takes (AE 82732/82733, SE 80605/80606), then puts back the render target and depth it changed
  (DrawInterface keeps a bound target). So the page copies, the pages' displays (into kBOOK_TEXT_n, the surfaces that existed already)
  and the book's scene are all inside the UI stage, in the engine's order. Nothing in the world reads the page targets, so no image
  changes. DrawInterface's detour is now installed in guest too (it only runs the due PreDisplay there). The map menu's offscreen work
  is not moved: a first version moved the whole loop (AE 82085), then the world map crashed; that crash is pre-existing (below), but
  the narrower hook keeps the local map where it was.
- **The menu colour client reads kBOOK_TEXT_0..3** (`ListInputs`).
- **Scheme 3 is owned** (`Owns`). The native menu scene draws a skinned shape with no renderer data from its one drawn skin partition
  (row 3, DCLF's world rule); a shape that draws several partitions, or whose palette update makes no matrices (a first-person hand on
  a loading model), stays left out as before (counted, logged once).
- **Diagnostics** (log only): each scheme's first native render with draws; a warning when a scheme drew nothing in 300 renders, with the
  last left-out reason; a palette failure names the matrix count.
- **Test harness**: `page next|prev` command (BookMenu's NextPage / PrevPage user event); `book` picks a real two-page book by default
  (a `Clutter\Books` model named for a book; notes and recipes keep theirs there too); config `menu-book` (tgm, health restored so
  no help message covers the book, open, shot, turn a page, shot). Pass it by path. The book site
  (`RenderEffect's texture form (in BookMenu::PreDisplay)`) is in the engine zones table; PostDisplay's scene call is a jump, not a site.

## Observe (native Vulkan, `s7-book-obs2-book`, The Nirnroot Missive)

Every book draw computed. Differences, explained:
- **Point light order** (every book draw, 2 lights): the engine's pass lists the two menu lights in the opposite order to the scene
  node's active list. The shader sums point lights, so only the float order of a sum changes. Decided by the agent, to confirm.
- **World rows** of the pages: skinned draws; the engine leaves stale World rows a skinned pass does not read (as menus5 saw).
- The frame rows' 1 ulp (decision 148); the loading model's permutation descriptors in observe mode (no CS shadow maps there).

## Detector (`[EngineZones]`, native Vulkan; Dev-Fast)

Book frames are world frames; baseline world frames read 9.0.

| Run | World frames | Book frames |
|---|---|---|
| before (`s7-book-n1-book`, base code) | - | fatal at the first book frame (tap error) |
| copy step only, scheme 3 engine-drawn (`-n2-book`, a one-page note) | 20.9 per frame (659 frames, 461 with the book) | max 26: 9 + the copy + 16 of UI3DSceneManager::Render's accumulator sites |
| scheme 3 owned (`-sw7-book`, four page targets) | 11.8 per frame (733 frames) | max 13: 9 + the four page copies (RenderEffect's texture form) |

The page copies remain engine calls: the ImageSpace manager's texture form runs (bookkeeping: the effect's texture list) and CS's
detour stages the copy without the engine's draw, as the other menu composites' RenderEffect calls do.

## Decided by the agent, to confirm

- **BookMenu::PreDisplay runs at DrawInterface's start in graph mode, guest included.** Guest books hit the same tap error
  without it, and guest+menu-book is a pass condition; the move draws the same pages (nothing reads them before the UI).
- **Point light order in book draws is left as the scene node's order** (a sum; see Observe).
- **A skinned menu shape without renderer data that draws several partitions stays left out** (non-fatal, logged), as before this
  change; one partition is drawn. Several-partition support needs a draw per partition.
- **The page copies stay RenderEffect texture-form calls** (counted as render, as the other menu composites' RenderEffect calls).
- **`CS_MENU_SCENES_OBSERVE` stays** for the local map and the never-seen scheme 5.

## Proposed decision-log entries

- Books work in native and guest: BookMenu's page copies are UI-stage ImageSpace steps (`cs.engine.imagespace.book`, ISCopy with a
  latched loaded source), and the offscreen menus' PreDisplay (AE 82085, called by DrawFrame before the world) runs at DrawInterface's
  start in graph mode, so the copies, the page displays into kBOOK_TEXT_n and the book scene are all in the UI stage.
- The book (scheme 3) is CS's: the menu colour client reads kBOOK_TEXT_0..3, skinned shapes without renderer data draw their one skin
  partition. Book frames: 26 -> 13 engine render calls (the four page copies and the world's 9 remain).

## Open items

1. The local map: not started. Its render runs from MapMenu::PreDisplay (jumps to `this + 0x60`), which DrawFrame's offscreen loop
   (AE 82085) calls before the world, outside the UI stage where `cs.menu-scenes.local-map` is; the book's move shows the way (a
   PreDisplay detour run at DrawInterface's start). No config opens it yet (MapMenu toggles it through its own input path, user event
   `LocalMap`); its native path (map camera, land and objects, fog of war, ISLocalMap) is open.
5. **Pre-existing, found on the way:** native-vulkan+map crashes when the world map opens (SkyrimSE+0xE4B9BB in SetDirtyStates under
   the engine's end-of-frame family with ISWorldMap; `s7-book-base-nvk-map` on the base code crashes the same way, a 17:32 run of
   another branch passed).
6. **Pre-existing:** a random loading screen model with a blood Effect draw (EdgeBlood03, pass 0x4842: `EF::Blood` is unported) is
   fatal in owned scheme 4 (`s7-book-fin-nvk-book`, at the loading screen before the book; the rerun passed).
2. Delete `CS_MENU_SCENES_OBSERVE` once the local map is switched.
3. Skinned menu shapes that draw several partitions without renderer data: a draw per partition.
4. The page copies as CS's own staging without the ImageSpace manager's texture form (a port of its texture-list bookkeeping).

## Runs (Dev-Fast)

| Config | Label | Result |
|---|---|---|
| native-vulkan+menu-book | s7-book-n1-book | hang: the page copy came outside any stage (DrawFrame's offscreen PreDisplay) |
| native-vulkan+menu-book | s7-book-n2-book | passed; a recipe note (one page) drawn, scheme 3 engine-drawn |
| native-vulkan+menu-book, observe | s7-book-obs1-book | passed; the note's pages observed |
| native-vulkan+menu-book | s7-book-sw1..sw4-book | passed but no book: owned scheme 3 left every draw out (no renderer data); diagnosed |
| native-vulkan+menu-book | s7-book-sw5-book, -sw6-book | fatal: palettes (book), then a loading model hand with no matrices; fixed |
| native-vulkan+menu-book | s7-book-sw7-book | passed; book drawn and turned by CS; ref check against guest g2 and engine-drawn obs2: pass |
| native-vulkan+menu-book, observe | s7-book-obs2-book | passed; differences above |
| guest+menu-book | s7-book-g1-book | hang: same tap error in guest |
| guest+menu-book | s7-book-g2-book | passed |

Final runs (`s7-book-fin-*`). The book runs are on the final code (cb09d5c0c); the others are on the build before the hook was narrowed
from the offscreen loop to BookMenu::PreDisplay (743e60dfb; none of them opens a book or the map).

| Config | Label | Result |
|---|---|---|
| native-vulkan+menu-book (detector) | s7-book-fin-nvk-book | fatal at the loading screen: blood Effect on a random loading model (open item 6) |
| native-vulkan+menu-book (detector) | s7-book-fin-nvk-book2, -nvk-book3 | passed; 12 engine render calls per world frame |
| guest+menu-book | s7-book-fin-guest-book | passed |
| guest+quick, native-vulkan+quick, native-d3d12-quick | s7-book-fin-guest-quick, -nvk-quick, -d3d12-quick | passed |
| native-vulkan+menu-scenes (detector), guest+menu-scenes | s7-book-fin-nvk-scenes, -guest-scenes | passed (native 9 per world frame) |
| native-vulkan+menu-main (detector) | s7-book-fin-nvk-main | passed |
| native-vulkan +menu-loading, +menu-lockpick, +menu-items, +facegen, +cadence-drop-cells | s7-book-fin-nvk-loading, -nvk-lockpick, -nvk-items, -nvk-facegen, -nvk-dropcells | passed |
| native-vulkan+map | s7-book-fin-nvk-map, -nvk-map2 | crashed at the world map: pre-existing (base run `s7-book-base-nvk-map` the same) |

Shot references to accept: `s7-book-fin-nvk-book3` (book-open, book-page; ref check against guest `-g2-book` and the engine-drawn
`-obs2-book`: pass) and `s7-book-fin-guest-book`.
