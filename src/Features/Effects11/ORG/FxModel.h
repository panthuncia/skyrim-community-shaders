#pragma once

// The ORG FX runtime's data model: what an .fx effect declares, as the parser (FxParser.h) reads it from preprocessed
// source. std only: no D3D11, no ORG. Design: docs/development/org-fx-runtime.md.

#include <array>
#include <cfloat>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace OrgFx
{
	/** @brief A byte range of the parsed text, [begin, end). */
	struct Range
	{
		std::uint32_t begin = 0;
		std::uint32_t end = 0;
	};

	enum class Severity : std::uint8_t
	{
		Info,
		Warning,  // parsed, but the ORG runtime cannot run it as FX11 would
		Error,    // not parsed
	};

	struct Diagnostic
	{
		Severity severity = Severity::Error;
		std::uint32_t line = 0;  // 1-based line of the parsed text
		std::string message;
	};

	/**
	 * @brief One annotation (`<string UIName = "Bloom"; float UIMin = 0;>`).
	 * text holds what FX11's reflection would return as a string: a string's value (adjacent literals joined), an int as
	 * "%d", a float as std::to_string, a bool as "1" or "0". It is the raw expression text when the value cannot be evaluated.
	 */
	struct Annotation
	{
		std::string type;
		std::string name;
		std::string text;
		std::vector<double> numbers;  // the evaluated components; empty for strings and unevaluated values
	};
	using Annotations = std::vector<Annotation>;

	enum class Scalar : std::uint8_t
	{
		None,
		Bool,
		Int,
		Uint,
		Float,
		Half,
		Double,
		Min16Float,
		Min10Float,
		Min16Int,
		Min12Int,
		Min16Uint,
	};

	enum class TypeClass : std::uint8_t
	{
		Unknown,
		Void,
		Numeric,  // scalar, vector or matrix
		Struct,   // a struct or class declared in the effect
		String,
		Texture,      // Texture1D/2D/3D/Cube(+Array, +MS), Buffer, StructuredBuffer, ByteAddressBuffer
		RWResource,   // RWTexture*, RWBuffer, RWStructuredBuffer, RWByteAddressBuffer, Append/Consume
		Sampler,      // SamplerState, SamplerComparisonState, sampler, sampler2D, ...
		Shader,       // VertexShader, PixelShader, ... objects
		StateObject,  // BlendState, DepthStencilState, RasterizerState
	};

	struct Type
	{
		TypeClass cls = TypeClass::Unknown;
		Scalar scalar = Scalar::None;
		std::uint8_t rows = 1;  // matrices: rows; vectors and scalars: 1
		std::uint8_t cols = 1;  // vectors: components; scalars: 1
		bool matrix = false;
		std::string name;         // the base type as written: "float4", "Texture2D", "VS_INPUT_POST"
		std::string templateArg;  // "float4" in Texture2D<float4>; empty when none
	};

	/** @brief Storage and type modifiers on a declaration. */
	struct Modifiers
	{
		bool isStatic = false;
		bool isConst = false;
		bool isUniform = false;
		bool isExtern = false;
		bool isShared = false;
		bool isGroupShared = false;
		bool isVolatile = false;
		bool rowMajor = false;
		bool columnMajor = false;
	};

	/** @brief A global variable (numeric, struct, string, resource, sampler declared without a state block, shader object). */
	struct Variable
	{
		std::string name;
		Type type;
		Modifiers modifiers;
		std::uint32_t arraySize = 0;  // 0: not an array
		std::string semantic;
		std::string registerBinding;  // "t3" from `: register(t3)`; packoffset is kept here too
		Annotations annotations;
		std::string initializerText;
		std::vector<double> initializer;  // flattened components, when initializerEvaluated
		bool initializerEvaluated = false;
		std::string cbuffer;  // the enclosing cbuffer/tbuffer; empty for $Globals
		/** @brief Byte offset in its constant buffer (FXC packing); -1 when it is not in one (static, resource, string...). */
		std::int32_t bufferOffset = -1;
		std::uint32_t bufferSize = 0;
		Range statement;   // the whole declaration statement, shared by `float a, b;`
		Range annotation;  // the `<...>` list; empty when there is none
		std::uint32_t line = 0;
	};

	enum class StateKind : std::uint8_t
	{
		Sampler,
		SamplerComparison,
		Blend,
		DepthStencil,
		Rasterizer,
	};

	struct StateAssignment
	{
		std::string key;
		std::int32_t index = -1;  // `BlendEnable[1]`
		std::string value;        // as written
		std::uint32_t line = 0;
	};

	enum class FilterMode : std::uint8_t
	{
		Point,
		Linear,
	};

	enum class FilterReduction : std::uint8_t
	{
		Standard,
		Comparison,
		Minimum,
		Maximum,
	};

	enum class AddressMode : std::uint8_t
	{
		Wrap,
		Mirror,
		Clamp,
		Border,
		MirrorOnce,
	};

	enum class CompareFunc : std::uint8_t
	{
		Never,
		Less,
		Equal,
		LessEqual,
		Greater,
		NotEqual,
		GreaterEqual,
		Always,
	};

	/** @brief A sampler state block, with D3D11's defaults for the keys it does not set. */
	struct SamplerDesc
	{
		FilterMode minFilter = FilterMode::Linear;
		FilterMode magFilter = FilterMode::Linear;
		FilterMode mipFilter = FilterMode::Linear;
		bool anisotropic = false;
		FilterReduction reduction = FilterReduction::Standard;
		AddressMode addressU = AddressMode::Clamp;
		AddressMode addressV = AddressMode::Clamp;
		AddressMode addressW = AddressMode::Clamp;
		float mipLodBias = 0.0f;
		std::uint32_t maxAnisotropy = 16;
		CompareFunc compare = CompareFunc::Never;
		std::array<float, 4> borderColor{ 1.0f, 1.0f, 1.0f, 1.0f };
		float minLod = -FLT_MAX;
		float maxLod = FLT_MAX;
	};

	enum class BlendFactor : std::uint8_t
	{
		Zero,
		One,
		SrcColor,
		InvSrcColor,
		SrcAlpha,
		InvSrcAlpha,
		DestAlpha,
		InvDestAlpha,
		DestColor,
		InvDestColor,
		SrcAlphaSat,
		Factor,
		InvFactor,
		Src1Color,
		InvSrc1Color,
		Src1Alpha,
		InvSrc1Alpha,
	};

	enum class BlendOp : std::uint8_t
	{
		Add,
		Subtract,
		RevSubtract,
		Min,
		Max,
	};

	struct BlendTarget
	{
		bool enable = false;
		BlendFactor src = BlendFactor::One;
		BlendFactor dest = BlendFactor::Zero;
		BlendOp op = BlendOp::Add;
		BlendFactor srcAlpha = BlendFactor::One;
		BlendFactor destAlpha = BlendFactor::Zero;
		BlendOp opAlpha = BlendOp::Add;
		std::uint8_t writeMask = 0xF;
	};

	struct BlendDesc
	{
		bool alphaToCoverage = false;
		bool independentBlend = false;
		std::array<BlendTarget, 8> targets{};
	};

	enum class StencilOp : std::uint8_t
	{
		Keep,
		Zero,
		Replace,
		IncrSat,
		DecrSat,
		Invert,
		Incr,
		Decr,
	};

	struct StencilFace
	{
		StencilOp fail = StencilOp::Keep;
		StencilOp depthFail = StencilOp::Keep;
		StencilOp pass = StencilOp::Keep;
		CompareFunc func = CompareFunc::Always;
	};

	struct DepthStencilDesc
	{
		bool depthEnable = true;
		bool depthWrite = true;
		CompareFunc depthFunc = CompareFunc::Less;
		bool stencilEnable = false;
		std::uint8_t stencilReadMask = 0xFF;
		std::uint8_t stencilWriteMask = 0xFF;
		StencilFace front{};
		StencilFace back{};
	};

	enum class FillMode : std::uint8_t
	{
		Solid,
		Wireframe,
	};

	enum class CullMode : std::uint8_t
	{
		None,
		Front,
		Back,
	};

	struct RasterizerDesc
	{
		FillMode fill = FillMode::Solid;
		CullMode cull = CullMode::Back;
		bool frontCounterClockwise = false;
		std::int32_t depthBias = 0;
		float depthBiasClamp = 0.0f;
		float slopeScaledDepthBias = 0.0f;
		bool depthClip = true;
		bool scissor = false;
		bool multisample = false;
		bool antialiasedLine = false;
	};

	/** @brief A state object block (`SamplerState Name { Filter = ...; };`). The desc matching kind is filled. */
	struct StateObject
	{
		StateKind kind = StateKind::Sampler;
		std::string name;
		std::uint32_t arraySize = 0;
		Annotations annotations;
		std::vector<StateAssignment> assignments;
		SamplerDesc sampler;
		BlendDesc blend;
		DepthStencilDesc depthStencil;
		RasterizerDesc rasterizer;
		bool legacySamplerState = false;  // a DX9 `sampler_state { Texture = <t>; }` initializer
		std::string legacyTexture;        // its Texture key
		Range statement;
		std::uint32_t line = 0;
	};

	enum class Stage : std::uint8_t
	{
		Vertex,
		Pixel,
		Geometry,
		Hull,
		Domain,
		Compute,
		Count,
	};

	/** @brief One argument of a pass's entry call (`PS_Resize(TextureDownsampled, 1024.0, 512.0)`). */
	struct ShaderArgument
	{
		enum class Kind : std::uint8_t
		{
			Literal,     // a constant expression; value holds it
			Resource,    // a global texture or sampler, by name
			Variable,    // a global numeric variable, by name
			Expression,  // anything else (kept as text)
		};
		Kind kind = Kind::Expression;
		std::string text;
		std::vector<double> value;
	};

	/** @brief A pass's shader for one stage. */
	struct ShaderBinding
	{
		bool set = false;     // the pass sets this stage
		bool null = false;    // set to NULL
		std::string profile;  // "ps_5_0"
		std::string entry;    // the function's name
		std::vector<ShaderArgument> arguments;
		std::string variable;                          // when set through a global shader variable: its name
		std::int32_t function = -1;                    // resolved overload (EffectDesc::functions), -1 when unresolved
		std::vector<std::int32_t> argumentParameters;  // the parameter each argument binds to
		std::uint32_t line = 0;
	};

	struct Pass
	{
		std::string name;
		Annotations annotations;
		std::array<ShaderBinding, static_cast<size_t>(Stage::Count)> shaders{};
		std::string blendState;  // empty: not set (D3D11 default, opaque)
		std::array<float, 4> blendFactor{ 0.0f, 0.0f, 0.0f, 0.0f };
		std::uint32_t sampleMask = 0xFFFFFFFF;
		std::string depthStencilState;
		std::uint32_t stencilRef = 0;
		std::string rasterizerState;
		std::vector<StateAssignment> otherStates;  // FX9/10 render-state assignments, kept raw
		std::uint32_t line = 0;
	};

	struct Technique
	{
		std::string name;
		std::uint32_t version = 11;  // 9 for `technique`, 10, 11
		std::string group;           // the fxgroup, empty when none
		Annotations annotations;
		std::vector<Pass> passes;
		Range statement;
		std::uint32_t line = 0;
	};

	struct Group
	{
		std::string name;
		Annotations annotations;
		std::vector<std::uint32_t> techniques;  // indices into EffectDesc::techniques
		Range statement;
		std::uint32_t line = 0;
	};

	struct Parameter
	{
		std::string name;
		Type type;
		std::uint32_t arraySize = 0;
		bool in = true;
		bool out = false;
		bool uniform = false;
		std::string interpolation;  // linear, nointerpolation, centroid, noperspective, sample
		std::string semantic;
		std::string defaultText;
	};

	struct Function
	{
		std::string name;
		Type returnType;
		std::string returnSemantic;
		std::vector<Parameter> parameters;
		Range statement;  // attributes, signature and body
		Range body;       // the braces included
		std::uint32_t line = 0;
	};

	struct StructMember
	{
		std::string name;
		Type type;
		std::uint32_t arraySize = 0;
		std::string semantic;
		std::string interpolation;
		std::uint32_t offset = 0;  // in a constant buffer (FXC packing), from the struct's start
	};

	/** @brief A struct or class. Member functions are skipped; only data members are listed. */
	struct StructDesc
	{
		std::string name;
		bool isClass = false;
		std::vector<StructMember> members;
		std::uint32_t size = 0;  // in a constant buffer: up to the end of the last member (FXC packing)
		Range statement;
		std::uint32_t line = 0;
	};

	struct ConstantBufferDesc
	{
		std::string name;
		bool textureBuffer = false;
		std::string registerBinding;
		std::vector<std::uint32_t> variables;  // indices into EffectDesc::variables
		std::uint32_t size = 0;                // FXC packing, in bytes
		Range statement;
		std::uint32_t line = 0;
	};

	/**
	 * @brief Techniques that run as one sequence, by Effects11's rule (Effect::LoadTechniques): a technique whose name is a
	 * base name plus digits joins the base's chain when the base came first; a named fxgroup is one chain.
	 */
	struct TechniqueChain
	{
		std::string key;
		std::vector<std::uint32_t> techniques;
	};

	/** @brief Everything one .fx file declares. */
	struct EffectDesc
	{
		std::vector<Variable> variables;  // declaration order
		std::vector<StateObject> states;
		std::vector<Technique> techniques;
		std::vector<Group> groups;
		std::vector<Function> functions;
		std::vector<StructDesc> structs;
		std::vector<ConstantBufferDesc> constantBuffers;
		std::vector<TechniqueChain> chains;
		std::uint32_t globalsSize = 0;  // $Globals in bytes (FXC packing)
		std::vector<Diagnostic> diagnostics;

		const Variable* FindVariable(std::string_view a_name) const;
		const StateObject* FindState(std::string_view a_name) const;
		const Technique* FindTechnique(std::string_view a_name) const;
		const StructDesc* FindStruct(std::string_view a_name) const;
		const TechniqueChain* FindChain(std::string_view a_key) const;
		std::size_t Count(Severity a_severity) const;
	};

	/** @brief The annotation by name, compared without case as Effect::GetUIAnnotation's fallback does; null when missing. */
	const Annotation* FindAnnotation(const Annotations& a_annotations, std::string_view a_name);

	/** @brief The annotation's text, or empty when it is missing. */
	std::string AnnotationText(const Annotations& a_annotations, std::string_view a_name);

	/** @brief The stage's name for messages ("vertex"). */
	const char* StageName(Stage a_stage);
}
