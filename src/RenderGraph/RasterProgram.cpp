#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/RasterProgram.h"

#	include "RenderGraph/RenderGraphRuntime.h"
#	include "Utils/Logging.h"

#	include <format>

#	include <Render/LatchBlock.h>
#	include <Render/PreparedPass.h>
#	include <Render/ResourceUseDeclaration.h>
#	include <Resources/Buffers/Buffer.h>

#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
#		include "RenderGraph/ProgramBuild.h"
#		include "RenderGraph/ShaderTarget.h"

#		include <ORGModuleServices/PipelineRegistry.h>
#		include <ORGModuleServices/ShaderCompiler.h>
#	endif

#	include <filesystem>

namespace
{
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	namespace Build = RenderGraph::ProgramBuild;

	// Kept for the registry's rebuilds.
	struct PipelineState
	{
		rhi::RasterState raster{};
		rhi::DepthStencilState depth{};
		rhi::BlendState blend{};
		rhi::RenderTargets targets{};
		rhi::Format depthFormat = rhi::Format::Unknown;
		rhi::PrimitiveTopology topology = rhi::PrimitiveTopology::TriangleList;
	};

	PipelineState StateOf(const RasterProgram::Desc& a_desc)
	{
		PipelineState state{};
		state.raster = a_desc.raster;
		state.depth = a_desc.depth;
		state.depthFormat = a_desc.depthFormat;
		state.topology = a_desc.topology;
		state.blend.alphaToCoverage = a_desc.alphaToCoverage;
		state.blend.numAttachments = static_cast<std::uint32_t>(a_desc.targets.size());
		state.targets.count = static_cast<std::uint32_t>(a_desc.targets.size());
		for (std::size_t i = 0; i < a_desc.targets.size(); ++i) {
			state.targets.formats[i] = a_desc.targets[i].format;
			state.blend.attachments[i] = a_desc.targets[i].blend;
			// Independent blending only when the targets differ, since one state for all works on every backend.
			const auto& a = state.blend.attachments[i];
			const auto& b = state.blend.attachments[0];
			if (a.enable != b.enable || a.srcColor != b.srcColor || a.dstColor != b.dstColor || a.colorOp != b.colorOp || a.srcAlpha != b.srcAlpha ||
				a.dstAlpha != b.dstAlpha || a.alphaOp != b.alphaOp || a.writeMask != b.writeMask)
				state.blend.independentBlend = true;
		}
		return state;
	}

	// a_label names the pipeline's debugging dumps.
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
		const rhi::SubobjPrimitiveTopology topology{ a_state.topology };
		const rhi::SubobjInputLayout input{};  // no vertex buffers, positions come from SV_VertexID
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

	struct Stages
	{
		std::string vertexSource, pixelSource;
		std::wstring vertexEntry, pixelEntry;
		Build::Defines vertexDefines, pixelDefines;
		std::string label;
		std::string vertexText, pixelText;
		std::wstring languageVersion = L"2021";
	};

	/** @brief A stage's generated text when it has one, else its file. */
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
	 * @brief Recompiles both stages off the owner thread and makes a new pipeline under the program's own layout.
	 * The program is held weakly, so one released meanwhile is skipped.
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
			// A memory-cache hit completes inline on this thread.
			(void)compiler->CompileAsync(StageRequest(vertexPath, *vertexSource, a_stages.vertexEntry, L"vs_6_6", a_stages),
				[compiler, a_program, a_device, a_state, a_stages, a_request, a_done, pixelPath, pixelSource](const org::services::ShaderArtifact& a_vertex) {
					if (!a_vertex)
						return a_done({ .error = fmt::format("{} build of {}'s vertex stage failed:\n{}", RenderGraph::ShaderTarget::FormatName(), a_stages.label, a_vertex.diagnostics.substr(0, 2000)) });
					(void)compiler->CompileAsync(StageRequest(pixelPath, *pixelSource, a_stages.pixelEntry, L"ps_6_6", a_stages),
						[a_program, a_device, a_state, a_stages, a_request, a_done, vertex = a_vertex.binary](const org::services::ShaderArtifact& a_pixel) {
							if (!a_pixel)
								return a_done({ .error = fmt::format("{} build of {}'s pixel stage failed:\n{}", RenderGraph::ShaderTarget::FormatName(), a_stages.label, a_pixel.diagnostics.substr(0, 2000)) });
							const std::uint64_t fingerprint = Fingerprint(vertex, a_pixel.binary);
							if (fingerprint == a_request.fingerprint && !a_request.force)
								return a_done({ .fingerprint = fingerprint });
							const auto program = a_program.lock();
							if (!program)
								return a_done({ .fingerprint = a_request.fingerprint });  // released, nothing binds it
							auto pipeline = CreatePipeline(a_device, program->layout->GetHandle(), a_state, vertex, Util::WStringToString(a_stages.vertexEntry), a_pixel.binary,
								Util::WStringToString(a_stages.pixelEntry), a_stages.label);
							if (!pipeline)
								return a_done({ .error = fmt::format("the raster pipeline ({}) could not be created", a_stages.label) });
							a_done({ .payload = std::make_shared<rhi::PipelinePtr>(std::move(pipeline)), .fingerprint = fingerprint });
						});
				});
		};
	}

	/** @brief Compiles one stage now, blocking. Empty and logged on failure. */
	std::vector<std::byte> CompileStage(org::services::ShaderCompiler& a_compiler, const std::string& a_source, const std::string& a_text, const std::wstring& a_entry,
		const wchar_t* a_target, const Stages& a_stages, spdlog::level::level_enum a_level = spdlog::level::err)
	{
		const auto path = Build::SourcePath(a_source);
		const auto bytes = StageBytes(path, a_text);
		if (bytes.empty()) {
			logger::error("[ORG] Missing shader source {}", path.string());
			return {};
		}
		const auto artifact = a_compiler.Compile(StageRequest(path, bytes, a_entry, a_target, a_stages));
		if (!artifact) {
			spdlog::log(a_level, "[ORG] {} build of {} ({}) failed:\n{}", RenderGraph::ShaderTarget::FormatName(), a_source, Util::WStringToString(a_entry), artifact.diagnostics.substr(0, 2000));
			return {};
		}
		return artifact.binary;
	}

	/** @brief Resolves the desc's defines now, since CS's common defines read State. */
	Stages StagesOf(const RasterProgram::Desc& a_desc)
	{
		return { a_desc.vertexSource, a_desc.pixelSource, a_desc.vertexEntry, a_desc.pixelEntry,
			!a_desc.vertexStageDefines.empty() ? a_desc.vertexStageDefines :
			a_desc.commonDefines               ? Build::WithCommonDefines(a_desc.defines, L"vs_6_6") :
												 a_desc.defines,
			!a_desc.pixelStageDefines.empty() ? a_desc.pixelStageDefines :
			a_desc.commonDefines              ? Build::WithCommonDefines(a_desc.defines, L"ps_6_6") :
												a_desc.defines,
			fmt::format("{} ({})", std::filesystem::path(a_desc.pixelSource).stem().string(), Util::WStringToString(a_desc.pixelEntry)), a_desc.vertexText,
			a_desc.pixelText, a_desc.languageVersion };
	}
#	endif
}

bool RasterProgram::CompileStages(const Desc& a_desc)
{
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	auto* compiler = RenderGraphRuntime::Get().ShaderCompiler();
	if (!compiler)
		return false;
	const Stages stages = StagesOf(a_desc);
	// A precompile failure is only a warning. A Load that needs it logs its own error.
	return !CompileStage(*compiler, stages.vertexSource, stages.vertexText, stages.vertexEntry, L"vs_6_6", stages, spdlog::level::warn).empty() &&
	       !CompileStage(*compiler, stages.pixelSource, stages.pixelText, stages.pixelEntry, L"ps_6_6", stages, spdlog::level::warn).empty();
#	else
	(void)a_desc;
	return false;
#	endif
}

rhi::PipelineHandle RasterProgram::Pipeline() const
{
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	// Non-owning. The registry keeps a replaced payload until every submission that may use it has completed.
	return (*static_cast<const rhi::PipelinePtr*>(slot->Peek()))->GetHandle();
#	else
	return {};
#	endif
}

std::shared_ptr<const RasterProgram> RasterProgram::Load(rhi::Device a_device, const Desc& a_desc)
{
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	// A depth-only program is allowed, since the engine skeleton's depth clears draw one.
	if ((a_desc.targets.empty() && a_desc.depthFormat == rhi::Format::Unknown) || a_desc.targets.size() > 8) {
		logger::error("[ORG] {}: a raster program needs 1 to 8 colour targets, or none with a depth format, not {}", a_desc.pixelSource, a_desc.targets.size());
		return {};
	}
	auto* compiler = RenderGraphRuntime::Get().ShaderCompiler();
	if (!compiler) {
		logger::warn("[ORG] {}: no runtime shader compiler", a_desc.pixelSource);
		return {};
	}
	auto& registry = *RenderGraphRuntime::PipelineRegistry();
	// Read before the sources, so a reload that starts after rebuilds it.
	const std::uint64_t builtFor = registry.SourcesGeneration();
	// Resolved once here, so a hot reload compiles the same permutation.
	const Stages stages = StagesOf(a_desc);
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

bool RasterProgram::Draw(org::RecordingContext& a_recording, const DrawDesc& a_draw) const
{
	// A skipped draw leaves its targets as they were, and its reason is logged once.
	static Util::ReasonLog skipped("[ORG] raster program: a draw was not recorded", spdlog::level::warn);
	const auto skip = [&](std::string_view a_reason) {
		skipped.Report(a_reason, a_draw.debugName ? a_draw.debugName : "");
		return false;
	};
	if (a_draw.colours.size() != targetCount)
		return skip(std::format("{} colour targets given, the program has {}", a_draw.colours.size(), targetCount));
	if ((a_draw.depth != nullptr) != hasDepth)
		return skip(hasDepth ? "no depth target given, the program has one" : "a depth target given, the program has none");
	if (a_draw.constants.size() > constantWords)
		return skip(std::format("{} constant words given, the program takes {}", a_draw.constants.size(), constantWords));
	if (!a_draw.viewport.width || !a_draw.viewport.height)
		return skip("an empty viewport");
	if (!slot)
		return skip("no pipeline slot");
	if (a_draw.indirectBuffer.valid() && !drawSignature)
		return skip("an indirect buffer without the program's draw signature");
	std::array<rhi::ColorAttachment, 8> colours{};
	for (std::uint32_t i = 0; i < targetCount; ++i) {
		const auto& colour = a_draw.colours[i];
		if (!colour.view)
			return skip("a colour target without a view");
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
			return skip("a depth target without a view");
		depth.dsv = a_recording.View(*a_draw.depth->view);
		depth.resource = a_recording.Resolve(*a_draw.depth->view).GetHandle();
		depth.readOnly = a_draw.depth->readOnly;
		depth.depthLoad = a_draw.depth->load;
		depth.depthStore = a_draw.depth->readOnly ? rhi::StoreOp::DontCare : rhi::StoreOp::Store;
		// The stencil is loaded and stored so a program that writes it keeps what it wrote. clearStencil clears it to 0.
		depth.stencilLoad = a_draw.depth->load == rhi::LoadOp::Clear && a_draw.depth->clearStencil ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
		depth.stencilStore = rhi::StoreOp::Store;
		depth.clear.type = rhi::ClearValueType::DepthStencil;
		depth.clear.depthStencil.depth = a_draw.depth->clear;
		depth.clear.depthStencil.stencil = 0;
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
	// Set every draw, since the reference holds across the list's passes.
	commands.SetStencilRef(a_draw.stencilReference);
	// BeginPass set viewport 0 and the scissor to the render area. A draw's own replace them inside this pass.
	if (a_draw.dynamicViewport)
		commands.SetViewports({ &*a_draw.dynamicViewport, 1 });
	if (a_draw.scissor) {
		// Clipped to the render area, which would clip it anyway.
		const auto& area = a_draw.viewport;
		const auto& rect = *a_draw.scissor;
		const std::int64_t left = (std::max)(static_cast<std::int64_t>(rect.x), static_cast<std::int64_t>(area.x));
		const std::int64_t top = (std::max)(static_cast<std::int64_t>(rect.y), static_cast<std::int64_t>(area.y));
		const std::int64_t right = (std::min)(static_cast<std::int64_t>(rect.x) + rect.width, static_cast<std::int64_t>(area.x) + area.width);
		const std::int64_t bottom = (std::min)(static_cast<std::int64_t>(rect.y) + rect.height, static_cast<std::int64_t>(area.y) + area.height);
		const rhi::ScissorRect clipped{ .x = static_cast<std::int32_t>(left),
			.y = static_cast<std::int32_t>(top),
			.width = static_cast<std::uint32_t>((std::max)(right - left, std::int64_t(0))),
			.height = static_cast<std::uint32_t>((std::max)(bottom - top, std::int64_t(0))) };
		commands.SetScissors({ &clipped, 1 });
	}
	if (!a_draw.constants.empty())
		commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, 0, 0, static_cast<std::uint32_t>(a_draw.constants.size()), a_draw.constants.data());
	// Vulkan ends a query begun inside a pass in that same pass, so the count is this draw's alone.
	const bool counted = a_draw.occlusionPool.valid();
	if (counted)
		commands.BeginQuery(a_draw.occlusionPool, a_draw.occlusionIndex);
	if (a_draw.indirect && drawSignature) {
		const auto region = a_draw.indirect->At(a_recording.Run());
		commands.ExecuteIndirect(drawSignature->GetHandle(), region.buffer->GetAPIResource().GetHandle(), region.offset + a_draw.indirectOffset, {}, 0, 1);
	} else if (a_draw.indirectBuffer.valid() && drawSignature)
		commands.ExecuteIndirect(drawSignature->GetHandle(), a_draw.indirectBuffer, a_draw.indirectBufferOffset, {}, 0, 1);
	else
		commands.Draw(a_draw.vertexCount, a_draw.instanceCount, 0, 0);
	if (counted)
		commands.EndQuery(a_draw.occlusionPool, a_draw.occlusionIndex);
	commands.EndPass();
	return true;
}

#endif
