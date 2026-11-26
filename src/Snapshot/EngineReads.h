#pragma once

#include <atomic>
#include <cstdint>
#include <source_location>

/**
* @brief The live-read detector. CS reads engine singletons (sky, TES, player, camera, shadow state, renderer data, timers, INI)
* through CS_ENGINE_READ, which counts each read at its call site. A read inside a CaptureScope is a capture. Any other read is
* a live read: the snapshot roadmap's phase 1 removes them from native's frame. The counters are always on: one relaxed atomic
* add per read, on a site object each call site owns. CS_SNAPSHOT_DETECT=1 logs the table at exit and at each load.
*/
namespace Snapshot::EngineReads
{
	/** @brief The engine object a site reads. */
	enum class Object : std::uint8_t
	{
		Sky,                 // RE::Sky, its weather and climate
		TES,                 // RE::TES, cells and the land
		Player,              // RE::PlayerCharacter
		Camera,              // RE::PlayerCamera, the world root camera, NiCamera state
		ShadowState,         // BSGraphics::RendererShadowState
		GraphicsState,       // BSGraphics::State
		Renderer,            // BSGraphics::Renderer and its render targets
		ShaderManagerState,  // BSShaderManager::State
		Accumulator,         // the current BSShaderAccumulator
		Shaders,             // the engine's current shaders, state flags and BSUtilityShader
		Timer,               // BSTimer and the game time globals
		Calendar,            // RE::Calendar
		Ini,                 // INI and game setting collections, GetINISetting
		UI,                  // RE::UI, MenuControls, MenuTopicManager
		ImageSpace,          // ImageSpaceManager
		Water,               // TESWaterSystem
		Data,                // TESDataHandler, ProcessLists, the grass manager and other game data
		BoundState,          // GPU state the engine bound to the D3D11 context
		FrameGlobals,        // DCLF's FrameGlobals read off the render thread with no capture bound
		Count
	};

	/** @brief The roadmap's read categories, so the table can group by what phase 1 does with a site. */
	enum class Category : std::uint8_t
	{
		CapturePoint,  // in a hook that is a capture point today, scattered across the engine's render sequence
		RecordTime,    // while CS records its frame (commits, passes, feature draw code)
		BoundState,    // reads what the engine bound to the D3D11 context
		Other,         // setup, settings UI, load-time work, writes to engine state
		Count
	};

	/** @brief The frame kinds the counters split by, RenderGraph::Frame::Kind's values. */
	inline constexpr std::uint32_t kKinds = 5;
	/** @brief 0 for the thread that began the current frame, 1 for any other. */
	inline constexpr std::uint32_t kThreads = 2;

	/** @brief One call site's counters. Each CS_ENGINE_READ owns one, constant-initialised, listed at its first read. */
	struct Site
	{
		constexpr Site(Object a_object, Category a_category, std::source_location a_where) noexcept :
			where(a_where), object(a_object), category(a_category) {}
		Site(const Site&) = delete;
		Site& operator=(const Site&) = delete;

		/** @brief Counts one read: as a capture inside a CaptureScope, else as a live read by thread and frame kind. */
		void Count() noexcept;
		/** @brief Live reads at this site so far, every thread and kind. */
		std::uint64_t Live() const noexcept;

		std::source_location where;
		Object object;
		Category category;
		std::atomic<bool> listed{ false };
		Site* next = nullptr;
		std::atomic<std::uint64_t> captured{ 0 };
		std::atomic<std::uint64_t> live[kThreads][kKinds]{};
	};

	/** @brief Reads on this thread are captures while one is alive. Nests. The snapshot builder and DCLF's frame capture open one. */
	class CaptureScope
	{
	public:
		CaptureScope() noexcept;
		~CaptureScope();
		CaptureScope(const CaptureScope&) = delete;
		CaptureScope& operator=(const CaptureScope&) = delete;
	};

	/** @brief Whether this thread is inside a CaptureScope. */
	bool InCapture() noexcept;

	/** @brief Renderer::Begin, before the frame's work: a_kind is the frame's RenderGraph::Frame::Kind. Its thread is the render thread. */
	void BeginFrame(std::uint8_t a_kind) noexcept;

	/** @brief The current frame's kind, as BeginFrame last set it. */
	std::uint8_t CurrentKind() noexcept;

	/** @brief 0 on the thread that began the current frame, 1 elsewhere. */
	std::uint32_t ThreadIndex() noexcept;

	/** @brief Whether CS_SNAPSHOT_DETECT=1 asked for the detectors' tables. */
	bool Logging();

	/** @brief Logs the live-read table (under CS_SNAPSHOT_DETECT=1) and writes its summary to the test status. a_when names the moment. */
	void Report(const char* a_when);
}

/**
* @brief Reads an engine object through the detector. a_object and a_category name Snapshot::EngineReads::Object and Category
* values. Evaluates and returns the expression exactly as written.
* @code auto* sky = CS_ENGINE_READ(Sky, RecordTime, globals::game::sky);
*/
#define CS_ENGINE_READ(a_object, a_category, ...)                                                                                                                  \
	([&]() -> decltype(auto) {                                                                                                                                     \
		static constinit ::Snapshot::EngineReads::Site csEngineReadSite{ ::Snapshot::EngineReads::Object::a_object, ::Snapshot::EngineReads::Category::a_category, \
			std::source_location::current() };                                                                                                                     \
		csEngineReadSite.Count();                                                                                                                                  \
		return (__VA_ARGS__);                                                                                                                                      \
	}())

/** @brief Counts a read that has no single expression to wrap, such as a run of bound-state queries. A statement. */
#define CS_ENGINE_READ_MARK(a_object, a_category)                                                                                                                  \
	do {                                                                                                                                                           \
		static constinit ::Snapshot::EngineReads::Site csEngineReadSite{ ::Snapshot::EngineReads::Object::a_object, ::Snapshot::EngineReads::Category::a_category, \
			std::source_location::current() };                                                                                                                     \
		csEngineReadSite.Count();                                                                                                                                  \
	} while (false)
