#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "DrawPipelines.h"
#	include "DrawPipelinesRhi.h"
#	include "Switches.h"

#	include "RenderGraph/RenderGraphRuntime.h"
#	include "SpirvReflection.h"
#	include "VertexInput.h"

#	include <OpenRenderGraph/PersistentGraphHost.h>
#	include <ORGModuleServices/PipelineService.h>
#	include <rhi_helpers.h>

#	include <future>
#	include <stdexcept>

namespace DCLF
{
	namespace
	{
		constexpr std::uint32_t kMaxInFlight = 4;  // pipeline builds running at once (each is a thread)
		constexpr std::uint32_t kMaxLoggedFailures = 8;

		// Register classes as shifted by the SPIR-V builds (ShaderPrograms.h); set 0.
		// Push data. The DCLF_BINDLESS builds declare a cbuffer here to read the object index out of it;
		// the address words beside it are still consumed by the layout's indirect ranges, not by a shader.
		constexpr std::uint32_t kRecordAddressBinding = 190;
		// The one texture register the vertex stage may declare: the per-object record buffer.
		constexpr std::uint32_t kObjectBufferBinding = kBindingShiftT + kObjectBufferRegister;
		constexpr std::uint32_t kDescriptorHeapBindings = 1000000;  // DXC's ResourceDescriptorHeap bindings (BasicRHI maps them)

		bool InRange(std::uint32_t a_binding, std::uint32_t a_first, std::uint32_t a_count)
		{
			return a_binding >= a_first && a_binding < a_first + a_count;
		}

		const char* KindName(SpirvReflection::BindingKind a_kind)
		{
			switch (a_kind) {
			case SpirvReflection::BindingKind::ConstantBuffer:
				return "constant buffer";
			case SpirvReflection::BindingKind::Texture:
				return "texture";
			case SpirvReflection::BindingKind::Sampler:
				return "sampler";
			case SpirvReflection::BindingKind::StorageBuffer:
				return "storage buffer";
			default:
				return "resource";
			}
		}

		// Every binding a stage declares must be one the layout maps for that stage.
		void CheckBindings(const SpirvReflection& a_module, bool a_pixel)
		{
			using Kind = SpirvReflection::BindingKind;
			for (const auto& binding : a_module.bindings) {
				if (binding.binding >= kDescriptorHeapBindings)
					continue;
				bool mapped = binding.set == 0;
				switch (binding.kind) {
				case Kind::ConstantBuffer:
					mapped = mapped && (InRange(binding.binding, kBindingShiftB, kConstantBufferRegisters) || binding.binding == kRecordAddressBinding);
					break;
				case Kind::Texture:
				case Kind::StorageBuffer:
					// The layout maps the whole t range for the pixel stage, and the object buffer alone for
					// the vertex stage - which is all the vertex half of DCLF_BINDLESS needs.
					mapped = mapped && (a_pixel ? InRange(binding.binding, kBindingShiftT, kTextureRegisters) : binding.binding == kObjectBufferBinding);
					break;
				case Kind::Sampler:
					mapped = mapped && a_pixel && InRange(binding.binding, kBindingShiftS, kSamplerRegisters);
					break;
				default:
					mapped = false;
					break;
				}
				if (!mapped)
					throw std::runtime_error(fmt::format("{} {} at set {} binding {} is outside the DCLF layout", a_pixel ? "pixel" : "vertex", KindName(binding.kind), binding.set,
						binding.binding));
			}
		}

		void AddUsage(const SpirvReflection& a_module, bool a_pixel, RegisterUsage& a_usage)
		{
			using Kind = SpirvReflection::BindingKind;
			for (const auto& binding : a_module.bindings) {
				if (binding.binding >= kDescriptorHeapBindings)
					continue;
				if (binding.binding == kRecordAddressBinding)
					continue;  // push data, not a DrawBindings entry (and 1u << 190 is not a shift)
				switch (binding.kind) {
				case Kind::ConstantBuffer:
					(a_pixel ? a_usage.pixelConstants : a_usage.vertexConstants) |= 1u << (binding.binding - kBindingShiftB);
					break;
				case Kind::Texture:
				case Kind::StorageBuffer:
					{
						const std::uint32_t t = binding.binding - kBindingShiftT;
						a_usage.textures[t / 64] |= 1ull << (t % 64);
					}
					break;
				case Kind::Sampler:
					a_usage.samplers |= 1u << (binding.binding - kBindingShiftS);
					break;
				default:
					break;
				}
			}
		}

		bool SameSemantic(std::string_view a_left, std::string_view a_right)
		{
			return a_left.size() == a_right.size() && std::equal(a_left.begin(), a_left.end(), a_right.begin(), [](char a, char b) { return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b)); });
		}

		// The vertex shader's input mask, as ShaderCache derives BSGraphics::VertexShader::vertexDesc.
		std::uint64_t InputMask(const SpirvReflection& a_vertex)
		{
			std::uint64_t mask = 0b1111;
			bool texcoord2 = false, texcoord3 = false;
			for (const auto& input : a_vertex.inputs) {
				const auto& name = input.semantic;
				const auto index = input.semanticIndex;
				if (SameSemantic(name, "POSITION") && index == 0)
					AddVertexAttribute(mask, 0);
				else if (SameSemantic(name, "TEXCOORD") && index <= 1)
					AddVertexAttribute(mask, 1 + index);
				else if (SameSemantic(name, "NORMAL") && index == 0)
					AddVertexAttribute(mask, 3);
				else if (SameSemantic(name, "BINORMAL") && index == 0)
					AddVertexAttribute(mask, 4);
				else if (SameSemantic(name, "COLOR") && index == 0)
					AddVertexAttribute(mask, 5);
				else if (SameSemantic(name, "BLENDWEIGHT") && index == 0)
					AddVertexAttribute(mask, 6);
				else if (SameSemantic(name, "TEXCOORD") && index >= 4 && index <= 7)
					AddVertexAttribute(mask, 9);
				else if (SameSemantic(name, "TEXCOORD") && index == 2)
					texcoord2 = true;
				else if (SameSemantic(name, "TEXCOORD") && index == 3)
					texcoord3 = true;
			}
			if (texcoord2)
				AddVertexAttribute(mask, texcoord3 ? 7 : 8);
			return mask;
		}

		rhi::FinalizedInputLayout BuildInputLayout(const SpirvReflection& a_vertex, std::uint64_t a_vertexLayout)
		{
			const auto elements = BuildVertexElements(InputMask(a_vertex) & a_vertexLayout);
			rhi::FinalizedInputLayout layout;
			// The stride is dynamic (set per draw by the VertexBuffer argument).
			layout.bindings.push_back(rhi::InputBindingDesc{ 0, 16, rhi::InputRate::PerVertex, 1 });
			for (const auto& input : a_vertex.inputs) {
				const VertexElement* element = nullptr;
				for (std::uint32_t i = 0; i < elements.count && !element; ++i) {
					if (SameSemantic(elements.elements[i].semantic, input.semantic) && elements.elements[i].semanticIndex == input.semanticIndex)
						element = &elements.elements[i];
				}
				if (!element)
					throw std::runtime_error(fmt::format("vertex input {}{} is not in the geometry's layout {:016X}", input.semantic, input.semanticIndex, a_vertexLayout));
				if (element->slot != 0 || element->perInstance)
					throw std::runtime_error(fmt::format("vertex input {}{} comes from stream {} (only static geometry's single stream is supported)", input.semantic,
						input.semanticIndex, element->slot));
				layout.attributes.push_back(rhi::InputAttributeDesc{ 0, element->offset, rhi::helpers::ToRHI(element->format), element->semantic, element->semanticIndex, input.location });
			}
			return layout;
		}
	}

	struct DrawPipelines::Impl
	{
		struct Entry
		{
			std::shared_future<org::services::PipelineArtifact> future;
			std::uint32_t index = kNotReady;
			bool failed = false;
		};

		rhi::Device device{};
		rhi::PipelineLayoutPtr layout;
		bool supported = false;
		bool attempted = false;
		org::services::PipelineService service;
		ankerl::unordered_dense::map<PipelineKey, Entry, PipelineKeyHash> entries;
		// What a build produces: the pipelines of both variants and the registers the shaders read.
		struct Built
		{
			std::array<rhi::PipelinePtr, kVariantCount> pipelines;
			// Per variant: the Z-prepass build of the pixel stage reads far less than the colour one, and a
			// draw only has to supply what its own variant declares.
			std::array<RegisterUsage, kVariantCount> usage;
		};

		// One set per variant; a key has the same index in both.
		std::array<rhi::IndirectPipelineSetPtr, kVariantCount> sets;
		std::array<rhi::CommandSignaturePtr, kVariantCount> signatures;
		std::vector<org::services::PipelinePayload> setPipelines;  // index -> Built
		std::uint32_t inFlight = 0;
		std::uint32_t loggedFailures = 0;

		bool Initialize()
		{
			if (layout)
				return true;
			if (attempted || !RenderGraphRuntime::Get().IsActive() || !RenderGraphRuntime::Get().Host())
				return false;
			attempted = true;
			device = RenderGraphRuntime::Get().Host()->GetDesc().device;

			::IndirectCommandsFeatureInfo indirect{};
			supported = device.QueryFeatureInfo(&indirect.header) == rhi::Result::Ok && indirect.pipelineSets && indirect.indexBufferArguments &&
			            indirect.vertexBufferArguments && indirect.indirectBindings && indirect.maxPipelineSetCount >= kMaxPipelines;
			if (!supported) {
				logger::warn("[DCLF] Indirect pipelines unavailable: pipeline sets {}, index buffers {}, vertex buffers {}, indirect bindings {}, up to {} pipelines",
					indirect.pipelineSets, indirect.indexBufferArguments, indirect.vertexBufferArguments, indirect.indirectBindings, indirect.maxPipelineSetCount);
				return false;
			}

			rhi::PushConstantRangeDesc recordAddress{};
			recordAddress.visibility = rhi::ShaderStage::AllGraphics;
			recordAddress.num32BitValues = 3;  // DrawBindings address (2) + the object index
			recordAddress.set = 0;
			recordAddress.binding = kRecordAddressBinding;

			auto range = [](std::uint32_t a_binding, std::uint32_t a_count, rhi::ShaderStage a_stage, rhi::LayoutRangeSource a_source, std::size_t a_offset, bool a_samplers = false) {
				rhi::LayoutBindingRange range{};
				range.set = 0;
				range.binding = a_binding;
				range.count = a_count;
				range.visibility = a_stage;
				range.source = a_source;
				range.recordOffset = static_cast<std::uint32_t>(a_offset);
				range.samplers = a_samplers;
				return range;
			};
			rhi::LayoutBindingRange ranges[] = {
				range(kBindingShiftB, kConstantBufferRegisters, rhi::ShaderStage::Vertex, rhi::LayoutRangeSource::IndirectAddress, offsetof(DrawBindings, vertexConstants)),
				range(kBindingShiftB, kConstantBufferRegisters, rhi::ShaderStage::Pixel, rhi::LayoutRangeSource::IndirectAddress, offsetof(DrawBindings, pixelConstants)),
				range(kBindingShiftT, kTextureRegisters, rhi::ShaderStage::Pixel, rhi::LayoutRangeSource::IndirectIndex, offsetof(DrawBindings, textures)),
				range(kBindingShiftS, kSamplerRegisters, rhi::ShaderStage::Pixel, rhi::LayoutRangeSource::IndirectIndex, offsetof(DrawBindings, samplers), true),
				// The vertex stage sees one texture register, the per-object record buffer, reading the same
				// DrawBindings entry the pixel stage does.
				range(kObjectBufferBinding, 1, rhi::ShaderStage::Vertex, rhi::LayoutRangeSource::IndirectIndex,
					offsetof(DrawBindings, textures) + 4 * std::size_t{ kObjectBufferRegister }),
			};
			const rhi::PipelineLayoutDesc desc{ .ranges = { ranges, static_cast<std::uint32_t>(std::size(ranges)) }, .pushConstants = { &recordAddress, 1 },
				.staticSamplers = {}, .flags = rhi::PF_AllowInputAssembler };
			if (device.CreatePipelineLayout(desc, layout) != rhi::Result::Ok) {
				supported = false;
				logger::error("[DCLF] Could not create the indirect draw pipeline layout");
				return false;
			}
			return true;
		}

		static org::services::PipelinePayload Build(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, PipelineKey a_key, const ShaderPrograms::Program* a_program,
			TargetFormats a_targets)
		{
			SpirvReflection vertex, pixel, depthPixel;
			if (!vertex.Parse(a_program->vertex) || !pixel.Parse(a_program->pixel) || !depthPixel.Parse(a_program->depthPixel))
				throw std::runtime_error("not SPIR-V");
			CheckBindings(vertex, false);
			CheckBindings(pixel, true);
			CheckBindings(depthPixel, true);
			auto built = std::make_shared<Built>();
			AddUsage(vertex, false, built->usage[kColorVariant]);
			AddUsage(pixel, true, built->usage[kColorVariant]);
			AddUsage(vertex, false, built->usage[kDepthVariant]);
			AddUsage(depthPixel, true, built->usage[kDepthVariant]);

			const rhi::SubobjLayout layout{ a_layout };
			const rhi::SubobjShader vertexShader{ rhi::ShaderStage::Vertex, { a_program->vertex.data(), static_cast<std::uint32_t>(a_program->vertex.size()) }, "main" };
			const rhi::SubobjShader pixelShader{ rhi::ShaderStage::Pixel, { a_program->pixel.data(), static_cast<std::uint32_t>(a_program->pixel.size()) }, "main" };
			const rhi::SubobjShader depthPixelShader{ rhi::ShaderStage::Pixel, { a_program->depthPixel.data(), static_cast<std::uint32_t>(a_program->depthPixel.size()) }, "main" };
			const rhi::SubobjDSV depthFormat{ rhi::helpers::ToRHI(a_targets.depth) };
			const rhi::SubobjPrimitiveTopology topology{ rhi::PrimitiveTopology::TriangleList };
			const rhi::SubobjInputLayout input{ BuildInputLayout(vertex, a_key.vertexLayout) };
			const rhi::SubobjFlags flags{ rhi::PipelineFlags_IndirectBindable };
			for (std::uint32_t variant = 0; variant < kVariantCount; ++variant) {
				const bool depthOnly = variant == kDepthVariant;
				rhi::SubobjRaster raster{};
				raster.rs.cull = (a_key.rasterFlags & kRasterTwoSided) ? rhi::CullMode::None : rhi::CullMode::Back;
				// frontCCW stays false: the engine's meshes are wound for D3D's "clockwise is front", which
				// is what RasterState's default means on every backend.
				rhi::SubobjDepth depth{};
				// CS_DCLF_NO_DEPTH_TEST=1: the colour variant stops testing depth, to tell "the depth test
				// rejects every fragment" apart from "the draws are not reaching the rasteriser at all".
				static const bool noDepthTest = SwitchEnabled("CS_DCLF_NO_DEPTH_TEST");
				depth.ds.depthEnable = !(noDepthTest && !depthOnly);
				// The depth variant is DCLF's own Z-prepass. The colour variant tests LESS_EQUAL against it
				// rather than the EQUAL the native main pass uses. EQUAL is the engine's way of avoiding
				// overdraw when the same pass wrote the depth; here the two are separate epochs assembled
				// from separate captures, so a value can differ in its last bits and EQUAL then rejects the
				// fragment outright - which left DCLF's objects unshaded and grey. LESS_EQUAL gives the same
				// visibility, because the prepass already stored the nearest surface and anything behind it
				// still fails, and it tolerates the last-bit differences.
				// CS_DCLF_COLOUR_DEPTH_WRITE=1: the colour variant stops testing and writes its own depth, so
				// a readback says exactly what it computes for a pixel - the number its depth test compares
				// against the one the Z-prepass stored.
				static const bool colourWritesDepth = SwitchEnabled("CS_DCLF_COLOUR_DEPTH_WRITE");
				if (!depthOnly && colourWritesDepth) {
					depth.ds.depthWrite = true;
					depth.ds.depthFunc = rhi::CompareOp::Always;
				} else {
					depth.ds.depthWrite = depthOnly;
					depth.ds.depthFunc = depthOnly ? rhi::CompareOp::Less : rhi::CompareOp::LessEqual;
				}
				rhi::SubobjBlend blend{};
				rhi::SubobjRTVs targets{};
				if (!depthOnly) {
					blend.bs.numAttachments = a_targets.colorCount;
					targets.rt.count = a_targets.colorCount;
					for (std::uint32_t i = 0; i < a_targets.colorCount; ++i)
						targets.rt.formats[i] = rhi::helpers::ToRHI(a_targets.colors[i]);
				} else {
					blend.bs.numAttachments = 0;
				}
				const rhi::PipelineStreamItem items[] = {
					rhi::Make(layout), rhi::Make(vertexShader), rhi::Make(depthOnly ? depthPixelShader : pixelShader), rhi::Make(raster), rhi::Make(depth), rhi::Make(blend),
					rhi::Make(targets), rhi::Make(depthFormat), rhi::Make(topology), rhi::Make(input), rhi::Make(flags),
				};
				if (const auto result = a_device.CreatePipeline(items, static_cast<std::uint32_t>(std::size(items)), built->pipelines[variant]); result != rhi::Result::Ok)
					throw std::runtime_error(fmt::format("CreatePipeline ({}) failed ({})", depthOnly ? "depth" : "color", static_cast<int>(result)));
			}
			return built;
		}

		// The command signature of DrawSequence (Records.h), created with the set it selects from.
		bool CreateSignature(std::uint32_t a_variant)
		{
			rhi::IndirectArg args[5]{};
			args[0].kind = rhi::IndirectArgKind::PipelineIndex;
			args[1].kind = rhi::IndirectArgKind::Constant;
			args[1].u.rootConstants = { 0, 0, 3 };  // DrawBindings address + object index -> the layout's push data
			args[2].kind = rhi::IndirectArgKind::VertexBuffer;
			args[2].u.vertexBuffer.slot = 0;
			args[3].kind = rhi::IndirectArgKind::IndexBuffer;
			args[4].kind = rhi::IndirectArgKind::DrawIndexed;
			rhi::CommandSignatureDesc desc{};
			desc.args = { args, 5 };
			desc.byteStride = sizeof(DrawSequence);
			desc.pipelineSet = sets[a_variant]->GetHandle();
			return device.CreateCommandSignature(desc, layout->GetHandle(), signatures[a_variant]) == rhi::Result::Ok;
		}
	};

	DrawPipelines::DrawPipelines() :
		impl(std::make_unique<Impl>())
	{}

	DrawPipelines::~DrawPipelines() = default;

	DrawPipelines& DrawPipelines::Get()
	{
		static DrawPipelines pipelines;
		return pipelines;
	}

	bool DrawPipelines::Enabled() const
	{
		return impl->Initialize() && impl->supported;
	}

	void DrawPipelines::SetTargetFormats(const TargetFormats& a_formats)
	{
		// A draw with nothing bound (seen while the game shuts down) is not the main pass.
		if (a_formats == targets || a_formats.colorCount == 0 || a_formats.depth == DXGI_FORMAT_UNKNOWN)
			return;
		if (HasTargetFormats()) {
			// Every pipeline depends on the targets: start over (in-flight builds finish and are dropped).
			auto describe = [](const TargetFormats& a_formats) {
				std::string text;
				for (std::uint32_t i = 0; i < a_formats.colorCount; ++i)
					text += fmt::format("{} ", static_cast<int>(a_formats.colors[i]));
				return text + fmt::format("depth {}", static_cast<int>(a_formats.depth));
			};
			logger::info("[DCLF] Main pass targets changed ({} -> {}); rebuilding {} indirect pipelines", describe(targets), describe(a_formats), impl->setPipelines.size());
			++stats.targetChanges;
			impl->entries.clear();
			for (std::uint32_t variant = 0; variant < kVariantCount; ++variant) {
				impl->signatures[variant].Reset();
				impl->sets[variant].Reset();
			}
			impl->setPipelines.clear();
			usage.clear();
			++generation;
			impl->inFlight = 0;
			stats.requested = stats.ready = stats.failed = 0;
		}
		targets = a_formats;
	}

	std::uint32_t DrawPipelines::Find(const PipelineKey& a_key, const ShaderPrograms::Program& a_program)
	{
		if (!HasTargetFormats() || !Enabled())
			return kNotReady;
		if (auto it = impl->entries.find(a_key); it != impl->entries.end())
			return it->second.index;
		if (impl->inFlight >= kMaxInFlight || impl->setPipelines.size() >= kMaxPipelines)
			return kNotReady;  // asked again next frame

		org::services::PipelineRecipe recipe;
		recipe.id = fmt::format("dclf.lighting.{:08X}.{:08X}.{:08X}.{:X}.{:016X}", a_key.vertexDescriptor, a_key.pixelDescriptor, a_key.passDescriptor, a_key.rasterFlags,
			a_key.vertexLayout);
		recipe.shaderKey = PipelineKeyHash{}(a_key);
		recipe.fixedFunctionKey = ankerl::unordered_dense::detail::wyhash::hash(&targets, sizeof(targets));
		recipe.build = [device = impl->device, layout = impl->layout->GetHandle(), key = a_key, program = &a_program, formats = targets] {
			return Impl::Build(device, layout, key, program, formats);
		};
		auto& entry = impl->entries[a_key];
		entry.future = impl->service.Request(std::move(recipe));
		++impl->inFlight;
		++stats.requested;
		return kNotReady;
	}

	void DrawPipelines::Update()
	{
		if (!impl->supported)
			return;
		for (auto& [key, entry] : impl->entries) {
			if (entry.index != kNotReady || entry.failed || !entry.future.valid() || entry.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
				continue;
			--impl->inFlight;
			const auto& artifact = entry.future.get();
			entry.future = {};
			if (!artifact) {
				entry.failed = true;
				++stats.failed;
				if (impl->loggedFailures++ < kMaxLoggedFailures)
					logger::warn("[DCLF] Indirect pipeline VS {:08X} PS {:08X} layout {:016X} failed: {}", key.vertexDescriptor, key.pixelDescriptor, key.vertexLayout, artifact.error);
				continue;
			}
			const auto* built = static_cast<const Impl::Built*>(artifact.payload.get());
			const auto index = static_cast<std::uint32_t>(impl->setPipelines.size());
			rhi::Result result = rhi::Result::Ok;
			for (std::uint32_t variant = 0; variant < kVariantCount && result == rhi::Result::Ok; ++variant) {
				const auto pipeline = built->pipelines[variant]->GetHandle();
				auto& set = impl->sets[variant];
				if (!set) {
					result = impl->device.CreateIndirectPipelineSet(rhi::IndirectPipelineSetDesc{ pipeline, kMaxPipelines }, set);
					if (result == rhi::Result::Ok) {
						set->SetName(variant == kDepthVariant ? "DCLF indirect pipelines (depth)" : "DCLF indirect pipelines");
						if (!impl->CreateSignature(variant)) {
							logger::error("[DCLF] Could not create the indirect draw command signature");
							set.Reset();
							result = rhi::Result::Failed;
						}
					}
				} else {
					result = impl->device.UpdateIndirectPipelineSet(set->GetHandle(), index, { &pipeline, 1 });
				}
			}
			if (result != rhi::Result::Ok) {
				entry.failed = true;
				++stats.failed;
				logger::warn("[DCLF] Could not add indirect pipeline {} to the set ({})", index, static_cast<int>(result));
				continue;
			}
			impl->setPipelines.push_back(artifact.payload);
			usage.push_back(built->usage);
			entry.index = index;
			++stats.ready;
		}
		impl->service.PublishReady(0);
	}

	IndirectState GetIndirectState()
	{
		const auto& impl = *DrawPipelines::Get().impl;
		IndirectState state;
		for (std::uint32_t variant = 0; variant < kVariantCount; ++variant) {
			if (!impl.sets[variant] || !impl.signatures[variant])
				return state;
			state.sets[variant] = impl.sets[variant]->GetHandle();
			state.signatures[variant] = impl.signatures[variant]->GetHandle();
		}
		state.layout = impl.layout->GetHandle();
		state.valid = true;
		return state;
	}
}

#else  // no render graph

#	include "DrawPipelines.h"

namespace DCLF
{
	struct DrawPipelines::Impl
	{};

	DrawPipelines::DrawPipelines() = default;
	DrawPipelines::~DrawPipelines() = default;

	DrawPipelines& DrawPipelines::Get()
	{
		static DrawPipelines pipelines;
		return pipelines;
	}

	bool DrawPipelines::Enabled() const { return false; }
	void DrawPipelines::SetTargetFormats(const TargetFormats& a_formats) { targets = a_formats; }
	std::uint32_t DrawPipelines::Find(const PipelineKey&, const ShaderPrograms::Program&) { return kNotReady; }
	void DrawPipelines::Update() {}
}

#endif
