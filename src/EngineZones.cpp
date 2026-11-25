#include "EngineZones.h"

#include "RenderGraph/Switches.h"

#include <array>
#include <cstdint>
#include <utility>

namespace EngineZones
{
#ifdef TRACY_ENABLE
	namespace
	{
		struct Site
		{
			std::uint64_t parent;  // AE id of the function holding the call
			std::uint32_t offset;  // the CALL rel32's offset in it
			std::uint64_t callee;  // AE id of the function it calls
			const char* name;
		};

		// Generated from Ghidra (AE 1.6.1170): the direct calls of the frame's render functions, without trivial setters and without
		// call sites that pass floats in XMM0-3, read a float return or pass a seventh argument. The thunk forwards six integer
		// arguments and RAX, nothing else.
		constexpr Site kSites[] = {
			{ 36564, 0xA97, 36555, "Engine Main::DrawFrame (in Main::Update)" },
		{ 36555, 0x39, 77243, "Engine Renderer::Lock (in Main::DrawFrame)" },
		{ 36555, 0x47, 77245, "Engine Renderer::Begin (in Main::DrawFrame)" },
		{ 36555, 0xC7, 36562, "Engine Main::DecalTest (in Main::DrawFrame)" },
		{ 36555, 0x162, 27041, "Engine FaceGen::UpdatePending (in Main::DrawFrame)" },
		{ 36555, 0x1CD, 82085, "Engine AE 82085 (in Main::DrawFrame)" },
		{ 36555, 0x1E5, 107698, "Engine AE 107698 (in Main::DrawFrame)" },
		{ 36555, 0x1EA, 107646, "Engine AE 107646 (in Main::DrawFrame)" },
		{ 36555, 0x207, 52210, "Engine AE 52210 (in Main::DrawFrame)" },
		{ 36555, 0x217, 82075, "Engine Main::FreezeTest #1 (in Main::DrawFrame)" },
		{ 36555, 0x284, 36558, "Engine Main::RenderScene #1 (in Main::DrawFrame)" },
		{ 36555, 0x2C3, 107130, "Engine AE 107130 (in Main::DrawFrame)" },
		{ 36555, 0x329, 105676, "Engine ImageSpaceManager::RenderEffect #1 (in Main::DrawFrame)" },
		{ 36555, 0x333, 35406, "Engine JobListManager::FrameJobs (in Main::DrawFrame)" },
		{ 36555, 0x343, 69516, "Engine WaitForJobTask (in Main::DrawFrame)" },
		{ 36555, 0x371, 82084, "Engine MenuManager::DrawInterface (in Main::DrawFrame)" },
		{ 36555, 0x3AC, 105676, "Engine ImageSpaceManager::RenderEffect #2 (in Main::DrawFrame)" },
		{ 36555, 0x414, 77247, "Engine Renderer::ApplyState (in Main::DrawFrame)" },
		{ 36555, 0x442, 105676, "Engine ImageSpaceManager::RenderEffect #3 (in Main::DrawFrame)" },
		{ 36555, 0x454, 36853, "Engine AE 36853 (in Main::DrawFrame)" },
		{ 36555, 0x495, 68121, "Engine Memory::static_ctor #1 (in Main::DrawFrame)" },
		{ 36555, 0x4A3, 68088, "Engine MemoryManager::GetThreadScrapHeap #1 (in Main::DrawFrame)" },
		{ 36555, 0x4AB, 68150, "Engine ScrapHeap::sub (in Main::DrawFrame)" },
		{ 36555, 0x4BE, 51639, "Engine AE 51639 (in Main::DrawFrame)" },
		{ 36555, 0x4C8, 36558, "Engine Main::RenderScene #2 (in Main::DrawFrame)" },
		{ 36555, 0x4E4, 68121, "Engine Memory::static_ctor #2 (in Main::DrawFrame)" },
		{ 36555, 0x4F2, 68088, "Engine MemoryManager::GetThreadScrapHeap #2 (in Main::DrawFrame)" },
		{ 36555, 0x50B, 52211, "Engine AE 52211 (in Main::DrawFrame)" },
		{ 36555, 0x55D, 12920, "Engine f_snprintf (in Main::DrawFrame)" },
		{ 36555, 0x5C3, 82075, "Engine Main::FreezeTest #2 (in Main::DrawFrame)" },
		{ 36555, 0x5CC, 36858, "Engine AE 36858 (in Main::DrawFrame)" },
		{ 36555, 0x5D5, 36857, "Engine AE 36857 (in Main::DrawFrame)" },
		{ 36555, 0x5DA, 36859, "Engine AE 36859 (in Main::DrawFrame)" },
		{ 36555, 0x5F5, 107502, "Engine AE 107502 (in Main::DrawFrame)" },
		{ 36555, 0x601, 77246, "Engine Renderer::End (in Main::DrawFrame)" },
		{ 36558, 0xF5, 107122, "Engine AE 107122 (in Main::RenderScene)" },
		{ 36558, 0x10F, 82074, "Engine AE 82074 (in Main::RenderScene)" },
		{ 36558, 0x11D, 36559, "Engine Main::RenderPlayerView (in Main::RenderScene)" },
		{ 36559, 0xD3, 36561, "Engine Main::SetupSunAndSky (in Main::RenderPlayerView)" },
		{ 36559, 0x11B, 76557, "Engine AE 76557 (in Main::RenderPlayerView)" },
		{ 36559, 0x172, 36560, "Engine Main::RenderWaterEffects (in Main::RenderPlayerView)" },
		{ 36559, 0x17E, 69353, "Engine JobList::Finish #1 (in Main::RenderPlayerView)" },
		{ 36559, 0x194, 106337, "Engine AE 106337 (in Main::RenderPlayerView)" },
		{ 36559, 0x1C4, 107132, "Engine Main::CalculateAndDrawShadowCasterLights (in Main::RenderPlayerView)" },
		{ 36559, 0x1F2, 36563, "Engine Main::FirstPersonCulling (in Main::RenderPlayerView)" },
		{ 36559, 0x26A, 107123, "Engine AE 107123 (in Main::RenderPlayerView)" },
		{ 36559, 0x2E7, 77503, "Engine BSGraphics::State::SetCameraData #1 (in Main::RenderPlayerView)" },
		{ 36559, 0x2EC, 107133, "Engine Main::RenderShadowMaps (in Main::RenderPlayerView)" },
		{ 36559, 0x30A, 77503, "Engine BSGraphics::State::SetCameraData #2 (in Main::RenderPlayerView)" },
		{ 36559, 0x31D, 107121, "Engine AE 107121 (in Main::RenderPlayerView)" },
		{ 36559, 0x325, 107014, "Engine VolumetricLighting::Descriptor (in Main::RenderPlayerView)" },
		{ 36559, 0x335, 107023, "Engine VolumetricLighting::Render (in Main::RenderPlayerView)" },
		{ 36559, 0x34F, 82074, "Engine AE 82074 (in Main::RenderPlayerView)" },
		{ 36559, 0x361, 37792, "Engine Actor::GetMovementController #1 (in Main::RenderPlayerView)" },
		{ 36559, 0x373, 37792, "Engine Actor::GetMovementController #2 (in Main::RenderPlayerView)" },
		{ 36559, 0x395, 107139, "Engine Main::RenderDepth (in Main::RenderPlayerView)" },
		{ 36559, 0x39C, 107140, "Engine Main::RenderShadowMasks (in Main::RenderPlayerView)" },
		{ 36559, 0x3A1, 107144, "Engine AE 107144 (in Main::RenderPlayerView)" },
		{ 36559, 0x3B6, 77503, "Engine BSGraphics::State::SetCameraData #3 (in Main::RenderPlayerView)" },
		{ 36559, 0x3C6, 107018, "Engine AE 107018 (in Main::RenderPlayerView)" },
		{ 36559, 0x3FC, 68121, "Engine Memory::static_ctor (in Main::RenderPlayerView)" },
		{ 36559, 0x412, 68115, "Engine MemoryManager::Allocate (in Main::RenderPlayerView)" },
		{ 36559, 0x42E, 69161, "Engine detail::BSFixedString::ctor8 (in Main::RenderPlayerView)" },
		{ 36559, 0x47F, 69164, "Engine BSFixedString::Release (in Main::RenderPlayerView)" },
		{ 36559, 0x645, 107131, "Engine AE 107131 (in Main::RenderPlayerView)" },
		{ 36559, 0x6B4, 107177, "Engine AE 107177 #1 (in Main::RenderPlayerView)" },
		{ 36559, 0x730, 107177, "Engine AE 107177 #2 (in Main::RenderPlayerView)" },
		{ 36559, 0x84D, 69353, "Engine JobList::Finish #2 (in Main::RenderPlayerView)" },
		{ 36559, 0x870, 106598, "Engine ShaderAccumulator::GroupFiveCount (in Main::RenderPlayerView)" },
		{ 36559, 0x879, 107145, "Engine AE 107145 (in Main::RenderPlayerView)" },
		{ 36559, 0x88C, 106600, "Engine ShaderAccumulator::ClearGroup #1 (in Main::RenderPlayerView)" },
		{ 36559, 0x89D, 106600, "Engine ShaderAccumulator::ClearGroup #2 (in Main::RenderPlayerView)" },
		{ 36559, 0x8CE, 107128, "Engine Main::ClearMasks (in Main::RenderPlayerView)" },
		{ 36559, 0x954, 107129, "Engine Main::RenderFirstPersonView (in Main::RenderPlayerView)" },
		{ 36559, 0x9C2, 107124, "Engine AE 107124 (in Main::RenderPlayerView)" },
		{ 36559, 0xA0F, 39034, "Engine AE 39034 (in Main::RenderPlayerView)" },
		{ 36559, 0xA4C, 107252, "Engine Main::RenderSaoCs (in Main::RenderPlayerView)" },
		{ 36559, 0xA54, 107148, "Engine Main::RenderPostProcess (in Main::RenderPlayerView)" },
		{ 36559, 0xA73, 107147, "Engine AE 107147 (in Main::RenderPlayerView)" },
		{ 36559, 0xA78, 107143, "Engine AE 107143 (in Main::RenderPlayerView)" },
		{ 107142, 0x11F, 106437, "Engine Renderer::StartAccumulating (in Main::RenderWorld)" },
		{ 107142, 0x1F2, 107245, "Engine ImageSpace::RenderSSAO #1 (in Main::RenderWorld)" },
		{ 107142, 0x21D, 107267, "Engine ImageSpace::RenderSnowSSS (in Main::RenderWorld)" },
		{ 107142, 0x253, 107219, "Engine AE 107219 (in Main::RenderWorld)" },
		{ 107142, 0x279, 107221, "Engine AE 107221 (in Main::RenderWorld)" },
		{ 107142, 0x286, 107217, "Engine ImageSpace::DispatchCopySubRegion (in Main::RenderWorld)" },
		{ 107142, 0x2BA, 107245, "Engine ImageSpace::RenderSSAO #2 (in Main::RenderWorld)" },
		{ 107142, 0x2DF, 106438, "Engine Renderer::FinishAccumulatingPostResolveDepth (in Main::RenderWorld)" },
		{ 107142, 0x2F5, 106324, "Engine AE 106324 (in Main::RenderWorld)" },
		{ 107142, 0x316, 106610, "Engine ShaderAccumulator::RenderFirstPersonViewEnd (in Main::RenderWorld)" },
		{ 107142, 0x334, 106979, "Engine AE 106979 (in Main::RenderWorld)" },
		{ 107142, 0x3FE, 106921, "Engine BSCullingProcess::Process (in Main::RenderWorld)" },
		{ 107142, 0x410, 106436, "Engine Renderer::SubmitAccumulator (in Main::RenderWorld)" },
		{ 107142, 0x616, 26353, "Engine Sun::SubmitOcclusionQuery (in Main::RenderWorld)" },
		{ 107148, 0x141, 77247, "Engine Renderer::ApplyState (in Main::RenderPostProcess)" },
		{ 107148, 0x163, 107014, "Engine VolumetricLighting::Descriptor (in Main::RenderPostProcess)" },
		{ 107148, 0x190, 107193, "Engine AE 107193 (in Main::RenderPostProcess)" },
		{ 107148, 0x1B0, 107206, "Engine AE 107206 (in Main::RenderPostProcess)" },
		{ 107148, 0x1BC, 106988, "Engine AE 106988 (in Main::RenderPostProcess)" },
		{ 107148, 0x1D0, 105675, "Engine ImageSpaceManager::RenderPreTonemapComposites (in Main::RenderPostProcess)" },
		{ 107148, 0x1E7, 105674, "Engine ImageSpaceManager::RenderEndOfFrameEffects (in Main::RenderPostProcess)" },
		{ 107132, 0x54, 106367, "Engine AE 106367 (in Main::CalculateAndDrawShadowCasterLights)" },
		{ 107132, 0xB6, 107601, "Engine BSShadowDirectionalLight::Select (in Main::CalculateAndDrawShadowCasterLights)" },
		{ 107132, 0xD6, 106365, "Engine AE 106365 (in Main::CalculateAndDrawShadowCasterLights)" },
		{ 107132, 0x126, 108492, "Engine AE 108492 (in Main::CalculateAndDrawShadowCasterLights)" },
		{ 107132, 0x15A, 106323, "Engine Main::AfterFullFrustum (in Main::CalculateAndDrawShadowCasterLights)" },
		{ 107132, 0x1E7, 76815, "Engine AE 76815 (in Main::CalculateAndDrawShadowCasterLights)" },
		{ 107132, 0x224, 76819, "Engine AE 76819 (in Main::CalculateAndDrawShadowCasterLights)" },
		{ 107132, 0x3B1, 107137, "Engine CalculateActiveShadowCasterLights (in Main::CalculateAndDrawShadowCasterLights)" },
		{ 107132, 0x3BD, 69353, "Engine JobList::Finish (in Main::CalculateAndDrawShadowCasterLights)" },
		{ 107132, 0x3D8, 16341, "Engine AE 16341 (in Main::CalculateAndDrawShadowCasterLights)" },
		{ 107132, 0x413, 76816, "Engine AE 76816 (in Main::CalculateAndDrawShadowCasterLights)" },
		{ 107139, 0x17B, 106437, "Engine Renderer::StartAccumulating #1 (in Main::RenderDepth)" },
		{ 107139, 0x1AA, 106610, "Engine ShaderAccumulator::RenderFirstPersonViewEnd (in Main::RenderDepth)" },
		{ 107139, 0x337, 106569, "Engine AE 106569 (in Main::RenderDepth)" },
		{ 107139, 0x3BB, 106437, "Engine Renderer::StartAccumulating #2 (in Main::RenderDepth)" },
		{ 107139, 0x535, 76551, "Engine AE 76551 (in Main::RenderDepth)" },
		{ 107139, 0x710, 106921, "Engine BSCullingProcess::Process (in Main::RenderDepth)" },
		{ 107139, 0xA09, 76552, "Engine AE 76552 (in Main::RenderDepth)" },
		{ 107129, 0x101, 106436, "Engine Renderer::SubmitAccumulator #1 (in Main::RenderFirstPersonView)" },
		{ 107129, 0x15E, 106599, "Engine AE 106599 (in Main::RenderFirstPersonView)" },
		{ 107129, 0x2D6, 106921, "Engine BSCullingProcess::Process (in Main::RenderFirstPersonView)" },
		{ 107129, 0x2E7, 106436, "Engine Renderer::SubmitAccumulator #2 (in Main::RenderFirstPersonView)" },
		{ 107129, 0x334, 16341, "Engine AE 16341 (in Main::RenderFirstPersonView)" },
		{ 107129, 0x389, 26353, "Engine Sun::SubmitOcclusionQuery (in Main::RenderFirstPersonView)" },
		{ 36560, 0x92, 32219, "Engine AE 32219 (in Main::RenderWaterEffects)" },
		{ 36560, 0xC1, 76846, "Engine AE 76846 (in Main::RenderWaterEffects)" },
		{ 36560, 0xE9, 32174, "Engine AE 32174 (in Main::RenderWaterEffects)" },
		{ 36560, 0x1B1, 76034, "Engine AE 76034 (in Main::RenderWaterEffects)" },
		{ 36560, 0x1F7, 107220, "Engine AE 107220 (in Main::RenderWaterEffects)" },
		{ 36560, 0x22A, 107221, "Engine AE 107221 (in Main::RenderWaterEffects)" },
		{ 36560, 0x242, 107217, "Engine ImageSpace::DispatchCopySubRegion (in Main::RenderWaterEffects)" },
		{ 36560, 0x258, 32047, "Engine AE 32047 #1 (in Main::RenderWaterEffects)" },
		{ 36560, 0x33C, 32047, "Engine AE 32047 #2 (in Main::RenderWaterEffects)" },
		{ 107133, 0x35, 106920, "Engine AE 106920 #1 (in Main::RenderShadowMaps)" },
		{ 107133, 0x40, 106920, "Engine AE 106920 #2 (in Main::RenderShadowMaps)" },
		{ 107133, 0x1D0, 69353, "Engine JobList::Finish (in Main::RenderShadowMaps)" },
		{ 107133, 0x23C, 106613, "Engine AE 106613 #1 (in Main::RenderShadowMaps)" },
		{ 107133, 0x24D, 106613, "Engine AE 106613 #2 (in Main::RenderShadowMaps)" },
		};

		using Fn = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);

		template <std::size_t I>
		struct Zone
		{
			static std::uintptr_t thunk(std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d, std::uintptr_t e, std::uintptr_t f)
			{
				static constexpr tracy::SourceLocationData kLocation{ kSites[I].name, kSites[I].name, __FILE__, static_cast<std::uint32_t>(I), 0 };
				tracy::ScopedZone zone(&kLocation, TracyIsConnected);
				return func(a, b, c, d, e, f);
			}
			static inline Fn func = nullptr;
		};

		struct Entry
		{
			std::uintptr_t thunk;
			Fn* func;
		};

		template <std::size_t... I>
		std::array<Entry, sizeof...(I)> MakeEntries(std::index_sequence<I...>)
		{
			return { Entry{ reinterpret_cast<std::uintptr_t>(&Zone<I>::thunk), &Zone<I>::func }... };
		}
	}

	void Install()
	{
		if (!Switches::On("CS_ENGINE_ZONES"))
			return;
		if (REL::Module::get().version() != REL::Version(1, 6, 1170, 0)) {
			logger::info("[EngineZones] CS_ENGINE_ZONES: the call sites are for AE 1.6.1170 only; this runtime is {}", REL::Module::get().version().string());
			return;
		}
		static SKSE::Trampoline trampoline("CS engine zones");
		// Placed from the end of the engine's code (the default), so every call site in it reaches the trampoline with a rel32.
		trampoline.create(std::size(kSites) * 16 + 64);
		static const auto entries = MakeEntries(std::make_index_sequence<std::size(kSites)>{});
		const auto moduleBase = REL::Module::get().base();
		const auto text = REL::Module::get().segment(REL::Segment::textx);
		const auto textBegin = text.address();
		const auto textEnd = textBegin + text.size();
		std::uint32_t written = 0;
		for (std::size_t i = 0; i < std::size(kSites); ++i) {
			const auto& site = kSites[i];
			const auto src = REL::ID(site.parent).address() + site.offset;
			if (*reinterpret_cast<const std::uint8_t*>(src) != 0xE8) {
				logger::warn("[EngineZones] {}: no CALL at AE {}+{:#x}; skipped", site.name, site.parent, site.offset);
				continue;
			}
			// The target is the engine's function, or a hook another plugin or feature wrote there first, outside the engine's code.
			const auto target = src + 5 + *reinterpret_cast<const std::int32_t*>(src + 1);
			const bool engine = target >= textBegin && target < textEnd;
			if (engine && target != REL::ID(site.callee).address()) {
				logger::warn("[EngineZones] {}: AE {}+{:#x} calls {:#x}, not AE {}; skipped", site.name, site.parent, site.offset, target - moduleBase, site.callee);
				continue;
			}
			*entries[i].func = reinterpret_cast<Fn>(trampoline.write_call<5>(src, entries[i].thunk));
			++written;
		}
		logger::info("[EngineZones] CS_ENGINE_ZONES: {} of {} engine call sites wrapped in Tracy zones", written, std::size(kSites));
	}
#else
	void Install()
	{
		if (Switches::On("CS_ENGINE_ZONES"))
			logger::info("[EngineZones] CS_ENGINE_ZONES: this build has no Tracy");
	}
#endif
}
