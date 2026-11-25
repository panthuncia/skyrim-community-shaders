// DclfPortalBench: the portal culling variants (the production inline evaluation, the hand-written flattened baseline and the
// experimental evaluators) timed on the GPU over real and synthetic corpora, each variant's verdicts checked bit for bit against the
// CPU oracle (PortalModel::Visible). See CMakeLists.txt for the build and docs/development/dclf-portal-query-batches.md for the results.
//
//   DclfPortalBench [--synthetic] [--corpus <file.dclfportal or directory>]... [--reps N] [--warmup N] [--scale F]
//                   [--variants id,id,...] [--csv <file>] [--save <directory>] [--programs-only] [--debug]

#include "AuxTables.h"
#include "Corpus.h"
#include "Gpu.h"
#include "Synthetic.h"

#include "Features/DrawcallLimitFix/Features/Portals/PortalBatchLayout.h"

#include <filesystem>
#include <optional>
#include <string_view>

namespace
{
	using namespace Bench;

	enum class AuxKind
	{
		None,
		Flat,
		PlaneBits
	};
	struct Variant
	{
		const char* id;
		const char* description;
		const char* evaluate;  // the SPIR-V
		AuxKind aux;
	};
	constexpr Variant kVariants[]{
		{ "V0", "floor: queries read, verdicts written, nothing evaluated", "inline-floor", AuxKind::None },
		{ "V3", "production: inline (one thread per query), short-circuit interpreter", "inline", AuxKind::None },
		{ "V3u", "inline, uniform instruction scan", "inline-uniform", AuxKind::None },
		{ "V4", "hand-written: flattened accepting paths, inline", "inline-flat", AuxKind::Flat },
		{ "B1", "plane bits: distinct planes once, the program on bits, inline", "inline-planebits", AuxKind::PlaneBits },
	};

	/** @brief PortalBatchPass's constants (PortalBatchCS's cbuffer), with the bench's aux index. */
	struct Constants
	{
		std::uint32_t queries, results, programs, traces, queryCount;
		std::uint32_t latchIndex, latchOffset, auxIndex, traceFrame, padding[3];
	};
	static_assert(sizeof(Constants) == Gpu::kConstantWords * 4);

	struct Programs
	{
		std::map<std::string, GpuProgram> evaluate;
	};

	std::string Spirv(const std::string& a_name) { return std::string(DCLF_PORTAL_BENCH_SPIRV) + "/" + a_name + ".spv"; }

	struct Options
	{
		bool synthetic = false, debug = false, programsOnly = false;
		std::vector<std::string> corpora;
		std::uint32_t reps = 200, warmup = 20;
		float scale = 1.0f;
		std::vector<std::string> variants;
		std::string csv, save;
	};

	Options Parse(int argc, char** argv)
	{
		Options o;
		for (int i = 1; i < argc; ++i) {
			const std::string a = argv[i];
			const auto next = [&]() -> std::string {
				if (i + 1 >= argc)
					throw std::runtime_error("missing value after " + a);
				return argv[++i];
			};
			if (a == "--synthetic")
				o.synthetic = true;
			else if (a == "--debug")
				o.debug = true;
			else if (a == "--programs-only")
				o.programsOnly = true;
			else if (a == "--corpus")
				o.corpora.push_back(next());
			else if (a == "--reps")
				o.reps = static_cast<std::uint32_t>(std::stoul(next()));
			else if (a == "--warmup")
				o.warmup = static_cast<std::uint32_t>(std::stoul(next()));
			else if (a == "--scale")
				o.scale = std::stof(next());
			else if (a == "--csv")
				o.csv = next();
			else if (a == "--save")
				o.save = next();
			else if (a == "--variants") {
				std::string list = next();
				for (std::size_t start = 0; start <= list.size();) {
					const auto end = std::min(list.find(',', start), list.size());
					if (end > start)
						o.variants.push_back(list.substr(start, end - start));
					start = end + 1;
				}
			} else
				throw std::runtime_error("unknown argument " + a);
		}
		if (!o.synthetic && o.corpora.empty())
			o.synthetic = true;
		return o;
	}

	struct Measured
	{
		double median = 0, p10 = 0, p90 = 0, min = 0;
		std::uint64_t mismatches = 0;
		std::string firstMismatch;
	};

	double Percentile(std::vector<double> a_values, double a_p)
	{
		std::sort(a_values.begin(), a_values.end());
		const auto i = static_cast<std::size_t>(a_p * static_cast<double>(a_values.size() - 1) + 0.5);
		return a_values[std::min(i, a_values.size() - 1)];
	}

	class Runner
	{
	public:
		Runner(Gpu& a_gpu, const Options& a_options) :
			gpu(a_gpu), options(a_options)
		{
			for (const auto& v : kVariants)
				if (!programs.evaluate.contains(v.evaluate))
					programs.evaluate[v.evaluate] = gpu.Load(Spirv(v.evaluate), "Inline");
		}

		void Run(const Corpus& a_corpus, std::FILE* a_csv)
		{
			std::vector<std::uint32_t> expected(a_corpus.queries.size());
			for (std::size_t i = 0; i < expected.size(); ++i)
				expected[i] = Expected(a_corpus, a_corpus.queries[i]);
			const Stats s = Measure(a_corpus, expected);
			AuxReport flatReport, bitsReport;
			const auto flat = BuildFlat(a_corpus.words, 32, flatReport);
			const auto bits = BuildPlaneBits(a_corpus.words, bitsReport);
			std::printf("\n== %s (%s)\n", a_corpus.name.c_str(), a_corpus.metadata.c_str());
			std::printf("   programs %u (%u reached, %u with operators): %u records, %u sets (max %u records, %u sets, path %u)\n", s.programs, s.reached, s.withOps,
				s.records, s.sets, s.maxRecords, s.maxSets, s.maxPath);
			std::printf("   queries %u: %u immediate, %u evaluated (%u against a program with operators), %u accepted\n", s.queries, s.immediate, s.evaluated,
				s.programQueries, s.accepted);
			std::printf("   flat: %u of %u programs (%llu terms, %llu literals, max %u terms); plane bits: %u of %u (%llu distinct of %llu active planes, max %u)\n",
				flatReport.covered, flatReport.programs, static_cast<unsigned long long>(flatReport.terms), static_cast<unsigned long long>(flatReport.literals),
				flatReport.maxTerms, bitsReport.covered, bitsReport.programs, static_cast<unsigned long long>(bitsReport.planes),
				static_cast<unsigned long long>(bitsReport.slots), bitsReport.maxPlanes);

			gpu.ResetHeap();
			const std::uint32_t queryCount = static_cast<std::uint32_t>(a_corpus.queries.size());
			const auto words = gpu.Create(a_corpus.words.size() * 4, 0, false, "programs");
			const auto queries = gpu.Create(std::uint64_t(queryCount) * sizeof(Query), sizeof(Query), false, "queries");
			const auto results = gpu.Create(std::uint64_t(queryCount) * 8, 8, true, "results");
			const auto flatBuffer = gpu.Create(flat.size() * 4, 0, false, "flat");
			const auto bitsBuffer = gpu.Create(bits.size() * 4, 0, false, "plane bits");
			gpu.Upload(words, a_corpus.words.data(), a_corpus.words.size() * 4);
			gpu.Upload(queries, a_corpus.queries.data(), std::uint64_t(queryCount) * sizeof(Query));
			gpu.Upload(flatBuffer, flat.data(), flat.size() * 4);
			gpu.Upload(bitsBuffer, bits.data(), bits.size() * 4);
			const std::vector<std::uint32_t> poison(std::size_t(queryCount) * 2, 0xDEADBEEFu);

			for (const auto& v : kVariants) {
				if (!options.variants.empty() && std::find(options.variants.begin(), options.variants.end(), v.id) == options.variants.end())
					continue;
				Constants c{};
				c.queries = queries.srv;
				c.results = results.uav;
				c.programs = words.srv;
				c.queryCount = queryCount;
				c.auxIndex = v.aux == AuxKind::Flat ? flatBuffer.srv : v.aux == AuxKind::PlaneBits ? bitsBuffer.srv : 0u;
				const auto& evaluate = programs.evaluate.at(v.evaluate);
				const auto body = [&](rhi::CommandList& a_list) {
					const auto shape = DCLF::Portals::DispatchFor(queryCount, 64);
					if (shape.x) {
						gpu.Bind(a_list, evaluate, reinterpret_cast<const std::uint32_t*>(&c));
						a_list.Dispatch(shape.x, shape.y, 1);
					}
				};
				gpu.Upload(results, poison.data(), poison.size() * 4);
				if (options.warmup)
					(void)gpu.Time(options.warmup, body);
				const auto times = gpu.Time(options.reps, body);
				Measured m;
				m.median = Percentile(times, 0.5);
				m.p10 = Percentile(times, 0.1);
				m.p90 = Percentile(times, 0.9);
				m.min = *std::min_element(times.begin(), times.end());
				const auto got = gpu.Read(results, std::uint64_t(queryCount) * 8);
				// The floor evaluates nothing: its verdicts are not the oracle's.
				for (std::uint32_t i = 0; i < queryCount && std::string_view(v.id) != "V0"; ++i) {
					const std::uint32_t generation = got[2 * i], verdict = got[2 * i + 1];
					if (generation == a_corpus.queries[i].generation && verdict == expected[i])
						continue;
					if (!m.mismatches++) {
						const auto& q = a_corpus.queries[i];
						m.firstMismatch = "query " + std::to_string(i) + " program " + std::to_string(q.program) + " flags " + std::to_string(q.flags) + ": expected " +
						                  std::to_string(static_cast<std::int32_t>(expected[i])) + ", got " + std::to_string(static_cast<std::int32_t>(verdict)) +
						                  " (generation " + std::to_string(generation) + ")";
					}
				}
				const double perQuery = queryCount ? m.median * 1000.0 / queryCount : 0.0;
				std::printf("   %-4s %8.2f us median (p10 %8.2f, p90 %8.2f, min %8.2f)  %6.2f ns/query  %s%s  %s\n", v.id, m.median, m.p10, m.p90, m.min, perQuery,
					std::string_view(v.id) == "V0" ? "n/a  " : m.mismatches ? "MISMATCHES " : "exact", m.mismatches ? std::to_string(m.mismatches).c_str() : "", v.description);
				if (m.mismatches)
					std::printf("        first: %s\n", m.firstMismatch.c_str());
				if (a_csv)
					std::fprintf(a_csv, "%s,%s,%u,%u,%u,%u,%u,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%u,%u,%u,%u\n", a_corpus.name.c_str(), v.id, s.queries, s.evaluated,
						s.programQueries, s.programs, s.withOps, s.records, s.sets, s.maxPath, m.median, m.p10, m.p90, m.min, perQuery,
						static_cast<unsigned long long>(m.mismatches), flatReport.covered, flatReport.fallback, bitsReport.covered, bitsReport.fallback);
				failures += m.mismatches ? 1 : 0;
			}
		}

		std::uint32_t Failures() const { return failures; }

	private:
		Gpu& gpu;
		const Options& options;
		Programs programs;
		std::uint32_t failures = 0;
	};
}

int main(int argc, char** argv)
{
	try {
		const Options options = Parse(argc, argv);
		std::vector<Corpus> corpora;
		if (options.synthetic)
			for (auto p : Synthetic::Standard()) {
				p.objectsPerRoom = std::max(1u, static_cast<std::uint32_t>(static_cast<float>(p.objectsPerRoom) * options.scale));
				corpora.push_back(Synthetic::Make(p));
			}
		for (const auto& path : options.corpora) {
			if (std::filesystem::is_directory(path)) {
				for (const auto& entry : std::filesystem::directory_iterator(path))
					if (entry.path().extension() == ".dclfportal")
						corpora.push_back(Load(entry.path().string()));
			} else {
				corpora.push_back(Load(path));
			}
		}
		// --programs-only: each corpus's queries against programs with operators, repeated to 64k (the evaluators' own cost).
		if (options.programsOnly) {
			std::vector<Corpus> only;
			for (const auto& c : corpora)
				if (auto p = Synthetic::ProgramsOnly(c, 65536); !p.queries.empty())
					only.push_back(std::move(p));
			corpora = std::move(only);
		}
		if (!options.save.empty()) {
			std::filesystem::create_directories(options.save);
			for (const auto& c : corpora)
				Save(c, options.save + "/" + c.name + ".dclfportal");
		}
		Gpu gpu(options.debug);
		std::printf("DclfPortalBench: %zu corpora, %u repetitions (%u warm-up), %s queue, %llu ticks/s\n", corpora.size(), options.reps, options.warmup, gpu.QueueName(),
			static_cast<unsigned long long>(gpu.TicksPerSecond()));
		std::FILE* csv = nullptr;
		if (!options.csv.empty()) {
			csv = std::fopen(options.csv.c_str(), "w");
			if (!csv)
				throw std::runtime_error("cannot write " + options.csv);
			std::fprintf(csv,
				"corpus,variant,queries,evaluated,program_queries,programs,with_ops,records,sets,max_path,median_us,p10_us,p90_us,min_us,ns_per_query,mismatches,"
				"flat_covered,flat_fallback,bits_covered,bits_fallback\n");
		}
		Runner runner(gpu, options);
		for (const auto& c : corpora)
			runner.Run(c, csv);
		if (csv)
			std::fclose(csv);
		if (runner.Failures()) {
			std::fprintf(stderr, "\n%u variant runs disagreed with the oracle\n", runner.Failures());
			return 2;
		}
		std::printf("\nevery variant agreed with the oracle\n");
		return 0;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "DclfPortalBench: %s\n", e.what());
		return 1;
	}
}
