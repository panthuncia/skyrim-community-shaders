# XeSS TDR and HDR colours (native D3D12, RX 6600), 2026-11-18

Investigation only. Nothing in the repo was edited, built or staged. No game launches: the coordinator held launches, and a
foreground game (started 19:10:24) was running. Evidence: the saved session log
(`scratchpad/tdr-xess/CommunityShaders.log`), the Windows event log, and code in worktree `test-runner` (`d7aadb11e`), including
`extern/Streamline` (CS's own `sl.xess` and `sl.fsr` plugins). **(inferred)** marks a conclusion, not something read.

## 1. Timeline from the log

| Time | Line | Event |
|---|---|---|
| 19:01:16.42 | 366-373 | Streamline on D3D12 for the native device (manual hooking). D3D12 device: debug layer off, **DRED off**, **4 frames in flight**. |
| 19:01:17.86 | 628 | Native support: DLSS=false Reflex=true FSR=true **XeSS=true**. So XeSS can be selected on this card in native D3D12 (DP4a path). |
| 19:01:17.92 | 661-664 | HDR display found (OS HDR on). Setting off at start: colour space SDR, back buffer format 24 (R10G10B10A2). |
| 19:01:19.03 | 993 | Upscaling: method 2 (FSR), quality 1, 1280x720 -> 1920x1080. |
| 19:01:27.79 | 1466 | 15 s Present stall report (loading, known). |
| 19:01:38.5 | 2004 | Loading screen -> world. |
| 19:01:57.27 | 4163-4166 | HDR on: "applying enableHDR=true for the next rendered frame"; swap chain HDR10 set the same frame (frame ~1836). |
| 19:01:57.32 | 4179-4190 | Post chain re-registered ("absent passes ... asks for a build at frame 1837"). DCLF load hold starts: presents held 6 frames. |
| 19:01:57.35 | | HDR output anchor `cs.hdr` attached 2 frames after it was asked. |
| 19:01:57.51 | | Post segment's new build switches in at **frame 1846**, "asked for at frame 1837, 9 frames before". |
| 19:01:58.76 | 4409-4419 | HDR off: colour space SDR immediately; post chain re-registered again (LdrScratch format 10 -> 24). |
| 19:01:59.70 | 4576-4586 | HDR on again, same pattern. |
| 19:02:08.56 | 5424-5425 | Method FSR -> XeSS (quality 1, 1280x720). Hudless texture created. First XeSS evaluate. |
| 19:02:11.54 | 5690-5708 | Quality 1 -> 3 (Performance, 960x540). Post chain re-registered at ratio 0.500. No error. |
| 19:02:14.13 | 6041-6052 | Quality 3 -> 2 (Balanced, 1129x635; the user's step "back to quality" passed through Balanced). CS logs "XeSS evaluate result=0" (render thread, 14.144). Post chain re-registered at 0.588. |
| 19:02:14.159 | 6063 | `[C] DX12 timeline diagnostics ... completed=UINT64_MAX`: device removed. |
| 19:02:14.164 | 6128 | `sl.xess: xessVKInit failed (-1000)` from `xessEntry.cpp:231 [ensureContext]`, on the recording thread (tid 21596). |
| 19:02:15 | event log | SkyrimSE.exe fast-fail 0xc0000409 in ucrtbase (the fatal abort after device loss **(inferred)**). |
| 19:02:21-22 | event log | ~50 `LiveKernelEvent` reports, mostly code **141** (VIDEO_ENGINE_TIMEOUT_DETECTED), also 1a8, 1b8, 1a1, a1000001. A GPU hang (TDR), not a clean page-fault removal. No Display 4101 entry. |

No hang or crash dump of CS was written for this session (none in `My Games\...\SKSE` after 19:01). No DRED data (DRED off in a
foreground run).

## 2. The TDR

### Most likely cause: `sl.xess` destroys the XeSS context while frames in flight still use it (plain bug)

`extern/Streamline/source/plugins/sl.xess/xessEntry.cpp:ensureContext` is called from `xessEndEvaluation` on every evaluate. When
the output size **or the quality mode** differs from the context's, it does:

```cpp
if (ctx.xessContext) { ctx.api.destroyContext(ctx.xessContext); ctx.xessContext = nullptr; }   // line 173-177
... d3d12CreateContext, d3d12BuildPipelines, d3d12Init ...
```

`xessDestroyContext` is immediate. The XeSS header says (`extern/Streamline/external/xess/inc/xess/xess.h`, `xessDestroyContext`):
"The user must ensure that any pending command lists are completed before destroying the context." Nothing does: the device has 4
frames in flight, and earlier frames' command lists recorded `xessD3D12Execute` with the old context's internal resources (history,
constant buffers, descriptor heaps, pipelines). CS does not drain on a quality change: `Upscaling.cpp:CheckResources` only reacts to a
method change (and its drain is `DXVKInterop::DrainCommandRing`, a guest-only mechanism). The evaluate is recorded every frame at
execution (`ORGUpscaling.cpp:EvaluatePass::Record`), so the old context is not replayed later; the hazard is only the frames already
submitted.

Evidence that fits:
- Every XeSS quality change destroys the context. The crash came 15-30 ms after the second such change (Performance -> Balanced).
- Order on the recording thread **(inferred from timestamps)**: destroy (about 14.13-14.14), then `xessD3D12BuildPipelines` (slow,
  tens of ms), during which the GPU hits freed memory and hangs; the host sees UINT64_MAX at 14.159; `xessD3D12Init` then fails with
  -1000 (XESS_RESULT_ERROR_UNKNOWN) at 14.164 because the device is gone.
- Event log: 141 = engine timeout. On RDNA2 a shader reading freed or re-used memory commonly hangs rather than reporting a clean
  fault **(inferred)**.
- The first change (Quality -> Performance at 19:02:11) also destroyed a live context and survived. That fits a race: whether the
  freed memory is unmapped or reused before the GPU reaches the old frames' executes **(inferred)**. Shrink vs grow is not the
  deciding factor for XeSS: both directions change the mode, so both destroy.

Classification: **plain bug** (SDK misuse / use-after-free of SDK resources with frames in flight). It is triggered by a mid-session
setting change, but the frame-revision plan would not fix it: even with atomic adoption at Begin, the plugin would still destroy the
context on the first evaluate of the new revision while older frames run. Confidence: **medium-high**. Code and SDK contract are
unambiguous; the link to this exact crash rests on timing (no DRED, no repro).

### Alternatives

- **Resize class (render-size dependents re-made under in-flight frames).** The quality change re-registers the post chain at a new
  ratio in the same frame. Earlier fixes (`e6b0f078b`, `7914a8fd5`, `5c3fcce9b`, `31f68b7a8`) cover the known parts; the log shows no
  stale-recording or tap error before the loss. Upscaling's own graph textures are display-size. Not ruled out, but there is no log
  sign of it. **Open.**
- **FSR -> XeSS method change** at 19:02:08: no context destroyed (first XeSS context), ran 3 s fine. Ruled out as the trigger.
- **HDR**: HDR was toggled 10 s earlier and frames ran normally since. Ruled out.
- **Wrong XeSS backend on D3D12.** The `xessVKInit` text suggested a Vulkan path on D3D12. It does not: the D3D12 branch calls
  `d3d12Init`; the error text at line 231 is shared by both branches and is wrong for D3D12 (a cosmetic bug, section 5).

### Same latent bug in `sl.fsr`

`extern/Streamline/source/plugins/sl.fsr/fsrEntry.cpp:ensureUpscaleContext` also calls `ffxApi.DestroyContext` immediately when the
render size grows past the context's, or the display size or HDR flag changes. So FSR Performance -> Quality (render size grows)
has the same hazard **(inferred: the FFX DX12/VK backends release resources on destroy without a GPU wait; samples flush the GPU
before destroying)**. FSR shrinking keeps the context, which is why FSR quality changes seem safe in one direction. DLSS is not
available on this card; not checked.

Guest mode runs the same plugins on Vulkan (`xessVK*`, FFX VK): the same immediate destroy applies there.

## 3. The HDR colours

### Most likely cause: the swap chain colour space and the content that encodes for it switch on different frames (resize class)

On an HDR toggle, three parts of one change are adopted at three times:
1. `HDRDisplay.cpp:BeginRenderFrame` latches `hdrEnabledForFrame` from the live setting and calls `UpdateSwapChainColorSpace` at
   once. `NativeDevice.cpp:SetSwapChainColorSpace` drains submissions and reconfigures the swap chain: the colour space is new from
   the first frame (frame ~1836). `PostChainTarget`, `RedirectFramebuffer` and the ImGui target format (log: "ImGui: drawing onto an
   imported render target ... format 28/24") follow the latch the same frame.
2. The HDR output anchor `cs.hdr` (PQ encode into the back buffer) attaches 2 frames later.
3. The post stage's new build (LdrScratch format 10 vs 24, the post chain's target HdrTexture vs framebuffer) switches in 9-10
   frames later (frame 1846).

In between, frames present with the new colour space and the old post build **(inferred from the log's frame numbers; no shots
taken)**:
- HDR on: the old post build writes SDR-encoded output into the frame path, while the swap chain reads PQ/Rec.2020, or the new HDR
  output pass reads an HdrTexture the old build did not write (born undefined: `HDRDisplay` notes "A new texture's contents are
  undefined"). Either gives washed-out, over-bright or garbage colours.
- HDR off: the swap chain is sRGB, but the old build still renders into HdrTexture that nothing composes: black or stale frames, or
  PQ values read as sRGB (dark, desaturated). This is the known "few black frames".
- On the first toggle DCLF also held presents for 6 frames (`load hold: ... a graph rebuild`), which hides some of the window and
  shows a frozen frame.

Classification: **resize / mid-frame-change class**. Confidence: **medium**. The frame mismatch is clear in the log. That it is what
the user saw is inferred: there are no shots of those frames.

Not a plain bug found here: the formats are right (back buffer 24 in both states, HDR10 metadata set), and the colour space change
drains submissions before reconfiguring, so frames already in flight do not present under the new space.

## 4. Additions for `frame-revision-plan.md`

The plan already covers: HDR as variables `HdrEnabled`/`HdrOutputTarget` (5b, line ~446), 6.15 (HDR Display), 9a ("HDR swap chain
colour space and format: an `HdrEnabled`/`SwapChainFormat` revision. The old chain draws until the new one is adopted"), 7.7 Native,
and the `hdr` test "no black frame in a shot taken 2 frames after the switch" (step 9). It does **not** mention upscaler SDK contexts
anywhere, and its HDR text is about black frames, not about the colour space being applied ahead of the content.

Text to add:

**4.1 Step 9a, after the "HDR swap chain colour space and format" bullet (new sub-bullet):**

> - The colour space call itself moves into adoption. Today `HDRDisplay.cpp:BeginRenderFrame` latches `hdrEnabledForFrame` from the
>   live setting and calls `UpdateSwapChainColorSpace` (native: `NativeDevice.cpp:SetSwapChainColorSpace`) on the same frame, while the
>   HDR output anchor attaches about 2 frames later and the post stage's build 9-10 frames later (log of 2026-11-18, frames 1836-1846).
>   Frames in between present PQ space with SDR content, or the reverse: "strange colours" on the switch, black frames on the way back.
>   End state: `SetSwapChainColorSpace` runs inside `AdoptLatest`, for the revision whose post chain, HDR output client, LdrScratch
>   format and ImGui target format were prepared for that `HdrEnabled` value. `hdrEnabledForFrame` is `Adopted().Get(HdrEnabled)`,
>   never the live setting. The drain it does today stays (a present queued on the old space finishes first); it is the only wait in
>   adoption and is allowed because the swap chain is reconfigured. If that wait breaks "the render thread never waits" (2.2), the
>   alternative is to switch the colour space at the first present of the adopted revision on the presenter thread.
> - Guest: `OwnedFrame.cpp:ApplyColorSpace` (at present) gets the same rule: it applies the colour space of the revision whose content
>   it presents.

**4.2 Step 9, Test (extend the `hdr` acceptance):** a black-frame check is not enough; wrong colours are not black.

> New cs-test config `hdr-frames`: with the menu closed, `hdr 1`, then shots at frames 1, 2, 3, 5, 10 and 60 after the step; then
> `hdr 0` and the same shots. Acceptance: every shot after `hdr 1` decodes (PQ) to within the noise of the frame-60 shot (mean and
> 99th percentile luminance, and mean chroma), and every shot after `hdr 0` matches its frame-60 SDR shot. Needs shots that copy the
> frame as presented, without waiting for a build to switch in (today's `shot` may wait, which would hide the bad frames: check
> `Testing` before relying on it).

**4.3 New sub-section 7.11 "SDK contexts and frames in flight" (and a row in step 5b's table):**

> Upscaler SDKs keep their own GPU objects (XeSS context, FFX context, NGX feature) whose shape depends on variables: `UpscaleMethod`,
> the quality mode, `DisplaySize`, and for FSR `RenderSize` growth and the HDR flag. They are bound state like a texture version, but
> they live inside Streamline plugins, so ORG cannot version them.
> - Rule: an SDK object replaced by an options change is retired on the retirement clock, after the last submitted frame that recorded
>   an evaluate with it. It is never destroyed at the next evaluate. Today both CS plugins break this
>   (`sl.xess/xessEntry.cpp:ensureContext`, `sl.fsr/fsrEntry.cpp:ensureUpscaleContext`); that is a plain bug to fix first (see
>   `_notes/xess-tdr-hdr-colours.md` section 5), not something the revision lane fixes by itself.
> - The SDK options for a frame (`slXeSSSetOptions`/`slFSRSetOptions`/`slDLSSSetOptions`: mode, output size) come from the adopted
>   revision, not the live settings (`Streamline.cpp:EvaluateXeSS` reads `settings.qualityMode` per call today). The new context is
>   then created on the first frame of the revision whose render size and post chain match it.
> - Better (removes the trigger): prepare the new context on the revision lane, as a fragment, so creation and
>   `xessD3D12BuildPipelines` (tens of ms) do not run on the recording thread. That needs a plugin entry point to prepare a context
>   for given options without evaluating. Decide in step 6.14.
> - Decision 6a interacts: with render-size textures at display size, a quality change is constants-only for CS, but XeSS still needs
>   a new context per quality mode (its input range is per preset **(inferred; check `xessGetInputResolution` min/max per preset)**).
>   If one preset's input range covers every ratio CS offers, one context per output size would remove the trigger.

Step 5b table, new row:

> | `UpscalerOptions` | method, quality mode, output size, HDR flag | `Upscaling` settings; read by `Streamline.cpp:Evaluate*` from `Adopted()` |

**4.4 Step 6.14 (Upscaling) and step 8 tests:**

> New cs-test config `xess-quality` (native D3D12, native Vulkan, guest): `upscale xess`, 120 frames, then `setting
> Upscaling.qualityMode=` 3, 1, 3, 2, 1, 4, 1, a few frames apart (also two on consecutive frames), shots after each. Same for `fsr`
> (the growth direction is the one that recreates FSR's context). Run with DRED (`DXVK_DEBUG=hang` does this in cs-test) and once
> with `org.validation=true`: the D3D12 debug layer reports objects deleted while still in use. Acceptance: no device loss, no debug
> layer "deleted while still in use" message, shots match. Step 8's `dynres` config should include XeSS and FSR, not only the
> default method.

## 5. Plain bugs, for a separate agent

### 5.1 `sl.xess`: context destroyed with command lists in flight (the TDR)

- File/function: `extern/Streamline/source/plugins/sl.xess/xessEntry.cpp:ensureContext` (lines 168-241); also `slOnPluginShutdown`
  (line 542, the same call at shutdown; fine only if the host has drained by then).
- Fix (plugin side, both backends): do not destroy in `ensureContext`. Move the old handle to a retire list with the frame id
  (`evd.frame`) and destroy it once it is older than the frames in flight plus slack (CS has 4 on D3D12; use 8, or better a value the
  host passes). Destroy the rest at shutdown after the host's drain. Alternative with a stall, simpler: `ctx.compute->waitForIdle(device)`
  (`sl.chi/compute.h:waitForIdle`) before `destroyContext`; a quality change is rare, so a one-off idle wait is acceptable as a first
  fix. A host-side alternative (CS drains the device in `Upscaling` when the method or quality changes) also works but leaves the
  plugin unsafe for other hosts and for output-size changes.
- Also fix the message at line 231: it says `xessVKInit` on the D3D12 path; log `xessD3D12Init` there. And the stale comment in
  `src/Features/Upscaling/Streamline.cpp:Initialize` (lines 428-429) saying CS's `sl.fsr`, `sl.fsr_g` and `sl.xess` "are Vulkan-only
  and stand down" on D3D12: since `7555408` FSR and XeSS run on D3D12.
- Verify: the `xess-quality` config above in native D3D12 with `org.validation=true` (debug layer should show "deleted while still in
  use" before the fix, none after), then without validation with DRED, 20+ quality changes, no device loss. Same in native Vulkan and
  guest.

### 5.2 `sl.fsr`: the same on context growth

- File/function: `extern/Streamline/source/plugins/sl.fsr/fsrEntry.cpp:ensureUpscaleContext` (line 213, destroy at 219-221).
- Fix: the same retire-list or idle wait.
- Verify: FSR, quality 3 -> 1 repeatedly (render size grows), and an HDR toggle if `colorHDR` ever changes (CS passes
  `colorBuffersHDR = true` always, `Streamline.cpp:2113`, so today only size growth and display size trigger it).

## 6. Other things in the log

- `D3D12: 'CS native present 0' was reset while it recorded; it was closed first and its commands dropped` (line 1101, 19:01:19,
  during the opening Bink movie). Dropped commands on the present list; not known to me as accepted. Worth a look with
  `BASICRHI_D3D12_RESET_TRACE=1`.
- `D3D12 barrier: buffer 0x1dababfb600 has no unordered access; the access bit is dropped from its barrier` (line 2863). A pass
  declares UAV access on a buffer made without UAV; the access bit is silently dropped. Possibly a wrong declaration.
- `declared view mismatch: cs.water.surface / cs.precipitation reads 'TerrainShadows::ShadowHeight' through descriptor 1908, its
  declared view is descriptor 9702` (lines 2385-2400). The plan says such lines remain only for DCLF readers; these are CS passes,
  so they are not covered by that known gap.
- The HDR toggle starts a DCLF load hold ("a re-entry: a graph rebuild"), holding presents 6 frames: part of the HDR class above,
  and it goes away when HDR switches by revision.
- `[Streamline] singular camera reprojection matrices on 1 frame(s)` at the first world frame: harmless, noted.
- The quality preset ratios in CS (`Upscaling.cpp:ConfigureUpscaling`: Quality 1.5, Balanced 1.7, Performance 2.0, Ultra 3.0) are
  fed to XeSS together with its own preset; XeSS's own ratios for Quality/Balanced differ in recent SDKs (1.5/1.7 vs XeSS 1.3's
  1.7/2.0) **(inferred)**. Not a crash cause; may matter for the input-range question in 4.3.

## Launches

None. The coordinator held launches during this investigation, and a foreground game was running. The game started fine at 19:10
after the TDR, so device creation works after this loss.
