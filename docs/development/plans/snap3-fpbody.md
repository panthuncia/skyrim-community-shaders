# snap3-fpbody: the third-person root after a switch to first person (snap3-pov's open item, decision 196)

Lead: `fpbody`, branch `snap3/fpbody` (base `org_integration` 629bd6299), stage `s7-fpbody`.

## Cause: the test setup, not the renderer

Native runs and guest runs did not load the same plugins. `native-vulkan.toml` turns Improved Camera SE off (it draws through
D3D11 itself and is incompatible with native mode, `org-extension-api-notes.md`); guest keeps it. Improved Camera shows the
player's third-person body in first person, so the third-person root differed between the modes.

How it does it (`ImprovedCameraSE.dll` 2.0.0.2231, from its PDB: `HOOK::FIXES::PlayerSwitchSkeleton::thunk`): a detour on
PlayerCharacter::SetFirstPerson (SE 39401 / AE 40476). On a switch to first person with its "first person body" option on, it
writes `EB 0A` at the function's +0xBC on AE (SE: 8 NOPs at +0x123) before calling the original. The jump skips the load and the
`OR ECX, 1` of the third-person hidden store, so the SWF-patched store at 0x140736237 writes the old hidden bit back. After the
call it clears the root's hidden bit itself. That is why snap3-pov's probe saw the bit clear in guest by the audio call (AE 67700).
On 1.6.1170 the skipped load also drops the root's other flag bits (the store writes the bit alone: flags 0x48ac00f in native and
in guest without the plugin, 0x0 / 0x4000 in guest with it). That is Improved Camera's, not ours.

None of decision 151's water work, the skeleton driver (192) or the skipped ClearMasks (170) changes the third-person root.

## Evidence (a temporary capture-time log of both roots' hidden bits, removed)

| Run | Third-person root in first person | Player shadow in first person |
|---|---|---|
| `s7-fpbody-a-guest-ic` (guest, Improved Camera on, as before) | shown, flags 0x0 | the plugin's body (its feet in view) |
| `s7-fpbody-a-guest-noic` (guest, Improved Camera off) | hidden, flags 0x48ac00f | none |
| `s7-fpbody-a-native` (native Vulkan) | hidden, flags 0x48ac00f | none |

Guest without the plugin and native give the same sequence on every switch (third person: shown / first-person root hidden;
first person: hidden / shown). The shadow shots (looking down at 70 degrees, in sunlight outside Whiterun and in Dragonsreach)
show no body shadow in either; other actors' sun shadows are drawn in both.

## Change (`cf8c9e422`)

No engine or renderer change. `first-person.toml` and `first-person-switch.toml` turn Improved Camera off in every mode, so
guest and native compare the same player. `first-person-switch` adds `first-shadow` (`tai`, `player.setangle x 70` in sunlight)
and `interior-shadow` (the same in Dragonsreach).

## Runs (stage `s7-fpbody`, Dev-Fast)

The `a-` runs are the observe runs above (built with the temporary log). The `b-` runs use the final build, with
`env.CS_SNAPSHOT_DETECT=1` and first-person-switch's new steps passed with `--set` (and Improved Camera off for guest).

| Config | Label | Result | Render calls / live reads per world frame |
|---|---|---|---|
| guest+quick | `s7-fpbody-b-guest+quick` | passed | 66 (guest) / 1119 |
| native-vulkan+quick | `s7-fpbody-b-native-vulkan+quick` | passed | 1 / 608 |
| native-d3d12-quick | `s7-fpbody-b-native-d3d12-quick` | passed | 1 / 649 |
| native-vulkan+first-person-switch | `s7-fpbody-b-native-vulkan-fps` | passed, 7 shots | 1 / 524 |
| guest+first-person-switch | `s7-fpbody-b-guest-fps` | passed, 7 shots, plugin off | - / 673 |
| native-vulkan+first-person | `s7-fpbody-b-native-vulkan+first-person` | passed | 1 / 568 |
| native-vulkan+shadows-blend | `s7-fpbody-b-native-vulkan+shadows-blend` | passed | 1 / 555 |
| native-vulkan+cadence-drop-cells | `s7-fpbody-b-native-vulkan+cadence-drop-cells` | passed, 3 shots | 3 (drop:3, as before) / 594 |

The detector numbers are unchanged from snap3-pov: there is no code change.

## Decided by the agent, to confirm

- FB-a. No code fix: native already matches the engine without the plugin. Guest's first-person configs turn Improved Camera off
  instead, as native-vulkan does; the plugin stays on in the other guest configs.
- FB-b. The local-light check is weak: no shadow-casting local light reaches the Dragonsreach spot, so `interior-shadow` only shows
  that neither mode draws a body there. The third-person root is hidden in both modes, so no caster list can hold the body.

## Proposed decision-log entry

- **Closes 196:** the third-person root difference after a switch to first person came from the test setup. Improved Camera SE
  (off in native runs, on in guest) patches SetFirstPerson's third-person hide and shows the body. Without it guest hides the root
  exactly as native does, and neither draws a player shadow in first person. The first-person configs turn the plugin off in every
  mode; `first-person-switch` gained two shadow shots.

## Open items

- Improved Camera's first-person body (and its shadow) in native needs the plugin's D3D11 draws ported (the extension API note's
  "engine draw family a plugin adds geometry to"). Until then native has no first-person body when the plugin is installed.
