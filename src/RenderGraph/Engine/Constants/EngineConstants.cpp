// The per-use capture of the engine's constant buffers (EngineConstants.h, docs/development/p5/constants.md).

#include "RenderGraph/Engine/Constants/EngineConstants.h"

#include "RenderGraph/Switches.h"

namespace RenderGraph::Engine::Constants
{
	bool Requested()
	{
		static const bool requested = Switches::On("CS_ORG_ENGINE_CONSTANTS");
		return requested;
	}
}

#if defined(CS_HAS_RENDER_GRAPH)

#	include "Globals.h"
#	include "RenderGraph/Engine/Constants/BufferContents.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "Utils/D3D.h"

#	include <Render/LatchBlock.h>

#	include <d3d11_1.h>

#	include <algorithm>
#	include <array>
#	include <bit>
#	include <cstring>
#	include <deque>
#	include <exception>
#	include <mutex>
#	include <optional>
#	include <unordered_map>
#	include <utility>
#	include <vector>
#	include <winrt/base.h>

namespace RenderGraph::Engine::Constants
{
	namespace
	{
		// One ring chunk. A slot addresses at most 64 KiB (kMaxConstantBytes), so every window fits.
		constexpr std::uint32_t kChunkBytes = 1u << 20;
		// What one slot can address: D3D11's 4096 constants.
		constexpr std::uint32_t kMaxConstantBytes = 4096 * 16;
		// A slot's bytes start on a float4: a shader loads them with Load4.
		constexpr std::uint32_t kAlignment = 16;
		// The buffer index: open addressing, a power of two. The engine has 146 constant buffers; CS and other plugins a few dozen.
		constexpr std::uint32_t kIndexSize = 1u << 14;
		// A frame that never ends (the main thread stopped presenting) stops capturing here rather than growing without bound.
		constexpr std::uint32_t kMaxUsesPerFrame = 1u << 20;
		// The custom buffers are 0xf00 bytes; the mapper's size argument bounds a use (MapCustomConstantBuffer).
		constexpr std::uint32_t kCustomBytes = 0xf00;

		/**
		 * @brief The engine's constant-buffer globals, as CreateConstantBufferPools (SE 75592 `0x140d720d0`, AE 77400 `0x140e4d000`)
		 * lays them out: one block of 146 pointers from SE 524753 `0x143027a08` / AE 411369 `0x143288308`. Slot 0 of each pool is
		 * never created (a size class of zero).
		 */
		struct EngineSpan
		{
			std::uint32_t first, count;
			Group group;
		};
		constexpr std::array<EngineSpan, 11> kEngineSpans{ {
			{ 0, 4, Group::Custom },           // +0x000, 4 x 0xf00
			{ 4, 1, Group::AlphaTest },        // +0x020, 0x10
			{ 5, 20, Group::VSTechnique },     // +0x028, 0x10 * i
			{ 25, 10, Group::VSMaterial },     // +0x0c8
			{ 35, 28, Group::VSGeometry },     // +0x118
			{ 63, 20, Group::PSTechnique },    // +0x1f8
			{ 83, 20, Group::PSMaterial },     // +0x298
			{ 103, 40, Group::PSGeometry },    // +0x338
			{ 143, 1, Group::Compute },        // +0x478, 0x240
			{ 144, 1, Group::PerFrame },       // +0x480, 0x2d0
			{ 145, 1, Group::InstanceGroup },  // +0x488, 0x10
		} };
		constexpr std::uint32_t kEngineBlockSlots = 146;

		ID3D11Buffer* const* EngineBlock()
		{
			static REL::Relocation<ID3D11Buffer**> block{ REL::RelocationID(524753, 411369) };
			return block.get();
		}

		/** @brief One constant buffer the capture has seen. Entries are never freed: a released buffer's address may come back. */
		struct Entry
		{
			ID3D11Buffer* buffer = nullptr;  // not referenced: identity only
			std::uint32_t byteWidth = 0;
			std::uint32_t written = 0;  // what the last Map wrote: the custom mapper's bound, else byteWidth
			Group group = Group::Other;
			std::uint8_t poolIndex = 0;
			BufferContents contents;

			// The last copy into the ring.
			std::uint64_t copiedFrame = UINT64_MAX;
			std::uint32_t copiedVersion = 0;
			std::uint32_t copiedBegin = 0;
			std::uint32_t copiedSize = 0;
			std::uint16_t copiedChunk = 0;
			std::uint32_t copiedOffset = 0;
		};

		/** @brief A bound slot: its buffer and, for SetConstantBuffers1, its window (size 0: the whole buffer). */
		struct Binding
		{
			Entry* entry = nullptr;
			std::uint32_t begin = 0;
			std::uint32_t size = 0;
		};

		struct UseRecord
		{
			std::uint32_t first = 0;
			std::uint32_t count = 0;
		};

		/** @brief Counters over the log window. */
		struct Window
		{
			std::uint64_t frames = 0, uses = 0, slots = 0, copies = 0, bytes = 0, unwritten = 0, dropped = 0;
			std::uint64_t peakBytes = 0;
			std::uint32_t peakChunks = 0;
			std::array<std::uint64_t, kGroups> copiesByGroup{};
			std::uint64_t checks = 0, mismatches = 0;
		};

		// The custom mapper's size bound for the Map it is about to make on this thread (0: none).
		thread_local std::uint32_t customBound = 0;

		/**
		 * @brief The capture. The hooks run on the thread that drives the immediate context, which Renderer::Lock serializes; uses and
		 * the frame's end only on the main thread. A retired chunk comes back from ReleaseAfterD3D11's service through its own lock.
		 */
		class Tracker
		{
		public:
			static Tracker& Get()
			{
				// Never destroyed: retired chunks may come back during shutdown.
				static Tracker* tracker = new Tracker;
				return *tracker;
			}

			bool Enabled() const { return enabled; }

			bool Active() const { return enabled && !failed && RenderGraphRuntime::Get().IsActive(); }

			void Enable() { enabled = true; }

			// The writes.

			void OnMap(ID3D11Resource* a_resource, D3D11_MAP a_mapType, void* a_data)
			{
				if (a_mapType == D3D11_MAP_READ || !a_data)
					return;
				Entry* entry = Track(a_resource);
				if (!entry)
					return;
				const std::uint32_t customBytes = std::exchange(customBound, 0);
				entry->written = customBytes ? (std::min)(customBytes, entry->byteWidth) : entry->byteWidth;
				entry->contents.OnMap(a_data);
			}

			void OnUnmap(ID3D11Resource* a_resource)
			{
				if (Entry* entry = Find(a_resource))
					entry->contents.OnUnmap();
			}

			void OnUpdate(ID3D11Resource* a_resource, const D3D11_BOX* a_box, const void* a_data)
			{
				if (Entry* entry = Track(a_resource)) {
					entry->written = entry->byteWidth;
					entry->contents.OnUpdate(a_box, a_data, entry->byteWidth);
				}
			}

			// The binds.

			void Bind(Stage a_stage, UINT a_start, UINT a_count, ID3D11Buffer* const* a_buffers, const UINT* a_first, const UINT* a_constants)
			{
				auto& table = bindings[static_cast<std::uint32_t>(a_stage)];
				auto& mask = bound[static_cast<std::uint32_t>(a_stage)];
				for (UINT i = 0; i < a_count && a_start + i < kSlots; ++i) {
					const UINT slot = a_start + i;
					ID3D11Buffer* buffer = a_buffers ? a_buffers[i] : nullptr;
					Binding binding;
					binding.entry = buffer ? Track(buffer) : nullptr;
					if (binding.entry && a_first && a_constants) {
						binding.begin = a_first[i] * 16;
						binding.size = a_constants[i] * 16;
					}
					table[slot] = binding;
					if (binding.entry)
						mask |= 1u << slot;
					else
						mask &= ~(1u << slot);
				}
			}

			void Unbind()
			{
				bindings = {};
				bound = {};
			}

			// The uses.

			UseId Capture(std::uint32_t a_stageMask)
			{
				if (!Active() || !FrameAnchors::OnMainThread())
					return {};
				if (uses.size() >= kMaxUsesPerFrame) {
					++window.dropped;
					return {};
				}
				UseRecord record{ static_cast<std::uint32_t>(slots.size()), 0 };
				for (std::uint32_t stage = 0; stage < kStages; ++stage) {
					if (!(a_stageMask & (1u << stage)))
						continue;
					for (std::uint32_t mask = bound[stage]; mask; mask &= mask - 1) {
						const auto slot = static_cast<std::uint32_t>(std::countr_zero(mask));
						const Binding& binding = bindings[stage][slot];
						auto captured = CaptureSlot(*binding.entry, binding, static_cast<Stage>(stage), slot);
						if (!captured) {
							if (failed) {
								slots.resize(record.first);
								return {};
							}
							continue;
						}
						slots.push_back(*captured);
						++record.count;
						if (checkRequested)
							MaybeCheck(*binding.entry, *captured);
					}
				}
				uses.push_back(record);
				++window.uses;
				current = { frame, static_cast<std::uint32_t>(uses.size() - 1) };
				return current;
			}

			UseId Current() const { return current.frame == frame ? current : UseId{}; }

			std::span<const SlotConstants> Slots(UseId a_use) const
			{
				if (!a_use.Valid() || a_use.frame != frame || a_use.index >= uses.size())
					return {};
				const UseRecord& record = uses[a_use.index];
				return { slots.data() + record.first, record.count };
			}

			std::span<const std::byte> Bytes(const SlotConstants& a_slot) const
			{
				if (a_slot.chunk >= frameChunks.size())
					return {};
				return { frameChunks[a_slot.chunk]->Slot(0).data() + a_slot.offset, a_slot.size };
			}

			Location Where(const SlotConstants& a_slot) const
			{
				if (a_slot.chunk >= frameChunks.size())
					return {};
				const auto& block = frameChunks[a_slot.chunk];
				return { block, block->SrvIndex(), a_slot.offset, a_slot.size };
			}

			void EndFrame()
			{
				if (!enabled || !FrameAnchors::OnMainThread())
					return;
				Retire();
				window.peakChunks = (std::max)(window.peakChunks, peakChunksThisFrame);
				window.peakBytes = (std::max)(window.peakBytes, bytesThisFrame);
				peakChunksThisFrame = 0;
				bytesThisFrame = 0;
				uses.clear();
				slots.clear();
				current = {};
				++frame;
				++totals.frames;
				++window.frames;
				if (window.frames >= logFrames)
					LogWindow();
			}

			Stats GetStats() const
			{
				Stats stats = totals;
				stats.chunks = chunksMade;
				return stats;
			}

			void ReadSwitches()
			{
				checkRequested = Switches::On("CS_ORG_ENGINE_CONSTANTS_CHECK");
				logFrames = (std::max<std::uint64_t>)(1, Switches::Number("CS_ORG_ENGINE_CONSTANTS_LOG_FRAMES", 600));
			}

		private:
			Tracker() = default;

			// The buffer index.

			static std::uint32_t Hash(const void* a_pointer)
			{
				const auto value = reinterpret_cast<std::uintptr_t>(a_pointer) >> 4;
				return static_cast<std::uint32_t>((value * 0x9E3779B97F4A7C15ull) >> 40) & (kIndexSize - 1);
			}

			Entry* Find(const void* a_resource)
			{
				for (std::uint32_t at = Hash(a_resource), probes = 0; probes < kIndexSize; at = (at + 1) & (kIndexSize - 1), ++probes) {
					Entry* entry = index[at];
					if (!entry)
						return nullptr;
					if (entry->buffer == a_resource)
						return entry;
				}
				return nullptr;
			}

			/** @brief The entry of a constant buffer, made the first time it is seen; null for any other resource. Refreshes the size. */
			Entry* Track(ID3D11Resource* a_resource)
			{
				if (!a_resource)
					return nullptr;
				// A released buffer's address can come back as another resource: the type and size are read again every time.
				D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
				a_resource->GetType(&dimension);
				if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER)
					return nullptr;
				Entry* entry = Find(a_resource);
				D3D11_BUFFER_DESC desc{};
				static_cast<ID3D11Buffer*>(a_resource)->GetDesc(&desc);
				if (!(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER))
					return nullptr;
				if (!entry)
					entry = Insert(static_cast<ID3D11Buffer*>(a_resource));
				if (entry)
					entry->byteWidth = desc.ByteWidth;
				return entry;
			}

			Entry* Track(ID3D11Buffer* a_buffer) { return Track(static_cast<ID3D11Resource*>(a_buffer)); }

			Entry* Insert(ID3D11Buffer* a_buffer)
			{
				if (entries.size() >= kIndexSize / 2) {
					if (!warnedFull) {
						warnedFull = true;
						logger::warn("[ORG] engine constants: {} constant buffers seen; later ones are not captured", entries.size());
					}
					return nullptr;
				}
				Entry& entry = entries.emplace_back();
				entry.buffer = a_buffer;
				Classify(entry);
				std::uint32_t at = Hash(a_buffer);
				while (index[at])
					at = (at + 1) & (kIndexSize - 1);
				index[at] = &entry;
				return &entry;
			}

			/** @brief Tags an engine global with its group (the block is filled before the engine's first Map). */
			static void Classify(Entry& a_entry)
			{
				ID3D11Buffer* const* block = EngineBlock();
				for (std::uint32_t i = 0; i < kEngineBlockSlots; ++i) {
					if (!block[i] || Tripwire::Unwrap(block[i]) != a_entry.buffer)
						continue;
					for (const auto& range : kEngineSpans) {
						if (i >= range.first && i < range.first + range.count) {
							a_entry.group = range.group;
							a_entry.poolIndex = static_cast<std::uint8_t>(i - range.first);
							return;
						}
					}
				}
			}

			// The ring.

			/** @brief Copies one bound slot's window into the ring, once per version and window; nullopt when nothing was written. */
			std::optional<SlotConstants> CaptureSlot(Entry& a_entry, const Binding& a_binding, Stage a_stage, std::uint32_t a_slot)
			{
				const std::byte* data = a_entry.contents.Data();
				if (!data) {
					++window.unwritten;
					++totals.unwritten;
					return std::nullopt;
				}
				const std::uint32_t limit = a_binding.size ? a_entry.byteWidth : a_entry.written;
				const std::uint32_t begin = a_binding.begin;
				if (begin >= limit)
					return std::nullopt;
				const std::uint32_t size = (std::min)({ a_binding.size ? a_binding.size : limit, limit - begin, kMaxConstantBytes });
				const std::uint32_t version = a_entry.contents.Version();
				if (a_entry.copiedFrame != frame || a_entry.copiedVersion != version || a_entry.copiedBegin != begin || a_entry.copiedSize != size) {
					const auto at = Copy(data + begin, size);
					if (!at)
						return std::nullopt;
					a_entry.copiedFrame = frame;
					a_entry.copiedVersion = version;
					a_entry.copiedBegin = begin;
					a_entry.copiedSize = size;
					a_entry.copiedChunk = at->first;
					a_entry.copiedOffset = at->second;
					++window.copies;
					++totals.copies;
					window.bytes += size;
					totals.bytes += size;
					bytesThisFrame += size;
					++window.copiesByGroup[static_cast<std::uint32_t>(a_entry.group)];
				}
				++window.slots;
				++totals.slots;
				SlotConstants slot;
				slot.stage = a_stage;
				slot.slot = static_cast<std::uint8_t>(a_slot);
				slot.group = a_entry.group;
				slot.poolIndex = a_entry.poolIndex;
				slot.chunk = a_entry.copiedChunk;
				slot.offset = a_entry.copiedOffset;
				slot.size = size;
				return slot;
			}

			/** @brief a_size bytes into the frame's current chunk, or a new one: (chunk, offset). Nullopt, and the capture stops, on failure. */
			std::optional<std::pair<std::uint16_t, std::uint32_t>> Copy(const std::byte* a_data, std::uint32_t a_size)
			{
				std::uint32_t at = (offset + kAlignment - 1) & ~(kAlignment - 1);
				if (frameChunks.empty() || at + a_size > kChunkBytes) {
					if (!NextChunk())
						return std::nullopt;
					at = 0;
				}
				std::memcpy(frameChunks.back()->Slot(0).data() + at, a_data, a_size);
				offset = at + a_size;
				return std::pair{ static_cast<std::uint16_t>(frameChunks.size() - 1), at };
			}

			bool NextChunk()
			{
				if (frameChunks.size() >= UINT16_MAX)
					return Fail("a frame filled every ring chunk it may have");
				std::shared_ptr<org::LatchBlock> chunk;
				{
					std::scoped_lock lock(freeMutex);
					if (!freeChunks.empty()) {
						chunk = std::move(freeChunks.back());
						freeChunks.pop_back();
					}
				}
				if (!chunk) {
					try {
						chunk = org::LatchBlock::Create("Engine::Constants", kChunkBytes, 1);
					} catch (const std::exception& e) {
						return Fail(fmt::format("a ring chunk could not be made ({})", e.what()));
					}
					++chunksMade;
					totals.chunks = chunksMade;
					if (chunksMade > 4 && std::has_single_bit(chunksMade))
						logger::info("[ORG] engine constants: the ring has {} chunks ({} MiB)", chunksMade, (std::uint64_t(chunksMade) * kChunkBytes) >> 20);
				}
				frameChunks.push_back(std::move(chunk));
				offset = 0;
				peakChunksThisFrame = (std::max)(peakChunksThisFrame, static_cast<std::uint32_t>(frameChunks.size()));
				return true;
			}

			/** @brief The frame's chunks go back to the free list once the GPU is past every D3D11 command issued so far. */
			void Retire()
			{
				auto& runtime = RenderGraphRuntime::Get();
				for (auto& chunk : frameChunks) {
					const org::LatchBlock* raw = chunk.get();
					std::shared_ptr<const void> token(raw, [this, kept = std::move(chunk)](const void*) mutable {
						std::scoped_lock lock(freeMutex);
						freeChunks.push_back(std::move(kept));
					});
					runtime.ReleaseAfterD3D11(std::move(token));
				}
				frameChunks.clear();
				offset = 0;
			}

			bool Fail(std::string_view a_why)
			{
				failed = true;
				logger::error("[ORG] engine constants: {}; the capture stops (consumers get no constants)", a_why);
				return false;
			}

			// The debug check (CS_ORG_ENGINE_CONSTANTS_CHECK=1).

			/**
			 * @brief About once a second, compares one captured slot with what the GPU sees in the D3D11 buffer at this point: a staging
			 * copy read back at once. It stalls, so it is a debug switch.
			 */
			void MaybeCheck(const Entry& a_entry, const SlotConstants& a_slot)
			{
				if (frame < nextCheckFrame)
					return;
				nextCheckFrame = frame + 60;
				auto* context = globals::d3d::context;
				if (!context)
					return;
				auto& staging = stagingBySize[a_entry.byteWidth];
				if (!staging) {
					D3D11_BUFFER_DESC desc{};
					desc.ByteWidth = a_entry.byteWidth;
					desc.Usage = D3D11_USAGE_STAGING;
					desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, staging.put())))
						return;
					Util::SetResourceName(staging.get(), "Engine::ConstantsCheck");
				}
				context->CopyResource(staging.get(), a_entry.buffer);
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
					return;
				const auto captured = Bytes(a_slot);
				const auto* gpu = static_cast<const std::byte*>(mapped.pData) + a_entry.copiedBegin;
				const bool same = captured.size() == a_slot.size && std::memcmp(gpu, captured.data(), captured.size()) == 0;
				context->Unmap(staging.get(), 0);
				++window.checks;
				if (!same) {
					++window.mismatches;
					if (mismatchesLogged < 16) {
						++mismatchesLogged;
						logger::warn("[ORG] engine constants check: {} b{} ({}, pool {}) differs from the D3D11 buffer ({} bytes at {})",
							StageName(a_slot.stage), a_slot.slot, GroupName(a_slot.group), a_slot.poolIndex, a_slot.size, a_entry.copiedBegin);
					}
				}
			}

			// The log.

			static const char* StageName(Stage a_stage)
			{
				constexpr const char* names[] = { "VS", "PS", "CS" };
				return names[static_cast<std::uint32_t>(a_stage)];
			}

			static const char* GroupName(Group a_group)
			{
				constexpr const char* names[] = { "other", "custom", "alpha test", "VS technique", "VS material", "VS geometry", "PS technique",
					"PS material", "PS geometry", "compute", "per-frame", "instance group" };
				return names[static_cast<std::uint32_t>(a_group)];
			}

			void LogWindow()
			{
				const auto frames = window.frames;
				std::string groups;
				for (std::uint32_t group = 0; group < kGroups; ++group) {
					if (window.copiesByGroup[group])
						groups += fmt::format("{}{} {}", groups.empty() ? "" : ", ", GroupName(static_cast<Group>(group)), window.copiesByGroup[group] / frames);
				}
				logger::info(
					"[ORG] engine constants: {} frames: per frame {} uses, {} slots, {} copies, {} KiB (peak {} KiB, {} chunks); copies per frame by "
					"group: {}; unwritten slots {}, dropped uses {}{}",
					frames, window.uses / frames, window.slots / frames, window.copies / frames, (window.bytes / frames) >> 10, window.peakBytes >> 10,
					window.peakChunks, groups.empty() ? "none" : groups, window.unwritten, window.dropped,
					checkRequested ? fmt::format("; checked {}, {} differ", window.checks, window.mismatches) : std::string{});
				window = {};
			}

			bool enabled = false;
			bool failed = false;
			bool warnedFull = false;
			bool checkRequested = false;
			std::uint64_t logFrames = 600;

			// Buffers.
			std::deque<Entry> entries;  // stable addresses
			std::array<Entry*, kIndexSize> index{};

			// The bound slots per stage.
			std::array<std::array<Binding, kSlots>, kStages> bindings{};
			std::array<std::uint32_t, kStages> bound{};

			// This frame's uses.
			std::uint64_t frame = 0;
			std::vector<UseRecord> uses;
			std::vector<SlotConstants> slots;
			UseId current;

			// The ring.
			std::vector<std::shared_ptr<org::LatchBlock>> frameChunks;
			std::uint32_t offset = 0;
			std::mutex freeMutex;
			std::vector<std::shared_ptr<org::LatchBlock>> freeChunks;
			std::uint32_t chunksMade = 0;
			std::uint32_t peakChunksThisFrame = 0;
			std::uint64_t bytesThisFrame = 0;

			// The check.
			std::uint64_t nextCheckFrame = 0;
			std::uint32_t mismatchesLogged = 0;
			std::unordered_map<std::uint32_t, winrt::com_ptr<ID3D11Buffer>> stagingBySize;

			Stats totals;
			Window window;
		};

		// The hooks.

		/** @brief VS/PS/CS SetConstantBuffers (context vtable 7, 16, 71). */
		template <Stage S>
		struct SetConstantBuffers
		{
			static void thunk(ID3D11DeviceContext* a_this, UINT a_start, UINT a_count, ID3D11Buffer* const* a_buffers)
			{
				func(a_this, a_start, a_count, a_buffers);
				Tracker::Get().Bind(S, a_start, a_count, a_buffers, nullptr, nullptr);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief VS/PS/CS SetConstantBuffers1 (ID3D11DeviceContext1 vtable 119, 123, 124): CS's windowed binds (GrassOptimizations). */
		template <Stage S>
		struct SetConstantBuffers1
		{
			static void thunk(ID3D11DeviceContext1* a_this, UINT a_start, UINT a_count, ID3D11Buffer* const* a_buffers, const UINT* a_first, const UINT* a_constants)
			{
				func(a_this, a_start, a_count, a_buffers, a_first, a_constants);
				Tracker::Get().Bind(S, a_start, a_count, a_buffers, a_first, a_constants);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief ClearState (vtable 110): Renderer::Begin calls it every frame. */
		struct ClearState
		{
			static void thunk(ID3D11DeviceContext* a_this)
			{
				func(a_this);
				Tracker::Get().Unbind();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief BSGraphics::Renderer::MapCustomConstantBuffer (SE 75561 `0x140d6ffd0`, AE 77362 `0x140e4acc0`; Ghidra called it
		 * GetID3D11Resource): `ID3D11Buffer** (Renderer*, uint size, void** outData, int type)`. Type 7 maps the 16-byte instance-group
		 * buffer; any other type the next of the 4 rotating 0xf00-byte buffers. The caller writes, unmaps and binds the returned slot to
		 * VS and/or PS b<type>. The engine ignores the size; its callers pass float4s (bones, grass) or floats (ImageSpace), so size * 16
		 * bytes always covers what they write.
		 */
		struct MapCustomConstantBuffer
		{
			static void* thunk(void* a_renderer, std::uint64_t a_size, void** a_outData, std::uint64_t a_type)
			{
				const auto size = static_cast<std::uint32_t>(a_size);
				customBound = size < kCustomBytes / 16 ? (std::max)(size, 1u) * 16 : kCustomBytes;
				void* const result = func(a_renderer, a_size, a_outData, a_type);
				customBound = 0;
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	bool Active() { return Tracker::Get().Active(); }

	UseId Current() { return Tracker::Get().Current(); }

	UseId Capture(std::uint32_t a_stageMask) { return Tracker::Get().Capture(a_stageMask); }

	std::span<const SlotConstants> Slots(UseId a_use) { return Tracker::Get().Slots(a_use); }

	const SlotConstants* Find(UseId a_use, Stage a_stage, std::uint32_t a_slot)
	{
		for (const auto& slot : Slots(a_use)) {
			if (slot.stage == a_stage && slot.slot == a_slot)
				return &slot;
		}
		return nullptr;
	}

	std::span<const std::byte> Bytes(const SlotConstants& a_slot) { return Tracker::Get().Bytes(a_slot); }

	Location Where(const SlotConstants& a_slot) { return Tracker::Get().Where(a_slot); }

	void InstallGameHooks()
	{
		if (!Requested())
			return;
		auto& tracker = Tracker::Get();
		tracker.ReadSwitches();
		tracker.Enable();
		logger::info("[ORG] engine constants: capturing every engine constant-buffer use (CS_ORG_ENGINE_CONSTANTS=1)");
		stl::detour_thunk<MapCustomConstantBuffer>(REL::RelocationID(75561, 77362));
	}

	void InstallD3DHooks(ID3D11DeviceContext* a_context)
	{
		if (!Requested() || !a_context)
			return;
		stl::detour_vfunc<7, SetConstantBuffers<Stage::Vertex>>(a_context);
		stl::detour_vfunc<16, SetConstantBuffers<Stage::Pixel>>(a_context);
		stl::detour_vfunc<71, SetConstantBuffers<Stage::Compute>>(a_context);
		stl::detour_vfunc<110, ClearState>(a_context);
		winrt::com_ptr<ID3D11DeviceContext1> context1;
		if (SUCCEEDED(a_context->QueryInterface(IID_PPV_ARGS(context1.put())))) {
			stl::detour_vfunc<119, SetConstantBuffers1<Stage::Vertex>>(context1.get());
			stl::detour_vfunc<123, SetConstantBuffers1<Stage::Pixel>>(context1.get());
			stl::detour_vfunc<124, SetConstantBuffers1<Stage::Compute>>(context1.get());
		} else {
			logger::warn("[ORG] engine constants: no ID3D11DeviceContext1; windowed constant-buffer binds are not seen");
		}
	}

	void OnMap(ID3D11Resource* a_resource, D3D11_MAP a_mapType, void* a_data)
	{
		if (auto& tracker = Tracker::Get(); tracker.Enabled())
			tracker.OnMap(a_resource, a_mapType, a_data);
	}

	void OnUnmap(ID3D11Resource* a_resource)
	{
		if (auto& tracker = Tracker::Get(); tracker.Enabled())
			tracker.OnUnmap(a_resource);
	}

	void OnUpdateSubresource(ID3D11Resource* a_resource, const D3D11_BOX* a_box, const void* a_data)
	{
		if (auto& tracker = Tracker::Get(); tracker.Enabled())
			tracker.OnUpdate(a_resource, a_box, a_data);
	}

	void OnDraw()
	{
		if (auto& tracker = Tracker::Get(); tracker.Enabled())
			tracker.Capture(kDrawStages);
	}

	void OnDispatch()
	{
		if (auto& tracker = Tracker::Get(); tracker.Enabled())
			tracker.Capture(StageBit(Stage::Compute));
	}

	void EndFrame() { Tracker::Get().EndFrame(); }

	Stats GetStats() { return Tracker::Get().GetStats(); }
}

#else  // !CS_HAS_RENDER_GRAPH: nothing is captured.

namespace RenderGraph::Engine::Constants
{
	bool Active() { return false; }
	UseId Current() { return {}; }
	UseId Capture(std::uint32_t) { return {}; }
	std::span<const SlotConstants> Slots(UseId) { return {}; }
	const SlotConstants* Find(UseId, Stage, std::uint32_t) { return nullptr; }
	std::span<const std::byte> Bytes(const SlotConstants&) { return {}; }
	Location Where(const SlotConstants&) { return {}; }
	void InstallGameHooks() {}
	void InstallD3DHooks(ID3D11DeviceContext*) {}
	void OnMap(ID3D11Resource*, D3D11_MAP, void*) {}
	void OnUnmap(ID3D11Resource*) {}
	void OnUpdateSubresource(ID3D11Resource*, const D3D11_BOX*, const void*) {}
	void OnDraw() {}
	void OnDispatch() {}
	void EndFrame() {}
	Stats GetStats() { return {}; }
}

#endif
