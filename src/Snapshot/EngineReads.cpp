#include "Snapshot/EngineReads.h"

#include "RenderGraph/Frame/FrameDescription.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Switches.h"
#include "Testing/Testing.h"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace Snapshot::EngineReads
{
	static_assert(kKinds == static_cast<std::uint32_t>(RenderGraph::Frame::Kind::Count), "the counters split by RenderGraph::Frame::Kind");

	namespace
	{
		constexpr std::uint32_t kWorld = static_cast<std::uint32_t>(RenderGraph::Frame::Kind::World);

		thread_local std::uint32_t t_captureDepth = 0;
		thread_local std::uint32_t t_uiDepth = 0;
		std::atomic<Site*> g_sites{ nullptr };
		std::atomic<std::uint32_t> g_renderThread{ 0 };
		// Before the first Begin the driver describes a main-menu frame.
		std::atomic<std::uint8_t> g_kind{ static_cast<std::uint8_t>(RenderGraph::Frame::Kind::MainMenu) };
		std::array<std::atomic<std::uint64_t>, kKinds> g_frames{};

		constexpr std::array<const char*, static_cast<std::size_t>(Object::Count)> kObjectNames{ "Sky", "TES", "Player", "Camera", "ShadowState", "GraphicsState",
			"Renderer", "ShaderManagerState", "Accumulator", "Shaders", "Timer", "Calendar", "Ini", "UI", "ImageSpace", "Water", "Data", "BoundState", "FrameGlobals",
			"ShadowLights" };
		constexpr std::array<const char*, static_cast<std::size_t>(Category::Count)> kCategoryNames{ "capture-point", "record-time", "bound-state", "other" };
		constexpr std::array<const char*, kKinds> kKindKeys{ "world", "frozen_world", "main_menu", "loading", "bink" };

		void List(Site& a_site) noexcept
		{
			if (a_site.listed.exchange(true, std::memory_order_relaxed))
				return;
			Site* head = g_sites.load(std::memory_order_relaxed);
			do
				a_site.next = head;
			while (!g_sites.compare_exchange_weak(head, &a_site, std::memory_order_release, std::memory_order_relaxed));
		}

		/** @brief The path below src/, with forward slashes. */
		std::string SourcePath(const char* a_file)
		{
			std::string path(a_file);
			std::replace(path.begin(), path.end(), '\\', '/');
			const auto at = path.rfind("/src/");
			return at == std::string::npos ? path : path.substr(at + 5);
		}

		/** @brief The feature a source path belongs to: Features/<Name>, the first directory, or the file's stem at src/'s top. */
		std::string FeatureOf(const std::string& a_path)
		{
			std::string_view rest(a_path);
			if (rest.starts_with("Features/"))
				rest.remove_prefix(9);
			const auto slash = rest.find('/');
			if (slash != std::string_view::npos)
				return std::string(rest.substr(0, slash));
			const auto dot = rest.rfind('.');
			return std::string(rest.substr(0, dot));
		}

		struct Totals
		{
			std::uint64_t live = 0, liveRender = 0, world = 0, captured = 0, ui = 0;
			std::array<std::uint64_t, static_cast<std::size_t>(Category::Count)> worldBy{};
		};
	}

	void Site::Count() noexcept
	{
		if (t_captureDepth)
			captured.fetch_add(1, std::memory_order_relaxed);
		else {
			live[ThreadIndex()][g_kind.load(std::memory_order_relaxed)].fetch_add(1, std::memory_order_relaxed);
			if (t_uiDepth)
				ui.fetch_add(1, std::memory_order_relaxed);
		}
		if (!listed.load(std::memory_order_relaxed))
			List(*this);
	}

	std::uint64_t Site::Live() const noexcept
	{
		std::uint64_t total = 0;
		for (const auto& thread : live)
			for (const auto& count : thread)
				total += count.load(std::memory_order_relaxed);
		return total;
	}

	CaptureScope::CaptureScope() noexcept
	{
		++t_captureDepth;
	}

	CaptureScope::~CaptureScope()
	{
		--t_captureDepth;
	}

	bool InCapture() noexcept
	{
		return t_captureDepth != 0;
	}

	void EnterUiFrame() noexcept
	{
		++t_uiDepth;
	}

	void LeaveUiFrame() noexcept
	{
		--t_uiDepth;
	}

	bool InUiFrame() noexcept
	{
		return t_uiDepth != 0;
	}

	void BeginFrame(std::uint8_t a_kind) noexcept
	{
		g_renderThread.store(::GetCurrentThreadId(), std::memory_order_relaxed);
		const auto previous = g_kind.exchange(a_kind, std::memory_order_relaxed);
		g_frames[a_kind].fetch_add(1, std::memory_order_relaxed);
		// The first world frame after a loading screen: a load's table.
		if (a_kind == kWorld && previous == static_cast<std::uint8_t>(RenderGraph::Frame::Kind::Loading))
			Report("after a load");
	}

	std::uint8_t CurrentKind() noexcept
	{
		return g_kind.load(std::memory_order_relaxed);
	}

	std::uint32_t ThreadIndex() noexcept
	{
		return ::GetCurrentThreadId() == g_renderThread.load(std::memory_order_relaxed) ? 0u : 1u;
	}

	bool Logging()
	{
		static const bool on = Switches::On("CS_SNAPSHOT_DETECT");
		return on;
	}

	void Report(const char* a_when)
	{
		std::array<std::uint64_t, kKinds> frames{};
		for (std::uint32_t k = 0; k < kKinds; ++k)
			frames[k] = g_frames[k].load(std::memory_order_relaxed);
		const double worldFrames = static_cast<double>(std::max<std::uint64_t>(frames[kWorld], 1));

		struct Row
		{
			const Site* site;
			std::string path;
			std::uint64_t live, world;
		};
		std::vector<Row> rows;
		Totals all;
		std::map<std::string, Totals> byFeature;
		std::array<Totals, static_cast<std::size_t>(Object::Count)> byObject{};
		for (const Site* site = g_sites.load(std::memory_order_acquire); site; site = site->next) {
			Row row{ site, SourcePath(site->where.file_name()), site->Live(), 0 };
			for (const auto& thread : site->live)
				row.world += thread[kWorld].load(std::memory_order_relaxed);
			std::uint64_t render = 0;
			for (const auto& count : site->live[0])
				render += count.load(std::memory_order_relaxed);
			const auto captured = site->captured.load(std::memory_order_relaxed);
			const auto ui = site->ui.load(std::memory_order_relaxed);
			for (Totals* t : { &all, &byFeature[FeatureOf(row.path)], &byObject[static_cast<std::size_t>(site->object)] }) {
				t->live += row.live;
				t->ui += ui;
				t->liveRender += render;
				t->world += row.world;
				t->captured += captured;
				t->worldBy[static_cast<std::size_t>(site->category)] += row.world;
			}
			rows.push_back(std::move(row));
		}
		std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.world != b.world ? a.world > b.world : a.live > b.live; });

		const auto categories = [&](const Totals& a_t) {
			std::string text;
			for (std::size_t c = 0; c < a_t.worldBy.size(); ++c)
				if (a_t.worldBy[c])
					text += fmt::format("{}{} {:.1f}", text.empty() ? "" : ", ", kCategoryNames[c], a_t.worldBy[c] / worldFrames);
			return text.empty() ? std::string("none") : text;
		};

		nlohmann::json features = nlohmann::json::object();
		for (const auto& [name, t] : byFeature)
			if (t.live)
				features[name] = { { "live", t.live }, { "world_per_frame", t.world / worldFrames } };
		nlohmann::json byCategory = nlohmann::json::object();
		for (std::size_t c = 0; c < all.worldBy.size(); ++c)
			byCategory[kCategoryNames[c]] = all.worldBy[c] / worldFrames;
		nlohmann::json kinds = nlohmann::json::object();
		for (std::uint32_t k = 0; k < kKinds; ++k)
			kinds[kKindKeys[k]] = frames[k];
		::Testing::SetReport("engine_reads", { { "mode", RenderGraph::Frame::Driver::ModeName() }, { "frames", kinds }, { "sites", rows.size() }, { "live", all.live },
												 { "captured", all.captured }, { "ui_frame", all.ui }, { "world_per_frame", all.world / worldFrames }, { "world_per_frame_by_category", byCategory },
												 { "features", features } });

		if (!Logging())
			return;
		logger::info(
			"[Snapshot] live engine reads {} ({}; frames: world {}, frozen world {}, main menu {}, loading {}, bink {}): {} live reads at {} sites, {} on the render "
			"thread; {} captured; per world frame {:.1f} ({})",
			a_when, RenderGraph::Frame::Driver::ModeName(), frames[0], frames[1], frames[2], frames[3], frames[4], all.live, rows.size(), all.liveRender, all.captured,
			all.world / worldFrames, categories(all));
		logger::info("[Snapshot] UI-frame engine reads {}: {} live reads inside the ImGui frame", a_when, all.ui);
		for (const auto& row : rows)
			if (const auto n = row.site->ui.load(std::memory_order_relaxed))
				logger::info("[Snapshot] UI-frame read site: {}:{} {}: {}", row.path, row.site->where.line(), kObjectNames[static_cast<std::size_t>(row.site->object)], n);
		for (const auto& [name, t] : byFeature)
			if (t.live)
				logger::info("[Snapshot] live reads by feature: {}: {} live ({} on the render thread), per world frame {:.1f} ({}); {} captured", name, t.live, t.liveRender,
					t.world / worldFrames, categories(t), t.captured);
		for (std::size_t o = 0; o < byObject.size(); ++o)
			if (byObject[o].live)
				logger::info("[Snapshot] live reads by object: {}: {} live, per world frame {:.1f} ({})", kObjectNames[o], byObject[o].live, byObject[o].world / worldFrames,
					categories(byObject[o]));
		for (const auto& row : rows) {
			if (!row.live)
				continue;
			std::string split;
			for (std::uint32_t th = 0; th < kThreads; ++th)
				for (std::uint32_t k = 0; k < kKinds; ++k)
					if (const auto n = row.site->live[th][k].load(std::memory_order_relaxed))
						split += fmt::format(" {}.{}={}", th ? "other" : "render", kKindKeys[k], n);
			logger::info("[Snapshot] live read site: {}:{} {} {}: per world frame {:.2f};{}", row.path, row.site->where.line(), kObjectNames[static_cast<std::size_t>(row.site->object)],
				kCategoryNames[static_cast<std::size_t>(row.site->category)], row.world / worldFrames, split);
		}
	}
}
