#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "ComputeProgram.h"

#	include "RenderGraph/RenderGraphRuntime.h"

#	include <Render/LatchBlock.h>
#	include <Render/PreparedPass.h>
#	include <Resources/Buffers/Buffer.h>

#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
#		include "RenderGraph/ProgramBuild.h"

#		include <ORGModuleServices/PipelineRegistry.h>
#		include <ORGModuleServices/ShaderCompiler.h>
#	endif

#	include <filesystem>
#	include <span>

namespace
{
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	using RenderGraph::ProgramBuild::HashBinary;
	using RenderGraph::ProgramBuild::ReadSource;

	org::services::ShaderCompileRequest MakeRequest(const ComputeProgram::Desc& a_desc, const std::filesystem::path& a_path, std::span<const std::byte> a_source)
	{
		return RenderGraph::ProgramBuild::MakeRequest(a_path, a_source, a_desc.entry, L"cs_6_6", a_desc.defines);
	}

	// a_source: the program's shader, the label its pipeline's debugging dumps go under (BASICRHI_VK_DUMP_ISA, DXVK_PIPELINE_STATS).
	rhi::PipelinePtr CreatePipeline(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, const std::vector<std::byte>& a_binary, const std::string& a_entry,
		const std::filesystem::path& a_source)
	{
		rhi::PipelinePtr pipeline;
		const rhi::SubobjLayout layout{ a_layout };
		// DXC keeps the HLSL entry point's name in the SPIR-V.
		const rhi::SubobjShader shader{ rhi::ShaderStage::Compute, { a_binary.data(), static_cast<std::uint32_t>(a_binary.size()) }, a_entry.c_str() };
		const rhi::PipelineStreamItem items[] = { rhi::Make(layout), rhi::Make(shader) };
		const std::string label = a_source.stem().string();
		rhi::vulkan::set_pipeline_statistics_label(label.c_str());
		const auto result = a_device.CreatePipeline(items, 2, pipeline);
		rhi::vulkan::set_pipeline_statistics_label(nullptr);
		if (result != rhi::Result::Ok)
			return {};
		return pipeline;
	}

	/**
	 * @brief The program's registry rebuild: its source compiled again as it is now (off the owner thread; an unchanged one is a memory
	 * cache hit), and a new SPIR-V's pipeline made under the program's own layout, on the compile's thread. The program is held weakly:
	 * one released meanwhile has nothing to rebuild.
	 */
	org::services::LiveRebuild Rebuild(std::weak_ptr<const ComputeProgram> a_program, rhi::Device a_device, ComputeProgram::Desc a_desc)
	{
		return [a_program = std::move(a_program), a_device, a_desc = std::move(a_desc)](const org::services::LiveRebuildRequest& a_request, org::services::LiveRebuildDone a_done) {
			auto* compiler = RenderGraphRuntime::Get().ShaderCompiler();
			const std::filesystem::path path = RenderGraph::ProgramBuild::SourcePath(a_desc.source);
			const auto source = ReadSource(path);
			if (!compiler || source.empty())
				return a_done({ .error = compiler ? fmt::format("missing shader source {}", path.string()) : std::string("no runtime shader compiler") });
			// The compiler copies the source; a memory-cache hit (nothing changed) completes here, on this thread.
			(void)compiler->CompileAsync(MakeRequest(a_desc, path, source),
				[a_program, a_device, a_request, a_done = std::move(a_done), entry = Util::WStringToString(a_desc.entry), source = std::filesystem::path(a_desc.source)](
					const org::services::ShaderArtifact& a_artifact) {
					if (!a_artifact)
						return a_done({ .error = fmt::format("SPIR-V build ({}) failed:\n{}", entry, a_artifact.diagnostics.substr(0, 2000)) });
					const std::uint64_t fingerprint = HashBinary(a_artifact.binary);
					if (fingerprint == a_request.fingerprint && !a_request.force)
						return a_done({ .fingerprint = fingerprint });
					const auto program = a_program.lock();
					if (!program)
						return a_done({ .fingerprint = a_request.fingerprint });  // released: nothing binds it
					auto pipeline = CreatePipeline(a_device, program->layout->GetHandle(), a_artifact.binary, entry, source);
					if (!pipeline)
						return a_done({ .error = fmt::format("the compute pipeline ({}) could not be created", entry) });
					a_done({ .payload = std::make_shared<rhi::PipelinePtr>(std::move(pipeline)), .fingerprint = fingerprint });
				});
		};
	}
#	endif
}

rhi::PipelineHandle ComputeProgram::Pipeline() const
{
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	// Non-owning: the registry holds the payload it replaced until every submission that may use it has completed.
	return (*static_cast<const rhi::PipelinePtr*>(slot->Peek()))->GetHandle();
#	else
	return {};
#	endif
}

void ComputeProgram::Dispatch(org::RecordingContext& a_recording, std::span<const std::uint32_t> a_constants, std::uint32_t a_x, std::uint32_t a_y,
	std::uint32_t a_z) const
{
	if (a_constants.size() > constantWords || !a_x || !a_y || !a_z || !slot)
		return;
	auto& commands = a_recording.Commands();
	commands.BindLayout(layout->GetHandle());
	commands.BindPipeline(Pipeline());
	if (!a_constants.empty())
		commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, static_cast<std::uint32_t>(a_constants.size()), a_constants.data());
	commands.Dispatch(a_x, a_y, a_z);
}

void ComputeProgram::DispatchIndirect(org::RecordingContext& a_recording, std::span<const std::uint32_t> a_constants, rhi::ResourceHandle a_arguments,
	std::uint64_t a_offset) const
{
	if (a_constants.size() > constantWords || !slot || !dispatchSignature)
		return;
	auto& commands = a_recording.Commands();
	commands.BindLayout(layout->GetHandle());
	commands.BindPipeline(Pipeline());
	if (!a_constants.empty())
		commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, static_cast<std::uint32_t>(a_constants.size()), a_constants.data());
	commands.ExecuteIndirect(dispatchSignature->GetHandle(), a_arguments, a_offset, {}, 0, 1);
}

void ComputeProgram::DispatchIndirect(org::RecordingContext& a_recording, std::span<const std::uint32_t> a_constants, const org::LatchBlock& a_latch,
	std::uint32_t a_offset) const
{
	DispatchIndirect(a_recording, a_constants, a_latch.Resource()->GetAPIResource().GetHandle(), a_latch.Offset(a_recording.FrameSlot()) + a_offset);
}

std::shared_ptr<const ComputeProgram> ComputeProgram::Load(rhi::Device a_device, const Desc& a_desc)
{
#	if defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)
	// The defines resolved once, here: a hot reload compiles the same permutation.
	Desc desc = a_desc;
	if (desc.commonDefines)
		desc.defines = RenderGraph::ProgramBuild::WithCommonDefines(a_desc.defines, L"cs_6_6");
	desc.commonDefines = false;
	auto* compiler = RenderGraphRuntime::Get().ShaderCompiler();
	if (!compiler) {
		logger::warn("[ORG] {}: no runtime shader compiler", desc.source);
		return {};
	}
	auto& registry = *RenderGraphRuntime::PipelineRegistry();
	// Before the source is read: a reload that starts after rebuilds it (PipelineRegistry::Register).
	const std::uint64_t builtFor = registry.SourcesGeneration();
	const std::filesystem::path path = RenderGraph::ProgramBuild::SourcePath(desc.source);
	const auto bytes = ReadSource(path);
	if (bytes.empty()) {
		logger::error("[ORG] Missing shader source {}", path.string());
		return {};
	}
	const std::string entry = Util::WStringToString(desc.entry);
	const auto artifact = compiler->Compile(MakeRequest(desc, path, bytes));
	if (!artifact) {
		logger::error("[ORG] SPIR-V build of {} ({}) failed:\n{}", desc.source, entry, artifact.diagnostics.substr(0, 2000));
		return {};
	}

	auto program = std::make_shared<ComputeProgram>();
	program->constantWords = desc.constantWords;
	rhi::PushConstantRangeDesc constants{};
	constants.visibility = rhi::ShaderStage::Compute;
	constants.num32BitValues = desc.constantWords;
	if (a_device.CreatePipelineLayout(rhi::PipelineLayoutDesc{ .pushConstants = { &constants, 1 }, .flags = rhi::PipelineLayoutFlags::PF_None }, program->layout) !=
		rhi::Result::Ok) {
		logger::error("[ORG] {}: the pipeline layout could not be created", desc.source);
		return {};
	}
	auto pipeline = CreatePipeline(a_device, program->layout->GetHandle(), artifact.binary, entry, desc.source);
	if (!pipeline) {
		logger::error("[ORG] {}: the compute pipeline could not be created", desc.source);
		return {};
	}
	rhi::IndirectArg dispatch[] = { { .kind = rhi::IndirectArgKind::Dispatch } };
	if (a_device.CreateCommandSignature(rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(dispatch, 1), sizeof(std::uint32_t) * 3 },
			program->layout->GetHandle(), program->dispatchSignature) != rhi::Result::Ok) {
		logger::error("[ORG] {}: the dispatch signature could not be created", desc.source);
		return {};
	}
	// Live while an owner holds the program (the slot is its).
	org::services::LivePipelineDesc live;
	live.id = fmt::format("{} ({})", desc.source, entry);
	live.sources = { desc.source };
	live.rebuild = Rebuild(program, a_device, desc);
	program->slot = registry.Register(std::move(live), std::make_shared<rhi::PipelinePtr>(std::move(pipeline)), HashBinary(artifact.binary), builtFor);
	return program;
#	else
	(void)a_device;
	logger::warn("[ORG] {}: built without ORGModuleServices' compiler", a_desc.source);
	return {};
#	endif
}

#endif
