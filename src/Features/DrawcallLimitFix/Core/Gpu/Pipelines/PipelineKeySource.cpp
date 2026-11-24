// The pipeline key sources: the local manifest and the shipped one.

#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/PipelineKeySource.h"

#include "RenderGraph/Backend.h"
#include "Utils/SharedShaderStore.h"

#include <Windows.h>

#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <thread>

namespace DCLF
{
	namespace PipelineManifestDetail
	{
		std::wstring FileName(std::wstring_view a_suffix = {})
		{
			return std::wstring(::RenderGraph::Backend::IsD3D12() ? L"dclf-pipelines-d3d12" : L"dclf-pipelines-vulkan") + std::wstring(a_suffix) + L".txt";
		}

		/** @brief In the store's folder. The store and the old-cache cleanup leave it alone. */
		std::filesystem::path LocalPath(std::wstring_view a_suffix)
		{
			return std::filesystem::path(L"Data/ShaderCache") / ::Util::kShaderStoreDirName / FileName(a_suffix);
		}

		/** @brief The read-only copy a build ships beside the shader pack. */
		std::filesystem::path ShippedPath()
		{
			return std::filesystem::path(L"Data/ShaderCache") / L"pack" / FileName();
		}

		/** @brief Reads a manifest into a_lines, skipping malformed lines and lines past a tag's cap. Returns the lines read and skipped. */
		std::pair<std::size_t, std::size_t> Parse(const std::filesystem::path& a_path, PipelineManifestLines& a_lines)
		{
			std::ifstream in{ a_path };
			if (!in)
				return {};
			std::string line;
			std::size_t read = 0, skipped = 0;
			while (std::getline(in, line)) {
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				const auto tab = line.find('\t');
				if (tab == std::string::npos || tab == 0) {
					skipped += !line.empty();
					continue;
				}
				PipelineWords identity, inputs;
				bool ok = true, separated = false;
				std::istringstream stream(line.substr(tab + 1));
				std::string token;
				while (ok && stream >> token) {
					if (token == "|") {
						ok = !separated;
						separated = true;
						continue;
					}
					std::uint32_t value = 0;
					const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
					ok = result.ec == std::errc{} && result.ptr == token.data() + token.size();
					(separated ? inputs : identity).push_back(value);
				}
				auto& tagLines = a_lines[line.substr(0, tab)];
				if (!ok || !separated || identity.empty() || tagLines.size() >= kMaxPipelineKeysPerTag) {
					++skipped;
					continue;
				}
				tagLines.insert_or_assign(std::move(identity), std::move(inputs));
				++read;
			}
			return { read, skipped };
		}

		std::string Format(const PipelineManifestLines& a_lines)
		{
			std::string text;
			for (const auto& [tag, tagLines] : a_lines)
				for (const auto& [identity, inputs] : tagLines) {
					text += tag;
					text += '\t';
					for (const auto word : identity)
						text += std::format("{} ", word);
					text += '|';
					for (const auto word : inputs)
						text += std::format(" {}", word);
					text += '\n';
				}
			return text;
		}
	}

	bool PipelineKeySet::Add(const std::string& a_tag, const PipelineWords& a_identity, const PipelineWords& a_inputs)
	{
		auto& tagLines = lines[a_tag];
		if (tagLines.size() >= kMaxPipelineKeysPerTag || !tagLines.try_emplace(a_identity, a_inputs).second)
			return false;
		++size;
		return true;
	}

	bool PipelineKeySet::AddExpanded(const std::string& a_tag, const PipelineWords& a_identity, const PipelineWords& a_inputs)
	{
		if (!Add(a_tag, a_identity, a_inputs))
			return false;
		expanded.emplace(a_tag, a_identity);
		return true;
	}

	bool PipelineKeySet::Contains(const std::string& a_tag, const PipelineWords& a_identity) const
	{
		const auto tagLines = lines.find(a_tag);
		return tagLines != lines.end() && tagLines->second.contains(a_identity);
	}

	void CollectPipelineKeys(PipelineKeySource& a_source, PipelineKeySet& a_set)
	{
		const std::size_t before = a_set.Size();
		a_source.Collect(a_set);
		a_set.NoteSource(a_source.Name(), a_set.Size() - before);
	}

	PipelineManifest& PipelineManifest::Get()
	{
		// Never destroyed, because a build may record a key while the process tears down.
		static auto* manifest = new PipelineManifest(L"", "pipeline manifest");
		return *manifest;
	}

	PipelineManifest& PipelineManifest::Unbuildable()
	{
		static auto* unbuildable = new PipelineManifest(L"-unbuildable", "unbuildable pipeline list");
		return *unbuildable;
	}

	void PipelineManifest::LoadOnce()
	{
		if (loaded)
			return;
		loaded = true;
		const auto [read, skipped] = PipelineManifestDetail::Parse(PipelineManifestDetail::LocalPath(suffix), lines);
		logger::info("[DCLF] {}: {} keys listed{}", label, read, skipped ? std::format(" ({} malformed lines skipped)", skipped) : std::string());
	}

	void PipelineManifest::Record(const std::string& a_tag, const PipelineWords& a_identity, const PipelineWords& a_inputs)
	{
		{
			std::scoped_lock lock(mutex);
			LoadOnce();
			auto& tagLines = lines[a_tag];
			const auto found = tagLines.find(a_identity);
			if (found != tagLines.end()) {
				if (found->second == a_inputs)
					return;
				found->second = a_inputs;
			} else {
				if (tagLines.size() >= kMaxPipelineKeysPerTag)
					return;
				tagLines.emplace(a_identity, a_inputs);
			}
		}
		SaveSoon();
	}

	void PipelineManifest::Forget(const std::string& a_tag, const PipelineWords& a_identity)
	{
		{
			std::scoped_lock lock(mutex);
			LoadOnce();
			const auto tagLines = lines.find(a_tag);
			if (tagLines == lines.end() || !tagLines->second.erase(a_identity))
				return;
		}
		SaveSoon();
	}

	PipelineManifestLines PipelineManifest::Lines()
	{
		std::scoped_lock lock(mutex);
		LoadOnce();
		return lines;
	}

	void PipelineManifest::SaveSoon()
	{
		// One save at a time. Keys recorded during a save are written by the next one.
		if (saveQueued.exchange(true))
			return;
		std::thread([this] {
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
			// Keys come in bursts after a load, so a save waits a moment to take the rest of the burst.
			std::this_thread::sleep_for(std::chrono::seconds(2));
			saveQueued.store(false);
			Save();
		}).detach();
	}

	void PipelineManifest::Save()
	{
		std::scoped_lock saving(saveMutex);
		std::string text;
		{
			std::scoped_lock lock(mutex);
			text = PipelineManifestDetail::Format(lines);
		}
		std::error_code ec;
		const auto path = PipelineManifestDetail::LocalPath(suffix);
		std::filesystem::create_directories(path.parent_path(), ec);
		auto temporary = path;
		temporary += L".tmp";
		{
			std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
			if (!out) {
				logger::warn("[DCLF] {}: {} could not be written", label, temporary.string());
				return;
			}
			out << text;
		}
		std::filesystem::rename(temporary, path, ec);
		if (ec)
			logger::warn("[DCLF] {}: {} could not be replaced: {}", label, path.string(), ec.message());
	}

	void LocalPipelineManifestSource::Collect(PipelineKeySet& a_set)
	{
		for (const auto& [tag, tagLines] : PipelineManifest::Get().Lines())
			for (const auto& [identity, inputs] : tagLines)
				a_set.Add(tag, identity, inputs);
	}

	void ShippedPipelineManifestSource::Collect(PipelineKeySet& a_set)
	{
		const auto path = PipelineManifestDetail::ShippedPath();
		std::error_code ec;
		if (!std::filesystem::exists(path, ec))
			return;
		PipelineManifestLines shipped;
		const auto [read, skipped] = PipelineManifestDetail::Parse(path, shipped);
		logger::info("[DCLF] shipped pipeline manifest: {} keys listed{}", read, skipped ? std::format(" ({} malformed lines skipped)", skipped) : std::string());
		for (const auto& [tag, tagLines] : shipped)
			for (const auto& [identity, inputs] : tagLines)
				a_set.Add(tag, identity, inputs);
	}
}
