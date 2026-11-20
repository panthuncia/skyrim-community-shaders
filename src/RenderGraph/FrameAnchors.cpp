#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/FrameAnchors.h"

#	include "Globals.h"
#	include "RenderGraph/Frame/BoundaryCheck.h"
#	include "RenderGraph/Frame/FrameDescription.h"
#	include "RenderGraph/Frame/FrameDriver.h"
#	include "RenderGraph/Frame/Recipe.h"
#	include "RenderGraph/Frame/RecipeTable.h"
#	include "RenderGraph/Frame/RevisionRecordings.h"
#	include "RenderGraph/Engine/LoadingScreen/LoadingScreen.h"
#	include "RenderGraph/Engine/Native/Native.h"
#	include "RenderGraph/Engine/Targets/EngineTargets.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/BuildManager.h"
#	include "RenderGraph/DxvkImageImport.h"
#	include "RenderGraph/ExternalWork.h"
#	include "RenderGraph/FrameSequence.h"
#	include "RenderGraph/FrameStaging.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/RunShapes.h"
#	include "RenderGraph/SharedConstants.h"
#	include "RenderGraph/Switches.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/PassExecutionContext.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/RenderGraphPass.h>

#	include <Resources/ExternalTextureResource.h>
#	include <Resources/PixelBuffer.h>
#	include <Resources/TextureDescription.h>
#	include <Resources/VersionedTexture.h>
#	include <rhi_helpers.h>
#	include <unordered_set>

#	include <algorithm>
#	include <array>
#	include <atomic>
#	include <bit>
#	include <chrono>
#	include <thread>
#	include <condition_variable>
#	include <map>
#	include <set>
#	include <mutex>
#	include <optional>
#	include <typeindex>

namespace FrameAnchors
{
	namespace
	{
		using Segment = RenderGraphRuntime::Segment;

		constexpr std::uint32_t kAnchors = static_cast<std::uint32_t>(Anchor::Count);
		static_assert(static_cast<std::uint32_t>(Segment::AnchorEngineCopy) - static_cast<std::uint32_t>(Segment::AnchorReflectionsPrepass) + 1 == kAnchors,
			"one segment per anchor, in the anchors' order");
		static_assert(static_cast<std::uint32_t>(Segment::Count) <= 64, "TakeSubmittedSegments holds one bit per segment");
		static_assert(kAnchors <= 32, "anchor masks are 32-bit");

		constexpr const char* kExtensionId = "cs.org.frame-anchors";

		struct AnchorInfo
		{
			const char* name;
			SegmentNames names;
			bool late;  // may be the last graph work before an upscaler, frame generation or the present
		};

		constexpr std::array<AnchorInfo, kAnchors> kInfo{ {
			// Runs once per cubemap the engine renders.
			{ "ReflectionsPrepass", { "CS anchor: reflections prepass", "CS anchor: reflections prepass inputs", "FrameAnchors::ReflectionsPrepass / ", "cs.anchor.reflections-prepass" }, false },
			// Runs once per reflection face whose sky is drawn.
			{ "SkyReflections", { "CS anchor: reflection sky", "CS anchor: reflection sky inputs", "FrameAnchors::SkyReflections / ", "cs.anchor.sky-reflections" }, false },
			{ "EarlyPrepass", { "CS anchor: early prepass", "CS anchor: early prepass inputs", "FrameAnchors::EarlyPrepass / ", "cs.anchor.early-prepass" }, false },
			{ "DepthDone", { "CS anchor: depth done", "CS anchor: depth done inputs", "FrameAnchors::DepthDone / ", "cs.anchor.depth-done" }, false },
			{ "Prepass", { "CS anchor: prepass", "CS anchor: prepass inputs", "FrameAnchors::Prepass / ", "cs.anchor.prepass" }, false },
			{ "Sky", { "CS anchor: sky", "CS anchor: sky inputs", "FrameAnchors::Sky / ", "cs.anchor.sky" }, false },
			{ "DeferredPasses", { "CS anchor: deferred passes", "CS anchor: deferred passes inputs", "FrameAnchors::DeferredPasses / ", "cs.anchor.deferred-passes" }, false },
			{ "DeferredComposite", { "CS anchor: deferred composite", "CS anchor: deferred composite inputs", "FrameAnchors::DeferredComposite / ", "cs.anchor.deferred-composite" }, false },
			{ "PostComposite", { "CS anchor: post-composite", "CS anchor: post-composite inputs", "FrameAnchors::PostComposite / ", "cs.anchor.post-composite" }, false },
			{ "PostProcessing", { "CS anchor: post-processing", "CS anchor: post-processing inputs", "FrameAnchors::PostProcessing / ", "cs.anchor.post-processing" }, true },
			{ "Upscale", { "CS anchor: upscale", "CS anchor: upscale inputs", "FrameAnchors::Upscale / ", "cs.anchor.upscale" }, true },
			{ "Tonemap", { "CS anchor: tonemap", "CS anchor: tonemap inputs", "FrameAnchors::Tonemap / ", "cs.anchor.tonemap" }, true },
			{ "FrameGeneration", { "CS anchor: frame generation", "CS anchor: frame generation inputs", "FrameAnchors::FrameGeneration / ", "cs.anchor.frame-generation" }, true },
			{ "Present", { "CS anchor: present", "CS anchor: present inputs", "FrameAnchors::Present / ", "cs.anchor.present" }, true },
			{ "Overlay", { "CS anchor: overlay", "CS anchor: overlay inputs", "FrameAnchors::Overlay / ", "cs.anchor.overlay" }, true },
			{ "HDROutput", { "CS anchor: HDR output", "CS anchor: HDR output inputs", "FrameAnchors::HDROutput / ", "cs.anchor.hdr-output" }, true },
			{ "Capture", { "CS anchor: capture", "CS anchor: capture inputs", "FrameAnchors::Capture / ", "cs.anchor.capture" }, true },
			// Runs once per shadow map slice cleared.
			{ "EngineShadowClear", { "CS anchor: engine shadow clear", "CS anchor: engine shadow clear inputs", "FrameAnchors::EngineShadowClear / ", "cs.anchor.engine-shadow-clear" }, false },
			// Runs once per reflection face batch.
			{ "EngineCubeClear", { "CS anchor: engine cube clear", "CS anchor: engine cube clear inputs", "FrameAnchors::EngineCubeClear / ", "cs.anchor.engine-cube-clear" }, false },
			// Runs once per batch of the other engine clears.
			{ "EngineFrameClear", { "CS anchor: engine frame clear", "CS anchor: engine frame clear inputs", "FrameAnchors::EngineFrameClear / ", "cs.anchor.engine-frame-clear" }, false },
			// Runs in Main::RenderDepth, FinishAccumulating and DeferredPasses.
			{ "EngineDepthCopy", { "CS anchor: engine depth copy", "CS anchor: engine depth copy inputs", "FrameAnchors::EngineDepthCopy / ", "cs.anchor.engine-depth-copy" }, false },
			// Runs once per Scaleform segment the graph draws, plus one per D3D11 part.
			{ "Interface", { "CS anchor: interface", "CS anchor: interface inputs", "FrameAnchors::Interface / ", "cs.anchor.interface" }, true },
			// Runs once per ImageSpace batch or effect call. ImageSpace.cpp's kScopeRuns has the counts.
			// Late because TAA or the upscaler can follow a batch inside the chain.
			{ "ImageSpace", { "CS anchor: image space", "CS anchor: image space inputs", "FrameAnchors::ImageSpace / ", "cs.anchor.image-space" }, true },
			// Runs a frame: the world's shadow masks, the water mask, the water, a menu scene's shadow masks and the screen blood.
			{ "EngineDraws", { "CS anchor: engine draws", "CS anchor: engine draws inputs", "FrameAnchors::EngineDraws / ", "cs.anchor.engine-draws" }, true },
			// Runs once per sun occlusion query. Usually two a frame.
			{ "EngineQueries", { "CS anchor: engine queries", "CS anchor: engine queries inputs", "FrameAnchors::EngineQueries / ", "cs.anchor.engine-queries" }, false },
			// Runs once, at the VL blur call in RenderWorldPostProcess.
			{ "VolumetricLighting", { "CS anchor: volumetric lighting", "CS anchor: volumetric lighting inputs", "FrameAnchors::VolumetricLighting / ", "cs.anchor.volumetric-lighting" }, false },
			// Runs once per decal occlusion test. Most frames have none.
			{ "DecalQueries", { "CS anchor: decal queries", "CS anchor: decal queries inputs", "FrameAnchors::DecalQueries / ", "cs.anchor.decal-queries" }, false },
			// Runs once per engine copy of kMAIN into its copy texture. Native mode only.
			{ "EngineCopy", { "CS anchor: engine copy", "CS anchor: engine copy inputs", "FrameAnchors::EngineCopy / ", "cs.anchor.engine-copy" }, true },
		} };

		namespace Table = RenderGraph::Frame::Recipes::Table;

		// The recipe table's epochs in frame order. AddToFrame puts each anchor epoch just before the next one in this order.
		constexpr auto kTimeline = Table::Timeline();

		std::uint32_t RankOf(std::uint32_t a_segment)
		{
			for (std::uint32_t i = 0; i < kTimeline.size(); ++i)
				if (static_cast<std::uint32_t>(kTimeline[i]) == a_segment)
					return i;
			return static_cast<std::uint32_t>(kTimeline.size());
		}

		std::uint32_t IndexOf(Anchor a_anchor) { return static_cast<std::uint32_t>(a_anchor); }

		std::uint32_t EpochOfAnchor(Anchor a_anchor) { return RenderGraphRuntime::EpochOf(static_cast<Segment>(SegmentOf(a_anchor))); }

		// CS stages from the recipe table. Each stage is one epoch that holds its steps' passes in order.
		// Earlier steps only stage values. The last step commits them and submits the stage's one ticket.
		constexpr std::uint32_t kDeclared = RenderGraph::Frame::kSegments;
		constexpr std::uint32_t kMaxSteps = static_cast<std::uint32_t>(Table::kMaxSteps);
		// Each step that reads CS's shared constants uploads them at its own point.
		static_assert(kMaxSteps <= SharedConstants::kUploadPoints, "a stage's steps need an upload point each");
		using Step = Table::Step;

		/** @brief A CS stage's entry of the recipe table, as FrameAnchors reads it. */
		struct DeclaredInfo
		{
			Segment segment = Segment::Count;
			const Step* steps = nullptr;
			std::uint32_t stepCount = 0;
			SegmentNames names{};
			bool opensLast = false;
			bool switchesEverySlot = false;
			Table::SegmentList commitBefore{};
		};

		constexpr std::array<DeclaredInfo, kDeclared> DeclaredFromTable()
		{
			std::array<DeclaredInfo, kDeclared> out{};
			for (std::uint32_t d = 0; d < kDeclared; ++d) {
				const auto& stage = Table::CsStage(d);
				out[d] = { RenderGraphRuntime::StageSegment(d), stage.steps.data(), Table::StepCount(stage), stage.names, stage.opensLast, stage.switchesEverySlot,
					stage.commitBefore };
			}
			return out;
		}
		constexpr std::array<DeclaredInfo, kDeclared> declaredInfo = DeclaredFromTable();

		/** @brief The anchors a declared segment has a step at in this mode, a bit each. */
		std::uint32_t StepAnchorsOf(std::uint32_t a_declared)
		{
			std::uint32_t mask = 0;
			for (std::uint32_t k = 0; k < declaredInfo[a_declared].stepCount; ++k)
				if (declaredInfo[a_declared].steps[k].anchor != Anchor::Count)
					mask |= 1u << static_cast<std::uint32_t>(declaredInfo[a_declared].steps[k].anchor);
			return mask;
		}

		/** @brief The anchors a declared segment holds wholly in this mode, a bit each. */
		std::uint32_t WholeAnchorsOf(std::uint32_t a_declared)
		{
			std::uint32_t mask = 0;
			for (std::uint32_t k = 0; k < declaredInfo[a_declared].stepCount; ++k)
				if (declaredInfo[a_declared].steps[k].anchor != Anchor::Count && declaredInfo[a_declared].steps[k].whole)
					mask |= 1u << static_cast<std::uint32_t>(declaredInfo[a_declared].steps[k].anchor);
			return mask;
		}

		/** @brief The step of a declared segment at an anchor (the first one), or kMaxSteps. */
		std::uint32_t StepAt(std::uint32_t a_declared, Anchor a_anchor)
		{
			for (std::uint32_t k = 0; k < declaredInfo[a_declared].stepCount; ++k)
				if (declaredInfo[a_declared].steps[k].anchor == a_anchor)
					return k;
			return kMaxSteps;
		}

		bool HasPassesAt(const ClientDesc& a_client, Anchor a_anchor)
		{
			return std::ranges::any_of(a_client.passes, [&](const PassDesc& a_pass) { return a_pass.anchor == a_anchor; });
		}

		/** @brief Whether a step holds a client. A whole step also holds diagnostic clients. */
		bool StepHolds(const Step& a_step, const ClientDesc& a_client)
		{
			if (a_step.anchor == Anchor::Count || !HasPassesAt(a_client, a_step.anchor))
				return false;
			return Table::Has(a_step.clients, a_client.id) || (a_step.whole && Table::Has(Table::kDiagnosticClients, a_client.id));
		}

		/** @brief The range of a_client's instances this step makes. */
		std::pair<std::uint32_t, std::uint32_t> StepInstances(const Step& a_step, const ClientDesc& a_client)
		{
			const std::uint32_t max = std::min<std::uint32_t>(a_client.maxInstances, 255);
			const std::uint32_t first = std::min<std::uint32_t>(a_step.firstInstance, max);
			const std::uint32_t count = a_step.instanceCount ? std::min<std::uint32_t>(a_step.instanceCount, max - first) : max - first;
			return { first, count };
		}

		/** @brief Whether a built client is a_desc's registration, by desc or by id for matchesById clients. */
		bool SameClient(const std::shared_ptr<const ClientDesc>& a_built, const std::shared_ptr<const ClientDesc>& a_desc)
		{
			return a_built == a_desc || (a_built && a_desc && a_built->matchesById && a_built->id == a_desc->id);
		}

		/** @brief Whether a declared segment ends at a point (its last step is no anchor's hook). */
		bool PointEnded(std::uint32_t a_declared) { return declaredInfo[a_declared].steps[declaredInfo[a_declared].stepCount - 1].point; }

		/** @brief The anchor whose hook ends a declared segment. */
		Anchor EndTap(std::uint32_t a_declared) { return declaredInfo[a_declared].steps[declaredInfo[a_declared].stepCount - 1].anchor; }

		std::uint32_t EpochOfDeclared(std::uint32_t a_declared) { return RenderGraphRuntime::EpochOf(declaredInfo[a_declared].segment); }

		/** @brief The versions a segment recording binds. Null versions mean the current ones. The serial tells sets apart in the cache. */
		struct RecordVersions
		{
			std::shared_ptr<const org::IResourceVersions> versions;
			std::uint64_t serial = 0;
		};

		/** @brief The frame description fields a segment recording is made for. Passes of clients that are off record only barriers. */
		struct SegmentHostData final : org::IHostExecutionData
		{
			std::uint32_t segment = 0;
			RenderGraph::Frame::SegmentFields fields;
			// The versions it binds, which may be pending. Null binds the current ones.
			std::shared_ptr<const org::IResourceVersions> versions;
			const void* TryGet(std::type_index a_type) const noexcept override
			{
				if (a_type == std::type_index(typeid(SegmentHostData)))
					return this;
				if (a_type == std::type_index(typeid(org::IResourceVersions)))
					return versions.get();
				return nullptr;
			}
		};

		/** @brief One recording of a segment, for one description and one frame slot.
		*  Recording waits for no GPU work because the slot last ran four frames ago. */
		struct SegmentRecording
		{
			std::uint64_t hash = 0;
			std::uint32_t slot = 0;
			std::uint64_t versionsSerial = 0;  // RecordVersions::serial; 0 for the current versions
			RenderGraph::Frame::SegmentFields fields;
			std::shared_ptr<const org::GraphHost::EpochRecording> recording;  // set once the host's thread recorded every copy
			bool done = false; // recorded, failed or dropped
			bool dropped = false; // a graph build dropped it before recording. Not a failure, it is asked for again
			std::string error;     // why it failed or was dropped
			std::uint64_t lastFrame = 0;                                     // the last frame that asked for it
		};

		/** @brief A segment's recordings for its last kKeep descriptions per slot, so a toggling field records nothing new. */
		struct SegmentRecordings
		{
			static constexpr std::size_t kKeep = 6 * RenderGraphRuntime::kGameFramesInFlight;
			std::mutex mutex;  // the host's thread completes requests under it
			std::vector<std::shared_ptr<SegmentRecording>> entries;
			RenderGraph::Frame::SegmentFields last{}; // the last description's fields, to log why a new one differs
			bool any = false;
			// Bumped when a kept recording binds a version that is no longer current. Render owner.
			std::uint64_t versionGeneration = 0;
		};

		/**
		* @brief Which passes a stage's build includes. A left-out pass gets no barriers and no recording.
		* Unlisted clients are present whole. Instances are never cut because the engine picks counts late.
		*/
		struct BuildKey
		{
			struct Client
			{
				std::shared_ptr<const ClientDesc> desc;
				bool present = true;
				std::array<std::uint64_t, 4> rows{}; // rows present, a bit per row
			};
			bool all = true; // every pass, before the stage's first key or with absence off
			std::vector<Client> clients;
			std::uint32_t foreign = 0; // module steps present, a bit per step
			std::uint64_t serial = 0;   // keys are numbered as they are made
			std::uint64_t frame = 0;    // the frame whose Begin made it

			const Client* Find(const ClientDesc* a_desc) const
			{
				for (const auto& client : clients)
					if (client.desc.get() == a_desc)
						return &client;
				return nullptr;
			}
		};

		/** @brief Whether a key has a pass present. A null key has every pass. a_row is -1 for none. */
		bool KeyHas(const BuildKey* a_key, const ClientDesc& a_client, std::int32_t a_row)
		{
			if (!a_key || a_key->all)
				return true;
			const auto* client = a_key->Find(&a_client);
			if (!client)
				return true;
			if (!client->present)
				return false;
			return a_row < 0 || a_row >= 256 || (client->rows[static_cast<std::size_t>(a_row) / 64] >> (static_cast<std::uint32_t>(a_row) % 64) & 1u) != 0;
		}

		/** @brief The rows a client's passes carry, a bit each. */
		std::array<std::uint64_t, 4> RowsOf(const ClientDesc& a_client)
		{
			std::array<std::uint64_t, 4> rows{};
			for (const auto& pass : a_client.passes)
				if (pass.row >= 0 && pass.row < 256)
					rows[static_cast<std::size_t>(pass.row) / 64] |= 1ull << (static_cast<std::uint32_t>(pass.row) % 64);
			return rows;
		}

		/** @brief A client's part of a key, whether it is present and which rows. */
		std::pair<bool, std::array<std::uint64_t, 4>> KeyPart(const BuildKey* a_key, const ClientDesc& a_client)
		{
			const auto own = RowsOf(a_client);
			if (!a_key || a_key->all)
				return { true, own };
			const auto* client = a_key->Find(&a_client);
			if (!client)
				return { true, own };
			if (!client->present)
				return { false, {} };
			std::array<std::uint64_t, 4> rows{};
			for (std::size_t w = 0; w < rows.size(); ++w)
				rows[w] = own[w] & client->rows[w];
			return { true, rows };
		}

		/** @brief The module step bits of a_steps that a key has present. */
		std::uint32_t KeyForeign(const BuildKey* a_key, std::uint32_t a_steps)
		{
			return !a_key || a_key->all ? a_steps : a_key->foreign & a_steps;
		}

		/** @brief Text listing what one key has that the other lacks. Empty when equal. */
		std::string DiffKeys(const BuildKey* a_before, const BuildKey* a_after, const std::vector<std::shared_ptr<const ClientDesc>>& a_clients, std::uint32_t a_steps)
		{
			std::string added, dropped;
			const auto note = [](std::string& a_text, std::string_view a_what) { a_text += fmt::format("{}{}", a_text.empty() ? "" : ", ", a_what); };
			for (const auto& client : a_clients) {
				const auto [wasOn, wasRows] = KeyPart(a_before, *client);
				const auto [isOn, isRows] = KeyPart(a_after, *client);
				if (wasOn != isOn) {
					note(isOn ? added : dropped, client->id);
					continue;
				}
				std::uint32_t plus = 0, minus = 0;
				for (std::size_t w = 0; w < 4; ++w) {
					plus += static_cast<std::uint32_t>(std::popcount(isRows[w] & ~wasRows[w]));
					minus += static_cast<std::uint32_t>(std::popcount(wasRows[w] & ~isRows[w]));
				}
				if (plus)
					note(added, fmt::format("{} rows of {}", plus, client->id));
				if (minus)
					note(dropped, fmt::format("{} rows of {}", minus, client->id));
			}
			const auto plusSteps = KeyForeign(a_after, a_steps) & ~KeyForeign(a_before, a_steps);
			const auto minusSteps = KeyForeign(a_before, a_steps) & ~KeyForeign(a_after, a_steps);
			if (plusSteps)
				note(added, fmt::format("module steps {:#x}", plusSteps));
			if (minusSteps)
				note(dropped, fmt::format("module steps {:#x}", minusSteps));
			std::string text;
			if (!added.empty())
				text += fmt::format("present now: {}", added);
			if (!dropped.empty())
				text += fmt::format("{}absent now: {}", text.empty() ? "" : "; ", dropped);
			return text;
		}

		/** @brief Whether every pass a_inner has present is present in a_outer too, over a_clients and a_steps module steps. */
		bool KeyCovers(const BuildKey* a_outer, const BuildKey* a_inner, const std::vector<std::shared_ptr<const ClientDesc>>& a_clients, std::uint32_t a_steps)
		{
			for (const auto& client : a_clients) {
				const auto [outerOn, outerRows] = KeyPart(a_outer, *client);
				const auto [innerOn, innerRows] = KeyPart(a_inner, *client);
				if (innerOn && !outerOn)
					return false;
				for (std::size_t w = 0; w < 4; ++w)
					if (innerRows[w] & ~outerRows[w])
						return false;
			}
			return (KeyForeign(a_inner, a_steps) & ~KeyForeign(a_outer, a_steps)) == 0;
		}

		/** @brief Whether a key includes passes only because it predates their client's registration. */
		bool KeyProvisional(const BuildKey* a_key, const std::vector<std::shared_ptr<const ClientDesc>>& a_clients)
		{
			if (!a_key || a_key->all)
				return true;
			return std::ranges::any_of(a_clients, [&](const auto& a_client) { return !a_key->Find(a_client.get()); });
		}

		/** @brief A stage's pass use for its next key, on a clock that only ticks in frames that run the stage. Render owner. */
		struct KeyUse
		{
			struct Client
			{
				std::shared_ptr<const ClientDesc> desc;
				double used = -1e30;
				std::array<double, 256> rows;
				Client() { rows.fill(-1e30); }
			};
			double clock = 0.0;
			std::chrono::steady_clock::time_point last{};
			std::vector<Client> clients;
			std::array<double, 32> foreign;
			std::uint64_t requestedSerial = 0;  // the last key a build was asked for with, and the frame it was asked at
			std::uint64_t requestedFrame = 0;
			std::uint64_t switchedSerial = 0; // the last requested key whose build switched in
			KeyUse() { foreign.fill(-1e30); }
		};

		/** @brief D3D11 textures dropped on any thread, released later on the render thread. */
		struct RetiredTextures
		{
			std::mutex mutex;
			std::vector<winrt::com_ptr<ID3D11Texture2D>> textures;
			std::atomic<bool> any{ false };
		};

		RetiredTextures& Retired()
		{
			// Never destroyed: entries let go of their textures during static destruction too.
			static auto* retired = new RetiredTextures;
			return *retired;
		}

		/** @brief Render thread. Releases the D3D11 textures dropped since the last call once D3D11 is done with them. */
		void ServiceRetiredTextures()
		{
			auto& retired = Retired();
			if (!retired.any.load(std::memory_order_acquire))
				return;
			auto textures = std::make_shared<std::vector<winrt::com_ptr<ID3D11Texture2D>>>();
			{
				const std::lock_guard lock(retired.mutex);
				textures->swap(retired.textures);
				retired.any.store(false, std::memory_order_release);
			}
			RenderGraphRuntime::Get().ReleaseAfterD3D11(std::move(textures));
		}

		/**
		* @brief An image's graph slot. An id import lives while an id names it. A new texture of the same shape is its next version.
		* A loose slot, from ShareImage before any id import, lives while its holders hold it. Each version owns this entry.
		*/
		struct EngineImport
		{
			winrt::com_ptr<ID3D11Texture2D> texture;  // the current version's D3D11 texture
			std::shared_ptr<org::VersionedTexture> slot;     // an id import's
			std::weak_ptr<org::VersionedTexture> looseSlot;  // a loose one's
			std::string id;                                  // the id (or the loose holder's name) it was made under, for the logs
			std::uint64_t image = 0;                         // the current version's VkImage (ID3D12Resource on D3D12)
			DxvkOrgInteropImageInfo info{}; // to import it again with more views
			org::TextureDescription views; // the current version's views, compared with a new importer's

			std::shared_ptr<org::VersionedTexture> Slot() const { return slot ? slot : looseSlot.lock(); }

			EngineImport() = default;
			EngineImport(const EngineImport&) = default;
			EngineImport& operator=(const EngineImport&) = default;

			/** @brief Sends the D3D11 texture to the render thread, since the last owner may drop it on any thread. */
			~EngineImport()
			{
				if (!texture)
					return;
				auto& retired = Retired();
				const std::lock_guard lock(retired.mutex);
				retired.textures.push_back(std::move(texture));
				retired.any.store(true, std::memory_order_release);
			}
		};

		/** @brief One pass of the build. The client keeps the desc alive. */
		struct ScheduledPass
		{
			std::shared_ptr<const ClientDesc> client;
			const PassDesc* pass = nullptr;
		};

		/** @brief What every graph build makes the anchors' passes and resources from. Immutable once published. */
		struct Snapshot
		{
			std::uint64_t serial = 0;                                // State::serial when it was published
			std::vector<std::shared_ptr<const ClientDesc>> clients; // registration order
			std::vector<ScheduledPass> passes;                       // by anchor, then order key, then registration
			std::vector<std::pair<std::string, std::shared_ptr<org::VersionedTexture>>> imports;  // each id's slot
			// Each declared segment's clients in registration order, made once at Publish.
			std::array<std::vector<std::shared_ptr<const ClientDesc>>, kDeclared> segmentClients;
		};

		/** @brief A registered client and what the anchors keep about it. Render thread. */
		struct Client
		{
			std::shared_ptr<const ClientDesc> desc;
			// The first snapshot holding its passes and resources as they are now. Graphs built from it or later run them.
			std::uint64_t serial = 0;
			// Per pass, the snapshot a pending one-shot pass was armed in. 0 once it ran.
			std::vector<std::uint64_t> armed;
			// The declared segments that hold it, a bit each. Taps check this instead of comparing client ids.
			const ClientDesc* heldOf = nullptr;
			std::uint64_t held = 0;
		};

		/**
		* @brief Gates of a registration that left the graphs. Older graphs still hold its passes and replay them,
		* so every run closes these gates to stop them reading latch slots nobody wrote.
		*/
		struct RetiredGates
		{
			std::vector<std::shared_ptr<RenderGraph::RunGate>> gates;
			std::uint32_t mask = 0;    // the anchors its passes were at
			std::uint64_t serial = 0;  // the first snapshot without them
		};

		/** @brief The anchors' state. Render thread, except the snapshot and built serials, which builds also touch. */
		struct State
		{
			std::array<bool, kAnchors> reserved{};
			bool frozen = false;  // AddToFrame ran: the reservations are the frame's epochs
			org::GraphHost* host = nullptr;
			std::vector<Client> clients;
			std::map<std::string, std::shared_ptr<EngineImport>, std::less<>> imports; // by graph id. Ids of one image share an entry
			std::vector<std::weak_ptr<EngineImport>> loose;                             // ShareImage's slots no id names
			std::uint64_t serial = 0;                                  // bumped by every Publish
			std::function<void()> lateHook;
			std::vector<RetiredGates> retired;
			// Per anchor, runs since the last Present seal, and the most ever in one frame for LogRunStats.
			std::array<std::uint32_t, kAnchors> runsThisFrame{};
			std::array<std::uint64_t, kAnchors> runsSeal{};
			std::array<std::atomic<std::uint32_t>, kAnchors> peakRuns{};
			std::atomic<std::shared_ptr<const Snapshot>> snapshot;
			std::array<std::atomic<std::uint64_t>, kAnchors> built{};  // the snapshot each anchor's graph was last built from
			// Each anchor's last build snapshot and the one before. OnEpochFailed drops the failed build's clients,
			// and a failed build that kept the old graph restores the previous snapshot.
			std::array<std::atomic<std::shared_ptr<const Snapshot>>, kAnchors> builtSnapshots{};
			std::array<std::atomic<std::shared_ptr<const Snapshot>>, kAnchors> previousSnapshots{};
			// Declared segments. Anchors a segment holds have no epoch of their own.
			std::array<bool, kDeclared> declaredEpochs{};
			std::uint32_t declaredMembers = 0;
			std::array<std::atomic<std::shared_ptr<const Snapshot>>, kDeclared> declaredSnapshots{};
			std::array<std::atomic<std::shared_ptr<const Snapshot>>, kDeclared> declaredPrevious{};
			std::array<SegmentRecordings, kDeclared> declaredRecordings;
			// Each segment graph's passes recorded ahead, from its last attached build: their revisions go into its description.
			std::array<std::atomic<std::shared_ptr<std::vector<std::weak_ptr<org::RenderPass>>>>, kDeclared> declaredPasses{};
			// A stage's staged build and its passes recorded ahead. BeginFrame records it for every slot and switches it in whole.
			std::array<std::atomic<std::shared_ptr<const Snapshot>>, kDeclared> stagedSnapshots{};
			std::array<std::atomic<std::shared_ptr<std::vector<std::weak_ptr<org::RenderPass>>>>, kDeclared> stagedPasses{};
			// Absent-pass keys per stage. A null key means every pass. Builds read nextKeys when they start. The rest is render owner.
			std::array<std::atomic<std::shared_ptr<const BuildKey>>, kDeclared> nextKeys{};
			std::array<std::atomic<std::shared_ptr<const BuildKey>>, kDeclared> declaredKeys{};
			std::array<std::atomic<std::shared_ptr<const BuildKey>>, kDeclared> stagedKeys{};
			std::array<KeyUse, kDeclared> keyUse;
			std::uint64_t keySerial = 0;
			bool describingKey = false; // BeginFrame is describing a stage's key
			// While a client's rows are described for a frame: the build's key the frame is described against, and the client (KeyHasRow).
			const BuildKey* describingAgainst = nullptr;
			const ClientDesc* describingClient = nullptr;
			RenderGraph::Frame::Kind keyKind = RenderGraph::Frame::Kind::Count;  // the last frame's kind, for the key's kind switch
			// Each stage's frame inputs, staged by its taps until the commit. stagedFrame is 0 when nothing is staged. Render owner.
			std::array<std::uint64_t, kDeclared> stagedFrame{};
			std::array<org::RunRef, kDeclared> stagingRun{};
			std::array<std::vector<std::pair<std::shared_ptr<const ClientDesc>, Point>>, kDeclared> stagedExecuted{};
			// Per segment and frame slot, the description, recording and snapshot the slot's frames run on.
			// A frame takes its slot's schedule at Begin and keeps it to the end. Render owner.
			struct Scheduled
			{
				RenderGraph::Frame::SegmentFields fields;
				std::shared_ptr<const org::GraphHost::EpochRecording> recording;
				std::shared_ptr<const Snapshot> built;
			};
			std::array<std::array<Scheduled, RenderGraphRuntime::kGameFramesInFlight>, kDeclared> schedule{};
			// This frame's schedule. Null when the segment is absent. Render owner.
			std::array<std::shared_ptr<const org::GraphHost::EpochRecording>, kDeclared> frameRecording{};
			std::array<std::shared_ptr<const Snapshot>, kDeclared> frameBuilt{};
			std::array<RenderGraph::Frame::SegmentFields, kDeclared> frameWanted{};
			// The description a slot last began to wait for, per segment, and the frame it did: a switch's lag counts from it.
			std::array<std::uint64_t, kDeclared> waitHash{};
			std::array<std::uint64_t, kDeclared> waitFrame{};
			// Whether this frame's recording bound the current versions at Begin.
			// A version published after Begin is a mismatch. One published before it lags until the new recording is ready.
			std::array<bool, kDeclared> frameVersionsCurrent{};
			bool inBegin = false; // BeginFrame is choosing schedules. A recording asked for outside it is a mid-frame re-record
			std::array<std::uint64_t, kDeclared> endedFrame{};
			// Taps so far of each step in the held run. Reset when the frame's first tap takes the ticket.
			std::array<std::array<std::uint32_t, kMaxSteps>, kDeclared> stepTaps{};
			std::uint32_t tapInstance = 0;  // the instance the commits running now write (TapInstance)
			std::atomic<bool> stagesOpen{ false }; // a stage holds uncommitted values. Lets BeforeGpuWork check without the mutex
			std::atomic<std::uint32_t> stagesOpenMask{ 0 }; // a bit per declared segment, for the residue census
			// A stage commit is submitting its block. The census and StagesHoldValues do not count that submission.
			std::atomic<bool> submittingStage{ false };
			// GPU work submitted between a stage's first tap and its commit, by reason. It runs ahead of the stage's work on the GPU.
			std::vector<std::pair<const char*, std::uint64_t>> insideHeld;
			std::uint64_t insideHeldLines = 0;
			// The epoch whose run was already counted. Its submission calls BeforeGpuWork again, which then counts nothing. UINT32_MAX for none.
			std::uint32_t insideHeldCounted = UINT32_MAX;
		};

		State& Anchors()
		{
			static State s_state;
			return s_state;
		}

		/**
		* @brief With CS_ORG_LOADING_THREAD=1, guards the anchors' state against a feature registering during a load.
		* Recursive since run callbacks may register. Never take Renderer::Lock under it. The host's thread never takes it.
		*/
		std::unique_lock<std::recursive_mutex> Guard()
		{
			if (!RenderGraph::Engine::LoadingScreen::Requested())
				return {};
			static std::recursive_mutex s_mutex;
			return std::unique_lock(s_mutex);
		}

		bool HasPasses(const ClientDesc& a_client, Anchor a_anchor)
		{
			return std::ranges::any_of(a_client.passes, [&](const PassDesc& a_pass) { return a_pass.anchor == a_anchor; });
		}

		/** @brief Whether the client is in the graphs. One that reads the shared constants waits until they exist. */
		bool InGraphs(const ClientDesc& a_client)
		{
			return !a_client.sharedConstants || SharedConstants::Ready();
		}

		/** @brief Whether a client at the anchor reads the shared constants, so the epoch must upload them. */
		bool UsesSharedConstants(const std::vector<std::shared_ptr<const ClientDesc>>& a_clients, Anchor a_anchor)
		{
			return std::ranges::any_of(a_clients, [&](const auto& a_client) { return a_client->sharedConstants && HasPasses(*a_client, a_anchor); });
		}

		/** @brief Whether the client has a one-shot pass at the anchor that has not run yet. */
		bool HasPendingOnce(const Client& a_client, Anchor a_anchor)
		{
			for (std::size_t i = 0; i < a_client.desc->passes.size(); ++i)
				if (a_client.armed[i] && a_client.desc->passes[i].anchor == a_anchor)
					return true;
			return false;
		}

		Client* FindClient(State& a_state, std::string_view a_id)
		{
			const auto found = std::ranges::find_if(a_state.clients, [&](const Client& a_client) { return a_client.desc->id == a_id; });
			return found != a_state.clients.end() ? &*found : nullptr;
		}

		/** @brief The anchor of an anchor epoch's graph, or Count. */
		Anchor AnchorOfEpoch(std::uint32_t a_epoch)
		{
			for (std::uint32_t i = 0; i < kAnchors; ++i)
				if (a_epoch == EpochOfAnchor(static_cast<Anchor>(i)))
					return static_cast<Anchor>(i);
			return Anchor::Count;
		}

		/** @brief The declared segment of a segment epoch's graph, or kDeclared. */
		std::uint32_t DeclaredOfEpoch(std::uint32_t a_epoch)
		{
			for (std::uint32_t d = 0; d < kDeclared; ++d)
				if (Anchors().declaredEpochs[d] && a_epoch == EpochOfDeclared(d))
					return d;
			return kDeclared;
		}

		/** @brief The stage whose epoch holds the anchor's passes, or kDeclared when it has its own epoch. */
		std::uint32_t DeclaredOfAnchor(const State& a_state, std::uint32_t a_index)
		{
			if (!(a_state.declaredMembers >> a_index & 1u))
				return kDeclared;
			for (std::uint32_t d = 0; d < kDeclared; ++d)
				if (a_state.declaredEpochs[d] && (WholeAnchorsOf(d) >> a_index & 1u))
					return d;
			return kDeclared;
		}

		/** @brief The snapshot the anchor's own graph was built from. Its own graph always holds its passes. */
		std::shared_ptr<const Snapshot> OwnSnapshot(const State& a_state, std::uint32_t a_index)
		{
			return a_state.builtSnapshots[a_index].load(std::memory_order_acquire);
		}

		/** @brief The serial the anchor's graphs were built from. For an anchor without its own epoch, the oldest stage's. */
		std::uint64_t BuiltSerial(const State& a_state, std::uint32_t a_index)
		{
			return a_state.built[a_index].load(std::memory_order_acquire);
		}

		/** @brief The epochs whose graphs hold the passes of the anchors in a_mask. */
		std::vector<std::uint32_t> EpochsOf(const State& a_state, std::uint32_t a_mask)
		{
			std::vector<std::uint32_t> epochs;
			for (std::uint32_t i = 0; i < kAnchors; ++i) {
				if (!(a_mask >> i & 1u) || !a_state.reserved[i])
					continue;
				// A declared segment's member has no epoch of its own: its passes are in the segments' that hold it.
				if (a_state.declaredMembers >> i & 1u) {
					for (std::uint32_t d = 0; d < kDeclared; ++d)
						if (a_state.declaredEpochs[d] && (StepAnchorsOf(d) >> i & 1u) && std::ranges::find(epochs, EpochOfDeclared(d)) == epochs.end())
							epochs.push_back(EpochOfDeclared(d));
					continue;
				}
				epochs.push_back(EpochOfAnchor(static_cast<Anchor>(i)));
				// A shared anchor whose named clients a segment holds: the segment's graph holds their passes too.
				for (std::uint32_t d = 0; d < kDeclared; ++d)
					if (a_state.declaredEpochs[d] && (StepAnchorsOf(d) >> i & 1u) && std::ranges::find(epochs, EpochOfDeclared(d)) == epochs.end())
						epochs.push_back(EpochOfDeclared(d));
			}
			return epochs;
		}

		bool SegmentHolds(std::uint32_t a_declared, const ClientDesc& a_client);

		/** @brief Publishes the clients and imports as they are now, for the next builds. */
		void Publish(State& a_state)
		{
			auto snapshot = std::make_shared<Snapshot>();
			snapshot->serial = ++a_state.serial;
			for (const auto& client : a_state.clients) {
				if (!InGraphs(*client.desc))
					continue;
				snapshot->clients.push_back(client.desc);
				for (std::size_t i = 0; i < client.desc->passes.size(); ++i) {
					const auto& pass = client.desc->passes[i];
					// A one-shot pass that ran is left out until a new registration of its client.
					if (!pass.once || client.armed[i])
						snapshot->passes.push_back({ client.desc, &pass });
				}
			}
			// Stable: equal keys keep the registration order, and each client's own.
			std::ranges::stable_sort(snapshot->passes, [](const ScheduledPass& a, const ScheduledPass& b) {
				if (a.pass->anchor != b.pass->anchor)
					return a.pass->anchor < b.pass->anchor;
				return a.pass->order < b.pass->order;
			});
			for (const auto& [id, import] : a_state.imports)
				snapshot->imports.emplace_back(id, import->slot);
			for (std::uint32_t d = 0; d < kDeclared; ++d)
				for (const auto& client : snapshot->clients)
					if (SegmentHolds(d, *client))
						snapshot->segmentClients[d].push_back(client);
			a_state.snapshot.store(std::move(snapshot), std::memory_order_release);
		}

		/** @brief The client's passes and resources, and its pending one-shot passes, as of the snapshot just published. */
		void MarkCurrent(const State& a_state, Client& a_client)
		{
			a_client.serial = a_state.serial;
			for (auto& armed : a_client.armed)
				if (armed)
					armed = a_state.serial;
		}

		/** @brief Rebuilds the anchors' epochs at their next execution. a_reason is logged. */
		void Rebuild(State& a_state, std::uint32_t a_anchorMask, std::string_view a_reason)
		{
			if (!a_state.host)
				return;
			const auto reason = fmt::format("frame anchors: {}", a_reason);
			for (const auto epoch : EpochsOf(a_state, a_anchorMask))
				RenderGraph::Builds::Request(epoch, reason);
		}

		/** @brief Rebuild for a resource identity change that no version covers, such as a replaced import. */
		void StructuralRebuild(State& a_state, std::uint32_t a_anchorMask, std::string_view a_reason)
		{
			if (!a_state.host)
				return;
			const auto epochs = EpochsOf(a_state, a_anchorMask);
			if (!epochs.empty())
				GraphResources::RequestStructuralRebuild(fmt::format("frame anchors: {}", a_reason), epochs);
		}

		std::uint32_t AnchorMask(const ClientDesc& a_client)
		{
			std::uint32_t mask = 0;
			for (const auto& pass : a_client.passes)
				if (IndexOf(pass.anchor) < kAnchors)
					mask |= 1u << IndexOf(pass.anchor);
			return mask;
		}

		/** @brief Closes a departed registration's gates at every run until its anchors are rebuilt without it. a_kept belongs to the replacement. */
		void RetireGates(State& a_state, const ClientDesc& a_client, const std::vector<std::shared_ptr<RenderGraph::RunGate>>& a_kept = {})
		{
			std::vector<std::shared_ptr<RenderGraph::RunGate>> gates;
			for (const auto& gate : a_client.gates)
				if (gate && std::ranges::find(a_kept, gate) == a_kept.end())
					gates.push_back(gate);
			if (!gates.empty())
				a_state.retired.push_back({ std::move(gates), AnchorMask(a_client), a_state.serial });
		}

		/** @brief Retired gates whose anchors' graphs were all built without their passes are let go. */
		void PruneRetired(State& a_state)
		{
			const auto builtFrom = [](const std::shared_ptr<const Snapshot>& a_built) { return a_built ? a_built->serial : 0; };
			std::erase_if(a_state.retired, [&](const RetiredGates& a_retired) {
				for (std::uint32_t i = 0; i < kAnchors; ++i) {
					if (!(a_retired.mask >> i & 1u) || !a_state.reserved[i])
						continue;
					if (a_state.built[i].load(std::memory_order_acquire) < a_retired.serial)
						return false;
					// A declared segment's graph holds its members' passes.
					for (std::uint32_t d = 0; d < kDeclared; ++d)
						if (a_state.declaredEpochs[d] && (StepAnchorsOf(d) >> i & 1u) &&
							builtFrom(a_state.declaredSnapshots[d].load(std::memory_order_acquire)) < a_retired.serial)
							return false;
				}
				return true;
			});
		}

		/** @brief The anchor's submitted runs since the last Present seal. */
		std::uint32_t& RunsSinceSeal(State& a_state, std::uint32_t a_index)
		{
			const std::uint64_t seals = a_state.host->Seals();
			if (a_state.runsSeal[a_index] != seals) {
				a_state.runsSeal[a_index] = seals;
				a_state.runsThisFrame[a_index] = 0;
			}
			return a_state.runsThisFrame[a_index];
		}

		/** @brief Whether the client waits for one of its anchors' graphs to be built with its registration. */
		bool WaitsToJoin(const State& a_state, const Client& a_client)
		{
			if (!InGraphs(*a_client.desc))
				return false;
			const std::uint32_t mask = AnchorMask(*a_client.desc);
			for (std::uint32_t i = 0; i < kAnchors; ++i)
				if ((mask >> i & 1u) && a_state.reserved[i] && a_client.serial > BuiltSerial(a_state, i))
					return true;
			return false;
		}

		/** @brief The ids of a_clients, comma separated, for a failure's message. */
		std::string IdsOf(const std::vector<std::shared_ptr<const ClientDesc>>& a_clients)
		{
			std::string ids;
			for (const auto& client : a_clients)
				ids += fmt::format("{}{}", ids.empty() ? "" : ", ", client->id);
			return ids.empty() ? std::string("none still registered") : ids;
		}

		/** @brief Reports failed clients. Development builds stop, since a graph mode has no D3D11 path to return them to.
		*  Release builds log it, and the already removed clients draw nothing. */
		void FailClients(std::string_view a_what, std::string_view a_why)
		{
			const auto message = fmt::format("Community Shaders: {}: {}", a_what, a_why);
			if (RenderGraph::Frame::Driver::MismatchFatal()) {
				logger::critical("[ORG] frame anchors: {}. Development builds stop here (CS_ORG_MISMATCH_FATAL=0 logs it instead).", message);
				spdlog::default_logger()->flush();
				stl::report_and_fail(message);
			}
			logger::error("[ORG] frame anchors: {}. The clients are removed and draw nothing.", message);
		}

		/** @brief Whether gpuGated passes record ahead and gate on the GPU through indirect counts. Native mode, unless CS_ORG_GPU_GATES=0. */
		bool GpuGates()
		{
			static const bool on = Switches::Number("CS_ORG_GPU_GATES", 1) != 0;
			return on && RenderGraph::Engine::Native::Active();
		}

		/** @brief Fails the clients of a failed build that are still registered as they were in a_failed. Render thread.
		*  Release builds remove them and rebuild their anchors without them. */
		void DropFailedClients(std::uint32_t a_anchors, const char* a_name, const std::shared_ptr<const Snapshot>& a_failed, const std::string& a_error)
		{
			auto lock = Guard();
			auto& state = Anchors();
			std::vector<std::shared_ptr<const ClientDesc>> dropped;
			std::uint32_t mask = a_anchors;
			if (a_failed)
				for (const auto& client : a_failed->clients) {
					if (!(AnchorMask(*client) & a_anchors))
						continue;
					const auto found = std::ranges::find_if(state.clients, [&](const Client& a_client) { return a_client.desc == client; });
					if (found == state.clients.end())
						continue;
					mask |= AnchorMask(*client);
					dropped.push_back(client);
					state.clients.erase(found);
				}
			FailClients(fmt::format("the {} graph failed to build with the clients [{}]", a_name, IdsOf(dropped)), a_error);
			if (dropped.empty())
				return;
			Publish(state);
			for (const auto& client : dropped)
				RetireGates(state, *client);
			Rebuild(state, mask, "clients dropped after a failed build");
		}

		/** @brief Whether a client with passes at one of the anchors in a_mask reads the shared constants. */
		bool UsesSharedConstantsIn(const std::vector<std::shared_ptr<const ClientDesc>>& a_clients, std::uint32_t a_mask)
		{
			return std::ranges::any_of(a_clients, [&](const auto& a_client) { return a_client->sharedConstants && (AnchorMask(*a_client) & a_mask); });
		}

		/** @brief Whether one of a declared segment's steps holds the client. */
		bool SegmentHolds(std::uint32_t a_declared, const ClientDesc& a_client)
		{
			for (std::uint32_t k = 0; k < declaredInfo[a_declared].stepCount; ++k)
				if (StepHolds(declaredInfo[a_declared].steps[k], a_client))
					return true;
			return false;
		}

		/** @brief Whether a declared segment holds a pass, through a step at the pass's anchor. */
		bool SegmentHoldsPass(std::uint32_t a_declared, const ClientDesc& a_client, const PassDesc& a_pass)
		{
			for (std::uint32_t k = 0; k < declaredInfo[a_declared].stepCount; ++k) {
				const auto& step = declaredInfo[a_declared].steps[k];
				if (step.anchor == a_pass.anchor && StepHolds(step, a_client))
					return true;
			}
			return false;
		}

		/** @brief A declared segment's clients in registration order, which is the order of the clientsOn bits. */
		const std::vector<std::shared_ptr<const ClientDesc>>& SegmentClients(const Snapshot& a_snapshot, std::uint32_t a_declared)
		{
			return a_snapshot.segmentClients[a_declared];
		}

		/** @brief SegmentHolds, cached per registration. Render thread. */
		bool ClientHolds(Client& a_client, std::uint32_t a_declared)
		{
			if (a_client.heldOf != a_client.desc.get()) {
				a_client.held = 0;
				for (std::uint32_t d = 0; d < kDeclared; ++d)
					if (SegmentHolds(d, *a_client.desc))
						a_client.held |= std::uint64_t{ 1 } << d;
				a_client.heldOf = a_client.desc.get();
			}
			return (a_client.held >> a_declared & 1u) != 0;
		}

		/** @brief Whether a step's clients read CS's shared constants, so the segment uploads them for that step. */
		bool StepUsesSharedConstants(const Snapshot& a_snapshot, std::uint32_t a_declared, std::uint32_t a_step)
		{
			const auto& step = declaredInfo[a_declared].steps[a_step];
			return std::ranges::any_of(a_snapshot.clients, [&](const auto& a_client) { return a_client->sharedConstants && StepHolds(step, *a_client); });
		}

		/** @brief Whether any step of a declared segment reads CS's shared constants. */
		bool SegmentUsesSharedConstants(const Snapshot& a_snapshot, std::uint32_t a_declared)
		{
			for (std::uint32_t k = 0; k < declaredInfo[a_declared].stepCount; ++k)
				if (StepUsesSharedConstants(a_snapshot, a_declared, k))
					return true;
			return false;
		}

		/** @brief A module epoch's passes a declared segment holds (SetForeignPasses). */
		struct ForeignPasses
		{
			std::function<void(std::uint32_t, void*)> gather;
			std::function<bool()> declared;
		};

		/** @brief Each module epoch's passes, by Segment value. The render thread writes and builds read a copy. */
		struct ForeignTable
		{
			std::mutex mutex;
			std::map<std::uint32_t, std::shared_ptr<const ForeignPasses>> entries;
		};

		ForeignTable& Foreign()
		{
			static ForeignTable table;
			return table;
		}

		std::shared_ptr<const ForeignPasses> ForeignOf(Segment a_segment)
		{
			auto& table = Foreign();
			const std::lock_guard lock(table.mutex);
			const auto found = table.entries.find(static_cast<std::uint32_t>(a_segment));
			return found != table.entries.end() ? found->second : nullptr;
		}

		class AnchorExtension final : public org::RenderGraph::IRenderGraphExtension
		{
		public:
			explicit AnchorExtension(std::shared_ptr<const Snapshot> a_snapshot) :
				snapshot(std::move(a_snapshot)) {}

			void PrepareForBuild(org::RenderGraph& a_graph) override
			{
				if (!snapshot)
					return;
				const std::uint32_t epoch = a_graph.PassEpochFilter();
				const Anchor anchor = AnchorOfEpoch(epoch);
				const std::uint32_t declared = anchor == Anchor::Count ? DeclaredOfEpoch(epoch) : kDeclared;
				if (anchor == Anchor::Count && declared == kDeclared)
					return;
				// Record what this graph was built from when it is installed. That is at attach for a managed build, which may be dropped,
				// or at once for a build the host makes on the owner thread.
				if (declared != kDeclared) {
					// The passes this build makes. Their revisions are part of the segment's description.
					declaredPasses = std::make_shared<std::vector<std::weak_ptr<org::RenderPass>>>();
					// The stage's key as the build starts.
					key = Anchors().nextKeys[declared].load(std::memory_order_acquire);
					const auto commit = [declared, built = snapshot, passes = declaredPasses, built_key = key] {
						auto& anchors = Anchors();
						anchors.declaredPrevious[declared].store(anchors.declaredSnapshots[declared].load(std::memory_order_acquire), std::memory_order_release);
						anchors.declaredSnapshots[declared].store(built, std::memory_order_release);
						anchors.declaredPasses[declared].store(passes, std::memory_order_release);
						anchors.declaredKeys[declared].store(built_key, std::memory_order_release);
						for (std::uint32_t i = 0; i < kAnchors; ++i)
							if (WholeAnchorsOf(declared) >> i & 1u) {
								anchors.built[i].store(built->serial, std::memory_order_release);
								anchors.previousSnapshots[i].store(anchors.builtSnapshots[i].load(std::memory_order_acquire), std::memory_order_release);
								anchors.builtSnapshots[i].store(built, std::memory_order_release);
							} else if ((StepAnchorsOf(declared) >> i & 1u) && (anchors.declaredMembers >> i & 1u)) {
								// A shared anchor is in the graphs once every stage with a step there was built with its clients.
								std::uint64_t serial = UINT64_MAX;
								for (std::uint32_t d = 0; d < kDeclared; ++d)
									if (anchors.declaredEpochs[d] && (StepAnchorsOf(d) >> i & 1u)) {
										const auto snapshot = anchors.declaredSnapshots[d].load(std::memory_order_acquire);
										serial = (std::min)(serial, snapshot ? snapshot->serial : 0);
									}
								anchors.built[i].store(serial == UINT64_MAX ? 0 : serial, std::memory_order_release);
							}
					};
					const auto onFailed = [declared, built = snapshot](const std::string& a_error) {
						DropFailedClients(StepAnchorsOf(declared), declaredInfo[declared].names.label, built, a_error);
					};
					// A staged build: its snapshot and passes describe the frames it is recorded for before its attach.
					(void)RenderGraph::Builds::DeferStaged([declared, built = snapshot, passes = declaredPasses, built_key = key] {
						auto& anchors = Anchors();
						anchors.stagedSnapshots[declared].store(built, std::memory_order_release);
						anchors.stagedPasses[declared].store(passes, std::memory_order_release);
						anchors.stagedKeys[declared].store(built_key, std::memory_order_release);
					});
					if (!RenderGraph::Builds::Defer(commit, onFailed))
						commit();
				} else {
					const auto index = IndexOf(anchor);
					const auto commit = [index, built = snapshot] {
						auto& anchors = Anchors();
						anchors.built[index].store(built->serial, std::memory_order_release);
						anchors.previousSnapshots[index].store(anchors.builtSnapshots[index].load(std::memory_order_acquire), std::memory_order_release);
						anchors.builtSnapshots[index].store(built, std::memory_order_release);
					};
					const auto onFailed = [index, built = snapshot](const std::string& a_error) { DropFailedClients(1u << index, kInfo[index].name, built, a_error); };
					if (!RenderGraph::Builds::Defer(commit, onFailed))
						commit();
				}
				// Each import's slot is the id's resolver, so a declaration by id follows its versions too.
				for (const auto& [id, slot] : snapshot->imports)
					if (const org::ResourceIdentifier identifier(id); !a_graph._resolverMap.contains(identifier))
						a_graph.RegisterResolver(identifier, slot);
				for (const auto& client : snapshot->clients)
					if (client->registerResources)
						client->registerResources(a_graph);
				if (declared != kDeclared ? SegmentUsesSharedConstants(*snapshot, declared) : UsesSharedConstantsIn(snapshot->clients, AnchorsIn(anchor, declared)))
					SharedConstants::Register(a_graph);
			}

			void GatherStructuralPasses(org::RenderGraph& a_graph, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
			{
				if (!snapshot)
					return;
				const std::uint32_t epoch = a_graph.PassEpochFilter();
				const Anchor anchor = AnchorOfEpoch(epoch);
				const std::uint32_t declared = anchor == Anchor::Count ? DeclaredOfEpoch(epoch) : kDeclared;
				const std::uint32_t anchors = AnchorsIn(anchor, declared);
				if (!anchors)
					return;
				if (declared != kDeclared) {
					GatherDeclared(a_graph, declared, epoch, a_out);
					return;
				}
				// Upload the shared constants first. The graph orders their readers after it through the buffers.
				if (UsesSharedConstantsIn(snapshot->clients, anchors))
					if (auto upload = SharedConstants::MakeUploadPass())
						a_out.push_back(std::move(org::RenderGraph::ExternalPassDesc::Copy("cs.shared.upload", std::move(upload))
								.PreferQueue(org::QueueKind::Graphics)
								.Epoch(epoch)));
				std::vector<const PassDesc*> passes;
				for (const auto& scheduled : snapshot->passes)
					if ((anchors >> IndexOf(scheduled.pass->anchor) & 1u) && !scheduled.client->segmentOnly)
						passes.push_back(scheduled.pass);
				for (const auto* scheduled : passes) {
					const auto& pass = *scheduled;
					auto made = pass.make ? pass.make() : pass.makeInstance ? pass.makeInstance(0) : nullptr;
					if (!made)
						continue;
					auto desc = pass.kind == PassKind::Compute ? org::RenderGraph::ExternalPassDesc::Compute(pass.name, std::move(made)) :
					            pass.kind == PassKind::Copy    ? org::RenderGraph::ExternalPassDesc::Copy(pass.name, std::move(made)) :
					                                             org::RenderGraph::ExternalPassDesc::Render(pass.name, std::move(made));
					// Beside the D3D11 work around the anchor: off the graphics queue, a pass would add a wait on DXVK's stream.
					if (pass.queue == PassQueue::Compute)
						desc.PreferQueue(org::QueueKind::Compute);
					else if (pass.kind != PassKind::Render)
						desc.PreferQueue(org::QueueKind::Graphics);
					// atExecution passes record after the run's commit, in DXVK's stream or inline in native mode.
					// A GPU-gated pass records ahead instead, and its closed entries zero its indirect counts.
					if (pass.atExecution && !(pass.gpuGated && GpuGates()) && pass.queue == PassQueue::Default && a_graph.PassEpochRecordsAtExecution())
						desc.RecordAtExecution();
					a_out.push_back(std::move(desc.Epoch(epoch)));
				}
			}

		private:
			/** @brief The anchors whose passes this snapshot puts in the graph. */
			std::uint32_t AnchorsIn(Anchor a_anchor, std::uint32_t a_declared) const
			{
				if (a_anchor != Anchor::Count)
					return 1u << IndexOf(a_anchor);
				if (a_declared != kDeclared)
					return StepAnchorsOf(a_declared);
				return 0;
			}

			/**
			* @brief A declared segment's graph, with its steps' passes in order and a shared-constants upload before steps that read them.
			* atExecution passes record late, inline at the segment's submission, unless their draws are GPU-gated.
			*/
			void GatherDeclared(org::RenderGraph& a_graph, std::uint32_t a_declared, std::uint32_t a_epoch, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) const
			{
				const auto& clients = SegmentClients(*snapshot, a_declared);
				const auto& info = declaredInfo[a_declared];
				const auto note = [&](const std::shared_ptr<org::RenderPass>& a_pass) {
					if (declaredPasses && a_pass)
						declaredPasses->push_back(a_pass);
				};
				// A pass the build's key leaves out is not handed to ORG at all.
				const BuildKey* const buildKey = key.get();
				const auto firstPass = a_out.size();
				// Logs the stage's pass count once per build, to check a frame kind's stage carries only its own passes.
				struct CountLine
				{
					const std::vector<org::RenderGraph::ExternalPassDesc>& out;
					std::size_t first;
					const char* label;
					~CountLine() { logger::info("[ORG] frame segments: a build of the {} holds {} passes", label, out.size() - first); }
				} countLine{ a_out, firstPass, info.names.label };
				for (std::uint32_t k = 0; k < info.stepCount; ++k) {
					const auto& step = info.steps[k];
					if (step.foreign != Segment::Count) {
						const auto foreign = ForeignOf(step.foreign);
						if (!foreign || !foreign->gather || !(KeyForeign(buildKey, ~0u) >> k & 1u))
							continue;
						const auto first = a_out.size();
						foreign->gather(a_epoch, &a_out);
						for (auto i = first; i < a_out.size(); ++i) {
							auto& desc = a_out[i];
							desc.epoch = a_epoch;
							if (!desc.pass)
								continue;
							desc.pass->SetSkipWhen([a_declared, k](const org::IHostExecutionData* a_data) {
								const auto* data = a_data ? a_data->Get<SegmentHostData>() : nullptr;
								return data && data->segment == a_declared && !(data->fields.foreignOn >> k & 1u);
							});
							if (!desc.recordAtExecution)
								note(desc.pass);
						}
						continue;
					}
					const bool uploads = std::ranges::any_of(snapshot->clients, [&](const auto& a_client) {
						return a_client->sharedConstants && StepHolds(step, *a_client) && KeyHas(buildKey, *a_client, -1);
					});
					if (uploads)
						if (auto upload = SharedConstants::MakeUploadPass(k)) {
							note(upload);
							a_out.push_back(std::move(org::RenderGraph::ExternalPassDesc::Copy(fmt::format("cs.shared.upload.{}", k), std::move(upload))
									.PreferQueue(org::QueueKind::Graphics)
									.Epoch(a_epoch)));
						}
					const auto emit = [&](const ScheduledPass& scheduled, std::uint32_t instance, bool instanced) {
						const auto& pass = *scheduled.pass;
						if (!KeyHas(buildKey, *scheduled.client, pass.row))
							return;
						const auto found = std::ranges::find(clients, scheduled.client);
						const auto bit = static_cast<std::uint32_t>(found - clients.begin());
						auto made = instanced ? pass.makeInstance(instance) : pass.make ? pass.make() : pass.makeInstance ? pass.makeInstance(0) : nullptr;
						if (!made)
							return;
						made->SetSkipWhen([a_declared, bit, instanced, instance, row = pass.row](const org::IHostExecutionData* a_data) {
							const auto* data = a_data ? a_data->Get<SegmentHostData>() : nullptr;
							if (!data || data->segment != a_declared)
								return false;
							// A row the description did not select records only its barriers.
							if (row >= 0 && row < 256 && !(data->fields.rows[static_cast<std::size_t>(row) / 64] >> (static_cast<std::uint32_t>(row) % 64) & 1u))
								return true;
							// A client past the described ones is always on.
							if (bit >= RenderGraph::Frame::kSegmentClients)
								return false;
							if (!(data->fields.clientsOn >> bit & 1u))
								return true;
							return instanced && instance >= data->fields.instances[bit];
						});
						// A gated pass without indirect draws still reads its run's gate answers when it records, so it records at execution.
						const bool live = pass.atExecution && !(pass.gpuGated && GpuGates()) && pass.queue == PassQueue::Default && a_graph.PassEpochRecordsAtExecution();
						if (!live)
							note(made);
						std::string name = instanced ? fmt::format("{}.i{}", pass.name, instance) : pass.name;
						auto desc = pass.kind == PassKind::Compute ? org::RenderGraph::ExternalPassDesc::Compute(std::move(name), std::move(made)) :
						            pass.kind == PassKind::Copy    ? org::RenderGraph::ExternalPassDesc::Copy(std::move(name), std::move(made)) :
						                                             org::RenderGraph::ExternalPassDesc::Render(std::move(name), std::move(made));
						if (pass.queue == PassQueue::Compute)
							desc.PreferQueue(org::QueueKind::Compute);
						else if (pass.kind != PassKind::Render)
							desc.PreferQueue(org::QueueKind::Graphics);
						if (live)
							desc.RecordAtExecution();
						a_out.push_back(std::move(desc.Epoch(a_epoch)));
					};
					std::vector<const ClientDesc*> major;  // instanceMajor clients already made at this step
					for (const auto& scheduled : snapshot->passes) {
						if (scheduled.pass->anchor != step.anchor || !StepHolds(step, *scheduled.client))
							continue;
						// A client with instances gets one pass per instance. Instance k is off when the description declares k or fewer.
						const bool instanced = scheduled.client->maxInstances > 1 && scheduled.pass->makeInstance;
						const auto [first, count] = instanced ? StepInstances(step, *scheduled.client) : std::pair<std::uint32_t, std::uint32_t>{ 0, 1 };
						if (instanced && scheduled.client->instanceMajor) {
							// Instance-major order, every pass of the first instance and then the next. This matches the taps' order.
							if (std::ranges::find(major, scheduled.client.get()) != major.end())
								continue;
							major.push_back(scheduled.client.get());
							for (std::uint32_t instance = first; instance < first + count; ++instance)
								for (const auto& other : snapshot->passes)
									if (other.client == scheduled.client && other.pass->anchor == step.anchor && other.pass->makeInstance)
										emit(other, instance, true);
							continue;
						}
						for (std::uint32_t instance = first; instance < first + count; ++instance)
							emit(scheduled, instance, instanced);
					}
				}
			}

			// The passes and key a declared segment's build was made with.
			std::shared_ptr<std::vector<std::weak_ptr<org::RenderPass>>> declaredPasses;
			std::shared_ptr<const BuildKey> key;
			std::shared_ptr<const Snapshot> snapshot;
		};

		/** @brief The import's description, with its views as their owner made them. Any view may be null. */
		org::TextureDescription DescribeColour(const D3D11_TEXTURE2D_DESC& a_desc, ID3D11ShaderResourceView* a_srv, ID3D11RenderTargetView* a_rtv,
			ID3D11UnorderedAccessView* a_uav)
		{
			org::TextureDescription desc{};
			desc.format = rhi::helpers::ToRHI(a_desc.Format);
			desc.channels = 4;
			if (a_srv) {
				D3D11_SHADER_RESOURCE_VIEW_DESC view{};
				a_srv->GetDesc(&view);
				desc.hasSRV = true;
				desc.srvFormat = rhi::helpers::ToRHI(view.Format);
				desc.format = desc.srvFormat;
			}
			if (a_rtv) {
				D3D11_RENDER_TARGET_VIEW_DESC view{};
				a_rtv->GetDesc(&view);
				desc.hasRTV = true;
				desc.rtvFormat = rhi::helpers::ToRHI(view.Format);
			}
			if (a_uav) {
				D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
				a_uav->GetDesc(&view);
				desc.hasUAV = true;
				desc.uavFormat = rhi::helpers::ToRHI(view.Format);
				// A CPU-side UAV too, so AnchorPasses::Clear can clear it.
				desc.hasNonShaderVisibleUAV = true;
			}
			return desc;
		}

		org::TextureDescription DescribeDepth(const RE::BSGraphics::DepthStencilData& a_target, const D3D11_TEXTURE2D_DESC& a_desc)
		{
			org::TextureDescription desc{};
			desc.format = rhi::helpers::ToRHI(a_desc.Format);
			desc.channels = 1;
			if (a_target.views[0]) {
				D3D11_DEPTH_STENCIL_VIEW_DESC view{};
				a_target.views[0]->GetDesc(&view);
				desc.hasDSV = true;
				desc.dsvFormat = rhi::helpers::ToRHI(view.Format);
				desc.format = desc.dsvFormat;
			}
			if (a_target.depthSRV) {
				D3D11_SHADER_RESOURCE_VIEW_DESC view{};
				a_target.depthSRV->GetDesc(&view);
				desc.hasSRV = true;
				desc.srvFormat = rhi::helpers::ToRHI(view.Format);
			}
			return desc;
		}

		/** @brief Width, height and format of a version's backing, whether imported or made by ORG. */
		std::uint32_t WidthOf(const org::Resource* a_resource)
		{
			if (const auto* external = dynamic_cast<const org::ExternalTextureResource*>(a_resource))
				return external->GetWidth();
			if (const auto* texture = dynamic_cast<const org::PixelBuffer*>(a_resource))
				return texture->GetWidth();
			return 0;
		}

		std::uint32_t HeightOf(const org::Resource* a_resource)
		{
			if (const auto* external = dynamic_cast<const org::ExternalTextureResource*>(a_resource))
				return external->GetHeight();
			if (const auto* texture = dynamic_cast<const org::PixelBuffer*>(a_resource))
				return texture->GetHeight();
			return 0;
		}

		rhi::Format FormatOf(const org::Resource* a_resource)
		{
			if (const auto* external = dynamic_cast<const org::ExternalTextureResource*>(a_resource))
				return external->GetFormat();
			if (const auto* texture = dynamic_cast<const org::PixelBuffer*>(a_resource))
				return texture->GetFormat();
			return rhi::Format::Unknown;
		}

		/** @brief A view's format as the backing makes it. An unnamed one is the description's own format. */
		rhi::Format ViewFormat(rhi::Format a_view, const org::TextureDescription& a_desc)
		{
			return a_view != rhi::Format::Unknown ? a_view : a_desc.format;
		}

		/** @brief Whether a backing view serves a requested one. An unnamed format asks for any. */
		bool HasView(bool a_have, rhi::Format a_haveFormat, bool a_want, rhi::Format a_wantFormat)
		{
			return !a_want || (a_have && (a_wantFormat == rhi::Format::Unknown || a_haveFormat == a_wantFormat));
		}

		/** @brief Whether a backing described by a_have has every view a_want asks for, in its format. */
		bool Covers(const org::TextureDescription& a_have, const org::TextureDescription& a_want)
		{
			return HasView(a_have.hasSRV, ViewFormat(a_have.srvFormat, a_have), a_want.hasSRV, a_want.srvFormat) &&
			       HasView(a_have.hasRTV, ViewFormat(a_have.rtvFormat, a_have), a_want.hasRTV, a_want.rtvFormat) &&
			       HasView(a_have.hasDSV, ViewFormat(a_have.dsvFormat, a_have), a_want.hasDSV, a_want.dsvFormat) &&
			       HasView(a_have.hasUAV, ViewFormat(a_have.uavFormat, a_have), a_want.hasUAV, a_want.uavFormat) &&
			       (!a_want.hasNonShaderVisibleUAV || a_have.hasNonShaderVisibleUAV) && (!a_want.isCubemap || a_have.isCubemap);
		}

		/** @brief a_have plus the views of a_want it lacks. Its format stays a_have's, so the shape class is the same.
		*  A view both have in different formats keeps a_have's, and a_conflict names it. */
		org::TextureDescription MergeViews(const org::TextureDescription& a_have, const org::TextureDescription& a_want, std::string& a_conflict)
		{
			auto merged = a_have;
			const auto add = [&](bool& a_mergedHas, rhi::Format& a_mergedFormat, bool a_wantHas, rhi::Format a_wantFormat, const char* a_name) {
				if (!a_wantHas)
					return;
				if (!a_mergedHas) {
					a_mergedHas = true;
					a_mergedFormat = a_wantFormat;
				} else if (a_wantFormat != rhi::Format::Unknown && ViewFormat(a_mergedFormat, merged) != a_wantFormat) {
					a_conflict = fmt::format("{}{}{} (format {} asked, {} kept)", a_conflict, a_conflict.empty() ? "" : ", ", a_name, static_cast<int>(a_wantFormat),
						static_cast<int>(ViewFormat(a_mergedFormat, merged)));
				}
			};
			add(merged.hasSRV, merged.srvFormat, a_want.hasSRV, a_want.srvFormat, "SRV");
			add(merged.hasRTV, merged.rtvFormat, a_want.hasRTV, a_want.rtvFormat, "RTV");
			add(merged.hasDSV, merged.dsvFormat, a_want.hasDSV, a_want.dsvFormat, "DSV");
			add(merged.hasUAV, merged.uavFormat, a_want.hasUAV, a_want.uavFormat, "UAV");
			merged.hasNonShaderVisibleUAV = merged.hasNonShaderVisibleUAV || a_want.hasNonShaderVisibleUAV;
			merged.isCubemap = merged.isCubemap || a_want.isCubemap;
			if (merged.channels == 0)
				merged.channels = a_want.channels;
			return merged;
		}

		/** @brief a_desc's subresources from the image, the same for every importer. Slices form cubes when a_desc asks for a cube. */
		void DescribeSubresources(org::TextureDescription& a_desc, const DxvkOrgInteropImageInfo& a_image)
		{
			a_desc.imageDimensions.clear();
			const std::uint32_t mips = (std::max)(a_image.mipLevels, 1u);
			for (std::uint32_t mip = 0; mip < mips; ++mip)
				a_desc.imageDimensions.push_back({ (std::max)(a_image.extent.width >> mip, 1u), (std::max)(a_image.extent.height >> mip, 1u), 0, 0 });
			const std::uint32_t layers = (std::max)(a_image.arrayLayers, 1u);
			a_desc.isCubemap = a_desc.isCubemap && layers % 6 == 0;
			const std::uint32_t slices = a_desc.isCubemap ? layers / 6 : layers;
			a_desc.isArray = slices > 1;
			a_desc.arraySize = slices;
		}

		/** @brief Limits each kind of import line to the first 16, since they repeat when a texture is made again. */
		bool LogImportLine(std::atomic<std::uint32_t>& a_count)
		{
			return a_count.fetch_add(1, std::memory_order_relaxed) < 16;
		}

		/**
		* @brief The backing of a new slot for a_image. This is the one place an image becomes a graph resource. Null on failure.
		* Native mode uses the image's graph-owned texture, which already has every view its bind flags allow.
		*/
		std::shared_ptr<org::GloballyIndexedResource> MakeBacking(ID3D11Texture2D* a_texture, const DxvkOrgInteropImageInfo& a_image,
			org::TextureDescription& a_desc, const std::string& a_id)
		{
			auto& runtime = RenderGraphRuntime::Get();
			const auto image = reinterpret_cast<std::uint64_t>(a_image.image);
			if (RenderGraph::Engine::Native::Active()) {
				auto owned = a_texture ? RenderGraph::Engine::Native::GraphTextureOf(a_texture) : RenderGraph::Engine::Native::GraphTextureOfImage(image);
				if (const auto* texture = dynamic_cast<const org::PixelBuffer*>(owned.get())) {
					const auto& made = texture->GetDescription();
					if (!Covers(made, a_desc)) {
						std::string conflict;
						const auto merged = MergeViews(made, a_desc, conflict);
						static std::atomic<std::uint32_t> logged{ 0 };
						if (LogImportLine(logged))
							logger::warn("[ORG] frame anchors: {}: the graph-owned texture {}{}{}; it stays the image's one graph resource", a_id,
								Covers(made, merged) ? "has the views asked for in other formats" : "lacks a view asked for, which its bind flags do not allow",
								conflict.empty() ? "" : ": ", conflict);
					}
					a_desc = made;
					DescribeSubresources(a_desc, a_image);
					return owned;
				}
			}
			DescribeSubresources(a_desc, a_image);
			auto resource = RenderGraph::ImportDxvkTexture(runtime.Host()->Device(), a_image, a_desc, a_id.c_str());
			if (!resource)
				logger::warn("[ORG] frame anchors: {} could not be imported", a_id);
			return resource;
		}

		/**
		* @brief The graph resource ORG already made for this image, such as a twin's export or an engine texture.
		* Null when there is none or it lacks a requested view. The caller then imports the image.
		*/
		Texture MadeTexture(ID3D11Texture2D* a_texture, std::uint64_t a_image, const org::TextureDescription& a_desc, std::string_view a_by)
		{
			// Native mode makes an exempt texture's graph resource on its first lookup. Look it up first so the image keeps one resource.
			if (a_texture && RenderGraph::Engine::Native::Active() && !a_desc.hasRTV && !a_desc.hasUAV && !a_desc.hasDSV)
				(void)RenderGraph::Engine::Native::ImmutableTextureOf(a_texture, DXGI_FORMAT_UNKNOWN);
			auto made = RenderGraph::MadeImageOf(a_image);
			if (!made.resource)
				return {};
			if (made.views && !Covers(*made.views, a_desc)) {
				static std::atomic<std::uint32_t> loggedLacking{ 0 };
				if (LogImportLine(loggedLacking))
					logger::warn("[ORG] frame anchors: {} asks for a view '{}' lacks; its image is imported again, a second graph resource of it", a_by, made.name);
				return {};
			}
			static std::atomic<std::uint32_t> loggedFound{ 0 };
			if (LogImportLine(loggedFound))
				logger::info("[ORG] frame anchors: {} is the image of '{}', which ORG made; it is that graph resource{}", a_by, made.name,
					made.slot ? "'s slot" : "");
			if (made.slot)
				return Texture(made.name, std::move(made.slot));
			return Texture(std::move(made.resource));
		}

		/** @brief How a new backing joined an import's slot. */
		enum class Joined : std::uint8_t
		{
			Version, // the slot's next version, same size. Nothing is rebuilt
			Resized, // the slot's next version, new size. Passes that read the size at build are made again
			NewShape, // new format, mip or slice count. A new slot, and graphs using the old one rebuild
		};

		/** @brief Publishes a_next as a_slot's next version if the shape class matches. a_owner keeps it alive. Render thread. */
		Joined JoinSlot(org::VersionedTexture& a_slot, std::shared_ptr<org::GloballyIndexedResource> a_next, std::shared_ptr<const void> a_owner = {})
		{
			const auto previous = a_slot.CurrentResource();
			const bool sameSize = previous && WidthOf(previous.get()) == WidthOf(a_next.get()) && HeightOf(previous.get()) == HeightOf(a_next.get());
			try {
				// Imports take versions when the engine makes them, an allowed exception until step 9b.
				const RenderGraph::Frame::Boundary::Caller caller("engine import");
				const RenderGraph::Frame::Boundary::Allow allow(RenderGraph::Frame::Boundary::Exception::EngineImports);
				a_slot.PublishImage(std::move(a_next), std::move(a_owner));
			} catch (const std::exception&) {
				return Joined::NewShape;
			}
			if (auto* host = RenderGraphRuntime::Get().Host())
				host->NoteNewVersions();
			return sameSize ? Joined::Version : Joined::Resized;
		}

		/** @brief The entry whose current version is a_image, an id import or a held loose slot. Null if none. */
		std::shared_ptr<EngineImport> FindImage(State& a_state, std::uint64_t a_image)
		{
			if (!a_image)
				return nullptr;
			for (const auto& [id, import] : a_state.imports)
				if (import->image == a_image)
					return import;
			std::erase_if(a_state.loose, [](const std::weak_ptr<EngineImport>& a_entry) {
				const auto entry = a_entry.lock();
				return !entry || entry->looseSlot.expired();
			});
			for (const auto& weak : a_state.loose)
				if (auto entry = weak.lock(); entry && entry->image == a_image)
					return entry;
			return nullptr;
		}

		/** @brief Whether an id other than a_except names a_import. */
		bool NamedElsewhere(const State& a_state, const std::shared_ptr<EngineImport>& a_import, std::string_view a_except)
		{
			return std::ranges::any_of(a_state.imports, [&](const auto& a_entry) { return a_entry.first != a_except && a_entry.second == a_import; });
		}

		/** @brief Makes a_import's slot serve a_want's views too, by importing the image again when a view is missing.
		*  Passes keep declaring the slot, so nothing is rebuilt. Render thread. */
		void EnsureViews(const std::shared_ptr<EngineImport>& a_import, const org::TextureDescription& a_want, std::string_view a_by)
		{
			if (Covers(a_import->views, a_want))
				return;
			auto slot = a_import->Slot();
			if (!slot)
				return;
			std::string conflict;
			auto merged = MergeViews(a_import->views, a_want, conflict);
			if (!conflict.empty()) {
				static std::atomic<std::uint32_t> logged{ 0 };
				if (LogImportLine(logged))
					logger::warn("[ORG] frame anchors: {} asks for a view of {}'s image in another format: {}; it gets the slot's", a_by, a_import->id, conflict);
			}
			if (Covers(a_import->views, merged))
				return;
			const auto old = slot->CurrentResource();
			// Native mode's graph-owned texture already has every view its bind flags allow. A new import would only duplicate it.
			if (dynamic_cast<const org::PixelBuffer*>(old.get())) {
				static std::atomic<std::uint32_t> logged{ 0 };
				if (LogImportLine(logged))
					logger::warn("[ORG] frame anchors: {} asks {}'s graph-owned texture for a view its bind flags do not allow; it keeps its views", a_by, a_import->id);
				return;
			}
			DescribeSubresources(merged, a_import->info);
			auto& runtime = RenderGraphRuntime::Get();
			// Frames already prepared with the old version finish with it.
			RenderGraph::RetireImageResource(old.get());
			auto next = RenderGraph::ImportDxvkTexture(runtime.Host()->Device(), a_import->info, merged, a_import->id.c_str());
			// A loose slot's versions keep its entry, and with it the D3D11 texture.
			std::shared_ptr<const void> owner;
			if (!a_import->slot)
				owner = a_import;
			if (!next || JoinSlot(*slot, next, owner) == Joined::NewShape) {
				logger::warn("[ORG] frame anchors: {}'s image could not be imported again with the views {} asks for; it keeps the slot's views", a_import->id, a_by);
				if (old)
					RenderGraph::NoteImageResource(a_import->image, old, a_import->id);
				return;
			}
			a_import->views = std::move(merged);
			static std::atomic<std::uint32_t> logged{ 0 };
			if (LogImportLine(logged))
				logger::info("[ORG] frame anchors: {}'s slot has the views {} asks for now: a version of the same image", a_import->id, a_by);
		}

		/** @brief Drops a_import once nothing names it. Its D3D11 texture goes once D3D11 is done with it. */
		void ReleaseUnnamed(State& a_state, std::shared_ptr<EngineImport> a_import)
		{
			if (a_import && !NamedElsewhere(a_state, a_import, {}))
				RenderGraphRuntime::Get().ReleaseAfterD3D11(std::move(a_import));
		}

		Texture Import(ID3D11Texture2D* a_texture, const std::string& a_id,
			const std::function<org::TextureDescription(const D3D11_TEXTURE2D_DESC&)>& a_describe)
		{
			auto lock = Guard();
			auto& state = Anchors();
			auto& runtime = RenderGraphRuntime::Get();
			if (!a_texture || !runtime.IsActive())
				return {};
			// An engine target ORG owns returns its published slot.
			// A second import would make a resource whose uses the graph does not order with the owner's.
			if (auto owned = RenderGraph::Engine::Targets::Find(a_texture))
				return owned;
			// Import DXVK's own texture, never a tripwire handle. Native mode has no DXVK texture under its handles.
			if (!RenderGraph::Engine::Native::Active())
				a_texture = RenderGraph::Engine::Tripwire::Unwrap(a_texture);
			const auto found = state.imports.find(a_id);
			const std::shared_ptr<EngineImport> previous = found != state.imports.end() ? found->second : nullptr;
			if (previous && previous->texture.get() == a_texture)
				return Texture(a_id, previous->slot);

			DxvkOrgInteropResourceInfo info{};
			if (!runtime.DescribeResource(a_texture, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE) {
				logger::warn("[ORG] frame anchors: {} cannot be described", a_id);
				return {};
			}
			D3D11_TEXTURE2D_DESC textureDesc{};
			a_texture->GetDesc(&textureDesc);
			auto desc = a_describe(textureDesc);
			// A cube's SRV is a cube view, and arraySize counts cubes.
			desc.isCubemap = (textureDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
			DescribeSubresources(desc, info.image);
			const auto image = reinterpret_cast<std::uint64_t>(info.image.image);

			// An image ORG already made a graph resource for. The id keeps its slot, and this texture is not a version of it.
			if (auto made = MadeTexture(a_texture, image, desc, a_id))
				return made;

			// One image, one graph resource, whoever imported it first.
			// Two resources would each track the image's state and race on layout moves.
			if (auto shared = FindImage(state, image)) {
				EnsureViews(shared, desc, a_id);
				if (shared == previous) {
					// The same image under another handle (a port's handle around it).
					shared->texture.copy_from(a_texture);
					return Texture(a_id, shared->slot);
				}
				if (!shared->slot) {
					// A loose slot from ShareImage becomes this id's. The loose entry stays its versions' owner, since they hold it.
					auto taken = std::make_shared<EngineImport>(*shared);
					taken->slot = shared->looseSlot.lock();
					taken->looseSlot.reset();
					std::erase_if(state.loose, [&](const std::weak_ptr<EngineImport>& a_entry) { return a_entry.lock() == shared; });
					shared = std::move(taken);
				}
				if (!shared->texture)
					shared->texture.copy_from(a_texture);
				logger::info("[ORG] frame anchors: {} is the image {} has already; it shares that slot", a_id, shared->id);
				state.imports[a_id] = shared;
				Publish(state);
				if (previous) {
					ReleaseUnnamed(state, previous);
					StructuralRebuild(state, ~0u, fmt::format("import '{}' now shares the slot of '{}'", a_id, shared->id));
				} else if (state.host) {
					RenderGraph::Builds::RequestFor(a_id, fmt::format("frame anchors: import '{}' added", a_id));
				}
				return Texture(a_id, shared->slot);
			}

			auto backing = MakeBacking(a_texture, info.image, desc, a_id);
			if (!backing)
				return {};
			// A new texture for an id that alone names its slot becomes the slot's next version. A shared slot stays with the other id.
			if (previous && !NamedElsewhere(state, previous, a_id)) {
				const auto joined = JoinSlot(*previous->slot, backing);
				if (joined != Joined::NewShape) {
					// The old D3D11 texture goes once D3D11 is done with it. The old version lives while a frame or ticket holds it.
					runtime.ReleaseAfterD3D11(std::make_shared<winrt::com_ptr<ID3D11Texture2D>>(std::move(previous->texture)));
					previous->texture.copy_from(a_texture);
					previous->image = image;
					previous->info = info.image;
					previous->views = desc;
					if (joined == Joined::Resized)
						Rebuild(state, ~0u, fmt::format("import '{}' resized: passes that took its size are made again", a_id));
					return Texture(a_id, previous->slot);
				}
			}
			auto import = std::make_shared<EngineImport>();
			import->texture.copy_from(a_texture);
			import->slot = org::VersionedTexture::CreateImage(std::move(backing));
			import->id = a_id;
			import->image = image;
			import->info = info.image;
			import->views = desc;
			auto slot = import->slot;
			state.imports[a_id] = std::move(import);
			ReleaseUnnamed(state, previous);
			Publish(state);
			// A new shape class is a new slot, so every graph holding the old one is rebuilt off the render thread.
			if (previous)
				StructuralRebuild(state, ~0u, fmt::format("import '{}' replaced by another shape class", a_id));
			else if (state.host)
				RenderGraph::Builds::RequestFor(a_id, fmt::format("frame anchors: import '{}' added", a_id));
			return Texture(a_id, std::move(slot));
		}
	}

	Texture::Texture(std::string a_id, std::shared_ptr<org::VersionedTexture> a_slot) :
		id(std::move(a_id)), slot(std::move(a_slot))
	{}

	std::shared_ptr<org::GloballyIndexedResource> Texture::Current() const
	{
		return std::dynamic_pointer_cast<org::GloballyIndexedResource>(slot ? slot->CurrentResource() : plain);
	}

	org::GloballyIndexedResource* Texture::operator->() const
	{
		// The slot keeps its current version alive until the render thread publishes the next one.
		return Current().get();
	}

	std::uint32_t Texture::Width() const { return WidthOf(Current().get()); }
	std::uint32_t Texture::Height() const { return HeightOf(Current().get()); }
	rhi::Format Texture::Format() const { return FormatOf(Current().get()); }

	const org::IResourceResolver* Texture::DeclaredResolver() const
	{
		return slot.get();
	}

	const char* Name(Anchor a_anchor)
	{
		return IndexOf(a_anchor) < kAnchors ? kInfo[IndexOf(a_anchor)].name : "unknown";
	}

	std::uint32_t SegmentOf(Anchor a_anchor)
	{
		return static_cast<std::uint32_t>(Segment::AnchorReflectionsPrepass) + IndexOf(a_anchor);
	}

	const SegmentNames* NamesOf(std::uint32_t a_segment)
	{
		for (const auto& declared : declaredInfo)
			if (a_segment == static_cast<std::uint32_t>(declared.segment))
				return &declared.names;
		const std::uint32_t first = static_cast<std::uint32_t>(Segment::AnchorReflectionsPrepass);
		if (a_segment < first || a_segment - first >= kAnchors)
			return nullptr;
		return &kInfo[a_segment - first].names;
	}

	bool IsUnusedSegment(std::uint32_t a_segment)
	{
		for (std::uint32_t d = 0; d < kDeclared; ++d)
			if (a_segment == static_cast<std::uint32_t>(declaredInfo[d].segment)) {
				const auto& state = Anchors();
				return !state.frozen || !state.declaredEpochs[d];
			}
		const std::uint32_t first = static_cast<std::uint32_t>(Segment::AnchorReflectionsPrepass);
		if (a_segment < first || a_segment - first >= kAnchors)
			return false;
		const auto& state = Anchors();
		// A declared segment's member has no epoch of its own.
		return !state.frozen || !state.reserved[a_segment - first] || (state.declaredMembers >> (a_segment - first) & 1u);
	}

	bool Reserve(Anchor a_anchor)
	{
		auto& state = Anchors();
		if (IndexOf(a_anchor) >= kAnchors)
			return false;
		if (state.reserved[IndexOf(a_anchor)])
			return true;
		if (state.frozen) {
			logger::warn("[ORG] frame anchor {}: reserved after the render graph initialized; it has no epoch this session", Name(a_anchor));
			return false;
		}
		state.reserved[IndexOf(a_anchor)] = true;
		return true;
	}

	bool HasEpoch(Anchor a_anchor)
	{
		const auto& state = Anchors();
		return IndexOf(a_anchor) < kAnchors && state.frozen && state.reserved[IndexOf(a_anchor)] && state.host && RenderGraphRuntime::Get().IsActive();
	}

	bool RecordsAtExecution(Anchor a_anchor)
	{
		// A declared segment records every pass ahead, on the host's thread.
		const auto& state = Anchors();
		const std::uint32_t index = IndexOf(a_anchor);
		if (index < kAnchors && DeclaredOfAnchor(state, index) < kDeclared)
			return false;
		if (!HasEpoch(a_anchor))
			return false;
		// A shared anchor's passes record in the stages' epochs, since it has no epoch of its own.
		if (state.declaredMembers >> index & 1u) {
			for (std::uint32_t d = 0; d < kDeclared; ++d)
				if (state.declaredEpochs[d] && (StepAnchorsOf(d) >> index & 1u))
					return RenderGraphRuntime::Get().RecordsAtExecution(EpochOfDeclared(d));
			return false;
		}
		return RenderGraphRuntime::Get().RecordsAtExecution(EpochOfAnchor(a_anchor));
	}

	namespace
	{
		/** @brief Whether the recipe table names client a_id at a_anchor. Diagnostic clients are named everywhere. */
		bool TableNames(const State& a_state, std::string_view a_id, Anchor a_anchor)
		{
			if (Table::Has(Table::kDiagnosticClients, a_id))
				return true;
			if (a_state.declaredMembers >> IndexOf(a_anchor) & 1u) {
				for (std::uint32_t d = 0; d < kDeclared; ++d)
					if (a_state.declaredEpochs[d])
						for (std::uint32_t k = 0; k < declaredInfo[d].stepCount; ++k)
							if (declaredInfo[d].steps[k].anchor == a_anchor && Table::Has(declaredInfo[d].steps[k].clients, a_id))
								return true;
				return false;
			}
			for (const auto& stage : Table::kStageTable) {
				for (const auto& step : stage.steps)
					if (step.anchor == a_anchor && Table::Has(step.clients, a_id))
						return true;
				for (const auto& epoch : stage.epochs)
					if (epoch.segment == Table::AnchorSegment(a_anchor) && epoch.role != Table::Role::Graph &&
						Table::Has(epoch.clients, a_id))
						return true;
			}
			return false;
		}
	}

	bool Register(ClientDesc a_client)
	{
		auto lock = Guard();
		auto& state = Anchors();
		if (!state.host || !RenderGraphRuntime::Get().IsActive()) {
			logger::warn("[ORG] frame anchors: {} not registered, the render graph is not active", a_client.id);
			return false;
		}
		// Every pass must be at an anchor where the recipe table names the client.
		// A mismatch is fatal in development builds, and refused with one log line in release.
		for (const auto& pass : a_client.passes) {
			if (IndexOf(pass.anchor) >= kAnchors || TableNames(state, a_client.id, pass.anchor))
				continue;
			const auto message = fmt::format(
				"Community Shaders: the client '{}' registered a pass ('{}') at the {} anchor, and no step of the recipe table names it there "
				"(src/RenderGraph/Frame/RecipeTable.h). Name the client at the step of the stage its work belongs to.",
				a_client.id, pass.name, Name(pass.anchor));
			if (RenderGraph::Frame::Driver::MismatchFatal()) {
				logger::critical("[ORG] recipe mismatch: {}", message);
				spdlog::default_logger()->flush();
				stl::report_and_fail(message);
			}
			static std::set<std::string, std::less<>> logged;
			if (logged.insert(a_client.id).second)
				logger::error("[ORG] recipe mismatch: {} The client is refused.", message);
			return false;
		}
		if (a_client.id.empty() || a_client.passes.empty()) {
			logger::warn("[ORG] frame anchors: a client without an id or passes ({}) is not registered", a_client.id);
			return false;
		}
		std::vector<std::string_view> names;
		for (const auto& pass : a_client.passes) {
			if (!HasEpoch(pass.anchor) || pass.name.empty() || (!pass.make && !pass.makeInstance) || std::ranges::find(names, pass.name) != names.end()) {
				logger::warn("[ORG] frame anchors: {} not registered: its pass '{}' at {} {}", a_client.id, pass.name, Name(pass.anchor),
					!HasEpoch(pass.anchor) ? "names an anchor without an epoch (reserve it before the graph initializes)" : "has no name, no make, or a repeated name");
				return false;
			}
			names.push_back(pass.name);
		}
		// Until CS's first upload the shared constants do not exist, so the client waits for Service.
		if (a_client.sharedConstants && !SharedConstants::Ensure() && SharedConstants::Failed()) {
			logger::warn("[ORG] frame anchors: {} not registered: it reads the shared constants, which could not be made", a_client.id);
			return false;
		}
		Client client;
		client.desc = std::make_shared<const ClientDesc>(std::move(a_client));
		for (const auto& pass : client.desc->passes)
			client.armed.push_back(pass.once ? 1 : 0);
		std::uint32_t mask = AnchorMask(*client.desc);
		auto* existing = FindClient(state, client.desc->id);
		std::shared_ptr<const ClientDesc> replaced;
		if (existing) {
			mask |= AnchorMask(*existing->desc);
			replaced = existing->desc;
			*existing = std::move(client);
		} else {
			existing = &state.clients.emplace_back(std::move(client));
		}
		Publish(state);
		MarkCurrent(state, *existing);
		if (replaced)
			RetireGates(state, *replaced, existing->desc->gates);
		{
			const RenderGraph::Frame::Boundary::Change change(existing->desc->id, replaced ? "register again" : "register");
			Rebuild(state, mask, fmt::format("client '{}' registered{}", existing->desc->id, replaced ? " again" : ""));
		}
		logger::info("[ORG] frame anchors: {} registered ({} passes){}", existing->desc->id, existing->desc->passes.size(),
			InGraphs(*existing->desc) ? "" : "; it joins the graphs once the shared constants exist");
		// A client registered again every frame never gets a build that switches in, so its stages stay without it.
		if (replaced) {
			struct Streak
			{
				std::uint64_t frame = 0, length = 0, warnings = 0;
			};
			static std::map<std::string, Streak, std::less<>> streaks;
			constexpr std::uint64_t kStreakFrames = 8;
			const auto frame = RenderGraph::Frame::Driver::Current().frame;
			auto& streak = streaks[existing->desc->id];
			streak.length = streak.frame + 1 == frame ? streak.length + 1 : streak.frame == frame ? streak.length : 1;
			streak.frame = frame;
			if (streak.length % kStreakFrames == 0 && std::has_single_bit(streak.length / kStreakFrames))
				logger::warn("[ORG] frame anchors: {} registered again on {} consecutive frames (frame {}, warning {}); its stages cannot switch in a "
							 "build with it", existing->desc->id, streak.length, frame, ++streak.warnings);
		}
		return true;
	}

	void Unregister(std::string_view a_id)
	{
		auto lock = Guard();
		auto& state = Anchors();
		const auto existing = std::ranges::find_if(state.clients, [&](const Client& a_other) { return a_other.desc->id == a_id; });
		if (existing == state.clients.end())
			return;
		const std::uint32_t mask = AnchorMask(*existing->desc);
		const auto desc = existing->desc;
		state.clients.erase(existing);
		Publish(state);
		RetireGates(state, *desc);
		const RenderGraph::Frame::Boundary::Change change(desc->id, "unregister");
		Rebuild(state, mask, fmt::format("client '{}' unregistered", desc->id));
	}

	void Refresh(std::string_view a_id)
	{
		auto lock = Guard();
		auto& state = Anchors();
		if (auto* client = FindClient(state, a_id)) {
			Publish(state);
			MarkCurrent(state, *client);
			const RenderGraph::Frame::Boundary::Change change(client->desc->id, "refresh");
			Rebuild(state, AnchorMask(*client->desc), fmt::format("client '{}' refreshed", client->desc->id));
		}
	}

	bool InBuiltGraph(std::string_view a_id, Anchor a_anchor)
	{
		auto lock = Guard();
		auto& state = Anchors();
		const std::uint32_t index = IndexOf(a_anchor);
		const auto* client = FindClient(state, a_id);
		return client && index < kAnchors && client->serial <= BuiltSerial(state, index);
	}

	void SetLateEpochHook(std::function<void()> a_hook)
	{
		Anchors().lateHook = std::move(a_hook);
	}

	namespace
	{
		/** @brief Before a run, joins clients waiting for the shared constants. */
		void Service(State& a_state)
		{
			const bool waiting = std::ranges::any_of(a_state.clients, [](const Client& a_client) { return !InGraphs(*a_client.desc); });
			if (waiting && SharedConstants::Ensure()) {
				Publish(a_state);
				std::uint32_t mask = 0;
				for (auto& client : a_state.clients)
					if (client.desc->sharedConstants) {
						MarkCurrent(a_state, client);
						mask |= AnchorMask(*client.desc);
					}
				Rebuild(a_state, mask, "the shared constants exist");
				logger::info("[ORG] frame anchors: the shared constants exist; the clients that read them join the graphs");
			} else if (waiting && SharedConstants::Failed()) {
				std::vector<std::shared_ptr<const ClientDesc>> failed;
				std::erase_if(a_state.clients, [&](const Client& a_client) {
					if (InGraphs(*a_client.desc))
						return false;
					failed.push_back(a_client.desc);
					return true;
				});
				Publish(a_state);
				for (const auto& client : failed)
					RetireGates(a_state, *client);
				FailClients(fmt::format("the clients [{}] could not join the graphs", IdsOf(failed)), "the shared constants they read could not be made");
			}
		}

		/** @brief A client with work at this run, and whether its commit runs. */
		struct RunClient
		{
			std::shared_ptr<const ClientDesc> desc;
			std::uint64_t serial = 0;
			bool active = false;
		};
	}

	namespace
	{
		std::atomic<DWORD> mainThread{ 0 };
		// With CS_ORG_LOADING_THREAD=1, the thread that owns rendering. 0 means the main thread, before the first claim.
		std::atomic<DWORD> renderOwner{ 0 };
		std::atomic<std::uint64_t> ownerChanges{ 0 };

		/** @brief The thread that owns rendering now: the last claim's, or the main thread before any. */
		DWORD Owner()
		{
			const DWORD stored = renderOwner.load(std::memory_order_acquire);
			return stored ? stored : mainThread.load(std::memory_order_acquire);
		}
	}

	void LatchMainThread()
	{
		mainThread.store(::GetCurrentThreadId(), std::memory_order_release);
	}

	bool IsMainThread()
	{
		return ::GetCurrentThreadId() == mainThread.load(std::memory_order_acquire);
	}

	std::uint32_t RunSegment(Anchor a_anchor)
	{
		const auto index = IndexOf(a_anchor);
		if (index >= kAnchors)
			return static_cast<std::uint32_t>(Segment::Count);
		if (const auto d = DeclaredOfAnchor(Anchors(), index); d < kDeclared)
			return static_cast<std::uint32_t>(declaredInfo[d].segment);
		return SegmentOf(a_anchor);
	}

	bool IsRenderOwner()
	{
		const DWORD main = mainThread.load(std::memory_order_acquire);
		const DWORD self = ::GetCurrentThreadId();
		if (main == 0)
			return true;
		if (!RenderGraph::Engine::LoadingScreen::Requested()) {
			// A loading screen presents from a job thread (SE 38085). The anchor state belongs to the main thread,
			// so one-shot passes wait for the next main-thread frame.
			if (main == self)
				return true;
			static std::atomic_flag logged;
			if (!logged.test_and_set())
				logger::info("[ORG] frame anchors: a call on thread {} (a loading screen or a movie), not the main thread {}: the graph waits for the main thread",
					self, main);
			return false;
		}
		return Owner() == self;
	}

	void ClaimRenderOwnership()
	{
		if (!RenderGraph::Engine::LoadingScreen::Requested() || mainThread.load(std::memory_order_acquire) == 0)
			return;
		const DWORD self = ::GetCurrentThreadId();
		// Every engine frame begins here: most claims are by the thread that already owns rendering.
		if (Owner() == self)
			return;
		// Drain the last owner's epochs to the queue first. Do it before taking the anchors' lock, since a pass recorded there may take it.
		RenderGraphRuntime::Get().DrainSubmissions("render ownership moved", true);
		// Under the mutex so no claim lands inside another thread's run or a main-thread Register.
		auto lock = Guard();
		const DWORD previous = Owner();
		if (previous == self)
			return;
		renderOwner.store(self, std::memory_order_release);
		const auto changes = ownerChanges.fetch_add(1, std::memory_order_acq_rel) + 1;
		RenderGraphRuntime::Get().MoveOwnerThread();
		if (changes <= 32 || changes % 1000 == 0)
			logger::info("[ORG] render ownership: thread {} begins a frame and takes it from thread {} (change {})", self, previous, changes);
	}

	std::uint64_t RenderOwnerChanges()
	{
		return ownerChanges.load(std::memory_order_acquire);
	}

	namespace
	{
		/** @brief How deep the calling thread is in Run (InRun). */
		thread_local std::uint32_t t_runDepth = 0;

		/** @brief Counts the calling thread into Run for the scope. */
		struct RunDepthScope
		{
			RunDepthScope() { ++t_runDepth; }
			~RunDepthScope() { --t_runDepth; }
			RunDepthScope(const RunDepthScope&) = delete;
			RunDepthScope& operator=(const RunDepthScope&) = delete;
		};
	}

	bool InRun()
	{
		return t_runDepth != 0;
	}

	namespace
	{
		/** @brief Fails every client after the graph stopped at a_name's epoch. */
		void StopAll(State& a_state, const char* a_name)
		{
			// What the stages staged is dropped uncommitted.
			for (std::uint32_t d = 0; d < kDeclared; ++d) {
				a_state.stagedFrame[d] = 0;
				a_state.stagedExecuted[d].clear();
			}
			a_state.stagesOpen.store(false, std::memory_order_release);
			a_state.stagesOpenMask.store(0, std::memory_order_release);
			auto failed = std::move(a_state.clients);
			a_state.clients.clear();
			a_state.retired.clear();
			Publish(a_state);
			std::vector<std::shared_ptr<const ClientDesc>> descs;
			for (const auto& client : failed)
				descs.push_back(client.desc);
			FailClients(fmt::format("the render graph stopped at the {} epoch with the clients [{}]", a_name, IdsOf(descs)),
				RenderGraphRuntime::Get().GetDisabledReason());
		}

		/** @brief A lasting copy of a_text, since the census keeps pointers. Render owner. */
		const char* Interned(std::string a_text)
		{
			static std::set<std::string> texts;
			return texts.insert(std::move(a_text)).first->c_str();
		}

		/** @brief The frame sequence census: a run's active clients' ids, comma separated. */
		void NoteSequenceRun(const Point& a_point, RenderGraph::FrameSequence::RunKind a_kind, const std::vector<RunClient>& a_clients)
		{
			if (!RenderGraph::FrameSequence::On())
				return;
			std::string ids;
			for (const auto& client : a_clients)
				if (client.active)
					ids += (ids.empty() ? "" : ",") + client.desc->id;
			RenderGraph::FrameSequence::NoteRun(kInfo[IndexOf(a_point.anchor)].name, a_kind, ids, a_point.input, a_point.output);
		}

	}

	void BeforeGpuWork(const char* a_why, std::uint32_t a_segment)
	{
		auto& state = Anchors();
		// A point-ended stage commits just before the next GPU work in its table entry's commitBefore list.
		// These are frames where its usual commit point, the blended draw or the present, did not come first.
		if (a_segment != UINT32_MAX && state.host && IsRenderOwner())
			for (std::uint32_t d = 0; d < kDeclared; ++d)
				if (std::ranges::find(declaredInfo[d].commitBefore, static_cast<Segment>(a_segment)) != declaredInfo[d].commitBefore.end())
					(void)TapPoint(static_cast<RenderGraph::Frame::SegmentId>(d));
		// While a stage holds its ticket, other GPU work submitted now runs ahead of the stage's work. Counted by reason for the census.
		if (state.stagesOpen.load(std::memory_order_acquire) && IsRenderOwner()) {
			auto lock = Guard();
			bool own = false;
			for (std::uint32_t d = 0; d < kDeclared; ++d)
				own = own || (state.stagedFrame[d] && a_segment == static_cast<std::uint32_t>(declaredInfo[d].segment));
			// Run already counted this epoch, and its submission is the same GPU work.
			const bool counted = a_segment != UINT32_MAX && state.insideHeldCounted == a_segment;
			if (counted)
				state.insideHeldCounted = UINT32_MAX;
			if (!own && !counted && !state.submittingStage.load(std::memory_order_relaxed)) {
				const char* why = a_segment != UINT32_MAX ? Interned(fmt::format("{} (epoch {})", a_why, a_segment)) : a_why;
				auto found = std::ranges::find_if(state.insideHeld, [&](const auto& a_entry) { return std::string_view(a_entry.first) == why; });
				if (found == state.insideHeld.end())
					state.insideHeld.emplace_back(why, 1);
				else
					++found->second;
				if (++state.insideHeldLines <= 32) {
					std::string held;
					for (std::uint32_t d = 0; d < kDeclared; ++d)
						if (state.stagedFrame[d])
							held += fmt::format("{}{}", held.empty() ? "" : ", ", declaredInfo[d].names.label);
					logger::warn("[ORG] frame segments: {} was submitted between the first tap and the commit of the {}; it runs ahead of the stage's work (frame {})", why, held,
						RenderGraph::Frame::Driver::Current().frame);
				}
			}
		}
	}


	namespace
	{
		using RenderGraph::Frame::SegmentFields;
		using RenderGraph::Frame::SegmentId;
		namespace Driver = RenderGraph::Frame::Driver;

		/** @brief A declared segment's fields for a frame, against a given build. */
		SegmentFields DescribeDeclaredFrom(State& a_state, std::uint32_t a_declared, bool a_declaredByKind, const std::shared_ptr<const Snapshot>& a_built,
			const std::shared_ptr<std::vector<std::weak_ptr<org::RenderPass>>>& a_passes, std::uint64_t a_graph, const BuildKey* a_key, bool a_keyed);

		/** @brief A declared segment's fields against its attached build. a_keyed cuts it to what the build's key has present. */
		SegmentFields DescribeDeclared(State& a_state, std::uint32_t a_declared, bool a_declaredByKind, bool a_keyed = true)
		{
			if (!a_declaredByKind || !a_state.host)
				return DescribeDeclaredFrom(a_state, a_declared, a_declaredByKind, nullptr, nullptr, 0, nullptr, false);
			const auto key = a_state.declaredKeys[a_declared].load(std::memory_order_acquire);
			return DescribeDeclaredFrom(a_state, a_declared, a_declaredByKind, a_state.declaredSnapshots[a_declared].load(std::memory_order_acquire),
				a_state.declaredPasses[a_declared].load(std::memory_order_acquire), a_state.host->BuildGeneration(EpochOfDeclared(a_declared)), key.get(), a_keyed);
		}

		/** @brief A declared segment's fields for a frame against a build. a_keyed cuts it to what the key has present. */
		SegmentFields DescribeDeclaredFrom(State& a_state, std::uint32_t a_declared, bool a_declaredByKind, const std::shared_ptr<const Snapshot>& a_built,
			const std::shared_ptr<std::vector<std::weak_ptr<org::RenderPass>>>& a_passes, std::uint64_t a_graph, const BuildKey* a_key, bool a_keyed)
		{
			SegmentFields fields;
			fields.declared = a_declaredByKind;
			if (!a_declaredByKind || !a_state.host)
				return fields;
			const auto& built = a_built;
			if (!built)
				return fields;  // graph 0: not built yet
			fields.graph = a_graph;
			fields.versions = a_state.declaredRecordings[a_declared].versionGeneration;
			const auto& clients = SegmentClients(*built, a_declared);
			// A stage with no client and no module step has nothing to record, so it is absent from the frame.
			bool foreignStep = false;
			for (std::uint32_t k = 0; k < declaredInfo[a_declared].stepCount; ++k)
				foreignStep = foreignStep || declaredInfo[a_declared].steps[k].foreign != Segment::Count;
			if (clients.empty() && !foreignStep) {
				fields.graph = 0;
				return fields;
			}
			fields.clients = static_cast<std::uint32_t>(clients.size());
			for (const auto& client : clients)
				if (client->rows) {
					std::array<std::uint64_t, 4> rows{};
					a_state.describingAgainst = a_key;
					a_state.describingClient = client.get();
					client->rows(rows);
					a_state.describingAgainst = nullptr;
					a_state.describingClient = nullptr;
					for (std::size_t w = 0; w < rows.size(); ++w)
						fields.rows[w] |= rows[w];
				}
			const auto& info = declaredInfo[a_declared];
			for (std::size_t i = 0; i < clients.size() && i < RenderGraph::Frame::kSegmentClients; ++i) {
				if (!clients[i]->declared || clients[i]->declared())
					fields.clientsOn |= 1u << i;
				if (clients[i]->maxInstances > 1)
					fields.instances[i] = static_cast<std::uint8_t>(std::min<std::uint32_t>(
						{ clients[i]->instances ? clients[i]->instances() : 1u, clients[i]->maxInstances, 255u }));
			}
			for (std::uint32_t k = 0; k < info.stepCount; ++k)
				if (info.steps[k].foreign != Segment::Count)
					if (const auto foreign = ForeignOf(info.steps[k].foreign); foreign && (!foreign->declared || foreign->declared()))
						fields.foreignOn |= 1u << k;
			// What the build's key left out is off in the description.
			if (a_keyed && a_key && !a_key->all) {
				for (std::size_t i = 0; i < clients.size(); ++i) {
					const auto [present, rows] = KeyPart(a_key, *clients[i]);
					const auto own = RowsOf(*clients[i]);
					for (std::size_t w = 0; w < 4; ++w)
						fields.rows[w] &= ~(own[w] & ~rows[w]);
					if (!present && i < RenderGraph::Frame::kSegmentClients) {
						fields.clientsOn &= ~(1u << i);
						fields.instances[i] = 0;
					}
				}
				fields.foreignOn &= KeyForeign(a_key, ~0u);
			}
			// A kept recording binds the revisions its passes depend on beyond their bindings, so they go in the description.
			// A pass that cannot tell is logged once per build.
			if (const auto& passes = a_passes) {
				std::uint64_t hash = 1469598103934665603ull;
				std::uint32_t untold = 0;
				for (const auto& weak : *passes) {
					const auto pass = weak.lock();
					std::uint64_t revision = 0;
					if (!pass || !pass->RevisionHash(revision)) {
						++untold;
						continue;
					}
					hash = (hash ^ revision) * 1099511628211ull;
				}
				fields.passes = hash;
				static std::array<std::uint64_t, kDeclared> logged{};
				if (untold && logged[a_declared] != fields.graph) {
					logged[a_declared] = fields.graph;
					logger::warn("[ORG] frame segments: {} of the {}'s passes recorded ahead give no revision; a change they depend on is not seen", untold,
						info.names.label);
				}
			}
			return fields;
		}

		/** @brief Stops the game when a segment recording fails, in every build. Drawing the frame another way would hide the defect. */
		[[noreturn]] void FailRecording(std::uint32_t a_declared, const SegmentFields& a_fields, std::uint32_t a_slot, std::string_view a_cause)
		{
			const auto message = fmt::format(
				"Community Shaders native mode: the {} could not be recorded, so the frame cannot be drawn. Description: graph build {}, "
				"version generation {}, clients on {:#x} of {}, module steps on {:#x}, pass revisions {:#x}, frame slot {}. Cause: {}",
				declaredInfo[a_declared].names.label, a_fields.graph, a_fields.versions, a_fields.clientsOn, a_fields.clients, a_fields.foreignOn,
				a_fields.passes, a_slot, a_cause);
			logger::critical("[ORG] frame segments: {}", message);
			stl::report_and_fail(message);
		}

		/**
		* @brief The segment recording for a_fields, kept or newly requested from the host's thread. Never waits. Render owner.
		* A kept one that failed stops the game. Requests outside BeginFrame count as mid-frame re-records.
		* a_versions names the versions it binds, which may be pending. A recording for other versions is another entry.
		*/
		std::shared_ptr<SegmentRecording> RequestDeclared(State& a_state, std::uint32_t a_declared, const SegmentFields& a_fields, std::uint32_t a_slot,
			std::uint64_t a_frame, std::string_view a_why, bool* a_asked = nullptr, const RecordVersions& a_versions = {})
		{
			if (a_asked)
				*a_asked = false;
			if (static_cast<bool>(a_versions.versions) != (a_versions.serial != 0))
				stl::report_and_fail(fmt::format("[ORG] frame segments: {} was asked for named versions without a serial, or a serial without versions",
					declaredInfo[a_declared].names.label));
			auto& recordings = a_state.declaredRecordings[a_declared];
			const auto hash = RenderGraph::Frame::FieldsHash(a_fields);
			std::string reason;
			auto entry = std::make_shared<SegmentRecording>();
			{
				std::unique_lock lock(recordings.mutex);
				for (auto it = recordings.entries.begin(); it != recordings.entries.end(); ++it) {
					auto& kept = **it;
					if (kept.hash != hash || kept.slot != a_slot || kept.versionsSerial != a_versions.serial || !(kept.fields == a_fields))
						continue;
					if (kept.done && !kept.recording) {
						if (!kept.dropped)
							FailRecording(a_declared, kept.fields, kept.slot, kept.error.empty() ? std::string("no recording") : kept.error);
						recordings.entries.erase(it);
						break;
					}
					kept.lastFrame = a_frame;
					return *it;
				}
				reason = recordings.any ? RenderGraph::Frame::DiffFields(recordings.last, a_fields) : std::string("the first description");
				if (reason.empty()) {
					// The description recorded for another slot or other versions already, or a recording of it that a build dropped.
					const bool other = std::ranges::any_of(recordings.entries, [&](const auto& a_entry) { return a_entry->hash == hash && a_entry->slot != a_slot; });
					const bool otherVersions = std::ranges::any_of(recordings.entries, [&](const auto& a_entry) {
						return a_entry->hash == hash && a_entry->slot == a_slot && a_entry->versionsSerial != a_versions.serial;
					});
					reason = other ? fmt::format("the same description, for slot {}", a_slot) :
					         otherVersions ? fmt::format("the same description, for named versions {}", a_versions.serial) :
					                         std::string("the same description, a build dropped its last recording");
				}
				recordings.last = a_fields;
				recordings.any = true;
				entry->hash = hash;
				entry->slot = a_slot;
				entry->versionsSerial = a_versions.serial;
				entry->fields = a_fields;
				entry->lastFrame = a_frame;
				// Recordings of an older build keep its graph and alias pools alive, tens of MB each, so they go.
				// The staged build's stay. It needs every slot ready to switch in, and alternating descriptions would starve it.
				const auto staged = a_state.host->StagedGeneration(EpochOfDeclared(a_declared));
				std::erase_if(recordings.entries, [&](const auto& a_kept) {
					return a_kept->done && a_kept->recording && !a_state.host->EpochRecordingCurrent(*a_kept->recording) &&
					       (staged == 0 || a_kept->recording->buildGeneration != staged);
				});
				// The least recently used of the finished ones goes when the list is full.
				while (recordings.entries.size() >= SegmentRecordings::kKeep) {
					auto oldest = recordings.entries.end();
					for (auto it = recordings.entries.begin(); it != recordings.entries.end(); ++it)
						if ((*it)->done && (oldest == recordings.entries.end() || (*it)->lastFrame < (*oldest)->lastFrame))
							oldest = it;
					if (oldest == recordings.entries.end())
						break;
					recordings.entries.erase(oldest);
				}
				recordings.entries.push_back(entry);
			}
			if (a_asked)
				*a_asked = true;
			Driver::NoteRecording(static_cast<SegmentId>(a_declared), a_frame, fmt::format("{}{}", reason, a_why));
			if (!a_state.inBegin)
				Driver::NoteLateRequest(static_cast<SegmentId>(a_declared));
			auto data = std::make_shared<SegmentHostData>();
			data->segment = a_declared;
			data->fields = a_fields;
			data->versions = a_versions.versions;
			const bool posted = a_state.host->RequestEpochRecording(EpochOfDeclared(a_declared), std::move(data),
				[&recordings, entry](std::shared_ptr<const org::GraphHost::EpochRecording> a_recording, std::exception_ptr a_error) {
					std::string error;
					bool dropped = false;
					if (a_error) {
						try {
							std::rethrow_exception(a_error);
						} catch (const org::GraphHost::EpochRecordingDropped& e) {
							error = e.what();
							dropped = true;
						} catch (const std::exception& e) {
							error = e.what();
						} catch (...) {
							error = "unknown error";
						}
					}
					if (!a_recording && !a_error)
						error = "the host's thread completed the request with neither a recording nor an error";
					{
						const std::lock_guard lock(recordings.mutex);
						entry->recording = std::move(a_recording);
						entry->error = std::move(error);
						entry->dropped = dropped;
						entry->done = true;
					}
				},
				a_slot, a_fields.graph);
			if (!posted) {
				const std::lock_guard lock(recordings.mutex);
				entry->error = "the host takes no recording requests (async epochs are off)";
				entry->done = true;
			}
			return entry;
		}

		/**
		* @brief A segment's schedule for this frame. A slot keeps its schedule until its recording of the new description is ready.
		* Nothing waits. Graph 0 in the result means the segment is absent this frame. Render owner.
		*/
		SegmentFields Schedule(State& a_state, std::uint32_t a_declared, bool a_world, const RenderGraph::Frame::Description& a_frame)
		{
			const auto segment = static_cast<SegmentId>(a_declared);
			auto& host = *a_state.host;
			const std::uint32_t slot = a_frame.slot;
			auto& active = a_state.schedule[a_declared][slot];
			auto& recordings = a_state.declaredRecordings[a_declared];
			// A schedule of an older build is gone. Builds attach only at a frame boundary, before this.
			if (active.recording && !host.EpochRecordingCurrent(*active.recording))
				active = {};
			// A schedule whose recording binds a replaced version gets a new description for the new version.
			// The slot keeps the old schedule until that is ready, so the stage lags a frame or two but is never blank.
			// Only slots published outside frame revisions lag (engine imports, the native back buffer), since a revision switches every
			// stage that declares its slots. The census counts the lags by slot.
			a_state.frameVersionsCurrent[a_declared] = true;
			if (active.recording && !host.EpochRecordingVersionsCurrent(*active.recording, slot)) {
				if (active.fields.versions == recordings.versionGeneration) {
					++recordings.versionGeneration;
					// The slot's name, without the version numbers, so the census groups by slot.
					const auto stale = host.EpochRecordingStaleVersion(*active.recording, slot);
					RenderGraph::Frame::Boundary::NoteVersionLag(std::string_view(stale).substr(0, stale.find(" (slot ")));
				}
				a_state.frameVersionsCurrent[a_declared] = false;
			}
			// A staged build switches in with a frame revision, recorded for every slot (Frame::Revision). Until then the slots keep the
			// attached build's schedule.
			const auto epoch = EpochOfDeclared(a_declared);
			const bool stillStaged = a_world && host.StagedGeneration(epoch) != 0;
			// The frame's description cut to the attached build's key, and the full one. Taps treat the difference as lagged work.
			SegmentFields wanted = DescribeDeclared(a_state, a_declared, a_world);
			a_state.frameWanted[a_declared] = DescribeDeclared(a_state, a_declared, a_world, false);
			const bool keyLags = !(a_state.frameWanted[a_declared] == wanted);
			// A stage outside its frame kinds still gets a first schedule recorded ahead for every slot.
			// After a kind switch its first frames may lag, but are never blank.
			if (!a_world && host.StagedGeneration(epoch) == 0) {
				const SegmentFields ahead = DescribeDeclared(a_state, a_declared, true);
				for (std::uint32_t s = 0; s < RenderGraphRuntime::kGameFramesInFlight && ahead.graph; ++s) {
					auto& kept = a_state.schedule[a_declared][s];
					if (kept.recording && host.EpochRecordingCurrent(*kept.recording))
						continue;
					// Parity fields fit only the slots of this frame's parity.
					if (s % 2 != a_frame.parity)
						continue;
					if (kept.recording && kept.fields == ahead && host.EpochRecordingCurrent(*kept.recording)) {
						// A version was replaced since. The next frame's description names the new one.
						if (!host.EpochRecordingVersionsCurrent(*kept.recording, s) && kept.fields.versions == recordings.versionGeneration)
							++recordings.versionGeneration;
						continue;
					}
					const auto entry = RequestDeclared(a_state, a_declared, ahead, s, a_frame.frame, " (ahead, for the next frame of its kinds)");
					std::shared_ptr<const org::GraphHost::EpochRecording> ready;
					{
						const std::lock_guard lock(recordings.mutex);
						if (entry->done)
							ready = entry->recording;
					}
					if (ready && host.EpochRecordingCurrent(*ready) && host.EpochRecordingVersionsCurrent(*ready, s))
						kept = { ahead, std::move(ready), a_state.declaredSnapshots[a_declared].load(std::memory_order_acquire) };
					else if (ready && host.EpochRecordingCurrent(*ready) && ahead.versions == recordings.versionGeneration)
						++recordings.versionGeneration;
				}
			}
			if (!wanted.declared)
				return wanted;
			if (!wanted.graph) {
				Driver::NoteScheduled(segment, Driver::Scheduled::Absent);
				return wanted;
			}
			// While a build is staged the attached graph's bindings are frozen, so slots keep what they have.
			if (!(active.recording && active.fields == wanted) && !stillStaged) {
				std::shared_ptr<SegmentRecording> entry;
				for (std::uint32_t ahead = 0; ahead < RenderGraphRuntime::kGameFramesInFlight; ++ahead) {
					auto requested = RequestDeclared(a_state, a_declared, wanted, (slot + ahead) % RenderGraphRuntime::kGameFramesInFlight, a_frame.frame, "");
					if (ahead == 0)
						entry = std::move(requested);
				}
				const auto hash = RenderGraph::Frame::FieldsHash(wanted);
				if (a_state.waitHash[a_declared] != hash) {
					a_state.waitHash[a_declared] = hash;
					a_state.waitFrame[a_declared] = a_frame.frame;
				}
				std::shared_ptr<const org::GraphHost::EpochRecording> ready;
				{
					const std::lock_guard lock(recordings.mutex);
					if (entry->done)
						ready = entry->recording;
				}
				if (ready && host.EpochRecordingCurrent(*ready) && host.EpochRecordingVersionsCurrent(*ready, slot)) {
					const auto what = active.recording ? RenderGraph::Frame::DiffFields(active.fields, wanted) :
					                                     fmt::format("the slot's first schedule on build {}", wanted.graph);
					active = { wanted, std::move(ready), a_state.declaredSnapshots[a_declared].load(std::memory_order_acquire) };
					Driver::NoteSwitch(segment, a_frame.frame, slot, a_frame.frame - a_state.waitFrame[a_declared], what);
					// The UI stage switches its other slots at this boundary too, once their recordings are ready.
					for (std::uint32_t s = 0; s < RenderGraphRuntime::kGameFramesInFlight && declaredInfo[a_declared].switchesEverySlot; ++s) {
						auto& other = a_state.schedule[a_declared][s];
						if (s == slot || (other.recording && other.fields == wanted))
							continue;
						const auto otherEntry = RequestDeclared(a_state, a_declared, wanted, s, a_frame.frame, "");
						std::shared_ptr<const org::GraphHost::EpochRecording> otherReady;
						{
							const std::lock_guard lock(recordings.mutex);
							if (otherEntry->done)
								otherReady = otherEntry->recording;
						}
						if (otherReady && host.EpochRecordingCurrent(*otherReady) && host.EpochRecordingVersionsCurrent(*otherReady, s))
							other = { wanted, std::move(otherReady), active.built };
					}
				} else if (ready && host.EpochRecordingCurrent(*ready)) {
					// A slot replaced a version while this was recorded. The next frame's description names the new one.
					++recordings.versionGeneration;
				}
			}
			if (!active.recording) {
				Driver::NoteScheduled(segment, Driver::Scheduled::Absent);
				wanted.graph = 0;
				return wanted;
			}
			Driver::NoteScheduled(segment, active.fields == wanted && !keyLags ? Driver::Scheduled::Current : Driver::Scheduled::Lagging);
			a_state.frameRecording[a_declared] = active.recording;
			a_state.frameBuilt[a_declared] = active.built;
			return active.fields;
		}

		/** @brief Seconds of a stage's own clock that a pass no frame wants stays present. */
		constexpr double kKeyHold = 10.0;

		/**
		* @brief A stage's key at Begin. Passes wanted within kKeyHold stay present. A steady scene asks for no build. Render owner.
		* A build is asked for when the key wants a pass the newest build lacks. Unused passes drop only at a load, a kind switch or after a provisional key.
		*/
		void UpdateKey(State& a_state, std::uint32_t a_declared, bool a_inRecipe, const RenderGraph::Frame::Description& a_frame, bool a_kindSwitch)
		{
			if (!a_state.host)
				return;
			auto& use = a_state.keyUse[a_declared];
			const auto snapshot = a_state.snapshot.load(std::memory_order_acquire);
			if (!snapshot)
				return;
			const auto& info = declaredInfo[a_declared];
			const auto& clients = SegmentClients(*snapshot, a_declared);
			std::erase_if(use.clients, [&](const KeyUse::Client& a_client) { return std::ranges::find(clients, a_client.desc) == clients.end(); });
			const bool load = Driver::LoadStartedThisFrame();
			if (a_inRecipe) {
				const auto now = std::chrono::steady_clock::now();
				if (use.last != std::chrono::steady_clock::time_point{})
					use.clock += (std::min)(0.1, std::chrono::duration<double>(now - use.last).count());
				use.last = now;
				a_state.describingKey = true;
				const SegmentFields wanted = DescribeDeclaredFrom(a_state, a_declared, true, snapshot, nullptr, 1, nullptr, false);
				const SegmentFields other{};
				a_state.describingKey = false;
				if (wanted.graph) {
					for (std::size_t i = 0; i < clients.size(); ++i) {
						auto found = std::ranges::find_if(use.clients, [&](const KeyUse::Client& a_client) { return a_client.desc == clients[i]; });
						if (found == use.clients.end()) {
							use.clients.emplace_back().desc = clients[i];
							found = std::prev(use.clients.end());
						}
						const bool on = i >= RenderGraph::Frame::kSegmentClients || ((wanted.clientsOn | other.clientsOn) >> i & 1u);
						if (!on)
							continue;
						found->used = use.clock;
						const auto own = RowsOf(*clients[i]);
						for (std::uint32_t r = 0; r < 256; ++r)
							if ((own[r / 64] & (wanted.rows[r / 64] | other.rows[r / 64])) >> (r % 64) & 1u)
								found->rows[r] = use.clock;
					}
					for (std::uint32_t k = 0; k < info.stepCount && k < 32; ++k)
						if ((wanted.foreignOn | other.foreignOn) >> k & 1u)
							use.foreign[k] = use.clock;
				}
			} else {
				use.last = {};
				// Outside the stage's frames a key changes only at a load or a kind switch, and only once the stage was described.
				if ((!load && !a_kindSwitch) || use.clients.empty())
					return;
			}
			// The key keeps what was wanted within the hold. A client never described is unlisted, so it is present whole.
			auto keep = std::make_shared<BuildKey>();
			keep->all = false;
			keep->frame = a_frame.frame;
			bool any = false;
			for (const auto& client : clients) {
				const auto found = std::ranges::find_if(use.clients, [&](const KeyUse::Client& a_client) { return a_client.desc == client; });
				if (found == use.clients.end()) {
					any = true;
					continue;
				}
				BuildKey::Client part{ .desc = client, .present = use.clock - found->used <= kKeyHold };
				if (part.present) {
					const auto own = RowsOf(*client);
					for (std::uint32_t r = 0; r < 256; ++r)
						if ((own[r / 64] >> (r % 64) & 1u) && use.clock - found->rows[r] <= kKeyHold)
							part.rows[r / 64] |= 1ull << (r % 64);
					any = true;
				}
				keep->clients.push_back(part);
			}
			for (std::uint32_t k = 0; k < info.stepCount && k < 32; ++k)
				if (info.steps[k].foreign != Segment::Count && use.clock - use.foreign[k] <= kKeyHold) {
					keep->foreign |= 1u << k;
					any = true;
				}
			// A stage is never left with nothing to record: its first client stays.
			if (!any && !keep->clients.empty()) {
				keep->clients.front().present = true;
				keep->clients.front().rows = RowsOf(*keep->clients.front().desc);
			}
			const auto epoch = EpochOfDeclared(a_declared);
			const bool hasStaged = a_state.host->StagedGeneration(epoch) != 0 && a_state.stagedSnapshots[a_declared].load(std::memory_order_acquire);
			const auto built = hasStaged ? a_state.stagedKeys[a_declared].load(std::memory_order_acquire) : a_state.declaredKeys[a_declared].load(std::memory_order_acquire);
			const auto next = a_state.nextKeys[a_declared].load(std::memory_order_acquire);
			std::uint32_t steps = 0;
			for (std::uint32_t k = 0; k < info.stepCount && k < 32; ++k)
				if (info.steps[k].foreign != Segment::Count)
					steps |= 1u << k;
			const auto equal = [&](const BuildKey* a_left, const BuildKey* a_right) {
				return a_left && a_right && KeyCovers(a_left, a_right, clients, steps) && KeyCovers(a_right, a_left, clients, steps);
			};
			const bool covered = KeyCovers(built.get(), keep.get(), clients, steps);
			const bool stale = !KeyCovers(keep.get(), built.get(), clients, steps);
			// A requested build that has not arrived is in flight. One that never came, after a failure, is asked for again.
			constexpr std::uint64_t kRequestFrames = 600;
			const bool inFlight = use.requestedSerial && (!built || built->serial < use.requestedSerial) && a_frame.frame - use.requestedFrame < kRequestFrames;
			const char* why = nullptr;
			if (!covered) {
				if (!(inFlight && next && KeyCovers(next.get(), keep.get(), clients, steps)))
					why = "a frame wants passes the build left out";
			} else if (stale) {
				const bool provisional = KeyProvisional(built.get(), clients);
				if ((provisional || load || a_kindSwitch) && !(inFlight && equal(next.get(), keep.get())))
					why = provisional ? (!built || built->all ? "the stage's first key" : "a client registered since the build") :
					      load        ? "unused passes leave at a load" :
					                    "unused passes leave at a frame-kind switch";
			}
			if (why) {
				keep->serial = ++a_state.keySerial;
				const auto text = DiffKeys(built.get(), keep.get(), clients, steps);
				use.requestedSerial = keep->serial;
				use.requestedFrame = a_frame.frame;
				a_state.nextKeys[a_declared].store(keep, std::memory_order_release);
				logger::info("[ORG] absent passes: the {} asks for a build at frame {} ({}): {}", info.names.label, a_frame.frame, why, text);
				RenderGraph::Builds::Request(epoch, fmt::format("frame anchors: the {}'s key: {}", info.names.label, why));
				Driver::NoteKeyBuild(static_cast<SegmentId>(a_declared));
			} else if (!equal(next.get(), keep.get())) {
				// Passes unused for the hold leave with the stage's next build, whatever asks for it.
				keep->serial = ++a_state.keySerial;
				a_state.nextKeys[a_declared].store(keep, std::memory_order_release);
			}
		}

		std::atomic<void (*)()> windowsClosedHook{ nullptr };

		void UpdateStagesOpen(State& a_state)
		{
			std::uint32_t mask = 0;
			for (std::uint32_t d = 0; d < kDeclared; ++d)
				if (a_state.stagedFrame[d] != 0)
					mask |= 1u << d;
			a_state.stagesOpenMask.store(mask, std::memory_order_release);
			const bool open = mask != 0;
			if (!a_state.stagesOpen.exchange(open, std::memory_order_acq_rel) || open)
				return;
			if (const auto hook = windowsClosedHook.load(std::memory_order_acquire))
				hook();
		}

		/** @brief Drops what a stage's taps staged this frame without committing it. Render owner. */
		void DropStaged(State& a_state, std::uint32_t a_declared)
		{
			a_state.stagedFrame[a_declared] = 0;
			a_state.stagedExecuted[a_declared].clear();
			UpdateStagesOpen(a_state);
		}

		/** @brief Every run gate a stage's graph holds, including those of departed registrations. */
		template <class F>
		void ForEachStageGate(const State& a_state, std::uint32_t a_declared, const Snapshot& a_built, F&& a_do)
		{
			const auto anchors = StepAnchorsOf(a_declared);
			for (const auto& client : SegmentClients(a_built, a_declared))
				for (const auto& gate : client->gates)
					if (gate)
						a_do(*gate);
			for (const auto& retired : a_state.retired)
				if ((retired.mask & anchors) && a_built.serial < retired.serial)
					for (const auto& gate : retired.gates)
						if (gate)
							a_do(*gate);
		}

		/** @brief Empties a present stage's frame inputs for this slot. Waits for nothing, since the GPU never reads the staging run. */
		void OpenInputs(State& a_state, std::uint32_t a_declared, std::uint32_t a_slot)
		{
			auto& host = *a_state.host;
			const auto& built = a_state.frameBuilt[a_declared];
			a_state.stepTaps[a_declared].fill(0);
			a_state.stagedExecuted[a_declared].clear();
			if (!built)
				return;
			ZoneScopedN("CS.Stage.OpenInputs");
			const auto staging = host.EpochStagingRun(EpochOfDeclared(a_declared), a_slot);
			a_state.stagingRun[a_declared] = staging;
			// A gate its client does not list reads closed too, unless a tap opens it.
			org::LatchBlock::ZeroStableRun(staging.lease);
			RenderGraph::RunGate::ClearOpenAll(staging);
			ForEachStageGate(a_state, a_declared, *built, [&](const RenderGraph::RunGate& a_gate) { a_gate.Close(staging); });
		}

		/** @brief A stage's one commit. Copies the staged values into the slot's copy and submits its ticket. Render owner, under the mutex. */
		RunResult SubmitSegment(State& a_state, const Point& a_point, std::uint32_t a_declared)
		{
			const auto segment = static_cast<SegmentId>(a_declared);
			const auto& frame = Driver::Current();
			auto& host = *a_state.host;
			auto& runtime = RenderGraphRuntime::Get();
			const std::uint32_t epoch = EpochOfDeclared(a_declared);
			// The frame runs on the schedule it took at Begin. Nothing is described, recorded or waited for here.
			const auto recording = a_state.frameRecording[a_declared];
			const auto built = a_state.frameBuilt[a_declared];
			const auto staging = a_state.stagingRun[a_declared];
			auto executed = std::exchange(a_state.stagedExecuted[a_declared], {});
			const auto mismatch = [&](Driver::TapError a_error, std::string_view a_what) {
				Driver::NoteTapError(a_error, segment, a_what);
				DropStaged(a_state, a_declared);
				return RunResult{ RunStatus::Idle };
			};
			if (!recording || !built)
				return mismatch(Driver::TapError::NotDeclared, "its commit came in a frame without a schedule for it");
			if (!host.EpochRecordingCurrent(*recording))
				return mismatch(Driver::TapError::LostToBuild, "a build of its graph attached after the frame's Begin; builds attach only at a frame boundary");
			if (a_state.frameVersionsCurrent[a_declared] && !host.EpochRecordingVersionsCurrent(*recording, frame.slot))
				return mismatch(Driver::TapError::StaleRecording,
					fmt::format("slot {} of its graph published a new version after the frame's Begin; versions are published only at a frame boundary",
						host.EpochRecordingStaleVersion(*recording, frame.slot)));
			const auto expected = host.EpochCopyRun(epoch, frame.slot);
			RenderGraphRuntime::EpochResult result = RenderGraphRuntime::EpochResult::Stopped;
			try {
				struct SubmittingScope
				{
					std::atomic<bool>& flag;
					explicit SubmittingScope(std::atomic<bool>& a_flag) :
						flag(a_flag) { flag.store(true, std::memory_order_release); }
					~SubmittingScope() { flag.store(false, std::memory_order_release); }
				} submitting{ a_state.submittingStage };
				host.SetEpochCopy(epoch, frame.slot);
				RenderGraphRuntime::EpochBodyScope body(declaredInfo[a_declared].segment);
				result = runtime.TryExecuteEpoch(declaredInfo[a_declared].segment, [&](org::RenderGraph&) {
					// Once the ticket is taken the copy's last run is done, and the staged values become this run's.
					const auto run = host.CurrentRun();
					if (!(run == expected))
						Driver::NoteTapError(Driver::TapError::SlotMismatch, segment,
							fmt::format("the run is lease {} copy {}, the slot's copy is lease {} copy {}", run.lease, run.copy, expected.lease, expected.copy));
					RenderGraph::RunShapes::BeginRun();
					{
						ZoneScopedN("CS.Stage.CopyLatch");
						org::LatchBlock::CopyStableRun(staging.lease, run.lease);
					}
					{
						ZoneScopedN("CS.Stage.CopyGates");
						RenderGraph::RunGate::CopyOpenAll(staging, run);
					}
					{
						ZoneScopedN("CS.Stage.MoveSlots");
						RenderGraph::RunSlotsRegistry::MoveAll(staging, run);
					}
					host.UseEpochRecording(recording);
				});
			} catch (const std::exception& e) {
				logger::error("[ORG] frame segments: the {} could not be submitted: {}", declaredInfo[a_declared].names.label, e.what());
				result = RenderGraphRuntime::EpochResult::Stopped;
			}
			// Submitted or not, the frame's inputs of the stage are spent.
			a_state.stagedFrame[a_declared] = 0;
			UpdateStagesOpen(a_state);
			if (result == RenderGraphRuntime::EpochResult::Skipped) {
				// A stage runs once a frame on its slot's copy, sealed at present, so a busy copy is a pacing defect. Fatal in every build.
				const auto message = fmt::format("Community Shaders: the {} found its frame slot's published copy busy at frame {} (slot {}) and would have "
												 "skipped the frame's work. A stage never skips: the copy's last run was neither sealed by a present nor done.",
					declaredInfo[a_declared].names.label, frame.frame, frame.slot);
				logger::critical("[ORG] frame segments: {}", message);
				spdlog::default_logger()->flush();
				stl::report_and_fail(message);
			}
			if (result == RenderGraphRuntime::EpochResult::EpochFailed)
				return { RunStatus::Failed };
			if (result != RenderGraphRuntime::EpochResult::Submitted) {
				StopAll(a_state, declaredInfo[a_declared].names.label);
				return { RunStatus::Failed };
			}
			const auto anchors = StepAnchorsOf(a_declared);
			bool late = false;
			for (std::uint32_t i = 0; i < kAnchors; ++i)
				late = late || ((anchors >> i & 1u) && kInfo[i].late);
			if (late && a_state.lateHook)
				a_state.lateHook();
			if (SegmentUsesSharedConstants(*built, a_declared))
				SharedConstants::CheckAfterRun();
			// A point step or a cut ends at no anchor, so it has no run shape.
			if (RenderGraph::RunShapes::On() && a_point.anchor != Anchor::Count) {
				const std::uint32_t index = IndexOf(a_point.anchor);
				RenderGraph::RunShapes::EndRun(index, kInfo[index].name, a_point.input, a_point.output, 0);
			}
			PruneRetired(a_state);
			// Rebuild only the anchors whose one-shot passes ran. Rebuilding every anchor of the stage also rebuilt
			// its shared anchors' epochs unstaged, which held Drawcall Limit Fix after each load.
			std::uint32_t ranOnce = 0;
			for (auto& client : a_state.clients)
				for (std::size_t i = 0; i < client.desc->passes.size(); ++i)
					if (client.armed[i] && client.armed[i] <= built->serial && SegmentHoldsPass(a_declared, *client.desc, client.desc->passes[i])) {
						client.armed[i] = 0;
						ranOnce |= 1u << IndexOf(client.desc->passes[i].anchor);
					}
			if (ranOnce) {
				Publish(a_state);
				Rebuild(a_state, ranOnce, "one-shot passes ran");
			}
			// Last, since an executed callback may register, refresh or rearm.
			for (const auto& [client, point] : executed)
				if (client->executed)
					client->executed(point);
			return { RunStatus::Ran };
		}

		/**
		* @brief A stage's tap at one of its steps' hooks. Clients stage their values into the slot's staging run. No ticket is taken.
		* The last step's tap is the stage's commit. Render owner, under the mutex.
		*/
		RunResult TapDeclared(State& a_state, const Point& a_point, std::uint32_t a_declared, std::uint32_t a_step, const std::vector<RunClient>& a_clients,
			const std::function<void()>* a_foreign = nullptr)
		{
			const auto segment = static_cast<SegmentId>(a_declared);
			const auto& info = declaredInfo[a_declared];
			const auto& step = info.steps[a_step];
			const char* tap = step.anchor != Anchor::Count ? kInfo[IndexOf(step.anchor)].name : step.point ? "the segment's end point" : "a module epoch";
			const bool end = a_step + 1 == info.stepCount;
			const auto& frame = Driver::Current();
			const SegmentFields& fields = frame.Segment(segment);
			if (!fields.declared) {
				Driver::NoteTapError(Driver::TapError::NotDeclared, segment, fmt::format("the {} tap fired", tap));
				return { RunStatus::Idle };
			}
			// A stage's first tap ends earlier stages that are still open because their commit point did not come.
			// Their tickets go first, in recipe order, so this stage's work follows theirs on the GPU.
			if (a_state.stagedFrame[a_declared] != frame.frame)
				for (std::uint32_t before = 0; before < a_declared; ++before) {
					if (a_state.endedFrame[before] == frame.frame || !a_state.declaredEpochs[before] || !frame.Segment(static_cast<SegmentId>(before)).declared)
						continue;
					if (a_state.stagedFrame[before] != frame.frame && !PointEnded(before))
						continue;
					for (std::uint32_t k = 0; k < declaredInfo[before].stepCount; ++k)
						if (declaredInfo[before].steps[k].point) {
							(void)TapDeclared(a_state, Point{}, before, k, {});
							break;
						}
				}
			if (!fields.graph) {
				// No schedule for this slot yet, so the stage is absent and draws nothing.
				// Its end tap still ends it, so later stages' windows open on time.
				Driver::NoteAbsentTap(segment);
				if (end)
					a_state.endedFrame[a_declared] = frame.frame;
				return { RunStatus::Idle };
			}
			if (a_state.endedFrame[a_declared] == frame.frame) {
				Driver::NoteTapError(Driver::TapError::Repeated, segment, fmt::format("the {} tap fired after the segment's end tap", tap));
				return { RunStatus::Idle };
			}
			if (end && !Driver::NoteTap(segment, tap))
				return { RunStatus::Idle };
			// An end point with nothing staged this frame: the stage has no work, and no ticket is submitted.
			if (end && step.point && a_state.stagedFrame[a_declared] != frame.frame) {
				a_state.endedFrame[a_declared] = frame.frame;
				return { RunStatus::Idle };
			}
			auto& host = *a_state.host;
			const std::uint32_t epoch = EpochOfDeclared(a_declared);
			const auto built = a_state.frameBuilt[a_declared];
			// The frame runs on the build its schedule was recorded on. Builds attach only at a frame boundary, before Begin chose it.
			if (!built || fields.graph != host.BuildGeneration(epoch)) {
				Driver::NoteTapError(Driver::TapError::LostToBuild, segment,
					fmt::format("a build of the segment's graph attached after the frame's Begin (schedule build {}, attached {})", fields.graph,
						host.BuildGeneration(epoch)));
				DropStaged(a_state, a_declared);
				if (end)
					a_state.endedFrame[a_declared] = frame.frame;
				return { RunStatus::Failed };
			}
			const SegmentFields& held = fields;
			// When the schedule lags the frame's description, work the schedule has off is left out. That is not a mismatch.
			const SegmentFields& wanted = a_state.frameWanted[a_declared];
			const bool comparable = wanted.graph == held.graph;
			// This step's clients that are on in the schedule and have work. An active client the schedule turned off is a mismatch,
			// unless the schedule only lags.
			std::vector<std::shared_ptr<const ClientDesc>> committed;
			bool foreignOn = false;
			const std::uint32_t local = a_state.stepTaps[a_declared][a_step]++;
			const std::uint32_t instance = step.firstInstance + local;
			if (a_foreign) {
				foreignOn = (held.foreignOn >> a_step & 1u) != 0;
				if (!foreignOn) {
					if (comparable && (wanted.foreignOn >> a_step & 1u))
						Driver::NoteLaggedWork(segment);
					else
						Driver::NoteTapError(Driver::TapError::NotInDescription, segment, fmt::format("{} had work at its tap, and the schedule has it off", tap));
				}
			} else {
				const auto& clients = SegmentClients(*built, a_declared);
				for (std::size_t i = 0; i < clients.size(); ++i) {
					if (!StepHolds(step, *clients[i]))
						continue;
					const auto found = std::ranges::find_if(a_clients, [&](const RunClient& a_client) { return SameClient(clients[i], a_client.desc); });
					const bool active = found != a_clients.end() && found->active;
					const bool on = i < RenderGraph::Frame::kSegmentClients && (held.clientsOn >> i & 1u);
					if (!on) {
						if (active) {
							if (comparable && i < RenderGraph::Frame::kSegmentClients && (wanted.clientsOn >> i & 1u))
								Driver::NoteLaggedWork(segment);
							else
								Driver::NoteTapError(Driver::TapError::NotInDescription, segment,
									fmt::format("{} had work at the {} tap, and the schedule has it off", clients[i]->id, tap));
						}
						continue;
					}
					if (!active)
						continue;
					// The k-th tap of the step writes instance k.
					const std::uint32_t declaredInstances = clients[i]->maxInstances > 1 ? held.instances[i] : clients[i]->repeats ? UINT32_MAX : 1u;
					const bool inRange = clients[i]->maxInstances <= 1 || local < StepInstances(step, *clients[i]).second;
					if (instance >= declaredInstances || !inRange) {
						if (clients[i]->maxInstances > 1 && comparable && instance < wanted.instances[i])
							Driver::NoteLaggedWork(segment);
						else
							Driver::NoteTapError(Driver::TapError::InstanceNotDeclared, segment,
								fmt::format("{} at the {} tap, instance {} of {} declared", clients[i]->id, tap, instance, declaredInstances));
						continue;
					}
					committed.push_back(clients[i]);
				}
			}
			if (step.anchor != Anchor::Count)
				NoteSequenceRun(a_point, end ? RenderGraph::FrameSequence::RunKind::Commit : RenderGraph::FrameSequence::RunKind::Held, a_clients);
			// Nothing on the GPU reads the staging run the commits write.
			a_state.stagedFrame[a_declared] = frame.frame;
			UpdateStagesOpen(a_state);
			{
				struct StagingScope
				{
					org::GraphHost& host;
					~StagingScope() { host.EndHeldRun(); }
				} scope{ host };
				RenderGraphRuntime::EpochBodyScope body(info.segment);
				host.UseRun(a_state.stagingRun[a_declared]);
				if (!a_foreign && StepUsesSharedConstants(*built, a_declared, a_step))
					SharedConstants::Commit(a_step);
				if (a_foreign && foreignOn)
					(*a_foreign)();
				a_state.tapInstance = instance;
				for (const auto& client : committed)
					if (client->commit)
						client->commit(a_point);
				a_state.tapInstance = 0;
			}
			if (step.anchor != Anchor::Count) {
				const std::uint32_t index = IndexOf(step.anchor);
				auto& runs = RunsSinceSeal(a_state, index);
				++runs;
				if (runs > a_state.peakRuns[index].load(std::memory_order_relaxed))
					a_state.peakRuns[index].store(runs, std::memory_order_relaxed);
			}
			if (!end) {
				// A staged tap counts as run. Its values reach the GPU with the stage's commit, before later work reads them.
				// Last, since an executed callback may register, refresh or rearm.
				for (const auto& client : committed)
					if (client->executed)
						client->executed(a_point);
				return { RunStatus::Ran };
			}
			for (const auto& client : committed)
				a_state.stagedExecuted[a_declared].emplace_back(client, a_point);
			a_state.endedFrame[a_declared] = frame.frame;
			return SubmitSegment(a_state, a_point, a_declared);
		}

		/** @brief Whether a declared segment's window is open now. Render owner. */
		bool WindowOpen(const State& a_state, std::uint32_t a_declared)
		{
			const auto& frame = Driver::Current();
			const auto& fields = frame.Segment(static_cast<SegmentId>(a_declared));
			if (!a_state.declaredEpochs[a_declared] || !fields.declared || !fields.graph || a_state.endedFrame[a_declared] == frame.frame)
				return false;
			if (a_state.stagedFrame[a_declared] == frame.frame)
				return true;
			// An earlier point-ended stage that staged nothing does not hold this one back, since this stage's first tap ends it.
			// A stage that opens last, the UI stage, waits for every earlier stage to end.
			const bool last = declaredInfo[a_declared].opensLast;
			for (std::uint32_t before = 0; before < a_declared; ++before)
				if (a_state.declaredEpochs[before] && frame.Segment(static_cast<SegmentId>(before)).declared && a_state.endedFrame[before] != frame.frame &&
					(last || !(PointEnded(before) && a_state.stagedFrame[before] != frame.frame)))
					return false;
			return true;
		}

		/** @brief Whether a step takes its anchor's taps now. An untilLater step does so until a later step of its stage taps. */
		bool StepTakesTaps(const State& a_state, std::uint32_t a_declared, std::uint32_t a_step)
		{
			const auto& info = declaredInfo[a_declared];
			if (!info.steps[a_step].untilLater || a_state.stagedFrame[a_declared] != Driver::Current().frame)
				return true;
			for (std::uint32_t k = a_step + 1; k < info.stepCount; ++k)
				if (a_state.stepTaps[a_declared][k])
					return false;
			return true;
		}

		/** @brief The first step of a declared segment that takes a tap of a_anchor now, or kMaxSteps. */
		std::uint32_t TapStepAt(const State& a_state, std::uint32_t a_declared, Anchor a_anchor)
		{
			for (std::uint32_t k = 0; k < declaredInfo[a_declared].stepCount; ++k)
				if (declaredInfo[a_declared].steps[k].anchor == a_anchor && StepTakesTaps(a_state, a_declared, k))
					return k;
			return kMaxSteps;
		}

		std::pair<std::uint32_t, std::uint32_t> OpenStepAt(const State& a_state, Anchor a_anchor)
		{
			for (std::uint32_t d = 0; d < kDeclared; ++d)
				if (WindowOpen(a_state, d))
					if (const auto k = TapStepAt(a_state, d, a_anchor); k < kMaxSteps)
						return { d, k };
			return { kDeclared, kMaxSteps };
		}

		/** @brief The step of a declared segment that holds a module epoch, or kMaxSteps. */
		std::uint32_t ForeignStep(std::uint32_t a_declared, std::uint32_t a_segment)
		{
			for (std::uint32_t k = 0; k < declaredInfo[a_declared].stepCount; ++k)
				if (static_cast<std::uint32_t>(declaredInfo[a_declared].steps[k].foreign) == a_segment)
					return k;
			return kMaxSteps;
		}
	}

	void SetForeignPasses(std::uint32_t a_segment, std::function<void(std::uint32_t, void*)> a_gather, std::function<bool()> a_declared)
	{
		if (!HoldsForeign(a_segment))
			return;
		{
			auto& table = Foreign();
			const std::lock_guard lock(table.mutex);
			auto entry = std::make_shared<ForeignPasses>();
			entry->gather = std::move(a_gather);
			entry->declared = std::move(a_declared);
			table.entries[a_segment] = std::move(entry);
		}
		auto lock = Guard();
		auto& state = Anchors();
		if (!state.host)
			return;
		for (std::uint32_t d = 0; d < kDeclared; ++d)
			if (state.declaredEpochs[d] && ForeignStep(d, a_segment) != kMaxSteps)
				RenderGraph::Builds::Request(EpochOfDeclared(d), fmt::format("frame segments: epoch {}'s passes joined the {}", a_segment, declaredInfo[d].names.label));
	}

	bool HoldsForeign(std::uint32_t a_segment)
	{
		const auto& state = Anchors();
		for (std::uint32_t d = 0; d < kDeclared; ++d)
			if (state.declaredEpochs[d] && ForeignStep(d, a_segment) != kMaxSteps)
				return true;
		return false;
	}

	RunResult TapForeign(std::uint32_t a_segment, const std::function<void()>& a_commit)
	{
		const RunDepthScope depth;
		auto& state = Anchors();
		if (!state.host)
			return { RunStatus::Idle };
		auto lock = Guard();
		if (!IsRenderOwner())
			return { RunStatus::Refused };
		if (!RenderGraphRuntime::Get().IsActive())
			return { RunStatus::Refused };
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			const auto step = state.declaredEpochs[d] ? ForeignStep(d, a_segment) : kMaxSteps;
			if (step == kMaxSteps)
				continue;
			if (!WindowOpen(state, d)) {
				// A segment absent this frame draws nothing, and that is not a mismatch.
				const auto& fields = Driver::Current().Segment(static_cast<SegmentId>(d));
				if (fields.declared && !fields.graph && state.endedFrame[d] != Driver::Current().frame)
					Driver::NoteAbsentTap(static_cast<SegmentId>(d));
				else
					Driver::NoteTapError(Driver::TapError::NotDeclared, static_cast<SegmentId>(d), fmt::format("the tap of epoch {} fired outside the segment", a_segment));
				return { RunStatus::Idle };
			}
			return TapDeclared(state, Point{}, d, step, {}, &a_commit);
		}
		return { RunStatus::Idle };
	}

	std::uint32_t TapInstance()
	{
		return Anchors().tapInstance;
	}

	bool StageOpen(std::uint32_t a_segment)
	{
		if (!RenderGraph::Engine::Native::GraphMode() || !IsRenderOwner() || a_segment >= kDeclared)
			return false;
		auto lock = Guard();
		return WindowOpen(Anchors(), a_segment);
	}

	bool StageAbsent(std::uint32_t a_segment)
	{
		if (!RenderGraph::Engine::Native::GraphMode() || !IsRenderOwner() || a_segment >= kDeclared)
			return false;
		auto lock = Guard();
		const auto& state = Anchors();
		const auto& frame = Driver::Current();
		const auto& fields = frame.Segment(static_cast<SegmentId>(a_segment));
		return state.declaredEpochs[a_segment] && fields.declared && !fields.graph && state.endedFrame[a_segment] != frame.frame;
	}

	bool HeldByStage(Anchor a_anchor)
	{
		const auto& state = Anchors();
		const std::uint32_t index = IndexOf(a_anchor);
		return state.frozen && index < kAnchors && DeclaredOfAnchor(state, index) < kDeclared;
	}

	void DropKeptRecordings(Anchor a_anchor, std::string_view a_why)
	{
		auto lock = Guard();
		auto& state = Anchors();
		const std::uint32_t index = IndexOf(a_anchor);
		if (!state.frozen || index >= kAnchors)
			return;
		std::size_t dropped = 0;
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			if (!state.declaredEpochs[d] || !(WholeAnchorsOf(d) >> index & 1u))
				continue;
			// A frame slot's schedule holds its own reference, so the recordings a frame runs on stay until it switches.
			auto& recordings = state.declaredRecordings[d];
			const std::lock_guard recordingsLock(recordings.mutex);
			dropped += std::erase_if(recordings.entries, [](const auto& a_kept) { return a_kept->done; });
		}
		if (dropped)
			logger::info("[ORG] frame segments: dropped {} kept recordings of the stages that hold the {} anchor ({})", dropped, kInfo[index].name, a_why);
	}

	std::optional<std::uint32_t> FirstGraphInstance(Anchor a_anchor)
	{
		const std::uint32_t index = IndexOf(a_anchor);
		if (!Anchors().frozen || index >= kAnchors)
			return std::nullopt;
		for (std::uint32_t d = 0; d < kDeclared; ++d)
			if (Anchors().declaredEpochs[d] && (WholeAnchorsOf(d) >> index & 1u))
				if (const auto k = StepAt(d, a_anchor); k < kMaxSteps)
					return declaredInfo[d].steps[k].firstInstance;
		return std::nullopt;
	}

	bool NamedByStage(std::string_view a_id)
	{
		const auto& state = Anchors();
		if (!state.frozen)
			return false;
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			if (!state.declaredEpochs[d])
				continue;
			for (std::uint32_t k = 0; k < declaredInfo[d].stepCount; ++k)
				if (Table::Has(declaredInfo[d].steps[k].clients, a_id))
					return true;
		}
		return false;
	}

	bool StageStepsAt(Anchor a_anchor)
	{
		const std::uint32_t index = IndexOf(a_anchor);
		if (index >= kAnchors || !RenderGraph::Engine::Native::GraphMode())
			return false;
		for (std::uint32_t d = 0; d < kDeclared; ++d)
			if (StepAnchorsOf(d) >> index & 1u)
				return true;
		return false;
	}

	bool CanTap(std::string_view a_id)
	{
		auto& state = Anchors();
		if (!state.host || !IsRenderOwner())
			return false;
		auto lock = Guard();
		auto* client = FindClient(state, a_id);
		if (!client)
			return false;
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			if (!state.declaredEpochs[d] || !ClientHolds(*client, d) || !WindowOpen(state, d))
				continue;
			const auto& built = state.frameBuilt[d];
			if (built && std::ranges::find(built->clients, client->desc) != built->clients.end())
				return true;
		}
		return false;
	}

	bool TapMayLag(std::string_view a_id)
	{
		auto& state = Anchors();
		if (!state.host || !IsRenderOwner())
			return true;
		auto lock = Guard();
		auto* client = FindClient(state, a_id);
		if (!client)
			return false;
		if (WaitsToJoin(state, *client))
			return true;
		const auto& frame = Driver::Current();
		bool held = false;
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			if (!state.declaredEpochs[d] || !ClientHolds(*client, d))
				continue;
			held = true;
			if (state.host->StagedGeneration(EpochOfDeclared(d)) != 0 || !WindowOpen(state, d) || !state.frameRecording[d])
				return true;
			// A build asked for by the stage's key (absent passes) and not in place yet.
			const auto& use = state.keyUse[d];
			const auto builtKey = state.declaredKeys[d].load(std::memory_order_acquire);
			if (use.requestedSerial && (!builtKey || builtKey->serial < use.requestedSerial))
				return true;
			// A lagging schedule: the frame's own description is being recorded.
			if (!(frame.Segment(static_cast<SegmentId>(d)) == state.frameWanted[d]))
				return true;
			const auto& built = state.frameBuilt[d];
			if (built && std::ranges::find(built->clients, client->desc) != built->clients.end())
				return true;
		}
		// A client no stage holds is not this rule's.
		return !held;
	}

	bool StageInputs(std::string_view a_id, const std::function<void()>& a_write)
	{
		auto& state = Anchors();
		if (!state.host || !IsRenderOwner())
			return false;
		auto lock = Guard();
		auto* client = FindClient(state, a_id);
		if (!client)
			return false;
		const auto& frame = Driver::Current();
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			if (!state.declaredEpochs[d] || !ClientHolds(*client, d) || !WindowOpen(state, d))
				continue;
			const auto& built = state.frameBuilt[d];
			if (!built || std::ranges::find(built->clients, client->desc) == built->clients.end())
				continue;
			// The client must be on in the frame's schedule: its passes do work only then.
			const auto& clients = SegmentClients(*built, d);
			const auto i = static_cast<std::size_t>(std::ranges::find(clients, client->desc) - clients.begin());
			if (i >= RenderGraph::Frame::kSegmentClients || !(frame.Segment(static_cast<SegmentId>(d)).clientsOn >> i & 1u))
				continue;
			struct StagingScope
			{
				org::GraphHost& host;
				~StagingScope() { host.EndHeldRun(); }
			} scope{ *state.host };
			state.host->UseRun(state.stagingRun[d]);
			state.stagedFrame[d] = frame.frame;
			UpdateStagesOpen(state);
			a_write();
			return true;
		}
		return false;
	}

	std::int32_t StagePlace(std::span<const std::string_view> a_ids)
	{
		auto& state = Anchors();
		if (!state.host || !IsRenderOwner())
			return -1;
		auto lock = Guard();
		const auto named = [&](const Step& a_step) -> std::int32_t {
			for (std::size_t i = 0; i < a_ids.size(); ++i)
				if (Table::Has(a_step.clients, a_ids[i]))
					return static_cast<std::int32_t>(i);
			return -1;
		};
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			if (!WindowOpen(state, d))
				continue;
			const auto& info = declaredInfo[d];
			// The stage's last tapped step this frame (-1: none yet).
			std::int32_t last = -1;
			if (state.stagedFrame[d] == Driver::Current().frame)
				for (std::uint32_t k = 0; k < info.stepCount; ++k)
					if (state.stepTaps[d][k])
						last = static_cast<std::int32_t>(k);
			std::int32_t first = -1, chosen = -1;
			for (std::uint32_t k = 0; k < info.stepCount; ++k) {
				const auto id = named(info.steps[k]);
				if (id < 0)
					continue;
				if (first < 0)
					first = id;
				if (static_cast<std::int32_t>(k) <= last + 1)
					chosen = id;
			}
			if (first >= 0)
				return chosen >= 0 ? chosen : first;
		}
		return -1;
	}

	RunResult TapPoint(RenderGraph::Frame::SegmentId a_segment)
	{
		const RunDepthScope depth;
		auto& state = Anchors();
		if (!state.host)
			return { RunStatus::Idle };
		auto lock = Guard();
		if (!IsRenderOwner() || !RenderGraphRuntime::Get().IsActive())
			return { RunStatus::Refused };
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			if (!state.declaredEpochs[d] || d != static_cast<std::uint32_t>(a_segment))
				continue;
			for (std::uint32_t k = 0; k < declaredInfo[d].stepCount; ++k)
				if (declaredInfo[d].steps[k].point) {
					// An absent segment (no schedule this frame) ends here too, without drawing, so the later windows open.
					const auto& fields = Driver::Current().Segment(static_cast<SegmentId>(d));
					if (fields.declared && !fields.graph && state.endedFrame[d] != Driver::Current().frame)
						return TapDeclared(state, Point{}, d, k, {});
					return WindowOpen(state, d) ? TapDeclared(state, Point{}, d, k, {}) : RunResult{ RunStatus::Idle };
				}
		}
		return { RunStatus::Idle };
	}

	bool DescribingKey()
	{
		return Anchors().describingKey;
	}

	std::optional<bool> DescribedBuildHasRow(std::uint32_t a_row)
	{
		const auto& state = Anchors();
		if (state.describingKey || !state.describingClient || !state.describingAgainst || state.describingAgainst->all || a_row >= 256)
			return std::nullopt;
		const auto [present, rows] = KeyPart(state.describingAgainst, *state.describingClient);
		return present && (rows[a_row / 64] >> (a_row % 64) & 1u) != 0;
	}

	bool StagesHoldValues()
	{
		const auto& state = Anchors();
		return state.stagesOpen.load(std::memory_order_acquire) && !state.submittingStage.load(std::memory_order_acquire);
	}

	std::uint32_t StagesHoldingValues()
	{
		const auto& state = Anchors();
		return state.submittingStage.load(std::memory_order_acquire) ? 0u : state.stagesOpenMask.load(std::memory_order_acquire);
	}

	void SetWindowsClosedHook(void (*a_hook)())
	{
		windowsClosedHook.store(a_hook, std::memory_order_release);
	}

	std::vector<std::uint32_t> SegmentEpochs()
	{
		std::vector<std::uint32_t> epochs;
		const auto& state = Anchors();
		for (std::uint32_t d = 0; d < kDeclared; ++d)
			if (state.declaredEpochs[d])
				epochs.push_back(EpochOfDeclared(d));
		return epochs;
	}

	std::vector<std::pair<std::uint8_t, std::uint32_t>> SegmentEpochsById()
	{
		std::vector<std::pair<std::uint8_t, std::uint32_t>> epochs;
		const auto& state = Anchors();
		for (std::uint32_t d = 0; d < kDeclared; ++d)
			if (state.declaredEpochs[d])
				epochs.push_back({ static_cast<std::uint8_t>(d), EpochOfDeclared(d) });
		return epochs;
	}

	void BeginFrame(RenderGraph::Frame::Description& a_frame)
	{
		auto& state = Anchors();
		if (!state.host || !IsRenderOwner())
			return;
		auto lock = Guard();
		if (!RenderGraphRuntime::Get().IsActive())
			return;
		using Kind = RenderGraph::Frame::Kind;
		// Drop values the last frame staged but never committed. The driver counts those.
		for (std::uint32_t d = 0; d < kDeclared; ++d)
			if (state.stagedFrame[d] && state.stagedFrame[d] != a_frame.frame) {
				if (!Driver::LoadStartedThisFrame())
					logger::warn("[ORG] frame segments: the {} staged values that its commit never took by the frame's end; they are dropped",
						declaredInfo[d].names.label);
				DropStaged(state, d);
			}
		// Each segment takes its slot's schedule here and keeps it to the frame's end.
		// A world frame draws every stage. Other frame kinds draw the UI stage.
		state.inBegin = true;
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			state.frameRecording[d].reset();
			state.frameBuilt[d].reset();
			state.frameWanted[d] = {};
			if (state.declaredEpochs[d]) {
				const bool inRecipe = RenderGraph::Frame::Recipes::KindHas(a_frame.kind, a_frame.capture, static_cast<SegmentId>(d));
				a_frame.segments[d] = Schedule(state, d, inRecipe, a_frame);
				// A present stage's frame inputs start empty for this frame.
				if (state.frameRecording[d])
					OpenInputs(state, d, a_frame.slot);
			}
		}
		// Each stage's key for its next build. The build is made off the render thread and switches in like any other.
		const bool kindSwitch = state.keyKind != Kind::Count && state.keyKind != a_frame.kind;
		state.keyKind = a_frame.kind;
		for (std::uint32_t d = 0; d < kDeclared; ++d)
			if (state.declaredEpochs[d])
				UpdateKey(state, d, RenderGraph::Frame::Recipes::KindHas(a_frame.kind, a_frame.capture, static_cast<SegmentId>(d)), a_frame, kindSwitch);
		state.inBegin = false;
	}

	/** @brief A revision's member stages, each with one recording request per frame slot that has a schedule. */
	struct RevisionRecordings
	{
		struct Slot
		{
			RenderGraph::Frame::SegmentFields fields;  // the slot's schedule the recording was asked for
			std::shared_ptr<SegmentRecording> entry;
		};
		struct Member
		{
			std::uint32_t declared = kDeclared;
			std::array<Slot, RenderGraphRuntime::kGameFramesInFlight> slots{};
			// The staged build the slots are recorded on (0: the attached one), with what describes it.
			std::uint64_t staged = 0;
			std::uint64_t stagedSince = 0;  // the frame the revision first saw that build
			RenderGraph::Frame::SegmentFields stagedFields;
			std::shared_ptr<const Snapshot> stagedBuilt;
			std::shared_ptr<const BuildKey> stagedKey;
		};
		RecordVersions versions;
		std::vector<Member> members;
		std::vector<StageBuildNeed> needs;
	};

	std::shared_ptr<RevisionRecordings> MakeRevisionRecordings(std::span<const std::uint32_t> a_epochs, std::shared_ptr<const org::IResourceVersions> a_versions,
		std::uint64_t a_serial, std::vector<std::uint32_t>& a_others)
	{
		auto revision = std::make_shared<RevisionRecordings>();
		revision->versions = { std::move(a_versions), a_serial };
		AddRevisionMembers(*revision, a_epochs, a_others);
		return revision;
	}

	std::uint64_t ClientSegments(std::string_view a_id)
	{
		auto lock = Guard();
		auto& state = Anchors();
		auto* client = FindClient(state, a_id);
		if (!client)
			return 0;
		std::uint64_t segments = 0;
		for (std::uint32_t d = 0; d < kDeclared; ++d)
			if (state.declaredEpochs[d] && ClientHolds(*client, d))
				segments |= std::uint64_t{ 1 } << d;
		return segments;
	}

	std::uint64_t ClientsSerial()
	{
		auto lock = Guard();
		return Anchors().serial;
	}

	void RequireBuilds(RevisionRecordings& a_revision, std::vector<StageBuildNeed> a_needs)
	{
		a_revision.needs = std::move(a_needs);
	}

	bool StagedStages()
	{
		auto& state = Anchors();
		if (!state.host)
			return false;
		for (std::uint32_t d = 0; d < kDeclared; ++d)
			if (state.declaredEpochs[d] && state.host->StagedGeneration(EpochOfDeclared(d)) && state.stagedSnapshots[d].load(std::memory_order_acquire))
				return true;
		return false;
	}

	std::uint32_t AddRevisionMembers(RevisionRecordings& a_revision, std::span<const std::uint32_t> a_epochs, std::vector<std::uint32_t>& a_others)
	{
		std::uint32_t added = 0;
		for (const auto epoch : a_epochs) {
			const auto declared = DeclaredOfEpoch(epoch);
			if (declared == kDeclared) {
				if (std::ranges::find(a_others, epoch) == a_others.end())
					a_others.push_back(epoch);
				continue;
			}
			if (std::ranges::any_of(a_revision.members, [&](const RevisionRecordings::Member& a_member) { return a_member.declared == declared; }))
				continue;
			a_revision.members.push_back({ declared });
			++added;
		}
		return added;
	}

	RevisionProgress AdvanceRevisionRecordings(RevisionRecordings& a_revision, std::uint64_t a_frame)
	{
		RevisionProgress progress;
		auto& state = Anchors();
		progress.ready = true;
		if (!state.host)
			return progress;
		auto lock = Guard();
		auto& host = *state.host;
		const auto note = [](std::string& a_line, std::string_view a_what) {
			a_line += fmt::format("{}{}", a_line.empty() ? "" : ", ", a_what);
		};
		const auto missing = [&](std::uint32_t a_declared, std::uint32_t a_slot) {
			if (progress.ready)
				progress.missing = a_declared;
			progress.ready = false;
			note(progress.waiting, fmt::format("{} slot {}", declaredInfo[a_declared].names.label, a_slot));
		};
		// Every staged stage build joins the revision. Its attached graph takes no new recording until the build is attached.
		for (std::uint32_t d = 0; d < kDeclared; ++d)
			if (state.declaredEpochs[d] && host.StagedGeneration(EpochOfDeclared(d)) && state.stagedSnapshots[d].load(std::memory_order_acquire) &&
				std::ranges::none_of(a_revision.members, [&](const RevisionRecordings::Member& a_member) { return a_member.declared == d; }))
				a_revision.members.push_back({ d });
		// Stages a needed build lands in are members, whether it is staged yet or not.
		for (const auto& need : a_revision.needs)
			for (std::uint32_t d = 0; d < kDeclared; ++d)
				if ((need.segments >> d & 1u) && state.declaredEpochs[d] &&
					std::ranges::none_of(a_revision.members, [&](const RevisionRecordings::Member& a_member) { return a_member.declared == d; }))
					a_revision.members.push_back({ d });
		// Requests made here are Begin's, not mid-frame re-records.
		const bool inBegin = std::exchange(state.inBegin, true);
		for (auto& member : a_revision.members) {
			const auto declared = member.declared;
			const auto& label = declaredInfo[declared].names.label;
			auto& recordings = state.declaredRecordings[declared];
			const std::uint64_t staged = host.StagedGeneration(EpochOfDeclared(declared));
			auto stagedBuilt = staged ? state.stagedSnapshots[declared].load(std::memory_order_acquire) : nullptr;
			const std::uint64_t target = stagedBuilt ? staged : 0;
			// A new staged build replaces what was recorded for the member. It is a new input, not a drop.
			if (member.staged != target) {
				member.slots = {};
				member.staged = target;
				member.stagedSince = target ? a_frame : 0;
				member.stagedBuilt = std::move(stagedBuilt);
				member.stagedKey = target ? state.stagedKeys[declared].load(std::memory_order_acquire) : nullptr;
				member.stagedFields = {};
				if (target)
					++progress.newBuilds;
			}
			if (member.staged) {
				++progress.builds;
				member.stagedFields = DescribeDeclaredFrom(state, declared, true, member.stagedBuilt, state.stagedPasses[declared].load(std::memory_order_acquire),
					member.staged, member.stagedKey.get(), true);
			}
			const auto usable = [&](const std::shared_ptr<const org::GraphHost::EpochRecording>& a_recording) {
				return a_recording && (member.staged ? a_recording->buildGeneration == member.staged : host.EpochRecordingCurrent(*a_recording));
			};
			for (std::uint32_t s = 0; s < RenderGraphRuntime::kGameFramesInFlight; ++s) {
				auto& slot = member.slots[s];
				// On a staged build every slot is recorded, unless the build leaves the stage absent. On the attached one, a slot
				// without a current schedule binds nothing yet; after adoption Schedule records it against the adopted versions.
				const RenderGraph::Frame::SegmentFields* wanted = nullptr;
				if (member.staged) {
					if (member.stagedFields.graph)
						wanted = &member.stagedFields;
				} else if (const auto& scheduled = state.schedule[declared][s]; scheduled.recording && host.EpochRecordingCurrent(*scheduled.recording)) {
					wanted = &scheduled.fields;
				}
				if (!wanted) {
					slot = {};
					continue;
				}
				if (slot.entry && slot.fields == *wanted) {
					std::shared_ptr<const org::GraphHost::EpochRecording> recorded;
					bool done = false;
					bool dropped = false;
					std::string error;
					{
						const std::lock_guard entryLock(recordings.mutex);
						done = slot.entry->done;
						recorded = slot.entry->recording;
						dropped = slot.entry->dropped;
						error = slot.entry->error;
					}
					if (!done) {
						missing(declared, s);
						continue;
					}
					if (usable(recorded)) {
						++progress.recordings;
						continue;
					}
					if (!recorded && !dropped)
						FailRecording(declared, slot.fields, s, error.empty() ? std::string("no recording") : error);
					// Dropped by a build, or recorded on a build replaced since: asked for again.
					++progress.askedAgain;
					note(progress.dropReasons, fmt::format("{} slot {}{}: {}", label, s, member.staged ? " (staged build)" : "",
												   recorded ? std::string("a build attached since it was recorded") : error));
				}
				slot.fields = *wanted;
				slot.entry = RequestDeclared(state, declared, slot.fields, s, a_frame, member.staged ? " (frame revision, staged build)" : " (frame revision)",
					nullptr, a_revision.versions);
				bool ready = false;
				{
					const std::lock_guard entryLock(recordings.mutex);
					ready = slot.entry->done && usable(slot.entry->recording);
				}
				if (ready)
					++progress.recordings;
				else
					missing(declared, s);
			}
		}
		state.inBegin = inBegin;
		// A needed build must be the one the member installs: staged, or already attached.
		for (const auto& need : a_revision.needs)
			for (const auto& member : a_revision.members) {
				if (!(need.segments >> member.declared & 1u))
					continue;
				const auto built = member.staged ? member.stagedBuilt : state.declaredSnapshots[member.declared].load(std::memory_order_acquire);
				if (built && built->serial >= need.serial)
					continue;
				if (progress.ready)
					progress.missing = member.declared;
				progress.ready = false;
				note(progress.waiting, fmt::format("{} build holding {}", declaredInfo[member.declared].names.label, need.what));
			}
		return progress;
	}

	std::vector<std::uint32_t> RevisionStagedEpochs(const RevisionRecordings& a_revision)
	{
		std::vector<std::uint32_t> epochs;
		for (const auto& member : a_revision.members)
			if (member.staged)
				epochs.push_back(EpochOfDeclared(member.declared));
		return epochs;
	}

	std::vector<std::string> RevisionMembers(const RevisionRecordings& a_revision)
	{
		std::vector<std::string> labels;
		for (const auto& member : a_revision.members)
			labels.emplace_back(member.staged ? fmt::format("{} (build {})", declaredInfo[member.declared].names.label, member.staged) :
			                                    std::string(declaredInfo[member.declared].names.label));
		return labels;
	}

	void InstallRevisionSchedules(const RevisionRecordings& a_revision, std::uint64_t a_frame)
	{
		auto& state = Anchors();
		if (!state.host)
			return;
		auto lock = Guard();
		auto& host = *state.host;
		const auto fail = [](std::uint32_t a_declared, std::uint32_t a_slot, std::string_view a_what) {
			const auto message = fmt::format("Community Shaders: the {}'s frame slot {} {} between its frame revision's last check and its adoption",
				declaredInfo[a_declared].names.label, a_slot, a_what);
			logger::critical("[ORG] frame revision: {}", message);
			stl::report_and_fail(message);
		};
		for (const auto& member : a_revision.members) {
			const auto declared = member.declared;
			auto& recordings = state.declaredRecordings[declared];
			const auto segment = static_cast<SegmentId>(declared);
			if (member.staged) {
				// The staged build was attached with the revision. Every slot takes its description, or none when the stage is absent on it.
				if (host.BuildGeneration(EpochOfDeclared(declared)) != member.staged)
					fail(declared, 0, fmt::format("lost its staged build {}", member.staged));
				std::array<State::Scheduled, RenderGraphRuntime::kGameFramesInFlight> next{};
				for (std::uint32_t s = 0; s < RenderGraphRuntime::kGameFramesInFlight && member.stagedFields.graph; ++s) {
					const auto& slot = member.slots[s];
					std::shared_ptr<const org::GraphHost::EpochRecording> recorded;
					if (slot.entry) {
						const std::lock_guard entryLock(recordings.mutex);
						recorded = slot.entry->recording;
					}
					if (!recorded || !host.EpochRecordingCurrent(*recorded) || !(slot.fields == member.stagedFields))
						fail(declared, s, "lost its staged build's recording");
					next[s] = { member.stagedFields, std::move(recorded), member.stagedBuilt };
				}
				// For a build asked for by a new key, how many frames its passes took to arrive.
				std::string keyNote;
				if (auto& use = state.keyUse[declared]; member.stagedKey && !member.stagedKey->all && member.stagedKey->serial >= use.requestedSerial &&
														use.switchedSerial < use.requestedSerial) {
					keyNote = fmt::format("; its key was asked for at frame {}, {} frames before", use.requestedFrame, a_frame - use.requestedFrame);
					use.switchedSerial = use.requestedSerial;
				}
				const auto& before = state.schedule[declared][0];
				const auto what = fmt::format("build {} attached with its frame revision and its recordings for every frame slot{}{}", member.staged,
					before.recording ? fmt::format(" ({})", RenderGraph::Frame::DiffFields(before.fields, member.stagedFields)) : std::string(), keyNote);
				state.schedule[declared] = next;
				// The replaced build's recordings are never used again. Each keeps that build's graph and alias pools alive.
				{
					const std::lock_guard entryLock(recordings.mutex);
					std::erase_if(recordings.entries,
						[&](const auto& a_kept) { return a_kept->done && a_kept->recording && a_kept->recording->buildGeneration != member.staged; });
				}
				Driver::NoteSwitch(segment, a_frame, 0, a_frame - member.stagedSince, what);
				Driver::NoteBuildSwitch(segment);
				state.stagedSnapshots[declared].store(nullptr, std::memory_order_release);
				state.stagedPasses[declared].store(nullptr, std::memory_order_release);
				state.stagedKeys[declared].store(nullptr, std::memory_order_release);
				continue;
			}
			for (std::uint32_t s = 0; s < RenderGraphRuntime::kGameFramesInFlight; ++s) {
				const auto& slot = member.slots[s];
				if (!slot.entry)
					continue;
				std::shared_ptr<const org::GraphHost::EpochRecording> recorded;
				{
					const std::lock_guard entryLock(recordings.mutex);
					recorded = slot.entry->recording;
				}
				auto& scheduled = state.schedule[declared][s];
				// The revision was found ready in this Begin, so nothing may have moved since.
				if (!recorded || !scheduled.recording || !(scheduled.fields == slot.fields))
					fail(declared, s, "changed");
				// The description and its build stay. Only what the slot binds moves to the adopted versions.
				scheduled.recording = std::move(recorded);
			}
		}
	}

	RunResult Run(const Point& a_point)
	{
		const RunDepthScope depth;
		auto& state = Anchors();
		const std::uint32_t index = IndexOf(a_point.anchor);
		if (index >= kAnchors || !state.reserved[index] || !state.host)
			return { RunStatus::Idle };
		// Ownership is checked under the mutex a claim takes: it cannot move in the middle of this run.
		auto lock = Guard();
		if (!IsRenderOwner())
			return { RunStatus::Refused };
		auto& runtime = RenderGraphRuntime::Get();
		if (!runtime.IsActive())
			return { RunStatus::Refused };
		ServiceRetiredTextures();
		// Drop failed epochs' clients here, before the clients are read, not in the middle of a run.
		runtime.PollEpochFailures();
		if (!runtime.IsActive())
			return { RunStatus::Failed };
		Service(state);
		// The run happens when a client is active or has a one-shot pass pending.
		// A client registered after the build waits for the next build.
		std::vector<RunClient> clients;
		bool work = false;
		for (const auto& client : state.clients) {
			if (!InGraphs(*client.desc) || !HasPasses(*client.desc, a_point.anchor))
				continue;
			const bool active = !client.desc->active || client.desc->active();
			work = work || active || HasPendingOnce(client, a_point.anchor);
			clients.push_back({ client.desc, client.serial, active });
		}
		// Declared segments: an anchor a segment holds wholly is one of its taps whenever it fires.
		if (const auto declared = DeclaredOfAnchor(state, index); declared < kDeclared) {
			const auto step = TapStepAt(state, declared, a_point.anchor);
			if (step >= kMaxSteps) {
				Driver::NoteTapError(Driver::TapError::Repeated, static_cast<SegmentId>(declared), fmt::format("the {} tap fired after its steps took their last tap", kInfo[index].name));
				return { RunStatus::Idle };
			}
			return TapDeclared(state, a_point, declared, step, clients);
		}
		// At a shared anchor, clients a segment names commit into it while its window is open. The others are a mismatch.
		// A segment-only client draws nothing outside every open window.
		const auto hasWork = [&](const RunClient& a_client) {
			if (a_client.active)
				return true;
			auto* client = FindClient(state, a_client.desc->id);
			if (!client || !HasPendingOnce(*client, a_point.anchor))
				return false;
			for (std::uint32_t d = 0; d < kDeclared; ++d)
				if (state.declaredEpochs[d] && ClientHolds(*client, d))
					return false;
			return true;
		};
		const auto dropSegmentOnly = [&](std::vector<RunClient>& a_clients, std::string_view a_why) {
			for (auto& client : a_clients)
				if (client.active && client.desc->segmentOnly) {
					Driver::NoteTapError(Driver::TapError::NotDeclared, SegmentId::Count, fmt::format("{} had work at the {} tap {}", client.desc->id, kInfo[index].name, a_why));
					client.active = false;
				}
		};
		if (work) {
			if (OpenStepAt(state, a_point.anchor).first < kDeclared) {
				std::vector<RunClient> others = clients;
				RunResult tapped{ RunStatus::Idle };
				// Two segments can be open at once. A stage whose commit point has not come yet, and the post stage after it.
				for (std::uint32_t declared = 0; declared < kDeclared; ++declared) {
					const auto first = StepAt(declared, a_point.anchor);
					if (first == kMaxSteps || !WindowOpen(state, declared))
						continue;
					const auto& built = state.frameBuilt[declared];
					// Each step at this anchor takes its own clients. The post stage has three ImageSpace steps.
					for (std::uint32_t k = first; k < declaredInfo[declared].stepCount; ++k) {
						const auto& stepInfo = declaredInfo[declared].steps[k];
						if (stepInfo.anchor != a_point.anchor || !StepTakesTaps(state, declared, k))
							continue;
						std::vector<RunClient> stepMembers;
						for (auto it = others.begin(); it != others.end();) {
							const bool member = built && StepHolds(stepInfo, *it->desc) &&
							                    std::ranges::any_of(built->clients, [&](const auto& a_built) { return SameClient(a_built, it->desc); });
							if (member) {
								stepMembers.push_back(*it);
								it = others.erase(it);
							} else {
								++it;
							}
						}
						// A client that commits once per segment run, active at a second run of its step: a mismatch. That run draws nothing.
						const bool again = state.stagedFrame[declared] == Driver::Current().frame && state.stepTaps[declared][k] > 0 &&
						                   std::ranges::any_of(stepMembers, [](const RunClient& a_client) {
											   return a_client.active && !a_client.desc->repeats && a_client.desc->maxInstances <= 1;
										   });
						if (again) {
							Driver::NoteTapError(Driver::TapError::Repeated, static_cast<RenderGraph::Frame::SegmentId>(declared), fmt::format("the {} step ran again in one segment run", kInfo[index].name));
							break;
						}
						if (std::ranges::any_of(stepMembers, [](const RunClient& a_client) { return a_client.active; })) {
							const auto result = TapDeclared(state, a_point, declared, k, stepMembers);
							if (tapped.status == RunStatus::Idle || !result)
								tapped = result;
						}
					}
				}
				dropSegmentOnly(others, "outside its segment's built graph");
				const bool otherWork = std::ranges::any_of(others, hasWork);
				if (!otherWork)
					return tapped;
				// The rest have no step that takes them now.
				clients = std::move(others);
			} else {
				dropSegmentOnly(clients, "outside its segment's window");
				work = std::ranges::any_of(clients, hasWork);
			}
		}
		if (!work)
			return { RunStatus::Idle };
		// No anchor runs an epoch of its own, so work here is a mismatch and draws nothing.
		std::string ids;
		for (const auto& client : clients)
			if (client.active)
				ids += (ids.empty() ? "" : ",") + client.desc->id;
		Driver::NoteTapError(Driver::TapError::OwnEpoch, SegmentId::Count,
			fmt::format("the {} anchor had work outside every stage's step (clients [{}]); no stage of the frame has a place for it", kInfo[index].name, ids));
		return { RunStatus::Idle };
	}


	namespace
	{
		/** @brief Whether this frame's schedule of the segment holds this client registration and has it on. Under the lock. */
		bool ScheduleHolds(const State& a_state, std::uint32_t a_declared, const std::shared_ptr<const ClientDesc>& a_client)
		{
			const auto& fields = Driver::Current().Segment(static_cast<SegmentId>(a_declared));
			const auto& built = a_state.frameBuilt[a_declared];
			if (!fields.declared || !fields.graph || !built)
				return false;
			const auto& clients = SegmentClients(*built, a_declared);
			const auto found = std::ranges::find(clients, a_client);
			const auto i = static_cast<std::size_t>(found - clients.begin());
			return found != clients.end() && i < RenderGraph::Frame::kSegmentClients && (fields.clientsOn >> i & 1u);
		}
	}

	bool Scheduled(std::string_view a_id, Anchor a_anchor)
	{
		auto lock = Guard();
		auto& state = Anchors();
		const std::uint32_t index = IndexOf(a_anchor);
		if (index >= kAnchors || !state.host || !IsRenderOwner() || !RenderGraphRuntime::Get().IsActive())
			return false;
		const auto* client = FindClient(state, a_id);
		if (!client)
			return false;
		// The stage that holds the anchor wholly, or at a shared anchor the open stage that takes its taps and holds the client.
		// With no such stage nothing draws the client's work.
		if (const auto declared = DeclaredOfAnchor(state, index); declared < kDeclared)
			return ScheduleHolds(state, declared, client->desc);
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			if (!WindowOpen(state, d))
				continue;
			for (std::uint32_t k = 0; k < declaredInfo[d].stepCount; ++k)
				if (declaredInfo[d].steps[k].anchor == a_anchor && StepTakesTaps(state, d, k) && StepHolds(declaredInfo[d].steps[k], *client->desc))
					return ScheduleHolds(state, d, client->desc);
		}
		return false;
	}

	bool ScheduledThisFrame(std::string_view a_id, Anchor a_anchor)
	{
		auto lock = Guard();
		auto& state = Anchors();
		if (IndexOf(a_anchor) >= kAnchors || !state.host || !IsRenderOwner() || !RenderGraphRuntime::Get().IsActive())
			return false;
		const auto* client = FindClient(state, a_id);
		if (!client)
			return false;
		const auto& frame = Driver::Current();
		bool any = false;
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			if (!state.declaredEpochs[d] || !frame.Segment(static_cast<SegmentId>(d)).declared)
				continue;
			bool steps = false;
			for (std::uint32_t k = 0; k < declaredInfo[d].stepCount; ++k)
				steps = steps || (declaredInfo[d].steps[k].anchor == a_anchor && StepHolds(declaredInfo[d].steps[k], *client->desc));
			if (!steps)
				continue;
			// A stage of this frame that would take the tap without this registration: the frame has no place for the work there.
			if (!ScheduleHolds(state, d, client->desc))
				return false;
			any = true;
		}
		return any;
	}

	bool StagePassed(std::string_view a_id, Anchor a_anchor)
	{
		auto lock = Guard();
		auto& state = Anchors();
		if (IndexOf(a_anchor) >= kAnchors || !state.host || !IsRenderOwner())
			return false;
		const auto* client = FindClient(state, a_id);
		if (!client)
			return false;
		const auto& frame = Driver::Current();
		bool any = false;
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			if (!state.declaredEpochs[d] || !frame.Segment(static_cast<SegmentId>(d)).declared)
				continue;
			bool steps = false;
			for (std::uint32_t k = 0; k < declaredInfo[d].stepCount; ++k)
				steps = steps || (declaredInfo[d].steps[k].anchor == a_anchor && StepHolds(declaredInfo[d].steps[k], *client->desc));
			if (!steps)
				continue;
			// This stage would take the tap and has not ended yet, so the tap is not past it.
			if (state.endedFrame[d] != frame.frame)
				return false;
			any = true;
		}
		return any;
	}

	bool InStage(std::string_view a_id, Anchor a_anchor)
	{
		auto lock = Guard();
		auto& state = Anchors();
		const std::uint32_t index = IndexOf(a_anchor);
		if (!state.host || index >= kAnchors)
			return false;
		const auto declared = DeclaredOfAnchor(state, index);
		if (declared >= kDeclared)
			return false;
		const auto* client = FindClient(state, a_id);
		if (!client)
			return false;
		const auto holds = [&](const std::shared_ptr<const Snapshot>& a_built) {
			return a_built && std::ranges::any_of(a_built->clients, [&](const auto& a_held) { return SameClient(a_held, client->desc); });
		};
		const auto& last = state.frameBuilt[declared];
		return last ? holds(last) : holds(state.declaredSnapshots[declared].load(std::memory_order_acquire));
	}

	StageHold HeldNow(std::string_view a_id, Anchor a_anchor)
	{
		auto lock = Guard();
		auto& state = Anchors();
		StageHold hold;
		if (!state.host || !IsRenderOwner())
			return hold;
		// The open stage's step that takes taps now and holds the client.
		const auto* registered = FindClient(state, a_id);
		std::uint32_t declared = kDeclared, step = kMaxSteps;
		for (std::uint32_t d = 0; d < kDeclared && declared == kDeclared; ++d) {
			if (!WindowOpen(state, d))
				continue;
			for (std::uint32_t k = 0; k < declaredInfo[d].stepCount; ++k)
				if (declaredInfo[d].steps[k].anchor == a_anchor && StepTakesTaps(state, d, k) && registered && StepHolds(declaredInfo[d].steps[k], *registered->desc)) {
					declared = d;
					step = k;
					break;
				}
		}
		if (declared >= kDeclared) {
			// A declared stage with no schedule yet that holds the client owns the work. It lags until the first schedule.
			const auto& frame = Driver::Current();
			for (std::uint32_t d = 0; d < kDeclared && registered; ++d) {
				const auto& fields = frame.Segment(static_cast<SegmentId>(d));
				if (!state.declaredEpochs[d] || !fields.declared || fields.graph || state.endedFrame[d] == frame.frame)
					continue;
				for (std::uint32_t k = 0; k < declaredInfo[d].stepCount; ++k)
					if (declaredInfo[d].steps[k].anchor == a_anchor && StepHolds(declaredInfo[d].steps[k], *registered->desc)) {
						hold.held = true;
						return hold;
					}
			}
			return hold;
		}
		hold.held = true;
		const auto& fields = Driver::Current().Segment(static_cast<SegmentId>(declared));
		const auto& built = state.frameBuilt[declared];
		if (!fields.declared || !fields.graph || !built)
			return hold;
		const auto& clients = SegmentClients(*built, declared);
		for (std::size_t i = 0; i < clients.size() && i < RenderGraph::Frame::kSegmentClients; ++i)
			if (clients[i]->id == a_id && StepHolds(declaredInfo[declared].steps[step], *clients[i])) {
				hold.scheduled = (fields.clientsOn >> i & 1u) != 0;
				hold.generation = clients[i]->generation;
			}
		return hold;
	}

	bool BuildPending(std::string_view a_caller)
	{
		auto& state = Anchors();
		if (!state.host || !IsRenderOwner()) {
			logger::error("[ORG] frame anchors: {} asked for the graphs off the render owner; its clients join at the first build point", a_caller);
			return false;
		}
		auto lock = Guard();
		auto& runtime = RenderGraphRuntime::Get();
		if (!runtime.IsActive())
			return false;
		const auto waiting = [&] { return std::ranges::count_if(state.clients, [&](const Client& a_client) { return WaitsToJoin(state, a_client); }); };
		if (waiting() == 0)
			return true;
		// Before the first world frame the graphs are built here. After it they build off the render thread and clients join at attach.
		const bool builtNow = runtime.BuildSetupGraphs(a_caller);
		const auto left = waiting();
		if (left == 0)
			logger::info("[ORG] frame anchors: graphs built at {}; every registered client is in its anchors' graphs", a_caller);
		else if (!builtNow && runtime.IsActive())
			logger::info("[ORG] frame anchors: {}: {} clients join their anchors' graphs when the builds asked for are attached", a_caller, left);
		else
			logger::error("[ORG] frame anchors: graphs built at {}, but {} clients are still not in their anchors' graphs", a_caller, left);
		return left == 0;
	}

	namespace
	{
		/** @brief Records one stage's current description for every slot while setup waits, and installs it as each slot's schedule. */
		void PrimeStage(State& state, Segment a_segment, std::string_view a_caller)
		{
			using namespace std::chrono_literals;
			std::uint32_t ui = kDeclared;
			RenderGraph::Frame::SegmentFields wanted{};
			std::shared_ptr<const Snapshot> built;
			std::array<std::shared_ptr<SegmentRecording>, RenderGraphRuntime::kGameFramesInFlight> entries{};
			{
				auto lock = Guard();
				if (!RenderGraphRuntime::Get().IsActive())
					return;
				for (std::uint32_t d = 0; d < kDeclared; ++d)
					if (state.declaredEpochs[d] && declaredInfo[d].segment == a_segment)
						ui = d;
				if (ui == kDeclared || state.host->StagedGeneration(EpochOfDeclared(ui)) != 0)
					return;
				state.inBegin = true;
				wanted = DescribeDeclared(state, ui, true);
				built = state.declaredSnapshots[ui].load(std::memory_order_acquire);
				if (wanted.graph)
					for (std::uint32_t s = 0; s < RenderGraphRuntime::kGameFramesInFlight; ++s)
						entries[s] = RequestDeclared(state, ui, wanted, s, 0, fmt::format(" (the first frame's, at {})", a_caller));
				state.inBegin = false;
			}
			if (!wanted.graph)
				return;
			// Recorded at setup so the first frame draws the stage. The render thread never waits for a recording in a frame.
			auto& recordings = state.declaredRecordings[ui];
			const auto deadline = std::chrono::steady_clock::now() + 10s;
			bool done = false;
			while (!done && std::chrono::steady_clock::now() < deadline) {
				{
					const std::lock_guard lock(recordings.mutex);
					done = std::ranges::all_of(entries, [](const auto& a_entry) { return a_entry && a_entry->done; });
				}
				if (!done)
					std::this_thread::sleep_for(1ms);
			}
			auto lock = Guard();
			std::uint32_t ready = 0;
			for (std::uint32_t s = 0; s < RenderGraphRuntime::kGameFramesInFlight; ++s) {
				std::shared_ptr<const org::GraphHost::EpochRecording> recording;
				{
					const std::lock_guard recordingsLock(recordings.mutex);
					if (entries[s]->done)
						recording = entries[s]->recording;
				}
				if (recording && state.host->EpochRecordingCurrent(*recording)) {
					state.schedule[ui][s] = State::Scheduled{ wanted, std::move(recording), built };
					++ready;
				}
			}
			if (ready == RenderGraphRuntime::kGameFramesInFlight)
				logger::info("[ORG] frame segments: the {}'s first schedule is recorded at {} for every frame slot (build {}); the first frame of its kinds draws it",
					declaredInfo[ui].names.label, a_caller, wanted.graph);
			else
				logger::error("[ORG] frame segments: the {}'s first schedule was recorded for {} of {} frame slots at {}; the others switch in at their first Begin",
					declaredInfo[ui].names.label, ready, RenderGraphRuntime::kGameFramesInFlight, a_caller);
		}
	}

	void PrimeStages(std::string_view a_caller)
	{
		auto& state = Anchors();
		if (!state.host || !IsRenderOwner())
			return;
		// The session's first frames are a Bink movie's, then the main menu's: the UI stage is ready before them.
		PrimeStage(state, RenderGraphRuntime::StageSegment(static_cast<std::uint32_t>(RenderGraph::Frame::SegmentId::Ui)), a_caller);
	}

	void LogRunStats()
	{
		auto& state = Anchors();
		{
			auto lock = Guard();
			std::string inside;
			for (auto& [why, count] : state.insideHeld)
				if (count)
					inside += fmt::format("{}{} {}", inside.empty() ? "" : ", ", why, std::exchange(count, 0));
			logger::info("[ORG] frame segments: GPU work submitted between a stage's first tap and its commit, since the last report: {}", inside.empty() ? "none" : inside);
		}

		std::string runs;
		for (std::uint32_t i = 0; i < kAnchors; ++i)
			if (state.reserved[i] && state.frozen)
				runs += fmt::format("{}{} {}", runs.empty() ? "" : ", ", kInfo[i].name, state.peakRuns[i].load(std::memory_order_relaxed));
		if (!runs.empty())
			logger::info("[ORG] frame anchors, peak runs in one frame: {}", runs);
	}

	Texture EngineTarget(std::uint32_t a_target)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer || a_target >= static_cast<std::uint32_t>(RE::RENDER_TARGET::kTOTAL))
			return {};
		const auto target = renderer->GetRuntimeData().renderTargets[a_target];
		// kFRAMEBUFFER is the swap chain's back buffer: the engine keeps only its RTV, so the texture comes from the view.
		auto* texture = target.texture;
		winrt::com_ptr<ID3D11Texture2D> fromView;
		if (!texture && target.RTV) {
			winrt::com_ptr<ID3D11Resource> resource;
			reinterpret_cast<ID3D11RenderTargetView*>(target.RTV)->GetResource(resource.put());
			if (resource)
				fromView = resource.try_as<ID3D11Texture2D>();
			texture = reinterpret_cast<decltype(texture)>(fromView.get());
		}
		return Import(texture, fmt::format("cs.engine.target.{}", a_target),
			[&](const D3D11_TEXTURE2D_DESC& a_desc) { return DescribeColour(a_desc, target.SRV, target.RTV, target.UAV); });
	}

	Texture EngineDepth(std::uint32_t a_depthStencil)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer || a_depthStencil >= static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL))
			return {};
		const auto target = renderer->GetDepthStencilData().depthStencils[a_depthStencil];
		return Import(target.texture, fmt::format("cs.engine.depth.{}", a_depthStencil),
			[&](const D3D11_TEXTURE2D_DESC& a_desc) { return DescribeDepth(target, a_desc); });
	}

	Texture EngineCubemap(std::uint32_t a_cubemap)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer || a_cubemap >= static_cast<std::uint32_t>(RE::RENDER_TARGETS_CUBEMAP::kTOTAL))
			return {};
		const auto target = renderer->GetRendererData().cubemapRenderTargets[a_cubemap];
		// The faces' RTVs share one format; the graph makes its own views of each face.
		return Import(target.texture, fmt::format("cs.engine.cubemap.{}", a_cubemap),
			[&](const D3D11_TEXTURE2D_DESC& a_desc) { return DescribeColour(a_desc, target.SRV, target.cubeSideRTV[0], nullptr); });
	}

	Texture D3D11Texture(std::string_view a_name, ID3D11Texture2D* a_texture, ID3D11ShaderResourceView* a_srv,
		ID3D11UnorderedAccessView* a_uav, ID3D11RenderTargetView* a_rtv)
	{
		if (a_name.empty())
			return {};
		return Import(a_texture, fmt::format("cs.d3d11.{}", a_name),
			[&](const D3D11_TEXTURE2D_DESC& a_desc) { return DescribeColour(a_desc, a_srv, a_rtv, a_uav); });
	}

	Texture ImmutableTexture(std::string_view a_name, ID3D11Texture2D* a_texture, ID3D11ShaderResourceView* a_srv)
	{
		// Native mode: an exempt texture is one plain graph resource, read untracked. No build is needed.
		if (RenderGraph::Engine::Native::Active() && a_texture && RenderGraphRuntime::Get().IsActive()) {
			D3D11_SHADER_RESOURCE_VIEW_DESC view{};
			if (a_srv)
				a_srv->GetDesc(&view);
			if (auto exempt = RenderGraph::Engine::Native::ImmutableTextureOf(a_texture, a_srv ? view.Format : DXGI_FORMAT_UNKNOWN))
				return Texture(std::move(exempt));
		}
		return D3D11Texture(a_name, a_texture, a_srv, nullptr);
	}

	Texture ImportDepth(const RE::BSGraphics::DepthStencilData& a_target, const std::string& a_id)
	{
		return Import(a_target.texture, a_id, [&](const D3D11_TEXTURE2D_DESC& a_desc) { return DescribeDepth(a_target, a_desc); });
	}

	Texture ImportTexture(std::string_view a_id, ID3D11Texture2D* a_texture, const org::TextureDescription& a_views)
	{
		if (a_id.empty())
			return {};
		return Import(a_texture, std::string(a_id), [&](const D3D11_TEXTURE2D_DESC&) { return a_views; });
	}

	Texture ShareImage(ID3D11Texture2D* a_texture, const DxvkOrgInteropImageInfo& a_image, const org::TextureDescription& a_desc, std::string_view a_name)
	{
		auto lock = Guard();
		auto& state = Anchors();
		auto& runtime = RenderGraphRuntime::Get();
		const auto image = reinterpret_cast<std::uint64_t>(a_image.image);
		if (!image || !runtime.IsActive() || !runtime.Host())
			return {};
		// An engine target ORG owns returns its published slot.
		if (auto owned = RenderGraph::Engine::Targets::FindImage(image))
			return owned;
		auto desc = a_desc;
		DescribeSubresources(desc, a_image);
		const std::string name(a_name);
		// An image ORG already made a graph resource for, such as a versioned twin's slot.
		if (auto made = MadeTexture(a_texture, image, desc, name))
			return made;
		// The slot an importer already made for the image, with the caller's views added when it lacks one.
		if (auto shared = FindImage(state, image)) {
			EnsureViews(shared, desc, name);
			if (auto slot = shared->Slot())
				return Texture(shared->id, std::move(slot));
		}
		auto entry = std::make_shared<EngineImport>();
		entry->texture.copy_from(a_texture);
		entry->id = name;
		entry->image = image;
		entry->info = a_image;
		auto backing = MakeBacking(a_texture, a_image, desc, name);
		if (!backing)
			return {};
		entry->views = desc;
		// The version keeps the entry and its D3D11 texture alive, so the image handle is not reused while the slot lives.
		auto slot = org::VersionedTexture::CreateImage(std::move(backing), entry);
		entry->looseSlot = slot;
		state.loose.push_back(entry);
		return Texture(name, std::move(slot));
	}

	void OnEpochFailed(std::uint32_t a_epoch, const std::string& a_error, bool a_keptGraph)
	{
		auto lock = Guard();
		auto& state = Anchors();
		const Anchor anchor = AnchorOfEpoch(a_epoch);
		if (const auto declared = anchor == Anchor::Count ? DeclaredOfEpoch(a_epoch) : kDeclared; declared != kDeclared) {
			// A segment epoch drops its members' clients from the failed build, as for an anchor.
			const auto failed = state.declaredSnapshots[declared].load(std::memory_order_acquire);
			if (a_keptGraph)
				state.declaredSnapshots[declared].store(state.declaredPrevious[declared].load(std::memory_order_acquire), std::memory_order_release);
			DropStaged(state, declared);
			DropFailedClients(StepAnchorsOf(declared), declaredInfo[declared].names.label, failed, a_error);
			return;
		}
		if (anchor == Anchor::Count)
			return;
		const std::uint32_t index = IndexOf(anchor);
		// The host's own build or preparation failed (a build on the owner thread records its snapshot at once).
		const auto failed = state.builtSnapshots[index].load(std::memory_order_acquire);
		if (a_keptGraph) {
			// The previous graph still runs: what it was built from is the anchor's again.
			const auto previous = state.previousSnapshots[index].load(std::memory_order_acquire);
			state.builtSnapshots[index].store(previous, std::memory_order_release);
			state.built[index].store(previous ? previous->serial : 0, std::memory_order_release);
		}
		DropFailedClients(1u << index, kInfo[index].name, failed, a_error);
	}

	void AddToFrame(std::vector<std::uint32_t>& a_epochs)
	{
		auto& state = Anchors();
		state.frozen = true;
		// Each enabled declared segment with a reserved anchor gets an epoch. Its end tap's anchor is reserved with it,
		// so the hook that ends it always runs.
		for (std::uint32_t d = 0; d < kDeclared; ++d) {
			std::uint32_t members = 0;
			bool any = false;
			for (std::uint32_t i = 0; i < kAnchors; ++i) {
				any = any || (state.reserved[i] && (StepAnchorsOf(d) >> i & 1u));
				// An anchor any stage has a step at has no epoch of its own, in both modes.
				if (state.reserved[i] && (StepAnchorsOf(d) >> i & 1u))
					members |= 1u << i;
			}
			if (!any)
				continue;
			if (const auto end = EndTap(d); end != Anchor::Count) {
				state.reserved[IndexOf(end)] = true;
				members |= 1u << IndexOf(end);
			}
			state.declaredEpochs[d] = true;
			state.declaredMembers |= members;
			const auto segment = static_cast<std::uint32_t>(declaredInfo[d].segment);
			const std::uint32_t epoch = RenderGraphRuntime::EpochOf(declaredInfo[d].segment);
			if (std::ranges::find(a_epochs, epoch) != a_epochs.end())
				continue;
			const auto after = std::ranges::find_if(a_epochs, [&](std::uint32_t a_epoch) { return RankOf(a_epoch) > RankOf(segment); });
			a_epochs.insert(after, epoch);
		}
		for (std::uint32_t i = 0; i < kAnchors; ++i) {
			if (!state.reserved[i] || (state.declaredMembers >> i & 1u))
				continue;
			const std::uint32_t segment = SegmentOf(static_cast<Anchor>(i));
			const std::uint32_t epoch = RenderGraphRuntime::EpochOf(static_cast<Segment>(segment));
			if (std::ranges::find(a_epochs, epoch) != a_epochs.end())
				continue;
			// Before the first epoch that runs after it in the frame.
			const auto after = std::ranges::find_if(a_epochs, [&](std::uint32_t a_epoch) { return RankOf(a_epoch) > RankOf(segment); });
			a_epochs.insert(after, epoch);
		}
	}

	void SetTicketsAhead(org::GraphHost& a_host)
	{
		auto& state = Anchors();
		// An anchor that runs ten times a frame back to back would otherwise wait for a preparation at each run.
		const auto most = static_cast<std::uint32_t>((std::min)(Switches::Number("CS_ORG_TICKETS_AHEAD", 16), std::uint64_t{ 64 }));
		for (std::uint32_t i = 0; i < kAnchors; ++i)
			if (state.reserved[i] && !(state.declaredMembers >> i & 1u))
				a_host.SetEpochTicketsAhead(EpochOfAnchor(static_cast<Anchor>(i)), most);
	}

	void Attach(org::GraphHost& a_host)
	{
		auto& state = Anchors();
		if (std::ranges::none_of(state.reserved, [](bool a_reserved) { return a_reserved; }))
			return;
		state.host = &a_host;
		Publish(state);
		// Made at every graph build from the snapshot published then; with no graph built yet, adding it rebuilds nothing.
		a_host.AddExtension(kExtensionId, [&state]() -> std::unique_ptr<org::RenderGraph::IRenderGraphExtension> {
			return std::make_unique<AnchorExtension>(state.snapshot.load(std::memory_order_acquire));
		});
		std::string reserved;
		for (std::uint32_t i = 0; i < kAnchors; ++i)
			if (state.reserved[i]) {
				const auto d = DeclaredOfAnchor(state, i);
				reserved += fmt::format("{}{} (epoch {}{})", reserved.empty() ? "" : ", ", kInfo[i].name, d < kDeclared ? EpochOfDeclared(d) : EpochOfAnchor(static_cast<Anchor>(i)),
					d < kDeclared ? fmt::format(", in the {}", declaredInfo[d].names.label) : std::string());
			}
		logger::info("[ORG] frame anchors with epochs: {}", reserved);
	}
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: no anchor has an epoch, and every hook does nothing.
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/Frame/RevisionRecordings.h"

namespace FrameAnchors
{
	const char* Name(Anchor) { return "unknown"; }
	bool Reserve(Anchor) { return false; }
	bool HasEpoch(Anchor) { return false; }
	bool RecordsAtExecution(Anchor) { return false; }
	bool Register(ClientDesc) { return false; }
	void Unregister(std::string_view) {}
	void Refresh(std::string_view) {}
	RunResult Run(const Point&) { return {}; }
	bool InRun() { return false; }
	bool StagesHoldValues() { return false; }
	std::uint32_t StagesHoldingValues() { return 0; }
	void SetWindowsClosedHook(void (*)()) {}
	bool DescribingKey() { return false; }
	std::optional<bool> DescribedBuildHasRow(std::uint32_t) { return std::nullopt; }
	bool IsMainThread() { return true; }
	std::uint32_t RunSegment(Anchor) { return 0; }
	void BeginFrame(RenderGraph::Frame::Description&) {}
	std::vector<std::uint32_t> SegmentEpochs() { return {}; }
	std::vector<std::pair<std::uint8_t, std::uint32_t>> SegmentEpochsById() { return {}; }
	void BeforeGpuWork(const char*, std::uint32_t) {}
	void LatchMainThread() {}
	bool IsRenderOwner() { return true; }
	void ClaimRenderOwnership() {}
	bool Scheduled(std::string_view, Anchor) { return false; }
	bool ScheduledThisFrame(std::string_view, Anchor) { return false; }
	bool StagePassed(std::string_view, Anchor) { return false; }
	bool HeldByStage(Anchor) { return false; }
	void DropKeptRecordings(Anchor, std::string_view) {}
	bool NamedByStage(std::string_view) { return false; }
	bool StageStepsAt(Anchor) { return false; }
	bool BuildPending(std::string_view) { return false; }
	bool TapMayLag(std::string_view) { return true; }
	void PrimeStages(std::string_view) {}
	void LogRunStats() {}
	std::uint64_t RenderOwnerChanges() { return 0; }
	Texture::Texture(std::string a_id, std::shared_ptr<org::VersionedTexture> a_slot) :
		id(std::move(a_id)), slot(std::move(a_slot)) {}
	std::shared_ptr<org::GloballyIndexedResource> Texture::Current() const { return nullptr; }
	org::GloballyIndexedResource* Texture::operator->() const { return nullptr; }
	std::uint32_t Texture::Width() const { return 0; }
	std::uint32_t Texture::Height() const { return 0; }
	rhi::Format Texture::Format() const { return rhi::Format{}; }
	const org::IResourceResolver* Texture::DeclaredResolver() const { return nullptr; }
	Texture EngineTarget(std::uint32_t) { return {}; }
	Texture EngineDepth(std::uint32_t) { return {}; }
	Texture EngineCubemap(std::uint32_t) { return {}; }
	Texture D3D11Texture(std::string_view, ID3D11Texture2D*, ID3D11ShaderResourceView*, ID3D11UnorderedAccessView*, ID3D11RenderTargetView*) { return {}; }
	Texture ImmutableTexture(std::string_view, ID3D11Texture2D*, ID3D11ShaderResourceView*) { return {}; }
	Texture ImportDepth(const RE::BSGraphics::DepthStencilData&, const std::string&) { return {}; }
	Texture ImportTexture(std::string_view, ID3D11Texture2D*, const org::TextureDescription&) { return {}; }
	Texture ShareImage(ID3D11Texture2D*, const DxvkOrgInteropImageInfo&, const org::TextureDescription&, std::string_view) { return {}; }
	bool InBuiltGraph(std::string_view, Anchor) { return false; }
	void SetLateEpochHook(std::function<void()>) {}
	std::uint32_t SegmentOf(Anchor) { return 0; }
	bool IsUnusedSegment(std::uint32_t) { return false; }
	const SegmentNames* NamesOf(std::uint32_t) { return nullptr; }
	void OnEpochFailed(std::uint32_t, const std::string&, bool) {}
	void AddToFrame(std::vector<std::uint32_t>&) {}
	void SetTicketsAhead(org::GraphHost&) {}
	void Attach(org::GraphHost&) {}
	struct RevisionRecordings
	{};
	std::shared_ptr<RevisionRecordings> MakeRevisionRecordings(std::span<const std::uint32_t>, std::shared_ptr<const org::IResourceVersions>, std::uint64_t,
		std::vector<std::uint32_t>&)
	{
		return nullptr;
	}
	bool StagedStages() { return false; }
	std::uint64_t ClientSegments(std::string_view) { return 0; }
	std::uint64_t ClientsSerial() { return 0; }
	void RequireBuilds(RevisionRecordings&, std::vector<StageBuildNeed>) {}
	std::uint32_t AddRevisionMembers(RevisionRecordings&, std::span<const std::uint32_t>, std::vector<std::uint32_t>&) { return 0; }
	RevisionProgress AdvanceRevisionRecordings(RevisionRecordings&, std::uint64_t) { return { .ready = true }; }
	std::vector<std::uint32_t> RevisionStagedEpochs(const RevisionRecordings&) { return {}; }
	std::vector<std::string> RevisionMembers(const RevisionRecordings&) { return {}; }
	void InstallRevisionSchedules(const RevisionRecordings&, std::uint64_t) {}
}

#endif
