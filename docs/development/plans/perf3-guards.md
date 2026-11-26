# Round 3: guards vanish in combat (lead guards)

User report: in guest mode, in combat with Whiterun guards, every guard vanished at once while the blood on them still drew.

## Cause (confirmed)

The fade root test (FadeStateCS `main` and `PreparePortalQueries`, FadeCpuStage `StepRoot`) took a root's centre from
`placements[root.Object]` row 8 and its radius from row 7 `.w`, else the seed's `root.Radius`. `root.Object` is one member slot.
For a guard it could be a head part under the helmet ('MaleMouthHumanoidDefault'). That part is hidden, so it is no placed
mover, and its placement row was never written: all zeros. The root was tested as a zero-radius point at the world origin, so
every member was culled (census fate "frustum"). Decals skip the root test, so the blood stayed. Wilmuth (no helmet) had the
same centre member but drew, because his mouth is drawn and sampled. The radius was a second fault: row 7 is the sun entry's
listed root, and actors have none (FLT_MAX), so the test fell back to the seed's radius, read once at listing.

## Fix

A root is now tested with its own node's world bound, sampled every frame, as BSFadeNode::OnVisible tests it.

- New frame-value pool `fade-root-bounds` (`Features/Fades/FadeRootBounds.cpp`). The producer reads every listed root node's
  `worldBound` (centre, radius) under engine-read leases, in 512-root chunks, and stamps the row with the root's generation.
  The kick holds the accepted tables and the installed publication's retirement hold, which keeps every listed node alive.
- Row `FadeRootBound` (32 bytes, `Records.h`, mirrored in FadeStateCS). A row counts only when its generation is the root
  row's. Until then the root is not stepped, the same rule as a root waiting for its seed.
- GPU: cull binding `RootBounds`; its index rides in the depth latch at byte 344 (the old `spare` word, now
  `fadeRootBoundsIndex`), filled in `FillCullLatch` like the portal programs. FadeStateCS reads it in both entry points. The
  placement-row reads and FadeStateCS's `PlacementRows` struct are gone.
- CPU: FadeCpuStage reads `ProducerRootBounds()` (the pool samples before the CPU stage's pool). Native uses the same path.
- `FadeRootStatic.object` now only marks the row listed and names a member for the census. `radius` stays for `lodScale`.
- Census (`SetParity.cpp`, CS_DCLF_PARITY=1): the actor line and the "rejected in view" follow-up now print the root bound the
  stage tested, its generation, and its offset from the live node's bound. CullFrame carries `rootBounds` for this.

## Second issue: "a missed detach" in rootrows3

Not in the known flaky list. It did not reproduce in my two parity runs (about 3,900 frames at the river save, combat). The
error carried only a pointer, so `ValidateSlice` now names the geometry, its reference, the category node it was tracked
under (and whether that is still a category node), the live one, and the live parent chain. The next occurrence names the
engine change that had no event. Open.

## Runs (all with `tgm` first)

| Config | Label | Result |
|---|---|---|
| guest+dclf-parity | p3-guards-parity1 | passed; 0 VANISHED; every guard's root bound matches the live node (centre off 0, radius off 0); guards fight with weapons drawn in shots |
| guest+quick | p3-guards-quick | passed |
| native-vulkan+quick | p3-guards-nvquick | passed, two shots, guard drawn |
| guest-interior (Dragonsreach shots only) | p3-guards-interior | passed |
| guest+dclf-parity, 2,400 frames, `stopcombatalarm` | p3-guards-parity2 | failed on one `sun on the GPU ... <- DIFFERS` at the first sample after load (7 of 2,612 draws); later samples OK. Pre-existing: the director's repro run 1125-231405 has it too. 0 VANISHED, no missed detach |

## Measurement

No Tracy capture: the render thread gains only the bounds kick (two shared_ptr copies). The sampling runs on DCLF's pool.
Decided by the agent, to confirm: a capture was not worth a lab slot for this.

## To confirm / open

- Decided by the agent, to confirm: every listed root is sampled every frame (about 2,700 roots at the river save), rather
  than only moving ones. It is simple, keeps static roots exact, and runs off the render thread.
- Decided by the agent, to confirm: a root without a bound of its generation waits a frame, like a root without a seed.
- Native D3D12 not run (ORG and BasicRHI untouched). SE untested at runtime (no new addresses).
- The missed-detach check above stays open until it reproduces with the new message.
