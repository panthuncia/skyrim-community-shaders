// Toggles: the flag table with seeds and menu entries, the packed active set, and its writes through a setting.
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Render/Direct/Direct.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Frame/Variables.h"
#include "RenderGraph/Backend.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <OpenRenderGraph/GraphHost.h>
#include <atomic>

#include <functional>

namespace DCLF
{
	namespace
	{
		using S = ToggleSet;
		constexpr std::string_view kOn = "1";

		constexpr ToggleInfo kToggles[] = {
			{ &S::plainSnowSparkle, Switch::PlainSnowSparkle, kOn, "plain snow sparkle", {}, "Sparkle on plain snow layers",
				"Glacier and ice snow layers carry a material without sparkle values, so the engine reads whatever memory follows it (in practice no "
				"sparkle). On: DCLF gives them the snow material's default sparkle. Off: no sparkle." },
		};

		// The packed word's low 32 bits: cullMode in the low two, then each flag in kToggles order.
		constexpr std::uint32_t kFirstFlagBit = 2;
		static_assert(kFirstFlagBit + std::size(kToggles) <= 32);
		// The high word holds the pipeline state: pipelineMode, interiorCpu, exteriorCpu, cpuRecord.
		constexpr std::uint32_t kPipelineModeBit = 32, kInteriorCpuBit = 33, kExteriorCpuBit = 34, kCpuRecordBit = 35;  // cpuRecord takes two bits

		constexpr std::string_view kCpuViewNames[] = { "main", "decal", "forward", "refraction", "sun", "spot", "point", "sky", "precipitation", "reflection",
			"first-person", "water" };
		static_assert(std::size(kCpuViewNames) == Direct::kViewTypes);

		/**
		 * @brief The record mode with no CS_DCLF_CPU_RECORD: indirect, which moves the driver's per-draw cost off the CPU.
		 * Its pulled pipelines find each draw through a chain of dependent loads that AMD's driver makes none of scalar.
		 * On an RX 6600 serial's colour pass was 0.5 ms of GPU time faster. CS_DCLF_CPU_RECORD=serial records direct draws again.
		 */
		Direct::RecordMode DefaultRecordMode()
		{
			return Direct::RecordMode::Indirect;
		}

		ToggleSet FromSwitches()
		{
			ToggleSet set;
			const auto& cull = SwitchValue(Switch::Cull);
			set.cullMode = cull.empty() ? DefaultCullMode() : cull == "occlusion" ? 2 : cull == "frustum" ? 1 : 0;
			if (const auto& pipeline = SwitchValue(Switch::CpuPipeline); !pipeline.empty() && !ApplyPipeline(set, pipeline))
				logger::warn("[DCLF] CS_DCLF_PIPELINE={}: not a pipeline (auto, manual, gpu, cpu); auto", pipeline);
			const auto& record = SwitchValue(Switch::CpuRecord);
			set.cpuRecord = static_cast<std::uint8_t>(record == "serial"   ? Direct::RecordMode::Serial :
													  record == "indirect" ? Direct::RecordMode::Indirect :
																			 DefaultRecordMode());
			for (const auto& toggle : kToggles) {
				const auto& value = SwitchValue(toggle.seed);
				SetToggleValue(set, toggle, value.empty() || value == toggle.onValue);
			}
			return set;
		}

		/** @brief Clamps the CPU pipeline's state to what it can be. */
		ToggleSet Normalised(ToggleSet a_set)
		{
			// Only Serial and Indirect exist. Anything else, such as the removed Parallel's 1, is Indirect.
			a_set.cpuRecord = a_set.cpuRecord == static_cast<std::uint8_t>(Direct::RecordMode::Serial) ? a_set.cpuRecord : static_cast<std::uint8_t>(Direct::RecordMode::Indirect);
			return a_set;
		}
	}

	std::uint8_t DefaultCullMode()
	{
		return 2;
	}

	bool CpuExteriorsByDefault()
	{
		// -1 until the device answers, then 0 or 1 for the session.
		static std::atomic<int> cached{ -1 };
		if (const int value = cached.load(std::memory_order_acquire); value >= 0)
			return value != 0;
		const auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return false;
		::ShaderFeatureInfo features{};
		if (!rhi::IsOk(host->Device().QueryFeatureInfo(&features.header)))
			return false;
		// Vulkan: Pascal reports unbounded tables, but without mutable descriptors BasicRHI runs it on the descriptor-sets model.
		const bool legacy = ::RenderGraph::Backend::IsD3D12() ?
		                        !features.unifiedResourceHeaps :
		                        (!features.unboundedDescriptorTables ||
									rhi::vulkan::GetBindingModel(host->Device()) == rhi::vulkan::BindingModel_DescriptorSets);
		int expected = -1;
		if (cached.compare_exchange_strong(expected, legacy ? 1 : 0, std::memory_order_acq_rel) && legacy)
			logger::info("[DCLF] the GPU has the legacy binding model: the pipeline's auto mode draws exteriors CPU-driven too (CS_DCLF_PIPELINE=gpu "
						 "for the GPU pipeline)");
		return legacy;
	}

	Direct::Mode ModeOf(const ToggleSet& a_set)
	{
		Direct::Mode mode;
		mode.record = a_set.cpuRecord == static_cast<std::uint8_t>(Direct::RecordMode::Serial) ? Direct::RecordMode::Serial : Direct::RecordMode::Indirect;
		return mode;
	}

	bool ApplyPipeline(ToggleSet& a_set, std::string_view a_value)
	{
		if (a_value == "auto" || a_value == "manual") {
			a_set.pipelineMode = a_value == "auto" ? PipelineMode::Auto : PipelineMode::Manual;
			return true;
		}
		if (a_value != "gpu" && a_value != "cpu")
			return false;
		a_set.pipelineMode = PipelineMode::Manual;
		a_set.interiorCpu = a_set.exteriorCpu = a_value == "cpu";
		return true;
	}

	std::string_view CpuViewName(std::size_t a_view)
	{
		return a_view < std::size(kCpuViewNames) ? kCpuViewNames[a_view] : std::string_view{};
	}

	std::span<const ToggleInfo> ToggleTable()
	{
		return kToggles;
	}

	Toggles& Toggles::Get()
	{
		static Toggles toggles;
		return toggles;
	}

	Toggles::Toggles()
	{
		const auto seeded = FromSwitches();
		requested = Pack(seeded);
		requested.Bind(CS::SettingRegistry::Declare("DCLF.Toggles"));
		active.store(Pack(Normalised(seeded)), std::memory_order_relaxed);
		frame.store(active.load(std::memory_order_relaxed), std::memory_order_relaxed);
		gameFrame.store(active.load(std::memory_order_relaxed), std::memory_order_relaxed);
		// The revision that carries a new requested set makes it the active one at its adoption.
		RenderGraph::Frame::Vars::OnAdopted(requested, std::function<void(const std::uint64_t&, const std::uint64_t&)>(
														   [this](const std::uint64_t&, const std::uint64_t& a_now) { ApplyAdopted(a_now); }));
	}

	std::uint64_t Toggles::Pack(const ToggleSet& a_set)
	{
		std::uint32_t bits = a_set.cullMode & 3u;
		for (std::uint32_t i = 0; i < std::size(kToggles); ++i)
			bits |= (a_set.*kToggles[i].member ? 1u : 0u) << (kFirstFlagBit + i);
		std::uint64_t word = bits;
		word |= static_cast<std::uint64_t>(a_set.pipelineMode == PipelineMode::Manual ? 1u : 0u) << kPipelineModeBit;
		word |= static_cast<std::uint64_t>(a_set.interiorCpu ? 1u : 0u) << kInteriorCpuBit;
		word |= static_cast<std::uint64_t>(a_set.exteriorCpu ? 1u : 0u) << kExteriorCpuBit;
		word |= static_cast<std::uint64_t>(a_set.cpuRecord & 3u) << kCpuRecordBit;
		return word;
	}

	ToggleSet Toggles::Unpack(std::uint64_t a_word)
	{
		const auto bits = static_cast<std::uint32_t>(a_word);
		ToggleSet set;
		set.cullMode = static_cast<std::uint8_t>(bits & 3u);
		for (std::uint32_t i = 0; i < std::size(kToggles); ++i)
			set.*kToggles[i].member = ((bits >> (kFirstFlagBit + i)) & 1u) != 0;
		set.pipelineMode = ((a_word >> kPipelineModeBit) & 1u) != 0 ? PipelineMode::Manual : PipelineMode::Auto;
		set.interiorCpu = ((a_word >> kInteriorCpuBit) & 1u) != 0;
		set.exteriorCpu = ((a_word >> kExteriorCpuBit) & 1u) != 0;
		set.cpuRecord = static_cast<std::uint8_t>((a_word >> kCpuRecordBit) & 3u);
		return set;
	}

	void Toggles::Commit(const ToggleSet& a_set, std::string_view a_writer)
	{
		// The frame driver installs before DCLF in every mode, so a commit always goes through a frame revision.
		if (!RenderGraph::Frame::Driver::Installed())
			::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} committed toggles before the frame driver was installed", a_writer));
		const auto word = Pack(a_set);
		if (word == requested.Get())
			return;
		requested = word;
		logger::info("[DCLF] toggles: {} asked for {:#x}; they apply with the frame revision that carries them", a_writer, word);
	}

	ToggleSet Toggles::Frame() const
	{
		return Unpack((Scene::FrameView::InGameHalf() ? gameFrame : frame).load(std::memory_order_relaxed));
	}

	void Toggles::InstallFrame(const ToggleSet& a_set)
	{
		(Scene::FrameView::InGameHalf() ? gameFrame : frame).store(Pack(a_set), std::memory_order_relaxed);
	}

	void Toggles::ApplyAdopted(std::uint64_t a_word)
	{
		const auto next = Normalised(Unpack(a_word));
		const auto current = Active();
		if (next == current)
			return;
		active.store(Pack(next), std::memory_order_relaxed);
		generation.fetch_add(1, std::memory_order_relaxed);
	}
}
