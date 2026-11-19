#pragma once

#include "Features/DrawcallLimitFix/Api/Settings.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

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
		Auto,    // exteriors GPU-driven, interiors CPU-driven
		Manual,  // ToggleSet::interiorCpu, exteriorCpu
	};

	/**
	 * @brief The feature switches that can change while the game runs, for live A/B comparisons.
	 * Each is seeded from its CS_DCLF_* switch and then edited from the menu. Unset, they default to the tested configuration.
	 * kToggles describes every Core flag. cullMode and the CPU pipeline's state are not table flags and are handled beside it.
	 */
	struct ToggleSet
	{
		// How BuildDrawsCS filters the frame's draws: 0 off, 1 frustum, 2 frustum then the HZB.
		std::uint8_t cullMode = 0;

		// The pipeline. CPU-driven means draws culled on the CPU and recorded as direct draws into DXVK's command buffer.
		// Changing it changes the graph's conditions from the next build point. dclf:pipeline=auto|manual|gpu|cpu.
		PipelineMode pipelineMode = PipelineMode::Auto;
		// Manual's choice for interiors and exteriors. true means CPU-driven.
		bool interiorCpu = true;
		bool exteriorCpu = false;
		// dclf:cpu-record: the per-frame views' Direct::RecordMode, 0 Serial, 1 Parallel, 2 Indirect.
		std::uint8_t cpuRecord = 2;
		// dclf:cpu-cadence-<view>: a bit per Direct::ViewType for views culled and recorded once per snapshot. Never set for Main.
		std::uint16_t cpuCadence = 0;

		// The object classes. kProjectedUV objects, for snow and moss projection.
		bool projectedUv = false;
		// A multi-index snow layer with the plain material class, as on vanilla glaciers, where SetupMaterial reads SparkleParams past the material's end.
		// On, it sparkles with the Snow class's default. Off, it draws without sparkle.
		bool plainSnowSparkle = false;

		// The shadow views are drawn by the render graph.
		bool shadows = false;

		// The features' toggles, a bit per Api::ToggleId.
		std::uint32_t registered = 0;
		/** @brief Whether a feature's toggle is on. False for none. */
		bool On(Api::ToggleId a_toggle) const { return a_toggle && ((registered >> a_toggle.bit) & 1u) != 0; }

		bool operator==(const ToggleSet&) const = default;
	};

	/** @brief The cull mode with no CS_DCLF_CULL: 2, frustum and occlusion, on every GPU. */
	std::uint8_t DefaultCullMode();

	/** @brief The CPU pipeline's state in a_set. Main is always per frame. */
	Direct::Mode ModeOf(const ToggleSet& a_set);

	/** @brief Whether a_set draws a cell of a_interior CPU-driven. Auto does interiors on the CPU and exteriors on the GPU. */
	inline bool CpuPipelineFor(const ToggleSet& a_set, bool a_interior)
	{
		if (a_set.pipelineMode == PipelineMode::Auto)
			return a_interior;
		return a_interior ? a_set.interiorCpu : a_set.exteriorCpu;
	}

	/** @brief Applies a pipeline value: "auto", "manual", "gpu" or "cpu". Returns false for an unknown value, leaving a_set unchanged. */
	bool ApplyPipeline(ToggleSet& a_set, std::string_view a_value);

	/** @brief A Direct::ViewType's name in toggles and reports, or empty past the last. */
	std::string_view CpuViewName(std::size_t a_view);

	/** @brief The Direct::ViewType named a_name, or the view type count for none. */
	std::size_t CpuViewByName(std::string_view a_name);

	/**
	 * @brief Sets view cadences from a_spec: "frame" or "snapshot" for every view, or "<view>=frame|snapshot,...". Main stays per frame.
	 * Returns false for an unknown name or value, after applying the rest.
	 */
	bool ApplyCpuCadence(ToggleSet& a_set, std::string_view a_spec);

	/** @brief One flag of ToggleSet: its seed, menu entry and needs. A feature's flag has no member, only a registered bit and seed. */
	struct ToggleInfo
	{
		bool ToggleSet::*member;
		Switch seed;
		std::string_view onValue;  // the seed value, besides unset, that turns it on
		std::string_view name;     // shown on the "This frame" line
		std::string_view section;  // the menu heading this flag opens, or empty
		const char* label;
		const char* tooltip;       // or nullptr
		bool entersClassification;  // a change drops the classification caches
		bool ToggleSet::*needs[2];  // flags it needs on, earlier in kToggles
		bool offByDefault = false;  // unset seeds it off
		Api::ToggleId registered{};       // a feature's toggle bit. member is null then.
		Api::SwitchId registeredSeed{};   // a feature's seed switch
		Api::ToggleId registeredNeeds[2]{};  // features' toggles it needs on, earlier in the table
		// A Core flag's needs by name, which may be a feature's toggle. SeedRegistered resolves each into needs or registeredNeeds at the same index.
		std::string_view needNames[2]{};
		bool needsMissing = false;  // a named need no feature registered, so the flag never takes effect
	};

	/** @brief A flag's value in a_set. */
	inline bool ToggleValue(const ToggleSet& a_set, const ToggleInfo& a_info)
	{
		return a_info.member ? a_set.*a_info.member : a_set.On(a_info.registered);
	}
	/** @brief Sets a flag in a_set. */
	inline void SetToggleValue(ToggleSet& a_set, const ToggleInfo& a_info, bool a_value)
	{
		if (a_info.member)
			a_set.*a_info.member = a_value;
		else if (a_info.registered)
			a_set.registered = a_value ? a_set.registered | (1u << a_info.registered.bit) : a_set.registered & ~(1u << a_info.registered.bit);
	}

	/** @brief Every flag in menu order, each after the flags it needs. Features' toggles are placed in it at install. Read-only after. */
	std::span<const ToggleInfo> ToggleTable();

	/**
	 * @brief Main thread, at install. Registers a feature's toggle and takes its bit now. It is placed in the table once every feature registered.
	 * Fails the install past the word's bits, for a taken name, or for both an after and a before.
	 */
	Api::ToggleId RegisterToggle(const Api::FeatureToggle& a_toggle, std::string_view a_feature);
	/** @brief Any thread after install. A feature's toggle by name, or none. */
	Api::ToggleId FindToggle(std::string_view a_name);

	/**
	 * @brief The requested, active and frame toggle sets. The menu edits the requested set, and the render thread copies it to the active set
	 * once per frame. The scene side commits its next publication under the active set. The frame set is the installed publication's,
	 * so a toggle never changes under a running frame and only takes effect once a publication is committed under it.
	 * Each set is one packed 64-bit word behind a relaxed atomic, because hooks on engine and DCLF threads read it with a single load.
	 * Changing a toggle that enters classification invalidates every cached verdict, because the caches witness objects, not switches.
	 */
	class Toggles
	{
	public:
		static Toggles& Get();

		/** @brief Render thread. The set the menu edits. */
		ToggleSet& Requested() { return requested; }

		/** @brief Any thread. The newest applied set, which the next publication commits under. */
		ToggleSet Active() const { return Unpack(active.load(std::memory_order_relaxed)); }

		/** @brief Any thread. The installed publication's set, or the active one before the first. */
		ToggleSet Frame() const { return Unpack(frame.load(std::memory_order_relaxed)); }

		/** @brief Render thread, at the frame's start. a_set, from an installed publication, becomes the frame set. */
		void Adopt(const ToggleSet& a_set) { frame.store(Pack(a_set), std::memory_order_relaxed); }

		/**
		 * @brief Render thread, once per frame before anything reads the toggles. Applies the requested set.
		 * @return Whether a toggle that enters classification changed, so the caches must be dropped.
		 */
		bool BeginFrame();

		/** @brief How many times the active set has changed. A cheap witness for caches. */
		std::uint32_t Generation() const { return generation.load(std::memory_order_relaxed); }

		/** @brief Whether a requested flag would take effect, because every flag it needs is on. */
		bool Editable(const ToggleInfo& a_info) const;

		/**
		 * @brief Main thread, after the features' install. Places the features' toggles by their anchors, resolves all needs, then seeds them.
		 * Fails the install for a missing or looping anchor, or a need not listed before its toggle.
		 */
		void SeedRegistered();

	private:
		Toggles();

		static std::uint64_t Pack(const ToggleSet& a_set);
		static ToggleSet Unpack(std::uint64_t a_word);

		ToggleSet requested;
		std::atomic<std::uint64_t> active{ 0 };
		std::atomic<std::uint64_t> frame{ 0 };
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
