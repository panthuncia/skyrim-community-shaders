// The engine draw families' programs (EngineDrawProgram.h).

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawProgram.h"

#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Pipeline/StateConversion.h"
#include "RenderGraph/ProgramCache.h"
#include "RenderGraph/RasterProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "ShaderCache.h"
#include "Utils/Logging.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <tuple>

namespace DCLF::Family
{
	namespace Pipeline = ::RenderGraph::Engine::Pipeline;
	namespace Detail = ::RenderGraph::ProgramCacheDetail;

	namespace
	{
		// The manifest of the keys sessions drew with, next to the shader store (Util::SharedShaderStore: Data/ShaderCache/v2). The store
		// touches only its own index and "cas" folder there, and the old-cache cleanup keeps the v2 folder.
		constexpr const wchar_t* kManifestPath = L"Data/ShaderCache/v2/engine-draw-programs.txt";
		// A manifest line: the family's tag, a tab, then the key's words (KeyWords).
		constexpr std::size_t kKeyWords = 6 + 4 + kBlendWords + kDepthStencilWords + kRasterWords;
		// The most keys of one family the manifest keeps: a manifest grown past it (a broken file, a mod changing states every frame)
		// stops adding, so a start never builds without bound.
		constexpr std::size_t kMaxManifestKeys = 512;
		// How long a draw waits for a prebuilt program before it gives up (the build thread stopped, a compile hangs): logged as an error.
		constexpr auto kLongestWait = std::chrono::seconds(60);

		enum class Status : std::uint8_t
		{
			Building,
			Ready,
			Failed,
		};

		struct Entry
		{
			std::shared_ptr<const RasterProgram> program;  // set under Impl::mutex with status
			Status status = Status::Building;
		};

		/** @brief The family's name as a manifest tag: no spaces or tabs. */
		std::string TagOf(std::string_view a_family)
		{
			std::string tag(a_family);
			std::ranges::replace(tag, ' ', '-');
			std::ranges::replace(tag, '\t', '-');
			return tag;
		}

		std::uint32_t Bits(float a_value) { return std::bit_cast<std::uint32_t>(a_value); }
		float Float(std::uint32_t a_value) { return std::bit_cast<float>(a_value); }

		void EncodeBlend(const D3D11_BLEND_DESC& a_desc, std::array<std::uint32_t, kBlendWords>& a_out)
		{
			a_out[0] = a_desc.AlphaToCoverageEnable ? 1 : 0;
			a_out[1] = a_desc.IndependentBlendEnable ? 1 : 0;
			for (std::size_t i = 0; i < 8; ++i) {
				const auto& t = a_desc.RenderTarget[i];
				auto* w = &a_out[2 + i * 8];
				w[0] = t.BlendEnable ? 1 : 0;
				w[1] = t.SrcBlend;
				w[2] = t.DestBlend;
				w[3] = t.BlendOp;
				w[4] = t.SrcBlendAlpha;
				w[5] = t.DestBlendAlpha;
				w[6] = t.BlendOpAlpha;
				w[7] = t.RenderTargetWriteMask;
			}
		}

		D3D11_BLEND_DESC DecodeBlend(const std::array<std::uint32_t, kBlendWords>& a_words)
		{
			D3D11_BLEND_DESC desc{};
			desc.AlphaToCoverageEnable = a_words[0] != 0;
			desc.IndependentBlendEnable = a_words[1] != 0;
			for (std::size_t i = 0; i < 8; ++i) {
				auto& t = desc.RenderTarget[i];
				const auto* w = &a_words[2 + i * 8];
				t.BlendEnable = w[0] != 0;
				t.SrcBlend = static_cast<D3D11_BLEND>(w[1]);
				t.DestBlend = static_cast<D3D11_BLEND>(w[2]);
				t.BlendOp = static_cast<D3D11_BLEND_OP>(w[3]);
				t.SrcBlendAlpha = static_cast<D3D11_BLEND>(w[4]);
				t.DestBlendAlpha = static_cast<D3D11_BLEND>(w[5]);
				t.BlendOpAlpha = static_cast<D3D11_BLEND_OP>(w[6]);
				t.RenderTargetWriteMask = static_cast<UINT8>(w[7]);
			}
			return desc;
		}

		void EncodeDepthStencil(const D3D11_DEPTH_STENCIL_DESC& a_desc, std::array<std::uint32_t, kDepthStencilWords>& a_out)
		{
			a_out = { static_cast<std::uint32_t>(a_desc.DepthEnable ? 1 : 0), static_cast<std::uint32_t>(a_desc.DepthWriteMask), static_cast<std::uint32_t>(a_desc.DepthFunc),
				static_cast<std::uint32_t>(a_desc.StencilEnable ? 1 : 0), a_desc.StencilReadMask, a_desc.StencilWriteMask,
				static_cast<std::uint32_t>(a_desc.FrontFace.StencilFailOp), static_cast<std::uint32_t>(a_desc.FrontFace.StencilDepthFailOp),
				static_cast<std::uint32_t>(a_desc.FrontFace.StencilPassOp), static_cast<std::uint32_t>(a_desc.FrontFace.StencilFunc),
				static_cast<std::uint32_t>(a_desc.BackFace.StencilFailOp), static_cast<std::uint32_t>(a_desc.BackFace.StencilDepthFailOp),
				static_cast<std::uint32_t>(a_desc.BackFace.StencilPassOp), static_cast<std::uint32_t>(a_desc.BackFace.StencilFunc) };
		}

		D3D11_DEPTH_STENCIL_DESC DecodeDepthStencil(const std::array<std::uint32_t, kDepthStencilWords>& a_words)
		{
			D3D11_DEPTH_STENCIL_DESC desc{};
			desc.DepthEnable = a_words[0] != 0;
			desc.DepthWriteMask = static_cast<D3D11_DEPTH_WRITE_MASK>(a_words[1]);
			desc.DepthFunc = static_cast<D3D11_COMPARISON_FUNC>(a_words[2]);
			desc.StencilEnable = a_words[3] != 0;
			desc.StencilReadMask = static_cast<UINT8>(a_words[4]);
			desc.StencilWriteMask = static_cast<UINT8>(a_words[5]);
			desc.FrontFace = { static_cast<D3D11_STENCIL_OP>(a_words[6]), static_cast<D3D11_STENCIL_OP>(a_words[7]), static_cast<D3D11_STENCIL_OP>(a_words[8]),
				static_cast<D3D11_COMPARISON_FUNC>(a_words[9]) };
			desc.BackFace = { static_cast<D3D11_STENCIL_OP>(a_words[10]), static_cast<D3D11_STENCIL_OP>(a_words[11]), static_cast<D3D11_STENCIL_OP>(a_words[12]),
				static_cast<D3D11_COMPARISON_FUNC>(a_words[13]) };
			return desc;
		}

		void EncodeRaster(const D3D11_RASTERIZER_DESC& a_desc, std::array<std::uint32_t, kRasterWords>& a_out)
		{
			a_out = { static_cast<std::uint32_t>(a_desc.FillMode), static_cast<std::uint32_t>(a_desc.CullMode), static_cast<std::uint32_t>(a_desc.FrontCounterClockwise ? 1 : 0),
				static_cast<std::uint32_t>(a_desc.DepthBias), Bits(a_desc.DepthBiasClamp), Bits(a_desc.SlopeScaledDepthBias),
				static_cast<std::uint32_t>(a_desc.DepthClipEnable ? 1 : 0), static_cast<std::uint32_t>(a_desc.ScissorEnable ? 1 : 0),
				static_cast<std::uint32_t>(a_desc.MultisampleEnable ? 1 : 0), static_cast<std::uint32_t>(a_desc.AntialiasedLineEnable ? 1 : 0) };
		}

		D3D11_RASTERIZER_DESC DecodeRaster(const std::array<std::uint32_t, kRasterWords>& a_words)
		{
			D3D11_RASTERIZER_DESC desc{};
			desc.FillMode = static_cast<D3D11_FILL_MODE>(a_words[0]);
			desc.CullMode = static_cast<D3D11_CULL_MODE>(a_words[1]);
			desc.FrontCounterClockwise = a_words[2] != 0;
			desc.DepthBias = static_cast<INT>(a_words[3]);
			desc.DepthBiasClamp = Float(a_words[4]);
			desc.SlopeScaledDepthBias = Float(a_words[5]);
			desc.DepthClipEnable = a_words[6] != 0;
			desc.ScissorEnable = a_words[7] != 0;
			desc.MultisampleEnable = a_words[8] != 0;
			desc.AntialiasedLineEnable = a_words[9] != 0;
			return desc;
		}

		/** @brief A key as the manifest's words. */
		std::vector<std::uint32_t> WordsOf(const ProgramKey& a_key)
		{
			std::vector<std::uint32_t> words{ a_key.descriptor, static_cast<std::uint32_t>(a_key.defineStage), a_key.variant, a_key.hasDepth, a_key.depthFormat,
				a_key.maskedTargets };
			words.insert(words.end(), a_key.colourFormats.begin(), a_key.colourFormats.end());
			words.insert(words.end(), a_key.blend.begin(), a_key.blend.end());
			words.insert(words.end(), a_key.depthStencil.begin(), a_key.depthStencil.end());
			words.insert(words.end(), a_key.raster.begin(), a_key.raster.end());
			return words;
		}

		/** @brief A key from the manifest's words; nothing for a malformed line. */
		std::optional<ProgramKey> KeyOf(std::span<const std::uint32_t> a_words)
		{
			if (a_words.size() != kKeyWords || a_words[1] > static_cast<std::uint32_t>(DefineStage::Pixel) || a_words[3] > 1)
				return std::nullopt;
			ProgramKey key{};
			key.descriptor = a_words[0];
			key.defineStage = static_cast<DefineStage>(a_words[1]);
			key.variant = a_words[2];
			key.hasDepth = a_words[3];
			key.depthFormat = a_words[4];
			key.maskedTargets = a_words[5];
			auto at = a_words.begin() + 6;
			const auto take = [&](auto& a_array) {
				std::copy_n(at, a_array.size(), a_array.begin());
				at += a_array.size();
			};
			take(key.colourFormats);
			take(key.blend);
			take(key.depthStencil);
			take(key.raster);
			return key;
		}

		/** @brief The keys sessions drew with, by family tag. Loaded once; saved on ProgramCache's build thread when a key is added. */
		class Manifest
		{
		public:
			static Manifest& Get()
			{
				static Manifest manifest;
				return manifest;
			}

			/** @brief The keys listed for a_tag. */
			std::vector<ProgramKey> Of(const std::string& a_tag)
			{
				std::scoped_lock lock(mutex);
				LoadOnce();
				const auto found = keys.find(a_tag);
				return found == keys.end() ? std::vector<ProgramKey>{} : std::vector<ProgramKey>(found->second.begin(), found->second.end());
			}

			/** @brief Lists a_key for a_tag; true when it was not listed (the caller saves). */
			bool Add(const std::string& a_tag, const ProgramKey& a_key)
			{
				std::scoped_lock lock(mutex);
				LoadOnce();
				auto& set = keys[a_tag];
				return set.size() < kMaxManifestKeys && set.insert(a_key).second;
			}

			/** @brief Writes every listed key (a temporary file, then renamed over the manifest). Any thread; the build thread in practice. */
			void Save()
			{
				std::string text;
				{
					std::scoped_lock lock(mutex);
					for (const auto& [tag, set] : keys)
						for (const auto& key : set) {
							text += tag;
							text += '\t';
							for (const auto word : WordsOf(key))
								text += std::format("{} ", word);
							text += '\n';
						}
				}
				std::error_code ec;
				const std::filesystem::path path(kManifestPath);
				std::filesystem::create_directories(path.parent_path(), ec);
				auto temporary = path;
				temporary += L".tmp";
				{
					std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
					if (!out) {
						logger::warn("[DCLF] engine draw programs: the manifest could not be written ({})", Util::WStringToString(temporary.wstring()));
						return;
					}
					out << text;
				}
				std::filesystem::rename(temporary, path, ec);
				if (ec)
					logger::warn("[DCLF] engine draw programs: the manifest could not be replaced: {}", ec.message());
			}

		private:
			/** @brief Reads the manifest once; a malformed line is skipped (logged once with the count). */
			void LoadOnce()
			{
				if (loaded)
					return;
				loaded = true;
				std::ifstream in{ std::filesystem::path(kManifestPath) };
				if (!in)
					return;
				std::string line;
				std::size_t read = 0, skipped = 0;
				while (std::getline(in, line)) {
					const auto tab = line.find('\t');
					if (tab == std::string::npos || tab == 0) {
						skipped += !line.empty();
						continue;
					}
					std::vector<std::uint32_t> words;
					std::istringstream stream(line.substr(tab + 1));
					std::string token;
					bool ok = true;
					while (ok && stream >> token) {
						std::uint32_t value = 0;
						const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
						ok = result.ec == std::errc{} && result.ptr == token.data() + token.size();
						words.push_back(value);
					}
					const auto key = ok ? KeyOf(words) : std::nullopt;
					auto& set = keys[line.substr(0, tab)];
					if (!key || set.size() >= kMaxManifestKeys) {
						++skipped;
						continue;
					}
					set.insert(*key);
					++read;
				}
				logger::info("[DCLF] engine draw programs: the manifest lists {} keys{}", read, skipped ? std::format(" ({} malformed lines skipped)", skipped) : std::string());
			}

			std::mutex mutex;
			bool loaded = false;
			std::map<std::string, std::set<ProgramKey>> keys;
		};
	}

	struct EngineDrawPrograms::Impl
	{
		std::string family;
		std::string tag;
		ProgramSource source;
		// The family's shader object, from the first descriptor CS loaded for its type (PrecompileStages) or the first draw (Get). The
		// engine's shaders live for the session.
		std::atomic<const RE::BSShader*> shader{ nullptr };
		std::mutex mutex;
		std::condition_variable changed;  // an entry left Building
		std::map<ProgramKey, std::shared_ptr<Entry>> entries;
		std::atomic<std::size_t> built{ 0 };
		bool prebuilt = false;
		bool waitLogged = false;
		// The stage requests compiled or queued (PrecompileStages), each once.
		std::mutex stageMutex;
		std::set<StageRequest> stages;

		Impl(std::string a_family, ProgramSource a_source) :
			family(std::move(a_family)), tag(TagOf(family)), source(std::move(a_source)) {}

		/** @brief The defines a descriptor gives both stages (ShaderCache's for its stage, less VSHADER and PSHADER) and the variant's. */
		Defines DefinesOf(std::uint32_t a_descriptor, DefineStage a_stage, std::uint32_t a_variant, const RE::BSShader& a_shader) const
		{
			Defines defines;
			const auto stage = a_stage == DefineStage::Vertex ? SIE::ShaderClass::Vertex : SIE::ShaderClass::Pixel;
			for (const auto& [name, value] : SIE::ShaderCache::GetCompileDefines(a_shader, stage, a_descriptor)) {
				if (name == "VSHADER" || name == "PSHADER")
					continue;
				defines.emplace_back(std::wstring(name.begin(), name.end()), std::wstring(value.begin(), value.end()));
			}
			if (source.variantDefines) {
				auto own = source.variantDefines(a_variant);
				defines.insert(defines.end(), own.begin(), own.end());
			}
			return defines;
		}

		/** @brief The desc both stages compile from, without targets or states (the stage compile and the program share it). */
		RasterProgram::Desc StageDescOf(std::uint32_t a_descriptor, DefineStage a_stage, std::uint32_t a_variant, const RE::BSShader& a_shader) const
		{
			RasterProgram::Desc desc{};
			desc.pixelSource = source.source;
			desc.vertexSource = source.source;
			desc.languageVersion = source.languageVersion;
			desc.constantWords = source.constantWords;
			desc.defines = DefinesOf(a_descriptor, a_stage, a_variant, a_shader);
			return desc;
		}

		/** @brief The program's desc from its key alone (no live state object), defines resolved; nothing with a_why set when it has no ORG form. */
		std::optional<RasterProgram::Desc> DescOf(const ProgramKey& a_key, const RE::BSShader& a_shader, std::string& a_why) const
		{
			const auto blendDesc = DecodeBlend(a_key.blend);
			rhi::BlendState blend{};
			std::string error;
			if (!Pipeline::BlendFromD3D11(blendDesc, blend, error)) {
				a_why = std::format("its blend state has no ORG form ({})", error);
				return std::nullopt;
			}
			if (Pipeline::UsesDualSource(blendDesc)) {
				a_why = "its blend state reads a second source";
				return std::nullopt;
			}
			if (!a_key.colourFormats[0] && !a_key.depthFormat) {
				a_why = "no target";
				return std::nullopt;
			}
			auto desc = StageDescOf(a_key.descriptor, a_key.defineStage, a_key.variant, a_shader);
			for (std::size_t i = 0; i < a_key.colourFormats.size() && a_key.colourFormats[i]; ++i) {
				auto attachment = blend.attachments[blend.independentBlend ? i : 0];
				if ((a_key.maskedTargets >> i) & 1)
					attachment.writeMask = static_cast<rhi::ColorWriteEnable>(0);
				desc.targets.push_back({ .format = static_cast<rhi::Format>(a_key.colourFormats[i]), .blend = attachment });
			}
			desc.alphaToCoverage = blend.alphaToCoverage;
			if (a_key.hasDepth) {
				desc.depth = Pipeline::DepthStencilFromD3D11(DecodeDepthStencil(a_key.depthStencil));
				desc.depthFormat = static_cast<rhi::Format>(a_key.depthFormat);
			}
			desc.raster = Pipeline::RasterFromD3D11(DecodeRaster(a_key.raster));
			Detail::ResolveDefines(desc);
			return desc;
		}

		/** @brief An entry's build finished: its program (null: failed) stored and the waiters woken. */
		void Finish(const std::shared_ptr<Entry>& a_entry, std::shared_ptr<const RasterProgram> a_program)
		{
			{
				std::scoped_lock lock(mutex);
				a_entry->status = a_program ? Status::Ready : Status::Failed;
				a_entry->program = std::move(a_program);
			}
			if (a_entry->status == Status::Ready)
				built.fetch_add(1, std::memory_order_relaxed);
			changed.notify_all();
		}

		/** @brief Builds a desc on the calling thread; null, logged by Load, when it failed. */
		static std::shared_ptr<const RasterProgram> Load(const RasterProgram::Desc& a_desc, std::string_view a_family)
		{
			try {
				return Detail::Load(a_desc);
			} catch (const std::exception& e) {
				logger::error("[DCLF] {}: building {} threw: {}", a_family, Detail::Label(a_desc), e.what());
				return nullptr;
			}
		}

		/** @brief Lists a key in the manifest; a new one is saved on the build thread. */
		void Remember(const ProgramKey& a_key) const
		{
			if (Manifest::Get().Add(tag, a_key))
				Detail::Post([] { Manifest::Get().Save(); });
		}
	};

	namespace
	{
		/** @brief The families PrecompileStages serves, registered at install. */
		struct StageRegistry
		{
			std::mutex mutex;
			std::vector<EngineDrawPrograms::Impl*> families;
			bool started = false;  // the runtime had a compiler: requests compile at once
			std::vector<std::tuple<EngineDrawPrograms::Impl*, const RE::BSShader*, StageRequest>> pending;
		};

		StageRegistry& Stages()
		{
			static StageRegistry registry;
			return registry;
		}

		/** @brief One stage request compiled into the shader store (blocking: a compile thread). */
		void CompileStageRequest(EngineDrawPrograms::Impl& a_impl, const RE::BSShader& a_shader, const StageRequest& a_request)
		{
			try {
				auto desc = a_impl.StageDescOf(a_request.descriptor, a_request.stage, a_request.variant, a_shader);
				Detail::ResolveDefines(desc);
				if (!RasterProgram::CompileStages(desc)) {
					static Util::ReasonLog failures("[DCLF] engine draw programs: a stage precompile failed", spdlog::level::warn);
					failures.Report(std::format("{} descriptor {:#x} variant {}", a_impl.family, a_request.descriptor, a_request.variant));
				}
			} catch (const std::exception& e) {
				logger::warn("[DCLF] {}: the stage precompile of descriptor {:#x} threw: {}", a_impl.family, a_request.descriptor, e.what());
			}
		}
	}

	bool DescribeStates(ProgramKey& a_key, ID3D11BlendState* a_blend, ID3D11DepthStencilState* a_depthStencil, ID3D11RasterizerState* a_raster)
	{
		const auto blend = a_blend ? Pipeline::FindBlend(a_blend) : nullptr;
		const auto raster = a_raster ? Pipeline::FindRaster(a_raster) : nullptr;
		if (!blend || !raster)
			return false;
		EncodeBlend(blend->d3d, a_key.blend);
		EncodeRaster(raster->d3d, a_key.raster);
		a_key.hasDepth = 0;
		a_key.depthStencil = {};
		if (a_depthStencil) {
			const auto depthStencil = Pipeline::FindDepthStencil(a_depthStencil);
			if (!depthStencil)
				return false;
			a_key.hasDepth = 1;
			EncodeDepthStencil(depthStencil->d3d, a_key.depthStencil);
		}
		return true;
	}

	EngineDrawPrograms::EngineDrawPrograms(std::string a_family, ProgramSource a_source) :
		impl(std::make_unique<Impl>(std::move(a_family), std::move(a_source)))
	{
		auto& registry = Stages();
		std::scoped_lock lock(registry.mutex);
		registry.families.push_back(impl.get());
	}

	EngineDrawPrograms::~EngineDrawPrograms()
	{
		auto& registry = Stages();
		std::scoped_lock lock(registry.mutex);
		std::erase(registry.families, impl.get());
		std::erase_if(registry.pending, [&](const auto& a_request) { return std::get<0>(a_request) == impl.get(); });
	}

	std::size_t EngineDrawPrograms::Built() const
	{
		return impl->built.load(std::memory_order_relaxed);
	}

	std::shared_ptr<const RasterProgram> EngineDrawPrograms::Get(const ProgramKey& a_key, const RE::BSShader& a_shader)
	{
		const RE::BSShader* none = nullptr;
		impl->shader.compare_exchange_strong(none, &a_shader);
		std::shared_ptr<Entry> entry;
		{
			std::unique_lock lock(impl->mutex);
			if (const auto found = impl->entries.find(a_key); found != impl->entries.end()) {
				entry = found->second;
				// A prebuilt key still building: the draw waits for it (the gate before the first world frame's draws).
				if (entry->status == Status::Building) {
					const auto start = std::chrono::steady_clock::now();
					while (entry->status == Status::Building) {
						if (impl->changed.wait_for(lock, std::chrono::milliseconds(100)) == std::cv_status::timeout &&
							std::chrono::steady_clock::now() - start > kLongestWait) {
							logger::error("[DCLF] {}: the prebuilt program for descriptor {:#x} (variant {}) did not finish in {} s; this draw is the engine's", impl->family,
								a_key.descriptor, a_key.variant, kLongestWait.count());
							return nullptr;
						}
					}
					const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
					if (!std::exchange(impl->waitLogged, true))
						logger::info("[DCLF] {}: the first draw waited {} ms for its prebuilt program (descriptor {:#x})", impl->family, waited, a_key.descriptor);
				}
				return entry->status == Status::Ready ? entry->program : nullptr;
			}
			if (!Detail::Available())
				return nullptr;
			entry = std::make_shared<Entry>();
			impl->entries.emplace(a_key, entry);
		}

		// Never seen: built now, on the render thread, from the stages the store has.
		std::string why;
		const auto desc = impl->DescOf(a_key, a_shader, why);
		if (!desc) {
			logger::warn("[DCLF] {}: the program for descriptor {:#x} (variant {}) cannot be built: {}; its draws stay the engine's", impl->family, a_key.descriptor,
				a_key.variant, why);
			impl->Finish(entry, nullptr);
			return nullptr;
		}
		const auto start = std::chrono::steady_clock::now();
		auto program = Impl::Load(*desc, impl->family);
		const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
		impl->Finish(entry, program);
		if (!program) {
			logger::error("[DCLF] {}: the program for descriptor {:#x} (variant {}) could not be built; its draws stay the engine's", impl->family, a_key.descriptor,
				a_key.variant);
			return nullptr;
		}
		logger::warn("[DCLF] {}: the program for descriptor {:#x} (variant {}) was not prebuilt: built on the render thread at its first draw in {} ms ({} built); "
					 "the manifest lists it from now on",
			impl->family, a_key.descriptor, a_key.variant, took, Built());
		impl->Remember(a_key);
		return program;
	}

	void EngineDrawPrograms::Prebuild()
	{
		if (std::exchange(impl->prebuilt, true))
			return;
		const auto keys = Manifest::Get().Of(impl->tag);
		if (keys.empty())
			return;
		const auto* shader = impl->shader.load();
		if (!shader) {
			logger::warn("[DCLF] {}: no descriptor of its shader was loaded at startup (CS's disk cache off?); the manifest's {} programs build at their first draw",
				impl->family, keys.size());
			return;
		}
		std::size_t queued = 0, refused = 0;
		for (const auto& key : keys) {
			std::shared_ptr<Entry> entry;
			{
				std::scoped_lock lock(impl->mutex);
				if (impl->entries.contains(key))
					continue;
				entry = std::make_shared<Entry>();
				impl->entries.emplace(key, entry);
			}
			std::string why;
			auto desc = impl->DescOf(key, *shader, why);
			if (!desc) {
				++refused;
				impl->Finish(entry, nullptr);
				continue;
			}
			const bool posted = Detail::Post([impl = impl.get(), entry, desc = std::move(*desc)] { impl->Finish(entry, Impl::Load(desc, impl->family)); });
			if (!posted) {
				impl->Finish(entry, nullptr);
				continue;
			}
			++queued;
		}
		logger::info("[DCLF] {}: {} programs from the manifest queued on the program build thread{}", impl->family, queued,
			refused ? std::format(" ({} listed keys have no ORG form)", refused) : std::string());
	}

	void PrecompileStages(const RE::BSShader& a_shader, bool a_pixel, std::uint32_t a_descriptor)
	{
		auto& registry = Stages();
		std::vector<std::pair<EngineDrawPrograms::Impl*, StageRequest>> now;
		{
			std::scoped_lock lock(registry.mutex);
			for (auto* family : registry.families) {
				if (family->source.shaderType != static_cast<std::uint32_t>(a_shader.shaderType.underlying()) || !family->source.stagesOf)
					continue;
				const RE::BSShader* none = nullptr;
				family->shader.compare_exchange_strong(none, &a_shader);
				std::vector<StageRequest> requests;
				family->source.stagesOf(a_pixel, a_descriptor, requests);
				for (const auto& request : requests) {
					{
						std::scoped_lock stageLock(family->stageMutex);
						if (!family->stages.insert(request).second)
							continue;
					}
					if (registry.started)
						now.emplace_back(family, request);
					else
						registry.pending.emplace_back(family, &a_shader, request);
				}
			}
		}
		// On this compile thread, as DCLF's precompile: CS's bounded workers stay busy until the task's stages are in the store.
		for (const auto& [family, request] : now)
			CompileStageRequest(*family, a_shader, request);
	}

	void StartStagePrecompile()
	{
		auto& registry = Stages();
		std::vector<std::tuple<EngineDrawPrograms::Impl*, const RE::BSShader*, StageRequest>> pending;
		{
			std::scoped_lock lock(registry.mutex);
			if (registry.started || !RenderGraphRuntime::Get().ShaderCompiler())
				return;
			registry.started = true;
			pending.swap(registry.pending);
		}
		if (!pending.empty())
			logger::info("[DCLF] engine draw programs: {} stage builds kept from the engine's shader load start now", pending.size());
		for (const auto& request : pending)
			SIE::ShaderCache::Instance().compilationPool.detach_task([request] {
				CompileStageRequest(*std::get<0>(request), *std::get<1>(request), std::get<2>(request));
			});
	}
}
