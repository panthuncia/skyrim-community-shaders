// The render state of CS's points inside native's Main::RenderWorld, from CS's frame. See WorldPass.h.

#include "RenderGraph/Engine/Native/WorldPass.h"

#include "Deferred.h"


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
					return s;
				}
			case Point::WaterSurface:
				{
					auto s = Base(kTest);
					s.colours = { kMain, -1, -1 };
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

	RenderWorldScope::RenderWorldScope() { inRenderWorld = true; }
	RenderWorldScope::~RenderWorldScope() { inRenderWorld = false; }
}
