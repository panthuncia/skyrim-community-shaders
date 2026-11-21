#include "RenderGraph/Frame/Transients.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/RenderGraph/FrameAliasArena.h>
#include <Resources/PixelBuffer.h>

#include <atomic>

namespace RenderGraph::Frame::Transients
{
	namespace
	{
		using Recipes::StageId;

		constexpr std::uint32_t Pos(StageId a_stage) { return static_cast<std::uint32_t>(a_stage); }

		struct State
		{
			std::atomic<bool> on{ false };
			org::GraphHost* host = nullptr;
		};

		State& Get()
		{
			static State state;
			return state;
		}

		Span FromAlias(org::AliasSpan a_span) { return { a_span.first, a_span.last }; }
	}

	std::uint32_t PositionOf(SegmentId a_segment)
	{
		switch (a_segment) {
		case SegmentId::Start:
			return Pos(StageId::Start);
		case SegmentId::PreDepth:
			return Pos(StageId::PreDepth);
		case SegmentId::PreMain:
			return Pos(StageId::PreMain);
		case SegmentId::Deferred:
			return Pos(StageId::Deferred);
		case SegmentId::Sun:
			return Pos(StageId::Sun);
		case SegmentId::Post:
			return Pos(StageId::Post);
		case SegmentId::Ui:
		default:
			return Pos(StageId::Ui);
		}
	}

	std::string Describe(Span a_span)
	{
		if (a_span.Empty())
			return "none (nothing uses it yet)";
		const auto name = [](std::uint32_t a_position) -> std::string { return Recipes::StageName(static_cast<StageId>(a_position)); };
		return a_span.first == a_span.last ? name(a_span.first) : fmt::format("{} to {}", name(a_span.first), name(a_span.last));
	}

	bool On() { return Get().on.load(std::memory_order_acquire); }

	bool Install(org::GraphHost& a_host)
	{
		// Every world recipe epoch gets a position, DCLF's too, since they bind the G-buffer from the Z-prepass to first person.
		// Only CS's stages put their own transients in the arena, since DCLF's epochs run a varying number of times a frame.
		std::vector<org::GraphHost::FramePosition> positions;
		std::string list;
		for (const auto& entry : Recipes::PositionedEpochs()) {
			const bool own = entry.owner == Recipes::Owner::Cs;
			positions.push_back({ entry.epoch, Pos(entry.stage), own });
			list += fmt::format("{}{} (epoch {}, position {}{})", list.empty() ? "" : ", ", Recipes::StageName(entry.stage), entry.epoch, Pos(entry.stage),
				own ? "" : ", shared arena");
		}
		a_host.SetFramePositions(std::move(positions));
		auto& state = Get();
		state.host = &a_host;
		state.on.store(true, std::memory_order_release);
		logger::info("[ORG] frame arena: epochs by their place in the frame (CS stages' transients share one arena by it; every graph counts for "
					 "the frame transients' derived spans): {}",
			list);
		return true;
	}

	std::shared_ptr<const void> Place(std::string a_label, std::span<const std::shared_ptr<org::PixelBuffer>> a_textures)
	{
		auto& state = Get();
		if (!On() || !state.host)
			return nullptr;
		return state.host->AddFrameTransients(a_textures, std::move(a_label));
	}

	std::optional<std::pair<std::string, Span>> FrameTransientOf(std::string_view a_name)
	{
		auto& state = Get();
		if (!On() || !state.host)
			return std::nullopt;
		const auto found = state.host->FrameTransientOf(a_name);
		if (!found)
			return std::nullopt;
		return std::pair{ found->label, FromAlias(found->span) };
	}
}

