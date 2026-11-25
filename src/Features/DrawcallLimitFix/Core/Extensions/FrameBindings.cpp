// The registered frame bindings' values and registers.

// Internal.h first, because volk must precede every Vulkan header in the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "RenderGraph/DxvkImageImport.h"
#include "RenderGraph/ImageRegistry.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Engine/Targets/EngineTargetSet.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/Relocation/Holders.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/UndeclaredAccess.h"

#include <Render/PassBuilders.h>
#include <Render/PreparedPass.h>
#include <Resources/VersionedResource.h>

#include <ankerl/unordered_dense.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <initializer_list>
#include <memory>
#include <vector>

namespace DCLF::Extensions
{
	const std::shared_ptr<org::VersionedBuffer>* FrameBindingBuffer(std::size_t a_binding, const Draws::SceneBuffers& a_scene, std::uint32_t a_frame)
	{
		const auto bindings = RegisteredFrameBindings();
		if (a_binding >= bindings.size())
			return nullptr;
		Api::FrameBindingSelect select{ a_scene, a_frame };
		bindings[a_binding].select(select);
		return select.buffer;
	}

	void FrameBindingValuesOf(const org::services::NamedVersions& a_sealed, const Draws::SceneBuffers& a_scene, std::uint32_t a_frame, FrameBindingValues& a_out,
		bool a_renderThread)
	{
		const auto bindings = RegisteredFrameBindings();
		for (std::size_t i = 0; i < bindings.size(); ++i) {
			Api::FrameBindingSelect select{ a_scene, a_frame, a_renderThread };
			bindings[i].select(select);
			a_out[i] = select.buffer && *select.buffer ? a_sealed.Of(**select.buffer).srvIndex :
			           a_renderThread && select.index != 0 ? select.index :
			                                                 kNoFrameBinding;
		}
	}

	void WriteFrameBindings(const FrameBindingValues& a_values, std::span<std::uint32_t> a_textures)
	{
		const auto bindings = RegisteredFrameBindings();
		for (std::size_t i = 0; i < bindings.size(); ++i)
			if (a_values[i] != kNoFrameBinding && bindings[i].textureRegister < a_textures.size())
				a_textures[bindings[i].textureRegister] = a_values[i];
	}

	bool FrameGraphBufferRegister(std::uint32_t a_register)
	{
		for (const auto& buffers : RegisteredFrameGraphBuffers())
			if (a_register >= buffers.firstRegister && a_register - buffers.firstRegister < buffers.count)
				return true;
		return false;
	}

	bool FrameGraphBufferIndices(std::size_t a_buffers, std::span<std::uint32_t, kFrameGraphBufferRegisters> a_out)
	{
		const auto registered = RegisteredFrameGraphBuffers();
		if (a_buffers >= registered.size())
			return false;
		const auto& buffers = registered[a_buffers];
		// Only checks the indices exist. The colour passes that read them declare their ids.
		RenderGraph::UndeclaredAccess::DeclaredIndexScope declared;
		return buffers.indices(a_out.first(buffers.count));
	}

	bool FrameBindingRegister(std::uint32_t a_register)
	{
		for (const auto& binding : RegisteredFrameBindings())
			if (binding.textureRegister == a_register)
				return true;
		return false;
	}

	namespace
	{
		/**
		 * @brief Every frame texture's graph ids once each, for the colour passes. With a_faces, the reflection faces' ids: a texture's
		 * own faces ids when it has them (per-epoch views), else its graph ids if it has a view.
		 */
		std::vector<std::string_view> GraphIdsOf(bool a_faces)
		{
			std::vector<std::string_view> ids;
			const auto add = [&](const auto& a_ids) {
				for (const auto id : a_ids)
					if (!id.empty() && std::ranges::find(ids, id) == ids.end())
						ids.push_back(id);
			};
			for (const auto& texture : RegisteredFrameTextures())
				if (a_faces && texture.FacesOwnIds())
					add(texture.facesGraphResources);
				else if (!a_faces || texture.view)
					add(texture.graphResources);
			return ids;
		}

		/** @brief The ids a graph build declared. Replaced whole, and it only ever grows. */
		struct DeclaredIds
		{
			std::vector<std::string_view> ids;
			ankerl::unordered_dense::set<std::string_view> set;
			bool Has(std::string_view a_id) const { return set.contains(a_id); }
		};
		std::atomic<std::shared_ptr<const DeclaredIds>> s_declared;
		std::atomic<std::uint64_t> s_declaredGeneration{ 0 };

		/** @brief The declared ids, kept per thread while no build adds one: the atomic load locks, and commits ask per register. */
		const std::shared_ptr<const DeclaredIds>& DeclaredNow()
		{
			thread_local std::uint64_t t_generation = ~0ull;
			thread_local std::shared_ptr<const DeclaredIds> t_declared;
			const std::uint64_t generation = s_declaredGeneration.load(std::memory_order_acquire);
			if (generation != t_generation) {
				t_declared = s_declared.load(std::memory_order_acquire);
				t_generation = generation;
			}
			return t_declared;
		}

		/**
		 * @brief The frame textures' published lookups, kept while their answers hold. The faces, colour and grass commits ask the
		 * same ones every frame. A publish, a relocation, a newly declared id or the runtime stopping drops them all. A versioned
		 * id's index and export follow its adopted version, so its answers also hold only while that version is current. Render
		 * thread: the commits ask.
		 */
		class GraphAnswers
		{
		public:
			/** @brief The id read and its descriptor. An empty read means none of the ids is read. */
			struct Answer
			{
				std::string_view read;
				std::uint32_t index = 0;
			};

			/** @brief a_id's descriptor if a build declared it and it is published. a_id must be a static string. */
			std::optional<std::uint32_t> DeclaredIndex(std::string_view a_id, const DeclaredIds* a_declared)
			{
				Refresh();
				if (const auto found = indices.find(a_id); found != indices.end() && Holds(found->second))
					return found->second.answer.read.empty() ? std::nullopt : std::optional(found->second.answer.index);
				Kept kept;
				if (a_declared && a_declared->Has(a_id)) {
					kept.Follow(PublishedResources::VersionedSlotOf(a_id));
					RenderGraph::UndeclaredAccess::DeclaredIndexScope declaredIndex;
					if (PublishedResources::ShaderResourceIndex(a_id, kept.answer.index))
						kept.answer.read = a_id;
				}
				indices.insert_or_assign(a_id, kept);
				return kept.answer.read.empty() ? std::nullopt : std::optional(kept.answer.index);
			}

			/** @brief The first declared id in a_ids whose export a_view is, and its descriptor. a_ids must be static. */
			Answer ExportIndex(const ID3D11ShaderResourceView* a_view, std::span<const std::string_view> a_ids, const DeclaredIds* a_declared)
			{
				Refresh();
				const ExportKey key{ a_view, a_ids.data(), a_ids.size() };
				if (const auto found = exports.find(key); found != exports.end() && Holds(found->second))
					return found->second.answer;
				Kept kept;
				for (const auto id : a_ids) {
					if (id.empty() || !a_declared || !a_declared->Has(id))
						continue;
					kept.Follow(PublishedResources::VersionedSlotOf(id));
					// The graph orders the declared read. Only its index comes from the build.
					RenderGraph::UndeclaredAccess::DeclaredIndexScope declaredIndex;
					if (PublishedResources::IsExportOf(a_view, id) && PublishedResources::ShaderResourceIndex(id, kept.answer.index)) {
						kept.answer.read = id;
						break;
					}
				}
				exports.insert_or_assign(key, kept);
				return kept.answer;
			}

		private:
			/** @brief An answer and the versioned slots it read, with the version each had. */
			struct Kept
			{
				static constexpr std::size_t kSlots = 16;
				Answer answer;
				std::array<const org::IVersionedResource*, kSlots> slots{};
				std::array<std::uint64_t, kSlots> numbers{};
				std::uint32_t followed = 0;
				bool unkept = false;  // read more slots than it can follow, so it is asked again every time

				void Follow(const org::IVersionedResource* a_slot)
				{
					if (!a_slot)
						return;
					if (followed == kSlots) {
						unkept = true;
						return;
					}
					slots[followed] = a_slot;
					numbers[followed++] = a_slot->CurrentNumber();
				}
			};
			struct ExportKey
			{
				const ID3D11ShaderResourceView* view;
				const std::string_view* ids;
				std::size_t count;
				bool operator==(const ExportKey&) const = default;
			};
			struct ExportKeyHash
			{
				using is_avalanching = void;
				std::uint64_t operator()(const ExportKey& a_key) const noexcept
				{
					const auto mix = [](std::uint64_t a_hash, std::uint64_t a_value) { return ankerl::unordered_dense::detail::wyhash::mix(a_hash ^ a_value, 0x9E3779B97F4A7C15ull); };
					return mix(mix(reinterpret_cast<std::uintptr_t>(a_key.view), reinterpret_cast<std::uintptr_t>(a_key.ids)), a_key.count);
				}
			};

			static bool Holds(const Kept& a_kept)
			{
				if (a_kept.unkept)
					return false;
				for (std::uint32_t i = 0; i < a_kept.followed; ++i)
					if (a_kept.slots[i]->CurrentNumber() != a_kept.numbers[i])
						return false;
				return true;
			}

			void Refresh()
			{
				const std::array<std::uint64_t, 4> now{ PublishedResources::Serial(), RenderGraph::Relocation::JournalHead(),
					s_declaredGeneration.load(std::memory_order_acquire), RenderGraphRuntime::Get().IsActive() ? 1ull : 0ull };
				if (now == holdsFor)
					return;
				holdsFor = now;
				indices.clear();
				exports.clear();
			}

			std::array<std::uint64_t, 4> holdsFor{ ~0ull, ~0ull, ~0ull, ~0ull };
			ankerl::unordered_dense::map<std::string_view, Kept> indices;
			ankerl::unordered_dense::map<ExportKey, Kept, ExportKeyHash> exports;
		};

		GraphAnswers& Answers()
		{
			static GraphAnswers s_answers;
			return s_answers;
		}

		std::string GraphTextureWho(GraphTextureReader a_reader, std::uint32_t a_register, std::string_view a_name)
		{
			switch (a_reader) {
			case GraphTextureReader::Colour:
				return fmt::format("frame texture {} (t{})", a_name, a_register);
			case GraphTextureReader::Faces:
				return fmt::format("the faces' frame texture {} (t{})", a_name, a_register);
			default:
				return std::string(a_name);
			}
		}
	}

	std::span<const std::string_view> ColourGraphTextureIds()
	{
		static const auto s_ids = GraphIdsOf(false);
		return s_ids;
	}

	std::span<const std::string_view> FacesGraphTextureIds()
	{
		static const auto s_ids = GraphIdsOf(true);
		return s_ids;
	}

	struct DeclaredReads::Entry
	{
		/** @brief The index the check compares with, and the journal's head when it was taken. A relocation gives the resource a new index. */
		struct Latch
		{
			std::atomic<std::uint32_t> index{ UINT32_MAX };  // UINT32_MAX means withdrawn at build, so nothing to compare
			std::atomic<std::uint64_t> journalAt{ 0 };
		};

		std::string id;
		org::DeclaredViewToken view;
		std::uint64_t resourceID = 0;  // the published resource the index is of, which journal entries name
		std::unique_ptr<Latch> latched = std::make_unique<Latch>();
		std::unique_ptr<std::atomic<bool>> reported = std::make_unique<std::atomic<bool>>(false);
	};

	void DeclarePublishedRead(org::RenderPassBuilder& a_builder, std::string_view a_id, DeclaredReads& a_out)
	{
		const org::SrvView view{ .variant = PublishedResources::ShaderResourceVariant(a_id) };
		DeclaredReads::Entry entry{ .id = std::string(a_id), .view = a_builder.ShaderResource(org::ResourceIdentifier(a_id), view).View() };
		// The index of the view just declared, which the commit latches. A versioned slot's index follows its versions, which change
		// only with a frame revision that records its readers against the new one (plan step 10b), so the build's index is no
		// reference for it. ORG checks those readers' version currency instead.
		RenderGraph::UndeclaredAccess::DeclaredIndexScope declared;
		// The head before the index, so a move between the two counts as after the latch.
		entry.latched->journalAt.store(RenderGraph::Relocation::JournalHead(), std::memory_order_relaxed);
		std::uint32_t latched = UINT32_MAX;
		if (PublishedResources::IsVersioned(a_id) || !PublishedResources::ShaderResourceIndex(a_id, latched))
			latched = UINT32_MAX;
		entry.latched->index.store(latched, std::memory_order_relaxed);
		entry.resourceID = PublishedResources::ResourceIdOf(a_id);
		if (!a_out.entries)
			a_out.entries = std::make_shared<std::vector<DeclaredReads::Entry>>();
		a_out.entries->push_back(std::move(entry));
	}

	void CheckDeclaredReads(const DeclaredReads& a_reads, const org::FramePreparationContext& a_preparation, std::string_view a_site)
	{
		if (!a_reads.entries)
			return;
		for (const auto& entry : *a_reads.entries) {
			const std::uint32_t latched = entry.latched->index.load(std::memory_order_acquire);
			if (latched == UINT32_MAX || entry.reported->load(std::memory_order_relaxed))
				continue;
			std::uint32_t declared = UINT32_MAX;
			try {
				declared = a_preparation.Resolve(entry.view).index;
			} catch (const std::exception& e) {
				if (!entry.reported->exchange(true, std::memory_order_relaxed))
					logger::warn("[ORG] declared view mismatch: {} reads '{}' (latched descriptor {}), and its declared view does not resolve: {}", a_site,
						entry.id, latched, e.what());
				continue;
			}
			if (declared == latched)
				continue;
			// A relocation since the latch gave the resource a new index. The commit reads the live index, so only the latch is old:
			// the declared view's index becomes the latch. The head is read first, so a move after it is checked next time.
			const auto head = RenderGraph::Relocation::JournalHead();
			const auto journalAt = entry.latched->journalAt.load(std::memory_order_acquire);
			if (entry.resourceID && RenderGraph::Relocation::MovedSince(journalAt, entry.resourceID)) {
				entry.latched->index.store(declared, std::memory_order_release);
				entry.latched->journalAt.store(head, std::memory_order_release);
				continue;
			}
			if (!entry.reported->exchange(true, std::memory_order_relaxed))
				logger::warn("[ORG] declared view mismatch: {} reads '{}' through descriptor {}, its declared view is descriptor {}", a_site, entry.id,
					latched, declared);
		}
	}

	DeclaredReads DeclareGraphTextures(org::RenderPassBuilder& a_builder, std::span<const std::string_view> a_ids)
	{
		DeclaredReads reads;
		DeclareGraphTextures(a_builder, a_ids, reads);
		return reads;
	}

	void DeclareGraphTextures(org::RenderPassBuilder& a_builder, std::span<const std::string_view> a_ids, DeclaredReads& a_out)
	{
		const auto& declared = DeclaredNow();
		std::vector<std::string_view> added;
		for (const auto id : a_ids) {
			// Only ids every graph registers, since declaring another fails the build. A skipped id is built in once published.
			if (!PublishedResources::IsDeclarable(id))
				continue;
			DeclarePublishedRead(a_builder, id, a_out);
			if (!declared || !declared->Has(id))
				added.push_back(id);
		}
		if (added.empty())
			return;
		auto next = std::make_shared<DeclaredIds>();
		if (declared)
			*next = *declared;
		for (const auto id : added)
			if (next->set.insert(id).second)
				next->ids.push_back(id);
		s_declared.store(std::move(next), std::memory_order_release);
		s_declaredGeneration.fetch_add(1, std::memory_order_acq_rel);
	}

	bool DeclaredTextureIndex(GraphTextureReader a_reader, std::uint32_t a_register, std::string_view a_name, std::span<const std::string_view> a_ids,
		std::uint32_t& a_index)
	{
		// Only an id a graph build declared, so the graph orders the read after its writes.
		const auto& declared = DeclaredNow();
		std::string_view read;
		for (const auto id : a_ids) {
			if (id.empty())
				continue;
			if (const auto index = Answers().DeclaredIndex(id, declared.get())) {
				a_index = *index;
				read = id;
				break;
			}
		}
		// Log each change once per reader and register. Commits run only on the render thread.
		constexpr auto kReaders = static_cast<std::size_t>(GraphTextureReader::Count);
		static std::array<std::array<std::string_view, kTextureRegisters>, kReaders> s_logged{};
		static std::array<std::array<bool, kTextureRegisters>, kReaders> s_seen{};
		const auto reader = static_cast<std::size_t>(a_reader);
		const auto slot = a_register < kTextureRegisters ? a_register : 0;
		if (!s_seen[reader][slot] || s_logged[reader][slot] != read) {
			s_seen[reader][slot] = true;
			s_logged[reader][slot] = read;
			const auto who = GraphTextureWho(a_reader, a_register, a_name);
			if (read.empty())
				logger::info("[DCLF] {} reads the null texture until its own graph resource is declared", who);
			else
				logger::info("[DCLF] {} reads its own graph resource '{}' (descriptor {})", who, read, a_index);
		}
		return !read.empty();
	}

	namespace
	{
		/** @brief Logs each path change once per reader and register, and returns whether a graph resource is read. Render thread. */
		bool NoteGraphTextureRead(GraphTextureReader a_reader, std::uint32_t a_register, std::string_view a_name, std::string_view a_read, std::uint32_t a_index)
		{
			constexpr auto kReaders = static_cast<std::size_t>(GraphTextureReader::Count);
			static std::array<std::array<std::string_view, kTextureRegisters>, kReaders> s_logged{};
			static std::array<std::array<bool, kTextureRegisters>, kReaders> s_seen{};
			const auto reader = static_cast<std::size_t>(a_reader);
			const auto slot = a_register < kTextureRegisters ? a_register : 0;
			if (!s_seen[reader][slot] || s_logged[reader][slot] != a_read) {
				s_seen[reader][slot] = true;
				s_logged[reader][slot] = a_read;
				const auto who = GraphTextureWho(a_reader, a_register, a_name);
				if (a_read.empty())
					logger::info("[DCLF] {} reads its D3D11 view", who);
				else
					logger::info("[DCLF] {} reads the graph resource '{}' (descriptor {})", who, a_read, a_index);
			}
			return !a_read.empty();
		}
	}

	bool GraphTextureIndex(GraphTextureReader a_reader, std::uint32_t a_register, std::string_view a_name, const ID3D11ShaderResourceView* a_view,
		std::span<const std::string_view> a_ids, std::uint32_t& a_index)
	{
		if (!a_view)
			return false;
		// Only declared ids, since an undeclared id must never be read.
		const auto answer = Answers().ExportIndex(a_view, a_ids, DeclaredNow().get());
		if (!answer.read.empty())
			a_index = answer.index;
		return NoteGraphTextureRead(a_reader, a_register, a_name, answer.read, a_index);
	}

	bool FrameTextureIndex(GraphTextureReader a_reader, std::uint32_t a_register, const FrameTextureEntry& a_texture, const ID3D11ShaderResourceView* a_view,
		std::uint32_t& a_index)
	{
		if (!a_texture.graphId)
			return GraphTextureIndex(a_reader, a_register, a_texture.name, a_view, a_texture.graphResources, a_index);
		// Only an id the registration lists, which is a static string, so the log may keep it.
		const std::string_view asked = a_texture.graphId();
		const auto listed = asked.empty() ? a_texture.graphResources.end() : std::ranges::find(a_texture.graphResources, asked);
		std::string_view read;
		if (listed != a_texture.graphResources.end()) {
			if (const auto index = Answers().DeclaredIndex(*listed, DeclaredNow().get())) {
				a_index = *index;
				read = *listed;
			}
		}
		return NoteGraphTextureRead(a_reader, a_register, a_texture.name, read, a_index);
	}


	namespace
	{
		/** @brief Static "Engine::<target>" ids for a_targets. */
		std::vector<std::string_view> EngineTargetIds(std::initializer_list<RE::RENDER_TARGET> a_targets)
		{
			static std::vector<std::unique_ptr<std::string>> s_names;  // kept for the process, because the views point into them
			std::vector<std::string_view> ids;
			for (const auto target : a_targets) {
				s_names.push_back(std::make_unique<std::string>(std::format("Engine::{}", magic_enum::enum_name(target))));
				ids.emplace_back(*s_names.back());
			}
			return ids;
		}
	}

	std::span<const std::string_view> CharacterLightGraphTextureIds()
	{
		using Target = RE::RENDER_TARGET;
		static const auto s_ids = [] {
			auto ids = EngineTargetIds({ Target::kHDR_DOWNSAMPLE0, Target::kHDR_DOWNSAMPLE1, Target::kHDR_DOWNSAMPLE2, Target::kHDR_DOWNSAMPLE3,
				Target::kHDR_DOWNSAMPLE4, Target::kHDR_DOWNSAMPLE5, Target::kHDR_DOWNSAMPLE6, Target::kHDR_DOWNSAMPLE7, Target::kHDR_DOWNSAMPLE8,
				Target::kHDR_DOWNSAMPLE9, Target::kHDR_DOWNSAMPLE10, Target::kHDR_DOWNSAMPLE11, Target::kHDR_DOWNSAMPLE12, Target::kHDR_DOWNSAMPLE13 });
			// CS's adapted luminance pair, which the post chain draws instead of the engine's last luminance level in native mode.
			for (const auto id : RenderGraph::Engine::ImageSpace::AdaptedLuminanceIds())
				ids.push_back(id);
			return ids;
		}();
		return s_ids;
	}

	bool AdaptedLuminanceTextureIndex(GraphTextureReader a_reader, std::uint32_t a_register, ID3D11ShaderResourceView* a_view, std::uint32_t& a_index)
	{
		const auto id = RenderGraph::Engine::ImageSpace::AdaptedLuminanceOf(a_view);
		if (id.empty())
			return false;
		// Only a read a graph build declared, so the graph orders it against the post stage's write.
		const auto index = Answers().DeclaredIndex(id, DeclaredNow().get());
		if (!index)
			return false;
		a_index = *index;
		static std::array<bool, static_cast<std::size_t>(GraphTextureReader::Count)> s_logged{};
		if (!std::exchange(s_logged[static_cast<std::size_t>(a_reader)], true))
			logger::info("[DCLF] {} reads CS's adapted luminance '{}' in place of the engine's target", GraphTextureWho(a_reader, a_register, "character light"), id);
		return true;
	}

	std::span<const std::string_view> ShadowMaskGraphTextureIds()
	{
		static const auto s_ids = EngineTargetIds({ RE::RENDER_TARGET::kSHADOW_MASK });
		return s_ids;
	}

	bool EngineTargetTextureIndex(GraphTextureReader a_reader, std::uint32_t a_register, std::string_view a_name, ID3D11ShaderResourceView* a_view,
		std::span<const std::string_view> a_ids, std::uint32_t& a_index)
	{
		if (!a_view)
			return false;
		// The export is the D3D11 view a handle wraps. A native handle wraps none, so it is the export itself.
		auto* view = RenderGraph::Engine::Tripwire::Unwrap(a_view);
		if (!view)
			view = a_view;
		// The engine target set publishes every target under its id at setup, so nothing is published here.
		return GraphTextureIndex(a_reader, a_register, a_name, view, a_ids, a_index);
	}

	std::span<const std::string_view> PrecipitationOcclusionGraphTextureIds()
	{
		static constexpr std::array<std::string_view, 1> s_ids{ "Engine::Depth::kPRECIPITATION_OCCLUSION_MAP" };
		return s_ids;
	}

	bool PrecipitationOcclusionTextureIndex(GraphTextureReader a_reader, std::uint32_t a_register, ID3D11ShaderResourceView* a_view, std::uint32_t& a_index)
	{
		if (!a_view)
			return false;
		// Match the D3D11 view a handle wraps. A native handle is the view itself.
		auto* view = RenderGraph::Engine::Tripwire::Unwrap(a_view);
		if (!view)
			view = a_view;
		const auto ids = PrecipitationOcclusionGraphTextureIds();
		if (GraphTextureIndex(a_reader, a_register, "precipitation occlusion", view, ids, a_index))
			return true;
		// Only the engine's own map. Skylighting swaps its map into the field while it draws, but Wetness Effects binds the engine's after that.
		auto* renderer = globals::game::renderer;
		if (!renderer)
			return false;
		constexpr auto target = RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP;
		ID3D11ShaderResourceView* field = renderer->GetDepthStencilData().depthStencils[target].depthSRV;
		ID3D11ShaderResourceView* engineView = RenderGraph::Engine::Tripwire::Unwrap(field);
		if (!engineView)
			engineView = field;
		if (!engineView || view != engineView)
			return false;
		// Once per new view. The first publish rebuilds every graph off the render thread, and the next build declares the id.
		static std::array<const void*, static_cast<std::size_t>(GraphTextureReader::Count)> s_served{};
		static bool s_publishedHere = false;
		auto& served = s_served[static_cast<std::size_t>(a_reader)];
		if (served == view)
			return false;
		served = view;
		// Skylighting's twin map swapped into the field is not the engine's map. It has its own id.
		// Publishing it under this id would replace the engine map's slot whenever the two take turns.
		namespace Targets = RenderGraph::Engine::Targets;
		const auto fieldTarget = RenderGraph::Engine::Tripwire::IsHandle(field) ? Targets::Identify(static_cast<const ID3D11View*>(field)) : std::nullopt;
		const bool engineMap = fieldTarget && fieldTarget->kind == Targets::Kind::Depth && fieldTarget->index == static_cast<std::uint16_t>(target);
		winrt::com_ptr<ID3D11Resource> resource;
		view->GetResource(resource.put());
		DxvkOrgInteropResourceInfo info{};
		const auto madeImage = [&] {
			const auto entry = RenderGraph::ImageRegistry::Find(reinterpret_cast<std::uint64_t>(info.image.image));
			return entry && entry->kind == RenderGraph::ImageRegistry::Kind::Made && entry->resource;
		};
		if (resource && !engineMap && RenderGraphRuntime::Get().DescribeResource(resource.get(), info) &&
			info.kind == DXVK_ORG_INTEROP_RESOURCE_IMAGE && madeImage())
			return false;
		const auto import = FrameAnchors::EngineTexture(FrameAnchors::Depth(static_cast<std::uint32_t>(target)));
		if (!import)
			return false;
		// Engine::Targets publishes ORG's own target under this id. Otherwise publish the import here, again with each new view.
		if (PublishedResources::IsPublished(ids[0]) && !s_publishedHere)
			return false;
		s_publishedHere = PublishedResources::PublishImport(ids[0], import, view) || s_publishedHere;
		return false;
	}
}
