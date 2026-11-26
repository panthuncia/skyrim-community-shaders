// Screenshots in the graph modes: the vanilla screenshot (S24, decision 15) and a save's screenshot (S15). Included by ImageSpace.cpp
// after NativePost.inl, before NativeWorld.inl, whose capture calls CaptureScreenshots.
//
// The vanilla screenshot takes the same route in native and guest mode, since neither has a CS pass for DrawFrame's copies from
// kFRAMEBUFFER. The game side keeps its part on the game thread. PrintScreen's request is taken in the snapshot capture, before Main::DrawFrame
// sees it, and the engine's own screenshot function (SE 35882 / AE 36853) runs there: it names the file, shows the HUD message,
// moves iScreenShotIndex and saves the INI. Its one render call, Renderer::SaveRenderTargetToFile of kSCREENSHOT (0x4C), only
// records the file. The capture mode (AE 36591) and Papyrus's TakeScreenshot call the same function. The snapshot carries the files,
// and the render side has CS's Screenshot feature save the frame to each. So DrawFrame's two PrintScreen copies never run.
//
// A save's screenshot: the save request pump asks for it through the manager's +0x211. The capture takes it as a count, the render
// side's end-of-frame effects copy the frame into kSAVE_GAME_SCREENSHOT and report the count back, and a later capture completes the
// request as the engine's end-of-frame effects did (+0x211 cleared, +0x218 the frame counter). Main::DrawFrame's handshake then
// marks it ready and the pump writes the save. Native only: guest's end-of-frame effects complete the request at render.

namespace NativePost
{
	/** @brief The vanilla screenshot files the engine asked for since the last capture. Any thread: Papyrus calls in from its own. */
	struct VanillaScreenshots
	{
		std::mutex lock;
		std::vector<std::string> files;
		std::uint32_t count = 0;                                   // game thread: every file taken into a snapshot so far
		std::array<std::string, Block::kScreenshotRing> recent{};  // game thread: file n at n % kScreenshotRing
	};

	VanillaScreenshots& Vanilla()
	{
		static VanillaScreenshots vanilla;
		return vanilla;
	}

	/**
	* @brief The call of Renderer::SaveRenderTargetToFile in the engine's screenshot function (SE 35882 +0x73, AE 36853 +0x69). The
	* file the engine names goes to the next snapshot, made absolute against the game's folder as the engine's relative name is.
	*/
	struct SaveScreenshotTarget
	{
		static void thunk(void* a_renderer, std::uint32_t a_target, const char* a_file, std::int32_t a_format)
		{
			(void)a_renderer;
			(void)a_target;
			(void)a_format;
			std::error_code error;
			auto path = std::filesystem::absolute(a_file ? a_file : "ScreenShot.png", error);
			if (error)
				path = a_file ? a_file : "ScreenShot.png";
			auto& vanilla = Vanilla();
			const std::lock_guard guard(vanilla.lock);
			vanilla.files.push_back(path.string());
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	* @brief The screenshot function records its file instead of saving the engine's target. Test commands that ask as the game does: "call imagespace.printscreen" (PrintScreen's request), "call imagespace.save=quick|auto"
	* (the save request pump's quicksave or autosave, both of which wait for a save screenshot).
	*/
	void InstallScreenshots()
	{
#if !defined(CS_RELEASE_PROFILE)
		::Testing::RegisterFeatureCommand("imagespace", "printscreen", [](const std::string&, std::string& a_error) {
			auto* controls = RE::MenuControls::GetSingleton();
			if (!controls || !controls->screenshotHandler) {
				a_error = "no screenshot handler";
				return false;
			}
			controls->screenshotHandler->screenshotQueued = true;
			return true;
		});
		::Testing::RegisterFeatureCommand("imagespace", "save", [](const std::string& a_arg, std::string& a_error) {
			// The save request pump's codes (SE 34859 / AE 35769 queues one): QuickSaveLoadHandler's quicksave, the autosave handler's.
			const std::uint32_t code = a_arg == "quick" ? 0xF0000200u : a_arg == "auto" ? 0xF0000040u :
			                                                                              0u;
			auto* manager = RE::BGSSaveLoadManager::GetSingleton();
			if (!code || !manager) {
				a_error = "not quick or auto, or no save manager";
				return false;
			}
			using Queue = void (*)(RE::BGSSaveLoadManager*, std::uint32_t);
			reinterpret_cast<Queue>(REL::RelocationID(34859, 35769).address())(manager, code);
			return true;
		});
#endif
		stl::write_thunk_call<SaveScreenshotTarget>(REL::RelocationID(35882, 36853).address() + REL::Relocate(0x73, 0x69));
		logger::info("[ORG] ImageSpace: the vanilla screenshot is a snapshot event, saved by the Screenshot feature");
	}

	/**
	* @brief Game thread, in a snapshot's capture: PrintScreen's request (MenuControls' screenshot handler +0x10, which Main::DrawFrame
	* would consume after the capture) and the files asked for since the last capture; in native mode also the save screenshot
	* handshake.
	*/
	void CaptureScreenshots(Block& a_block)
	{
		if (auto* controls = RE::MenuControls::GetSingleton(); controls && controls->screenshotHandler && controls->screenshotHandler->screenshotQueued) {
			controls->screenshotHandler->screenshotQueued = false;
			// The engine's own screenshot function, as Main::DrawFrame calls it after its copies: PNG, named from iScreenShotIndex.
			using SaveScreenshot = void (*)(const char*, std::int32_t);
			reinterpret_cast<SaveScreenshot>(REL::RelocationID(35882, 36853).address())(nullptr, 3);
		}
		auto& vanilla = Vanilla();
		{
			const std::lock_guard guard(vanilla.lock);
			for (auto& file : vanilla.files) {
				++vanilla.count;
				vanilla.recent[vanilla.count % Block::kScreenshotRing] = std::move(file);
			}
			vanilla.files.clear();
		}
		a_block.screenshots = vanilla.count;
		a_block.screenshotPaths = vanilla.recent;
		if (!RenderGraph::Engine::Native::Requested())
			return;

		static std::uint32_t s_saveShots = 0, s_inFlight = 0;
		const auto base = ManagerBase();
		if (!base)
			return;
		// The render side copied the request: complete it as RenderEndOfFrameEffects did.
		if (s_inFlight && g_saveShotsDone.load(std::memory_order_acquire) >= s_inFlight) {
			Field<bool>(base, 0x211) = false;
			Field<std::uint64_t>(base, 0x218) = (std::max)(std::uint64_t{ 1 }, static_cast<std::uint64_t>(*G().frameCounter));
			if (s_inFlight <= 16)
				logger::info("[ORG] native post chain: save screenshot {} copied by the render side, completed for the game", s_inFlight);
			s_inFlight = 0;
		}
		// +0x211 stays set while the copy is pending, so the handshake asks for nothing more meanwhile.
		if (!s_inFlight && Field<bool>(base, 0x211))
			s_inFlight = ++s_saveShots;
		a_block.saveShots = s_saveShots;
	}

	/** @brief Render owner, each render frame's Begin, in both graph modes: the snapshot's new vanilla screenshots go to the Screenshot feature. */
	void RenderScreenshots()
	{
		const auto& snapshot = Snapshot::Current();
		const auto* block = snapshot ? snapshot->Block<Block>() : nullptr;
		if (!block)
			return;
		static std::uint32_t s_done = 0;
		if (block->screenshots == s_done)
			return;
		// A count behind the snapshot's by more than the ring lost files: logged, the newest are saved.
		if (block->screenshots - s_done > Block::kScreenshotRing) {
			logger::warn("[ORG] ImageSpace: {} vanilla screenshots came in one render frame; the oldest {} are not saved", block->screenshots - s_done,
				block->screenshots - s_done - Block::kScreenshotRing);
			s_done = block->screenshots - Block::kScreenshotRing;
		}
		while (s_done != block->screenshots) {
			++s_done;
			const auto& file = block->screenshotPaths[s_done % Block::kScreenshotRing];
			logger::info("[ORG] ImageSpace: vanilla screenshot {} to {} (snapshot {})", s_done, file, Snapshot::CurrentId());
			globals::features::screenshotFeature.RequestCapture(file);
		}
	}
}
