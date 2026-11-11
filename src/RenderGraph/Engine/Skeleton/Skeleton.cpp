#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "RenderGraph/Switches.h"

bool RenderGraph::Engine::Skeleton::Requested()
{
	static const bool requested = Switches::On("CS_ORG_ENGINE_SKELETON");
	return requested;
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/Engine/Skeleton/SkeletonSites.h"

#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/PassBuilders.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <array>
#	include <bit>
#	include <cstring>
#	include <map>
#	include <optional>
#	include <set>
#	include <tuple>
#	include <vector>

namespace RenderGraph::Engine::Skeleton
{
	namespace
	{
		using FrameAnchors::Anchor;
		using DepthTarget = RE::RENDER_TARGETS_DEPTHSTENCIL;

		constexpr const char* kShader = "RenderGraph/EngineClear.hlsl";
		// Gate entries per group. A target's entries are never reused after a reset (a resize), so a recording made before the reset
		// can never clear the wrong view; a group that runs out returns to D3D11.
		constexpr std::uint32_t kGateEntries = 128;
		// Times a group (or the copy) may be built again for new engine textures before it stays on D3D11 for the session.
		constexpr std::uint32_t kMaxResets = 16;
		// Clear passes per view (one per clear value): a view whose clear colour keeps changing stays on D3D11 instead of using up
		// its group's gate entries.
		constexpr std::size_t kMaxKeysPerTarget = 4;

		/** @brief The three clear groups, each an anchor of its own so a run declares only its group's views. */
		enum class Group : std::uint8_t
		{
			Shadow,
			Cube,
			Frame,
			Count
		};
		constexpr std::size_t kGroups = static_cast<std::size_t>(Group::Count);
		constexpr std::array<Anchor, kGroups> kGroupAnchors{ Anchor::EngineShadowClear, Anchor::EngineCubeClear, Anchor::EngineFrameClear };
		constexpr std::array<const char*, kGroups> kGroupIds{ "cs.engine.skeleton.shadow", "cs.engine.skeleton.cube", "cs.engine.skeleton.frame" };
		constexpr std::array<const char*, kGroups> kGroupNames{ "shadow", "cube", "frame" };
		constexpr const char* kCopyId = "cs.engine.skeleton.depth-copy";

		/** @brief Which engine table a view comes from. */
		enum class Kind : std::uint8_t
		{
			Colour,  // RendererData::renderTargets[index].RTV
			Depth,   // depthStencils[index].views[slice]
			Cube,    // cubemapRenderTargets[index].cubeSideRTV[face]
		};

		struct Target
		{
			Kind kind = Kind::Colour;
			std::uint32_t index = 0;
			std::uint32_t slice = 0;  // the depth view's slice or the cube face; the view's own array slice is checked against it
		};

		/** @brief What one clear pass is made for: the view, the depth flags, the colour's bits. */
		struct Key
		{
			Target target;
			std::uint32_t depthFlags = 0;
			std::array<std::uint32_t, 4> colour{};

			auto Tie() const { return std::tie(target.kind, target.index, target.slice, depthFlags, colour); }
			bool operator<(const Key& a_other) const { return Tie() < a_other.Tie(); }
			bool operator==(const Key& a_other) const { return Tie() == a_other.Tie(); }
		};

		/** @brief One clear pass's immutable spec: its view, its program and its gate entry. */
		struct ClearSpec
		{
			std::string name;
			std::shared_ptr<org::ExternalTextureResource> resource;
			std::uint32_t slice = 0;
			bool depth = false;
			std::shared_ptr<const RasterProgram> program;
			std::array<std::uint32_t, 4> colour{};
			std::uint32_t width = 0, height = 0;
			std::shared_ptr<const RunGate> gate;
			std::uint32_t entry = 0;
		};

		struct ClearBindings
		{
			org::DeclaredViewToken view;
		};

		struct ClearFrame
		{
			std::shared_ptr<const ClearSpec> spec;
		};

		/** @brief A fullscreen draw into one view through a run gate: the clear when the commit opened the entry, nothing otherwise. */
		class ClearPass final : public org::TypedRenderGraphPass<ClearPass, ClearFrame, ClearBindings>
		{
		public:
			explicit ClearPass(std::shared_ptr<const ClearSpec> a_spec) :
				spec(std::move(a_spec)) {}

			ClearBindings Declare(org::PassBuilder& a_builder)
			{
				const auto range = org::Subresources(spec->resource, org::Mip(0, 1), org::Slice(spec->slice, 1));
				if (spec->depth)
					return { a_builder.DepthReadWrite(range, org::DsvView{ .slice = spec->slice }).View() };
				return { a_builder.RenderTarget(range, org::RtvView{ .slice = spec->slice }).View() };
			}

			// The recording depends only on what the pass was made with; the gate decides each run.
			void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.get())); }

			ClearFrame Prepare(const ClearBindings&, const org::PassPrepareContext&) const { return { spec }; }

			static void Record(const ClearBindings& a_bindings, const ClearFrame& a_frame, org::PassRecordContext& a_recording)
			{
				const auto& clear = *a_frame.spec;
				RasterProgram::DrawDesc draw{};
				draw.viewport = { .x = 0, .y = 0, .width = clear.width, .height = clear.height };
				draw.debugName = clear.name.c_str();
				draw.indirect = &clear.gate->Latch();
				draw.indirectOffset = clear.gate->Offset(clear.entry);
				if (clear.depth) {
					const RasterProgram::Depth depth{ .view = &a_bindings.view, .readOnly = false, .load = rhi::LoadOp::Load };
					draw.depth = &depth;
					clear.program->Draw(a_recording, draw);
				} else {
					const RasterProgram::Colour colour{ .view = &a_bindings.view, .load = rhi::LoadOp::Load };
					draw.colours = { &colour, 1 };
					draw.constants = clear.colour;
					clear.program->Draw(a_recording, draw);
				}
			}

		private:
			std::shared_ptr<const ClearSpec> spec;
		};

		/** @brief A clear pass the group registered, or one waiting for the group's next registration. */
		struct Entry
		{
			Key key;
			std::shared_ptr<const ClearSpec> spec;
			bool registered = false;  // in the group's current registration
		};

		/** @brief A clear the engine asked for this flush, with what replays it on D3D11 when the pass did not run. */
		struct Pending
		{
			std::uint32_t gateEntry = 0;
			bool depth = false;
			void* view = nullptr;
			std::array<float, 4> colour{};
			UINT flags = 0;
			FLOAT depthValue = 1.0f;
			UINT8 stencil = 0;
		};

		struct GroupState
		{
			std::vector<Entry> entries;
			std::shared_ptr<RunGate> gate;
			std::uint32_t nextEntry = 0;
			std::vector<Pending> pending;
			bool dirty = false;       // an entry joined or the group was reset: register before the next run
			bool registered = false;  // a registration is live
			bool executed = false;    // the current run's executed signal
			bool failed = false;      // stays on D3D11 for the session
			bool announced = false;   // the first clear on the graph was logged
			std::uint32_t resets = 0;
			std::uint64_t taken = 0, replayed = 0;
		};

		struct CopyState
		{
			std::shared_ptr<org::ExternalTextureResource> source, target;
			bool wanted = false;
			bool executed = false;
			bool failed = false;
			bool announced = false;
			std::uint32_t resets = 0;
		};

		struct SkeletonState
		{
			bool installed = false;
			bool setUp = false;
			bool flushing = false;
			std::array<GroupState, kGroups> groups;
			CopyState copy;
			// Programs by (format, depth flags; 0 for colour).
			std::map<std::pair<rhi::Format, std::uint32_t>, std::shared_ptr<const RasterProgram>> programs;
			std::set<std::tuple<Kind, std::uint32_t, std::uint32_t>> refused;  // views a clear pass cannot be made for (logged once)
		};

		SkeletonState& Skel()
		{
			static SkeletonState state;
			return state;
		}

		GroupState& GroupOf(Group a_group) { return Skel().groups[static_cast<std::size_t>(a_group)]; }

		bool Usable()
		{
			auto& state = Skel();
			return state.setUp && !state.flushing && FrameAnchors::OnMainThread() && RenderGraphRuntime::Get().IsActive();
		}

		std::string TargetName(const Target& a_target)
		{
			switch (a_target.kind) {
			case Kind::Colour:
				return fmt::format("rt{}", a_target.index);
			case Kind::Depth:
				return fmt::format("depth{}.{}", a_target.index, a_target.slice);
			default:
				return fmt::format("cube{}.{}", a_target.index, a_target.slice);
			}
		}

		Group GroupFor(const Target& a_target)
		{
			if (a_target.kind == Kind::Cube)
				return Group::Cube;
			if (a_target.kind == Kind::Depth) {
				switch (a_target.index) {
				case DepthTarget::kCUBEMAP_REFLECTIONS:
					return Group::Cube;
				case DepthTarget::kSHADOWMAPS_ESRAM:
				case DepthTarget::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM:
				case DepthTarget::kSHADOWMAPS:
				case DepthTarget::kPRECIPITATION_OCCLUSION_MAP:
					return Group::Shadow;
				default:
					break;
				}
			}
			return Group::Frame;
		}

		/** @brief The engine table entry a render target view is, or none. kFRAMEBUFFER (the window's) is left to D3D11. */
		std::optional<Target> FindColour(const ID3D11RenderTargetView* a_view)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || !a_view)
				return std::nullopt;
			const auto& targets = renderer->GetRuntimeData().renderTargets;
			for (std::uint32_t i = RE::RENDER_TARGET::kMAIN; i < RE::RENDER_TARGET::kTOTAL; ++i)
				if (targets[i].RTV == a_view)
					return Target{ Kind::Colour, i, 0 };
			const auto& cubes = renderer->GetRendererData().cubemapRenderTargets;
			for (std::uint32_t i = 0; i < RE::RENDER_TARGET_CUBEMAP::kTOTAL; ++i)
				for (std::uint32_t face = 0; face < 6; ++face)
					if (cubes[i].cubeSideRTV[face] == a_view)
						return Target{ Kind::Cube, i, face };
			return std::nullopt;
		}

		/** @brief The engine table entry a depth-stencil view is (a writable view only), or none. */
		std::optional<Target> FindDepth(const ID3D11DepthStencilView* a_view)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || !a_view)
				return std::nullopt;
			// The engine's depth table has kTOTAL entries on SE and AE, whatever the array's declared size.
			const auto& depths = renderer->GetDepthStencilData().depthStencils;
			for (std::uint32_t i = 0; i < DepthTarget::kTOTAL; ++i)
				for (std::uint32_t slice = 0; slice < 8; ++slice)
					if (depths[i].views[slice] == a_view)
						return Target{ Kind::Depth, i, slice };
			return std::nullopt;
		}

		/** @brief The graph texture of a target now (the published one when ORG owns it, else the anchors' import). */
		std::shared_ptr<org::ExternalTextureResource> ResourceOf(const Target& a_target)
		{
			switch (a_target.kind) {
			case Kind::Colour:
				return FrameAnchors::EngineTarget(a_target.index);
			case Kind::Depth:
				return FrameAnchors::EngineDepth(a_target.index);
			default:
				return FrameAnchors::EngineCubemap(a_target.index);
			}
		}

		ID3D11Texture2D* TextureOf(const Target& a_target)
		{
			auto* renderer = globals::game::renderer;
			switch (a_target.kind) {
			case Kind::Colour:
				return renderer->GetRuntimeData().renderTargets[a_target.index].texture;
			case Kind::Depth:
				return renderer->GetDepthStencilData().depthStencils[a_target.index].texture;
			default:
				return renderer->GetRendererData().cubemapRenderTargets[a_target.index].texture;
			}
		}

		bool IsIntegerFormat(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R32G32B32A32_UINT:
			case DXGI_FORMAT_R32G32B32A32_SINT:
			case DXGI_FORMAT_R32G32B32_UINT:
			case DXGI_FORMAT_R32G32B32_SINT:
			case DXGI_FORMAT_R16G16B16A16_UINT:
			case DXGI_FORMAT_R16G16B16A16_SINT:
			case DXGI_FORMAT_R32G32_UINT:
			case DXGI_FORMAT_R32G32_SINT:
			case DXGI_FORMAT_R10G10B10A2_UINT:
			case DXGI_FORMAT_R8G8B8A8_UINT:
			case DXGI_FORMAT_R8G8B8A8_SINT:
			case DXGI_FORMAT_R16G16_UINT:
			case DXGI_FORMAT_R16G16_SINT:
			case DXGI_FORMAT_R32_UINT:
			case DXGI_FORMAT_R32_SINT:
			case DXGI_FORMAT_R8G8_UINT:
			case DXGI_FORMAT_R8G8_SINT:
			case DXGI_FORMAT_R16_UINT:
			case DXGI_FORMAT_R16_SINT:
			case DXGI_FORMAT_R8_UINT:
			case DXGI_FORMAT_R8_SINT:
				return true;
			default:
				return false;
			}
		}

		bool HasStencil(DXGI_FORMAT a_format) { return a_format == DXGI_FORMAT_D24_UNORM_S8_UINT || a_format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT; }

		/** @brief The clear program for a format: colour (a_depthFlags 0) or depth with the D3D11 clear flags. Compiled once. */
		std::shared_ptr<const RasterProgram> ProgramFor(rhi::Format a_format, std::uint32_t a_depthFlags)
		{
			auto& state = Skel();
			const auto key = std::make_pair(a_format, a_depthFlags);
			if (const auto it = state.programs.find(key); it != state.programs.end())
				return it->second;
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return nullptr;
			RasterProgram::Desc desc{ .pixelSource = kShader, .vertexSource = kShader, .vertexEntry = L"VSMain", .commonDefines = false };
			if (a_depthFlags == 0) {
				desc.pixelEntry = L"PSColour";
				desc.constantWords = 4;
				desc.targets = { { .format = a_format } };
			} else {
				desc.pixelEntry = L"PSDepth";
				desc.depthFormat = a_format;
				const bool depth = (a_depthFlags & D3D11_CLEAR_DEPTH) != 0;
				desc.depth = { .depthEnable = depth, .depthWrite = depth, .depthFunc = rhi::CompareOp::Always };
				if (a_depthFlags & D3D11_CLEAR_STENCIL) {
					// Zero whatever the test: the engine clears the stencil to 0, and no reference is needed.
					const rhi::StencilFaceState zero{ .failOp = rhi::StencilOp::Zero, .depthFailOp = rhi::StencilOp::Zero, .passOp = rhi::StencilOp::Zero, .func = rhi::CompareOp::Always };
					desc.depth.stencilEnable = true;
					desc.depth.stencilReadMask = 0;
					desc.depth.stencilWriteMask = 0xFF;
					desc.depth.front = zero;
					desc.depth.back = zero;
				}
			}
			auto program = RasterProgram::Load(host->Device(), desc);
			if (!program)
				logger::warn("[ORG] engine skeleton: the clear program for format {} (flags {}) could not be made; those clears stay on D3D11",
					static_cast<int>(a_format), a_depthFlags);
			// Kept even when null: one attempt per format.
			state.programs.emplace(key, program);
			return program;
		}

		void Refuse(const Target& a_target, std::string_view a_reason)
		{
			if (Skel().refused.emplace(a_target.kind, a_target.index, a_target.slice).second)
				logger::info("[ORG] engine skeleton: {} stays on D3D11 ({})", TargetName(a_target), a_reason);
		}

		/** @brief The view's format and its array slice, from its D3D11 description; false when it is not one mip-0 slice. */
		bool ViewShape(ID3D11View* a_view, bool a_depth, DXGI_FORMAT& a_format, std::uint32_t& a_slice)
		{
			if (a_depth) {
				D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
				static_cast<ID3D11DepthStencilView*>(a_view)->GetDesc(&desc);
				a_format = desc.Format;
				if (desc.Flags != 0)
					return false;
				if (desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D) {
					a_slice = 0;
					return desc.Texture2D.MipSlice == 0;
				}
				if (desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY) {
					a_slice = desc.Texture2DArray.FirstArraySlice;
					return desc.Texture2DArray.MipSlice == 0 && desc.Texture2DArray.ArraySize == 1;
				}
				return false;
			}
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			static_cast<ID3D11RenderTargetView*>(a_view)->GetDesc(&desc);
			a_format = desc.Format;
			if (desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) {
				a_slice = 0;
				return desc.Texture2D.MipSlice == 0;
			}
			if (desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) {
				a_slice = desc.Texture2DArray.FirstArraySlice;
				return desc.Texture2DArray.MipSlice == 0 && desc.Texture2DArray.ArraySize == 1;
			}
			return false;
		}

		/** @brief The group dropped back to no entries: every pass made for textures that are gone is out at its next registration. */
		void Reset(Group a_group, std::string_view a_why)
		{
			auto& group = GroupOf(a_group);
			group.entries.clear();
			group.dirty = true;
			if (++group.resets > kMaxResets) {
				group.failed = true;
				logger::warn("[ORG] engine skeleton: {} clears were built again {} times ({}); they stay on D3D11", kGroupNames[static_cast<std::size_t>(a_group)],
					group.resets, a_why);
			} else
				logger::info("[ORG] engine skeleton: {} clears are made again ({})", kGroupNames[static_cast<std::size_t>(a_group)], a_why);
		}

		/**
		 * @brief The registered entry for a_key, or null when the caller must use D3D11 this time: a new view (made now, registered
		 * at the next flush), a view that cannot be cleared on the graph, or a group that was reset for new textures.
		 */
		const Entry* EntryFor(Group a_group, const Key& a_key, ID3D11View* a_view)
		{
			auto& group = GroupOf(a_group);
			const auto current = ResourceOf(a_key.target);
			for (const auto& entry : group.entries) {
				if (!(entry.key == a_key))
					continue;
				if (entry.spec->resource != current) {
					Reset(a_group, fmt::format("{} has a new texture", TargetName(a_key.target)));
					return nullptr;
				}
				return entry.registered ? &entry : nullptr;
			}
			const auto& target = a_key.target;
			if (Skel().refused.contains({ target.kind, target.index, target.slice }))
				return nullptr;
			const auto sameView = std::ranges::count_if(group.entries, [&](const Entry& a_entry) {
				const auto& other = a_entry.key.target;
				return other.kind == target.kind && other.index == target.index && other.slice == target.slice;
			});
			if (static_cast<std::size_t>(sameView) >= kMaxKeysPerTarget) {
				Refuse(target, "its clear value keeps changing");
				return nullptr;
			}
			if (!current) {
				Refuse(target, "no graph texture");
				return nullptr;
			}
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			std::uint32_t slice = 0;
			const bool depth = target.kind == Kind::Depth;
			if (!ViewShape(a_view, depth, format, slice) || (target.kind == Kind::Cube && slice != target.slice)) {
				Refuse(target, "not a single mip-0 slice");
				return nullptr;
			}
			if (IsIntegerFormat(format)) {
				Refuse(target, "an integer format");
				return nullptr;
			}
			auto* texture = TextureOf(target);
			if (!texture) {
				Refuse(target, "no engine texture");
				return nullptr;
			}
			D3D11_TEXTURE2D_DESC textureDesc{};
			texture->GetDesc(&textureDesc);
			const std::uint32_t flags = depth ? (a_key.depthFlags & (HasStencil(format) ? 3u : static_cast<std::uint32_t>(D3D11_CLEAR_DEPTH))) : 0u;
			if (depth && flags == 0)
				return nullptr;  // nothing a pass would clear: the D3D11 call does nothing either
			auto program = ProgramFor(rhi::helpers::ToRHI(format), flags);
			if (!program)
				return nullptr;
			if (group.nextEntry >= kGateEntries) {
				if (!std::exchange(group.failed, true))
					logger::warn("[ORG] engine skeleton: {} clears used all {} gate entries; they stay on D3D11", kGroupNames[static_cast<std::size_t>(a_group)], kGateEntries);
				return nullptr;
			}
			auto spec = std::make_shared<ClearSpec>();
			spec->name = fmt::format("{}.{}.{}", kGroupIds[static_cast<std::size_t>(a_group)], TargetName(target), group.nextEntry);
			spec->resource = current;
			spec->slice = slice;
			spec->depth = depth;
			spec->program = std::move(program);
			spec->colour = a_key.colour;
			spec->width = textureDesc.Width;
			spec->height = textureDesc.Height;
			spec->gate = group.gate;
			spec->entry = group.nextEntry++;
			logger::info("[ORG] engine skeleton: {} ({}x{}, format {}, {}) joins the {} anchor", TargetName(target), spec->width, spec->height,
				static_cast<int>(format), depth ? fmt::format("depth flags {}", flags) : std::string("colour"), FrameAnchors::Name(kGroupAnchors[static_cast<std::size_t>(a_group)]));
			group.entries.push_back({ a_key, std::move(spec), false });
			group.dirty = true;
			return nullptr;
		}

		/** @brief Registers the group's client with every entry it has; its earlier registration is replaced. */
		void Register(Group a_group)
		{
			auto& group = GroupOf(a_group);
			group.dirty = false;
			const auto index = static_cast<std::size_t>(a_group);
			if (group.entries.empty()) {
				if (std::exchange(group.registered, false))
					FrameAnchors::Unregister(kGroupIds[index]);
				return;
			}
			FrameAnchors::ClientDesc client{};
			client.id = kGroupIds[index];
			for (const auto& entry : group.entries) {
				const auto spec = entry.spec;
				client.passes.push_back({ .name = spec->name,
					.anchor = kGroupAnchors[index],
					.order = static_cast<std::int32_t>(spec->entry),
					.kind = FrameAnchors::PassKind::Render,
					.make = [spec] { return std::static_pointer_cast<org::RenderPass>(std::make_shared<ClearPass>(spec)); } });
			}
			client.active = [a_group] { return !GroupOf(a_group).pending.empty(); };
			client.commit = [a_group](const FrameAnchors::Point&) {
				const auto& state = GroupOf(a_group);
				for (const auto& pending : state.pending)
					state.gate->OpenDraw(pending.gateEntry);
			};
			client.executed = [a_group](const FrameAnchors::Point&) { GroupOf(a_group).executed = true; };
			client.onFailed = [a_group] {
				auto& state = GroupOf(a_group);
				state.failed = true;
				state.registered = false;
				logger::error("[ORG] engine skeleton: the {} anchor failed; its clears return to D3D11", FrameAnchors::Name(kGroupAnchors[static_cast<std::size_t>(a_group)]));
			};
			client.gates = { group.gate };
			group.registered = FrameAnchors::Register(std::move(client));
			if (!group.registered) {
				group.failed = true;
				logger::warn("[ORG] engine skeleton: the {} anchor refused its clears; they stay on D3D11", FrameAnchors::Name(kGroupAnchors[index]));
				return;
			}
			for (auto& entry : group.entries)
				entry.registered = true;
		}

		bool Take(const Target& a_target, const Key& a_key, ID3D11View* a_view, Pending a_pending)
		{
			const Group group = GroupFor(a_target);
			auto& state = GroupOf(group);
			if (state.failed || !state.gate)
				return false;
			const auto* entry = EntryFor(group, a_key, a_view);
			if (!entry)
				return false;
			a_pending.gateEntry = entry->spec->entry;
			state.pending.push_back(a_pending);
			return true;
		}

		void Replay(ID3D11DeviceContext* a_context, const std::vector<Pending>& a_pending)
		{
			for (const auto& pending : a_pending) {
				if (pending.depth)
					a_context->ClearDepthStencilView(static_cast<ID3D11DepthStencilView*>(pending.view), pending.flags, pending.depthValue, pending.stencil);
				else
					a_context->ClearRenderTargetView(static_cast<ID3D11RenderTargetView*>(pending.view), pending.colour.data());
			}
		}

		bool RegisterCopy(const std::shared_ptr<org::ExternalTextureResource>& a_source, const std::shared_ptr<org::ExternalTextureResource>& a_target)
		{
			auto& copy = Skel().copy;
			copy.source = a_source;
			copy.target = a_target;
			if (a_source->GetWidth() != a_target->GetWidth() || a_source->GetHeight() != a_target->GetHeight() || a_source->GetFormat() != a_target->GetFormat()) {
				copy.failed = true;
				logger::warn("[ORG] engine skeleton: kMAIN and kPOST_ZPREPASS_COPY differ in size or format; the depth copy stays on D3D11");
				return false;
			}
			AnchorPasses::Builder builder(Anchor::EngineDepthCopy, kCopyId, 0);
			builder.Add(AnchorPasses::Copy{ .name = "post-zprepass", .source = a_source, .target = a_target, .width = a_source->GetWidth(), .height = a_source->GetHeight() });
			FrameAnchors::ClientDesc client{};
			client.id = kCopyId;
			client.passes = builder.Take();
			client.active = [] { return Skel().copy.wanted; };
			client.executed = [](const FrameAnchors::Point&) { Skel().copy.executed = true; };
			client.onFailed = [] {
				auto& state = Skel().copy;
				state.failed = true;
				logger::error("[ORG] engine skeleton: the EngineDepthCopy anchor failed; the depth copy returns to D3D11");
			};
			if (!FrameAnchors::Register(std::move(client))) {
				copy.failed = true;
				logger::warn("[ORG] engine skeleton: the EngineDepthCopy anchor refused the copy; it stays on D3D11");
				return false;
			}
			return true;
		}

		/** @brief The copy on the graph at this point; false when it did not run (the caller copies). */
		bool RunCopy()
		{
			auto& copy = Skel().copy;
			if (copy.failed || !Usable())
				return false;
			auto source = FrameAnchors::EngineDepth(DepthTarget::kMAIN);
			auto target = FrameAnchors::EngineDepth(DepthTarget::kPOST_ZPREPASS_COPY);
			if (!source || !target)
				return false;
			if (source != copy.source || target != copy.target) {
				if (copy.source && ++copy.resets > kMaxResets) {
					copy.failed = true;
					logger::warn("[ORG] engine skeleton: the depth copy was built again {} times; it stays on D3D11", copy.resets);
					return false;
				}
				if (!RegisterCopy(source, target))
					return false;
			}
			copy.wanted = true;
			copy.executed = false;
			auto& state = Skel();
			state.flushing = true;
			const bool ran = FrameAnchors::Run(Anchor::EngineDepthCopy);
			state.flushing = false;
			copy.wanted = false;
			// The pass was in the submitted graph only when the executed signal came: a run of a graph built before this registration
			// had no copy in it.
			const bool done = ran && copy.executed;
			if (done && !std::exchange(copy.announced, true))
				logger::info("[ORG] engine skeleton: the main depth is copied into kPOST_ZPREPASS_COPY on the graph");
			return done;
		}
	}

	void Reserve()
	{
		if (!Requested())
			return;
		bool reserved = true;
		for (const auto anchor : { Anchor::EngineShadowClear, Anchor::EngineCubeClear, Anchor::EngineFrameClear, Anchor::EngineDepthCopy })
			reserved = FrameAnchors::Reserve(anchor) && reserved;
		if (!reserved)
			logger::warn("[ORG] engine skeleton: CS_ORG_ENGINE_SKELETON=1, but not every anchor could be reserved; those clears stay on D3D11");
	}

	void Setup()
	{
		auto& state = Skel();
		if (!Requested() || state.setUp)
			return;
		if (!state.installed) {
			logger::warn("[ORG] engine skeleton: CS_ORG_ENGINE_SKELETON=1, but its engine hooks are not installed; the engine clears with D3D11");
			return;
		}
		if (!RenderGraphRuntime::Get().IsActive()) {
			logger::warn("[ORG] engine skeleton: CS_ORG_ENGINE_SKELETON=1, but the render graph is not active; the engine clears with D3D11");
			return;
		}
		for (std::size_t i = 0; i < kGroups; ++i) {
			auto& group = state.groups[i];
			if (!FrameAnchors::HasEpoch(kGroupAnchors[i])) {
				group.failed = true;
				continue;
			}
			group.gate = RunGate::Create(kGroupIds[i], kGateEntries);
			group.failed = !group.gate;
		}
		state.copy.failed = !FrameAnchors::HasEpoch(Anchor::EngineDepthCopy);
		state.setUp = true;
		logger::info("[ORG] engine skeleton: on (CS_ORG_ENGINE_SKELETON); the engine's clears and its main depth copy join the graph as their targets appear");
	}

	bool CopyPostZPrepassDepth()
	{
		return Requested() && RunCopy();
	}

	void MarkInstalled()
	{
		Skel().installed = true;
	}

	namespace Sites
	{
		bool TakeColourClear(ID3D11DeviceContext*, ID3D11RenderTargetView* a_view, const FLOAT a_colour[4])
		{
			if (!Usable() || !a_colour)
				return false;
			const auto target = FindColour(a_view);
			if (!target)
				return false;
			Key key{ .target = *target };
			for (std::size_t i = 0; i < 4; ++i)
				key.colour[i] = std::bit_cast<std::uint32_t>(a_colour[i]);
			Pending pending{ .depth = false, .view = a_view };
			std::memcpy(pending.colour.data(), a_colour, sizeof(pending.colour));
			return Take(*target, key, a_view, pending);
		}

		bool TakeDepthClear(ID3D11DeviceContext*, ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil)
		{
			// The clear programs write depth 1 and stencil 0, the only values the engine clears to.
			if (!Usable() || ((a_flags & D3D11_CLEAR_DEPTH) && a_depth != 1.0f) || ((a_flags & D3D11_CLEAR_STENCIL) && a_stencil != 0))
				return false;
			const auto target = FindDepth(a_view);
			if (!target)
				return false;
			const Key key{ .target = *target, .depthFlags = a_flags & 3u };
			return Take(*target, key, a_view, Pending{ .depth = true, .view = a_view, .flags = a_flags, .depthValue = a_depth, .stencil = a_stencil });
		}

		void Flush(ID3D11DeviceContext* a_context)
		{
			auto& state = Skel();
			// Clears are taken on the main thread only; a loading screen's thread has none to flush.
			if (!state.setUp || state.flushing || !FrameAnchors::OnMainThread())
				return;
			for (std::size_t i = 0; i < kGroups; ++i) {
				auto& group = state.groups[i];
				if (group.failed) {
					// A group that failed with clears taken: they get the engine's calls.
					if (!group.pending.empty()) {
						Replay(a_context, group.pending);
						group.pending.clear();
					}
					continue;
				}
				if (group.dirty)
					Register(static_cast<Group>(i));
				if (group.pending.empty())
					continue;
				group.executed = false;
				state.flushing = true;
				const bool ran = group.registered && FrameAnchors::Run(kGroupAnchors[i]);
				state.flushing = false;
				// The executed signal says the run's graph held this registration's passes; without it (a graph built before the last
				// entry joined, a failed run, a call off the main thread) the engine's clears are made here, in their order.
				if (ran && group.executed) {
					group.taken += group.pending.size();
					if (!std::exchange(group.announced, true))
						logger::info("[ORG] engine skeleton: the first {} clears ran on the graph ({} anchor)", kGroupNames[i], FrameAnchors::Name(kGroupAnchors[i]));
				} else {
					group.replayed += group.pending.size();
					Replay(a_context, group.pending);
				}
				group.pending.clear();
			}
		}

		bool TakeDepthCopy(ID3D11Resource* a_destination, ID3D11Resource* a_source)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || !Requested())
				return false;
			const auto& depths = renderer->GetDepthStencilData().depthStencils;
			if (a_destination != depths[DepthTarget::kPOST_ZPREPASS_COPY].texture || a_source != depths[DepthTarget::kMAIN].texture)
				return false;
			return RunCopy();
		}
	}
}

#else  // !CS_HAS_RENDER_GRAPH

#	include "RenderGraph/Engine/Skeleton/SkeletonSites.h"

namespace RenderGraph::Engine::Skeleton
{
	void Reserve() {}
	void Setup() {}
	bool CopyPostZPrepassDepth() { return false; }
	void MarkInstalled() {}

	namespace Sites
	{
		bool TakeColourClear(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]) { return false; }
		bool TakeDepthClear(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8) { return false; }
		void Flush(ID3D11DeviceContext*) {}
		bool TakeDepthCopy(ID3D11Resource*, ID3D11Resource*) { return false; }
	}
}

#endif
