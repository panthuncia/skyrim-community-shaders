// The texture transforms' frame-value pools. The game half reads every installed entry's TexcoordOffset and frame variables at
// each ticket's frame start. Each render's upload builds its rows from those values for the list its own payload installed, and sends
// the rows that moved. A second pool is the render's copy of the animated rows, written only by the GPU.
// The rows follow the render's list, never the newest: a repeated render keeps its ticket's payload while later tickets may install
// newer ones, and the draws point into the copy by their payload's animated slots.

#include "Features/DrawcallLimitFix/Features/TextureTransforms/TextureTransformsPool.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <unordered_map>

namespace DCLF::Features::TextureTransforms
{
	namespace
	{
		// The render thread's installed list. The game half reads it through gameList.
		std::shared_ptr<const TextureTransformList> installed;
		std::atomic<std::shared_ptr<const TextureTransformList>> gameList;
		Api::FrameValuePoolId pool;
		Api::FrameValuePoolId copyPool;
		bool copyAvailable = false;

		/** @brief An entry's read: its material and which values it reads. */
		struct ValueKey
		{
			const RE::BSShaderMaterial* material = nullptr;
			const Api::MaterialFrameComponent* components = nullptr;
			bool workingColour = false;
			bool operator==(const ValueKey&) const = default;
		};
		struct ValueKeyHash
		{
			std::size_t operator()(const ValueKey& a_key) const
			{
				const std::size_t material = std::hash<const void*>{}(a_key.material);
				const std::size_t components = std::hash<const void*>{}(a_key.components);
				return material ^ (components * 31u) ^ (a_key.workingColour ? 0x9e3779b9u : 0u);
			}
		};
		using Values = std::unordered_map<ValueKey, std::array<float, 4>, ValueKeyHash>;

		ValueKey KeyOf(const TextureTransformList::Entry& a_entry) { return { a_entry.material, a_entry.components, a_entry.workingColour }; }

		// The game half's last reads. The upload jobs read the newest.
		std::atomic<std::shared_ptr<const Values>> published;

		// The upload jobs run one at a time in frame order.
		KeptWholeArray<TextureTransformRow> transforms;

		/** @brief Game half, at each ticket's frame start: the values of every entry the installed list names. */
		void FrameStart(const Api::FrameValuePoolFrameStart& a_start)
		{
			DCLF_ZONE("CS.DCLF.FV.FrameStart.TextureTransforms");
			const auto list = gameList.load(std::memory_order_acquire);
			if (!list)
				return;
			// Main::Update flips the TexcoordOffset buffer and the controllers write the other one. The frame variables are read as the
			// frame start finds them.
			const std::size_t buffer = a_start.globals ? a_start.globals->material.vanilla.textureTransformBuffer & 1u : 0u;
			const auto* space = Integrations::LinearLighting::LiveWorkingSpace();
			auto values = std::make_shared<Values>();
			values->reserve(list->entries.size());
			for (const auto& entry : list->entries) {
				float read[4];
				Read(entry, buffer, space, read);
				(*values)[KeyOf(entry)] = { read[0], read[1], read[2], read[3] };
			}
			published.store(std::move(values), std::memory_order_release);
		}

		/** @brief Render thread, with each upload: the list the render's payload installed. */
		Api::FrameValueInput Kick(const Api::FrameValuePoolKick&)
		{
			DCLF_ZONE("CS.DCLF.FV.Kick.TextureTransforms");
			const std::uint64_t rows = (installed ? installed->entries.size() + installed->animatedSlots.size() : 0u) + 1u;
			return { installed, rows };
		}

		// Nothing is sampled or sent. TextureTransformsCS writes every row the render's draws point at before any of them reads it.
		Api::FrameValueInput KickCopy(const Api::FrameValuePoolKick&)
		{
			return { installed, installed ? installed->animatedSlots.size() : 0u };
		}
		void SampleCopy(Api::FrameValuePoolProducer&) {}
		void SendCopy(Api::FrameValuePoolProducer&) {}

		/** @brief The render's upload: its list's rows from the game half's newest values. Reads no engine memory. */
		void Sample(Api::FrameValuePoolProducer& a_producer)
		{
			DCLF_ZONE("CS.DCLF.FrameValues.TextureTransforms");
			a_producer.BeginBuild(transforms);
			auto& out = transforms.Mutable();
			const auto* list = a_producer.Input<TextureTransformList>();
			const std::size_t count = list ? list->entries.size() : 0;
			const std::size_t animated = list ? list->animatedSlots.size() : 0;
			if (out.size() < count + animated + 1) {
				const std::size_t first = out.size();
				out.resize(count + animated + 1, TextureTransformRow{});
				transforms.MarkRange(first, count + animated + 1 - first);
			}
			if (out[0].rowOffset != count || out[0].mask != animated) {
				out[0] = TextureTransformRow{};
				out[0].rowOffset = static_cast<std::uint32_t>(count);
				out[0].mask = static_cast<std::uint32_t>(animated);
				transforms.Mark(0);
			}
			for (std::size_t k = 0; k < animated; ++k) {
				const std::uint32_t slot = list->animatedSlots[k];
				TextureTransformRow next{};
				next.rowOffset = slot == ~0u ? ~0u : slot * kMaterialRowBytes;
				if (std::memcmp(&next, &out[count + 1 + k], sizeof(next)) != 0) {
					out[count + 1 + k] = next;
					transforms.Mark(count + 1 + k);
				}
			}
			const auto values = published.load(std::memory_order_acquire);
			std::uint64_t changed = 0, unread = 0, variablesChanged = 0;
			for (std::size_t i = 0; i < count; ++i) {
				const auto& entry = list->entries[i];
				auto& row = out[i + 1];
				TextureTransformRow next = row;
				next.rowOffset = entry.rowOffset;
				next.mask = entry.mask;
				const auto found = values ? values->find(KeyOf(entry)) : Values::const_iterator{};
				if (values && found != values->end()) {
					std::memcpy(next.values, found->second.data(), sizeof(next.values));
				} else {
					// Not read yet (a list newer than the game half's last frame start): the row keeps the installed values.
					++unread;
					next.mask = 0;
				}
				if (std::memcmp(&next, &row, sizeof(row)) != 0) {
					row = next;
					transforms.Mark(i + 1);
					++changed;
					variablesChanged += entry.components ? 1u : 0u;
				}
			}
			CountPoolLine(count, changed, unread, list ? list->variables : 0u, variablesChanged);
		}

		void Send(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.Send(transforms);
		}
	}

	void RegisterPool(Api::InstallContext& a_context)
	{
		pool = a_context.RegisterFrameValuePool({ .name = "texture-transforms",
			.stride = sizeof(TextureTransformRow),
			.perRender = true,
			.frameStart = &FrameStart,
			.kick = &Kick,
			.sample = &Sample,
			.send = &Send });
		copyPool = a_context.RegisterFrameValuePool({ .name = "animated-material-rows",
			.stride = kMaterialRowBytes,
			.deviceAddress = true,
			.unorderedAccess = true,
			.perRender = true,
			.kick = &KickCopy,
			.sample = &SampleCopy,
			.send = &SendCopy });
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
		gameList.store(list, std::memory_order_release);
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
