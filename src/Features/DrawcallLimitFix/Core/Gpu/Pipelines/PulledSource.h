#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

/**
 * @brief The pulled builds' source rewrite. Standard library only, with the game's inputs in Options, so the game and
 * tools/shader-compare make the same source from the same file.
 */
namespace DCLF::PulledSource
{
	struct Options
	{
		// By texture register, the ones read at the material row's index.
		std::array<bool, 128> materialTexture{};
		std::uint32_t shadowMaskSlot = 14;  // read at the pipeline row's index, as its sampler is
		std::uint32_t samplerSlots = 16;  // s0 to s(n-1) are read from the rows
		// CS_DCLF_LAZY_ROWS: members kept as statics. Every other one is loaded where it is read. Null keeps every member a static.
		const std::unordered_set<std::string>* eager = nullptr;
	};

	struct Result
	{
		std::string source;  // empty with an error
		std::string error;
		std::uint32_t textures = 0, samplers = 0, members = 0, lazy = 0;
	};

	/** @brief Names a block member may not become a macro of, because some shader names them as a field or assigns or declares them. */
	std::unordered_set<std::string> EagerRowNames(const std::vector<std::filesystem::path>& a_files);

	/** @brief a_source rewritten for a pulled build. */
	Result Rewrite(std::string_view a_source, const Options& a_options);
}
