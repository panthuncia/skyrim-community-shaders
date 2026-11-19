#include "RenderGraph/Engine/Native/OpCensus.h"

#include "RenderGraph/CensusWindow.h"
#include "RenderGraph/Switches.h"

#include <d3d11.h>

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace RenderGraph::Engine::Native::OpCensus
{
	namespace
	{
		constexpr auto kOpCount = static_cast<std::size_t>(Op::Count);

		constexpr std::array<const char*, kOpCount> kOpNames{ "Map read", "Map write", "Map write (discard)", "Map write (no overwrite)", "Map read-write",
			"Unmap upload", "UpdateSubresource buffer", "UpdateSubresource texture", "CopyResource into", "CopyResource from", "CopySubresourceRegion into",
			"CopySubresourceRegion from", "ClearRenderTargetView", "ClearDepthStencilView", "ClearUnorderedAccessViewUint", "ClearUnorderedAccessViewFloat",
			"GenerateMips" };

		/** @brief Where an operation fell in its frame. */
		struct Position
		{
			std::string label;          // "frame start", or "after <epoch> #n"
			std::uint32_t ordinal = 0;  // epochs submitted earlier in the frame
		};

		struct TargetStats
		{
			std::string name;
			std::uint32_t bindFlags = 0, usage = 0, dimension = 0;
			bool initialData = false;
			std::uint64_t count = 0;
			std::map<std::string, std::uint64_t> positions;  // only the first kMaxPositions labels
		};
		constexpr std::size_t kMaxPositions = 6;

		struct PositionStats
		{
			std::array<std::uint64_t, kOpCount> byKind{};
			std::uint64_t ordinalSum = 0;
			std::uint64_t count = 0;
		};

		struct OpWindow
		{
			std::array<std::uint64_t, kOpCount> byKind{};
			std::map<std::pair<std::uint8_t, std::uint64_t>, TargetStats> targets;
			std::unordered_map<std::string, PositionStats> positions;
			// Operations that went to a static graph pass or to ORG's external client instead.
			std::array<std::uint64_t, kOpCount> graphPasses{}, externalWork{};
		};

		/** @brief A resource made SRV-only or VB/IB-only and written after creation. */
		struct WrittenStats
		{
			std::string name;
			std::uint32_t bindFlags = 0, usage = 0, dimension = 0;
			bool initialData = false;
			std::array<std::uint64_t, kOpCount> byKind{};
			std::uint64_t count = 0;
		};

		struct WriteWindow
		{
			std::map<std::uint64_t, WrittenStats> resources;
		};

		struct Census
		{
			std::mutex lock;
			CensusWindow::PerWindow<OpWindow> ops;
			CensusWindow::PerWindow<WriteWindow> writes;
			Position position{ "frame start", 0 };
			std::unordered_map<std::string, std::uint32_t> runs;
			std::unordered_set<std::uint64_t> writtenEver;
		};

		Census& Get()
		{
			static Census census;
			return census;
		}

		const char* DimensionName(std::uint32_t a_dimension)
		{
			switch (a_dimension) {
			case D3D11_RESOURCE_DIMENSION_BUFFER:
				return "buffer";
			case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
				return "Texture1D";
			case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
				return "Texture2D";
			case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
				return "Texture3D";
			default:
				return "unknown";
			}
		}

		const char* UsageName(std::uint32_t a_usage)
		{
			switch (a_usage) {
			case D3D11_USAGE_DEFAULT:
				return "DEFAULT";
			case D3D11_USAGE_IMMUTABLE:
				return "IMMUTABLE";
			case D3D11_USAGE_DYNAMIC:
				return "DYNAMIC";
			case D3D11_USAGE_STAGING:
				return "STAGING";
			default:
				return "?";
			}
		}

		std::string BindNames(std::uint32_t a_flags)
		{
			static constexpr std::pair<std::uint32_t, const char*> kNames[] = { { D3D11_BIND_VERTEX_BUFFER, "VB" }, { D3D11_BIND_INDEX_BUFFER, "IB" },
				{ D3D11_BIND_CONSTANT_BUFFER, "CB" }, { D3D11_BIND_SHADER_RESOURCE, "SRV" }, { D3D11_BIND_STREAM_OUTPUT, "SO" },
				{ D3D11_BIND_RENDER_TARGET, "RTV" }, { D3D11_BIND_DEPTH_STENCIL, "DSV" }, { D3D11_BIND_UNORDERED_ACCESS, "UAV" } };
			std::string names;
			for (const auto& [flag, name] : kNames)
				if (a_flags & flag)
					names += fmt::format("{}{}", names.empty() ? "" : "|", name);
			return names.empty() ? std::string("none") : names;
		}

		std::string Describe(std::uint32_t a_dimension, std::uint32_t a_bindFlags, std::uint32_t a_usage, bool a_initialData)
		{
			return fmt::format("{}, bind {}, usage {}, {}", DimensionName(a_dimension), BindNames(a_bindFlags), UsageName(a_usage),
				a_initialData ? "with initial data" : "no initial data");
		}

		/** @brief Whether the write census watches a resource, a texture made SRV-only or a buffer made VB/IB-only. */
		bool LooksImmutable(const Target& a_target)
		{
			if (a_target.dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
				constexpr std::uint32_t geometry = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER;
				return a_target.bindFlags && (a_target.bindFlags & ~geometry) == 0;
			}
			return a_target.bindFlags == D3D11_BIND_SHADER_RESOURCE;
		}

		double PerFrame(std::uint64_t a_count, std::uint64_t a_frames) { return static_cast<double>(a_count) / static_cast<double>((std::max<std::uint64_t>)(a_frames, 1)); }

		std::string KindList(const std::array<std::uint64_t, kOpCount>& a_byKind, std::uint64_t a_frames)
		{
			std::string kinds;
			for (std::size_t i = 0; i < kOpCount; ++i)
				if (a_byKind[i])
					kinds += fmt::format("{}{} {:.2f}", kinds.empty() ? "" : ", ", kOpNames[i], PerFrame(a_byKind[i], a_frames));
			return kinds.empty() ? std::string("none") : kinds;
		}

		void ReportOps(CensusWindow::Kind a_window, const OpWindow& a_data, std::uint64_t a_frames)
		{
			const char* window = CensusWindow::Name(a_window);
			std::uint64_t total = 0;
			for (const auto count : a_data.byKind)
				total += count;
			logger::info("[ORG native] op census ({}, {} frames): {:.2f} operations a frame; by kind (a frame): {}", window, a_frames, PerFrame(total, a_frames),
				KindList(a_data.byKind, a_frames));
			logger::info("[ORG native] op census ({}): not on the immediate context (a frame): graph passes: {}; external work: {}", window,
				KindList(a_data.graphPasses, a_frames), KindList(a_data.externalWork, a_frames));
			if (!total)
				return;
			const std::size_t top = CensusWindow::ReportTop();
			std::vector<const std::pair<const std::pair<std::uint8_t, std::uint64_t>, TargetStats>*> targets;
			for (const auto& entry : a_data.targets)
				targets.push_back(&entry);
			std::ranges::sort(targets, [](const auto* a, const auto* b) { return a->second.count > b->second.count; });
			for (std::size_t i = 0; i < targets.size() && i < top; ++i) {
				const auto& [key, stats] = *targets[i];
				std::string positions;
				for (const auto& [label, count] : stats.positions)
					positions += fmt::format("{}{} {:.0f}%", positions.empty() ? "" : ", ", label, 100.0 * static_cast<double>(count) / static_cast<double>(stats.count));
				logger::info("[ORG native] op census ({}): target #{}: {} {:.2f} a frame on \"{}\" ({}); at: {}", window, i + 1, kOpNames[key.first],
					PerFrame(stats.count, a_frames), stats.name, Describe(stats.dimension, stats.bindFlags, stats.usage, stats.initialData), positions);
			}
			if (targets.size() > top)
				logger::info("[ORG native] op census ({}): {} more (kind, target) pairs", window, targets.size() - top);
			// Order positions by mean ordinal, which is the order one frame graph must keep.
			std::vector<std::pair<double, const std::pair<const std::string, PositionStats>*>> order;
			for (const auto& entry : a_data.positions)
				order.emplace_back(static_cast<double>(entry.second.ordinalSum) / static_cast<double>((std::max<std::uint64_t>)(entry.second.count, 1)), &entry);
			std::ranges::sort(order, [](const auto& a, const auto& b) { return a.first < b.first; });
			for (std::size_t i = 0; i < order.size(); ++i) {
				const auto& [label, stats] = *order[i].second;
				logger::info("[ORG native] op census ({}): order #{}: {} (epoch {:.1f} of the frame): {:.2f} a frame: {}", window, i + 1, label, order[i].first,
					PerFrame(stats.count, a_frames), KindList(stats.byKind, a_frames));
			}
		}

		void ReportWrites(CensusWindow::Kind a_window, const WriteWindow& a_data, std::uint64_t a_frames, std::size_t a_session)
		{
			const char* window = CensusWindow::Name(a_window);
			std::size_t textures = 0, buffers = 0, withData = 0;
			for (const auto& [id, stats] : a_data.resources) {
				(stats.dimension == D3D11_RESOURCE_DIMENSION_BUFFER ? buffers : textures) += 1;
				withData += stats.initialData ? 1 : 0;
			}
			logger::info(
				"[ORG native] write census ({}, {} frames): {} resources made SRV-only or VB/IB-only were written ({} textures, {} buffers; {} made with "
				"initial data); {} in the session so far",
				window, a_frames, a_data.resources.size(), textures, buffers, withData, a_session);
			std::vector<const WrittenStats*> sorted;
			for (const auto& [id, stats] : a_data.resources)
				sorted.push_back(&stats);
			std::ranges::sort(sorted, [](const auto* a, const auto* b) { return a->count > b->count; });
			const std::size_t top = CensusWindow::ReportTop();
			for (std::size_t i = 0; i < sorted.size() && i < top; ++i) {
				const auto& stats = *sorted[i];
				logger::info("[ORG native] write census ({}): #{} \"{}\" ({}): {} writes: {}", window, i + 1, stats.name,
					Describe(stats.dimension, stats.bindFlags, stats.usage, stats.initialData), stats.count, KindList(stats.byKind, 1));
			}
			if (sorted.size() > top)
				logger::info("[ORG native] write census ({}): {} more resources", window, sorted.size() - top);
		}
	}

	void Configure()
	{
		std::uint8_t flags = 0;
		if (Switches::On("CS_ORG_NATIVE_OP_CENSUS"))
			flags |= kOps;
		if (Switches::On("CS_ORG_NATIVE_WRITE_CENSUS"))
			flags |= kWrites;
		if (Switches::On("CS_ORG_VALIDATE_UNDECLARED"))
			flags |= kMarkWrites;
		Detail::flags.store(flags, std::memory_order_relaxed);
		flags &= kOps | kWrites;
		if (flags)
			logger::info("[ORG native] census: {}{}{}; a report per window every {} frames (CS_ORG_CENSUS_FRAMES) with the top {} (CS_ORG_CENSUS_TOP)",
				flags & kOps ? "immediate operations (CS_ORG_NATIVE_OP_CENSUS=1)" : "", flags == (kOps | kWrites) ? ", " : "",
				flags & kWrites ? "writes after creation (CS_ORG_NATIVE_WRITE_CENSUS=1)" : "", CensusWindow::ReportFrames(), CensusWindow::ReportTop());
	}

	bool IsWrite(Op a_op)
	{
		switch (a_op) {
		case Op::MapWrite:
		case Op::MapWriteDiscard:
		case Op::MapWriteNoOverwrite:
		case Op::MapReadWrite:
		case Op::UpdateBuffer:
		case Op::UpdateTexture:
		case Op::Copy:
		case Op::CopyRegion:
		case Op::ClearRenderTarget:
		case Op::ClearDepthStencil:
		case Op::ClearUavUint:
		case Op::ClearUavFloat:
		case Op::GenerateMips:
			return true;
		default:
			// A DYNAMIC texture's Unmap follows its Map for writing, which was already counted.
			return false;
		}
	}

	void Record(Op a_op, const Target& a_target)
	{
		const std::uint8_t flags = Flags();
		const auto kind = static_cast<std::size_t>(a_op);
		if (!flags || kind >= kOpCount)
			return;
		auto& census = Get();
		std::lock_guard guard(census.lock);
		if (flags & kOps) {
			auto& window = census.ops.Active();
			++window.byKind[kind];
			auto& target = window.targets[{ static_cast<std::uint8_t>(a_op), a_target.id }];
			if (!target.count) {
				target.name = a_target.name;
				target.bindFlags = a_target.bindFlags;
				target.usage = a_target.usage;
				target.dimension = a_target.dimension;
				target.initialData = a_target.initialData;
			}
			++target.count;
			if (const auto it = target.positions.find(census.position.label); it != target.positions.end())
				++it->second;
			else if (target.positions.size() < kMaxPositions)
				target.positions.emplace(census.position.label, 1);
			auto& position = window.positions[census.position.label];
			++position.byKind[kind];
			++position.count;
			position.ordinalSum += census.position.ordinal;
		}
		if ((flags & kWrites) && IsWrite(a_op) && LooksImmutable(a_target)) {
			auto& written = census.writes.Active().resources[a_target.id];
			if (!written.count) {
				written.name = a_target.name;
				written.bindFlags = a_target.bindFlags;
				written.usage = a_target.usage;
				written.dimension = a_target.dimension;
				written.initialData = a_target.initialData;
				if (census.writtenEver.insert(a_target.id).second)
					logger::info("[ORG native] write census: \"{}\" ({}) is written after creation: first by {}", a_target.name,
						Describe(a_target.dimension, a_target.bindFlags, a_target.usage, a_target.initialData), kOpNames[kind]);
			}
			++written.byKind[kind];
			++written.count;
		}
	}

	const char* Name(Op a_op)
	{
		const auto kind = static_cast<std::size_t>(a_op);
		return kind < kOpCount ? kOpNames[kind] : "unknown";
	}

	void RecordRouted(Op a_op, Route a_route)
	{
		const auto kind = static_cast<std::size_t>(a_op);
		if (!(Flags() & kOps) || kind >= kOpCount)
			return;
		auto& census = Get();
		std::lock_guard guard(census.lock);
		auto& window = census.ops.Active();
		++(a_route == Route::GraphPass ? window.graphPasses : window.externalWork)[kind];
	}

	void NoteEpoch(const char* a_label)
	{
		if (!(Flags() & kOps) || !a_label)
			return;
		auto& census = Get();
		std::lock_guard guard(census.lock);
		const std::uint32_t run = ++census.runs[a_label];
		census.position.label = fmt::format("after \"{}\" #{}", a_label, run);
		++census.position.ordinal;
	}

	void EndFrame()
	{
		const std::uint8_t flags = Flags();
		if (!flags)
			return;
		auto& census = Get();
		std::lock_guard guard(census.lock);
		census.position = { "frame start", 0 };
		census.runs.clear();
		const std::uint64_t every = CensusWindow::ReportFrames();
		if (flags & kOps)
			census.ops.EndFrame(every, [](CensusWindow::Kind a_window, const OpWindow& a_data, std::uint64_t a_frames) { ReportOps(a_window, a_data, a_frames); });
		if (flags & kWrites)
			census.writes.EndFrame(every, [&](CensusWindow::Kind a_window, const WriteWindow& a_data, std::uint64_t a_frames) {
				ReportWrites(a_window, a_data, a_frames, census.writtenEver.size());
			});
	}
}
