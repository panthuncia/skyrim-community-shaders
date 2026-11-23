#pragma once

// Where the startup pipeline step gets its keys: the local manifest of keys sessions built, and a shipped copy.
// See docs/development/plans/precompile-pipelines.md.
//
// A manifest line is a tag, a tab, the key's identity words, a "|" token, then the resolved inputs its build needs:
//   <tag>\t<identity words> | <input words>
// Words are decimal. A line that does not parse is skipped. The tag is "shadow" for a shadow key, else the main key's family
// ("lighting", or a program family's key name). The pipeline lane decodes the words (DrawPipelines::Impl).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace DCLF
{
	/** @brief A manifest line's words. */
	using PipelineWords = std::vector<std::uint32_t>;

	/** @brief Lines by tag, each identity once, with its inputs. */
	using PipelineManifestLines = std::map<std::string, std::map<PipelineWords, PipelineWords>>;

	/** @brief The tag of shadow keys. */
	inline constexpr std::string_view kShadowPipelineTag = "shadow";

	/** @brief Keys per tag a manifest or key set holds, so a broken file cannot make startup build without bound. */
	inline constexpr std::size_t kMaxPipelineKeysPerTag = 4096;

	/** @brief The keys every source gave, each once, capped per tag. The first source to give a key supplies its inputs. */
	class PipelineKeySet
	{
	public:
		/** @brief Adds a line. False when the identity is already in or its tag is full. */
		bool Add(const std::string& a_tag, const PipelineWords& a_identity, const PipelineWords& a_inputs);

		const PipelineManifestLines& Lines() const { return lines; }
		std::size_t Size() const { return size; }

		/** @brief Each source's name and the lines it added, in collection order. */
		const std::vector<std::pair<std::string, std::size_t>>& Sources() const { return sources; }
		/** @brief Counts a_added lines under a source's name. */
		void NoteSource(std::string_view a_name, std::size_t a_added) { sources.emplace_back(std::string(a_name), a_added); }

	private:
		PipelineManifestLines lines;
		std::size_t size = 0;
		std::vector<std::pair<std::string, std::size_t>> sources;
	};

	/** @brief A source of pipeline keys. Later sources: a combination expander, and keys from the game's forms. */
	class PipelineKeySource
	{
	public:
		virtual ~PipelineKeySource() = default;
		/** @brief A short name for the summary line. */
		virtual std::string_view Name() const = 0;
		/** @brief Adds this source's keys to a_set. */
		virtual void Collect(PipelineKeySet& a_set) = 0;
	};

	/** @brief Runs a_source into a_set and counts the lines it added under its name. */
	void CollectPipelineKeys(PipelineKeySource& a_source, PipelineKeySet& a_set);

	/**
	 * @brief The local manifest, Data/ShaderCache/v3/dclf-pipelines-<backend>.txt. Loaded once, on first use. Any thread.
	 * A key is recorded when it is first built and dropped when its build fails.
	 */
	class PipelineManifest
	{
	public:
		static PipelineManifest& Get();

		/** @brief Lists a line, or replaces its inputs. Saved soon after on a thread of its own when it changed. */
		void Record(const std::string& a_tag, const PipelineWords& a_identity, const PipelineWords& a_inputs);
		/** @brief Drops a line, if listed. Saved soon after when it was. */
		void Forget(const std::string& a_tag, const PipelineWords& a_identity);
		/** @brief A copy of every line. */
		PipelineManifestLines Lines();

	private:
		PipelineManifest() = default;
		void LoadOnce();
		void SaveSoon();
		void Save();

		std::mutex mutex;
		std::mutex saveMutex;
		std::atomic<bool> saveQueued{ false };
		bool loaded = false;
		PipelineManifestLines lines;
	};

	/** @brief The local manifest as a key source. */
	class LocalPipelineManifestSource final : public PipelineKeySource
	{
	public:
		std::string_view Name() const override { return "local"; }
		void Collect(PipelineKeySet& a_set) override;
	};

	/** @brief The shipped manifest, Data/ShaderCache/pack/dclf-pipelines-<backend>.txt, read-only. Nothing when absent. */
	class ShippedPipelineManifestSource final : public PipelineKeySource
	{
	public:
		std::string_view Name() const override { return "shipped"; }
		void Collect(PipelineKeySet& a_set) override;
	};
}
