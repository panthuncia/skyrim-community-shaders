#include "FxLower.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>
#include <optional>

namespace OrgFx
{
	namespace
	{
		constexpr std::uint32_t Align16(std::uint32_t a_v) { return (a_v + 15u) & ~15u; }

		std::uint32_t ScalarBytes(Scalar a_scalar) { return a_scalar == Scalar::Double ? 8u : 4u; }

		/** @brief The type the values block is read as: "float", "int", "uint" or "double". */
		const char* LoadScalar(Scalar a_scalar)
		{
			switch (a_scalar) {
			case Scalar::Int:
			case Scalar::Min16Int:
			case Scalar::Min12Int:
				return "int";
			case Scalar::Uint:
			case Scalar::Min16Uint:
			case Scalar::Bool:
				return "uint";
			case Scalar::Double:
				return "double";
			default:
				return "float";
			}
		}

		/** @brief The scalar type as HLSL spells it ("float", "bool", "min16float"). */
		const char* ScalarName(Scalar a_scalar)
		{
			switch (a_scalar) {
			case Scalar::Bool:
				return "bool";
			case Scalar::Int:
				return "int";
			case Scalar::Uint:
				return "uint";
			case Scalar::Half:
				return "half";
			case Scalar::Double:
				return "double";
			case Scalar::Min16Float:
				return "min16float";
			case Scalar::Min10Float:
				return "min10float";
			case Scalar::Min16Int:
				return "min16int";
			case Scalar::Min12Int:
				return "min12int";
			case Scalar::Min16Uint:
				return "min16uint";
			default:
				return "float";
			}
		}

		std::string TypeText(const Type& a_type)
		{
			return a_type.templateArg.empty() ? a_type.name : std::format("{}<{}>", a_type.name, a_type.templateArg);
		}

		std::string NumericName(Scalar a_scalar, std::uint32_t a_cols)
		{
			return a_cols > 1 ? std::format("{}{}", ScalarName(a_scalar), a_cols) : std::string(ScalarName(a_scalar));
		}

		std::string Upper(std::string_view a_s)
		{
			std::string out(a_s);
			for (auto& c : out)
				c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			return out;
		}

		/** @brief "TEXCOORD" and "texcoord0" both become "TEXCOORD0"; FX matches semantics this way. */
		std::string NormalizeSemantic(std::string_view a_semantic)
		{
			auto s = Upper(a_semantic);
			if (s.empty() || !std::isdigit(static_cast<unsigned char>(s.back())))
				s += '0';
			return s;
		}

		class Lowering
		{
		public:
			Lowering(const EffectDesc& a_fx, std::vector<Diagnostic>& a_diagnostics) :
				fx(a_fx), diagnostics(a_diagnostics) {}

			void Error(std::uint32_t a_line, std::string a_message) { diagnostics.push_back({ Severity::Error, a_line, std::move(a_message) }); }
			void Warn(std::uint32_t a_line, std::string a_message) { diagnostics.push_back({ Severity::Warning, a_line, std::move(a_message) }); }

			/** @brief One element's size in a constant buffer. */
			std::optional<std::uint32_t> ElementSize(const Type& a_type, const Modifiers& a_mods) const
			{
				if (a_type.cls == TypeClass::Numeric) {
					const auto bytes = ScalarBytes(a_type.scalar);
					if (a_type.matrix) {
						const std::uint32_t registers = a_mods.rowMajor ? a_type.rows : a_type.cols;
						const std::uint32_t perRegister = a_mods.rowMajor ? a_type.cols : a_type.rows;
						return 16u * (registers - 1u) + perRegister * bytes;
					}
					return a_type.cols * bytes;
				}
				if (a_type.cls == TypeClass::Struct)
					if (const auto* s = fx.FindStruct(a_type.name); s && s->size)
						return s->size;
				return std::nullopt;
			}

			std::string Load(const char* a_type, std::uint32_t a_offset) const
			{
				return std::format("OrgFxValues.Load<{}>(OrgFxValuesOffset + {}u)", a_type, a_offset);
			}

			/** @brief An expression reading one element of a_type at a_offset of the region. Empty when the type cannot be read. */
			std::string ElementLoad(const Type& a_type, const Modifiers& a_mods, std::uint32_t a_offset, std::uint32_t a_line, int a_depth = 0)
			{
				if (a_type.cls == TypeClass::Numeric) {
					const auto bytes = ScalarBytes(a_type.scalar);
					if (a_type.matrix) {
						std::string args;
						for (std::uint32_t r = 0; r < a_type.rows; ++r)
							for (std::uint32_t c = 0; c < a_type.cols; ++c) {
								const auto at = a_offset + (a_mods.rowMajor ? r * 16u + c * bytes : c * 16u + r * bytes);
								std::string e = Load(LoadScalar(a_type.scalar), at);
								if (a_type.scalar == Scalar::Bool)
									e = "(" + e + " != 0)";
								args += (args.empty() ? "" : ", ") + e;
							}
						return std::format("{}{}x{}({})", ScalarName(a_type.scalar), a_type.rows, a_type.cols, args);
					}
					const auto loadType = NumericName(Scalar::None == a_type.scalar ? Scalar::Float : a_type.scalar, a_type.cols);
					const std::string readType = a_type.cols > 1 ? std::format("{}{}", LoadScalar(a_type.scalar), a_type.cols) : std::string(LoadScalar(a_type.scalar));
					std::string e = Load(readType.c_str(), a_offset);
					if (a_type.scalar == Scalar::Bool)
						return "(" + e + " != 0)";
					if (readType != loadType)
						return std::format("({})({})", loadType, e);
					return e;
				}
				if (a_type.cls == TypeClass::Struct && a_depth < 8) {
					const auto* s = fx.FindStruct(a_type.name);
					if (!s || (!s->members.empty() && s->size == 0)) {
						Error(a_line, std::format("struct '{}' cannot be read from a constant buffer", a_type.name));
						return {};
					}
					std::string out = "{ ";
					for (std::size_t i = 0; i < s->members.size(); ++i) {
						const auto& m = s->members[i];
						auto e = ValueLoad(m.type, {}, m.arraySize, a_offset + m.offset, a_line, a_depth + 1);
						if (e.empty())
							return {};
						out += (i ? ", " : "") + e;
					}
					return out + " }";
				}
				Error(a_line, std::format("type '{}' cannot be read from a constant buffer", a_type.name));
				return {};
			}

			/** @brief ElementLoad for a value that may be an array: an initializer list of its elements. */
			std::string ValueLoad(const Type& a_type, const Modifiers& a_mods, std::uint32_t a_arraySize, std::uint32_t a_offset, std::uint32_t a_line, int a_depth = 0)
			{
				if (a_arraySize == 0)
					return ElementLoad(a_type, a_mods, a_offset, a_line, a_depth);
				const auto size = ElementSize(a_type, a_mods);
				if (!size) {
					Error(a_line, std::format("type '{}' cannot be read from a constant buffer", a_type.name));
					return {};
				}
				const auto stride = Align16(*size);
				std::string out = "{ ";
				for (std::uint32_t i = 0; i < a_arraySize; ++i) {
					auto e = ElementLoad(a_type, a_mods, a_offset + i * stride, a_line, a_depth);
					if (e.empty())
						return {};
					out += (i ? ", " : "") + e;
				}
				return out + " }";
			}

		private:
			const EffectDesc& fx;
			std::vector<Diagnostic>& diagnostics;
		};

		/** @brief A text edit: a_range replaced by text, its newlines kept after it so line numbers stay. */
		struct Edit
		{
			Range range;
			std::string text;
		};

		std::string ApplyEdits(std::string_view a_text, std::vector<Edit> a_edits)
		{
			std::sort(a_edits.begin(), a_edits.end(), [](const Edit& a, const Edit& b) { return a.range.begin < b.range.begin; });
			std::string out;
			out.reserve(a_text.size());
			std::uint32_t at = 0;
			for (const auto& e : a_edits) {
				if (e.range.begin < at)
					continue;  // inside an edit already applied (a technique in a removed fxgroup)
				out.append(a_text.substr(at, e.range.begin - at));
				out += e.text;
				const auto replaced = a_text.substr(e.range.begin, e.range.end - e.range.begin);
				out.append(static_cast<std::size_t>(std::count(replaced.begin(), replaced.end(), '\n')), '\n');
				at = e.range.end;
			}
			out.append(a_text.substr(at));
			return out;
		}

		/** @brief An input or output of an entry: where it is in the entry's arguments, its type and semantic. */
		struct Signal
		{
			std::string path;  // "OrgFxArg0.txcoord" or "OrgFxArg1"
			Type type;
			std::uint32_t arraySize = 0;
			std::string semantic;  // normalized
			std::string interpolation;
		};

		/** @brief The signals of a parameter or return value: a struct's members, or the value itself. */
		bool CollectSignals(const EffectDesc& a_fx, const std::string& a_path, const Type& a_type, std::uint32_t a_arraySize, const std::string& a_semantic,
			const std::string& a_interpolation, std::vector<Signal>& a_out, std::string& a_error)
		{
			if (a_type.cls == TypeClass::Struct) {
				const auto* s = a_fx.FindStruct(a_type.name);
				if (!s) {
					a_error = std::format("struct '{}' is not declared", a_type.name);
					return false;
				}
				for (const auto& m : s->members) {
					if (m.semantic.empty()) {
						a_error = std::format("member '{}' of '{}' has no semantic", m.name, s->name);
						return false;
					}
					if (m.type.cls == TypeClass::Struct) {
						a_error = std::format("member '{}' of '{}' is a struct, which the wrappers do not flatten", m.name, s->name);
						return false;
					}
					a_out.push_back({ a_path + "." + m.name, m.type, m.arraySize, NormalizeSemantic(m.semantic), m.interpolation });
				}
				return true;
			}
			if (a_semantic.empty()) {
				a_error = std::format("'{}' has no semantic", a_path);
				return false;
			}
			a_out.push_back({ a_path, a_type, a_arraySize, NormalizeSemantic(a_semantic), a_interpolation });
			return true;
		}

		/** @brief a_expr (of a_from) as a_to: a cast, a swizzle to fewer components, or zeros added for more. */
		std::string Convert(const std::string& a_expr, const Type& a_from, const Type& a_to)
		{
			const auto to = TypeText(a_to);
			if (a_from.matrix || a_to.matrix || a_from.cols == a_to.cols)
				return std::format("({})({})", to, a_expr);
			if (a_to.cols < a_from.cols)
				return std::format("({})(({}).{})", to, a_expr, std::string_view("xyzw").substr(0, a_to.cols));
			std::string zeros;
			for (std::uint32_t i = a_from.cols; i < a_to.cols; ++i)
				zeros += ", 0";
			return std::format("{}({}{})", to, a_expr, zeros);
		}
	}

	LoweredEffect LowerEffect(std::string_view a_text, const EffectDesc& a_fx)
	{
		LoweredEffect out;
		Lowering lowering(a_fx, out.diagnostics);

		out.regions.push_back({ {}, 0, a_fx.globalsSize });
		std::uint32_t valuesEnd = Align16(a_fx.globalsSize);
		for (const auto& cb : a_fx.constantBuffers) {
			out.regions.push_back({ cb.name, valuesEnd, cb.size });
			valuesEnd += Align16(cb.size);
		}
		out.valuesSize = valuesEnd;
		auto regionOffset = [&](const std::string& a_cbuffer) -> std::uint32_t {
			for (const auto& r : out.regions)
				if (r.cbuffer == a_cbuffer)
					return r.offset;
			return 0;
		};

		// Replacement text per statement, keyed by the statement's start: several declarators share one.
		std::map<std::uint32_t, Edit> statements;
		auto addToStatement = [&](const Range& a_range, const std::string& a_text) {
			auto& e = statements[a_range.begin];
			e.range = a_range;
			if (!a_text.empty())
				e.text += (e.text.empty() ? "" : " ") + a_text;
		};
		std::vector<Edit> edits;

		auto slot = [&](ResourceSlot a_slot) {
			out.resources.push_back(std::move(a_slot));
			return static_cast<std::uint32_t>(out.resources.size() - 1);
		};

		std::map<std::string, std::string> cbufferText;
		for (std::size_t i = 0; i < a_fx.variables.size(); ++i) {
			const auto& v = a_fx.variables[i];
			std::string replacement;
			switch (v.type.cls) {
			case TypeClass::Numeric:
			case TypeClass::Struct:
				if (v.modifiers.isStatic) {
					if (v.annotation.end > v.annotation.begin)
						edits.push_back({ v.annotation, {} });
					continue;
				}
				if (v.bufferOffset < 0) {
					lowering.Error(v.line, std::format("'{}' has no constant buffer layout", v.name));
					continue;
				} else {
					const auto load = lowering.ValueLoad(v.type, v.modifiers, v.arraySize, regionOffset(v.cbuffer) + static_cast<std::uint32_t>(v.bufferOffset), v.line);
					if (load.empty())
						continue;
					const auto type = v.type.matrix ? std::format("{}{}x{}", ScalarName(v.type.scalar), v.type.rows, v.type.cols) : TypeText(v.type);
					replacement = std::format("static {} {}{} = {};", type, v.name, v.arraySize ? std::format("[{}]", v.arraySize) : std::string{}, load);
				}
				break;
			case TypeClass::Texture:
				if (v.arraySize) {
					lowering.Error(v.line, std::format("texture array '{}' is not supported", v.name));
					continue;
				}
				replacement = std::format("static {} {} = ResourceDescriptorHeap[OrgFxHandle({})];", TypeText(v.type), v.name,
					slot({ ResourceSlot::Kind::Texture, v.name, static_cast<std::int32_t>(i), -1 }));
				break;
			case TypeClass::Sampler:
				if (v.type.name != "SamplerState" && v.type.name != "SamplerComparisonState") {
					lowering.Error(v.line, std::format("'{}' is a DX9 sampler ('{}')", v.name, v.type.name));
					continue;
				}
				replacement = std::format("static {} {} = SamplerDescriptorHeap[OrgFxHandle({})];", v.type.name, v.name,
					slot({ ResourceSlot::Kind::Sampler, v.name, static_cast<std::int32_t>(i), -1 }));
				break;
			case TypeClass::RWResource:
				lowering.Error(v.line, std::format("UAV '{}' is not supported", v.name));
				continue;
			case TypeClass::String:
			case TypeClass::Shader:
			case TypeClass::StateObject:
				break;  // removed
			default:
				if (!v.modifiers.isStatic)
					lowering.Error(v.line, std::format("global '{}' of type '{}' cannot be lowered", v.name, v.type.name));
				continue;
			}
			if (!v.cbuffer.empty())
				cbufferText[v.cbuffer] += (cbufferText[v.cbuffer].empty() ? "" : " ") + replacement;
			else
				addToStatement(v.statement, replacement);
		}

		for (std::size_t i = 0; i < a_fx.states.size(); ++i) {
			const auto& s = a_fx.states[i];
			std::string replacement;
			if (s.kind == StateKind::Sampler || s.kind == StateKind::SamplerComparison) {
				if (s.legacySamplerState)
					lowering.Error(s.line, std::format("'{}' is a DX9 sampler_state", s.name));
				else if (s.arraySize)
					lowering.Error(s.line, std::format("sampler array '{}' is not supported", s.name));
				else
					replacement = std::format("static {} {} = SamplerDescriptorHeap[OrgFxHandle({})];", s.kind == StateKind::Sampler ? "SamplerState" : "SamplerComparisonState",
						s.name, slot({ ResourceSlot::Kind::Sampler, s.name, -1, static_cast<std::int32_t>(i) }));
			}
			addToStatement(s.statement, replacement);
		}

		for (const auto& cb : a_fx.constantBuffers)
			edits.push_back({ cb.statement, cbufferText[cb.name] });
		for (const auto& t : a_fx.techniques)
			if (t.group.empty())
				edits.push_back({ t.statement, {} });
		for (const auto& g : a_fx.groups)
			edits.push_back({ g.statement, {} });
		for (auto& [begin, e] : statements)
			edits.push_back(std::move(e));

		if (out.resources.size() > kMaxResources)
			lowering.Error(0, std::format("{} textures and samplers, more than the {} the push block holds", out.resources.size(), kMaxResources));

		// The push block as rows: word 0 the values block, word 1 the region's offset, then the handles. A cbuffer array of uint
		// would take a row per element.
		std::string body;
		body += "// Generated by CS's ORG FX runtime (src/Features/Effects11/ORG/FxLower.cpp).\n";
		body += std::format("cbuffer OrgFxBindings : register(b0)\n{{\n\tuint4 OrgFxPush[{}];\n}};\n", out.PushWords() / 4u);
		body += "static ByteAddressBuffer OrgFxValues = ResourceDescriptorHeap[OrgFxPush[0].x];\n";
		body += "static uint OrgFxValuesOffset = OrgFxPush[0].y;\n";
		body += std::format("uint OrgFxHandle(uint a_slot) {{ return OrgFxPush[(a_slot + {0}u) / 4u][(a_slot + {0}u) % 4u]; }}\n", kPushHeaderWords);
		body += "#line 1\n";
		body += ApplyEdits(a_text, std::move(edits));
		out.body = std::move(body);
		out.ok = std::none_of(out.diagnostics.begin(), out.diagnostics.end(), [](const Diagnostic& d) { return d.severity == Severity::Error; });
		return out;
	}

	LoweredPass LowerPass(const LoweredEffect& a_effect, const EffectDesc& a_fx, std::uint32_t a_technique, std::uint32_t a_pass)
	{
		LoweredPass out;
		auto fail = [&](std::uint32_t a_line, std::string a_message) {
			out.diagnostics.push_back({ Severity::Error, a_line, std::move(a_message) });
			return out;
		};
		if (!a_effect.ok)
			return fail(0, "the effect could not be lowered");
		if (a_technique >= a_fx.techniques.size() || a_pass >= a_fx.techniques[a_technique].passes.size())
			return fail(0, "no such pass");
		const auto& technique = a_fx.techniques[a_technique];
		const auto& pass = technique.passes[a_pass];
		for (std::size_t s = 0; s < pass.shaders.size(); ++s) {
			const auto stage = static_cast<Stage>(s);
			const auto& b = pass.shaders[s];
			if (stage != Stage::Vertex && stage != Stage::Pixel && b.set && !b.null)
				return fail(b.line, std::format("pass '{}' of '{}' sets a {} shader", pass.name, technique.name, StageName(stage)));
		}
		const auto& vs = pass.shaders[static_cast<std::size_t>(Stage::Vertex)];
		const auto& ps = pass.shaders[static_cast<std::size_t>(Stage::Pixel)];
		if (!vs.set || vs.null || vs.function < 0 || !ps.set || ps.null || ps.function < 0)
			return fail(pass.line, std::format("pass '{}' of '{}' needs a resolved vertex and pixel entry", pass.name, technique.name));
		const auto& vsFn = a_fx.functions[vs.function];
		const auto& psFn = a_fx.functions[ps.function];

		// Arguments of an entry call: uniform parameters take the pass's arguments, the rest are the wrapper's locals.
		auto callArguments = [&](const Function& a_fn, const ShaderBinding& a_b) {
			std::string args;
			std::size_t uniform = 0;
			for (std::size_t i = 0; i < a_fn.parameters.size(); ++i) {
				const auto& p = a_fn.parameters[i];
				const std::string arg = p.uniform ? (uniform < a_b.arguments.size() ? a_b.arguments[uniform++].text : std::string("0")) : std::format("OrgFxArg{}", i);
				args += (args.empty() ? "" : ", ") + arg;
			}
			return args;
		};

		std::string error;
		std::string wrappers = "\n// Entry wrappers\n";

		// Vertex wrapper
		std::vector<Signal> vsInputs, vsOutputs;
		std::string vsLocals, vsAssign;
		for (std::size_t i = 0; i < vsFn.parameters.size(); ++i) {
			const auto& p = vsFn.parameters[i];
			if (p.uniform)
				continue;
			const auto name = std::format("OrgFxArg{}", i);
			vsLocals += std::format("\t{} {}{} = ({})0;\n", TypeText(p.type), name, p.arraySize ? std::format("[{}]", p.arraySize) : std::string{}, TypeText(p.type));
			if (p.in && !CollectSignals(a_fx, name, p.type, p.arraySize, p.semantic, p.interpolation, vsInputs, error))
				return fail(vsFn.line, std::format("vertex entry '{}': {}", vsFn.name, error));
			if (p.out && !CollectSignals(a_fx, name, p.type, p.arraySize, p.semantic, p.interpolation, vsOutputs, error))
				return fail(vsFn.line, std::format("vertex entry '{}': {}", vsFn.name, error));
		}
		for (const auto& in : vsInputs) {
			if (in.arraySize)
				return fail(vsFn.line, std::format("vertex input '{}' is an array", in.path));
			std::string value;
			Type from;
			from.cls = TypeClass::Numeric;
			from.scalar = Scalar::Float;
			from.cols = 4;
			from.name = "float4";
			// The quad's position, whichever name the input uses (Effects11's input layout has only POSITION and TEXCOORD0).
			if (in.semantic == "POSITION0" || in.semantic == "SV_POSITION0")
				value = "OrgFxPosition";
			else if (in.semantic == "TEXCOORD0")
				value = "OrgFxTexcoord";
			else if (in.semantic == "SV_VERTEXID0") {
				value = "OrgFxVertexId";
				from.scalar = Scalar::Uint;
				from.cols = 1;
				from.name = "uint";
			} else {
				out.diagnostics.push_back({ Severity::Warning, vsFn.line, std::format("vertex input '{}' ({}) is not provided by the quad; it reads zero", in.path, in.semantic) });
				continue;
			}
			vsAssign += std::format("\t{} = {};\n", in.path, Convert(value, from, in.type));
		}
		const bool vsReturns = vsFn.returnType.cls != TypeClass::Void;
		if (vsReturns && !CollectSignals(a_fx, "OrgFxResult", vsFn.returnType, 0, vsFn.returnSemantic, {}, vsOutputs, error))
			return fail(vsFn.line, std::format("vertex entry '{}': {}", vsFn.name, error));

		// A vertex output: its semantic's name and first index, how many indices (and locations) it covers, its location.
		struct Varying
		{
			std::string base;
			std::uint32_t index = 0;
			std::uint32_t count = 1;
			Type type;
			std::uint32_t location = 0;
			std::string interpolation;
		};
		auto split = [](const std::string& a_semantic) {
			auto end = a_semantic.size();
			while (end > 0 && std::isdigit(static_cast<unsigned char>(a_semantic[end - 1])))
				--end;
			const auto digits = a_semantic.substr(end);
			return std::pair{ a_semantic.substr(0, end), digits.empty() ? 0u : static_cast<std::uint32_t>(std::stoul(digits)) };
		};
		std::vector<Varying> varyings;
		std::uint32_t nextLocation = 0;
		std::string vsParams = "uint OrgFxVertexId : SV_VertexID";
		std::string vsCopy;
		for (std::size_t i = 0; i < vsOutputs.size(); ++i) {
			const auto& o = vsOutputs[i];
			const auto arraySuffix = o.arraySize ? std::format("[{}]", o.arraySize) : std::string{};
			if ((o.semantic == "SV_POSITION0" || o.semantic == "POSITION0") && !o.arraySize) {
				vsParams += std::format(", out float4 OrgFxOut{} : SV_Position", i);
				vsCopy += std::format("\tOrgFxOut{} = {};\n", i, o.path);
				continue;
			}
			if (o.semantic.starts_with("SV_"))
				return fail(vsFn.line, std::format("vertex output '{}' ({}) is not supported", o.path, o.semantic));
			if (o.type.matrix)
				return fail(vsFn.line, std::format("vertex output '{}' is a matrix", o.path));
			const auto [base, index] = split(o.semantic);
			const std::uint32_t count = std::max(o.arraySize, 1u);
			varyings.push_back({ base, index, count, o.type, nextLocation, o.interpolation });
			vsParams += std::format(", [[vk::location({})]] out {}{}{} OrgFxOut{}{} : {}", nextLocation, o.interpolation, o.interpolation.empty() ? "" : " ", TypeText(o.type), i, arraySuffix,
				o.semantic);
			vsCopy += std::format("\tOrgFxOut{} = {};\n", i, o.path);
			nextLocation += count;
		}
		wrappers += std::format("void {}({})\n{{\n", kVertexEntry, vsParams);
		wrappers += "\tstatic const float2 OrgFxCorners[6] = { float2(-1, -1), float2(-1, 1), float2(1, -1), float2(1, -1), float2(-1, 1), float2(1, 1) };\n";
		wrappers += "\tconst float2 OrgFxCorner = OrgFxCorners[OrgFxVertexId];\n";
		wrappers += "\tconst float4 OrgFxPosition = float4(OrgFxCorner, 0.0, 1.0);\n";
		wrappers += "\tconst float4 OrgFxTexcoord = float4(OrgFxCorner.x * 0.5 + 0.5, 0.5 - OrgFxCorner.y * 0.5, 0.0, 0.0);\n";
		wrappers += vsLocals + vsAssign;
		const auto vsCall = std::format("{}({})", vsFn.name, callArguments(vsFn, vs));
		wrappers += vsReturns ? std::format("\t{} OrgFxResult = {};\n", TypeText(vsFn.returnType), vsCall) : std::format("\t{};\n", vsCall);
		wrappers += vsCopy + "}\n\n";

		// Pixel wrapper
		std::vector<Signal> psInputs;
		std::string psLocals, psAssign, psParams = "float4 OrgFxFragCoord : SV_Position";
		for (std::size_t i = 0; i < psFn.parameters.size(); ++i) {
			const auto& p = psFn.parameters[i];
			if (p.uniform)
				continue;
			const auto name = std::format("OrgFxArg{}", i);
			if (p.out && !p.in) {
				// An output (SV_Target): the wrapper's own out parameter, passed straight through.
				auto semantic = p.semantic;
				if (NormalizeSemantic(semantic).starts_with("COLOR"))
					semantic = "SV_Target" + NormalizeSemantic(semantic).substr(5);
				psParams += std::format(", out {} {}{}", TypeText(p.type), name, semantic.empty() ? std::string{} : " : " + semantic);
				continue;
			}
			if (p.out)
				return fail(psFn.line, std::format("pixel entry '{}': inout parameter '{}' is not supported", psFn.name, p.name));
			psLocals += std::format("\t{} {}{} = ({})0;\n", TypeText(p.type), name, p.arraySize ? std::format("[{}]", p.arraySize) : std::string{}, TypeText(p.type));
			if (!CollectSignals(a_fx, name, p.type, p.arraySize, p.semantic, p.interpolation, psInputs, error))
				return fail(psFn.line, std::format("pixel entry '{}': {}", psFn.name, error));
		}
		for (std::size_t i = 0; i < psInputs.size(); ++i) {
			const auto& in = psInputs[i];
			if ((in.semantic == "SV_POSITION0" || in.semantic == "POSITION0" || in.semantic == "VPOS0") && !in.arraySize) {
				Type from;
				from.cls = TypeClass::Numeric;
				from.scalar = Scalar::Float;
				from.cols = 4;
				from.name = "float4";
				psAssign += std::format("\t{} = {};\n", in.path, Convert("OrgFxFragCoord", from, in.type));
				continue;
			}
			const auto arraySuffix = in.arraySize ? std::format("[{}]", in.arraySize) : std::string{};
			if (in.semantic.starts_with("SV_")) {
				psParams += std::format(", {} OrgFxIn{}{} : {}", TypeText(in.type), i, arraySuffix, in.semantic);
				psAssign += std::format("\t{} = OrgFxIn{};\n", in.path, i);
				continue;
			}
			// The vertex output whose semantic indices cover this input's: its location, offset by the index.
			const auto [base, index] = split(in.semantic);
			const std::uint32_t count = std::max(in.arraySize, 1u);
			const auto it = std::find_if(varyings.begin(), varyings.end(), [&](const Varying& v) {
				return v.base == base && index >= v.index && index + count <= v.index + v.count;
			});
			if (it == varyings.end()) {
				out.diagnostics.push_back({ Severity::Warning, psFn.line, std::format("pixel input '{}' ({}) is not written by '{}'; it reads zero", in.path, in.semantic, vsFn.name) });
				continue;
			}
			const auto location = it->location + (index - it->index);
			if (in.arraySize) {
				// An array takes the vertex output's element type; the elements must match it.
				if (TypeText(in.type) != TypeText(it->type)) {
					out.diagnostics.push_back({ Severity::Warning, psFn.line, std::format("pixel input array '{}' does not match '{}''s output type; it reads zero", in.path, vsFn.name) });
					continue;
				}
				psParams += std::format(", [[vk::location({})]] {}{}{} OrgFxIn{}{} : {}", location, it->interpolation, it->interpolation.empty() ? "" : " ", TypeText(in.type), i, arraySuffix,
					in.semantic);
				psAssign += std::format("\t{} = OrgFxIn{};\n", in.path, i);
				continue;
			}
			psParams += std::format(", [[vk::location({})]] {}{}{} OrgFxIn{} : {}", location, it->interpolation, it->interpolation.empty() ? "" : " ", TypeText(it->type), i, in.semantic);
			psAssign += std::format("\t{} = {};\n", in.path, Convert(std::format("OrgFxIn{}", i), it->type, in.type));
		}
		const auto psCall = std::format("{}({})", psFn.name, callArguments(psFn, ps));
		std::string returnType = "void", returnSemantic;
		if (psFn.returnType.cls != TypeClass::Void) {
			returnType = TypeText(psFn.returnType);
			if (psFn.returnType.cls != TypeClass::Struct) {
				auto semantic = NormalizeSemantic(psFn.returnSemantic);
				if (psFn.returnSemantic.empty())
					return fail(psFn.line, std::format("pixel entry '{}' returns a value without a semantic", psFn.name));
				returnSemantic = " : " + (semantic.starts_with("COLOR") ? "SV_Target" + semantic.substr(5) : psFn.returnSemantic);
			}
		}
		wrappers += std::format("{} {}({}){}\n{{\n", returnType, kPixelEntry, psParams, returnSemantic);
		wrappers += psLocals + psAssign;
		wrappers += psFn.returnType.cls != TypeClass::Void ? std::format("\treturn {};\n", psCall) : std::format("\t{};\n", psCall);
		wrappers += "}\n";

		out.source = a_effect.body + wrappers;
		out.ok = true;
		return out;
	}
}
