// The D3D12 builds' frame rewrite (FrameSource.h). Standard library only, so tools/shader-compare's dclf-shadertool builds it as the
// game does.
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/FrameSource.h"

#include <bit>
#include <cctype>
#include <format>
#include <optional>
#include <vector>

namespace DCLF::FrameSource
{
	namespace
	{
		bool IsWord(char a_c) { return std::isalnum(static_cast<unsigned char>(a_c)) || a_c == '_'; }
		bool IsSpace(char a_c) { return std::isspace(static_cast<unsigned char>(a_c)) != 0; }

		std::string Trim(std::string_view a_text)
		{
			std::size_t first = 0, last = a_text.size();
			while (first < last && IsSpace(a_text[first]))
				++first;
			while (last > first && IsSpace(a_text[last - 1]))
				--last;
			return std::string(a_text.substr(first, last - first));
		}

		std::size_t SkipSpace(std::string_view a_text, std::size_t a_at)
		{
			while (a_at < a_text.size() && IsSpace(a_text[a_at]))
				++a_at;
			return a_at;
		}

		// Reads digits at a_at into a_value; false when there are none.
		bool ReadNumber(std::string_view a_text, std::size_t& a_at, std::uint32_t& a_value)
		{
			const std::size_t start = a_at;
			a_value = 0;
			while (a_at < a_text.size() && std::isdigit(static_cast<unsigned char>(a_text[a_at])))
				a_value = a_value * 10 + static_cast<std::uint32_t>(a_text[a_at++] - '0');
			return a_at > start;
		}

		// Splits a_text into its preprocessor lines (kept as they are) and the rest (the declaration's text).
		void SplitDirectives(std::string_view a_text, std::string& a_directives, std::string& a_rest)
		{
			std::size_t at = 0;
			while (at < a_text.size()) {
				std::size_t end = a_text.find('\n', at);
				end = end == std::string_view::npos ? a_text.size() : end + 1;
				const auto line = a_text.substr(at, end - at);
				const auto trimmed = Trim(line);
				if (!trimmed.empty() && trimmed[0] == '#')
					a_directives += Trim(line) + "\n";
				else
					a_rest += line;
				at = end;
			}
		}

		// A declarator: the type (with its qualifiers), the name and the array count (0: none).
		struct Declarator
		{
			std::string qualifiers;  // row_major or column_major, if given
			std::string type;
			std::string name;
			std::uint32_t count = 0;
			bool array = false;
		};

		bool ParseDeclarator(std::string_view a_text, Declarator& a_out)
		{
			std::string text = Trim(a_text);
			a_out = {};
			// The array suffix.
			if (!text.empty() && text.back() == ']') {
				const auto open = text.rfind('[');
				if (open == std::string::npos)
					return false;
				std::size_t at = open + 1;
				at = SkipSpace(text, at);
				if (!ReadNumber(text, at, a_out.count))
					return false;
				a_out.array = true;
				text = Trim(std::string_view(text).substr(0, open));
			}
			std::size_t end = text.size();
			std::size_t begin = end;
			while (begin > 0 && IsWord(text[begin - 1]))
				--begin;
			if (begin == end)
				return false;
			a_out.name = text.substr(begin, end - begin);
			std::string type = Trim(std::string_view(text).substr(0, begin));
			// Matrix orientation and other qualifiers before the type.
			for (;;) {
				bool removed = false;
				for (const char* qualifier : { "row_major", "column_major", "uniform", "precise" }) {
					const std::string_view q(qualifier);
					if (type.size() > q.size() && type.compare(0, q.size(), q) == 0 && IsSpace(type[q.size()])) {
						if (q == "row_major" || q == "column_major")
							a_out.qualifiers = std::string(q);
						type = Trim(std::string_view(type).substr(q.size()));
						removed = true;
					}
				}
				if (!removed)
					break;
			}
			a_out.type = type;
			return !a_out.type.empty();
		}

		// A packed member's shape: its scalar type and its rows and columns (a vector: 1 row; a matrix: rows x columns).
		struct Shape
		{
			std::string scalar;
			std::uint32_t rows = 1, columns = 1;
			bool matrix = false;
		};

		bool ParseShape(const std::string& a_type, Shape& a_out)
		{
			static constexpr std::string_view kScalars[] = { "float", "uint", "int", "bool", "dword" };
			for (const auto scalar : kScalars) {
				if (a_type.compare(0, scalar.size(), scalar) != 0)
					continue;
				const std::string_view rest = std::string_view(a_type).substr(scalar.size());
				a_out = {};
				a_out.scalar = std::string(scalar);
				if (rest.empty())
					return true;
				if (rest.size() == 1 && rest[0] >= '1' && rest[0] <= '4') {
					a_out.columns = static_cast<std::uint32_t>(rest[0] - '0');
					return true;
				}
				if (rest.size() == 3 && rest[1] == 'x' && rest[0] >= '1' && rest[0] <= '4' && rest[2] >= '1' && rest[2] <= '4') {
					a_out.rows = static_cast<std::uint32_t>(rest[0] - '0');
					a_out.columns = static_cast<std::uint32_t>(rest[2] - '0');
					a_out.matrix = true;
					return true;
				}
				return false;
			}
			return false;
		}

		// A register's components (a_first, a_count of them) as a_scalar: the float4 the block gives is reinterpreted.
		std::string Components(const std::string& a_register, std::uint32_t a_first, std::uint32_t a_count, const std::string& a_scalar)
		{
			const std::string value = std::format("{}.{}", a_register, std::string_view("xyzw").substr(a_first, a_count));
			if (a_scalar == "float")
				return value;
			if (a_scalar == "int")
				return std::format("asint({})", value);
			if (a_scalar == "bool")
				return std::format("(asuint({}) != 0)", value);
			return std::format("asuint({})", value);  // uint, dword
		}

		struct Rewriter
		{
			const Options& options;
			std::string out;
			Result result;
			std::uint32_t blockNumber = 0;

			bool Fail(std::string a_error)
			{
				result.error = std::move(a_error);
				return false;
			}

			// A resource: its static from the heap.
			bool Resource(char a_class, std::uint32_t a_register, std::string_view a_declaration)
			{
				Declarator declarator;
				if (!ParseDeclarator(a_declaration, declarator))
					return Fail(std::format("the declaration of {}{} is not understood: {}", a_class, a_register, Trim(a_declaration)));
				if (declarator.array)
					return Fail(std::format("{} is an array of resources at {}{}, which the D3D12 build cannot read from the frame record", declarator.name, a_class,
						a_register));
				if (a_class == 'u')
					return Fail(std::format("{} is a UAV at u{}, which DCLF's layouts do not map", declarator.name, a_register));
				if (a_class == 't') {
					out += std::format("static {} {} = ResourceDescriptorHeap[DCLFFrameTextureIndex({})];", declarator.type, declarator.name, a_register);
					++result.textures;
				} else {
					out += std::format("static {} {} = SamplerDescriptorHeap[DCLFFrameSamplerIndex({})];", declarator.type, declarator.name, a_register);
					++result.samplers;
				}
				return true;
			}

			// A constant buffer's members: a_body split at its statements, preprocessor lines left out.
			bool Members(std::string_view a_body, std::vector<std::pair<Declarator, std::optional<std::uint32_t>>>& a_members)
			{
				std::string body;
				std::string directives;
				SplitDirectives(a_body, directives, body);
				std::size_t at = 0;
				while (at < body.size()) {
					std::size_t end = body.find(';', at);
					if (end == std::string::npos) {
						if (!Trim(std::string_view(body).substr(at)).empty())
							return Fail(std::format("a constant buffer member is not closed: {}", Trim(std::string_view(body).substr(at))));
						break;
					}
					const std::string statement = Trim(std::string_view(body).substr(at, end - at));
					at = end + 1;
					if (statement.empty())
						continue;
					std::optional<std::uint32_t> offset;
					std::string_view declaration = statement;
					// ": packoffset(c<n>[.<component>])"
					if (const auto colon = statement.find(':'); colon != std::string::npos) {
						declaration = std::string_view(statement).substr(0, colon);
						std::string_view rest = std::string_view(statement).substr(colon + 1);
						std::size_t p = SkipSpace(rest, 0);
						if (rest.compare(p, 10, "packoffset") != 0)
							return Fail(std::format("a constant buffer member's annotation is not understood: {}", statement));
						p = SkipSpace(rest, p + 10);
						if (p >= rest.size() || rest[p] != '(')
							return Fail(std::format("packoffset not understood: {}", statement));
						p = SkipSpace(rest, p + 1);
						if (p >= rest.size() || rest[p] != 'c')
							return Fail(std::format("packoffset not understood: {}", statement));
						++p;
						std::uint32_t row = 0;
						if (!ReadNumber(rest, p, row))
							return Fail(std::format("packoffset not understood: {}", statement));
						std::uint32_t component = 0;
						if (p < rest.size() && rest[p] == '.') {
							const auto c = std::string_view("xyzw").find(rest[p + 1]);
							if (p + 1 >= rest.size() || c == std::string_view::npos)
								return Fail(std::format("packoffset not understood: {}", statement));
							component = static_cast<std::uint32_t>(c);
							p += 2;
						}
						offset = 16 * row + 4 * component;
					}
					if (declaration.find(',') != std::string_view::npos && declaration.find('<') == std::string_view::npos)
						return Fail(std::format("a constant buffer statement declares more than one member: {}", statement));
					Declarator declarator;
					if (!ParseDeclarator(declaration, declarator))
						return Fail(std::format("a constant buffer member is not understood: {}", statement));
					a_members.emplace_back(std::move(declarator), offset);
				}
				return true;
			}

			// A constant buffer: its view, then a static per member.
			bool Block(std::string_view a_name, std::uint32_t a_register, std::string_view a_body)
			{
				std::vector<std::pair<Declarator, std::optional<std::uint32_t>>> members;
				if (!Members(a_body, members))
					return false;
				const std::string name(a_name);
				const std::uint32_t mask = options.pixel ? options.pixelBlocks : options.vertexBlocks;
				const bool passWide = a_register < 32 && ((mask >> a_register) & 1);
				if (a_register >= 14)
					return Fail(std::format("constant buffer {} is at b{}, which is no register a DCLF pass gives", name, a_register));
				std::size_t packed = 0;
				for (const auto& member : members)
					packed += member.second ? 1 : 0;
				if (packed != 0 && packed != members.size())
					return Fail(std::format("constant buffer {} places some members with packoffset and not others", name));
				const std::uint32_t ordinal = (options.pixel ? static_cast<std::uint32_t>(std::popcount(options.vertexBlocks)) : 0u) +
				                              static_cast<std::uint32_t>(std::popcount(mask & ((1u << a_register) - 1)));
				const std::string stage = options.pixel ? "PS" : "VS";
				out += std::format("// {} b{} ({}): the pass's {} (DCLF FrameSource)\n", name, a_register, stage, passWide ? "view of the block" : "block, by address");
				++result.blocks;
				if (packed == 0) {
					// The same members in a struct, read as a ConstantBuffer: cbuffer packing, as the block was written.
					if (!passWide)
						return Fail(std::format("constant buffer {} at {} b{} has no packoffset and no view of its own in the pass", name, stage, a_register));
					out += std::format("struct DCLFBlock_{}\n{{\n", name);
					for (const auto& [member, offset] : members)
						out += std::format("\t{}{}{} {}{};\n", member.qualifiers, member.qualifiers.empty() ? "" : " ", member.type, member.name,
							member.array ? std::format("[{}]", member.count) : std::string{});
					out += "};\n";
					out += std::format("static ConstantBuffer<DCLFBlock_{0}> DCLFBlockView_{0} = ResourceDescriptorHeap[DCLFFrameBlockIndex({1})];\n", name, ordinal);
					for (const auto& [member, offset] : members) {
						out += std::format("static const {} {}{} = DCLFBlockView_{}.{};\n", member.type, member.name,
							member.array ? std::format("[{}]", member.count) : std::string{}, name, member.name);
						++result.members;
					}
					return true;
				}
				// Register by register: the pass's view, or the block's raw view by its address.
				std::string registerOf;
				if (passWide) {
					out += std::format("static ConstantBuffer<DCLFRawBlock> DCLFBlockView_{} = ResourceDescriptorHeap[DCLFFrameBlockIndex({})];\n", name, ordinal);
					registerOf = std::format("DCLFBlockView_{}.r[{{}}]", name);
				} else {
					out += std::format("static const uint2 DCLFBlockSpan_{} = DCLFFrameBlockSpan({}, {});\n", name, options.pixel ? "true" : "false", a_register);
					registerOf = std::format("DCLFSpanRegister(DCLFBlockSpan_{}, {{}})", name);
				}
				const auto reg = [&](std::uint32_t a_row) { return std::vformat(registerOf, std::make_format_args(a_row)); };
				for (const auto& [member, offset] : members) {
					Shape shape;
					if (!ParseShape(member.type, shape))
						return Fail(std::format("constant buffer {} member {} has a type the D3D12 build does not read by register: {}", name, member.name, member.type));
					const std::uint32_t row = *offset / 16, component = (*offset % 16) / 4;
					const bool rowMajor = member.qualifiers == "row_major";
					// An element's value at a_row, and the registers it spans.
					const auto element = [&](std::uint32_t a_row) -> std::string {
						if (!shape.matrix)
							return Components(reg(a_row), component, shape.columns, shape.scalar);
						const std::string type = std::format("{}{}x{}", shape.scalar, rowMajor ? shape.rows : shape.columns, rowMajor ? shape.columns : shape.rows);
						std::string value = type + "(";
						const std::uint32_t lines = rowMajor ? shape.rows : shape.columns, width = rowMajor ? shape.columns : shape.rows;
						for (std::uint32_t l = 0; l < lines; ++l)
							value += (l ? ", " : "") + Components(reg(a_row + l), 0, width, shape.scalar);
						value += ")";
						// Column-major: the registers are the columns.
						return rowMajor ? value : std::format("transpose({})", value);
					};
					const std::uint32_t span = shape.matrix ? (rowMajor ? shape.rows : shape.columns) : 1u;
					if (component + (shape.matrix ? 0u : shape.columns) > 4 || (shape.matrix && component != 0) || (member.array && component != 0))
						return Fail(std::format("constant buffer {} member {} straddles a register", name, member.name));
					if (!member.array) {
						out += std::format("static const {} {} = {};\n", member.type, member.name, element(row));
					} else {
						out += std::format("static const {} {}[{}] = {{ ", member.type, member.name, member.count);
						for (std::uint32_t e = 0; e < member.count; ++e)
							out += (e ? ", " : "") + element(row + e * span);
						out += " };\n";
					}
					++result.members;
				}
				return true;
			}

			// The text before a replaced declaration, then its preprocessor lines, each replacement starting on a line of its own.
			void Begin(std::string_view a_before, const std::string& a_directives)
			{
				out.append(a_before);
				if (!out.empty() && out.back() != '\n')
					out += "\n";
				out += a_directives;
			}

			bool Run(std::string_view a_text)
			{
				std::size_t copied = 0;  // a_text up to here is in out
				std::size_t at = 0;
				constexpr std::string_view kRegister = "register";
				while ((at = a_text.find(kRegister, at)) != std::string_view::npos) {
					const std::size_t keyword = at;
					at += kRegister.size();
					if ((keyword > 0 && IsWord(a_text[keyword - 1])) || (at < a_text.size() && IsWord(a_text[at])))
						continue;
					std::size_t p = SkipSpace(a_text, at);
					if (p >= a_text.size() || a_text[p] != '(')
						continue;
					p = SkipSpace(a_text, p + 1);
					if (p >= a_text.size() || std::string_view("tsub").find(a_text[p]) == std::string_view::npos)
						return Fail(std::format("a register binding is not understood near: {}", a_text.substr(keyword, 40)));
					const char registerClass = a_text[p++];
					std::uint32_t number = 0;
					if (!ReadNumber(a_text, p, number))
						return Fail(std::format("a register binding is not understood near: {}", a_text.substr(keyword, 40)));
					p = SkipSpace(a_text, p);
					if (p < a_text.size() && a_text[p] == ',') {  // ", space<n>": the register's own number is what the pass gives
						p = SkipSpace(a_text, p + 1);
						if (a_text.compare(p, 5, "space") != 0)
							return Fail(std::format("a register binding is not understood near: {}", a_text.substr(keyword, 40)));
						p += 5;
						std::uint32_t space = 0;
						if (!ReadNumber(a_text, p, space))
							return Fail(std::format("a register binding is not understood near: {}", a_text.substr(keyword, 40)));
						p = SkipSpace(a_text, p);
					}
					if (p >= a_text.size() || a_text[p] != ')')
						return Fail(std::format("a register binding is not understood near: {}", a_text.substr(keyword, 40)));
					p = SkipSpace(a_text, p + 1);
					// The declaration's start: after the previous statement or block.
					std::size_t colon = keyword;
					while (colon > 0 && IsSpace(a_text[colon - 1]))
						--colon;
					if (colon == 0 || a_text[colon - 1] != ':')
						return Fail(std::format("a register binding has no declaration before it near: {}", a_text.substr(keyword, 40)));
					--colon;
					std::size_t start = colon;
					while (start > copied && a_text[start - 1] != ';' && a_text[start - 1] != '{' && a_text[start - 1] != '}')
						--start;
					std::string directives, declaration;
					SplitDirectives(a_text.substr(start, colon - start), directives, declaration);
					const std::string trimmed = Trim(declaration);
					const bool block = trimmed.compare(0, 7, "cbuffer") == 0 && trimmed.size() > 7 && IsSpace(trimmed[7]);
					if (block != (registerClass == 'b'))
						return Fail(std::format("register {}{} binds {}", registerClass, number, trimmed));
					if (block && number >= options.keepFirst && number <= options.keepLast) {
						at = p;  // a push block: as it is
						continue;
					}
					if (!block) {
						if (p >= a_text.size() || a_text[p] != ';')
							return Fail(std::format("the declaration of {}{} does not end after its register", registerClass, number));
						Begin(a_text.substr(copied, start - copied), directives);
						if (!Resource(registerClass, number, trimmed))
							return false;
						out += "\n";
						copied = at = p + 1;
						continue;
					}
					// cbuffer <name> : register(b<n>) { ... } [;]
					if (p >= a_text.size() || a_text[p] != '{')
						return Fail(std::format("constant buffer at b{} has no body", number));
					std::size_t end = p + 1;
					for (int depth = 1; end < a_text.size() && depth > 0; ++end)
						depth += a_text[end] == '{' ? 1 : a_text[end] == '}' ? -1 : 0;
					if (end > a_text.size() || a_text[end - 1] != '}')
						return Fail(std::format("constant buffer at b{} is not closed", number));
					const std::string_view body = a_text.substr(p + 1, end - 1 - (p + 1));
					std::size_t after = SkipSpace(a_text, end);
					if (after < a_text.size() && a_text[after] == ';')
						end = after + 1;
					Begin(a_text.substr(copied, start - copied), directives);
					if (!Block(Trim(std::string_view(trimmed).substr(7)), number, body))
						return false;
					copied = at = end;
				}
				out.append(a_text.substr(copied));
				return true;
			}
		};
	}

	Result Rewrite(std::string_view a_preprocessed, const Options& a_options)
	{
		Rewriter rewriter{ a_options };
		rewriter.out.reserve(a_preprocessed.size() + 4096);
		if (!rewriter.Run(a_preprocessed)) {
			Result failed;
			failed.error = std::move(rewriter.result.error);
			return failed;
		}
		rewriter.result.source = std::move(rewriter.out);
		return std::move(rewriter.result);
	}
}
