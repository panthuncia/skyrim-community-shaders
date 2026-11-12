#include "Features/DrawcallLimitFix/Diagnostics/RefTrace.h"

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "RenderGraph/BufferRefs.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/ShaderTarget.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Resources/Buffers/Buffer.h>
#include <rhi.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace DCLF::Diagnostics::RefTrace
{
	namespace
	{
		// Common/DeviceRef.hlsli: kDeviceRefTraceEntries entries of four words (key, ref, hits).
		constexpr std::uint32_t kEntries = 1024;
		constexpr std::uint32_t kEntryWords = 4;
		constexpr std::uint32_t kLinesPerReport = 48;

		std::shared_ptr<org::Buffer> buffer;  // the render thread's, kept for the process
		std::atomic<std::uint32_t> slot{ 0 };
		std::atomic<volatile std::uint32_t*> words{ nullptr };
		std::atomic<std::uint64_t> traced{ 0 }, untraced{ 0 };
		bool refused = false;

		// The render thread's: each entry's hits as last logged, and the source lines a site's line can be.
		std::vector<std::uint32_t> loggedHits(kEntries, 0);
		std::map<std::uint32_t, std::string> sites;
		bool sitesRead = false;
		std::chrono::steady_clock::time_point lastReport{};

		bool On()
		{
			static const bool on = SwitchEnabled(Switch::RefTrace);
			return on;
		}

		// The lines of the shader sources with a load or store through a ref, by line number: a site is its line, and a line number can
		// be in more than one file. A pulled Lighting build's own lines can sit a few lines later than the file's (PulledSource adds lines);
		// the headers' are exact.
		void ReadSites()
		{
			sitesRead = true;
			const std::filesystem::path root = RenderGraphRuntime::kShaderDirectory;
			std::error_code error;
			for (std::filesystem::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
				const auto extension = it->path().extension().string();
				if (!it->is_regular_file(error) || (extension != ".hlsl" && extension != ".hlsli"))
					continue;
				std::ifstream file(it->path());
				std::string text;
				for (std::uint32_t line = 1; std::getline(file, text); ++line) {
					if ((text.find("DeviceLoad(") == std::string::npos && text.find("DeviceStore(") == std::string::npos && text.find("DEVICE_REF_TRACE_ZERO_INDEX(") == std::string::npos) ||
						text.find("#") != std::string::npos)
						continue;
					auto& entry = sites[line];
					entry += (entry.empty() ? "" : " | ") + std::filesystem::relative(it->path(), root, error).generic_string() + ":" + std::to_string(line);
				}
			}
		}

		const char* StageName(std::uint32_t a_stage) { return a_stage == 0 ? "VS" : a_stage == 1 ? "PS" : "other stage"; }
		const char* KindName(std::uint32_t a_kind)
		{
			return a_kind == 1 ? "NULL ref" : a_kind == 2 ? "ODD slot" : a_kind == 3 ? "ZERO descriptor index (register, row offset)" : "ref";
		}
	}

	void Ensure()
	{
		if (buffer || refused || !On())
			return;
		if (!::RenderGraph::ShaderTarget::IsSpirv()) {
			refused = true;
			logger::warn("[DCLF] ref trace (CS_DCLF_REF_TRACE): Vulkan only (a host-visible UAV buffer); off on D3D12");
			return;
		}
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !host->Graph())
			return;
		buffer = org::Buffer::CreateShared(rhi::HeapType::HostVisibleCoherent, std::uint64_t(kEntries) * kEntryWords * sizeof(std::uint32_t), true);
		buffer->SetName("cs.dclf.ref-trace");
		auto resource = buffer->GetAPIResource();
		void* mapped = nullptr;
		resource.Map(&mapped);
		const RenderGraph::BufferRef made = RenderGraph::BufferRefOf(host->Device(), resource.GetHandle());
		if (!mapped || !made) {
			refused = true;
			logger::warn("[DCLF] ref trace (CS_DCLF_REF_TRACE): the table has no {}; off", mapped ? "buffer view ref" : "mapping");
			return;
		}
		auto* mappedWords = static_cast<volatile std::uint32_t*>(mapped);
		for (std::uint32_t i = 0; i < kEntries * kEntryWords; ++i)
			mappedWords[i] = 0;
		words.store(mappedWords, std::memory_order_release);
		slot.store(made.viewSlot, std::memory_order_release);
		logger::info("[DCLF] ref trace (CS_DCLF_REF_TRACE=1): table at view slot {}; DCLF stages requested from now on record their refs "
					 "(a pixel or vertex in 64, one lane a wave); logged once a second as \"ref trace:\" lines",
			made.viewSlot);
	}

	std::uint32_t Slot()
	{
		return slot.load(std::memory_order_acquire);
	}

	void NoteRequest(bool a_traced)
	{
		(a_traced ? traced : untraced).fetch_add(1, std::memory_order_relaxed);
	}

	void Report()
	{
		auto* table = words.load(std::memory_order_acquire);
		if (!table)
			return;
		const auto now = std::chrono::steady_clock::now();
		if (now - lastReport < std::chrono::seconds(1))
			return;
		lastReport = now;
		if (!sitesRead)
			ReadSites();
		std::uint32_t lines = 0, entries = 0, nullEntries = 0;
		std::uint64_t nullHits = 0;
		for (std::uint32_t e = 0; e < kEntries; ++e) {
			const std::uint32_t key = table[e * kEntryWords];
			if (key == 0)
				continue;
			++entries;
			const std::uint32_t refSlot = table[e * kEntryWords + 1], refOffset = table[e * kEntryWords + 2], hits = table[e * kEntryWords + 3];
			const std::uint32_t line = key >> 8, store = (key >> 6) & 1, stage = (key >> 4) & 3, kind = key & 0xF;
			if (kind != 0) {
				++nullEntries;
				nullHits += hits;
			}
			// A ref with a slot is logged once, as a sample; a null or odd one whenever its hits grow.
			const bool grew = hits != loggedHits[e] && (kind != 0 || loggedHits[e] == 0);
			if (!grew || lines >= kLinesPerReport)
				continue;
			++lines;
			const auto site = sites.find(line);
			logger::info("[DCLF] ref trace: {} {} line {} ({}) {} ({}, {}) hits {} (+{})", StageName(stage), store ? "store" : "load", line,
				site != sites.end() ? site->second : std::string("no source line"), KindName(kind), refSlot, refOffset, hits, hits - loggedHits[e]);
			loggedHits[e] = hits;
		}
		if (lines == 0)
			return;
		logger::info("[DCLF] ref trace: {} sites, {} of them null or odd ({} hits); stage requests traced {}, untraced (before the table) {}", entries, nullEntries,
			nullHits, traced.load(std::memory_order_relaxed), untraced.load(std::memory_order_relaxed));
	}
}
