#include "VolumetricLighting.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/Publication.h"

#include "Features/VolumetricLighting/ORGVolumetricLighting.h"
#include "I18n/I18n.h"
#include "InteriorSun.h"
#include "RenderGraph/Frame/Declare.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/Game.h"

#define I18N_KEY_PREFIX "feature.volumetric_lighting."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	VolumetricLighting::TextureSize,
	Width,
	Height,
	Depth);

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	VolumetricLighting::Settings,
	ExteriorEnabled,
	ExteriorQuality,
	ExteriorCustomSize,
	InteriorEnabled,
	InteriorQuality,
	InteriorCustomSize);

VolumetricLighting::VolumetricLighting()
{
	settings.ExteriorEnabled.Bind(CS::SettingRegistry::Declare("VolumetricLighting.ExteriorEnabled"));
	settings.ExteriorQuality.Bind(CS::SettingRegistry::Declare("VolumetricLighting.ExteriorQuality"));
	settings.ExteriorCustomSize.Bind(CS::SettingRegistry::Declare("VolumetricLighting.ExteriorCustomSize"));
	settings.InteriorEnabled.Bind(CS::SettingRegistry::Declare("VolumetricLighting.InteriorEnabled"));
	settings.InteriorQuality.Bind(CS::SettingRegistry::Declare("VolumetricLighting.InteriorQuality"));
	settings.InteriorCustomSize.Bind(CS::SettingRegistry::Declare("VolumetricLighting.InteriorCustomSize"));
}

void VolumetricLighting::DrawSettings()
{
	// The engine is given the new values at the next EarlyPrepass (on the render graph, once a revision carries them).
	Util::Checkbox(T(TKEY("enable_exteriors"), "Enable Volumetric Lighting in Exteriors"), settings.ExteriorEnabled);

	if (settings.ExteriorEnabled)
		DrawVolumetricLightingSettings(settings.ExteriorQuality, settings.ExteriorCustomSize, false);

	Util::Checkbox(T(TKEY("enable_interiors"), "Enable Volumetric Lighting in Interiors"), settings.InteriorEnabled);

	if (settings.InteriorEnabled)
		DrawVolumetricLightingSettings(settings.InteriorQuality, settings.InteriorCustomSize, true);
}

void VolumetricLighting::DrawVolumetricLightingSettings(CS::Setting<int32_t>& quality, CS::Setting<TextureSize>& customSize, const bool isInterior)
{
	auto& [Width, Height, Depth] = FetchCurrentSizeInUnits(isInterior);
	const char* qualityNames[] = {
		T(TKEY("quality_low"), "Low"),
		T(TKEY("quality_medium"), "Medium"),
		T(TKEY("quality_high"), "High"),
		T(TKEY("quality_custom"), "Custom")
	};

	Util::SliderInt(isInterior ? T(TKEY("interior_quality"), "Interior Quality") : T(TKEY("exterior_quality"), "Exterior Quality"), quality, 0,
		static_cast<uint8_t>(Quality::Count) - 1, qualityNames[std::clamp(quality.Get(), 0, static_cast<int32_t>(Quality::Count) - 1)], ImGuiSliderFlags_AlwaysClamp);

	const bool isCustomQuality = static_cast<Quality>(quality.Get()) == Quality::Custom;
	if (!isCustomQuality)
		ImGui::BeginDisabled();

	if (ImGui::SliderInt(isInterior ? T(TKEY("interior_width"), "Interior Width") : T(TKEY("exterior_width"), "Exterior Width"), &Width, 1, 20, FromUnits(Width, 32), ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput)) {
		auto size = customSize.Get();
		size.Width = Width * 32;
		customSize = size;
	}

	if (ImGui::SliderInt(isInterior ? T(TKEY("interior_height"), "Interior Height") : T(TKEY("exterior_height"), "Exterior Height"), &Height, 1, 20, FromUnits(Height, 32), ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput)) {
		auto size = customSize.Get();
		size.Height = Height * 32;
		customSize = size;
	}

	if (ImGui::SliderInt(isInterior ? T(TKEY("interior_depth"), "Interior Depth") : T(TKEY("exterior_depth"), "Exterior Depth"), &Depth, 1, 64, FromUnits(Depth, 10), ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput)) {
		auto size = customSize.Get();
		size.Depth = Depth * 10;
		customSize = size;
	}

	if (!isCustomQuality)
		ImGui::EndDisabled();
}

inline const char* VolumetricLighting::FromUnits(const int32_t value, const int32_t unitScale)
{
	static std::string s;
	s = std::to_string(value * unitScale);
	return s.c_str();
}

VolumetricLighting::TextureSize& VolumetricLighting::FetchCurrentSizeInUnits(const bool interior)
{
	auto& size = interior ? interiorSizeInUnits : exteriorSizeInUnits;
	size = interior ? VolumeSizeFor(settings.InteriorQuality, settings.InteriorCustomSize) : VolumeSizeFor(settings.ExteriorQuality, settings.ExteriorCustomSize);

	size.Height /= 32;
	size.Width /= 32;
	size.Depth /= 10;

	return size;
}

void VolumetricLighting::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.ExteriorQuality = std::clamp(settings.ExteriorQuality.Get(), 0, static_cast<int32_t>(Quality::Count) - 1);
	settings.InteriorQuality = std::clamp(settings.InteriorQuality.Get(), 0, static_cast<int32_t>(Quality::Count) - 1);
	// Custom sizes are stored in texels; the sliders only offer whole 32/32/10 texel steps
	for (auto* setting : { &settings.ExteriorCustomSize, &settings.InteriorCustomSize }) {
		auto size = setting->Get();
		size.Width = std::clamp(size.Width, 32, 640);
		size.Height = std::clamp(size.Height, 32, 640);
		size.Depth = std::clamp(size.Depth, 10, 640);
		*setting = size;
	}
}

void VolumetricLighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void VolumetricLighting::RestoreDefaultSettings()
{
	settings = {};
}

void VolumetricLighting::DataLoaded()
{
}

void VolumetricLighting::PostPostLoad()
{
	Snapshot::RegisterCapture(&VolumetricLighting::Capture);
	gVolumetricLightingSizeLow = reinterpret_cast<TextureSize*>(REL::RelocationID(527970, 414916).address());
	gVolumetricLightingSizeMedium = reinterpret_cast<TextureSize*>(REL::RelocationID(527973, 414919).address());
	gVolumetricLightingSizeHigh = reinterpret_cast<TextureSize*>(REL::RelocationID(527976, 414922).address());
	defaultSizeHigh = *gVolumetricLightingSizeHigh;

	// Ensure the VL raymarch compute shader is only dispatched once, rather than once for every level of depth
	// The updated raymarch shader iterates through the depth now instead
	// Skip the first call, the second call has read/write texture setup in the correct order
	REL::safe_fill(REL::RelocationID(100309, 107023).address() + REL::Relocate(0xA4, 0x406), REL::NOP, 3);
	// Exit the loop after the first iteration
	REL::safe_fill(REL::RelocationID(100309, 107023).address() + REL::Relocate(0x147, 0x4A9), REL::NOP, 6);
}

void VolumetricLighting::SetupResources()
{
	vlDataCB = new ConstantBuffer(ConstantBufferDesc<VLData>(), "VolumetricLighting::VLDataCB");
}

VolumetricLighting::TextureSize VolumetricLighting::VolumeSizeFor(const int32_t a_quality, const TextureSize& a_custom) const
{
	// As ApplyEngineVL: the quality picks the engine's preset; Custom is set into the High preset's slot.
	switch (static_cast<Quality>(a_quality)) {
	case Quality::Low:
		return *gVolumetricLightingSizeLow;
	case Quality::Medium:
		return *gVolumetricLightingSizeMedium;
	case Quality::Custom:
		return a_custom;
	default:
		return defaultSizeHigh;
	}
}

void VolumetricLighting::Capture(Snapshot::Builder& a_builder)
{
	if (!globals::features::volumetricLighting.loaded)
		return;
	Block block{};
	if (const auto* graphicsState = CS_ENGINE_READ(GraphicsState, CapturePoint, globals::game::graphicsState)) {
		block.screenWidth = static_cast<std::int32_t>(graphicsState->screenWidth);
		block.screenHeight = static_cast<std::int32_t>(graphicsState->screenHeight);
	}
	if (const auto* tes = CS_ENGINE_READ(TES, CapturePoint, RE::TES::GetSingleton()); tes && tes->interiorCell) {
		block.interior = true;
		block.cellHasSun = InteriorSun::CellHasSun(tes->interiorCell);
	}
	a_builder.Set(block);
}

void VolumetricLighting::EarlyPrepass()
{
	// The adopted snapshot's block; nothing before the first.
	const auto& snapshot = Snapshot::Current();
	const Block* frame = snapshot ? snapshot->Block<Block>() : nullptr;
	if (!frame)
		return;
	int32_t width = frame->screenWidth;
	int32_t height = frame->screenHeight;

	if (width != fullScreenX || height != fullScreenY) {
		blurHCS = nullptr;
		blurVCS = nullptr;
	}

	fullScreenX = width;
	fullScreenY = height;

	// The blur targets are allocated at the full screen size, but under dynamic resolution only
	// the top-left render area holds valid data. Clamping and dispatching against the full size
	// makes the blur taps read stale pixels from outside that area along the right/bottom edges.
	const float2 renderSize = Util::ConvertToDynamic(float2{ (float)width, (float)height });

	vlData.screenX = std::max(1, static_cast<int32_t>(renderSize.x));
	vlData.screenY = std::max(1, static_cast<int32_t>(renderSize.y));
	vlData.screenXMin1 = vlData.screenX - 1;
	vlData.screenYMin1 = vlData.screenY - 1;
	vlDataCB->Update(vlData);

	inInterior = frame->interior;
	inInteriorWithSun = InteriorSun::IsInteriorWithSun(frame->interior, frame->cellHasSun);

	// The engine makes its volume at the size it is given here. On the graph that must be the declared volume's size, so the
	// adopted values are given, from the Begin that adopts them.
	const auto wanted = WantedEngineVL();
	if (appliedVL && *appliedVL == wanted)
		return;
	appliedVL = wanted;
	ApplyEngineVL(wanted);
}

VolumetricLighting::EngineVL VolumetricLighting::WantedEngineVL() const
{
	EngineVL vl;
	TextureSize custom;
	if (ORGVolumetricLighting::Get().IsActive()) {
		const auto& adopted = RenderGraph::Frame::Revision::Adopted();
		const bool interior = adopted.Get(RenderGraph::Frame::Vars::Interior());
		// The live cell's sun counts only while the live cell is the adopted kind of location.
		vl.enabled = interior ? adopted.Get(settings.InteriorEnabled) && inInterior && inInteriorWithSun : adopted.Get(settings.ExteriorEnabled);
		vl.quality = interior ? adopted.Get(settings.InteriorQuality) : adopted.Get(settings.ExteriorQuality);
		custom = interior ? adopted.Get(settings.InteriorCustomSize) : adopted.Get(settings.ExteriorCustomSize);
	} else {
		vl.enabled = inInterior ? settings.InteriorEnabled && inInteriorWithSun : settings.ExteriorEnabled.Get();
		vl.quality = inInterior ? settings.InteriorQuality : settings.ExteriorQuality;
		custom = inInterior ? settings.InteriorCustomSize : settings.ExteriorCustomSize;
	}
	vl.highSize = static_cast<Quality>(vl.quality) == Quality::Custom ? custom : defaultSizeHigh;
	return vl;
}

void VolumetricLighting::ApplyEngineVL(const EngineVL& a_vl)
{
	*globals::game::bEnableVolumetricLighting = a_vl.enabled;
	*gVolumetricLightingSizeHigh = a_vl.highSize;
	SetVLQuality(GetVLDescriptor(), a_vl.quality);
}

VolumetricLighting::VolumetricLightingDescriptor& VolumetricLighting::GetVLDescriptor()
{
	using func_t = decltype(&VolumetricLighting::GetVLDescriptor);
	static REL::Relocation<func_t> func{ REL::RelocationID(100297, 107014) };
	return func();
}

void VolumetricLighting::SetVLQuality(VolumetricLightingDescriptor& descriptor, const uint32_t quality)
{
	using func_t = decltype(&VolumetricLighting::SetVLQuality);
	static REL::Relocation<func_t> func{ REL::RelocationID(100299, 107016).address() };
	func(descriptor, std::clamp<uint32_t>(quality, 0, 2));
}

RE::BSImagespaceShader* VolumetricLighting::CreateShader(const std::string_view& name, const std::string_view& fileName, RE::BSComputeShader* computeShader)
{
	auto shader = RE::BSImagespaceShader::Create();
	shader->shaderType = RE::BSShader::Type::ImageSpace;
	shader->fxpFilename = fileName.data();
	shader->name = name.data();
	shader->originalShaderName = fileName.data();
	shader->computeShader = computeShader;
	shader->isComputeShader = true;
	return shader;
}

RE::BSImagespaceShader* VolumetricLighting::GetOrCreateGenerateCS(RE::BSComputeShader* computeShader)
{
	if (generateCS == nullptr)
		generateCS = CreateShader("BSImagespaceShaderVolumetricLightingGenerateCS", "ISVolumetricLightingGenerateCS", computeShader);
	return generateCS;
}

RE::BSImagespaceShader* VolumetricLighting::GetOrCreateRaymarchCS(RE::BSComputeShader* computeShader)
{
	if (raymarchCS == nullptr)
		raymarchCS = CreateShader("BSImagespaceShaderVolumetricLightingRaymarchCS", "ISVolumetricLightingRaymarchCS", computeShader);
	return raymarchCS;
}

RE::BSImagespaceShader* VolumetricLighting::GetOrCreateBlurHCS(RE::BSComputeShader* computeShader)
{
	if (blurHCS == nullptr)
		blurHCS = CreateShader("BSImagespaceShaderVolumetricLightingBlurHCS", "ISVolumetricLightingBlurHCS", computeShader);
	return blurHCS;
}

RE::BSImagespaceShader* VolumetricLighting::GetOrCreateBlurVCS(RE::BSComputeShader* computeShader)
{
	if (blurVCS == nullptr)
		blurVCS = CreateShader("BSImagespaceShaderVolumetricLightingBlurVCS", "ISVolumetricLightingBlurVCS", computeShader);
	return blurVCS;
}

void VolumetricLighting::SetDimensionsCB() const
{
	auto cb = vlDataCB->CB();
	globals::d3d::context->CSSetConstantBuffers(1, 1, &cb);
}

void VolumetricLighting::SetGroupCountsHCS(uint32_t& threadGroupCountX, uint32_t& threadGroupCountY) const
{
	threadGroupCountX = (vlData.screenX + BlurThreadGroupSizeX - BlurWindow * 2u - 1u) / (BlurThreadGroupSizeX - BlurWindow * 2u);
	// One group per row, so the game's full-height count would blur rows outside the dynamic resolution area.
	threadGroupCountY = static_cast<uint32_t>(vlData.screenY);
}

void VolumetricLighting::SetGroupCountsVCS(uint32_t& threadGroupCountX, uint32_t& threadGroupCountY) const
{
	// One group per column, so the game's full-width count would blur columns outside the dynamic resolution area.
	threadGroupCountX = static_cast<uint32_t>(vlData.screenX);
	threadGroupCountY = (vlData.screenY + BlurThreadGroupSizeY - BlurWindow * 2u - 1u) / (BlurThreadGroupSizeY - BlurWindow * 2u);
}

#undef I18N_KEY_PREFIX
