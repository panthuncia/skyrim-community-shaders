// Engine-side hooks for the water family: the water batches. Render thread.
// Each batch first submits DCLF's water epoch for the water the scene path claims (Indirect::ReachBatch). The water the batch
// renderer still holds (the wading displacement mesh, and any water the scene path refuses) is computed from its registered passes
// (WaterNative.cpp), drawn by the anchor's pass (WaterDraws.cpp), and taken out of the batch, so the engine's call draws none of it.
// In native the main camera's batches are computed from the tracked water shapes the epochs do not draw, not from registrations.
// A batch that cannot be computed reaches the engine with nobody vouching for it. Under the flush latch it is skipped. Each such
// case is a family defect, and its reason is logged once.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Features/Water/Draw/WaterDraw.h"
#include "Features/DrawcallLimitFix/Features/Water/Water.h"
#include "Features/DrawcallLimitFix/Features/Water/WaterNative.h"
#include "Snapshot/EngineReads.h"

#include "Deferred.h"
#include "Globals.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/FrameAnchors.h"
#include "Utils/Logging.h"

#include <rhi_helpers.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace DCLF::Features::Water
{
	namespace
	{
		static_assert(sizeof(Deferred::DirectionalShadowLightData) == kDirectionalShadowBytes, "t98's record");

		// The most draws one run holds. A longer batch continues in a new run.
		constexpr std::size_t kMaxRunDraws = 64;

		const char* NameOf(View a_view) { return a_view == View::Mask ? "mask" : "surface"; }

		/** @brief The inherent refusals, each logged once per reason. */
		Family::RefusalLog& Refusals()
		{
			static Family::RefusalLog log("[DCLF] water: the engine draws");
			return log;
		}

		std::uint32_t FormatOf(ID3D11DepthStencilView* a_dsv)
		{
			if (!a_dsv)
				return 0;
			D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
			a_dsv->GetDesc(&desc);
			return static_cast<std::uint32_t>(rhi::helpers::ToRHI(desc.Format));
		}

		/**
		 * @brief Reads the batch's depth format and header inputs. Only the main camera's accumulator with kMAIN's depth is taken.
		 * Returns false for anything else, which stays the engine's.
		 */
		bool CurrentShape(const void* a_accumulator, View a_view, Run& a_run, std::uint32_t& a_depthFormat)
		{
			const auto& rss = CS_ENGINE_READ(ShadowState, RecordTime, RE::BSGraphics::RendererShadowState::GetSingleton())->GetRuntimeData();
			static REL::Relocation<void**> mainAccumulator{ REL::Offset(Engine::Address::kMainAccumulator) };
			if (a_accumulator != *mainAccumulator || rss.cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
				return false;
			const auto main = static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
			if (rss.depthStencil != main) {
				Refusals().Report(std::format("a main camera {} batch with depth target {} bound, not kMAIN's", NameOf(a_view), rss.depthStencil));
				return false;
			}
			const auto& depths = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)->GetDepthStencilData().depthStencils;
			a_depthFormat = FormatOf(depths[main].views[0]);
			if (!a_depthFormat)
				return false;

			for (std::size_t h = 0; h < kHeaderRegisters.size(); ++h) {
				if (!HeadersRead(a_view)) {
					auto binding = GpuTextures::Get().NullOf(kHeaderKinds[h]);
					if (binding.index == GpuTextures::kInvalid)
						return false;
					a_run.headerIndices[h] = binding.index;
					if (binding.owner)
						a_run.owners.push_back(std::move(binding.owner));
					continue;
				}
				Family::Header header;
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				ID3D11ShaderResourceView* view = nullptr;
				const auto result = Family::ResolveHeader(kHeaderRegisters[h], kHeaderKinds[h], GpuTextures::Tag(GpuTextures::Source::FamilyHeader, static_cast<std::uint32_t>(h)),
					header, a_run.owners, &failure, &view);
				if (result == Family::HeaderResult::Buffer) {
					// A D3D11 buffer, as on Light Limit Fix's D3D11 path, cannot be imported, so the water stays the engine's.
					Refusals().Report(std::format("the water: t{} is a D3D11 buffer the graph cannot read", kHeaderRegisters[h]), "turn on its feature's ORG path");
					return false;
				}
				if (result != Family::HeaderResult::Ok) {
					Refusals().Report(std::format("a {} batch whose t{} could not be imported", NameOf(a_view), kHeaderRegisters[h]),
						GpuTextures::DescribeFailure(view, kHeaderRegisters[h], failure));
					return false;
				}
				a_run.headerIndices[h] = header.index;
				if (header.publishedId.empty())
					continue;
				const auto read = WaterDraws::ReadPublished(a_view, header.publishedId, a_run.headerIndices[h]);
				if (read != Family::InputResult::Graph) {
					Refusals().Report(std::format("a {} batch whose t{} holds '{}'", NameOf(a_view), kHeaderRegisters[h], header.publishedId), Family::ReasonOf(read));
					return false;
				}
			}
			// Physical Sky's resources, which the pixel stages read through the draw's latch. A view that reads no headers reads none of them.
			if (!HeadersRead(a_view))
				return true;
			const auto readPhysSky = [a_view](std::string_view a_id, std::uint32_t& a_index, std::string& a_why) {
				const auto result = WaterDraws::ReadPublished(a_view, a_id, a_index);
				a_why = Family::ReasonOf(result);
				return result == Family::InputResult::Graph;
			};
			std::string why;
			if (!Integrations::PhysicalSky::ResolveOrgHandles(a_run.physSky, a_run.owners, readPhysSky, why)) {
				Refusals().Report(std::format("a {} batch", NameOf(a_view)), why);
				return false;
			}
			return true;
		}

		/**
		 * @brief Whether RenderBatches (AE 0x1414b4fe0) would draw anything: a technique in [a_first, a_last] with passes in the batch
		 * renderer it draws, the accumulator's (+0x130) or, for a geometry group, the group's (*(*(renderer + 0x70 + group * 8))). Its
		 * techniques are a hash map at +0x48 (capacity +0x2C, sentinel +0x38; entries of key, group index, next), its pass groups 0x30
		 * apart at +0x8 with a head per sub-group. A narrow range looks its techniques up; a wide one walks every bucket.
		 */
		bool BatchHasPasses(const void* a_accumulator, std::uint32_t a_first, std::uint32_t a_last, std::int32_t a_group)
		{
			if (a_last < a_first)
				return false;
			const auto field = [](const void* a_object, std::size_t a_offset) {
				return *reinterpret_cast<const std::byte* const*>(static_cast<const std::byte*>(a_object) + a_offset);
			};
			const std::byte* batch = field(a_accumulator, 0x130);
			if (batch && a_group >= 0) {
				const std::byte* group = field(batch, 0x70 + static_cast<std::size_t>(a_group) * 8);
				batch = group ? field(group, 0) : nullptr;
			}
			if (!batch)
				return false;
			const std::byte* buckets = field(batch, 0x48);
			const std::byte* groups = field(batch, 0x8);
			if (!buckets || !groups)
				return false;
			const auto capacity = *reinterpret_cast<const std::uint32_t*>(batch + 0x2C);
			const std::byte* sentinel = field(batch, 0x38);
			const auto hasHeads = [&](const std::byte* a_entry) {
				const std::byte* passGroup = groups + std::size_t(*reinterpret_cast<const std::uint32_t*>(a_entry + 4)) * 0x30;
				for (std::size_t sub = 0; sub < 5; ++sub)
					if (field(passGroup, sub * 8))
						return true;
				return false;
			};
			if (a_last - a_first > 64) {
				for (std::uint32_t bucket = 0; bucket < capacity; ++bucket) {
					const std::byte* entry = buckets + std::size_t(bucket) * 0x10;
					if (!field(entry, 8))
						continue;
					for (; entry && entry != sentinel; entry = field(entry, 8)) {
						const auto technique = *reinterpret_cast<const std::uint32_t*>(entry);
						if (technique >= a_first && technique <= a_last && hasHeads(entry))
							return true;
					}
				}
				return false;
			}
			for (std::uint32_t technique = a_first; technique <= a_last; ++technique) {
				const std::byte* entry = buckets + std::size_t((capacity - 1u) & technique) * 0x10;
				if (!field(entry, 8))
					continue;
				for (; entry && entry != sentinel; entry = field(entry, 8)) {
					if (*reinterpret_cast<const std::uint32_t*>(entry) != technique)
						continue;
					if (hasHeads(entry))
						return true;
					break;
				}
			}
			return false;
		}

		/**
		 * @brief Draws a batch's computed draws: runs of at most kMaxRunDraws draws, the first carrying the batch's headers' owners and
		 * the CPU-driven frame's DCLF water. Nothing is submitted when there is nothing to draw.
		 */
		void SubmitDraws(Run& a_shape, std::vector<Draw>&& a_draws, Family::Owners&& a_owners, void* a_cpuWater, std::uint32_t a_cpuVariant)
		{
			if (a_draws.empty() && !a_cpuWater) {
				for (auto& owner : a_owners)
					a_shape.owners.push_back(std::move(owner));
				WaterDraws::Keep(std::move(a_shape.owners));
				return;
			}
			for (std::size_t at = 0;; at += kMaxRunDraws) {
				auto run = std::make_shared<Run>();
				run->view = a_shape.view;
				run->headerIndices = a_shape.headerIndices;
				run->physSky = a_shape.physSky;
				if (at == 0) {
					run->owners = std::move(a_shape.owners);
					for (auto& owner : a_owners)
						run->owners.push_back(std::move(owner));
					run->cpuWater = a_cpuWater;
					run->cpuVariant = a_cpuVariant;
				}
				const auto end = (std::min)(a_draws.size(), at + kMaxRunDraws);
				run->draws.assign(std::make_move_iterator(a_draws.begin() + at), std::make_move_iterator(a_draws.begin() + end));
				WaterDraws::Submit(std::move(run));
				if (end >= a_draws.size())
					break;
			}
		}

		enum Site : std::uint32_t
		{
			kMaskSite,
			kGroup10Site,
			kSurfaceSite,
		};

		/**
		 * @brief A water batch hook. kSite tells apart the mask, the surface, and geometry group 10 just before the surface (it holds
		 * the wading surface, hint 13). The mask and surface batches first submit DCLF's water epoch for what it claims; the rest of
		 * the batch's water is computed and drawn here, and the engine's call draws what is not water.
		 */
		template <View kView, std::uint32_t kSite>
		struct WaterBatch
		{
			static void thunk(void* a_accumulator, std::uint32_t a_first, std::uint32_t a_last, std::uint32_t a_flags, std::int32_t a_group)
			{
				void* cpuWater = nullptr;
				std::uint32_t cpuVariant = 0;
				if constexpr (kSite != kGroup10Site)
					(void)Indirect::ReachBatch(kView, a_accumulator, &cpuWater, &cpuVariant);
				if (!FrameAnchors::IsRenderOwner() || !WaterDraws::Available()) {
					if (cpuWater) {
						static std::atomic<bool> logged{ false };
						if (!logged.exchange(true))
							logger::error("[DCLF] water: a CPU-driven frame's {} batch could not be taken by the water family; its DCLF water is not drawn", NameOf(kView));
					}
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				// Native computes the main camera's batch from the tracked water shapes, not from the engine's registrations.
				static REL::Relocation<void**> mainAccumulator{ REL::Offset(Engine::Address::kMainAccumulator) };
				const bool fromScene = ::RenderGraph::Engine::Native::Active() && a_accumulator == *mainAccumulator;
				// Claimed water leaves the engine's batch empty: then there is nothing to compute, and the engine's call draws nothing.
				if (!cpuWater && !fromScene && !BatchHasPasses(a_accumulator, a_first, a_last, a_group)) {
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				Family::BeginBatch();
				Run run;
				run.view = kView;
				std::uint32_t depthFormat = 0;
				if (!CurrentShape(a_accumulator, kView, run, depthFormat) || !WaterDraws::CanTake(kView)) {
					if (cpuWater) {
						static std::atomic<bool> logged{ false };
						if (!logged.exchange(true))
							logger::warn("[DCLF] water: a CPU-driven frame's {} batch is not the water family's yet; its DCLF water is not drawn that frame", NameOf(kView));
					}
					WaterDraws::Keep(std::move(run.owners));
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				// The sun's shadow record is the frame's, so it is read once per batch rather than per draw.
				static Deferred::DirectionalShadowLightData directionalShadow{};
				if (globals::deferred)
					globals::deferred->GetDirectionalShadowLightData(directionalShadow);
				const Native::BatchInputs inputs{ .view = kView,
					.accumulator = static_cast<RE::BSShaderAccumulator*>(a_accumulator),
					.first = a_first,
					.last = a_last,
					.group = a_group,
					.depthFormat = depthFormat,
					.directionalShadow = &directionalShadow,
					.physSky = run.physSky };
				std::vector<Draw> draws;
				Family::Owners owners;
				std::string why;
				if (!(fromScene ? Native::BuildScene(inputs, draws, owners, why) : Native::BuildBatch(inputs, draws, owners, why))) {
					// The batch's water reaches the engine, which draws it or, under the flush latch, skips it.
					Refusals().Report(std::format("a {} batch's water", NameOf(kView)), why);
					SubmitDraws(run, {}, std::move(owners), cpuWater, cpuVariant);
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				auto detached = Native::DetachBatch(inputs);
				SubmitDraws(run, std::move(draws), std::move(owners), cpuWater, cpuVariant);
				func(a_accumulator, a_first, a_last, a_flags, a_group);
				Native::Reattach(detached);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief FinishAccumulating's check for water mask passes. When DCLF draws every water shape, the engine holds none and would skip
		 * the mask block, and with it DCLF's mask epoch. The block then runs with an empty engine batch.
		 */
		struct MaskPassesCheck
		{
			static bool thunk(void* a_batchRenderer, std::uint32_t a_first, std::uint32_t a_last)
			{
				return func(a_batchRenderer, a_first, a_last) || Indirect::MaskBatchWanted(a_batchRenderer);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void PreflightHooks()
	{
		if (!Enabled())
			return;
		const bool known = Address::kMaskBatch.Known() && Address::kSurfaceBatch.Known();
		HookSites::RequireKnown("the water batches", known);
		if (!known)
			return;
		HookSites::RequireCall("the water mask batch (FinishAccumulating's RenderBatches of the Stencil technique)", REL::Offset(Address::kMaskBatch).address());
		HookSites::RequireCall("the water batch (FinishAccumulatingPostResolveDepth's RenderBatches of the water techniques)",
			REL::Offset(Address::kSurfaceBatch).address());
		if (Address::kMaskPassesCheck.Known())
			HookSites::RequireCall("the water mask passes check (FinishAccumulating, before the mask block)", REL::Offset(Address::kMaskPassesCheck).address());
		if (Address::kGroup10Batch.Known())
			HookSites::RequireCall("geometry group 10's batch (FinishAccumulatingPostResolveDepth, before the water)", REL::Offset(Address::kGroup10Batch).address());
	}

	bool InstallHooks()
	{
		if (!Address::kMaskBatch.Known() || !Address::kSurfaceBatch.Known()) {
			logger::warn("[DCLF] water: this runtime has no address for the water batches; the water stays the engine's");
			return false;
		}
		stl::write_thunk_call<WaterBatch<View::Mask, kMaskSite>>(REL::Offset(Address::kMaskBatch).address());
		if (Address::kMaskPassesCheck.Known())
			stl::write_thunk_call<MaskPassesCheck>(REL::Offset(Address::kMaskPassesCheck).address());
		stl::write_thunk_call<WaterBatch<View::Surface, kSurfaceSite>>(REL::Offset(Address::kSurfaceBatch).address());
		if (Address::kGroup10Batch.Known())
			stl::write_thunk_call<WaterBatch<View::Surface, kGroup10Site>>(REL::Offset(Address::kGroup10Batch).address());
		Family::AddSetup("water", [] { return WaterDraws::Setup(); });
		logger::info("[DCLF] water: hooks installed (the water mask check and batch, geometry group 10 and water batches)");
		return true;
	}
}
