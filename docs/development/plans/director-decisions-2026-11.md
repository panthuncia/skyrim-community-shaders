# Decisions to confirm (2026-11-19 onward)

Decisions taken while the user was away, during the fix round, the port completion and the two plans
(`native-frame-generation-plan.md`, then `frame-revision-plan.md`). Each says what was decided and why. Nothing here is
final until the user confirms it.

## Fix round and ports

1. **Physical Sky phase 4 (volumetric clouds) gets its own lead agent.** The phase is about 12,000 shader lines, 45 programs
   and about 60 resources, too large for the agent that did phases 2, 5 and 6. Until it lands, guest runs the AP shadow, the
   fog scattering and the sky tail on D3D11 on frames with clouds on, and native has no clouds or lightning.
2. **Physical Sky open questions 9-15** (`physical-sky-port.md`, section 8) were decided by the port agent and are marked
   "to confirm". The ones that most need a look: 9 (depth without Dynamic Cubemaps), 10 (when the AP composite runs), 13 (D3D11
   inputs in guest mode), 14 (the sky tail waiting on ViewAtmosphere).
3. **cs-test configs with a setting change one frame after a shot.** On the graph a run's first shot needs a rebuild to
   add its copy, so a change one frame later lands first. `lens.toml` was fixed (60 frames). `quality-burst.toml`,
   `resize.toml` and `upscale-burst.toml` have the same pattern and were left alone, since they are burst tests. A
   longer-term fix: cs-test's `previous` anchor after a `shot` waits for the shot's read back.
4. **The PublishedResources off-thread read** (DCLF's `DeclarePublishedRead` during an async build) is recorded in
   `dclf-open-defects.md` and left for later. It needs the build snapshot to carry each entry's state and view.
