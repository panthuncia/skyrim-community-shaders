// The payload parts' version chains and backing pools, and the frame's payload read from the installed snapshot.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "RenderGraph/BufferRefs.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Gpu/PooledBackings.h"

#include <ORGModuleServices/ElasticCapacity.h>
#include "RenderGraph/Async/Retirement.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/BufferChains.h>
#include <ORGModuleServices/VersionedBufferBuild.h>
#include <Render/Runtime/IUploadService.h>

namespace DCLF::Draws
{
	namespace
	{
		using org::services::BufferByteRange;

		constexpr std::size_t Part(PayloadPart a_part) { return static_cast<std::size_t>(a_part); }

		template <class T>
		constexpr std::uint64_t Words(std::uint64_t a_elements) { return a_elements * sizeof(T) / 4; }

		// Inputs and geometries are 4-byte words because the shaders read them that way.
		// The row parts need a device address because the latches name them by it.
		struct PartPool
		{
			const char* name;
			std::uint32_t stride;
			bool deviceAddress;
		};
		constexpr std::array<PartPool, kPayloadParts> kPools{ { { "objects", sizeof(BindlessObject), false }, { "extras", 16, false },
			{ "geometries", 4, false }, { "material-rows", kMaterialRowBytes, true }, { "pipeline-rows", kPipelineRowBytes, true }, { "inputs", 4, false },
			{ "shadow-rows", sizeof(ShadowMaterialRow), true } } };

		/** @brief Part a_part's late-bound buffer. Readers declare it and MakeFramePayload publishes its version. */
		RenderGraph::LateRefBuffer& PartLate(std::size_t a_part)
		{
			static const auto parts = [] {
				std::array<std::unique_ptr<RenderGraph::LateRefBuffer>, kPayloadParts> made;
				for (std::size_t k = 0; k < kPayloadParts; ++k)
					made[k] = std::make_unique<RenderGraph::LateRefBuffer>(fmt::format("cs.dclf.payload.{}.late", kPools[k].name));
				return made;
			}();
			return *parts[a_part];
		}

		// One backing a frame in flight may still bind, plus the newest version and the one being built.
		// A build that finds none free waits. The frames keep drawing the snapshot they have.
		constexpr std::uint32_t kBackingsPerClass = RenderGraphRuntime::kGameFramesInFlight + 2;

		constexpr std::uint64_t kNoTail = org::services::BufferChainSource::kNoTail;  // the part has no tails

		/** @brief Whether a_part has tails, rows after the journalled ones that are written whole into each version. */
		constexpr bool Tailed(PayloadPart a_part) { return a_part == PayloadPart::Geometries || a_part == PayloadPart::Inputs; }

		struct PartsState;
		PartsState& Parts();
		void ReportFailure(std::size_t a_part, std::uint64_t a_version, std::string_view a_error);

		/** @brief Producer thread, at a part's first post. Its pool, on DCLF's retirement clock. */
		std::shared_ptr<org::services::BackingPool> MakePool(std::size_t a_part,
			std::function<void(org::services::BackingPoolEvent, std::string_view, std::uint64_t)> a_telemetry)
		{
			// A payload version lives for a scene revision or two, so its churn goes to ORG's per-frame blocks. The row bases hand
			// out its address (Backing::address), which stays valid there: per-frame backings never move.
			auto hooks = PooledBackings::PerFrameHooks();
			hooks.telemetry = std::move(a_telemetry);
			// One spare is enough. A fresh backing costs a GPU copy, not an upload, so more spares only hold VRAM.
			// On HexRim five pipeline-row backings took 40 MB.
			return org::services::BackingPool::Create(std::move(hooks), ::RenderGraph::Frame::Retirement::Clock(),
				{ .name = fmt::format("cs.dclf.payload.{}", kPools[a_part].name), .maxPerClass = kBackingsPerClass,
					.framesInFlight = RenderGraphRuntime::kGameFramesInFlight, .spareFree = 1 });
		}

		org::services::BufferChainsDesc ChainsDesc()
		{
			org::services::BufferChainsDesc desc;
			for (std::size_t k = 0; k < kPayloadParts; ++k)
				desc.chains.push_back({ .name = fmt::format("cs.dclf.payload.{}", kPools[k].name), .stride = kPools[k].stride,
					.deviceAddress = kPools[k].deviceAddress, .tailed = Tailed(static_cast<PayloadPart>(k)) });
			desc.makePool = MakePool;
			// Each version is built from its predecessor on the preparation pool. Its tag's built runs on the build's thread before it is frozen.
			desc.post = [](org::services::VersionedBufferBuildInput a_input, org::services::BufferChainBuilt a_built, org::services::BufferChainDone a_done) {
				auto tag = std::make_shared<Runtime::PayloadPartTag>();
				tag->built = std::move(a_built);
				a_input.tag = std::move(tag);
				return SceneScheduler::Graph().PostPayloadPart(std::move(a_input), std::move(a_done));
			};
			// The builder picks up the result in its next Settle.
			desc.wake = [] { WakeSnapshotBuilder(); };
			desc.failed = ReportFailure;
			// Grow with a quarter's headroom, not a power of two. A part that changes every frame keeps a backing per frame in flight.
			// HexRim's pipeline rows used 4 backings of 16 MB for 9.5 MB of rows.
			desc.growth = org::services::GrowthPolicy::Headroom(25, 64);
			// Shrink once the part stays much smaller. The predecessor's copy is clipped to this version's rows.
			desc.shrink = PooledBackings::Shrink();
			return desc;
		}

		struct PartsState
		{
			org::services::BufferChains chains{ ChainsDesc() };  // the snapshot builder's, apart from the any-thread reads
			std::array<bool, kPayloadParts> reported{};        // the snapshot builder's: a part's failure logged
			std::uint64_t sources = 0;                          // the snapshot builder's: MakeSource's serials
			bool uploaderReported = false;                      // the snapshot builder's
			std::shared_ptr<const ScenePayload> installed;      // render thread: the frame's (Install)
		};
		/** @brief Never destroyed, because leases and build results can arrive while the process tears down. */
		PartsState& Parts()
		{
			static auto* state = new PartsState();
			return *state;
		}

		/** @brief Snapshot builder. A part's build failed. No fallback: the part keeps its last version and the next source builds it again. */
		void ReportFailure(std::size_t a_part, std::uint64_t a_version, std::string_view a_error)
		{
			if (!std::exchange(Parts().reported[a_part], true))
				logger::error("[DCLF] the scene payload's {} version {} failed: {}; its snapshot has no payload, the next publication builds it again",
					PayloadPools::kPartNames[a_part], a_version, a_error);
		}

		/** @brief A part's rows in elements. Geometries and Inputs count words. */
		std::uint64_t RowsOf(const PayloadSource& a_source, PayloadPart a_part)
		{
			const auto units = a_source.units[Part(a_part)];
			switch (a_part) {
			case PayloadPart::Geometries:
				return Words<GeometryDraw>(units);
			case PayloadPart::Inputs:
				return Words<DrawInput>(units);
			default:
				return units;
			}
		}

		bool Present(const PayloadSource& a_source, PayloadPart a_part)
		{
			switch (a_part) {
			case PayloadPart::MaterialRows:
			case PayloadPart::PipelineRows:
				return a_source.main != nullptr;
			case PayloadPart::ShadowRows:
				return a_source.shadow != nullptr;
			default:
				return true;
			}
		}

		/** @brief The first byte of a_part's tails, or kNoTail. */
		std::uint64_t TailStartOf(const PayloadSource& a_source, PayloadPart a_part)
		{
			switch (a_part) {
			case PayloadPart::Geometries:
				return a_source.geometries->SlotCount() * sizeof(GeometryDraw);
			case PayloadPart::Inputs:
				return a_source.regionCount * sizeof(DrawInput);
			default:
				return kNoTail;
			}
		}

		/** @brief A part's journal position. 0 means it is not journalled and is written whole. */
		std::uint64_t ContentEpochOf(const PayloadSource& a_source, PayloadPart a_part)
		{
			switch (a_part) {
			case PayloadPart::Objects:
				return a_source.views->objects.ContentEpoch();
			case PayloadPart::Extras:
				return a_source.views->extras.ContentEpoch();
			case PayloadPart::Geometries:
				return a_source.geometries->ContentEpoch();
			case PayloadPart::MaterialRows:
				return a_source.main->body->materialRows.ContentEpoch();
			case PayloadPart::PipelineRows:
				return a_source.main->body->pipelineRows.ContentEpoch();
			case PayloadPart::Inputs:
				return a_source.region->ContentEpoch();
			case PayloadPart::ShadowRows:
				return a_source.shadow->materialRows.ContentEpoch();
			default:
				return 0;
			}
		}

		/** @brief A journal's runs since a_from as byte ranges. nullopt when it does not reach back that far. */
		std::optional<std::vector<BufferByteRange>> RunsOf(const ChangeJournal::Snapshot& a_changes, std::uint64_t a_unitBytes, std::uint64_t a_count,
			std::uint64_t a_from)
		{
			if (!a_changes.contentEpoch || !a_from || a_from < a_changes.floor || a_from > a_changes.contentEpoch)
				return std::nullopt;
			std::vector<BufferByteRange> out;
			a_changes.ForEachRun(a_from, a_count, [&](std::uint64_t a_first, std::uint64_t a_run) { out.push_back({ a_first * a_unitBytes, a_run * a_unitBytes }); });
			return out;
		}

		/** @brief Bytes a_part's journal changed since a_from. A superset is fine, never too little. nullopt below the floor. */
		std::optional<std::vector<BufferByteRange>> JournalRanges(const PayloadSource& a_source, PayloadPart a_part, std::uint64_t a_from)
		{
			switch (a_part) {
			case PayloadPart::Objects:
				return RunsOf(a_source.views->objects.changes, sizeof(BindlessObject), a_source.views->objects.Count(), a_from);
			case PayloadPart::Extras:
				return RunsOf(a_source.views->extras.changes, 16, a_source.views->extras.Rows(), a_from);
			case PayloadPart::Geometries:
				return RunsOf(a_source.geometries->slots.changes, sizeof(GeometryDraw), a_source.geometries->SlotCount(), a_from);
			case PayloadPart::MaterialRows: {
				const auto& rows = a_source.main->body->materialRows;
				return RunsOf(rows.changes, sizeof(MaterialRow) / kMaterialRowParts, rows.Count() * kMaterialRowParts, a_from);
			}
			case PayloadPart::PipelineRows:
				return RunsOf(a_source.main->body->pipelineRows.changes, sizeof(PipelineRow), a_source.main->body->pipelineRows.Count(), a_from);
			case PayloadPart::Inputs:
				return RunsOf(a_source.region->changes, sizeof(DrawInput), a_source.region->Count(), a_from);
			case PayloadPart::ShadowRows:
				return RunsOf(a_source.shadow->materialRows.changes, sizeof(ShadowMaterialRow), a_source.shadow->materialRows.Count(), a_from);
			default:
				return std::nullopt;
			}
		}

		using DeltaWrite = org::services::ChainDeltaWrite;

		/**
		 * @brief Emits a part's delta on the producer's thread. The journal's runs since a_from, then its tails whole.
		 * nullopt a_from means everything.
		 */
		void EmitDelta(PayloadPart a_part, const PayloadSource& a_source, std::optional<std::uint64_t> a_from, std::uint64_t a_version, std::uint64_t a_previous,
			const DeltaWrite& a_write)
		{
			// ~0 is past every journal position, so a holder there is sent everything.
			const std::uint64_t from = a_from ? *a_from : ~std::uint64_t{ 0 };
			const auto sink = [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) { a_write(a_data, a_bytes, a_offset); };
			switch (a_part) {
			case PayloadPart::Objects:
				a_source.views->objects.Emit(from, sink);
				break;
			case PayloadPart::Extras: {
				// CS_DCLF_PERSISTENT_PARITY checks the extras rows against the observer's mirror as each version is built.
				std::vector<float>* mirror = nullptr;
				Diagnostics::Notify(Diagnostics::ExtrasSending{ a_version, a_previous, mirror });
				EmitExtras(a_source.views->extras, from, mirror, sink);
				Diagnostics::Notify(Diagnostics::ExtrasSent{ a_version, a_source.views->extras, a_source.views->tables.get() });
				break;
			}
			case PayloadPart::Geometries: {
				const auto& geometries = *a_source.geometries;
				geometries.slots.Emit(from, sink);
				if (!geometries.streamRows.empty())
					sink(geometries.streamRows.data(), geometries.streamRows.size() * sizeof(GeometryDraw), geometries.SlotCount() * sizeof(GeometryDraw));
				break;
			}
			case PayloadPart::MaterialRows: {
				const auto& rows = a_source.main->body->materialRows;
				EmitMainRowParts(rows, kMaterialRowParts, from, rows.Count(), sink);
				break;
			}
			case PayloadPart::PipelineRows: {
				const auto& rows = a_source.main->body->pipelineRows;
				EmitMainRows(rows, from, rows.Count(), sink);
				break;
			}
			case PayloadPart::Inputs: {
				const auto put = [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) { a_write(a_data, a_bytes, a_offset); };
				a_source.region->Emit(from, put);
				if (a_source.mainList && !a_source.mainList->empty())
					put(a_source.mainList->data(), a_source.mainList->size() * sizeof(DrawInput), a_source.mainInputsAt * sizeof(DrawInput));
				if (a_source.shadowList && !a_source.shadowList->empty())
					put(a_source.shadowList->data(), a_source.shadowList->size() * sizeof(DrawInput), a_source.shadowInputsAt * sizeof(DrawInput));
				break;
			}
			case PayloadPart::ShadowRows:
				a_source.shadow->materialRows.Emit(from, sink);
				break;
			default:
				break;
			}
		}

	}

	bool PayloadSource::Present(std::size_t a_part) const { return Draws::Present(*this, static_cast<PayloadPart>(a_part)); }
	std::uint64_t PayloadSource::Elements(std::size_t a_part) const { return RowsOf(*this, static_cast<PayloadPart>(a_part)); }
	std::uint64_t PayloadSource::ContentEpoch(std::size_t a_part) const { return ContentEpochOf(*this, static_cast<PayloadPart>(a_part)); }
	std::uint64_t PayloadSource::TailStart(std::size_t a_part) const { return TailStartOf(*this, static_cast<PayloadPart>(a_part)); }

	std::optional<std::vector<org::services::BufferByteRange>> PayloadSource::ChangedSince(std::size_t a_part, std::uint64_t a_epoch) const
	{
		return JournalRanges(*this, static_cast<PayloadPart>(a_part), a_epoch);
	}

	void PayloadSource::EmitDelta(std::size_t a_part, std::optional<std::uint64_t> a_from, std::uint64_t a_version, std::uint64_t a_previous,
		const org::services::ChainDeltaWrite& a_write) const
	{
		Draws::EmitDelta(static_cast<PayloadPart>(a_part), *this, a_from, a_version, a_previous, a_write);
	}

	namespace PayloadPools
	{
		std::shared_ptr<const PayloadSource> MakeSource(std::shared_ptr<const void> a_keep, const StreamViews& a_views, const MainPayload* a_main,
			const ShadowPayload* a_shadow)
		{
			DCLF_ZONE("CS.DCLF.Payload.Source");
			if (!a_main && !a_shadow)
				return nullptr;
			auto out = std::make_shared<PayloadSource>();
			out->keep = std::move(a_keep);
			out->views = &a_views;
			out->main = a_main;
			out->shadow = a_shadow;
			if (a_main) {
				out->products = a_main->body->products;
				Extensions::CarryPayloadProducts(out->products);
			}
			// The main and shadow payloads share the same geometry table.
			out->geometries = a_main ? &a_main->body->geometryDraws : &a_shadow->geometries;
			// The scene list is the region, then the main frame inputs, then the shadow inputs. The last two are written whole into each version.
			out->region = a_main ? &a_main->resident : &a_shadow->regionInputs;
			out->mainList = a_main ? &a_main->body->inputList : nullptr;
			out->shadowList = a_shadow ? &a_shadow->inputList : nullptr;
			out->regionCount = out->region->Count();
			out->mainInputsAt = out->regionCount;
			out->shadowInputsAt = out->mainInputsAt + (out->mainList ? out->mainList->size() : 0);
			auto& units = out->units;
			units[Part(PayloadPart::Objects)] = a_views.objects.Count();
			units[Part(PayloadPart::Extras)] = a_views.extras.Rows();
			units[Part(PayloadPart::Geometries)] = out->geometries->Count();
			units[Part(PayloadPart::MaterialRows)] = a_main ? a_main->body->materialRows.Count() : 0;
			units[Part(PayloadPart::PipelineRows)] = a_main ? a_main->body->pipelineRows.Count() : 0;
			units[Part(PayloadPart::Inputs)] = out->shadowInputsAt + (out->shadowList ? out->shadowList->size() : 0);
			units[Part(PayloadPart::ShadowRows)] = a_shadow ? a_shadow->materialRows.Count() : 0;
			out->serial = ++Parts().sources;
			return out;
		}

		bool Settle()
		{
			return Parts().chains.Settle();
		}

		std::string PendingParts()
		{
			const auto& chains = Parts().chains;
			std::string out;
			for (const auto& pending : chains.Pending())
				out += fmt::format("{}{} v{}{}", out.empty() ? "" : ", ", kPartNames[pending.chain], pending.version, pending.done ? " (done)" : "");
			return fmt::format("{}; {} suspensions since the report", out.empty() ? std::string("none") : out, chains.Suspensions());
		}

		bool Post(const std::shared_ptr<const PayloadSource>& a_source, const std::shared_ptr<org::runtime::IUploadService>& a_uploads)
		{
			auto& s = Parts();
			// The builds copy from the predecessor on the dedicated upload queue.
			if (!a_uploads || !a_uploads->HasDedicatedStreamingQueue()) {
				if (!std::exchange(s.uploaderReported, true))
					logger::info("[DCLF] the scene payload's versions wait for the dedicated upload queue: snapshots have no payload until it is made");
				return false;
			}
			return s.chains.Post(a_source, a_uploads);
		}

		std::shared_ptr<const ScenePayload> Finish(const PayloadSource& a_source)
		{
			auto out = std::make_shared<ScenePayload>();
			if (!Parts().chains.Finish(a_source, out->versions))
				return nullptr;
			out->units = a_source.units;
			out->regionCount = a_source.regionCount;
			out->mainInputsAt = a_source.mainInputsAt;
			out->shadowInputsAt = a_source.shadowInputsAt;
			out->main = a_source.main != nullptr;
			out->shadow = a_source.shadow != nullptr;
			out->products = a_source.products;
			out->serial = a_source.serial;
			return out;
		}

		void Install(std::shared_ptr<const ScenePayload> a_payload)
		{
			Parts().installed = std::move(a_payload);
		}

		void DeclareReads(org::RenderPassBuilder& a_builder)
		{
			RenderGraph::DeclaredRefs unchecked;  // MakeFramePayload's refs are the published versions' own
			for (std::size_t k = 0; k < kPayloadParts; ++k)
				PartLate(k).Declare(a_builder, RenderGraph::RefAccess::Read, unchecked);
		}

		bool MakeFramePayload(PayloadVersions& a_out)
		{
			a_out = {};
			const auto& installed = Parts().installed;
			if (!installed)
				return false;
			const auto& payload = *installed;
			for (std::size_t k = 0; k < kPayloadParts; ++k) {
				const auto& version = payload.versions[k];
				if (!version)
					continue;
				const auto& backing = version->lease;
				const RenderGraph::BufferRef published = PartLate(k).Publish(backing->buffer, version);
				// Shaders read rows through the ref. The layouts use the address.
				a_out.parts[k] = { backing->srvIndex, kPools[k].deviceAddress ? backing->address : 0,
					kPools[k].deviceAddress ? published : RenderGraph::BufferRef{}, payload.units[k] };
				if (!a_out.parts[k].srvIndex)
					return false;
			}
			a_out.regionCount = payload.regionCount;
			a_out.mainInputsAt = payload.mainInputsAt;
			a_out.shadowInputsAt = payload.shadowInputsAt;
			a_out.main = payload.main;
			a_out.shadow = payload.shadow;
			a_out.serial = payload.serial;
			return true;
		}

		void BindInstalled(std::uint64_t a_point)
		{
			if (const auto& installed = Parts().installed; installed && a_point)
				for (const auto& version : installed->versions)
					if (version)
						version->lease.Bind(a_point);
		}

		bool Available()
		{
			auto* host = RenderGraphRuntime::Get().Host();
			auto uploads = host ? host->RetainUploads() : nullptr;
			return uploads && uploads->HasDedicatedStreamingQueue();
		}

		std::optional<std::uint64_t> OldestContentEpoch(PayloadPart a_part)
		{
			return Parts().chains.OldestContentEpoch(Part(a_part));
		}

		std::array<PartReport, kPayloadParts> TakeReport()
		{
			const auto reports = Parts().chains.TakeReport();
			std::array<PartReport, kPayloadParts> out{};
			for (std::size_t k = 0; k < kPayloadParts && k < reports.size(); ++k)
				out[k] = reports[k];
			return out;
		}
	}
}
