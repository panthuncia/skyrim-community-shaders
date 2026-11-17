#include "RenderGraph/Frame/Transients.h"

#if defined(CS_HAS_RENDER_GRAPH)

#	include "RenderGraph/Switches.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/FrameAliasArena.h>
#	include <Resources/PixelBuffer.h>

#	include <atomic>
#	include <map>
#	include <mutex>

namespace RenderGraph::Frame::Transients
{
	namespace
	{
		using Recipes::StageId;

		constexpr std::uint32_t Pos(StageId a_stage) { return static_cast<std::uint32_t>(a_stage); }

		struct GroupInfo
		{
			Group group;
			const char* name;
			Span span;
			const char* uses;  // who uses it, first to last: why the span is what it is
		};

		// The groups several stages use. A span covers every stage whose graph declares the resource, not only those that need its
		// content: a stage that binds it as a render target writes it, so nothing else may hold those bytes then.
		constexpr std::array<GroupInfo, static_cast<std::size_t>(Group::Count)> kGroups{ {
			{ Group::GBuffer, "Deferred's G-buffer", { Pos(StageId::Depth), Pos(StageId::FirstPerson) },
				"DCLF's Z-prepass binds it; pre-main clears it; DCLF's opaque pass draws it; deferred+water reads it (SSGI, subsurface, the "
				"composite, its last read); DCLF's forward and first-person passes bind it" },
		} };

		struct State
		{
			std::atomic<bool> on{ false };
			org::GraphHost* host = nullptr;
			std::mutex mutex;
			std::map<std::string, Group, std::less<>> names;  // placed resources by graph name
		};

		State& Get()
		{
			static State state;
			return state;
		}
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
			return Pos(StageId::Ui);
		default:
			return kMenuUiPosition;
		}
	}

	std::string Describe(Span a_span)
	{
		const auto name = [](std::uint32_t a_position) -> std::string {
			return a_position == kMenuUiPosition ? "menu UI" : Recipes::StageName(static_cast<StageId>(a_position));
		};
		return a_span.first == a_span.last ? name(a_span.first) : fmt::format("{} to {}", name(a_span.first), name(a_span.last));
	}

	const char* NameOf(Group a_group) { return kGroups[static_cast<std::size_t>(a_group)].name; }

	Span SpanOf(Group a_group) { return kGroups[static_cast<std::size_t>(a_group)].span; }

	bool On() { return Get().on.load(std::memory_order_acquire); }

	bool Install(org::GraphHost& a_host, std::span<const std::pair<SegmentId, std::uint32_t>> a_epochs)
	{
		if (Switches::Off("CS_ORG_FRAME_ARENA")) {
			logger::info("[ORG] frame arena off (CS_ORG_FRAME_ARENA=0): each CS stage keeps its transients in pools of its own");
			return false;
		}
		std::vector<std::pair<std::uint32_t, org::AliasSpan>> spans;
		std::string list;
		for (const auto& [segment, epoch] : a_epochs) {
			const auto position = PositionOf(segment);
			spans.push_back({ epoch, org::AliasSpan{ position, position } });
			list += fmt::format("{}{} (epoch {}, position {})", list.empty() ? "" : ", ", SegmentName(segment), epoch, position);
		}
		a_host.SetFrameAliasSpans(std::move(spans));
		auto& state = Get();
		state.host = &a_host;
		state.on.store(true, std::memory_order_release);
		logger::info("[ORG] frame arena: the CS stages' transients share one arena by their place in the frame: {}", list);
		for (const auto& group : kGroups)
			logger::info("[ORG] frame arena: group {} spans {}: {}", group.name, Describe(group.span), group.uses);
		return true;
	}

	std::shared_ptr<const void> Place(Group a_group, std::span<const std::shared_ptr<org::PixelBuffer>> a_textures)
	{
		auto& state = Get();
		if (!On() || !state.host)
			return nullptr;
		const auto arena = state.host->FrameArena();
		if (!arena)
			return nullptr;
		const auto span = SpanOf(a_group);
		auto placed = arena->ReserveTextures(org::AliasSpan{ span.first, span.last }, a_textures, NameOf(a_group));
		std::lock_guard lock(state.mutex);
		for (const auto& texture : a_textures)
			if (texture)
				state.names[texture->GetName()] = a_group;
		return placed;
	}

	std::optional<std::pair<Group, Span>> GroupOf(std::string_view a_name)
	{
		auto& state = Get();
		std::lock_guard lock(state.mutex);
		const auto found = state.names.find(a_name);
		if (found == state.names.end())
			return std::nullopt;
		return std::pair{ found->second, SpanOf(found->second) };
	}

	void LogStats()
	{
		auto& state = Get();
		if (!On() || !state.host)
			return;
		const auto arena = state.host->FrameArena();
		if (!arena)
			return;
		const auto blocks = arena->Blocks();
		logger::info("[ORG] frame arena: {} blocks, {:.1f} MiB", blocks.size(), arena->Bytes() / (1024.0 * 1024.0));
		for (const auto& block : blocks) {
			std::string ranges;
			for (const auto& range : block.ranges)
				ranges += fmt::format("{}{} [{:.1f}, {:.1f}) MiB {}", ranges.empty() ? "" : "; ", range.name, range.begin / (1024.0 * 1024.0),
					range.end / (1024.0 * 1024.0), Describe({ range.span.first, range.span.last }));
			logger::info("[ORG] frame arena: block {:#x} class {} {:.1f} MiB: {}", block.poolID, block.resourceClass, block.capacity / (1024.0 * 1024.0), ranges);
		}
	}
}

#else

namespace RenderGraph::Frame::Transients
{
	std::uint32_t PositionOf(SegmentId) { return 0; }
	std::string Describe(Span) { return {}; }
	const char* NameOf(Group) { return ""; }
	Span SpanOf(Group) { return {}; }
	bool On() { return false; }
	bool Install(org::GraphHost&, std::span<const std::pair<SegmentId, std::uint32_t>>) { return false; }
	std::shared_ptr<const void> Place(Group, std::span<const std::shared_ptr<org::PixelBuffer>>) { return nullptr; }
	std::optional<std::pair<Group, Span>> GroupOf(std::string_view) { return std::nullopt; }
	void LogStats() {}
}

#endif
