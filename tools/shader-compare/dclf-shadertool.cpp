// dclf-shadertool: the game's DCLF shader source transforms, run offline (tools/shader-compare/shader_compare.py drives it).
//
//   dclf-shadertool rewrite <in.hlsl> <out.hlsl> --shaders <Data/Shaders dir> --material-textures <r,r,...>
//                   [--shadow-mask <slot>] [--samplers <count>] [--lazy]
//
// rewrite: the pulled build's source (DCLF_PULLED), as ShaderPrograms makes it: PulledSource::Rewrite with the options a
// CS_DCLF_STAGE_DUMP "options" line names; --lazy (CS_DCLF_LAZY_ROWS) takes the eager names from every .hlsl and .hlsli under --shaders,
// as RenderGraphRuntime::ShaderSourceFiles lists them.
//
//   dclf-shadertool d3d12 <preprocessed.hlsl> <out.hlsl> --pixel|--vertex
//
// d3d12: a D3D12 pulled build's frame rewrite (FrameSource::Rewrite) of DXC -P output, with the pass-wide blocks of DrawPipelines.h
// (kFramePushVS, kFramePushPS), as ShaderPrograms makes it.
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/FrameSource.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/PulledSource.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	int Usage()
	{
		std::cerr << "usage: dclf-shadertool rewrite <in.hlsl> <out.hlsl> --shaders <dir> --material-textures <r,r,...> [--shadow-mask <slot>] "
					 "[--samplers <count>] [--lazy]\n";
		return 2;
	}

	int Rewrite(int a_argc, char** a_argv)
	{
		if (a_argc < 4)
			return Usage();
		const std::filesystem::path input = a_argv[2], output = a_argv[3];
		std::filesystem::path shaders;
		bool lazy = false;
		DCLF::PulledSource::Options options;
		for (int i = 4; i < a_argc; ++i) {
			const std::string arg = a_argv[i];
			const auto value = [&]() -> std::string { return i + 1 < a_argc ? a_argv[++i] : std::string{}; };
			if (arg == "--shaders")
				shaders = value();
			else if (arg == "--material-textures") {
				std::stringstream list(value());
				for (std::string r; std::getline(list, r, ',');)
					if (!r.empty() && std::stoul(r) < options.materialTexture.size())
						options.materialTexture[std::stoul(r)] = true;
			} else if (arg == "--shadow-mask")
				options.shadowMaskSlot = static_cast<std::uint32_t>(std::stoul(value()));
			else if (arg == "--samplers")
				options.samplerSlots = static_cast<std::uint32_t>(std::stoul(value()));
			else if (arg == "--lazy")
				lazy = true;
			else
				return Usage();
		}
		std::unordered_set<std::string> eager;
		if (lazy) {
			std::vector<std::filesystem::path> files;
			std::error_code ec;
			for (auto it = std::filesystem::recursive_directory_iterator(shaders, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
				const auto extension = it->path().extension();
				if (it->is_regular_file() && (extension == ".hlsl" || extension == ".hlsli"))
					files.push_back(it->path());
			}
			if (files.empty()) {
				std::cerr << "no shader files under " << shaders.string() << "\n";
				return 1;
			}
			eager = DCLF::PulledSource::EagerRowNames(files);
			options.eager = &eager;
		}
		std::ifstream in(input, std::ios::binary);
		const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		if (source.empty()) {
			std::cerr << "cannot read " << input.string() << "\n";
			return 1;
		}
		const auto result = DCLF::PulledSource::Rewrite(source, options);
		if (!result.error.empty()) {
			std::cerr << "rewrite failed: " << result.error << "\n";
			return 1;
		}
		std::ofstream(output, std::ios::binary) << result.source;
		std::cout << result.textures << " textures, " << result.samplers << " samplers, " << result.members << " block members (" << result.lazy << " where used)\n";
		return 0;
	}
}

namespace
{
	int D3D12(int a_argc, char** a_argv)
	{
		if (a_argc != 5)
			return Usage();
		const std::string stage = a_argv[4];
		if (stage != "--pixel" && stage != "--vertex")
			return Usage();
		std::ifstream in(a_argv[2], std::ios::binary);
		const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		if (source.empty()) {
			std::cerr << "cannot read " << a_argv[2] << "\n";
			return 1;
		}
		// DrawPipelines.h's kFramePushVS and kFramePushPS.
		DCLF::FrameSource::Options options;
		options.pixel = stage == "--pixel";
		options.vertexBlocks = (1u << 3) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 11) | (1u << 12) | (1u << 13);
		options.pixelBlocks = (1u << 3) | (1u << 5) | (1u << 6) | (1u << 9) | (1u << 10) | (1u << 12) | (1u << 13);
		const auto result = DCLF::FrameSource::Rewrite(source, options);
		if (!result.error.empty()) {
			std::cerr << "d3d12 rewrite failed: " << result.error << "\n";
			return 1;
		}
		std::ofstream(a_argv[3], std::ios::binary) << result.source;
		std::cout << result.textures << " textures, " << result.samplers << " samplers, " << result.blocks << " blocks (" << result.members << " members)\n";
		return 0;
	}
}

int main(int a_argc, char** a_argv)
{
	if (a_argc >= 2 && std::string(a_argv[1]) == "rewrite")
		return Rewrite(a_argc, a_argv);
	if (a_argc >= 2 && std::string(a_argv[1]) == "d3d12")
		return D3D12(a_argc, a_argv);
	return Usage();
}
