#pragma once

#include "Features/DrawcallLimitFix/Api/Settings.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Utils/Setting.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace DCLF::Direct
{
	struct Mode;
}

namespace DCLF
{
	/** @brief How the pipeline is chosen: by the cell (Auto), or per kind of cell (Manual). */
	enum class PipelineMode : std::uint8_t
	{
		Auto,    // exteriors GPU-driven, interiors CPU-driven; both CPU-driven on a legacy-binding GPU (CpuExteriorsByDefault)
		Manual,  // ToggleSet::interiorCpu, exteriorCpu
	};

	/**
	 * @brief The settings that can change while the game runs: the cull mode, the pipeline, the CPU pipeline's options and plain snow's
	 * sparkle. Each is seeded from its CS_DCLF_* switch and then edited from the menu. Unset, they default to the tested configuration.
	 * kToggles describes every flag. cullMode and the CPU pipeline's state are not table flags and are handled beside it.
	 * What DCLF draws is not a toggle: every object class a feature takes is DCLF's.
	 */
	struct ToggleSet
	{
		// How BuildDrawsCS filters the frame's draws: 0 off, 1 frustum, 2 frustum then the HZB.
		std::uint8_t cullMode = 0;

		// The pipeline. CPU-driven means draws culled on the CPU and recorded as direct draws into DXVK's command buffer.
		// Changing it changes the graph's conditions at the Begin that adopts it (plan step 10c). dclf:pipeline=auto|manual|gpu|cpu.
		PipelineMode pipelineMode = PipelineMode::Auto;
		// Manual's choice for interiors and exteriors. true means CPU-driven.
		bool interiorCpu = true;
		bool exteriorCpu = false;
		// dclf:cpu-record: the per-frame views' Direct::RecordMode, 0 Serial, 2 Indirect. 1 was Parallel and reads as Indirect.
		std::uint8_t cpuRecord = 2;

		// A multi-index snow layer with the plain material class, as on vanilla glaciers, where SetupMaterial reads SparkleParams past the material's end.
		// On, it sparkles with the Snow class's default. Off, it draws without sparkle.
		bool plainSnowSparkle = false;

		bool operator==(const ToggleSet&) const = default;
	};

	/** @brief The cull mode with no CS_DCLF_CULL: 2, frustum and occlusion, on every GPU. */
	std::uint8_t DefaultCullMode();

	/** @brief The CPU pipeline's state in a_set. Main is always per frame. */
	Direct::Mode ModeOf(const ToggleSet& a_set);

	/**
	 * @brief Whether Auto draws exteriors CPU-driven too: on a GPU with the legacy binding model (D3D12 resource heap tier 1, or
	 * Vulkan without bindless descriptors: NVIDIA Pascal and older). There the GPU pipeline's culling and pulled bindless draws cost
	 * more than the CPU pipeline (GTX 1070 Ti, native D3D12: about 2.3 ms of the frame), and its build pass lost the device in about a
	 * third of the perf runs. False until the render graph's device exists.
	 */
	bool CpuExteriorsByDefault();

	/** @brief Whether a_set draws a cell of a_interior CPU-driven. Auto does interiors on the CPU and exteriors on the GPU. */
	inline bool CpuPipelineFor(const ToggleSet& a_set, bool a_interior)
	{
		if (a_set.pipelineMode == PipelineMode::Auto)
			return a_interior || CpuExteriorsByDefault();
		return a_interior ? a_set.interiorCpu : a_set.exteriorCpu;
	}

	/** @brief Applies a pipeline value: "auto", "manual", "gpu" or "cpu". Returns false for an unknown value, leaving a_set unchanged. */
	bool ApplyPipeline(ToggleSet& a_set, std::string_view a_value);

	/** @brief A Direct::ViewType's name in toggles and reports, or empty past the last. */
	std::string_view CpuViewName(std::size_t a_view);

	/** @brief One flag of ToggleSet: its seed and menu entry. */
	struct ToggleInfo
	{
		bool ToggleSet::*member;
		Switch seed;
		std::string_view onValue;  // the seed value, besides unset, that turns it on
		std::string_view name;     // shown on the "This frame" line, and the test harness key, dclf:<name>=<0|1>
		std::string_view section;  // the menu heading this flag opens, or empty
		const char* label;
		const char* tooltip;  // or nullptr
	};

	/** @brief A flag's value in a_set. */
	inline bool ToggleValue(const ToggleSet& a_set, const ToggleInfo& a_info) { return a_set.*a_info.member; }
	/** @brief Sets a flag in a_set. */
	inline void SetToggleValue(ToggleSet& a_set, const ToggleInfo& a_info, bool a_value) { a_set.*a_info.member = a_value; }

	/** @brief Every flag in menu order. */
	std::span<const ToggleInfo> ToggleTable();

	/**
	 * @brief The requested, active and frame toggle sets (plan step 10c).
	 * The requested set is a CS::Setting, the packed word. Writers edit a copy and Commit it, which writes the setting. The frame
	 * revision that carries the new value adopts it, and the setting's adoption listener makes it the active set. The scene side commits
	 * its next publication under the active set, and the graph conditions follow it. The frame set is the installed publication's, set
	 * with the scene DCLF adopted, so a toggle never changes under a running frame and takes effect with the scene, the builds and the
	 * conditions made for it. The active and frame sets are packed 64-bit words behind relaxed atomics, because hooks on engine and DCLF
	 * threads read them with a single load. The active one is the adopted setting normalised, kept so those reads normalise nothing.
	 */
	class Toggles
	{
	public:
		static Toggles& Get();

		/** @brief Any thread. A copy of the requested set. A writer edits it and Commits it. */
		ToggleSet Requested() const { return Unpack(requested.Get()); }

		/**
		 * @brief Render or main thread, once the frame driver is installed (before is fatal). Makes a_set the requested set. Writing
		 * the same set is no event. The set applies only at the adoption of the revision that carries it.
		 */
		void Commit(const ToggleSet& a_set, std::string_view a_writer);

		/** @brief Any thread. The adopted set, which the next publication commits under. */
		ToggleSet Active() const { return Unpack(active.load(std::memory_order_relaxed)); }

		/**
		 * @brief Any thread. The installed publication's set, or the active one before the first. Inside native's game half, the set
		 * its frame start installed; anywhere else, the render side's (its adopted ticket's).
		 */
		ToggleSet Frame() const;

		/**
		 * @brief When DCLF installs the publication of the scene it adopted: native's game half at its frame start, the render side when
		 * it adopts the ticket's installed frame, guest at its frame start. a_set becomes that side's frame set.
		 */
		void InstallFrame(const ToggleSet& a_set);

		/** @brief How many times the active set has changed. A cheap witness for caches. */
		std::uint32_t Generation() const { return generation.load(std::memory_order_relaxed); }

	private:
		Toggles();

		/** @brief Inside the adoption point of a revision that changed the requested setting. a_word, normalised, becomes the active set. */
		void ApplyAdopted(std::uint64_t a_word);

		static std::uint64_t Pack(const ToggleSet& a_set);
		static ToggleSet Unpack(std::uint64_t a_word);

		CS::Setting<std::uint64_t> requested;  // the packed requested set, as writers left it (not normalised)
		std::atomic<std::uint64_t> active{ 0 };
		std::atomic<std::uint64_t> frame{ 0 };
		std::atomic<std::uint64_t> gameFrame{ 0 };  // native's game half
		std::atomic<std::uint32_t> generation{ 0 };
	};

	/**
	 * @brief A thread's toggle snapshot that ActiveToggles reads while installed.
	 * A scene pass reads the set its frame inputs carried on every thread it runs on, even if the render thread changes the active set meanwhile.
	 */
	inline thread_local const ToggleSet* activeToggleSnapshot = nullptr;

	/** @brief Any thread. The frame's toggles, the installed publication's. */
	inline ToggleSet FrameToggles() { return Toggles::Get().Frame(); }

	/** @brief Any thread. The newest toggles, for scene-side producers that run outside a scene pass. */
	inline ToggleSet SceneToggles() { return Toggles::Get().Active(); }

	/** @brief Any thread. The toggles in force here: this thread's snapshot inside a scene pass, otherwise the frame's. */
	inline ToggleSet ActiveToggles() { return activeToggleSnapshot ? *activeToggleSnapshot : FrameToggles(); }

	/** @brief Makes a_set this thread's ActiveToggles while the scope lives. Null means the frame's set. */
	class ToggleSnapshotScope
	{
	public:
		explicit ToggleSnapshotScope(const ToggleSet* a_set) :
			previous(std::exchange(activeToggleSnapshot, a_set)) {}
		~ToggleSnapshotScope() { activeToggleSnapshot = previous; }
		ToggleSnapshotScope(const ToggleSnapshotScope&) = delete;
		ToggleSnapshotScope& operator=(const ToggleSnapshotScope&) = delete;

	private:
		const ToggleSet* previous;
	};
}
