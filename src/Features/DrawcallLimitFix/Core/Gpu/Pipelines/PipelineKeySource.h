#pragma once

// Where the startup pipeline step gets its keys: the local manifest of keys sessions built, and a shipped copy.
//
// A manifest line is a tag, a tab, the key's identity words, a "|" token, then the resolved inputs its build needs:
//   <tag>\t<identity words> | <input words>
// Words are decimal. A line that does not parse is skipped. The tag is "shadow" for a shadow key, "forward" for a forward key,
// "refraction" for the refraction pipeline, "draw-family-<key name>" for a draw family's pipelines, else the main key's family
// ("lighting", or a program family's key name). The pipeline lane decodes the words (DrawPipelines::Impl).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
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
	/** @brief The tag of forward keys. */
	inline constexpr std::string_view kForwardPipelineTag = "forward";
	/** @brief The tag of the refraction pipeline. */
	inline constexpr std::string_view kRefractionPipelineTag = "refraction";
	/** @brief The start of a draw family's tag. Its key name follows. */
	inline constexpr std::string_view kDrawFamilyPipelineTagPrefix = "draw-family-";

	/** @brief Whether a tag names a main key (Lighting or a program family), not a shadow, forward, refraction or draw family line. */
	inline bool IsMainPipelineTag(std::string_view a_tag)
	{
		return a_tag != kShadowPipelineTag && a_tag != kForwardPipelineTag && a_tag != kRefractionPipelineTag && !a_tag.starts_with(kDrawFamilyPipelineTagPrefix);
	}

	// A main line's identity: the key (8 words: VS, PS, raster, pass, layout low and high, runtime VS and PS), the main targets
	// (count, 8 formats, depth) and the winding.
	inline constexpr std::size_t kMainManifestKeyWords = 8;
	inline constexpr std::size_t kMainManifestIdentityWords = kMainManifestKeyWords + 10 + 1;
	// A main line's inputs: the state bits, depth bias (3), alpha to coverage, independent blend, attachments, then 8 per attachment.
	inline constexpr std::size_t kManifestAttachmentWords = 8;
	inline constexpr std::size_t kManifestStateWords = 1 + 6 + 8 * kManifestAttachmentWords;
	// A shadow line's identity: the key without its view state (technique, raster, layout low and high), the shadow map format and
	// the view's rasterizer state (6).
	inline constexpr std::size_t kShadowManifestIdentityWords = 4 + 1 + 6;

	/** @brief Keys per tag a manifest or key set holds, so a broken file cannot make startup build without bound. */
	inline constexpr std::size_t kMaxPipelineKeysPerTag = 4096;

	/** @brief The keys every source gave, each once, capped per tag. The first source to give a key supplies its inputs. */
	class PipelineKeySet
	{
	public:
		/** @brief Adds a line. False when the identity is already in or its tag is full. */
		bool Add(const std::string& a_tag, const PipelineWords& a_identity, const PipelineWords& a_inputs);
		/**
		 * @brief Adds a line no session built, guessed from the others. Its build may fail, which is expected: it is then listed as
		 * unbuildable instead of logged. A built one joins the local manifest only when play asks for it.
		 */
		bool AddExpanded(const std::string& a_tag, const PipelineWords& a_identity, const PipelineWords& a_inputs);
		/** @brief Whether AddExpanded added the line. */
		bool IsExpanded(const std::string& a_tag, const PipelineWords& a_identity) const { return expanded.contains({ a_tag, a_identity }); }
		/** @brief Whether the identity is in under its tag. */
		bool Contains(const std::string& a_tag, const PipelineWords& a_identity) const;

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
		std::set<std::pair<std::string, PipelineWords>> expanded;
	};

	/**
	 * @brief A source of pipeline keys. Sources run in order, each seeing what the earlier ones added: the manifests, then the
	 * combination expander (PipelineExpander.h). A later source of keys from the game's forms plugs in the same way.
	 */
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
		/**
		 * @brief The expanded keys whose build failed, dclf-pipelines-<backend>-unbuildable.txt beside the manifest, so the expander
		 * does not offer them again each start. Same line format, with no inputs. Deleting the file offers them again.
		 */
		static PipelineManifest& Unbuildable();

		/** @brief Lists a line, or replaces its inputs. Saved soon after on a thread of its own when it changed. */
		void Record(const std::string& a_tag, const PipelineWords& a_identity, const PipelineWords& a_inputs);
		/** @brief Drops a line, if listed. Saved soon after when it was. */
		void Forget(const std::string& a_tag, const PipelineWords& a_identity);
		/** @brief A copy of every line. */
		PipelineManifestLines Lines();

	private:
		PipelineManifest(std::wstring a_suffix, std::string a_label) : suffix(std::move(a_suffix)), label(std::move(a_label)) {}
		void LoadOnce();
		void SaveSoon();
		void Save();

		std::mutex mutex;
		std::mutex saveMutex;
		std::atomic<bool> saveQueued{ false };
		bool loaded = false;
		PipelineManifestLines lines;
		std::wstring suffix;  // after the backend in the file name
		std::string label;    // for the log
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
