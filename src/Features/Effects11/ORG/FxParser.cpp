#include "FxParser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace OrgFx
{
	namespace
	{
		bool IsIdentStart(char a_c) { return std::isalpha(static_cast<unsigned char>(a_c)) || a_c == '_'; }
		bool IsIdentChar(char a_c) { return std::isalnum(static_cast<unsigned char>(a_c)) || a_c == '_'; }
		bool IsDigit(char a_c) { return std::isdigit(static_cast<unsigned char>(a_c)) != 0; }

		bool EqualsNoCase(std::string_view a_a, std::string_view a_b)
		{
			return a_a.size() == a_b.size() && std::equal(a_a.begin(), a_a.end(), a_b.begin(), [](char x, char y) {
				return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
			});
		}

		std::string Upper(std::string_view a_s)
		{
			std::string out(a_s);
			for (auto& c : out)
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			return out;
		}

		// Lexer

		enum class Tok : std::uint8_t
		{
			Ident,
			Number,
			String,
			Punct,
			End,
		};

		struct Token
		{
			Tok kind = Tok::End;
			std::string_view text;
			std::uint32_t begin = 0;
			std::uint32_t end = 0;
			std::uint32_t line = 0;
		};

		/** @brief Tokens of the text. Directive lines and comments are dropped; punctuation is one character except "::". */
		std::vector<Token> Lex(std::string_view a_src, std::vector<Diagnostic>& a_diagnostics)
		{
			std::vector<Token> out;
			const std::size_t n = a_src.size();
			std::uint32_t line = 1;
			std::size_t i = 0;
			bool lineStart = true;
			auto push = [&](Tok a_kind, std::size_t a_begin, std::size_t a_end, std::uint32_t a_line) {
				out.push_back({ a_kind, a_src.substr(a_begin, a_end - a_begin), static_cast<std::uint32_t>(a_begin), static_cast<std::uint32_t>(a_end), a_line });
			};
			while (i < n) {
				const char c = a_src[i];
				if (c == '\n') {
					++line;
					++i;
					lineStart = true;
					continue;
				}
				if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
					++i;
					continue;
				}
				if (c == '#' && lineStart) {
					const std::size_t directiveStart = i;
					while (i < n && a_src[i] != '\n') {
						if (a_src[i] == '\\' && i + 1 < n && a_src[i + 1] == '\n') {
							++line;
							i += 2;
							continue;
						}
						if (a_src[i] == '\\' && i + 2 < n && a_src[i + 1] == '\r' && a_src[i + 2] == '\n') {
							++line;
							i += 3;
							continue;
						}
						++i;
					}
					const auto directive = a_src.substr(directiveStart, i - directiveStart);
					if (directive.find("pack_matrix") != std::string_view::npos)
						a_diagnostics.push_back({ Severity::Warning, line, "#pragma pack_matrix is not applied to the $Globals layout" });
					continue;
				}
				lineStart = false;
				if (c == '/' && i + 1 < n && a_src[i + 1] == '/') {
					while (i < n && a_src[i] != '\n')
						++i;
					continue;
				}
				if (c == '/' && i + 1 < n && a_src[i + 1] == '*') {
					i += 2;
					while (i + 1 < n && !(a_src[i] == '*' && a_src[i + 1] == '/')) {
						if (a_src[i] == '\n')
							++line;
						++i;
					}
					i = std::min(i + 2, n);
					continue;
				}
				const std::size_t b = i;
				if (IsIdentStart(c)) {
					while (i < n && IsIdentChar(a_src[i]))
						++i;
					push(Tok::Ident, b, i, line);
					continue;
				}
				if (IsDigit(c) || (c == '.' && i + 1 < n && IsDigit(a_src[i + 1]))) {
					if (c == '0' && i + 1 < n && (a_src[i + 1] == 'x' || a_src[i + 1] == 'X')) {
						i += 2;
						while (i < n && std::isxdigit(static_cast<unsigned char>(a_src[i])))
							++i;
					} else {
						while (i < n && (IsDigit(a_src[i]) || a_src[i] == '.'))
							++i;
						if (i < n && (a_src[i] == 'e' || a_src[i] == 'E')) {
							std::size_t j = i + 1;
							if (j < n && (a_src[j] == '+' || a_src[j] == '-'))
								++j;
							if (j < n && IsDigit(a_src[j])) {
								i = j;
								while (i < n && IsDigit(a_src[i]))
									++i;
							}
						}
					}
					while (i < n && std::strchr("fFhHlLuU", a_src[i]) != nullptr && a_src[i] != '\0')
						++i;
					push(Tok::Number, b, i, line);
					continue;
				}
				if (c == '"' || c == '\'') {
					const std::uint32_t startLine = line;
					++i;
					while (i < n && a_src[i] != c) {
						if (a_src[i] == '\\' && i + 1 < n)
							++i;
						if (a_src[i] == '\n')
							++line;
						++i;
					}
					if (i < n)
						++i;
					else
						a_diagnostics.push_back({ Severity::Error, startLine, "unterminated string literal" });
					push(c == '"' ? Tok::String : Tok::Number, b, i, startLine);
					continue;
				}
				if (c == ':' && i + 1 < n && a_src[i + 1] == ':') {
					i += 2;
					push(Tok::Punct, b, i, line);
					continue;
				}
				++i;
				push(Tok::Punct, b, i, line);
			}
			out.push_back({ Tok::End, {}, static_cast<std::uint32_t>(n), static_cast<std::uint32_t>(n), line });
			return out;
		}

		std::string Unquote(std::string_view a_literal)
		{
			std::string out;
			if (a_literal.size() < 2)
				return out;
			for (std::size_t i = 1; i + 1 < a_literal.size(); ++i) {
				char c = a_literal[i];
				if (c == '\\' && i + 2 < a_literal.size()) {
					const char e = a_literal[++i];
					switch (e) {
					case 'n':
						c = '\n';
						break;
					case 't':
						c = '\t';
						break;
					case 'r':
						c = '\r';
						break;
					case '0':
						c = '\0';
						break;
					default:
						c = e;
						break;
					}
				}
				out += c;
			}
			return out;
		}

		// Types

		struct ScalarName
		{
			std::string_view name;
			Scalar scalar;
		};

		constexpr ScalarName kScalars[] = {
			{ "bool", Scalar::Bool },
			{ "int", Scalar::Int },
			{ "uint", Scalar::Uint },
			{ "dword", Scalar::Uint },
			{ "float", Scalar::Float },
			{ "half", Scalar::Half },
			{ "double", Scalar::Double },
			{ "min16float", Scalar::Min16Float },
			{ "min10float", Scalar::Min10Float },
			{ "min16int", Scalar::Min16Int },
			{ "min12int", Scalar::Min12Int },
			{ "min16uint", Scalar::Min16Uint },
		};

		/** @brief "float", "float3", "float4x4" into a numeric type; nullopt for anything else. */
		std::optional<Type> NumericType(std::string_view a_name)
		{
			for (const auto& s : kScalars) {
				if (!a_name.starts_with(s.name))
					continue;
				auto rest = a_name.substr(s.name.size());
				Type t;
				t.cls = TypeClass::Numeric;
				t.scalar = s.scalar;
				t.name = std::string(a_name);
				if (rest.empty())
					return t;
				if (rest.size() == 1 && rest[0] >= '1' && rest[0] <= '4') {
					t.cols = static_cast<std::uint8_t>(rest[0] - '0');
					return t;
				}
				if (rest.size() == 3 && rest[0] >= '1' && rest[0] <= '4' && rest[1] == 'x' && rest[2] >= '1' && rest[2] <= '4') {
					t.rows = static_cast<std::uint8_t>(rest[0] - '0');
					t.cols = static_cast<std::uint8_t>(rest[2] - '0');
					t.matrix = true;
					return t;
				}
			}
			return std::nullopt;
		}

		const std::unordered_set<std::string_view> kTextureTypes = {
			"Texture", "texture", "Texture1D", "Texture1DArray", "Texture2D", "Texture2DArray", "Texture2DMS", "Texture2DMSArray",
			"Texture3D", "TextureCube", "TextureCubeArray", "Buffer", "StructuredBuffer", "ByteAddressBuffer"
		};
		const std::unordered_set<std::string_view> kRWTypes = {
			"RWTexture1D", "RWTexture1DArray", "RWTexture2D", "RWTexture2DArray", "RWTexture3D", "RWBuffer", "RWStructuredBuffer",
			"RWByteAddressBuffer", "AppendStructuredBuffer", "ConsumeStructuredBuffer"
		};
		const std::unordered_set<std::string_view> kSamplerTypes = {
			"SamplerState", "SamplerComparisonState", "sampler", "sampler1D", "sampler2D", "sampler3D", "samplerCUBE", "sampler_state"
		};
		const std::unordered_set<std::string_view> kShaderTypes = {
			"VertexShader", "PixelShader", "GeometryShader", "HullShader", "DomainShader", "ComputeShader"
		};
		const std::unordered_set<std::string_view> kStateTypes = { "BlendState", "DepthStencilState", "RasterizerState" };

		const std::unordered_set<std::string_view> kDeclModifiers = {
			"static", "const", "uniform", "extern", "shared", "groupshared", "volatile", "row_major", "column_major", "inline",
			"precise", "snorm", "unorm"
		};
		const std::unordered_set<std::string_view> kInterpolation = { "linear", "centroid", "nointerpolation", "noperspective", "sample" };

		constexpr std::uint32_t Align16(std::uint32_t a_v) { return (a_v + 15u) & ~15u; }

		// State values

		template <class E>
		struct EnumName
		{
			std::string_view name;
			E value;
		};

		/** @brief Drops a "D3D11_BLEND_"-style prefix and underscores, uppercased: "D3D11_BLEND_INV_SRC_ALPHA" -> "INVSRCALPHA". */
		std::string NormalizeStateValue(std::string_view a_value, std::string_view a_prefix)
		{
			std::string v = Upper(a_value);
			for (const auto* api : { "D3D11_", "D3D10_" }) {
				if (v.starts_with(api)) {
					v.erase(0, std::strlen(api));
					break;
				}
			}
			if (!a_prefix.empty() && v.starts_with(a_prefix))
				v.erase(0, a_prefix.size());
			std::erase(v, '_');
			return v;
		}

		template <class E, std::size_t N>
		std::optional<E> LookupEnum(std::string_view a_value, std::string_view a_prefix, const EnumName<E> (&a_table)[N])
		{
			const auto v = NormalizeStateValue(a_value, a_prefix);
			for (const auto& entry : a_table)
				if (v == entry.name)
					return entry.value;
			char* end = nullptr;
			const std::string s(a_value);
			const long numeric = std::strtol(s.c_str(), &end, 0);
			if (end && *end == '\0' && !s.empty())
				for (const auto& entry : a_table)
					if (static_cast<long>(entry.value) == numeric - 1)
						return entry.value;
			return std::nullopt;
		}

		constexpr EnumName<AddressMode> kAddressModes[] = {
			{ "WRAP", AddressMode::Wrap },
			{ "MIRROR", AddressMode::Mirror },
			{ "CLAMP", AddressMode::Clamp },
			{ "BORDER", AddressMode::Border },
			{ "MIRRORONCE", AddressMode::MirrorOnce },
		};
		constexpr EnumName<CompareFunc> kCompareFuncs[] = {
			{ "NEVER", CompareFunc::Never },
			{ "LESS", CompareFunc::Less },
			{ "EQUAL", CompareFunc::Equal },
			{ "LESSEQUAL", CompareFunc::LessEqual },
			{ "GREATER", CompareFunc::Greater },
			{ "NOTEQUAL", CompareFunc::NotEqual },
			{ "GREATEREQUAL", CompareFunc::GreaterEqual },
			{ "ALWAYS", CompareFunc::Always },
		};
		constexpr EnumName<BlendFactor> kBlendFactors[] = {
			{ "ZERO", BlendFactor::Zero },
			{ "ONE", BlendFactor::One },
			{ "SRCCOLOR", BlendFactor::SrcColor },
			{ "INVSRCCOLOR", BlendFactor::InvSrcColor },
			{ "SRCALPHA", BlendFactor::SrcAlpha },
			{ "INVSRCALPHA", BlendFactor::InvSrcAlpha },
			{ "DESTALPHA", BlendFactor::DestAlpha },
			{ "INVDESTALPHA", BlendFactor::InvDestAlpha },
			{ "DESTCOLOR", BlendFactor::DestColor },
			{ "INVDESTCOLOR", BlendFactor::InvDestColor },
			{ "SRCALPHASAT", BlendFactor::SrcAlphaSat },
			{ "BLENDFACTOR", BlendFactor::Factor },
			{ "INVBLENDFACTOR", BlendFactor::InvFactor },
			{ "SRC1COLOR", BlendFactor::Src1Color },
			{ "INVSRC1COLOR", BlendFactor::InvSrc1Color },
			{ "SRC1ALPHA", BlendFactor::Src1Alpha },
			{ "INVSRC1ALPHA", BlendFactor::InvSrc1Alpha },
		};
		constexpr EnumName<BlendOp> kBlendOps[] = {
			{ "ADD", BlendOp::Add },
			{ "SUBTRACT", BlendOp::Subtract },
			{ "REVSUBTRACT", BlendOp::RevSubtract },
			{ "MIN", BlendOp::Min },
			{ "MAX", BlendOp::Max },
		};
		constexpr EnumName<StencilOp> kStencilOps[] = {
			{ "KEEP", StencilOp::Keep },
			{ "ZERO", StencilOp::Zero },
			{ "REPLACE", StencilOp::Replace },
			{ "INCRSAT", StencilOp::IncrSat },
			{ "DECRSAT", StencilOp::DecrSat },
			{ "INVERT", StencilOp::Invert },
			{ "INCR", StencilOp::Incr },
			{ "DECR", StencilOp::Decr },
		};
		constexpr EnumName<FillMode> kFillModes[] = {
			{ "WIREFRAME", FillMode::Wireframe },
			{ "SOLID", FillMode::Solid },
		};
		constexpr EnumName<CullMode> kCullModes[] = {
			{ "NONE", CullMode::None },
			{ "FRONT", CullMode::Front },
			{ "BACK", CullMode::Back },
		};

		std::optional<bool> ParseBool(std::string_view a_value)
		{
			const auto v = Upper(a_value);
			if (v == "TRUE" || v == "1")
				return true;
			if (v == "FALSE" || v == "0")
				return false;
			return std::nullopt;
		}

		/** @brief A filter name ("MIN_POINT_MAG_LINEAR_MIP_POINT", "COMPARISON_ANISOTROPIC") into the sampler. */
		bool ParseFilter(std::string_view a_value, SamplerDesc& a_sampler)
		{
			std::string v = Upper(a_value);
			for (const auto* api : { "D3D11_FILTER_", "D3D10_FILTER_" })
				if (v.starts_with(api))
					v.erase(0, std::strlen(api));
			if (v.starts_with("COMPARISON_")) {
				a_sampler.reduction = FilterReduction::Comparison;
				v.erase(0, 11);
			} else if (v.starts_with("MINIMUM_")) {
				a_sampler.reduction = FilterReduction::Minimum;
				v.erase(0, 8);
			} else if (v.starts_with("MAXIMUM_")) {
				a_sampler.reduction = FilterReduction::Maximum;
				v.erase(0, 8);
			}
			if (v == "ANISOTROPIC") {
				a_sampler.anisotropic = true;
				a_sampler.minFilter = a_sampler.magFilter = a_sampler.mipFilter = FilterMode::Linear;
				return true;
			}
			std::vector<FilterMode*> pending;
			bool any = false;
			std::size_t start = 0;
			while (start <= v.size()) {
				const auto stop = v.find('_', start);
				const auto word = std::string_view(v).substr(start, stop == std::string::npos ? std::string::npos : stop - start);
				if (word == "MIN")
					pending.push_back(&a_sampler.minFilter);
				else if (word == "MAG")
					pending.push_back(&a_sampler.magFilter);
				else if (word == "MIP")
					pending.push_back(&a_sampler.mipFilter);
				else if (word == "POINT" || word == "LINEAR") {
					for (auto* f : pending)
						*f = word == "POINT" ? FilterMode::Point : FilterMode::Linear;
					any = any || !pending.empty();
					pending.clear();
				} else
					return false;
				if (stop == std::string::npos)
					break;
				start = stop + 1;
			}
			return any && pending.empty();
		}

		// Values

		using Values = std::vector<double>;

		/** @brief FX11's string for a numeric annotation (Effect.cpp, ReadAnnotationValue). */
		std::string AnnotationNumberText(std::string_view a_type, const Values& a_values)
		{
			std::string out;
			for (std::size_t i = 0; i < a_values.size(); ++i) {
				if (i)
					out += ",";
				const double v = a_values[i];
				if (a_type.starts_with("float") || a_type.starts_with("half") || a_type.starts_with("double"))
					out += std::to_string(static_cast<float>(v));
				else if (a_type.starts_with("bool"))
					out += v != 0.0 ? "1" : "0";
				else
					out += std::to_string(static_cast<long long>(v));
			}
			return out;
		}

		std::uint32_t ScalarBytes(Scalar a_scalar) { return a_scalar == Scalar::Double ? 8u : 4u; }

		class Parser
		{
		public:
			Parser(std::string_view a_source, EffectDesc& a_fx) :
				src(a_source), fx(a_fx)
			{
				toks = Lex(a_source, fx.diagnostics);
				limit = toks.size() - 1;
			}

			void Run()
			{
				while (!AtEnd())
					ParseGlobal();
				ResolveEntries();
				BuildChains();
				ComputeLayouts();
				CheckRuntimeSupport();
			}

		private:
			std::string_view src;
			EffectDesc& fx;
			std::vector<Token> toks;
			std::size_t pos = 0;
			std::size_t limit = 0;  // index of the token that reads as End (the real End, or a sub-range's end)
			std::size_t attributeStart = std::numeric_limits<std::size_t>::max();
			std::unordered_map<std::string, ShaderBinding> shaderVariables;
			std::unordered_map<std::string, Type> typedefs;
			mutable Token sentinel{};

			// Cursor

			/** @brief The token at a_index, or an End token at and after the current range's end. */
			const Token& At(std::size_t a_index) const
			{
				if (a_index < limit)
					return toks[a_index];
				sentinel = Token{ Tok::End, {}, toks[limit].begin, toks[limit].begin, toks[limit].line };
				return sentinel;
			}

			const Token& Peek(std::size_t a_k = 0) const { return At(pos + a_k); }
			bool AtEnd() const { return Peek().kind == Tok::End; }
			bool Is(std::string_view a_text, std::size_t a_k = 0) const
			{
				const auto& t = Peek(a_k);
				return (t.kind == Tok::Ident || t.kind == Tok::Punct) && t.text == a_text;
			}
			bool IsIdent(std::size_t a_k = 0) const { return Peek(a_k).kind == Tok::Ident; }
			bool Accept(std::string_view a_text)
			{
				if (!Is(a_text))
					return false;
				++pos;
				return true;
			}
			bool Expect(std::string_view a_text, std::string_view a_where)
			{
				if (Accept(a_text))
					return true;
				Diag(Severity::Error, Peek().line, std::format("expected '{}' {}, found '{}'", a_text, a_where, Peek().text));
				return false;
			}

			void Diag(Severity a_severity, std::uint32_t a_line, std::string a_message)
			{
				fx.diagnostics.push_back({ a_severity, a_line, std::move(a_message) });
			}

			/** @brief The index after the bracket that closes the one at a_open ( "(", "[", "{" or "<"). */
			std::size_t MatchClose(std::size_t a_open) const
			{
				const auto open = At(a_open).text;
				const std::string_view close = open == "(" ? ")" : open == "[" ? "]" :
				                                               open == "{"     ? "}" :
				                                                                 ">";
				int depth = 0;
				for (std::size_t i = a_open; At(i).kind != Tok::End; ++i) {
					const auto& t = At(i);
					if (t.kind != Tok::Punct)
						continue;
					if (t.text == open)
						++depth;
					else if (t.text == close && --depth == 0)
						return i + 1;
				}
				return limit;
			}

			/** @brief Skips to after the next ';' at depth 0, or after a balanced {} block at depth 0 (and a ';' right after it). */
			void SkipStatement()
			{
				while (!AtEnd()) {
					if (Is("(") || Is("[")) {
						pos = MatchClose(pos);
						continue;
					}
					if (Is("{")) {
						pos = MatchClose(pos);
						Accept(";");
						return;
					}
					if (Accept(";"))
						return;
					++pos;
				}
			}

			std::string Text(std::size_t a_first, std::size_t a_last) const
			{
				if (a_last <= a_first)
					return {};
				const auto b = toks[a_first].begin;
				const auto e = toks[a_last - 1].end;
				return std::string(src.substr(b, e - b));
			}

			Range RangeOf(std::size_t a_first, std::size_t a_last) const
			{
				if (a_last <= a_first)
					return { toks[a_first].begin, toks[a_first].begin };
				return { toks[a_first].begin, toks[a_last - 1].end };
			}

			/**
			 * @brief Token ranges of the comma-separated items in [a_first, a_last), split at depth 0. With a_angles, "<" and ">"
			 * nest too: a parameter list, where they only appear in template arguments (Texture2DMS<float4, 2>).
			 */
			std::vector<std::pair<std::size_t, std::size_t>> SplitArgs(std::size_t a_first, std::size_t a_last, bool a_angles = false) const
			{
				std::vector<std::pair<std::size_t, std::size_t>> out;
				if (a_first >= a_last)
					return out;
				int depth = 0;
				std::size_t start = a_first;
				for (std::size_t i = a_first; i < a_last; ++i) {
					const auto& t = toks[i];
					if (t.kind != Tok::Punct)
						continue;
					if (t.text == "(" || t.text == "[" || t.text == "{" || (a_angles && t.text == "<"))
						++depth;
					else if (t.text == ")" || t.text == "]" || t.text == "}" || (a_angles && t.text == ">"))
						--depth;
					else if (t.text == "," && depth == 0) {
						out.emplace_back(start, i);
						start = i + 1;
					}
				}
				out.emplace_back(start, a_last);
				return out;
			}

			/** @brief Runs a_fn with the cursor on [a_first, a_last), then restores it. */
			template <class F>
			auto WithRange(std::size_t a_first, std::size_t a_last, F&& a_fn)
			{
				const auto savedPos = pos;
				const auto savedLimit = limit;
				pos = a_first;
				limit = std::min(a_last, savedLimit);
				struct Restore
				{
					Parser& parser;
					std::size_t restorePos;
					std::size_t restoreLimit;
					~Restore()
					{
						parser.pos = restorePos;
						parser.limit = restoreLimit;
					}
				} restore{ *this, savedPos, savedLimit };
				return a_fn();
			}

			// Types

			Type Classify(const std::string& a_name) const
			{
				if (auto numeric = NumericType(a_name))
					return *numeric;
				Type t;
				t.name = a_name;
				if (a_name == "void")
					t.cls = TypeClass::Void;
				else if (a_name == "string")
					t.cls = TypeClass::String;
				else if (a_name == "vector" || a_name == "matrix") {
					t.cls = TypeClass::Numeric;
					t.scalar = Scalar::Float;
					t.cols = 4;
					if (a_name == "matrix") {
						t.rows = 4;
						t.matrix = true;
					}
				} else if (kTextureTypes.contains(a_name))
					t.cls = TypeClass::Texture;
				else if (kRWTypes.contains(a_name))
					t.cls = TypeClass::RWResource;
				else if (kSamplerTypes.contains(a_name))
					t.cls = TypeClass::Sampler;
				else if (kShaderTypes.contains(a_name))
					t.cls = TypeClass::Shader;
				else if (kStateTypes.contains(a_name))
					t.cls = TypeClass::StateObject;
				else if (auto it = typedefs.find(a_name); it != typedefs.end())
					return it->second;
				else if (fx.FindStruct(a_name))
					t.cls = TypeClass::Struct;
				return t;
			}

			/** @brief A type at the cursor: a name, "A::B", and an optional template argument list. */
			bool ParseType(Type& a_out)
			{
				if (!IsIdent())
					return false;
				std::string name(Peek().text);
				++pos;
				while (Is("::") && IsIdent(1)) {
					name += "::";
					name += Peek(1).text;
					pos += 2;
				}
				a_out = Classify(name);
				if (Is("<")) {  // right after a type name, "<" opens template arguments (an annotation follows a name)
					const auto close = MatchClose(pos);
					a_out.templateArg = Text(pos + 1, close - 1);
					if (name == "vector" || name == "matrix") {
						const auto args = SplitArgs(pos + 1, close - 1);
						if (!args.empty())
							if (auto numeric = NumericType(Text(args[0].first, args[0].second)))
								a_out.scalar = numeric->scalar;
						auto dim = [&](std::size_t a_i, std::uint8_t a_fallback) -> std::uint8_t {
							if (a_i >= args.size())
								return a_fallback;
							Values v;
							if (!Evaluate(args[a_i].first, args[a_i].second, v) || v.size() != 1)
								return a_fallback;
							return static_cast<std::uint8_t>(std::clamp(static_cast<int>(v[0]), 1, 4));
						};
						if (name == "vector")
							a_out.cols = dim(1, 4);
						else {
							a_out.rows = dim(1, 4);
							a_out.cols = dim(2, 4);
						}
					}
					pos = close;
				}
				return true;
			}

			bool ParseModifiers(Modifiers& a_mods)
			{
				bool any = false;
				while (IsIdent() && kDeclModifiers.contains(Peek().text) && IsIdent(1)) {
					const auto t = Peek().text;
					a_mods.isStatic |= t == "static";
					a_mods.isConst |= t == "const";
					a_mods.isUniform |= t == "uniform";
					a_mods.isExtern |= t == "extern";
					a_mods.isShared |= t == "shared";
					a_mods.isGroupShared |= t == "groupshared";
					a_mods.isVolatile |= t == "volatile";
					a_mods.rowMajor |= t == "row_major";
					a_mods.columnMajor |= t == "column_major";
					++pos;
					any = true;
				}
				return any;
			}

			/** @brief "[n]" (several dimensions multiply); 0 when there is none, 1 with a diagnostic when it cannot be evaluated. */
			std::uint32_t ParseArraySize()
			{
				std::uint32_t size = 0;
				while (Is("[")) {
					const auto close = MatchClose(pos);
					Values v;
					std::uint32_t dim = 1;
					if (close - pos > 2 && Evaluate(pos + 1, close - 1, v) && v.size() == 1 && v[0] >= 1)
						dim = static_cast<std::uint32_t>(v[0]);
					else if (close - pos > 2)
						Diag(Severity::Warning, Peek().line, std::format("array size '{}' could not be evaluated", Text(pos + 1, close - 1)));
					size = size ? size * dim : dim;
					pos = close;
				}
				return size;
			}

			/** @brief `: SEMANTIC`, `: register(t0)`, `: packoffset(c0)`, repeated. */
			void ParseBindings(std::string& a_semantic, std::string* a_register)
			{
				while (Is(":")) {
					++pos;
					if ((Is("register") || Is("packoffset")) && Is("(", 1)) {
						const auto close = MatchClose(pos + 1);
						const bool isPack = Is("packoffset");
						if (a_register)
							*a_register = isPack ? "packoffset(" + Text(pos + 2, close - 1) + ")" : Text(pos + 2, close - 1);
						if (isPack)
							Diag(Severity::Warning, Peek().line, "packoffset is not applied to the layout");
						pos = close;
					} else if (IsIdent()) {
						a_semantic = std::string(Peek().text);
						++pos;
					} else {
						Diag(Severity::Error, Peek().line, std::format("expected a semantic after ':', found '{}'", Peek().text));
						return;
					}
				}
			}

			// Constant expressions

			/** @brief Evaluates [a_first, a_last) as a constant; false when it is not one this evaluator understands. */
			bool Evaluate(std::size_t a_first, std::size_t a_last, Values& a_out)
			{
				if (a_first >= a_last)
					return false;
				return WithRange(a_first, a_last, [&] {
					Values v;
					if (!EvalExpr(v) || !AtEnd())
						return false;
					a_out = std::move(v);
					return true;
				});
			}

			static bool Combine(Values& a_lhs, const Values& a_rhs, char a_op)
			{
				if (a_lhs.empty() || a_rhs.empty())
					return false;
				const std::size_t n = (a_lhs.size() == 1 || a_rhs.size() == 1) ? std::max(a_lhs.size(), a_rhs.size()) : std::min(a_lhs.size(), a_rhs.size());
				Values out(n);
				for (std::size_t i = 0; i < n; ++i) {
					const double l = a_lhs[a_lhs.size() == 1 ? 0 : i];
					const double r = a_rhs[a_rhs.size() == 1 ? 0 : i];
					switch (a_op) {
					case '+':
						out[i] = l + r;
						break;
					case '-':
						out[i] = l - r;
						break;
					case '*':
						out[i] = l * r;
						break;
					case '/':
						out[i] = l / r;
						break;
					case '%':
						out[i] = std::fmod(l, r);
						break;
					default:
						return false;
					}
				}
				a_lhs = std::move(out);
				return true;
			}

			bool EvalExpr(Values& a_out)
			{
				if (!EvalTerm(a_out))
					return false;
				while (Is("+") || Is("-")) {
					const char op = Peek().text[0];
					++pos;
					Values rhs;
					if (!EvalTerm(rhs) || !Combine(a_out, rhs, op))
						return false;
				}
				return true;
			}

			bool EvalTerm(Values& a_out)
			{
				if (!EvalUnary(a_out))
					return false;
				while (Is("*") || Is("/") || Is("%")) {
					const char op = Peek().text[0];
					++pos;
					Values rhs;
					if (!EvalUnary(rhs) || !Combine(a_out, rhs, op))
						return false;
				}
				return true;
			}

			bool EvalUnary(Values& a_out)
			{
				if (Accept("-")) {
					if (!EvalUnary(a_out))
						return false;
					for (auto& v : a_out)
						v = -v;
					return true;
				}
				if (Accept("+"))
					return EvalUnary(a_out);
				if (Accept("!")) {
					if (!EvalUnary(a_out))
						return false;
					for (auto& v : a_out)
						v = v == 0.0 ? 1.0 : 0.0;
					return true;
				}
				return EvalPostfix(a_out);
			}

			bool EvalList(std::size_t a_first, std::size_t a_last, Values& a_out)
			{
				for (auto [b, e] : SplitArgs(a_first, a_last)) {
					if (b == e)
						continue;  // trailing comma
					Values v;
					if (!Evaluate(b, e, v))
						return false;
					a_out.insert(a_out.end(), v.begin(), v.end());
				}
				return true;
			}

			bool EvalPostfix(Values& a_out)
			{
				if (!EvalPrimary(a_out))
					return false;
				while (true) {
					if (Is("[")) {
						const auto close = MatchClose(pos);
						Values index;
						if (!Evaluate(pos + 1, close - 1, index) || index.size() != 1)
							return false;
						const auto i = static_cast<std::size_t>(index[0]);
						if (i >= a_out.size())
							return false;
						a_out = { a_out[i] };
						pos = close;
					} else if (Is(".") && IsIdent(1)) {
						const auto swizzle = Peek(1).text;
						Values out;
						for (char c : swizzle) {
							const auto at = std::string_view("xyzw").find(c) != std::string_view::npos ? std::string_view("xyzw").find(c) : std::string_view("rgba").find(c);
							if (at == std::string_view::npos || at >= a_out.size())
								return false;
							out.push_back(a_out[at]);
						}
						a_out = std::move(out);
						pos += 2;
					} else
						return true;
				}
			}

			bool EvalPrimary(Values& a_out)
			{
				const auto& t = Peek();
				if (t.kind == Tok::Number) {
					if (t.text.starts_with("'"))
						return false;
					std::string s(t.text);
					const bool hex = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
					while (!s.empty() && std::strchr(hex ? "uUlL" : "fFhHlLuU", s.back()) != nullptr)
						s.pop_back();
					char* end = nullptr;
					const double v = hex ? static_cast<double>(std::strtoull(s.c_str(), &end, 16)) : std::strtod(s.c_str(), &end);
					if (!end || *end != '\0')
						return false;
					a_out = { v };
					++pos;
					return true;
				}
				if (t.kind == Tok::Ident && (t.text == "true" || t.text == "false")) {
					a_out = { t.text == "true" ? 1.0 : 0.0 };
					++pos;
					return true;
				}
				if (Is("(")) {
					const auto close = MatchClose(pos);
					// A cast: (float) x
					if (close == pos + 3 && IsIdent(1) && NumericType(std::string(Peek(1).text))) {
						pos = close;
						return EvalUnary(a_out);
					}
					if (!Evaluate(pos + 1, close - 1, a_out))
						return false;
					pos = close;
					return true;
				}
				if (Is("{")) {
					const auto close = MatchClose(pos);
					if (!EvalList(pos + 1, close - 1, a_out))
						return false;
					pos = close;
					return true;
				}
				if (t.kind == Tok::Ident) {
					const std::string name(t.text);
					if (Is("(", 1)) {
						auto numeric = NumericType(name);
						if (!numeric && name != "vector" && name != "matrix")
							return false;
						const auto close = MatchClose(pos + 1);
						if (!EvalList(pos + 2, close - 1, a_out))
							return false;
						if (numeric && a_out.size() == 1 && numeric->rows * numeric->cols > 1)
							a_out.assign(static_cast<std::size_t>(numeric->rows) * numeric->cols, a_out[0]);
						pos = close;
						return true;
					}
					if (const auto* v = fx.FindVariable(name); v && v->initializerEvaluated) {
						a_out = v->initializer;
						++pos;
						return true;
					}
				}
				return false;
			}

			// Annotations

			void ParseAnnotations(Annotations& a_out)
			{
				const auto close = MatchClose(pos);
				++pos;
				const auto stop = close - 1;
				while (pos < stop) {
					if (Accept(";"))
						continue;
					const auto line = Peek().line;
					Annotation a;
					if (!IsIdent()) {
						Diag(Severity::Error, line, std::format("expected an annotation type, found '{}'", Peek().text));
						break;
					}
					Type type;
					ParseType(type);
					a.type = type.name;
					if (!IsIdent()) {
						Diag(Severity::Error, line, std::format("expected an annotation name after '{}'", a.type));
						break;
					}
					a.name = std::string(Peek().text);
					++pos;
					ParseArraySize();
					if (!Expect("=", std::format("after annotation '{}'", a.name)))
						break;
					const auto valueStart = pos;
					while (pos < stop && !Is(";")) {
						if (Is("(") || Is("{") || Is("["))
							pos = MatchClose(pos);
						else
							++pos;
					}
					const auto valueEnd = pos;
					if (a.type == "string") {
						bool allStrings = valueEnd > valueStart;
						for (auto i = valueStart; i < valueEnd; ++i) {
							if (toks[i].kind != Tok::String) {
								allStrings = false;
								break;
							}
							a.text += Unquote(toks[i].text);
						}
						if (!allStrings) {
							a.text = Text(valueStart, valueEnd);
							Diag(Severity::Warning, line, std::format("string annotation '{}' is not a literal: '{}'", a.name, a.text));
						}
					} else if (Evaluate(valueStart, valueEnd, a.numbers))
						a.text = AnnotationNumberText(a.type, a.numbers);
					else {
						a.text = Text(valueStart, valueEnd);
						Diag(Severity::Warning, line, std::format("annotation '{}' value '{}' could not be evaluated", a.name, a.text));
					}
					a_out.push_back(std::move(a));
				}
				pos = close;
			}

			// Globals

			void ParseGlobal()
			{
				if (Accept(";"))
					return;
				if (Is("[")) {
					if (attributeStart == std::numeric_limits<std::size_t>::max())
						attributeStart = pos;
					pos = MatchClose(pos);
					return;
				}
				if (!IsIdent()) {
					Diag(Severity::Error, Peek().line, std::format("unexpected '{}' at global scope", Peek().text));
					++pos;
					SkipStatement();
					return;
				}
				const auto t = Peek().text;
				if (t == "technique" || t == "technique10" || t == "technique11") {
					ParseTechnique({});
					return;
				}
				if (t == "fxgroup") {
					ParseGroup();
					return;
				}
				if ((t == "struct" || t == "class") && IsIdent(1) && (Is("{", 2) || Is(":", 2))) {
					ParseStruct();
					return;
				}
				if (t == "cbuffer" || t == "tbuffer") {
					ParseConstantBuffer();
					return;
				}
				if (t == "typedef") {
					ParseTypedef();
					return;
				}
				if (t == "namespace" || t == "interface") {
					Diag(Severity::Warning, Peek().line, std::format("'{}' is not supported; skipped", t));
					SkipStatement();
					return;
				}
				ParseDeclaration({});
			}

			void ParseDeclaration(const std::string& a_cbuffer)
			{
				const auto start = attributeStart != std::numeric_limits<std::size_t>::max() ? attributeStart : pos;
				attributeStart = std::numeric_limits<std::size_t>::max();
				Modifiers mods;
				ParseModifiers(mods);
				if ((Is("struct") || Is("class")) && IsIdent(1))
					++pos;
				Type type;
				const auto line = Peek().line;
				if (!ParseType(type)) {
					Diag(Severity::Error, line, std::format("expected a declaration, found '{}'", Peek().text));
					SkipStatement();
					return;
				}
				ParseModifiers(mods);
				if (!IsIdent()) {
					Diag(Severity::Error, line, std::format("expected a name after type '{}', found '{}'", type.name, Peek().text));
					SkipStatement();
					return;
				}
				if (Is("(", 1)) {
					ParseFunction(start, type);
					return;
				}
				if (type.cls == TypeClass::Unknown)
					Diag(Severity::Warning, line, std::format("unknown type '{}' for '{}'", type.name, Peek().text));
				ParseVariables(start, type, mods, a_cbuffer);
			}

			void ParseVariables(std::size_t a_start, const Type& a_type, const Modifiers& a_mods, const std::string& a_cbuffer)
			{
				const auto firstVariable = fx.variables.size();
				const auto firstState = fx.states.size();
				while (true) {
					if (!IsIdent()) {
						Diag(Severity::Error, Peek().line, std::format("expected a name in a '{}' declaration, found '{}'", a_type.name, Peek().text));
						SkipStatement();
						break;
					}
					Variable v;
					v.type = a_type;
					v.modifiers = a_mods;
					v.name = std::string(Peek().text);
					v.line = Peek().line;
					v.cbuffer = a_cbuffer;
					++pos;
					v.arraySize = ParseArraySize();
					ParseBindings(v.semantic, &v.registerBinding);
					if (Is("<")) {
						const auto annotationStart = pos;
						ParseAnnotations(v.annotations);
						v.annotation = RangeOf(annotationStart, pos);
					}

					const bool stateType = a_type.cls == TypeClass::StateObject || a_type.cls == TypeClass::Sampler;
					if (stateType && Is("{")) {
						ParseStateBlock(v, false);
					} else if (Accept("=")) {
						if (stateType && Is("sampler_state")) {
							++pos;
							ParseStateBlock(v, true);
						} else {
							const auto initStart = pos;
							while (!AtEnd() && !Is(",") && !Is(";")) {
								if (Is("(") || Is("{") || Is("["))
									pos = MatchClose(pos);
								else
									++pos;
							}
							v.initializerText = Text(initStart, pos);
							if (a_type.cls == TypeClass::Shader) {
								ShaderBinding binding;
								ParseShaderExpression(initStart, pos, binding, v.line);
								shaderVariables[v.name] = std::move(binding);
							} else if (a_type.cls == TypeClass::String) {
								std::string joined;
								for (auto i = initStart; i < pos; ++i)
									if (toks[i].kind == Tok::String)
										joined += Unquote(toks[i].text);
								v.initializerText = std::move(joined);
							} else if (Evaluate(initStart, pos, v.initializer)) {
								v.initializerEvaluated = true;
							} else if (a_type.cls == TypeClass::Numeric && !a_mods.isStatic) {
								// A static's initializer runs in the shader; only a uniform's must be known on the CPU.
								Diag(Severity::Warning, v.line, std::format("initializer of '{}' could not be evaluated: '{}'", v.name, v.initializerText));
							}
							fx.variables.push_back(std::move(v));
						}
					} else {
						fx.variables.push_back(std::move(v));
					}
					if (Accept(","))
						continue;
					if (!Accept(";")) {
						Diag(Severity::Error, Peek().line, std::format("expected ';' after the declaration of '{}', found '{}'", fx.variables.empty() ? std::string{} : fx.variables.back().name, Peek().text));
						SkipStatement();
					}
					break;
				}
				const auto statement = RangeOf(a_start, pos);
				for (auto i = firstVariable; i < fx.variables.size(); ++i)
					fx.variables[i].statement = statement;
				for (auto i = firstState; i < fx.states.size(); ++i)
					fx.states[i].statement = statement;
			}

			void ParseFunction(std::size_t a_start, const Type& a_returnType)
			{
				Function f;
				f.name = std::string(Peek().text);
				f.line = Peek().line;
				f.returnType = a_returnType;
				++pos;
				const auto open = pos;
				const auto close = MatchClose(open);
				for (auto [b, e] : SplitArgs(open + 1, close - 1, true)) {
					if (b == e)
						continue;
					if (e == b + 1 && toks[b].text == "void")
						continue;
					Parameter p;
					if (WithRange(b, e, [&] { return ParseParameter(p); }))
						f.parameters.push_back(std::move(p));
					else
						Diag(Severity::Error, toks[b].line, std::format("could not parse parameter '{}' of '{}'", Text(b, e), f.name));
				}
				pos = close;
				std::string registerUnused;
				ParseBindings(f.returnSemantic, &registerUnused);
				if (Is("{")) {
					const auto bodyEnd = MatchClose(pos);
					f.body = RangeOf(pos, bodyEnd);
					pos = bodyEnd;
					f.statement = RangeOf(a_start, pos);
					fx.functions.push_back(std::move(f));
				} else if (!Accept(";")) {
					Diag(Severity::Error, Peek().line, std::format("expected a body after function '{}'", f.name));
					SkipStatement();
				}
			}

			bool ParseParameter(Parameter& a_p)
			{
				while (IsIdent() && IsIdent(1)) {
					const auto t = Peek().text;
					if (t == "in")
						a_p.in = true;
					else if (t == "out") {
						a_p.out = true;
						a_p.in = false;
					} else if (t == "inout")
						a_p.in = a_p.out = true;
					else if (t == "uniform")
						a_p.uniform = true;
					else if (kInterpolation.contains(t))
						a_p.interpolation = std::string(t);
					else if (!(t == "const" || t == "precise" || t == "row_major" || t == "column_major" || t == "point" || t == "line" ||
								 t == "triangle" || t == "lineadj" || t == "triangleadj"))
						break;
					++pos;
				}
				if (!ParseType(a_p.type) || !IsIdent())
					return false;
				a_p.name = std::string(Peek().text);
				++pos;
				a_p.arraySize = ParseArraySize();
				std::string registerUnused;
				ParseBindings(a_p.semantic, &registerUnused);
				if (Accept("=")) {
					a_p.defaultText = Text(pos, limit);
					pos = limit;
				}
				return AtEnd();
			}

			void ParseStruct()
			{
				const auto start = pos;
				StructDesc s;
				s.isClass = Peek().text == "class";
				s.line = Peek().line;
				++pos;
				s.name = std::string(Peek().text);
				++pos;
				if (Accept(":")) {
					while (!AtEnd() && !Is("{"))
						++pos;
				}
				const auto close = MatchClose(pos);
				WithRange(pos + 1, close - 1, [&] {
					ParseStructBody(s);
					return 0;
				});
				pos = close;
				s.statement = RangeOf(start, pos);
				const std::string name = s.name;
				fx.structs.push_back(std::move(s));
				if (IsIdent()) {
					Type t = Classify(name);
					t.cls = TypeClass::Struct;
					ParseVariables(start, t, {}, {});
					return;
				}
				Accept(";");
			}

			void ParseStructBody(StructDesc& s)
			{
				while (!AtEnd()) {
					if (Accept(";"))
						continue;
					if (Is("[")) {
						pos = MatchClose(pos);
						continue;
					}
					if (Is("public") || Is("private") || Is("protected")) {
						pos += Is(":", 1) ? 2 : 1;
						continue;
					}
					bool isStatic = false;
					std::string interpolation;
					while (IsIdent() && IsIdent(1) &&
						   (kDeclModifiers.contains(Peek().text) || kInterpolation.contains(Peek().text))) {
						isStatic |= Peek().text == "static";
						if (kInterpolation.contains(Peek().text))
							interpolation = std::string(Peek().text);
						++pos;
					}
					Type type;
					const auto memberLine = Peek().line;
					if (!ParseType(type) || !IsIdent()) {
						Diag(Severity::Warning, memberLine, std::format("could not parse a member of '{}'; skipped", s.name));
						while (!AtEnd() && !Is(";")) {
							if (Is("{") || Is("(") || Is("["))
								pos = MatchClose(pos);
							else
								++pos;
						}
						continue;
					}
					if (Is("(", 1)) {
						pos = MatchClose(pos + 1);
						std::string unused;
						ParseBindings(unused, nullptr);
						if (Is("{"))
							pos = MatchClose(pos);
						else
							Accept(";");
						continue;
					}
					while (IsIdent()) {
						StructMember m;
						m.type = type;
						m.interpolation = interpolation;
						m.name = std::string(Peek().text);
						++pos;
						m.arraySize = ParseArraySize();
						ParseBindings(m.semantic, nullptr);
						if (Accept("=")) {
							while (!AtEnd() && !Is(",") && !Is(";")) {
								if (Is("(") || Is("{") || Is("["))
									pos = MatchClose(pos);
								else
									++pos;
							}
						}
						if (!isStatic)
							s.members.push_back(std::move(m));
						if (!Accept(","))
							break;
					}
					if (!Accept(";"))
						Diag(Severity::Warning, memberLine, std::format("expected ';' after a member of '{}'", s.name));
				}
			}

			void ParseConstantBuffer()
			{
				const auto start = pos;
				ConstantBufferDesc cb;
				cb.textureBuffer = Peek().text == "tbuffer";
				cb.line = Peek().line;
				++pos;
				if (IsIdent()) {
					cb.name = std::string(Peek().text);
					++pos;
				}
				std::string unused;
				ParseBindings(unused, &cb.registerBinding);
				if (!Is("{")) {
					Diag(Severity::Error, cb.line, std::format("expected '{{' after cbuffer '{}'", cb.name));
					SkipStatement();
					return;
				}
				const auto close = MatchClose(pos);
				++pos;
				const auto first = fx.variables.size();
				WithRange(pos, close - 1, [&] {
					while (!AtEnd()) {
						if (Accept(";"))
							continue;
						ParseDeclaration(cb.name);
					}
					return 0;
				});
				pos = close;
				Accept(";");
				for (auto i = first; i < fx.variables.size(); ++i)
					cb.variables.push_back(static_cast<std::uint32_t>(i));
				cb.statement = RangeOf(start, pos);
				fx.constantBuffers.push_back(std::move(cb));
			}

			void ParseTypedef()
			{
				const auto line = Peek().line;
				++pos;
				Modifiers mods;
				ParseModifiers(mods);
				if ((Is("struct") || Is("class")) && IsIdent(1))
					++pos;
				Type type;
				if (ParseType(type) && IsIdent()) {
					typedefs[std::string(Peek().text)] = type;
					++pos;
					ParseArraySize();
					if (Accept(";"))
						return;
				}
				Diag(Severity::Warning, line, "could not parse typedef; skipped");
				SkipStatement();
			}

			// State objects

			void ParseStateBlock(const Variable& a_var, bool a_legacy)
			{
				StateObject s;
				s.name = a_var.name;
				s.arraySize = a_var.arraySize;
				s.annotations = a_var.annotations;
				s.line = a_var.line;
				s.legacySamplerState = a_legacy;
				const auto& tn = a_var.type.name;
				s.kind = tn == "BlendState"             ? StateKind::Blend :
				         tn == "DepthStencilState"      ? StateKind::DepthStencil :
				         tn == "RasterizerState"        ? StateKind::Rasterizer :
				         tn == "SamplerComparisonState" ? StateKind::SamplerComparison :
				                                          StateKind::Sampler;
				const auto blockEnd = MatchClose(pos);
				auto first = pos + 1;
				auto last = blockEnd - 1;
				if (At(first).kind == Tok::Punct && At(first).text == "{") {
					Diag(Severity::Warning, s.line, std::format("state array '{}': only its first element is read", s.name));
					last = MatchClose(first) - 1;
					++first;
				}
				WithRange(first, last, [&] {
					ParseStateAssignments(s);
					return 0;
				});
				pos = blockEnd;
				if (a_legacy)
					Diag(Severity::Warning, s.line, std::format("'{}' is a DX9 sampler_state; the ORG runtime binds samplers and textures separately", s.name));
				DecodeState(s);
				fx.states.push_back(std::move(s));
			}

			void ParseStateAssignments(StateObject& s)
			{
				while (!AtEnd()) {
					if (Accept(";"))
						continue;
					StateAssignment a;
					a.line = Peek().line;
					if (!IsIdent()) {
						Diag(Severity::Error, a.line, std::format("expected a state name in '{}', found '{}'", s.name, Peek().text));
						break;
					}
					a.key = std::string(Peek().text);
					++pos;
					if (Is("[")) {
						const auto c = MatchClose(pos);
						Values idx;
						if (Evaluate(pos + 1, c - 1, idx) && idx.size() == 1)
							a.index = static_cast<std::int32_t>(idx[0]);
						pos = c;
					}
					if (!Expect("=", std::format("after state '{}'", a.key)))
						break;
					const auto valueStart = pos;
					while (!AtEnd() && !Is(";")) {
						if (Is("(") || Is("{") || Is("["))
							pos = MatchClose(pos);
						else
							++pos;
					}
					a.value = Text(valueStart, pos);
					if (s.legacySamplerState && EqualsNoCase(a.key, "Texture")) {
						std::string texture = a.value;
						std::erase_if(texture, [](char c) { return c == '<' || c == '>' || c == '(' || c == ')' || std::isspace(static_cast<unsigned char>(c)); });
						s.legacyTexture = texture;
					}
					s.assignments.push_back(std::move(a));
				}
			}

			void UnknownState(const StateObject& a_s, const StateAssignment& a_a)
			{
				Diag(Severity::Warning, a_a.line, std::format("state '{} = {}' in '{}' is not understood", a_a.key, a_a.value, a_s.name));
			}

			/** @brief A state value as a float: a number, or FLT_MAX spelled out. */
			static std::optional<float> StateFloat(const StateAssignment& a_a)
			{
				char* end = nullptr;
				std::string s = a_a.value;
				while (!s.empty() && std::strchr("fFhH", s.back()) != nullptr)
					s.pop_back();
				const double d = std::strtod(s.c_str(), &end);
				if (end && *end == '\0' && !s.empty())
					return static_cast<float>(d);
				if (Upper(a_a.value) == "FLT_MAX" || Upper(a_a.value) == "D3D11_FLOAT32_MAX")
					return FLT_MAX;
				if (Upper(a_a.value) == "-FLT_MAX")
					return -FLT_MAX;
				return std::nullopt;
			}

			void DecodeState(StateObject& a_s)
			{
				for (const auto& a : a_s.assignments) {
					const auto key = Upper(a.key);
					bool ok = true;
					if (a_s.kind == StateKind::Sampler || a_s.kind == StateKind::SamplerComparison) {
						auto& d = a_s.sampler;
						if (key == "FILTER")
							ok = ParseFilter(a.value, d);
						else if (key == "ADDRESSU" || key == "ADDRESSV" || key == "ADDRESSW") {
							auto m = LookupEnum(a.value, "TEXTUREADDRESS", kAddressModes);
							ok = m.has_value();
							if (m)
								(key == "ADDRESSU" ? d.addressU : key == "ADDRESSV" ? d.addressV :
																					  d.addressW) = *m;
						} else if (key == "MIPLODBIAS" || key == "MINLOD" || key == "MAXLOD") {
							auto f = StateFloat(a);
							ok = f.has_value();
							if (f)
								(key == "MIPLODBIAS" ? d.mipLodBias : key == "MINLOD" ? d.minLod :
																						d.maxLod) = *f;
						} else if (key == "MAXANISOTROPY") {
							auto f = StateFloat(a);
							ok = f.has_value();
							if (f)
								d.maxAnisotropy = static_cast<std::uint32_t>(*f);
						} else if (key == "COMPARISONFUNC") {
							auto f = LookupEnum(a.value, "COMPARISON", kCompareFuncs);
							ok = f.has_value();
							if (f)
								d.compare = *f;
						} else if (key == "BORDERCOLOR") {
							Values v;
							ok = EvaluateText(a.value, v) && (v.size() == 4 || v.size() == 1);
							if (ok)
								for (int i = 0; i < 4; ++i)
									d.borderColor[i] = static_cast<float>(v[v.size() == 1 ? 0 : i]);
						} else if (key == "TEXTURE" && a_s.legacySamplerState)
							ok = true;
						else
							ok = false;
					} else if (a_s.kind == StateKind::Blend) {
						auto& d = a_s.blend;
						const auto first = a.index < 0 ? 0 : std::min(a.index, 7);
						auto& rt = d.targets[first];
						if (key == "ALPHATOCOVERAGEENABLE") {
							auto b = ParseBool(a.value);
							ok = b.has_value();
							if (b)
								d.alphaToCoverage = *b;
						} else if (key == "INDEPENDENTBLENDENABLE") {
							auto b = ParseBool(a.value);
							ok = b.has_value();
							if (b)
								d.independentBlend = *b;
						} else if (key == "BLENDENABLE") {
							auto b = ParseBool(a.value);
							ok = b.has_value();
							if (b)
								rt.enable = *b;
						} else if (key == "SRCBLEND" || key == "DESTBLEND" || key == "SRCBLENDALPHA" || key == "DESTBLENDALPHA") {
							auto f = LookupEnum(a.value, "BLEND", kBlendFactors);
							ok = f.has_value();
							if (f)
								(key == "SRCBLEND" ? rt.src : key == "DESTBLEND" ? rt.dest :
														  key == "SRCBLENDALPHA" ? rt.srcAlpha :
																				   rt.destAlpha) = *f;
						} else if (key == "BLENDOP" || key == "BLENDOPALPHA") {
							auto o = LookupEnum(a.value, "BLENDOP", kBlendOps);
							ok = o.has_value();
							if (o)
								(key == "BLENDOP" ? rt.op : rt.opAlpha) = *o;
						} else if (key == "RENDERTARGETWRITEMASK") {
							Values v;
							ok = EvaluateText(a.value, v) && v.size() == 1;
							if (ok)
								rt.writeMask = static_cast<std::uint8_t>(static_cast<int>(v[0]) & 0xF);
						} else
							ok = false;
					} else if (a_s.kind == StateKind::DepthStencil) {
						auto& d = a_s.depthStencil;
						auto face = [&](std::string_view a_k, StencilFace& a_face) -> std::optional<bool> {
							if (key == std::string(a_k) + "STENCILFAIL" || key == std::string(a_k) + "STENCILDEPTHFAIL" || key == std::string(a_k) + "STENCILPASS") {
								auto o = LookupEnum(a.value, "STENCILOP", kStencilOps);
								if (!o)
									return false;
								(key.ends_with("DEPTHFAIL") ? a_face.depthFail : key.ends_with("PASS") ? a_face.pass :
																										 a_face.fail) = *o;
								return true;
							}
							if (key == std::string(a_k) + "STENCILFUNC") {
								auto f = LookupEnum(a.value, "COMPARISON", kCompareFuncs);
								if (!f)
									return false;
								a_face.func = *f;
								return true;
							}
							return std::nullopt;
						};
						if (key == "DEPTHENABLE" || key == "STENCILENABLE") {
							auto b = ParseBool(a.value);
							ok = b.has_value();
							if (b)
								(key == "DEPTHENABLE" ? d.depthEnable : d.stencilEnable) = *b;
						} else if (key == "DEPTHWRITEMASK") {
							const auto v = NormalizeStateValue(a.value, "DEPTHWRITEMASK");
							ok = v == "ZERO" || v == "ALL" || v == "0" || v == "1";
							d.depthWrite = v == "ALL" || v == "1";
						} else if (key == "DEPTHFUNC") {
							auto f = LookupEnum(a.value, "COMPARISON", kCompareFuncs);
							ok = f.has_value();
							if (f)
								d.depthFunc = *f;
						} else if (key == "STENCILREADMASK" || key == "STENCILWRITEMASK") {
							Values v;
							ok = EvaluateText(a.value, v) && v.size() == 1;
							if (ok)
								(key == "STENCILREADMASK" ? d.stencilReadMask : d.stencilWriteMask) = static_cast<std::uint8_t>(v[0]);
						} else if (auto front = face("FRONTFACE", d.front))
							ok = *front;
						else if (auto back = face("BACKFACE", d.back))
							ok = *back;
						else
							ok = false;
					} else if (a_s.kind == StateKind::Rasterizer) {
						auto& d = a_s.rasterizer;
						if (key == "FILLMODE") {
							auto m = LookupEnum(a.value, "FILL", kFillModes);
							ok = m.has_value();
							if (m)
								d.fill = *m;
						} else if (key == "CULLMODE") {
							auto m = LookupEnum(a.value, "CULL", kCullModes);
							ok = m.has_value();
							if (m)
								d.cull = *m;
						} else if (key == "DEPTHBIAS") {
							auto f = StateFloat(a);
							ok = f.has_value();
							if (f)
								d.depthBias = static_cast<std::int32_t>(*f);
						} else if (key == "DEPTHBIASCLAMP" || key == "SLOPESCALEDDEPTHBIAS") {
							auto f = StateFloat(a);
							ok = f.has_value();
							if (f)
								(key == "DEPTHBIASCLAMP" ? d.depthBiasClamp : d.slopeScaledDepthBias) = *f;
						} else {
							bool* flag = key == "FRONTCOUNTERCLOCKWISE" ? &d.frontCounterClockwise :
							             key == "DEPTHCLIPENABLE"       ? &d.depthClip :
							             key == "SCISSORENABLE"         ? &d.scissor :
							             key == "MULTISAMPLEENABLE"     ? &d.multisample :
							             key == "ANTIALIASEDLINEENABLE" ? &d.antialiasedLine :
							                                              nullptr;
							auto b = flag ? ParseBool(a.value) : std::nullopt;
							ok = b.has_value();
							if (b)
								*flag = *b;
						}
					}
					if (!ok)
						UnknownState(a_s, a);
				}
			}

			/** @brief Evaluates a value kept as text by lexing it on its own. */
			bool EvaluateText(const std::string& a_text, Values& a_out)
			{
				EffectDesc scratch;
				Parser sub(a_text, scratch);
				sub.fx.variables = fx.variables;
				return sub.Evaluate(0, sub.toks.size() - 1, a_out);
			}

			// Techniques

			void ParseGroup()
			{
				const auto start = pos;
				Group g;
				g.line = Peek().line;
				++pos;
				if (IsIdent()) {
					g.name = std::string(Peek().text);
					++pos;
				}
				if (Is("<"))
					ParseAnnotations(g.annotations);
				if (!Is("{")) {
					Diag(Severity::Error, g.line, std::format("expected '{{' after fxgroup '{}'", g.name));
					SkipStatement();
					return;
				}
				const auto close = MatchClose(pos);
				const auto groupIndex = fx.groups.size();
				fx.groups.push_back(std::move(g));
				WithRange(pos + 1, close - 1, [&] {
					while (!AtEnd()) {
						if (Accept(";"))
							continue;
						if (Is("technique") || Is("technique10") || Is("technique11")) {
							const auto index = fx.techniques.size();
							ParseTechnique(fx.groups[groupIndex].name);
							if (fx.techniques.size() > index)
								fx.groups[groupIndex].techniques.push_back(static_cast<std::uint32_t>(index));
						} else {
							Diag(Severity::Error, Peek().line, std::format("expected a technique in fxgroup, found '{}'", Peek().text));
							SkipStatement();
						}
					}
					return 0;
				});
				pos = close;
				Accept(";");
				fx.groups[groupIndex].statement = RangeOf(start, pos);
			}

			void ParseTechnique(const std::string& a_group)
			{
				const auto start = pos;
				Technique t;
				t.line = Peek().line;
				t.group = a_group;
				t.version = Peek().text == "technique11" ? 11u : Peek().text == "technique10" ? 10u :
				                                                                                9u;
				++pos;
				if (IsIdent()) {
					t.name = std::string(Peek().text);
					++pos;
				}
				if (Is("<"))
					ParseAnnotations(t.annotations);
				if (!Is("{")) {
					Diag(Severity::Error, t.line, std::format("expected '{{' after technique '{}'", t.name));
					SkipStatement();
					return;
				}
				const auto close = MatchClose(pos);
				WithRange(pos + 1, close - 1, [&] {
					while (!AtEnd()) {
						if (Accept(";"))
							continue;
						if (Is("pass")) {
							ParsePass(t);
							continue;
						}
						Diag(Severity::Error, Peek().line, std::format("expected 'pass' in technique '{}', found '{}'", t.name, Peek().text));
						SkipStatement();
					}
					return 0;
				});
				pos = close;
				Accept(";");
				t.statement = RangeOf(start, pos);
				if (t.passes.empty())
					Diag(Severity::Warning, t.line, std::format("technique '{}' has no passes", t.name));
				fx.techniques.push_back(std::move(t));
			}

			static std::optional<Stage> StageOfCall(std::string_view a_name)
			{
				if (a_name == "SetVertexShader" || a_name == "VertexShader")
					return Stage::Vertex;
				if (a_name == "SetPixelShader" || a_name == "PixelShader")
					return Stage::Pixel;
				if (a_name == "SetGeometryShader" || a_name == "GeometryShader")
					return Stage::Geometry;
				if (a_name == "SetHullShader" || a_name == "HullShader")
					return Stage::Hull;
				if (a_name == "SetDomainShader" || a_name == "DomainShader")
					return Stage::Domain;
				if (a_name == "SetComputeShader" || a_name == "ComputeShader")
					return Stage::Compute;
				return std::nullopt;
			}

			void ParsePass(Technique& a_t)
			{
				Pass p;
				p.line = Peek().line;
				++pos;
				if (IsIdent()) {
					p.name = std::string(Peek().text);
					++pos;
				}
				if (Is("<"))
					ParseAnnotations(p.annotations);
				if (!Is("{")) {
					Diag(Severity::Error, p.line, std::format("expected '{{' after pass '{}'", p.name));
					SkipStatement();
					return;
				}
				const auto close = MatchClose(pos);
				WithRange(pos + 1, close - 1, [&] {
					ParsePassBody(p);
					return 0;
				});
				pos = close;
				a_t.passes.push_back(std::move(p));
			}

			void ParsePassBody(Pass& p)
			{
				while (!AtEnd()) {
					if (Accept(";"))
						continue;
					const auto line = Peek().line;
					if (!IsIdent()) {
						Diag(Severity::Error, line, std::format("unexpected '{}' in pass '{}'", Peek().text, p.name));
						SkipStatement();
						continue;
					}
					const std::string name(Peek().text);
					if (Is("(", 1)) {
						const auto callClose = MatchClose(pos + 1);
						const auto args = SplitArgs(pos + 2, callClose - 1);
						pos = callClose;
						if (!Accept(";"))
							Diag(Severity::Warning, line, std::format("expected ';' after {}", name));
						if (auto stage = StageOfCall(name); stage && name.starts_with("Set")) {
							auto& binding = p.shaders[static_cast<std::size_t>(*stage)];
							if (!args.empty())
								ParseShaderExpression(args[0].first, args[0].second, binding, line);
						} else if (name == "SetBlendState") {
							if (!args.empty())
								p.blendState = Text(args[0].first, args[0].second);
							Values v;
							if (args.size() > 1 && Evaluate(args[1].first, args[1].second, v) && v.size() == 4)
								for (int i = 0; i < 4; ++i)
									p.blendFactor[i] = static_cast<float>(v[i]);
							if (args.size() > 2 && Evaluate(args[2].first, args[2].second, v) && v.size() == 1)
								p.sampleMask = static_cast<std::uint32_t>(v[0]);
						} else if (name == "SetDepthStencilState") {
							if (!args.empty())
								p.depthStencilState = Text(args[0].first, args[0].second);
							Values v;
							if (args.size() > 1 && Evaluate(args[1].first, args[1].second, v) && v.size() == 1)
								p.stencilRef = static_cast<std::uint32_t>(v[0]);
						} else if (name == "SetRasterizerState") {
							if (!args.empty())
								p.rasterizerState = Text(args[0].first, args[0].second);
						} else
							Diag(Severity::Warning, line, std::format("pass call '{}' is not understood", name));
						continue;
					}
					// The FX9/10 form: Key[ i ] = value;
					StateAssignment a;
					a.line = line;
					a.key = name;
					++pos;
					if (Is("[")) {
						const auto c = MatchClose(pos);
						Values idx;
						if (Evaluate(pos + 1, c - 1, idx) && idx.size() == 1)
							a.index = static_cast<std::int32_t>(idx[0]);
						pos = c;
					}
					if (!Expect("=", std::format("after '{}' in pass '{}'", name, p.name))) {
						SkipStatement();
						continue;
					}
					const auto valueStart = pos;
					while (!AtEnd() && !Is(";")) {
						if (Is("(") || Is("{") || Is("["))
							pos = MatchClose(pos);
						else
							++pos;
					}
					const auto valueEnd = pos;
					Accept(";");
					if (auto stage = StageOfCall(name)) {
						ParseShaderExpression(valueStart, valueEnd, p.shaders[static_cast<std::size_t>(*stage)], line);
						continue;
					}
					a.value = Text(valueStart, valueEnd);
					Diag(Severity::Warning, line, std::format("render state '{} = {}' in pass '{}' is not applied by the ORG runtime", a.key, a.value, p.name));
					p.otherStates.push_back(std::move(a));
				}
			}

			/** @brief CompileShader(profile, Entry(args)), compile profile Entry(args), NULL, or a shader variable's name. */
			void ParseShaderExpression(std::size_t a_first, std::size_t a_last, ShaderBinding& a_out, std::uint32_t a_line)
			{
				a_out = {};
				a_out.set = true;
				a_out.line = a_line;
				WithRange(a_first, a_last, [&] {
					if ((Is("NULL") || (Peek().kind == Tok::Number && Peek().text == "0")) && Peek(1).kind == Tok::End) {
						a_out.null = true;
						return 0;
					}
					if (Is("CompileShader") && Is("(", 1)) {
						const auto close = MatchClose(pos + 1);
						const auto args = SplitArgs(pos + 2, close - 1);
						if (args.size() != 2) {
							Diag(Severity::Error, a_line, std::format("CompileShader takes a profile and an entry call: '{}'", Text(a_first, a_last)));
							return 0;
						}
						a_out.profile = Text(args[0].first, args[0].second);
						ParseEntryCall(args[1].first, args[1].second, a_out);
						return 0;
					}
					if (Is("compile") && IsIdent(1)) {
						a_out.profile = std::string(Peek(1).text);
						ParseEntryCall(pos + 2, limit, a_out);
						return 0;
					}
					if (Is("ConstructGSWithSO") && Is("(", 1)) {
						Diag(Severity::Warning, a_line, "ConstructGSWithSO (stream output) is not supported by the ORG runtime");
						const auto close = MatchClose(pos + 1);
						const auto args = SplitArgs(pos + 2, close - 1);
						if (!args.empty()) {
							ShaderBinding inner;
							ParseShaderExpression(args[0].first, args[0].second, inner, a_line);
							a_out = std::move(inner);
						}
						return 0;
					}
					if (IsIdent() && Peek(1).kind == Tok::End) {
						const std::string name(Peek().text);
						if (auto it = shaderVariables.find(name); it != shaderVariables.end()) {
							a_out = it->second;
							a_out.variable = name;
							a_out.line = a_line;
							return 0;
						}
						Diag(Severity::Error, a_line, std::format("'{}' is not a shader variable", name));
						return 0;
					}
					Diag(Severity::Error, a_line, std::format("shader expression '{}' is not understood", Text(a_first, a_last)));
					return 0;
				});
			}

			void ParseEntryCall(std::size_t a_first, std::size_t a_last, ShaderBinding& a_out)
			{
				WithRange(a_first, a_last, [&] {
					if (!IsIdent() || !Is("(", 1)) {
						Diag(Severity::Error, Peek().line, std::format("expected an entry call, found '{}'", Text(a_first, a_last)));
						return 0;
					}
					a_out.entry = std::string(Peek().text);
					const auto close = MatchClose(pos + 1);
					for (auto [b, e] : SplitArgs(pos + 2, close - 1)) {
						if (b == e)
							continue;
						ShaderArgument arg;
						arg.text = Text(b, e);
						if (e == b + 1 && toks[b].kind == Tok::Ident && toks[b].text != "true" && toks[b].text != "false") {
							const std::string name(toks[b].text);
							if (const auto* v = fx.FindVariable(name)) {
								const bool resource = v->type.cls == TypeClass::Texture || v->type.cls == TypeClass::RWResource ||
								                      v->type.cls == TypeClass::Sampler;
								arg.kind = resource ? ShaderArgument::Kind::Resource : ShaderArgument::Kind::Variable;
								if (!resource && v->modifiers.isStatic && v->initializerEvaluated) {
									arg.kind = ShaderArgument::Kind::Literal;
									arg.value = v->initializer;
								}
							} else if (fx.FindState(name))
								arg.kind = ShaderArgument::Kind::Resource;
						} else if (Evaluate(b, e, arg.value))
							arg.kind = ShaderArgument::Kind::Literal;
						a_out.arguments.push_back(std::move(arg));
					}
					pos = close;
					if (!AtEnd())
						Diag(Severity::Warning, Peek().line, std::format("text after the entry call '{}' is ignored", Text(a_first, a_last)));
					return 0;
				});
			}

			// Post-processing

			void ResolveEntries()
			{
				for (auto& t : fx.techniques) {
					for (auto& p : t.passes) {
						for (std::size_t s = 0; s < p.shaders.size(); ++s) {
							auto& b = p.shaders[s];
							if (!b.set || b.null || b.entry.empty())
								continue;
							std::vector<std::int32_t> candidates;
							for (std::size_t f = 0; f < fx.functions.size(); ++f)
								if (fx.functions[f].name == b.entry)
									candidates.push_back(static_cast<std::int32_t>(f));
							if (candidates.empty()) {
								Diag(Severity::Error, b.line, std::format("{} entry '{}' of technique '{}' pass '{}' is not defined", StageName(static_cast<Stage>(s)), b.entry, t.name, p.name));
								continue;
							}
							for (auto f : candidates) {
								const auto& fn = fx.functions[f];
								std::vector<std::int32_t> uniforms;
								for (std::size_t i = 0; i < fn.parameters.size(); ++i)
									if (fn.parameters[i].uniform)
										uniforms.push_back(static_cast<std::int32_t>(i));
								if (uniforms.size() != b.arguments.size())
									continue;
								if (b.function >= 0) {
									Diag(Severity::Info, b.line, std::format("entry '{}' has several overloads taking {} uniform arguments; the first is used", b.entry, uniforms.size()));
									break;
								}
								b.function = f;
								b.argumentParameters = std::move(uniforms);
							}
							if (b.function < 0)
								Diag(Severity::Error, b.line, std::format("no overload of '{}' takes {} uniform argument(s) ({} pass '{}')", b.entry, b.arguments.size(), t.name, p.name));
						}
					}
				}
			}

			void BuildChains()
			{
				auto chainOf = [&](const std::string& a_key) -> TechniqueChain& {
					for (auto& c : fx.chains)
						if (c.key == a_key)
							return c;
					fx.chains.push_back({ a_key, {} });
					return fx.chains.back();
				};
				for (std::size_t i = 0; i < fx.techniques.size(); ++i) {
					const auto& t = fx.techniques[i];
					if (!t.group.empty())
						continue;
					const std::string name = t.name.empty() ? "technique" + std::to_string(i) : t.name;
					std::string base = name;
					while (!base.empty() && IsDigit(base.back()))
						base.pop_back();
					const bool joins = !base.empty() && base != name && fx.FindChain(base) != nullptr;
					chainOf(joins ? base : name).techniques.push_back(static_cast<std::uint32_t>(i));
				}
				for (const auto& g : fx.groups)
					if (!g.name.empty())
						chainOf(g.name).techniques = g.techniques;
			}

			struct Placement
			{
				std::uint32_t size = 0;
				bool startsRow = false;  // matrices, arrays and structs start a new 16-byte row
			};

			/** @brief One element's size in a cbuffer with FXC packing. */
			std::optional<Placement> ElementPlacement(const Type& a_type, const Modifiers& a_mods, int a_depth = 0)
			{
				if (a_type.cls == TypeClass::Numeric) {
					const auto bytes = ScalarBytes(a_type.scalar);
					if (a_type.matrix) {
						const std::uint32_t registers = a_mods.rowMajor ? a_type.rows : a_type.cols;
						const std::uint32_t components = a_mods.rowMajor ? a_type.cols : a_type.rows;
						return Placement{ 16u * (registers - 1u) + components * bytes, true };
					}
					return Placement{ a_type.cols * bytes, false };
				}
				if (a_type.cls == TypeClass::Struct && a_depth < 8) {
					const auto* s = fx.FindStruct(a_type.name);
					if (!s)
						return std::nullopt;
					std::uint32_t offset = 0;
					for (const auto& m : s->members) {
						auto placement = Place(offset, m.type, {}, m.arraySize, a_depth + 1);
						if (!placement)
							return std::nullopt;
						offset = placement->second;
					}
					return Placement{ offset, true };
				}
				return std::nullopt;
			}

			/** @brief Places a value at a_offset: its start and end offsets. */
			std::optional<std::pair<std::uint32_t, std::uint32_t>> Place(std::uint32_t a_offset, const Type& a_type, const Modifiers& a_mods, std::uint32_t a_arraySize, int a_depth = 0)
			{
				auto element = ElementPlacement(a_type, a_mods, a_depth);
				if (!element)
					return std::nullopt;
				std::uint32_t start = a_offset;
				std::uint32_t size = element->size;
				if (a_arraySize > 0) {
					start = Align16(start);
					size = Align16(element->size) * (a_arraySize - 1) + element->size;
				} else if (element->startsRow || (start % 16u) + size > 16u)
					start = Align16(start);
				return std::pair{ start, start + size };
			}

			bool InConstantBuffer(const Variable& a_v) const
			{
				return !a_v.modifiers.isStatic && !a_v.modifiers.isGroupShared &&
				       (a_v.type.cls == TypeClass::Numeric || a_v.type.cls == TypeClass::Struct);
			}

			void ComputeLayouts()
			{
				// Struct members first: their offsets are what a lowered struct load reads.
				for (auto& s : fx.structs) {
					std::uint32_t offset = 0;
					bool ok = true;
					for (auto& m : s.members) {
						auto placement = Place(offset, m.type, {}, m.arraySize);
						if (!placement) {
							ok = false;
							break;
						}
						m.offset = placement->first;
						offset = placement->second;
					}
					s.size = ok ? offset : 0;
				}
				std::unordered_map<std::string, std::uint32_t> ends;
				for (auto& v : fx.variables) {
					if (!InConstantBuffer(v))
						continue;
					auto& end = ends[v.cbuffer];
					auto placement = Place(end, v.type, v.modifiers, v.arraySize);
					if (!placement) {
						Diag(Severity::Error, v.line, std::format("cannot lay out '{}' of type '{}' in a constant buffer", v.name, v.type.name));
						continue;
					}
					v.bufferOffset = static_cast<std::int32_t>(placement->first);
					v.bufferSize = placement->second - placement->first;
					end = placement->second;
				}
				fx.globalsSize = Align16(ends[{}]);
				for (auto& cb : fx.constantBuffers)
					cb.size = Align16(ends[cb.name]);
			}

			/** @brief Warnings for what parses but the ORG runtime will not run as FX11 does. */
			void CheckRuntimeSupport()
			{
				for (const auto& t : fx.techniques)
					for (const auto& p : t.passes) {
						for (std::size_t s = 0; s < p.shaders.size(); ++s) {
							const auto& b = p.shaders[s];
							const auto stage = static_cast<Stage>(s);
							if (b.set && !b.null && stage != Stage::Vertex && stage != Stage::Pixel)
								Diag(Severity::Warning, b.line, std::format("technique '{}' pass '{}' sets a {} shader, which the ORG runtime does not run", t.name, p.name, StageName(stage)));
							for (const auto& a : b.arguments)
								if (a.kind == ShaderArgument::Kind::Expression)
									Diag(Severity::Warning, b.line, std::format("argument '{}' of '{}' is not a constant or a global", a.text, b.entry));
						}
						if (!p.shaders[static_cast<std::size_t>(Stage::Vertex)].set || !p.shaders[static_cast<std::size_t>(Stage::Pixel)].set)
							Diag(Severity::Warning, p.line, std::format("technique '{}' pass '{}' does not set both a vertex and a pixel shader", t.name, p.name));
					}
				for (const auto& v : fx.variables) {
					if (v.type.cls == TypeClass::RWResource)
						Diag(Severity::Warning, v.line, std::format("'{}' is a UAV; the ORG runtime binds textures only", v.name));
					if (v.modifiers.isShared)
						Diag(Severity::Warning, v.line, std::format("'{}' is shared (effect pools are not supported)", v.name));
				}
			}
		};
	}

	EffectDesc Parse(std::string_view a_source)
	{
		EffectDesc fx;
		Parser parser(a_source, fx);
		parser.Run();
		return fx;
	}

	std::vector<std::uint8_t> BuildDefaultValues(const EffectDesc& a_fx, std::string_view a_cbuffer)
	{
		std::uint32_t size = a_fx.globalsSize;
		if (!a_cbuffer.empty()) {
			size = 0;
			for (const auto& cb : a_fx.constantBuffers)
				if (cb.name == a_cbuffer)
					size = cb.size;
		}
		std::vector<std::uint8_t> image(size, 0);
		for (const auto& v : a_fx.variables) {
			if (v.cbuffer != a_cbuffer || v.bufferOffset < 0 || !v.initializerEvaluated || v.initializer.empty() || v.type.cls != TypeClass::Numeric)
				continue;
			const auto& t = v.type;
			const std::uint32_t bytes = ScalarBytes(t.scalar);
			const std::uint32_t components = static_cast<std::uint32_t>(t.rows) * t.cols;
			const std::uint32_t elements = std::max(v.arraySize, 1u);
			// Element size and stride, as Place lays them out.
			std::uint32_t elementSize = t.cols * bytes;
			if (t.matrix) {
				const std::uint32_t registers = v.modifiers.rowMajor ? t.rows : t.cols;
				const std::uint32_t perRegister = v.modifiers.rowMajor ? t.cols : t.rows;
				elementSize = 16u * (registers - 1u) + perRegister * bytes;
			}
			const std::uint32_t stride = v.arraySize > 0 ? Align16(elementSize) : elementSize;
			const bool broadcast = v.initializer.size() == 1;
			for (std::uint32_t e = 0; e < elements; ++e) {
				for (std::uint32_t c = 0; c < components; ++c) {
					const std::size_t index = broadcast ? 0 : static_cast<std::size_t>(e) * components + c;
					if (index >= v.initializer.size())
						break;
					const double value = v.initializer[index];
					// FXC reads a global matrix's initializer list column by column, whatever the HLSL constructor order
					// (fx-parse-check compares with FXC's reflection). FX11 starts from FXC's bytes, so this does the same.
					const std::uint32_t row = c % t.rows;
					const std::uint32_t col = c / t.rows;
					std::uint32_t offset = static_cast<std::uint32_t>(v.bufferOffset) + e * stride;
					if (t.matrix)
						offset += v.modifiers.rowMajor ? row * 16u + col * bytes : col * 16u + row * bytes;
					else
						offset += c * bytes;
					if (offset + bytes > image.size())
						continue;
					auto* out = image.data() + offset;
					switch (t.scalar) {
					case Scalar::Double:
						std::memcpy(out, &value, 8);
						break;
					case Scalar::Int:
					case Scalar::Min16Int:
					case Scalar::Min12Int:
						{
							const auto i = static_cast<std::int32_t>(value);
							std::memcpy(out, &i, 4);
							break;
						}
					case Scalar::Uint:
					case Scalar::Min16Uint:
						{
							const auto u = static_cast<std::uint32_t>(value);
							std::memcpy(out, &u, 4);
							break;
						}
					case Scalar::Bool:
						{
							const std::uint32_t b = value != 0.0 ? 0xFFFFFFFFu : 0u;  // FXC stores true as all bits set
							std::memcpy(out, &b, 4);
							break;
						}
					default:
						{
							const auto f = static_cast<float>(value);
							std::memcpy(out, &f, 4);
							break;
						}
					}
				}
			}
		}
		return image;
	}

	const Variable* EffectDesc::FindVariable(std::string_view a_name) const
	{
		for (const auto& v : variables)
			if (v.name == a_name)
				return &v;
		return nullptr;
	}

	const StateObject* EffectDesc::FindState(std::string_view a_name) const
	{
		for (const auto& s : states)
			if (s.name == a_name)
				return &s;
		return nullptr;
	}

	const Technique* EffectDesc::FindTechnique(std::string_view a_name) const
	{
		for (const auto& t : techniques)
			if (t.name == a_name)
				return &t;
		return nullptr;
	}

	const StructDesc* EffectDesc::FindStruct(std::string_view a_name) const
	{
		for (const auto& s : structs)
			if (s.name == a_name)
				return &s;
		return nullptr;
	}

	const TechniqueChain* EffectDesc::FindChain(std::string_view a_key) const
	{
		for (const auto& c : chains)
			if (c.key == a_key)
				return &c;
		return nullptr;
	}

	std::size_t EffectDesc::Count(Severity a_severity) const
	{
		return static_cast<std::size_t>(std::count_if(diagnostics.begin(), diagnostics.end(), [&](const Diagnostic& d) { return d.severity == a_severity; }));
	}

	const Annotation* FindAnnotation(const Annotations& a_annotations, std::string_view a_name)
	{
		for (const auto& a : a_annotations)
			if (a.name == a_name)
				return &a;
		for (const auto& a : a_annotations)
			if (EqualsNoCase(a.name, a_name))
				return &a;
		return nullptr;
	}

	std::string AnnotationText(const Annotations& a_annotations, std::string_view a_name)
	{
		const auto* a = FindAnnotation(a_annotations, a_name);
		return a ? a->text : std::string{};
	}

	const char* StageName(Stage a_stage)
	{
		switch (a_stage) {
		case Stage::Vertex:
			return "vertex";
		case Stage::Pixel:
			return "pixel";
		case Stage::Geometry:
			return "geometry";
		case Stage::Hull:
			return "hull";
		case Stage::Domain:
			return "domain";
		case Stage::Compute:
			return "compute";
		default:
			return "unknown";
		}
	}
}
