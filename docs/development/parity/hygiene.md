# Parity wave: hygiene (WS0)

Lane `par-hygiene`, branch `org-native/par-hygiene`, based on `f34fce25d`.

## Removed

- `CS_ORG_SKY_CLOUDS`: dropped from native mode's port switches, the switch registry, the cs-test schema, and the nine cs-ab
  scenarios that set it to 0. Nothing read it: the cloud layers are always on since 91b722642.
- `CS_ORG_SCALEFORM_D3D11_PARTS`: dropped from native mode's conflicts and the registry. Its path went in 5.5.8.
- `CS_ORG_BINDLESS`: dropped from the registry. It is a shader define (`ProgramBuild.cpp`), not an environment switch.
- `CS_ORG_LLF_SHADOWS`: the alias read is gone. Only `CS_ORG_LIGHT_LIMIT_FIX_SHADOWS` is read. No scenario used the old name.
- `Backend::UnsupportedReason`: no callers.

## Changed

- `CS_ORG_DEFERRED_CLEARS` is registered.
- The epoch and submission switches (`CS_ORG_EPOCHS`, `ASYNC_EPOCHS`, `CLOSED`, `EXTERNAL_HOME`, `BATCH_SUBMIT`, `EARLY_FLUSH`)
  moved from DCLF's registry to `src/Testing/SwitchRegistry.cpp` (`kDescribed`, with their help text). They are now checked in
  builds without DCLF. DCLF's fade switch was anchored before `CS_ORG_EPOCHS`; it is now anchored before
  `CS_DCLF_PERSISTENT_PARITY`, the same place in the list.
- Side effect: DCLF's startup lines (`[DCLF] switches:` and the narrowed-featureset line) and its diagnostic capture no longer list
  these six switches. The switch dump (`CS_TEST_LIST_SWITCHES`) lists them as family `org`, kind `other`.
- Native.cpp reads `CS_ORG_NATIVE_BACKEND` through `Switches` (GraphicsAPI sets it before `ApplySwitches` runs).
- GraphicsAPI keeps its raw reads of `CS_ORG_NATIVE` and `CS_ORG_NATIVE_BACKEND`. It sets both after reading them, and a cached
  `Switches` read would hide the new values from `Native::Requested`. Its comment says so.
- `tools/cs-test/gen_schema.py` now holds the rows that had been written into `schema.toml` by hand (`NATIVE_OWNED`,
  `NATIVE_EXCLUSIVE_IMPORTS`, `NATIVE_HOME_STATES`, `NATIVE_IN_STREAM`, `IMAGE_DUPLICATE_CHECK`, `RAW_ACCESS_CHECK`), plus the six
  epoch switches and `DEFERRED_CLEARS`. The schema was regenerated. Before that, a regeneration would have dropped them.

## Native guard

`Native::ApplySwitches` now stops the start, with the usual "cannot run" message, when:
- `CS_ORG_WATER_PARTS` does not name both `mask` and `surface`;
- `CS_ORG_SHADOW_MASK_VIEWS` does not name both `world` and `menu`;
- `CS_ORG_IMAGESPACE_EFFECTS` is set to anything but `all` (any case);
- `CS_DISABLE_FEATURES` names `DrawcallLimitFix`, or `CS_DCLF=0` (environment or switches file).

Unset is fine for all of them. DCLF's `CS_DCLF_*` switches are not checked. No native scenario or cs-test config sets any of these.

How to test: a native run with `CS_ORG_WATER_PARTS=mask` must stop at stage 1 with
`[ORG native] ... CS_ORG_WATER_PARTS=mask still needs DXVK (it leaves the water views it does not name to the engine)`. A normal
native run must start as before.

## Doc drift fixed

render-graph.md (WrapTexture/WrapBuffer, menu scenes, water displacement, native startup conflicts, five missing switch rows, the
LLF alias), native-d3d12.env's header, p5/bink.md, p5/water.md, p5/sky.md, org-full-port.md (cloud layers, ConfigureSwapchain),
the `CS_DCLF_CPU_RECORD` default (DCLF's registry, dclf-architecture.md, dclf-validation.md), dclf-validation.md's epoch rows.

## Left for later (outside this lane)

- `src/RenderGraph/Engine/ImageSpace/ImageSpace.h:20` is on WS0's drift list; the imagespace lane owns that file.
- `CS_ORG_EPOCHS=0` also keeps DCLF from installing, so in native mode it drops the world's draws like `CS_DCLF=0`. It is not a
  native conflict yet: check whether native runs without epochs at all before adding it.
- Native.cpp's `kPortSwitches` and `kD3D12DclfSwitches` loops read the environment raw on purpose: they set variables before
  anything caches them.
- p5/flush.md, p5/primary-path.md and org-full-port.md still name `CS_ORG_SKY_CLOUDS` and `CS_ORG_SCALEFORM_D3D11_PARTS` in
  dated census tables and task rows. They are history, so they were left as they are.
- `docs/development/render-graph.md` still has no rows for several registered switches outside the Phase 4 set. The registry
  (`CS_TEST_LIST_SWITCHES`) is the full list.
