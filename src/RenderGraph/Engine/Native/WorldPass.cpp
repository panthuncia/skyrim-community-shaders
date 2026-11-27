// The render state of CS's points inside native's Main::RenderWorld, from CS's frame. See WorldPass.h.

#include "RenderGraph/Engine/Native/WorldPass.h"

#include "Deferred.h"
#include "RenderGraph/Engine/Native/EngineAccess.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Native/RenderScene.h"
#include "Snapshot/Publication.h"


namespace RenderGraph::Engine::Native::WorldPass
{
	namespace
	{
		using T = RE::RENDER_TARGETS;
		constexpr auto kMain = static_cast<std::int32_t>(T::kMAIN);
		constexpr auto kMotion = static_cast<std::int32_t>(T::kMOTION_VECTOR);
		constexpr auto kMainDepth = static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
		// Depth modes: 1 test, 3 test and write.
		constexpr std::uint32_t kTest = 1, kTestWrite = 3;

		thread_local bool inRenderWorld = false;
		thread_local bool inRenderDepth = false;
		thread_local bool shiftWindow = false;
		bool rawWater = false;
		std::int32_t taaForm = 0;
		thread_local bool inScope = false;
		thread_local Point current = Point::Count;
		thread_local State state;

		State Base(std::uint32_t a_depthMode)
		{
			State s;
			s.depth = kMainDepth;
			s.depthMode = a_depthMode;
			s.stencilRef = 255;
			s.cull = 1;
			s.write = 1;
			return s;
		}

		/** @brief The state at a_point, as the engine's resolve functions leave it there. */
		State Of(Point a_point)
		{
			switch (a_point) {
			case Point::SkyBatch:
				{
					// Inside CS's deferred pass (Deferred::StartDeferred binds its G-buffer from slot 2); the pre-resolve turns the alpha
					// test on (reference 128/255) before the sky techniques.
					auto s = Base(kTestWrite);
					s.colours = { kMain, kMotion, static_cast<std::int32_t>(NORMALROUGHNESS) };
					s.alphaTest = true;
					return s;
				}
			case Point::WaterMask:
				{
					// The pre-resolve binds the normals target and the water mask (kMOTION_VECTOR's slot holds 7) before the mask batch.
					auto s = Base(kTestWrite);
					s.colours = { NormalsTarget(), kMotion, -1 };
					s.shifted = true;
					return s;
				}
			case Point::WaterSurface:
				{
					// The whole post-resolve is one depth shift window.
					auto s = Base(kTest);
					s.colours = { kMain, -1, -1 };
					s.shifted = true;
					return s;
				}
			case Point::SunQuery:
				{
					auto s = Base(kTestWrite);
					s.colours = { kMain, -1, -1 };
					return s;
				}
			default:
				return {};
			}
		}
	}

	Scope::Scope(Point a_point) :
		previous(current)
	{
		if (!inRenderWorld)
			return;
		set = true;
		inScope = true;
		current = a_point;
		state = Of(a_point);
	}

	Scope::~Scope()
	{
		if (!set)
			return;
		current = previous;
		inScope = current != Point::Count;
		if (inScope)
			state = Of(current);
	}

	const State* Current()
	{
		return inScope ? &state : nullptr;
	}

	std::int32_t NormalsTarget()
	{
		// Native's driver took it in its game half: the render half reads no engine state.
		if (const auto& prepared = Scene::Current(); prepared.valid)
			return prepared.normalsTarget;
		return EngineNormalsTarget();
	}

	std::int32_t EngineNormalsTarget()
	{
		// ImageSpaceManager+0x1c0 is the reflections pass; +0x10 whether it runs. Its swap target alternates with the frame counter.
		static REL::Relocation<const std::uint32_t*> frameCounter{ REL::RelocationID(525008, 411489) };
		auto* manager = RE::ImageSpaceManager::GetSingleton();
		const auto reflections = manager ? *reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(manager) + 0x1c0) : 0;
		const bool ssr = reflections && *reinterpret_cast<const bool*>(reflections + 0x10);
		return static_cast<std::int32_t>(ssr ? ((~*frameCounter.get() & 1u) | 4u) : 4u);
	}

	bool InRenderWorld()
	{
		return inRenderWorld;
	}

	std::array<RE::RENDER_TARGET, 4> ForwardTargets()
	{
		return { T::kMAIN, T::kMOTION_VECTOR, static_cast<RE::RENDER_TARGET>(NormalsTarget()), T::kNONE };
	}

	namespace
	{
		using Access::Global;

		/** @brief The depth shift's test (SE 100407 / AE 107125) and the maximum it sets, on the game thread. */
		void CaptureBlock(Snapshot::Builder& a_builder)
		{
			if (!Native::Active() || a_builder.Built().kind != Snapshot::FrameKind::World)
				return;
			Block block;
			// The local map open, or no flag 528095 / 415040; a world scene graph; ToggleDepthShift on.
			const bool mapMenu = Global<bool>(528005, 414950);
			const bool flag = Global<bool>(528095, 415040);
			const auto* sceneGraph = Global<const void*>(528087, 415032);
			const bool toggle = Global<bool>(513435, 391260);
			block.depthShift = (mapMenu || !flag) && sceneGraph && toggle;
			block.shiftedMax = (std::min)(1.0f, Global<float>(304859, 254902));
			if (block.depthShift) {
				// SceneGraph::GetFarDistance and GetNearDistance (vtable +0x1f0 and +0x1f8), as the shift calls them.
				using Distance = float (*)(const void*);
				const auto* vtable = *static_cast<const std::uintptr_t* const*>(sceneGraph);
				block.farDistance = reinterpret_cast<Distance>(vtable[0x1f0 / 8])(sceneGraph);
				block.nearDistance = reinterpret_cast<Distance>(vtable[0x1f8 / 8])(sceneGraph);
				block.shiftScale = Global<float>(304860, 254903);
			}
			a_builder.Set(block);
		}
	}

	void InstallCapture()
	{
		Snapshot::RegisterCapture(&CaptureBlock);
	}

	std::array<float, 2> DepthRange(bool a_shifted)
	{
		const auto* block = Snapshot::CurrentBlock<Block>();
		return { 0.0f, a_shifted && block && block->depthShift ? block->shiftedMax : 1.0f };
	}

	std::array<float, 2> CameraPlanes(bool a_shifted)
	{
		const auto& snapshot = Snapshot::Current();
		if (!snapshot)
			return { 0.0f, 0.0f };
		const auto* block = snapshot->Block<Block>();
		if (a_shifted && block && block->depthShift) {
			// In the engine's order: near * far over (near - far) * scale + far.
			const float n = block->nearDistance, f = block->farDistance;
			return { n, (n * f) / ((n - f) * block->shiftScale + f) };
		}
		const auto& frustum = snapshot->cameras.Current(Snapshot::ViewId::Main).frustum;
		return { frustum.fNear, frustum.fFar };
	}

	bool WaterBlendRuns(std::uint32_t a_flags)
	{
		// ImageSpaceManager +0x1f0 is TAA's helper: +0x38 its water history. Its form is CS's (TaaForm), 1 after the water mask point.
		const auto* manager = RE::ImageSpaceManager::GetSingleton();
		const auto taa = manager ? *reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(manager) + 0x1f0) : 0;
		if (!taa || *reinterpret_cast<const bool*>(taa + 0x38) || taaForm != 1)
			return false;
		if ((a_flags & 0x280) || (a_flags & 10))
			return false;
		// 1 or 2 under water (TESWaterSystem's state). At 1 the blend runs only with the camera above the water's height (+0xc0).
		const auto underwater = Global<std::int32_t>(513267, 391008);
		if (underwater == 2)
			return false;
		if (underwater == 1) {
			const auto* water = RE::TESWaterSystem::GetSingleton();
			const auto& snapshot = Snapshot::Current();
			if (!water || !snapshot)
				return false;
			const float height = *reinterpret_cast<const float*>(reinterpret_cast<std::uintptr_t>(water) + 0xc0);
			if (snapshot->cameras.Current(Snapshot::ViewId::Main).position.z <= height)
				return false;
		}
		// The local map's passes.
		return !Global<bool>(527793, 414746);
	}

	ShiftWindow::ShiftWindow() { shiftWindow = true; }
	ShiftWindow::~ShiftWindow() { shiftWindow = false; }

	bool InShiftWindow()
	{
		return shiftWindow;
	}

	std::optional<std::array<float, 2>> CurrentCameraPlanes()
	{
		if (!inRenderWorld)
			return std::nullopt;
		return CameraPlanes(shiftWindow);
	}

	void SetRawWater(bool a_raw)
	{
		rawWater = a_raw;
	}

	std::int32_t TaaForm()
	{
		return taaForm;
	}

	void SetTaaForm(std::int32_t a_form)
	{
		taaForm = a_form;
	}

	bool RawWater()
	{
		return rawWater;
	}

	bool InRenderDepth()
	{
		return inRenderDepth;
	}

	RenderDepthScope::RenderDepthScope() { inRenderDepth = true; }
	RenderDepthScope::~RenderDepthScope() { inRenderDepth = false; }

	RenderWorldScope::RenderWorldScope() { inRenderWorld = true; }
	RenderWorldScope::~RenderWorldScope() { inRenderWorld = false; }
}
