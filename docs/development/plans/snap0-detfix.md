# Snapshot roadmap phase 0: dark shots under the detector (lead `detfix`)

## The bug

Every quick run with `CS_SNAPSHOT_DETECT=1` gave a dark start shot. A diagnostic must not change what is drawn.

## Cause

The engine call sites (`src/EngineZones.cpp`) went through a C++ thunk per site, `uintptr_t thunk(a, b, c, d, e, f)`, which
forwarded six integer arguments and returned RAX. The table was meant to leave out every call that passes more, but
`perf-tools/gensites2.py` looks for a seventh argument at `[RSP+0x38..0x60]`. The seventh argument is at `[RSP+0x30]`
(shadow space 0x00-0x1F, the fifth at 0x20, the sixth at 0x28). So two calls to AE 107221 (SE 100496) were listed:

- `AE 107221 (in Main::RenderWorld)`, site 73;
- `AE 107221 (in Main::RenderWaterEffects)`, site 119.

AE 107221 takes seven arguments and stores them into a structure (`+0x40..+0x54`, then zeroes `+0x58`). Through the thunk its
seventh argument was whatever lay on the thunk's stack, so `+0x54` got garbage every frame.

Confirmed by bisection: the old thunk build with only those two sites left out gives a normal shot (61.8 against 15.2
with all sites and 62.2 with the detector off). Nothing else in the table mattered.

## Fix

The thunk is gone. Each site's CALL now goes to a small machine-code stub on the detector's trampoline:

```
push rax; push rcx
mov rax, &g_kind; movzx eax, byte [rax]
mov rcx, &g_entries[site]; lock inc qword [rcx+rax*8]
(render role) mov rcx, &g_frameCalls; lock inc qword [rcx]
pop rcx; pop rax
jmp [rip+0] -> the call's old target
```

It touches only RAX, RCX and the flags and restores RAX and RCX, so every argument register, XMM register, stack argument
and the return value reach the callee and the caller as the engine left them, whatever the signature. There is no work
after the call. The frame kind the stubs read is EngineZones' own `g_kind`, set in `BeginFrame` next to EngineReads'.
`CallTarget` returns the stored old target.

Tracy zones: only with `CS_ENGINE_ZONES=1` in Tracy builds (decided by the agent, to confirm: `CS_SNAPSHOT_DETECT` counts
only, so the detector run stays the plain stub). The site's stub then jumps to a shared entry stub that saves RCX, RDX,
R8-R11 and XMM0-5, and while Tracy is connected begins a zone and swaps the return address for a shared exit stub's (the
original goes on a per-thread stack). The exit stub saves RAX, RCX, RDX, R8-R11 and XMM0-5 around the zone end and returns
to the engine's caller. Not connected, nothing is swapped. Known limit: an exception unwinding through a swapped frame would
not find the caller; only while Tracy is connected under `CS_ENGINE_ZONES`.

The table and its structure are unchanged (the `Role` column stays, so `ui2` reclassifying `MenuManager::DrawInterface` is
still a one-line change). Its comment now says any call is safe to list. `gensites2.py` (outside the repo,
`perf-tools/gensites2.py`) keeps its signature skips so the table and the counts measured against it stay as they are; its
docstring says they are no longer a safety rule and names the off-by-one.

## Runs (cs-test, stage s0-detfix, Dev-Fast)

Start-shot mean channel value (0-255) and engine render calls per world frame. All passed.

| Label | Build | Mode | Switch | Mean | Render calls |
|---|---|---|---|---|---|
| s0-detfix-nvk-off1 | old thunk | native Vulkan | off | 62.2 | |
| s0-detfix-nvk-detold | old thunk | native Vulkan | DETECT | 15.2 | 69.3 |
| s0-detfix-nvk-skip7 | old thunk, sites 73 and 119 skipped | native Vulkan | DETECT | 61.8 | 69.4 |
| s0-detfix-nvk-on | stubs | native Vulkan | DETECT | 62.6 | 69.5 |
| s0-detfix-nvk-off2 | stubs | native Vulkan | off | 62.5 | |
| s0-detfix-nvk-zones | stubs | native Vulkan | ENGINE_ZONES | 61.4 | 70 |
| s0-detfix-d3d12-off | stubs | native D3D12 | off | 61.9 | |
| s0-detfix-d3d12-on | stubs | native D3D12 | DETECT | 62.5 | 70.2 |
| s0-detfix-guest-off, -off2 | stubs | guest | off | 61.4, 61.8 | |
| s0-detfix-guest-on, -on2 | stubs | guest | DETECT | 57.5, 61.8 | 69.7, 68 |

Noise floor: the two native Vulkan off runs differ by a mean channel diff of 18.7 (eye adaptation and time of day move every
run); off against on is 15.7 in native Vulkan, D3D12 and guest, inside it. The guest-on 57.5 was a single low run; the
second pair reads 61.77 both. 249 of 252 sites install on AE, as before. The zones run had no Tracy connection, so it
exercised the shared entry stub's register saves but not the return swap.

## Open items

- Regenerating the table without the signature skips would add sites (and change the per-frame counts); not done, to keep
  the baselines comparable.
