#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/RunGate.h"

#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunShapes.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <OpenRenderGraph/Runtime.h>
#	include <Render/LatchBlock.h>
#	include <Render/RunRef.h>

#	include <algorithm>
#	include <cstring>
#	include <mutex>
#	include <unordered_set>
#	include <vector>
#	include <vector>

namespace RenderGraph
{
	std::shared_ptr<RunGate> RunGate::Create(std::string_view a_name, std::uint32_t a_entries)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !a_entries) {
			logger::warn("[ORG] run gate {}: not made (no render graph host, or no entries)", a_name);
			return nullptr;
		}
		// Closed in every run until a commit opens an entry: a run's region is zeroed when it is allocated.
		auto latch = org::LatchBlock::Create(std::string(a_name), a_entries * kEntryBytes);
		return std::make_shared<RunGate>(std::move(latch), a_entries);
	}

	namespace
	{
		// Every live gate, so a stage's commit can move each one's record of its staging run (CopyOpenAll).
		std::mutex& GatesMutex()
		{
			static auto* mutex = new std::mutex;
			return *mutex;
		}
		std::vector<const RunGate*>& Gates()
		{
			static auto* gates = new std::vector<const RunGate*>;
			return *gates;
		}
		// By lease index: the gates whose record a commit opened an entry of there since the index's last ClearOpenAll, so a stage's
		// commit copies only those. Under GatesMutex.
		std::vector<std::unordered_set<const RunGate*>>& Touched()
		{
			static auto* touched = new std::vector<std::unordered_set<const RunGate*>>;
			return *touched;
		}
		void NoteTouched(const RunGate* a_gate, std::uint32_t a_lease)
		{
			const std::lock_guard lock(GatesMutex());
			auto& touched = Touched();
			if (touched.size() <= a_lease)
				touched.resize(static_cast<std::size_t>(a_lease) + 1);
			touched[a_lease].insert(a_gate);
		}
	}

	RunGate::RunGate(std::shared_ptr<org::LatchBlock> a_latch, std::uint32_t a_entries) :
		latch(std::move(a_latch)), entries(a_entries), words((a_entries + 63) / 64)
	{
		open = std::make_unique<std::atomic<std::uint64_t>[]>(static_cast<std::size_t>(org::Runtime::kMaxSlots) * words);
		const std::lock_guard lock(GatesMutex());
		Gates().push_back(this);
	}

	RunGate::~RunGate()
	{
		const std::lock_guard lock(GatesMutex());
		std::erase(Gates(), this);
		for (auto& touched : Touched())
			touched.erase(this);
	}

	void RunGate::CopyOpenAll(const org::RunRef& a_from, const org::RunRef& a_to)
	{
		if (a_from.lease >= org::Runtime::kMaxSlots || a_to.lease >= org::Runtime::kMaxSlots || a_from.lease == a_to.lease)
			return;
		const std::lock_guard lock(GatesMutex());
		auto& touched = Touched();
		const auto size = static_cast<std::size_t>((std::max)(a_from.lease, a_to.lease)) + 1;
		if (touched.size() < size)
			touched.resize(size);
		// A gate the target's last run had open and the source does not is copied closed; then the target's list is the source's.
		for (const auto* gate : touched[a_to.lease])
			if (!touched[a_from.lease].contains(gate))
				gate->CopyOpen(a_from, a_to);
		for (const auto* gate : touched[a_from.lease])
			gate->CopyOpen(a_from, a_to);
		touched[a_to.lease] = touched[a_from.lease];
	}

	void RunGate::ClearOpenAll(const org::RunRef& a_run)
	{
		if (a_run.lease >= org::Runtime::kMaxSlots)
			return;
		const std::lock_guard lock(GatesMutex());
		auto& touched = Touched();
		if (a_run.lease >= touched.size())
			return;
		for (const auto* gate : touched[a_run.lease])
			for (std::uint32_t i = 0; i < gate->words; ++i)
				gate->open[static_cast<std::size_t>(a_run.lease) * gate->words + i].store(0, std::memory_order_release);
		touched[a_run.lease].clear();
	}

	org::RunRef RunGate::Current()
	{
		return RenderGraphRuntime::Get().Host()->CurrentRun();
	}

	void RunGate::Write(const org::RunRef& a_run, std::uint32_t a_entry, const std::array<std::uint32_t, 4>& a_words, bool a_work) const
	{
		if (a_entry >= entries)
			return;
		const auto& run = a_run;
		latch->Write(run, Offset(a_entry), std::as_bytes(std::span(a_words)));
		if (run.lease >= org::Runtime::kMaxSlots)
			return;
		auto& word = open[static_cast<std::size_t>(run.lease) * words + a_entry / 64];
		const std::uint64_t bit = std::uint64_t{ 1 } << (a_entry % 64);
		if (a_work) {
			if ((word.fetch_or(bit, std::memory_order_release) & bit) == 0)
				NoteTouched(this, run.lease);
			RunShapes::NoteOpen(this, a_entry);
		} else {
			word.fetch_and(~bit, std::memory_order_release);
		}
	}

	void RunGate::Close() const
	{
		Close(Current());
	}

	void RunGate::Close(const org::RunRef& a_run) const
	{
		const auto& run = a_run;
		const auto bytes = latch->Bytes(run);
		std::memset(bytes.data(), 0, static_cast<std::size_t>(entries) * kEntryBytes);
		if (run.lease >= org::Runtime::kMaxSlots)
			return;
		for (std::uint32_t i = 0; i < words; ++i)
			open[static_cast<std::size_t>(run.lease) * words + i].store(0, std::memory_order_release);
	}

	void RunGate::CopyOpen(const org::RunRef& a_from, const org::RunRef& a_to) const
	{
		if (a_from.lease >= org::Runtime::kMaxSlots || a_to.lease >= org::Runtime::kMaxSlots || a_from.lease == a_to.lease)
			return;
		for (std::uint32_t i = 0; i < words; ++i)
			open[static_cast<std::size_t>(a_to.lease) * words + i].store(open[static_cast<std::size_t>(a_from.lease) * words + i].load(std::memory_order_acquire),
				std::memory_order_release);
	}

	bool RunGate::IsOpen(const org::RunRef& a_run, std::uint32_t a_entry) const
	{
		if (a_entry >= entries || a_run.lease >= org::Runtime::kMaxSlots)
			return true;
		const auto word = open[static_cast<std::size_t>(a_run.lease) * words + a_entry / 64].load(std::memory_order_acquire);
		return ((word >> (a_entry % 64)) & 1) != 0;
	}

	void RunGate::OpenDispatch(std::uint32_t a_entry, std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z) const
	{
		OpenDispatch(Current(), a_entry, a_x, a_y, a_z);
	}

	void RunGate::OpenDraw(std::uint32_t a_entry, std::uint32_t a_vertices, std::uint32_t a_instances) const
	{
		OpenDraw(Current(), a_entry, a_vertices, a_instances);
	}

	void RunGate::OpenDispatch(const org::RunRef& a_run, std::uint32_t a_entry, std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z) const
	{
		Write(a_run, a_entry, { a_x, a_y, a_z, 0 }, a_x && a_y && a_z);
	}

	void RunGate::OpenDraw(const org::RunRef& a_run, std::uint32_t a_entry, std::uint32_t a_vertices, std::uint32_t a_instances) const
	{
		Write(a_run, a_entry, { a_vertices, a_instances, 0, 0 }, a_vertices && a_instances);
	}

	void RunGate::Dispatch(org::RecordingContext& a_recording, const ComputeProgram& a_program, std::span<const std::uint32_t> a_constants, std::uint32_t a_entry) const
	{
		if (a_entry < entries)
			a_program.DispatchIndirect(a_recording, a_constants, *latch, Offset(a_entry));
	}
}

#endif
