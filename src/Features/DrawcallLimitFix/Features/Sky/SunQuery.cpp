// Native's sun occlusion query (Main::RenderWorld's, query index 1). The render side draws it into CS's own query slot
// (SubmitSunQuery); the game side reads the answer here and writes the sun's results as Sun::UpdateOcclusionQueries does, so the
// engine's query functions are not called in native. Guest keeps the engine's functions.

#include "Features/DrawcallLimitFix/Features/Sky/Sky.h"

#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Queries/OcclusionQueries.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/Publication.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace DCLF::Features::Sky
{
	namespace
	{
		namespace Queries = ::RenderGraph::Engine::OcclusionQueries;

		// Sky::sun (SE 528089 / AE 415034 is the Sun the world render uses).
		inline constexpr Engine::Offset kSun{ { 415034 }, { 528089 } };
		// UpdateOcclusionQueries' sample weight: a visibility of 1 - min(count, 1500) * weight.
		inline constexpr Engine::Offset kSampleWeight{ { 254682 }, { 304462 } };

		// The Sun's fields: the query geometry, whether its results were read since the last submit, its accumulator.
		constexpr std::size_t kQueryGeometry = 0x28;
		constexpr std::size_t kResultsRead = 0x4C;
		constexpr std::size_t kAccumulator = 0x50;
		// The accumulator's query block: the lowest count, a result still pending, the visibility the game reads, three entries.
		constexpr std::size_t kLowestCount = 0x60;
		constexpr std::size_t kAnyPending = 0x64;
		constexpr std::size_t kVisibility = 0x68;
		constexpr std::size_t kEntries = 0x70;
		constexpr std::size_t kEntryStride = 0x18;
		constexpr std::uint32_t kMainQuery = 1;
		constexpr std::uint32_t kMaxCount = 1500;

		struct Stats
		{
			std::uint64_t answers = 0;
			std::uint64_t lagSum = 0;
			bool logged = false;
		} stats;

		/** @brief Sun::UpdateOcclusionQueries (SE 25801, AE 26355) with CS's slot as the main accumulator's query 1. Game thread. */
		void ApplyAnswers(std::byte* a_sun, std::uint64_t a_snapshot)
		{
			Engine::At<bool>(a_sun, kResultsRead) = true;
			auto* accumulator = Engine::At<std::byte*>(a_sun, kAccumulator);
			if (!accumulator)
				return;
			const bool main = accumulator == Engine::Global<std::byte*>(Engine::Address::kMainAccumulator);
			const float weight = Engine::Global<float>(kSampleWeight);
			const auto visibilityOf = [weight](std::uint32_t a_count) { return a_count ? 1.0f - static_cast<float>((std::min)(a_count, kMaxCount)) * weight : 1.0f; };
			bool settled = true;
			float best = 0.0f;
			std::uint32_t lowest = 0x7fffffff;
			for (std::uint32_t i = 0; i < 3; ++i) {
				auto* entry = accumulator + kEntries + i * kEntryStride;
				auto& visibility = Engine::At<float>(entry, 0x8);
				auto& count = Engine::At<std::uint32_t>(entry, 0xC);
				auto& waits = Engine::At<std::uint32_t>(entry, 0x10);
				auto& pending = Engine::At<bool>(entry, 0x14);
				const auto wait = [&] {
					++waits;
					settled = false;
					Engine::At<bool>(accumulator, kAnyPending) = true;
				};
				if (main && i == kMainQuery) {
					std::uint32_t answer = 0;
					std::uint64_t tag = 0;
					if (Queries::Take(Queries::kSunSlot, answer, tag)) {
						visibility = visibilityOf(answer);
						count = answer;
						pending = false;
						++stats.answers;
						stats.lagSum += a_snapshot > tag ? a_snapshot - tag : 0;
						if (!std::exchange(stats.logged, true))
							logger::info("[DCLF] sky: the first sun occlusion answer reached the game side: {} samples, drawn for snapshot {}, read at {}", answer, tag, a_snapshot);
						if (stats.answers % 1000 == 0)
							logger::info("[DCLF] sky: {} sun occlusion answers, {:.2f} snapshots late on average", stats.answers,
								static_cast<double>(stats.lagSum) / static_cast<double>(stats.answers));
					} else if (Queries::Pending(Queries::kSunSlot))
						wait();
				} else if (pending) {
					const auto answer = Engine::At<void*>(entry, 0) ? Queries::ReadRingEntry(Engine::At<void*>(entry, 0)) : Queries::kNotReady;
					if (answer != Queries::kNotReady) {
						visibility = visibilityOf(answer);
						count = answer;
						pending = false;
					} else
						wait();
				}
				if (visibility > 0.0f || count != 0) {
					best = (std::max)(best, visibility);
					lowest = (std::min)(lowest, count);
				}
			}
			if (!settled)
				return;
			Engine::At<float>(accumulator, kVisibility) = lowest < 10 ? 1.0f : best;
			Engine::At<bool>(accumulator, kAnyPending) = false;
			Engine::At<std::uint32_t>(accumulator, kLowestCount) = lowest;
			for (std::uint32_t i = 0; i < 3; ++i)
				Engine::At<std::uint64_t>(accumulator + kEntries + i * kEntryStride, 0x8) = 0;
		}

		void CaptureBlock(Snapshot::Builder& a_builder)
		{
			if (!::RenderGraph::Engine::Native::Active() || a_builder.Built().kind != Snapshot::FrameKind::World)
				return;
			auto* sun = Engine::Global<std::byte*>(kSun);
			if (!sun)
				return;
			ApplyAnswers(sun, a_builder.Built().id);
			SunQueryBlock block;
			auto* geometry = Engine::At<RE::BSGeometry*>(sun, kQueryGeometry);
			block.submit = geometry && Engine::At<bool>(sun, kResultsRead);
			if (block.submit) {
				block.geometry = Snapshot::EngineRef(geometry);
				// Sun::SubmitOcclusionQuery takes the accumulator it submits to; the render side submits to the main one.
				Engine::At<RE::NiPointer<RE::BSShaderAccumulator>>(sun, kAccumulator) = RE::NiPointer<RE::BSShaderAccumulator>(
					Engine::Global<RE::BSShaderAccumulator*>(Engine::Address::kMainAccumulator));
			}
			a_builder.Set(block);
		}
	}

	RE::BSGeometry* SunQueryGeometryNow()
	{
		auto* sun = Engine::Global<std::byte*>(kSun);
		return sun ? Engine::At<RE::BSGeometry*>(sun, kQueryGeometry) : nullptr;
	}

	void InstallSunQuery()
	{
		Snapshot::RegisterCapture(&CaptureBlock);
	}
}
