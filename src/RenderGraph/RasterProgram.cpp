#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/RasterProgram.h"

#	include "RenderGraph/RenderGraphRuntime.h"

#	include <Render/LatchBlock.h>
#	include <Render/PreparedPass.h>
#	include <Render/ResourceUseDeclaration.h>
#	include <Resources/Buffers/Buffer.h>

#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
#		include "RenderGraph/ProgramBuild.h"

#		include <ORGModuleServices/PipelineRegistry.h>
#		include <ORGModuleServices/ShaderCompiler.h>
#	endif

#	include <filesystem>

namespace
{
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	namespace Build = RenderGraph::ProgramBuild;

	// The state Load builds the pipeline from, kept for the registry's rebuilds.
	struct PipelineState
	{
		rhi::RasterState raster{};
		rhi::DepthStencilState depth{};
		rhi::BlendState blend{};
		rhi::RenderTargets targets{};
		rhi::Format depthFormat = rhi::Format::Unknown;
	};

	PipelineState StateOf(const RasterProgram::Desc& a_desc)
	{
		PipelineState state{};
		state.raster = a_desc.raster;
		state.depth = a_desc.depth;
		state.depthFormat = a_desc.depthFormat;
		state.blend.alphaToCoverage = a_desc.alphaToCoverage;
		state.blend.numAttachments = static_cast<std::uint32_t>(a_desc.targets.size());
		state.targets.count = static_cast<std::uint32_t>(a_desc.targets.size());
		for (std::size_t i = 0; i < a_desc.targets.size(); ++i) {
			state.targets.formats[i] = a_desc.targets[i].format;
			state.blend.attachments[i] = a_desc.targets[i].blend;
			// Independent blending only when the targets differ: one state for all is what every backend supports.
			const auto& a = state.blend.attachments[i];
			const auto& b = state.blend.attachments[0];
			if (a.enable != b.enable || a.srcColor != b.srcColor || a.dstColor != b.dstColor || a.colorOp != b.colorOp || a.srcAlpha != b.srcAlpha ||
				a.dstAlpha != b.dstAlpha || a.alphaOp != b.alphaOp || a.writeMask != b.writeMask)
				state.blend.independentBlend = true;
		}
		return state;
	}

	// a_label: what the pipeline's debugging dumps go under (BASICRHI_VK_DUMP_ISA, DXVK_PIPELINE_STATS).
	rhi::PipelinePtr CreatePipeline(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, const PipelineState& a_state, const std::vector<std::byte>& a_vertex,
		const std::string& a_vertexEntry, const std::vector<std::byte>& a_pixel, const std::string& a_pixelEntry, const std::string& a_label)
	{
		const rhi::SubobjLayout layout{ a_layout };
		// DXC keeps the HLSL entry points' names in the SPIR-V.
		const rhi::SubobjShader vertex{ rhi::ShaderStage::Vertex, { a_vertex.data(), static_cast<std::uint32_t>(a_vertex.size()) }, a_vertexEntry };
		const rhi::SubobjShader pixel{ rhi::ShaderStage::Pixel, { a_pixel.data(), static_cast<std::uint32_t>(a_pixel.size()) }, a_pixelEntry };
		const rhi::SubobjRaster raster{ a_state.raster };
		const rhi::SubobjDepth depth{ a_state.depth };
		const rhi::SubobjBlend blend{ a_state.blend };
		const rhi::SubobjRTVs targets{ a_state.targets };
		const rhi::SubobjDSV depthFormat{ a_state.depthFormat };
		const rhi::SubobjPrimitiveTopology topology{ rhi::PrimitiveTopology::TriangleList };
		const rhi::SubobjInputLayout input{};  // no vertex buffers: the vertex stage makes its positions from SV_VertexID
		const rhi::PipelineStreamItem items[] = {
			rhi::Make(layout),
			rhi::Make(vertex),
			rhi::Make(pixel),
			rhi::Make(raster),
			rhi::Make(depth),
			rhi::Make(blend),
			rhi::Make(targets),
			rhi::Make(depthFormat),
			rhi::Make(topology),
			rhi::Make(input),
		};
		rhi::vulkan::set_pipeline_statistics_label(a_label.c_str());
		rhi::PipelinePtr pipeline;
		const auto result = a_device.CreatePipeline(items, static_cast<std::uint32_t>(std::size(items)), pipeline);
		rhi::vulkan::set_pipeline_statistics_label(nullptr);
		if (result != rhi::Result::Ok)
			return {};
		return pipeline;
	}

	/** @brief What Load and the rebuild compile: both stages' sources (or generated text), entries and defines. */
	struct Stages
	{
		std::string vertexSource, pixelSource;
		std::wstring vertexEntry, pixelEntry;
		Build::Defines vertexDefines, pixelDefines;
		std::string label;
		std::string vertexText, pixelText;
		std::wstring languageVersion = L"2021";
	};

	/** @brief A stage's bytes: its generated text when it has one, else its file. */
	std::vector<std::byte> StageBytes(const std::filesystem::path& a_path, const std::string& a_text)
	{
		if (a_text.empty())
			return Build::ReadSource(a_path);
		const auto* data = reinterpret_cast<const std::byte*>(a_text.data());
		return { data, data + a_text.size() };
	}

	org::services::ShaderCompileRequest StageRequest(const std::filesystem::path& a_path, std::span<const std::byte> a_source, const std::wstring& a_entry,
		const wchar_t* a_target, const Stages& a_stages)
	{
		const bool vertex = std::wstring_view(a_target).starts_with(L"vs_");
		auto request = Build::MakeRequest(a_path, a_source, a_entry, a_target, vertex ? a_stages.vertexDefines : a_stages.pixelDefines);
		request.languageVersion = a_stages.languageVersion;
		return request;
	}

	std::uint64_t Fingerprint(const std::vector<std::byte>& a_vertex, const std::vector<std::byte>& a_pixel)
	{
		return Build::HashBinary(a_vertex) * 1099511628211ull ^ Build::HashBinary(a_pixel);
	}

	/**
	 * @brief The program's registry rebuild: both stages compiled again as they are now (off the owner thread; an unchanged one is a
	 * memory cache hit), the pixel stage once the vertex stage is in, and a new pair's pipeline made under the program's own layout,
	 * on the compile's thread. The program is held weakly: one released meanwhile has nothing to rebuild.
	 */
	org::services::LiveRebuild Rebuild(std::weak_ptr<const RasterProgram> a_program, rhi::Device a_device, PipelineState a_state, Stages a_stages)
	{
		return [a_program = std::move(a_program), a_device, a_state, a_stages = std::move(a_stages)](const org::services::LiveRebuildRequest& a_request,
				   org::services::LiveRebuildDone a_done) {
			auto* compiler = RenderGraphRuntime::Get().ShaderCompiler();
			const auto vertexPath = Build::SourcePath(a_stages.vertexSource);
			const auto pixelPath = Build::SourcePath(a_stages.pixelSource);
			auto vertexSource = std::make_shared<std::vector<std::byte>>(StageBytes(vertexPath, a_stages.vertexText));
			auto pixelSource = std::make_shared<std::vector<std::byte>>(StageBytes(pixelPath, a_stages.pixelText));
			if (!compiler || vertexSource->empty() || pixelSource->empty())
				return a_done({ .error = compiler ? fmt::format("missing shader source for {}", a_stages.label) : std::string("no runtime shader compiler") });
			// The compiler copies each source; a memory-cache hit (nothing changed) completes inline, on this thread.
			(void)compiler->CompileAsync(StageRequest(vertexPath, *vertexSource, a_stages.vertexEntry, L"vs_6_6", a_stages),
				[compiler, a_program, a_device, a_state, a_stages, a_request, a_done, pixelPath, pixelSource](const org::services::ShaderArtifact& a_vertex) {
					if (!a_vertex)
						return a_done({ .error = fmt::format("SPIR-V build of {}'s vertex stage failed:\n{}", a_stages.label, a_vertex.diagnostics.substr(0, 2000)) });
					(void)compiler->CompileAsync(StageRequest(pixelPath, *pixelSource, a_stages.pixelEntry, L"ps_6_6", a_stages),
						[a_program, a_device, a_state, a_stages, a_request, a_done, vertex = a_vertex.binary](const org::services::ShaderArtifact& a_pixel) {
							if (!a_pixel)
								return a_done({ .error = fmt::format("SPIR-V build of {}'s pixel stage failed:\n{}", a_stages.label, a_pixel.diagnostics.substr(0, 2000)) });
							const std::uint64_t fingerprint = Fingerprint(vertex, a_pixel.binary);
							if (fingerprint == a_request.fingerprint && !a_request.force)
								return a_done({ .fingerprint = fingerprint });
							const auto program = a_program.lock();
							if (!program)
								return a_done({ .fingerprint = a_request.fingerprint });  // released: nothing binds it
							auto pipeline = CreatePipeline(a_device, program->layout->GetHandle(), a_state, vertex, Util::WStringToString(a_stages.vertexEntry), a_pixel.binary,
								Util::WStringToString(a_stages.pixelEntry), a_stages.label);
							if (!pipeline)
								return a_done({ .error = fmt::format("the raster pipeline ({}) could not be created", a_stages.label) });
							a_done({ .payload = std::make_shared<rhi::PipelinePtr>(std::move(pipeline)), .fingerprint = fingerprint });
						});
				});
		};
	}

	/** @brief One stage compiled now (blocking); empty, logged, on failure. */
	std::vector<std::byte> CompileStage(org::services::ShaderCompiler& a_compiler, const std::string& a_source, const std::string& a_text, const std::wstring& a_entry,
		const wchar_t* a_target, const Stages& a_stages)
	{
		const auto path = Build::SourcePath(a_source);
		const auto bytes = StageBytes(path, a_text);
		if (bytes.empty()) {
			logger::error("[ORG] Missing shader source {}", path.string());
			return {};
		}
		const auto artifact = a_compiler.Compile(StageRequest(path, bytes, a_entry, a_target, a_stages));
		if (!artifact) {
			logger::error("[ORG] SPIR-V build of {} ({}) failed:\n{}", a_source, Util::WStringToString(a_entry), artifact.diagnostics.substr(0, 2000));
			return {};
		}
		return artifact.binary;
	}
#	endif
}

rhi::PipelineHandle RasterProgram::Pipeline() const
{
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	// Non-owning: the registry holds the payload it replaced until every submission that may use it has completed.
	return (*static_cast<const rhi::PipelinePtr*>(slot->Peek()))->GetHandle();
#	else
	return {};
#	endif
}

std::shared_ptr<const RasterProgram> RasterProgram::Load(rhi::Device a_device, const Desc& a_desc)
{
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	if (a_desc.targets.empty() || a_desc.targets.size() > 8) {
		logger::error("[ORG] {}: a raster program needs 1 to 8 colour targets, not {}", a_desc.pixelSource, a_desc.targets.size());
		return {};
	}
	auto* compiler = RenderGraphRuntime::Get().ShaderCompiler();
	if (!compiler) {
		logger::warn("[ORG] {}: no runtime shader compiler", a_desc.pixelSource);
		return {};
	}
	auto& registry = *RenderGraphRuntime::PipelineRegistry();
	// Before the sources are read: a reload that starts after rebuilds it (PipelineRegistry::Register).
	const std::uint64_t builtFor = registry.SourcesGeneration();
	// The defines resolved once, here: a hot reload compiles the same permutation.
	Stages stages{ a_desc.vertexSource, a_desc.pixelSource, a_desc.vertexEntry, a_desc.pixelEntry,
		a_desc.commonDefines ? Build::WithCommonDefines(a_desc.defines, L"vs_6_6") : a_desc.defines,
		a_desc.commonDefines ? Build::WithCommonDefines(a_desc.defines, L"ps_6_6") : a_desc.defines,
		fmt::format("{} ({})", std::filesystem::path(a_desc.pixelSource).stem().string(), Util::WStringToString(a_desc.pixelEntry)), a_desc.vertexText,
		a_desc.pixelText, a_desc.languageVersion };
	const auto vertex = CompileStage(*compiler, stages.vertexSource, stages.vertexText, stages.vertexEntry, L"vs_6_6", stages);
	const auto pixel = vertex.empty() ? std::vector<std::byte>{} : CompileStage(*compiler, stages.pixelSource, stages.pixelText, stages.pixelEntry, L"ps_6_6", stages);
	if (vertex.empty() || pixel.empty())
		return {};

	auto program = std::make_shared<RasterProgram>();
	program->constantWords = a_desc.constantWords;
	program->targetCount = static_cast<std::uint32_t>(a_desc.targets.size());
	program->hasDepth = a_desc.depthFormat != rhi::Format::Unknown;
	rhi::PushConstantRangeDesc constants{};
	constants.visibility = rhi::ShaderStage::AllGraphics;
	constants.num32BitValues = a_desc.constantWords;
	if (a_device.CreatePipelineLayout(rhi::PipelineLayoutDesc{ .pushConstants = { &constants, a_desc.constantWords ? 1u : 0u }, .flags = rhi::PipelineLayoutFlags::PF_None },
			program->layout) != rhi::Result::Ok) {
		logger::error("[ORG] {}: the pipeline layout could not be created", stages.label);
		return {};
	}
	const auto state = StateOf(a_desc);
	auto pipeline = CreatePipeline(a_device, program->layout->GetHandle(), state, vertex, Util::WStringToString(stages.vertexEntry), pixel,
		Util::WStringToString(stages.pixelEntry), stages.label);
	if (!pipeline) {
		logger::error("[ORG] {}: the raster pipeline could not be created", stages.label);
		return {};
	}
	rhi::IndirectArg draw[] = { { .kind = rhi::IndirectArgKind::Draw } };
	if (a_device.CreateCommandSignature(rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(draw, 1), sizeof(std::uint32_t) * 4 },
			program->layout->GetHandle(), program->drawSignature) != rhi::Result::Ok) {
		logger::error("[ORG] {}: the draw signature could not be created", stages.label);
		return {};
	}
	// Live while an owner holds the program (the slot is its).
	org::services::LivePipelineDesc live;
	live.id = fmt::format("{} + {}", a_desc.vertexSource, stages.label);
	// Generated stages have no file a reload could name.
	if (a_desc.vertexText.empty())
		live.sources.push_back(a_desc.vertexSource);
	if (a_desc.pixelText.empty())
		live.sources.push_back(a_desc.pixelSource);
	live.rebuild = Rebuild(program, a_device, state, stages);
	program->slot = registry.Register(std::move(live), std::make_shared<rhi::PipelinePtr>(std::move(pipeline)), Fingerprint(vertex, pixel), builtFor);
	return program;
#	else
	(void)a_device;
	logger::warn("[ORG] {}: built without ORGModuleServices' compiler", a_desc.pixelSource);
	return {};
#	endif
}

void RasterProgram::Draw(org::RecordingContext& a_recording, const DrawDesc& a_draw) const
{
	if (a_draw.colours.size() != targetCount || (a_draw.depth != nullptr) != hasDepth || a_draw.constants.size() > constantWords || !a_draw.viewport.width ||
		!a_draw.viewport.height || !slot)
		return;
	std::array<rhi::ColorAttachment, 8> colours{};
	for (std::uint32_t i = 0; i < targetCount; ++i) {
		const auto& colour = a_draw.colours[i];
		if (!colour.view)
			return;
		auto& attachment = colours[i];
		attachment.rtv = a_recording.View(*colour.view);
		attachment.resource = a_recording.Resolve(*colour.view).GetHandle();
		attachment.loadOp = colour.load;
		attachment.storeOp = rhi::StoreOp::Store;
		attachment.clear.type = rhi::ClearValueType::Color;
		std::copy(colour.clear.begin(), colour.clear.end(), attachment.clear.rgba);
	}
	rhi::DepthAttachment depth{};
	if (a_draw.depth) {
		if (!a_draw.depth->view)
			return;
		depth.dsv = a_recording.View(*a_draw.depth->view);
		depth.resource = a_recording.Resolve(*a_draw.depth->view).GetHandle();
		depth.readOnly = a_draw.depth->readOnly;
		depth.depthLoad = a_draw.depth->load;
		depth.depthStore = a_draw.depth->readOnly ? rhi::StoreOp::DontCare : rhi::StoreOp::Store;
		// The stencil is kept as it is: no raster program tests or writes it.
		depth.stencilLoad = rhi::LoadOp::Load;
		depth.stencilStore = rhi::StoreOp::Store;
		depth.clear.type = rhi::ClearValueType::DepthStencil;
		depth.clear.depthStencil.depth = a_draw.depth->clear;
	}
	rhi::PassBeginInfo begin{};
	begin.colors = { colours.data(), targetCount };
	begin.depth = a_draw.depth ? &depth : nullptr;
	begin.x = a_draw.viewport.x;
	begin.y = a_draw.viewport.y;
	begin.width = a_draw.viewport.width;
	begin.height = a_draw.viewport.height;
	begin.minDepth = a_draw.viewport.minDepth;
	begin.maxDepth = a_draw.viewport.maxDepth;
	begin.debugName = a_draw.debugName;
	auto& commands = a_recording.Commands();
	commands.BeginPass(begin);
	commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
	commands.BindLayout(layout->GetHandle());
	commands.BindPipeline(Pipeline());
	if (!a_draw.constants.empty())
		commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, 0, 0, static_cast<std::uint32_t>(a_draw.constants.size()), a_draw.constants.data());
	if (a_draw.indirect && drawSignature)
		commands.ExecuteIndirect(drawSignature->GetHandle(), a_draw.indirect->Resource()->GetAPIResource().GetHandle(),
			a_draw.indirect->Offset(a_recording.FrameSlot()) + a_draw.indirectOffset, {}, 0, 1);
	else
		commands.Draw(a_draw.vertexCount, a_draw.instanceCount, 0, 0);
	commands.EndPass();
}

#endif
