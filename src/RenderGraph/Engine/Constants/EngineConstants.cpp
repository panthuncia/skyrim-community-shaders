// The per-use capture of the engine's constant buffers. See EngineConstants.h.

#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Fatal.h"
#include "Snapshot/EngineReads.h"

#include "RenderGraph/Switches.h"

#include "Globals.h"
#include "RenderGraph/Engine/Buffers/UploadRing.h"
#include "RenderGraph/Engine/Constants/BufferContents.h"
#include "RenderGraph/Engine/Flush/Flush.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Tripwire/GuestDevice.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/FrameStaging.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <d3d11_1.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>
#include <winrt/base.h>

namespace RenderGraph::Engine::Constants
{
	namespace
	{
		// A slot addresses at most 64 KiB, so every window fits.
		constexpr std::uint32_t kChunkBytes = 1u << 20;
		// D3D11's 4096 constants.
		constexpr std::uint32_t kMaxConstantBytes = 4096 * 16;
		// Shaders load them with Load4.
		constexpr std::uint32_t kAlignment = 16;
		// Open addressing, a power of two. The engine has 146 constant buffers, CS and other plugins a few dozen.
		constexpr std::uint32_t kIndexSize = 1u << 14;
		// A frame that never ends stops capturing here instead of growing without bound.
		constexpr std::uint32_t kMaxUsesPerFrame = 1u << 20;
		constexpr std::uint32_t kCustomBytes = 0xf00;
		constexpr std::uint64_t kUnseenFrames = 1800;

		/**
		* @brief The engine's 146 constant-buffer pointers as CreateConstantBufferPools lays them out, from SE 524753 / AE 411369.
		* Slot 0 of each pool is never created.
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

		/**
		* @brief The entry holds a reference, so the buffer's address cannot be reused while it lives and lookups need no D3D11 check.
		* Entries never move. One unseen for kUnseenFrames is retired.
		*/
		struct Entry
		{
			ID3D11Buffer* buffer = nullptr;  // null once retired
			winrt::com_ptr<ID3D11Buffer> held;
			std::uint64_t seenFrame = 0;
			std::uint32_t byteWidth = 0;
			std::uint32_t written = 0;  // the custom mapper's bound, else byteWidth
			Group group = Group::Other;
			std::uint8_t poolIndex = 0;
			BufferContents contents;

			std::uint64_t copiedFrame = UINT64_MAX;
			std::uint32_t copiedVersion = 0;
			std::uint32_t copiedBegin = 0;
			std::uint32_t copiedSize = 0;
			std::uint16_t copiedChunk = 0;
			std::uint32_t copiedOffset = 0;
		};

		/** @brief Window size 0 means the whole buffer. */
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

		struct Window
		{
			std::uint64_t frames = 0, uses = 0, slots = 0, copies = 0, bytes = 0, unwritten = 0, dropped = 0;
			std::uint64_t peakBytes = 0;
			std::uint32_t peakChunks = 0;
			std::array<std::uint64_t, kGroups> copiesByGroup{};
		};

		// The custom mapper's size bound for its upcoming Map on this thread. 0 for none.
		thread_local std::uint32_t customBound = 0;

		/** @brief An upload ring chunk that shaders read as a ByteAddressBuffer: a frame arena staging region, kept for the process. */
		struct ConstantsChunk final : Buffers::UploadRing::Chunk
		{
			RenderGraph::Staging region;
		};

		/** @brief Fatal when it cannot be made: without the capture no engine draw the graph takes has its constants. */
		std::unique_ptr<Buffers::UploadRing::Chunk> MakeConstantsChunk(std::uint32_t a_bytes)
		{
			auto chunk = std::make_unique<ConstantsChunk>();
			chunk->region = RenderGraph::AcquireStaging("engine constants", a_bytes);
			chunk->data = chunk->region->mapped;
			chunk->size = chunk->region->size;
			return chunk;
		}

		/** @brief The capture's chunks come from an upload ring, which takes the retired ones back. */
		class Tracker
		{
		public:
			static Tracker& Get()
			{
				// Never destroyed, because retired chunks may come back during shutdown.
				static Tracker* tracker = new Tracker;
				return *tracker;
			}

			bool Enabled() const { return enabled; }

			bool Active() const { return enabled && RenderGraphRuntime::Get().IsActive(); }

			void Enable() { enabled = true; }


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


			UseId Capture(std::uint32_t a_stageMask)
			{
				ZoneScopedN("CS.Engine.Constants.Capture");
				CS_ENGINE_READ_MARK(BoundState, BoundState);
				if (!Active() || !FrameAnchors::IsRenderOwner())
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
						if (!captured)
							continue;
						slots.push_back(*captured);
						++record.count;
					}
				}
				uses.push_back(record);
				++window.uses;
				return { frame, static_cast<std::uint32_t>(uses.size() - 1) };
			}

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
				// The chunk is GPU-visible upload memory, slow to read back, so read the CPU copy.
				return { mirrors[a_slot.chunk].get() + a_slot.offset, a_slot.size };
			}

			/** @brief Copies a_bytes into this frame's ring, padded to whole rows. Empty when inactive. */
			Location Upload(std::span<const std::byte> a_bytes)
			{
				if (!Active() || !FrameAnchors::IsRenderOwner() || a_bytes.empty())
					return {};
				const auto size = static_cast<std::uint32_t>((a_bytes.size() + kAlignment - 1) & ~std::size_t(kAlignment - 1));
				std::array<std::byte, 4096> padded{};
				if (size > padded.size())
					return {};
				std::memcpy(padded.data(), a_bytes.data(), a_bytes.size());
				const auto at = Copy(padded.data(), size);
				if (!at)
					return {};
				bytesThisFrame += size;
				const auto& region = frameChunks[at->first]->region;
				return { region, region->srvIndex, static_cast<std::uint32_t>(region->offset) + at->second, size };
			}

			Location Where(const SlotConstants& a_slot) const
			{
				if (a_slot.chunk >= frameChunks.size())
					return {};
				const auto& region = frameChunks[a_slot.chunk]->region;
				return { region, region->srvIndex, static_cast<std::uint32_t>(region->offset) + a_slot.offset, a_slot.size };
			}

			void EndFrame()
			{
				if (!enabled || !FrameAnchors::IsRenderOwner())
					return;
				// Every execution of the frame has been submitted, so its chunks go behind the work issued by now.
				ring.Retire();
				ring.ReleaseParked(UINT64_MAX);
				frameChunks.clear();
				window.peakChunks = (std::max)(window.peakChunks, peakChunksThisFrame);
				window.peakBytes = (std::max)(window.peakBytes, bytesThisFrame);
				peakChunksThisFrame = 0;
				bytesThisFrame = 0;
				uses.clear();
				slots.clear();
				if (frame % kUnseenFrames == 0)
					LetGoUnseen();
				++frame;
				++totals.frames;
				++window.frames;
				if (window.frames >= logFrames)
					LogWindow();
			}

			Stats GetStats() const
			{
				Stats stats = totals;
				stats.chunks = ring.Made();
				return stats;
			}

			void ReadSwitches()
			{
				logFrames = (std::max<std::uint64_t>)(1, Switches::Number("CS_ORG_ENGINE_CONSTANTS_LOG_FRAMES", 600));
			}

		private:
			Tracker() :
				ring({ .name = "engine constants", .chunkBytes = kChunkBytes, .alignment = kAlignment, .make = &MakeConstantsChunk })
			{}


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

			/** @brief Null for anything that is not a constant buffer. An unknown resource is checked once. */
			Entry* Track(ID3D11Resource* a_resource)
			{
				if (!a_resource)
					return nullptr;
				if (Entry* entry = Find(a_resource)) {
					entry->seenFrame = frame;
					return entry;
				}
				D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
				a_resource->GetType(&dimension);
				if (dimension != D3D11_RESOURCE_DIMENSION_BUFFER)
					return nullptr;
				D3D11_BUFFER_DESC desc{};
				static_cast<ID3D11Buffer*>(a_resource)->GetDesc(&desc);
				if (!(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER))
					return nullptr;
				Entry* entry = Insert(static_cast<ID3D11Buffer*>(a_resource));
				if (entry)
					entry->byteWidth = desc.ByteWidth;
				return entry;
			}

			void LetGoUnseen()
			{
				for (auto& entry : entries) {
					if (!entry.buffer || frame - entry.seenFrame < kUnseenFrames)
						continue;
					entry.buffer = nullptr;
					entry.held = nullptr;
					entry.contents = {};
				}
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
				entry.held.copy_from(a_buffer);
				entry.seenFrame = frame;
				Classify(entry);
				std::uint32_t at = Hash(a_buffer);
				while (index[at])
					at = (at + 1) & (kIndexSize - 1);
				index[at] = &entry;
				return &entry;
			}

			/** @brief The engine fills the block before its first Map. */
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


			/** @brief Once per version and window. Nullopt when nothing was written. */
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

			/** @brief Where the bytes went: the frame's chunk index and the offset in it. */
			std::optional<std::pair<std::uint16_t, std::uint32_t>> Copy(const std::byte* a_data, std::uint32_t a_size)
			{
				const auto at = ring.Allocate(a_size);
				if (!at)
					RenderGraph::Fatal("engine constants", "a ring chunk could not be made");
				auto* chunk = static_cast<ConstantsChunk*>(at->chunk);
				if (frameChunks.empty() || frameChunks.back() != chunk) {
					if (frameChunks.size() >= UINT16_MAX)
						RenderGraph::Fatal("engine constants", "a frame filled every ring chunk it may have");
					frameChunks.push_back(chunk);
					if (mirrors.size() < frameChunks.size())
						mirrors.push_back(std::make_unique<std::byte[]>(kChunkBytes));
					peakChunksThisFrame = (std::max)(peakChunksThisFrame, static_cast<std::uint32_t>(frameChunks.size()));
				}
				// Copy into the CPU mirror first so the source, possibly upload memory, is read only once.
				std::byte* mirror = mirrors[frameChunks.size() - 1].get() + at->offset;
				std::memcpy(mirror, a_data, a_size);
				std::memcpy(at->data, mirror, a_size);
				return std::pair{ static_cast<std::uint16_t>(frameChunks.size() - 1), at->offset };
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
					"group: {}; unwritten slots {}, dropped uses {}",
					frames, window.uses / frames, window.slots / frames, window.copies / frames, (window.bytes / frames) >> 10, window.peakBytes >> 10,
					window.peakChunks, groups.empty() ? "none" : groups, window.unwritten, window.dropped);
				window = {};
			}

			bool enabled = false;
			bool warnedFull = false;
			std::uint64_t logFrames = 600;

			std::deque<Entry> entries;  // stable addresses
			std::array<Entry*, kIndexSize> index{};

			std::array<std::array<Binding, kSlots>, kStages> bindings{};
			std::array<std::uint32_t, kStages> bound{};

			std::uint64_t frame = 0;
			std::vector<UseRecord> uses;
			std::vector<SlotConstants> slots;

			Buffers::UploadRing ring;
			std::vector<ConstantsChunk*> frameChunks;  // this frame's, in the order it took them
			// Kept for the session at the frame's peak count.
			std::vector<std::unique_ptr<std::byte[]>> mirrors;
			std::uint32_t peakChunksThisFrame = 0;
			std::uint64_t bytesThisFrame = 0;


			Stats totals;
			Window window;
		};


		/**
		* @brief The engine's pool buffers, answered from CS memory in graph mode. Each has two copies: a discard map takes the other
		* one, so the last mapping stays readable until the map after next, as BufferContents and DCLF's mirror expect of DXVK.
		* Runs on whichever thread drives the immediate context, under Renderer::Lock.
		*/
		class EnginePools
		{
		public:
			static EnginePools& Get()
			{
				// Never destroyed, since the engine may map during shutdown.
				static EnginePools* pools = new EnginePools;
				return *pools;
			}

			bool Map(ID3D11Resource* a_resource, UINT a_subresource, D3D11_MAP a_type, D3D11_MAPPED_SUBRESOURCE* a_out)
			{
				if (a_subresource != 0 || !Answering())
					return false;
				Pool* pool = Find(a_resource);
				if (!pool) {
					// Native mode's context keeps no service for the pool buffers: these maps answer every one of them.
					if (Native::Requested() && !built.load(std::memory_order_acquire) && InEngineBlock(a_resource))
						RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::EngineWork, RenderGraph::Frame::SegmentId::Count,
							"the engine mapped a pool constant buffer before the pools exist, so the map reached the native context");
					return false;
				}
				if (a_type == D3D11_MAP_WRITE_DISCARD)
					pool->current ^= 1u;
				else if (a_type != D3D11_MAP_WRITE_NO_OVERWRITE)
					RenderGraph::Fatal("engine constants", fmt::format("the engine mapped a pool constant buffer with map type {}, which graph mode has no D3D11 buffer for", static_cast<int>(a_type)));
				pool->mapped = true;
				a_out->pData = pool->copies[pool->current].get();
				a_out->RowPitch = a_out->DepthPitch = pool->byteWidth;
				// Guest mode hooks DXVK's context, which this map never reaches. Native mode hooks the handle, which calls it after this returns.
				if (!Native::Requested())
					globals::OnContextMap(a_resource, a_type, a_out);
				return true;
			}

			/** @brief Only for a map this answered, so a map made before graph mode took over goes on to D3D11. */
			bool Unmap(ID3D11Resource* a_resource, UINT a_subresource)
			{
				if (a_subresource != 0)
					return false;
				Pool* pool = Find(a_resource);
				if (!pool || !pool->mapped)
					return false;
				if (!Native::Requested())
					globals::OnContextUnmap(a_resource);
				pool->mapped = false;
				return true;
			}

		private:
			/** @brief kEngineBlockSlots buffers in a list this short: a sorted array of their keys. */
			struct Pool
			{
				const void* key = nullptr;
				std::uint32_t byteWidth = 0;
				std::uint32_t current = 0;
				bool mapped = false;
				std::array<std::unique_ptr<std::byte[]>, 2> copies;
			};

			/** @brief Native mode has no D3D11 path at all. Guest mode answers once the engine's context drops its draws, so none reaches DXVK. */
			static bool Answering() { return Native::Requested() || Flush::EngineDrawsDropped(); }

			/** @brief In guest mode the hook gets DXVK's buffer, in native mode the handle the engine holds. */
			static const void* KeyOf(ID3D11Buffer* a_engine)
			{
				if (Native::Requested())
					return a_engine;
				return Tripwire::Unwrap(a_engine);
			}

			/** @brief Whether a_resource is one of the engine's pool buffers, before the index exists. */
			static bool InEngineBlock(const ID3D11Resource* a_resource)
			{
				ID3D11Buffer* const* block = EngineBlock();
				for (std::uint32_t i = 0; i < kEngineBlockSlots; ++i)
					if (block[i] && KeyOf(block[i]) == a_resource)
						return true;
				return false;
			}

			/** @brief Null for any other resource. The pools exist before the first frame, so the index is built once. */
			Pool* Find(const ID3D11Resource* a_resource)
			{
				if (!built.load(std::memory_order_acquire) && !Build())
					return nullptr;
				const auto it = std::ranges::lower_bound(pools, static_cast<const void*>(a_resource), {}, &Pool::key);
				return it != pools.end() && it->key == a_resource ? &*it : nullptr;
			}

			bool Build()
			{
				std::scoped_lock lock(buildMutex);
				if (built.load(std::memory_order_relaxed))
					return true;
				ID3D11Buffer* const* block = EngineBlock();
				// The instance-group buffer is the last the engine makes.
				if (!block[kEngineBlockSlots - 1])
					return false;
				std::vector<Pool> made;
				made.reserve(kEngineBlockSlots);
				for (std::uint32_t i = 0; i < kEngineBlockSlots; ++i) {
					ID3D11Buffer* buffer = block[i];
					if (!buffer)
						continue;
					D3D11_BUFFER_DESC desc{};
					buffer->GetDesc(&desc);
					if (desc.Usage != D3D11_USAGE_DYNAMIC || !desc.ByteWidth)
						RenderGraph::Fatal("engine constants", fmt::format("the engine's pool constant buffer {} is not a dynamic buffer", i));
					Pool pool;
					pool.key = KeyOf(buffer);
					pool.byteWidth = desc.ByteWidth;
					for (auto& copy : pool.copies) {
						copy = std::make_unique<std::byte[]>(desc.ByteWidth);
						std::memset(copy.get(), 0, desc.ByteWidth);
					}
					made.push_back(std::move(pool));
				}
				std::ranges::sort(made, {}, &Pool::key);
				pools = std::move(made);
				built.store(true, std::memory_order_release);
				logger::info("[ORG] engine constants: the engine's {} pool constant buffers are answered from CS memory in graph mode", pools.size());
				return true;
			}

			std::mutex buildMutex;
			std::atomic<bool> built{ false };
			std::vector<Pool> pools;  // sorted by key, fixed once built
		};

		bool MapEnginePool(ID3D11Resource* a_resource, UINT a_subresource, D3D11_MAP a_type, UINT, D3D11_MAPPED_SUBRESOURCE* a_out)
		{
			return EnginePools::Get().Map(a_resource, a_subresource, a_type, a_out);
		}

		bool UnmapEnginePool(ID3D11Resource* a_resource, UINT a_subresource)
		{
			return EnginePools::Get().Unmap(a_resource, a_subresource);
		}

		/** @brief Context vtable 7, 16, 71. */
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

		/** @brief ID3D11DeviceContext1 vtable 119, 123, 124. GrassOptimizations uses windowed binds. */
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

		/** @brief Vtable 110. Renderer::Begin calls it every frame. */
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
		 * @brief The engine context handle's vtable 7, 16, 71 in guest mode, where the engine's binds stay in the handle and never
		 * reach DXVK's context. Tracked as DXVK's buffers, which the map hooks name.
		 */
		template <Stage S>
		struct EngineSetConstantBuffers
		{
			static void thunk(ID3D11DeviceContext* a_this, UINT a_start, UINT a_count, ID3D11Buffer* const* a_buffers)
			{
				func(a_this, a_start, a_count, a_buffers);
				std::array<ID3D11Buffer*, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> buffers{};
				const UINT count = (std::min)(a_count, static_cast<UINT>(buffers.size()));
				for (UINT i = 0; i < count; ++i)
					buffers[i] = a_buffers ? Tripwire::Unwrap(a_buffers[i]) : nullptr;
				Tracker::Get().Bind(S, a_start, count, buffers.data(), nullptr, nullptr);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The engine context handle's vtable 110 in guest mode. */
		struct EngineClearState
		{
			static void thunk(ID3D11DeviceContext* a_this)
			{
				func(a_this);
				Tracker::Get().Unbind();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		* @brief MapCustomConstantBuffer (SE 75561, AE 77362). Type 7 maps the 16-byte instance-group buffer, others the next 0xf00-byte buffer.
		* The engine ignores the size, but size * 16 bytes always covers what its callers write.
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

	Location Upload(std::span<const std::byte> a_bytes) { return Tracker::Get().Upload(a_bytes); }

	ID3D11Buffer* EngineComputeBuffer()
	{
		// The Compute span's one slot.
		constexpr std::uint32_t kComputeSlot = 143;
		return EngineBlock()[kComputeSlot];
	}

	void InstallGameHooks()
	{
		if (!Engine::GraphMode())
			return;
		auto& tracker = Tracker::Get();
		tracker.ReadSwitches();
		tracker.Enable();
		logger::info("[ORG] engine constants: capturing every engine constant-buffer use");
		stl::detour_thunk<MapCustomConstantBuffer>(REL::RelocationID(75561, 77362));
		Tripwire::SetMapHooks({ .map = &MapEnginePool, .unmap = &UnmapEnginePool });
	}

	void InstallD3DHooks(ID3D11DeviceContext* a_context)
	{
		if (!Engine::GraphMode() || !a_context)
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
		// Guest mode: a_context is DXVK's, which sees CS's binds. The engine's stay in its context handle. Native mode has one context.
		if (auto* engine = Tripwire::EngineContext(); engine && engine != a_context && Tripwire::IsHandle(engine)) {
			stl::detour_vfunc<7, EngineSetConstantBuffers<Stage::Vertex>>(engine);
			stl::detour_vfunc<16, EngineSetConstantBuffers<Stage::Pixel>>(engine);
			stl::detour_vfunc<71, EngineSetConstantBuffers<Stage::Compute>>(engine);
			stl::detour_vfunc<110, EngineClearState>(engine);
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

	void EndFrame() { Tracker::Get().EndFrame(); }

	Stats GetStats() { return Tracker::Get().GetStats(); }
}

