# Crash catalog

Crashes met while developing the Drawcall Limit Fix, from the crash logger's logs in
`Documents\My Games\Skyrim Special Edition\SKSE\crash-*.log`. AE 1.6.1170 addresses, with the Address Library
ID in brackets. Newest first.

## Open

### `SkyrimSE+14F79AA`: a material virtual call during a model load

| | |
| --- | --- |
| Seen | 2026-09-22 23:32, 23:52, 23:58; 2026-09-23 00:12. The variant below: 23:57 and 00:07. |
| Thread | An IO thread (`IOManager::DoOnPreRunTask` on the stack), loading a model: `BSResource::EntryDB<BSModelDB>`, `BSStream`, `BSResourceNiBinaryStream`, `CompressedArchiveStream`. |
| Fault | `call [rax+0x18]` in `FUN_1414f7850` (107719+0x21A) reads 0xFFFFFFFFFFFFFFFF: a virtual call through a vtable that is not one. |
| Objects | R14 a `BSLightingShaderMaterial`, and on the stack the `BSLightingShaderProperty` being loaded and its `NiAlphaProperty`. |
| Stack | `FUN_1414f7850` (107719) <- `FUN_141476cc0` (105544) <- `FUN_1414ac820` (106494) <- `SkyrimSE+0D21AE7` (70381, which the logger labels `NiPSysSphericalCollider::Func38`, a mislabel). |
| Variant | The same call jumps to address `0x6` ("tried to execute memory at 0x6"), returning into `SkyrimSE+14F79AD` (107719+0x21D) from `FUN_140d25d00` (70502). Same function, same vtable call, different garbage. |
| When | Loading cells: repeated `coc`, and the test harness's flights. Seen with `CS_DCLF_FADING=0` too, so it is not the fading work. |

**Attribution: unknown.** DCLF writes none of the engine's material or property memory. It keeps pointers
to materials as cache keys, rewrites only its own material records (`RefreshMaterialPatch`), and runs the
engine's material setup on the render thread (`ConstantEvaluator::EvaluateMaterial`). A material whose
vtable reads as garbage on the loading thread looks like a use after free, or an object being built while
something else reads it.

**Next steps:**

-   Repeat the `coc` sequence with DCLF off (`CS_DCLF=0`), and with Community Shaders off.
-   Decompile `1414f7850` to find which material call it is making and where the material came from.

## Fixed

### `CommunityShaders.dll`: `NativeProbe::OnNativeLightingDraw` (2026-09-22 22:36)

An access violation reading `0x1FFDD`, inside `SetupGeometry`'s hook (`Hooks.cpp:230` -> `NativeProbe.cpp`).

-   **Cause:** the native probe read an `NiSwitchNode`'s `childRevID` array through CommonLib's members.
    CommonLib declares them after `NiNode`, whose declared size in a multi-runtime build is VR's, so the
    members read the wrong memory.
-   **Fix:** `SceneStore::ReadSwitch` reads them at the SE/AE offsets (engine notes, "Switch nodes").

## Earlier, not investigated here

From the logs of 2026-09-22 before the tree and actor work:

-   **01:06 and 01:10:** a C++ exception from `Util::` in `src\Utils\D3D.cpp:237`, called from
    `GrassOptimizations.cpp`.
-   **01:15:** an engine null read in `FUN_140e4c730` (77397), from `bhkConstraintChain::Func55` on the
    pathing and navmesh job.
-   **01:59:** a null read in `org::StatisticsManager::SetupQueryHeap`, from
    `org::runtime::CreateDefaultStatisticsService` during `RenderGraph::Update`.
