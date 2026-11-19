// The texture transforms' frame-value pools. FrameValues' producer samples every installed material row's TexcoordOffset and frame
// variables each frame and sends the rows that moved. A second pool is the frame's copy of the animated rows, written only by the GPU.

#include "Features/DrawcallLimitFix/Features/TextureTransforms/TextureTransformsPool.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"

#include <algorithm>
#include <cstring>

namespace DCLF::Features::TextureTransforms
{
	namespace
	{
		// The rows a list named never shrink.
		std::shared_ptr<const TextureTransformList> installed;
		std::uint64_t rowsNeeded = 1;
		Api::FrameValuePoolId pool;
		Api::FrameValuePoolId copyPool;
		bool copyAvailable = false;

		// The producers run one at a time in frame order.
		struct Produced
		{
			// A refused read keeps the last values only for the same material.
			KeptWholeArray<TextureTransformRow> transforms;
			std::vector<const RE::BSShaderMaterial*> materials;
		};
		Produced produced;

		Api::FrameValueInput Kick(const Api::FrameValuePoolKick&)
		{
			DCLF_ZONE("CS.DCLF.FV.Kick.TextureTransforms");
			rowsNeeded = std::max<std::uint64_t>(rowsNeeded, (installed ? installed->entries.size() + installed->animatedSlots.size() : 0u) + 1u);
			return { installed, rowsNeeded };
		}

		// Nothing is sampled or sent. TextureTransformsCS writes every row the frame's draws point at before any of them reads it.
		Api::FrameValueInput KickCopy(const Api::FrameValuePoolKick&)
		{
			return { installed, installed ? installed->animatedSlots.size() : 0u };
		}
		void SampleCopy(Api::FrameValuePoolProducer&) {}
		void SendCopy(Api::FrameValuePoolProducer&) {}

		struct Sampled
		{
			std::uint64_t changed = 0, refused = 0, variablesChanged = 0;
		};

		Sampled SampleEntries(const Api::FrameValuePoolProducer& a_producer, const TextureTransformList* a_list, std::size_t a_count, std::size_t a_buffer,
			std::vector<TextureTransformRow>& a_out)
		{
			auto& p = produced;
			constexpr std::size_t kGrain = 256;
			struct Chunk
			{
				std::vector<std::uint32_t> changed;
				std::uint64_t refused = 0, variablesChanged = 0;
			};
			std::vector<Chunk> chunks((a_count + kGrain - 1) / kGrain);
			SceneScheduler::Executor().ParallelFor("CS.DCLF.FrameValues.TextureTransforms", a_count, kGrain, [&](std::size_t a_begin, std::size_t a_end) {
				DCLF_ZONE("CS.DCLF.FrameValues.TextureTransformsShards");
				auto& mine = chunks[a_begin / kGrain];
				// One engine-read lease for the chunk. If it cannot be had, each entry is unread.
				EngineReadWindow::Lease lease;
				const bool readable = Readable(a_producer, lease);
				for (std::size_t i = a_begin; i < a_end; ++i) {
					const auto& entry = a_list->entries[i];
					auto& row = a_out[i + 1];
					TextureTransformRow next = row;
					next.rowOffset = entry.rowOffset;
					next.mask = entry.mask;
					if (readable) {
						Read(entry, a_buffer, next.values);
						p.materials[i] = entry.material;
					} else {
						// An unread entry keeps its values for the same material, else the row keeps its own.
						++mine.refused;
						if (p.materials[i] != entry.material)
							next.mask = 0;
					}
					if (std::memcmp(&next, &row, sizeof(row)) != 0) {
						row = next;
						mine.changed.push_back(static_cast<std::uint32_t>(i + 1));
						mine.variablesChanged += entry.components ? 1u : 0u;
					}
				}
			});
			Sampled out;
			for (const auto& chunk : chunks) {
				for (const auto at : chunk.changed)
					p.transforms.Mark(at);
				out.changed += chunk.changed.size();
				out.refused += chunk.refused;
				out.variablesChanged += chunk.variablesChanged;
			}
			return out;
		}

		void Sample(Api::FrameValuePoolProducer& a_producer)
		{
			// Main::Update flips the TexcoordOffset buffer and the controllers write the other one, so a read anywhere in the window gets this
			// frame's value. The frame variables are read as the window finds them, and parity reads them again at the producer's end.
			DCLF_ZONE("CS.DCLF.FrameValues.TextureTransforms");
			auto& p = produced;
			a_producer.BeginBuild(p.transforms);
			auto& out = p.transforms.Mutable();
			const auto* list = a_producer.Input<TextureTransformList>();
			const std::size_t count = list ? list->entries.size() : 0;
			const std::size_t animated = list ? list->animatedSlots.size() : 0;
			if (out.size() < count + animated + 1) {
				const std::size_t first = out.size();
				out.resize(count + animated + 1, TextureTransformRow{});
				p.transforms.MarkRange(first, count + animated + 1 - first);
			}
			if (p.materials.size() < count)
				p.materials.resize(count, nullptr);
			if (out[0].rowOffset != count || out[0].mask != animated) {
				out[0] = TextureTransformRow{};
				out[0].rowOffset = static_cast<std::uint32_t>(count);
				out[0].mask = static_cast<std::uint32_t>(animated);
				p.transforms.Mark(0);
			}
			for (std::size_t k = 0; k < animated; ++k) {
				const std::uint32_t slot = list->animatedSlots[k];
				TextureTransformRow next{};
				next.rowOffset = slot == ~0u ? ~0u : slot * kMaterialRowBytes;
				if (std::memcmp(&next, &out[count + 1 + k], sizeof(next)) != 0) {
					out[count + 1 + k] = next;
					p.transforms.Mark(count + 1 + k);
				}
			}
			// The buffer the frame's shaders read. The controllers write the other one.
			const std::size_t buffer = a_producer.TextureTransformBuffer() & 1u;
			const auto sampled = SampleEntries(a_producer, list, count, buffer, out);
			if (a_producer.parity)
				CheckPoolParity(a_producer, list, count, buffer, out, p.materials);
			CountPoolLine(count, sampled.changed, sampled.refused, list ? list->variables : 0u, sampled.variablesChanged);
		}

		void Send(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.Send(produced.transforms);
		}
	}

	void RegisterPool(Api::InstallContext& a_context)
	{
		pool = a_context.RegisterFrameValuePool({ .name = "texture-transforms", .stride = sizeof(TextureTransformRow), .kick = &Kick, .sample = &Sample, .send = &Send });
		copyPool = a_context.RegisterFrameValuePool(
			{ .name = "animated-material-rows", .stride = kMaterialRowBytes, .deviceAddress = true, .unorderedAccess = true, .kick = &KickCopy, .sample = &SampleCopy, .send = &SendCopy });
	}

	// Installs the main material rows that carry TexcoordOffset, before Kick. Render thread.
	void InstallList(std::shared_ptr<const void> a_list)
	{
		auto list = std::static_pointer_cast<const TextureTransformList>(std::move(a_list));
		if (list) {
			TextureTransformStats::adopted.fetch_add(1, std::memory_order_relaxed);
			TextureTransformStats::adoptedEntries.fetch_add(list->entries.size(), std::memory_order_relaxed);
		} else {
			TextureTransformStats::adoptedNone.fetch_add(1, std::memory_order_relaxed);
		}
		installed = std::move(list);
	}

	std::uint32_t RowsIndex()
	{
		return Extensions::FrameValuePoolIndex(pool);
	}

	Api::FrameRows AnimatedRowsAddress()
	{
		// Without a producer the frame has neither backing, so the pass stores nothing and the draws read the installed rows.
		if (!copyAvailable || !RowsIndex())
			return {};
		const auto& values = FrameValues::Get();
		return { values.PoolAddress(copyPool), values.PoolRef(copyPool), static_cast<std::uint32_t>(values.PoolBytes(copyPool) / kMaterialRowBytes) };
	}

	Api::FrameValuePoolId CopyPool()
	{
		return copyPool;
	}

	void SetCopyAvailable(bool a_available)
	{
		copyAvailable = a_available;
	}
}
