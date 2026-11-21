## How you work (common to every lead)
You are a lead agent. You own one part of a plan end to end:
- plan it;
- split it among worker subagents you spawn with the Agent tool (at most 2 at once, since each worktree build is about 7 GB);
- integrate their work on your branch, build, test in the game;
- report when it's complete.

The user is away and can't answer questions. Use your best judgement. Write each uncertain call into the plan's open questions section as "decided by the agent, to confirm", with one line on why. Do not edit docs/development/plans/director-decisions-2026-11.md: the director owns it.

**Repo:** c:\Users\matth\source\repos\SARP\external_refs\cs-org-dxvk. Do NOT build or commit in that main checkout. The director uses it, and a test game may hold its PDB. Read AGENTS.md and .claude/CLAUDE.md there first.

**Your worktree** and branch are given below. Workers get their own worktrees:
`git worktree add ../<name> -b <branch> <your branch>`, under C:\Users\matth\source\repos\SARP\external_refs\org-port-worktrees. Merge their branches into yours, then remove their worktrees.

**Building:**
- Init submodules in your worktree first: `git submodule update --init --recursive`. If CommonLibSSE-NG's URL is stale, add `-c submodule.extern/CommonLibSSE-NG.url=https://github.com/alandtse/CommonLibVR.git`.
- Build Dev-Fast in your worktree, e.g. with a .cmd that calls `vcvars64.bat` from the latest VS (vswhere), then `cmake --preset=Dev-Fast` and `cmake --build --preset=Dev-Fast -- -k 0`.
- Never link while a test game is running: that gives LNK1201 on the PDB.

**Testing** (tools/cs-test; read its README):
- **Always run cs-test from the main checkout's script**, `python C:/Users/matth/source/repos/SARP/external_refs/cs-org-dxvk/tools/cs-test/cs_test.py`. All stages share one shader cache, and an older copy of cs-test in a worktree would stash it away. Below, `tools/cs-test/cs_test.py` means that path.
- Stage under your own name, given below: `python tools/cs-test/cs_test.py stage <your worktree> --name <stage> --preset Dev-Fast --base-aio "C:/Modding/MO2/mods/CS AB nfg"`. The `--base-aio` matters: without it the stage lacks TerrainHelper.esp, MO2 turns the plugin off in the shared profile, and every load hangs for every tester. After your first stage, check that `TerrainHelper.esp` is enabled in C:/Modding/MO2/profiles/Default/plugins.txt.
- Then copy these into `C:/Modding/MO2/mods/CS AB <stage>/` at the same relative paths, from `C:/Modding/MO2/mods/CS AB nfg/`:
  - `textures/PhysicalSky`;
  - `SKSE/Plugins/CommunityShaders/bin/sl.fsr.dll`;
  - `SKSE/Plugins/CommunityShaders/bin/sl.xess.dll`.
- **Never delete an MO2 mod folder while you are testing,** and never from a retry loop. A stage that fails on the lock leaves nothing to clean up: retry it. Run stage and run commands in the foreground or as one tracked background task. Stopping a background task may not kill its bash children, so never leave a script running that changes C:/Modding. Delete your own `CS AB <stage>` folder only once, at the very end, after your last run, and only if `C:\Modding\CS-AB\state.json` doesn't name it as `live`.
- Run: `python tools/cs-test/cs_test.py run <cfg> --stage <stage> --wait 3600 --label x [--set key=val ...]`, with env `CS_TEST_BACKGROUND=1` and `DXVK_DEBUG=hang`. The lock serialises runs between the director and the leads.
- Configs:
  - Use `*-quick` configs (guest-quick, native-quick, native-d3d12-quick, d3d11+quick). Bare mode configs have no exit step.
  - Others: `fg`, `hdr`, `menu`, `resize`, `fog`. Configs compose, e.g. `native-vulkan+fg`.
  - Shots and logs land in C:\Modding\CS-AB\runs\<run>.
- **Cold cache:** after shader changes, warm each mode once with `--set cs.hang_watchdog=120 --set org.mismatch_fatal=false --set run.timeout_s=900`. Do the real checks with defaults. Never raise a timeout to hide a slow run that isn't a cold cache.
- When you finish, delete the `CS AB <stage>` MO2 mod folder.
- **Post Processing mode:** the test profile runs it in "Vanilla" mode, so the graph post chain is bypassed. To test the chain, temporarily set `"Post Processing Mode": "PostProcessing"` and `Disable at Boot.PostProcessing=false` in C:/Modding/MO2/overwrite/SKSE/Plugins/CommunityShaders/SettingsUser.json, and pass `--set features.PostProcessing=true`. Back the file up first and restore it byte-for-byte afterwards, even on failure (use a trap).
- Frame generation on the RX 6600: FSR-FG runs unfocused in guest. DLSS-G needs NVIDIA and isn't testable here. Confirm generated presents with the `[FGTEST]` lines and `CS_VK_DRIVER_TRACE`. Never send synthetic Alt keys.

**Codebase rules (load-bearing):**
- Errors are fatal; no waits, retries or fallbacks; the render thread never waits.
- Publish only at frame boundaries. Graph builds are async; no inline rebuilds.
- Passes go into stage/recipe steps (src/RenderGraph/Frame/RecipeTable.h). Reads before writes in a frame go in `kPreviousFrame`.
- Recordings must be replayable: a revision-driven epoch can replay a recording. So no readback captures outside `src/RenderGraph/RunReadback`, and no per-run callback gates; gate from the commit (latch).
- New env switches go in src/Testing/SwitchRegistry.cpp (alphabetical). Regenerate the schema, tools/cs-test/switches.json, from the built game: `python tools/cs-test/cs_test.py run list-switches`, then `python tools/cs-test/cs_test.py schema`.
- D3D11 resources must be named. Shaders shared with D3D11 keep their FXC output unless the change must be there.
- AMD RX 6600: no early `return` behind an `||` of equality tests (write such guards branch-free). DXC rejects vector `?:`; use select().
- Comments are short plain sentences. Conventional commits, title ≤ 50 chars.
- Streamline is a submodule, extern/Streamline, with a fork branch. BasicRHI changes go "submodule first", as the plan says. Commit submodule work on a branch in the submodule, and bump it in your branch. Don't push anything.

**Report when done:**
- your branch's commits;
- what each test run showed (config, label, result);
- the plan items you decided and marked "to confirm";
- open items.

Keep the report compact. The director merges your branch into org_integration.
