#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/AnchorPasses.h"

#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/PublishedResources.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "RenderGraph/UavClearPass.h"

#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ResourceIdentifier.h>
#	include <Resources/GloballyIndexedResource.h>

#	include <algorithm>
#	include <format>
#	include <span>

namespace RenderGraph::AnchorPasses
{
	namespace
	{
		/** @brief A pass's push words, at most ShaderBindings::kMaxPushWords. */
		struct PushWords
		{
			std::array<std::uint32_t, ShaderBindings::kMaxPushWords> data{};
			std::uint32_t count = 0;

			std::span<const std::uint32_t> Span() const { return { data.data(), count }; }
		};

		std::uint32_t WordCount(const std::vector<Bind>& a_bindings)
		{
			std::uint32_t words = 0;
			for (const auto& bind : a_bindings)
				words += bind.kind == Bind::Kind::Latch ? 2u : (bind.kind == Bind::Kind::Shared ? 4u : 1u);
			return words;
		}

		bool ReadsShared(const std::vector<Bind>& a_bindings)
		{
			return std::ranges::any_of(a_bindings, [](const Bind& a_bind) { return a_bind.kind == Bind::Kind::Shared; });
		}

		/** @brief One mip of a texture, for a pass that reads or writes that mip alone. */
		org::ResourcePtrAndRange MipOf(const std::shared_ptr<org::Resource>& a_texture, std::uint32_t a_mip)
		{
			return org::Subresources(a_texture, org::Mip(a_mip, 1));
		}

		/** @brief A pass's declared entries: a view for each Srv and Uav entry (empty for the others), and the shared buffers. */
		struct DeclaredBindings
		{
			std::vector<org::DeclaredViewToken> views;
			SharedConstants::Bindings shared;
		};

		DeclaredBindings DeclareBindings(org::PassBuilder& a_builder, const std::vector<Bind>& a_bindings)
		{
			DeclaredBindings declared;
			declared.views.resize(a_bindings.size());
			for (std::size_t i = 0; i < a_bindings.size(); ++i) {
				const auto& bind = a_bindings[i];
				const bool whole = bind.mip == kAllMips;
				if (bind.kind == Bind::Kind::Srv && bind.variant != UINT32_MAX)
					declared.views[i] = a_builder.ShaderResource(bind.resource, org::SrvView{ .variant = bind.variant }).View();
				else if (bind.kind == Bind::Kind::Srv)
					declared.views[i] = whole ? a_builder.ShaderResource(bind.resource).View() :
					                            a_builder.ShaderResource(MipOf(bind.resource, bind.mip), org::SrvView{ .mip = bind.mip }).View();
				else if (bind.kind == Bind::Kind::Uav)
					declared.views[i] = whole ? a_builder.UnorderedAccess(bind.resource).View() :
					                            a_builder.UnorderedAccess(MipOf(bind.resource, bind.mip), org::UavView{ .mip = bind.mip }).View();
				else if (bind.kind == Bind::Kind::Published)
					declared.views[i] = bind.variant != UINT32_MAX ?
					                         a_builder.ShaderResource(org::ResourceIdentifier(bind.id), org::SrvView{ .variant = bind.variant }).View() :
					                         a_builder.ShaderResource(org::ResourceIdentifier(bind.id)).View();
			}
			if (ReadsShared(a_bindings))
				declared.shared = SharedConstants::Declare(a_builder);
			return declared;
		}

		PushWords FillWords(const std::vector<Bind>& a_bindings, const DeclaredBindings& a_declared, const org::LatchBlock* a_latch,
			org::PassRecordContext& a_recording)
		{
			PushWords words;
			for (std::size_t i = 0; i < a_bindings.size(); ++i) {
				const auto& bind = a_bindings[i];
				switch (bind.kind) {
				case Bind::Kind::Srv:
				case Bind::Kind::Uav:
				case Bind::Kind::Published:
					words.data[words.count++] = a_recording.Index(a_declared.views[i]);
					break;
				case Bind::Kind::Handle:
					words.data[words.count++] = bind.value;
					break;
				case Bind::Kind::Latch:
					{
						// Builder::Check refuses a Latch entry without a latch.
						const auto latch = ShaderBindings::LatchOf(*a_latch, a_recording.Run(), bind.value);
						words.data[words.count++] = latch.handle;
						words.data[words.count++] = latch.offset;
						break;
					}
				case Bind::Kind::Shared:
					{
						const auto shared = SharedConstants::Index(a_recording, a_declared.shared);
						for (const auto handle : { shared.frameBuffer, shared.sharedData, shared.featureData, shared.permutation })
							words.data[words.count++] = handle;
						break;
					}
				}
			}
			return words;
		}

		/** @brief What a pass is made from at a build: the step and the client's latch and gate. Immutable. */
		template <class Step>
		struct Spec
		{
			Step step;
			std::shared_ptr<const org::LatchBlock> latch;
			std::shared_ptr<const RunGate> gate;
		};

		template <class Step>
		struct SpecFrame
		{
			std::shared_ptr<const Spec<Step>> spec;
		};

		struct DrawBindings
		{
			org::DeclaredViewToken target;
			std::vector<org::DeclaredViewToken> extraTargets;
			DeclaredBindings declared;
		};

		class FullscreenPass final : public org::TypedRenderGraphPass<FullscreenPass, SpecFrame<Draw>, DrawBindings>
		{
		public:
			explicit FullscreenPass(std::shared_ptr<const Spec<Draw>> a_spec) :
				spec(std::move(a_spec)) {}

			DrawBindings Declare(org::PassBuilder& a_builder)
			{
				const auto& draw = spec->step;
				auto view = draw.mip == kAllMips ? a_builder.RenderTarget(draw.target).View() :
				                                   a_builder.RenderTarget(MipOf(draw.target, draw.mip), org::RtvView{ .mip = draw.mip }).View();
				std::vector<org::DeclaredViewToken> extra;
				for (const auto& target : draw.extraTargets)
					extra.push_back(a_builder.RenderTarget(target).View());
				return { view, std::move(extra), DeclareBindings(a_builder, draw.bindings) };
			}

			// The recording depends only on what the pass was made with.
			void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.get())); }

			SpecFrame<Draw> Prepare(const DrawBindings&, const org::PassPrepareContext&) const { return { spec }; }

			static void Record(const DrawBindings& a_bindings, const SpecFrame<Draw>& a_frame, org::PassRecordContext& a_recording)
			{
				const auto& spec = *a_frame.spec;
				const auto& draw = spec.step;
				const auto words = FillWords(draw.bindings, a_bindings.declared, spec.latch.get(), a_recording);
				std::array<RasterProgram::Colour, 8> colours{};
				std::size_t colourCount = 0;
				colours[colourCount++] = { .view = &a_bindings.target, .load = draw.load, .clear = draw.clear };
				for (const auto& extra : a_bindings.extraTargets)
					if (colourCount < colours.size())
						colours[colourCount++] = { .view = &extra, .load = draw.load, .clear = draw.clear };
				RasterProgram::DrawDesc desc{ .colours = { colours.data(), colourCount }, .viewport = draw.viewport, .constants = words.Span(), .debugName = draw.name.c_str() };
				if (draw.gate) {
					desc.indirect = &spec.gate->Latch();
					desc.indirectOffset = spec.gate->Offset(*draw.gate);
				}
				draw.program->Draw(a_recording, desc);
			}

		private:
			std::shared_ptr<const Spec<Draw>> spec;
		};

		struct DispatchBindings
		{
			DeclaredBindings declared;
		};

		class DispatchPass final : public org::TypedRenderGraphPass<DispatchPass, SpecFrame<Dispatch>, DispatchBindings>
		{
		public:
			explicit DispatchPass(std::shared_ptr<const Spec<Dispatch>> a_spec) :
				spec(std::move(a_spec)) {}

			DispatchBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				return { DeclareBindings(a_builder, spec->step.bindings) };
			}

			void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.get())); }

			SpecFrame<Dispatch> Prepare(const DispatchBindings&, const org::PassPrepareContext&) const { return { spec }; }

			static void Record(const DispatchBindings& a_bindings, const SpecFrame<Dispatch>& a_frame, org::PassRecordContext& a_recording)
			{
				const auto& spec = *a_frame.spec;
				const auto& dispatch = spec.step;
				const auto words = FillWords(dispatch.bindings, a_bindings.declared, spec.latch.get(), a_recording);
				if (dispatch.gate)
					spec.gate->Dispatch(a_recording, *dispatch.program, words.Span(), *dispatch.gate);
				else if (dispatch.latchArgs)
					dispatch.program->DispatchIndirect(a_recording, words.Span(), *spec.latch, *dispatch.latchArgs);
				else
					dispatch.program->Dispatch(a_recording, words.Span(), dispatch.groups[0], dispatch.groups[1], dispatch.groups[2]);
			}

		private:
			std::shared_ptr<const Spec<Dispatch>> spec;
		};

		struct CopyBindings
		{
			org::ResourceBindingToken source, target;
		};

		class CopyPass final : public org::TypedRenderGraphPass<CopyPass, SpecFrame<Copy>, CopyBindings>
		{
		public:
			explicit CopyPass(std::shared_ptr<const Spec<Copy>> a_spec) :
				spec(std::move(a_spec)) {}

			CopyBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				return { a_builder.CopySource(spec->step.source), a_builder.CopyDestination(spec->step.target) };
			}

			void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.get())); }

			SpecFrame<Copy> Prepare(const CopyBindings&, const org::PassPrepareContext&) const { return { spec }; }

			static void Record(const CopyBindings& a_bindings, const SpecFrame<Copy>& a_frame, org::PassRecordContext& a_recording)
			{
				const auto& copy = a_frame.spec->step;
				const rhi::TextureCopyRegion target{ .texture = a_recording.Resolve(a_bindings.target).GetHandle(), .width = copy.width, .height = copy.height };
				const rhi::TextureCopyRegion source{ .texture = a_recording.Resolve(a_bindings.source).GetHandle(), .width = copy.width, .height = copy.height };
				a_recording.Commands().CopyTextureRegion(target, source);
			}

		private:
			std::shared_ptr<const Spec<Copy>> spec;
		};

		struct UploadBindings
		{
			org::ResourceBindingToken target;
		};

		class UploadPass final : public org::TypedRenderGraphPass<UploadPass, SpecFrame<Upload>, UploadBindings>
		{
		public:
			explicit UploadPass(std::shared_ptr<const Spec<Upload>> a_spec) :
				spec(std::move(a_spec)) {}

			UploadBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				return { a_builder.CopyDestination(spec->step.target) };
			}

			void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.get())); }

			SpecFrame<Upload> Prepare(const UploadBindings&, const org::PassPrepareContext&) const { return { spec }; }

			static void Record(const UploadBindings& a_bindings, const SpecFrame<Upload>& a_frame, org::PassRecordContext& a_recording)
			{
				const auto& upload = a_frame.spec->step;
				rhi::BufferTextureCopyFootprint copy{};
				copy.texture = a_recording.Resolve(a_bindings.target).GetHandle();
				copy.buffer = upload.staging->GetAPIResource().GetHandle();
				copy.mip = upload.mip;
				copy.arraySlice = upload.slice;
				copy.footprint = upload.footprint;
				a_recording.Commands().CopyBufferToTexture(copy);
			}

		private:
			std::shared_ptr<const Spec<Upload>> spec;
		};
	}

	Bind Bind::SrvArray(std::shared_ptr<org::Resource> a_resource)
	{
		Bind bind = Srv(std::move(a_resource));
		bind.variant = static_cast<std::uint32_t>(org::SRVViewType::Texture2DArrayFull);
		return bind;
	}

	Builder::Builder(FrameAnchors::Anchor a_anchor, std::string a_prefix, std::int32_t a_orderBase, std::shared_ptr<const org::LatchBlock> a_latch,
		std::shared_ptr<const RunGate> a_gate) :
		anchor(a_anchor), prefix(std::move(a_prefix)), orderBase(a_orderBase), latch(std::move(a_latch)), gate(std::move(a_gate))
	{}

	bool Builder::Check(std::string_view a_name, const std::vector<Bind>& a_bindings, std::uint32_t a_programWords, std::optional<std::uint32_t> a_gate)
	{
		for (const auto& bind : a_bindings) {
			if ((bind.kind == Bind::Kind::Srv || bind.kind == Bind::Kind::Uav) && !bind.resource) {
				Fail(std::format("{}: an input is missing", a_name));
				return false;
			}
			if (bind.kind == Bind::Kind::Published && !PublishedResources::IsDeclarable(bind.id)) {
				Fail(std::format("{}: {} is not published", a_name, bind.id));
				return false;
			}
			if (bind.kind == Bind::Kind::Latch && !latch) {
				Fail(std::format("{}: a latch entry without the client's latch", a_name));
				return false;
			}
		}
		if (const auto words = WordCount(a_bindings); words != a_programWords) {
			Fail(std::format("{}: {} push words, the program takes {}", a_name, words, a_programWords));
			return false;
		}
		if (a_gate && (!gate || *a_gate >= gate->Entries())) {
			Fail(std::format("{}: gate entry {} is not one of the client's gate", a_name, *a_gate));
			return false;
		}
		sharedConstants = sharedConstants || ReadsShared(a_bindings);
		return true;
	}

	void Builder::Push(std::string_view a_step, FrameAnchors::PassKind a_kind, std::function<std::shared_ptr<org::RenderPass>()> a_make, bool a_once)
	{
		passes.push_back({ .name = std::format("{}.{}", prefix, a_step),
			.anchor = anchor,
			.order = orderBase + static_cast<std::int32_t>(passes.size()),
			.kind = a_kind,
			.make = std::move(a_make),
			.once = a_once });
	}

	void Builder::Add(Draw a_draw)
	{
		if (!a_draw.program || !a_draw.target || std::ranges::any_of(a_draw.extraTargets, [](const auto& a_target) { return !a_target; })) {
			Fail(std::format("{}: a program or target is missing", a_draw.name));
			return;
		}
		if (!Check(a_draw.name, a_draw.bindings, a_draw.program->constantWords, a_draw.gate))
			return;
		const std::string step = a_draw.name;
		a_draw.name = std::format("{}.{}", prefix, step);
		auto spec = std::make_shared<const Spec<Draw>>(Spec<Draw>{ std::move(a_draw), latch, gate });
		Push(step, FrameAnchors::PassKind::Render, [spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<FullscreenPass>(spec); }, false);
	}

	void Builder::Add(Dispatch a_dispatch)
	{
		if (!a_dispatch.program) {
			Fail(std::format("{}: a program is missing", a_dispatch.name));
			return;
		}
		if (!Check(a_dispatch.name, a_dispatch.bindings, a_dispatch.program->constantWords, a_dispatch.gate))
			return;
		if (a_dispatch.latchArgs && !latch) {
			Fail(std::format("{}: indirect group counts without the client's latch", a_dispatch.name));
			return;
		}
		const std::string step = a_dispatch.name;
		auto spec = std::make_shared<const Spec<Dispatch>>(Spec<Dispatch>{ std::move(a_dispatch), latch, gate });
		Push(step, FrameAnchors::PassKind::Compute, [spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<DispatchPass>(spec); }, false);
	}

	void Builder::Add(Copy a_copy)
	{
		if (!a_copy.source || !a_copy.target || !a_copy.width || !a_copy.height) {
			Fail(std::format("{}: the copy's textures or size are missing", a_copy.name));
			return;
		}
		const std::string step = a_copy.name;
		auto spec = std::make_shared<const Spec<Copy>>(Spec<Copy>{ std::move(a_copy), latch, gate });
		Push(step, FrameAnchors::PassKind::Copy, [spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<CopyPass>(spec); }, false);
	}

	void Builder::Add(Clear a_clear, bool a_once)
	{
		if (!a_clear.target) {
			Fail(std::format("{}: the texture to clear is missing", a_clear.name));
			return;
		}
		const std::string step = a_clear.name;
		auto target = a_clear.target;
		const auto value = a_clear.value;
		const auto uintValue = a_clear.uintValue;
		Push(step, FrameAnchors::PassKind::Compute, [target, value, uintValue]() {
			return uintValue ? GraphPasses::MakeUavClearUint(target, *uintValue) : GraphPasses::MakeUavClear(target, value);
		}, a_once);
	}

	void Builder::Add(Upload a_upload)
	{
		if (!a_upload.staging || !a_upload.target) {
			Fail(std::format("{}: the staging buffer or the texture is missing", a_upload.name));
			return;
		}
		const std::string step = a_upload.name;
		auto spec = std::make_shared<const Spec<Upload>>(Spec<Upload>{ std::move(a_upload), latch, gate });
		Push(step, FrameAnchors::PassKind::Copy, [spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<UploadPass>(spec); }, true);
	}

	void Builder::Add(std::string_view a_name, FrameAnchors::PassKind a_kind, std::function<std::shared_ptr<org::RenderPass>()> a_make, bool a_once)
	{
		if (!a_make) {
			Fail(std::format("{}: no pass", a_name));
			return;
		}
		Push(a_name, a_kind, std::move(a_make), a_once);
	}

	void Builder::Fail(std::string_view a_reason)
	{
		if (failure.empty())
			failure = std::format("{}: {}", prefix, a_reason);
	}

	std::vector<std::pair<std::wstring, std::wstring>> Defines(const std::vector<std::pair<std::string, std::string>>& a_defines)
	{
		std::vector<std::pair<std::wstring, std::wstring>> wide;
		for (const auto& [name, value] : a_defines)
			wide.emplace_back(std::wstring(name.begin(), name.end()), std::wstring(value.begin(), value.end()));
		return wide;
	}
}

#endif
