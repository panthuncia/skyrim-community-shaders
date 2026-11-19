// Physical Sky: its pixel-stage resources as frame textures, and as the engine draw families' handles. The render thread's
// callbacks read the feature live.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Integrations/PhysicalSky/PhysicalSky.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "Globals.h"

#include "Features/PhysicalSky.h"
#include "Features/PhysicalSky/ORGPhysicalSky.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/PublishedResources.h"

#include <format>

namespace DCLF::Integrations::PhysicalSky
{
	namespace
	{
		using PhysSkyKind = GpuTextures::ViewKind;

		template <class T>
		ID3D11ShaderResourceView* PhysSkySrvOf(const T& a_texture)
		{
			return a_texture ? a_texture->srv.get() : nullptr;
		}

		/** @brief Where a slot's texture comes from. */
		struct PhysSkySlot
		{
			bool lut = false;  // a graph twin, exported, in every graph mode
			ID3D11ShaderResourceView* (*srv)(const ::PhysicalSky& a_sky) = nullptr;
		};

		// kOrgTextureRegisters' textures, in that order.
		const std::array<PhysSkySlot, kOrgTextureRegisters.size()> kPhysSkySlots{ {
			{ true, [](const ::PhysicalSky& a_sky) { return PhysSkySrvOf(a_sky.texTrLut); } },
			{ true, [](const ::PhysicalSky& a_sky) { return PhysSkySrvOf(a_sky.texSvLut); } },
			{ true, [](const ::PhysicalSky& a_sky) { return PhysSkySrvOf(a_sky.texApLut); } },
			{ false, [](const ::PhysicalSky& a_sky) { return PhysSkySrvOf(a_sky.texApShadow); } },
			{ false, [](const ::PhysicalSky& a_sky) { return PhysSkySrvOf(a_sky.texSkyTail); } },
			{ false, [](const ::PhysicalSky& a_sky) { return PhysSkySrvOf(a_sky.texVolCubeAux); } },
			{ false, [](const ::PhysicalSky& a_sky) { return PhysSkySrvOf(a_sky.texCloudShadowMean); } },
			{ true, [](const ::PhysicalSky& a_sky) { return PhysSkySrvOf(a_sky.texDiskPhaseLut); } },
			{ false, [](const ::PhysicalSky& a_sky) { return PhysSkySrvOf(a_sky.texLightningCubeLum); } },
			{ false, [](const ::PhysicalSky& a_sky) { return PhysSkySrvOf(a_sky.texVolTr); } },
			{ false, [](const ::PhysicalSky& a_sky) { return PhysSkySrvOf(a_sky.texVolLum); } },
			{ false, [](const ::PhysicalSky& a_sky) { return PhysSkySrvOf(a_sky.texShadowVolume); } },
			{ false, [](const ::PhysicalSky& a_sky) { return PhysSkySrvOf(a_sky.texLightningChannels); } },
			{ false, [](const ::PhysicalSky& a_sky) { return PhysSkySrvOf(a_sky.texVolCubeTr); } },
			{ false, [](const ::PhysicalSky& a_sky) { return PhysSkySrvOf(a_sky.texVolCubeLum); } },
		} };

		// The graph twins' published ids, by slot: the LUTs and the AP shadow. Empty for D3D11's textures.
		constexpr std::array<std::string_view, kOrgTextureRegisters.size()> kPhysSkySlotIds{ ORGPhysicalSky::kTransmittanceLutId, ORGPhysicalSky::kSkyViewLutId,
			ORGPhysicalSky::kApLutId, ORGPhysicalSky::kApShadowId, {}, {}, {}, ORGPhysicalSky::kDiskPhaseLutId };

		constexpr std::size_t PhysSkySlotOf(std::uint32_t a_register)
		{
			for (std::size_t i = 0; i < kOrgTextureRegisters.size(); ++i)
				if (kOrgTextureRegisters[i] == a_register)
					return i;
			return kOrgTextureRegisters.size();
		}

		template <std::uint32_t Register>
		void* PhysSkyFrameView()
		{
			// A null view keeps the commit's capture, which in guest mode is what the feature binds.
			return OrgView(PhysSkySlotOf(Register));
		}

		// The view medium's cloud depth. D3D11 binds it at t116 after the medium composite, so no capture holds it at t105.
		void* PhysSkyMediumCloudDepthView()
		{
			const auto& sky = globals::features::physicalSky;
			const bool made = !RenderGraph::Engine::Native::Active() && sky.IsActive() && sky.VolumetricCloudsEnabled() && sky.texVolFilteredAux;
			return made ? sky.texVolFilteredAux->srv.get() : GpuTextures::NullView(PhysSkyKind::Texture2D);
		}

		template <std::uint32_t Register>
		void RegisterPhysSkySlot(Api::InstallContext& a_context, std::string_view a_name)
		{
			constexpr std::size_t slot = PhysSkySlotOf(Register);
			static_assert(slot < kOrgTextureRegisters.size());
			a_context.RegisterFrameTexture(
				{ .name = a_name, .textureRegister = Register, .view = &PhysSkyFrameView<Register>, .capturedWhenNull = true, .graphResources = { kPhysSkySlotIds[slot] } });
		}

		// The LUTs' views are the twins' exports, so commits read them ordered after the LUT passes at EarlyPrepass.
		void Install(Api::InstallContext& a_context)
		{
			RegisterPhysSkySlot<kTransmittanceRegister>(a_context, "physical sky transmittance");
			RegisterPhysSkySlot<kSkyViewRegister>(a_context, "physical sky sky-view");
			RegisterPhysSkySlot<kAerialPerspectiveRegister>(a_context, "physical sky aerial perspective");
			RegisterPhysSkySlot<kApShadowRegister>(a_context, "physical sky aerial perspective shadow");
			RegisterPhysSkySlot<kSkyTailRegister>(a_context, "physical sky sky tail");
			RegisterPhysSkySlot<kVolCubeDepthRegister>(a_context, "physical sky cloud cube depth");
			RegisterPhysSkySlot<kShadowMeanRegister>(a_context, "physical sky cloud shadow mean");
			RegisterPhysSkySlot<kDiskPhaseRegister>(a_context, "physical sky disk phase");
			RegisterPhysSkySlot<kLightningReflectionRegister>(a_context, "physical sky lightning reflection");
			RegisterPhysSkySlot<kVolTrRegister>(a_context, "physical sky cloud transmittance");
			RegisterPhysSkySlot<kVolLumRegister>(a_context, "physical sky cloud luminance");
			RegisterPhysSkySlot<kShadowVolumeRegister>(a_context, "physical sky cloud shadow volume");
			RegisterPhysSkySlot<kLightningChannelsRegister>(a_context, "physical sky lightning channels");
			RegisterPhysSkySlot<kVolCubeTrRegister>(a_context, "physical sky cloud cube transmittance");
			RegisterPhysSkySlot<kVolCubeLumRegister>(a_context, "physical sky cloud cube luminance");
			a_context.RegisterFrameTexture({ .name = "physical sky medium cloud depth", .textureRegister = kMediumCloudDepthRegister, .view = &PhysSkyMediumCloudDepthView });
		}
	}

	ID3D11ShaderResourceView* OrgView(std::size_t a_slot)
	{
		if (a_slot >= kPhysSkySlots.size())
			return nullptr;
		const auto& sky = globals::features::physicalSky;
		const auto& slot = kPhysSkySlots[a_slot];
		auto* srv = sky.loaded && sky.cbData.enabled ? slot.srv(sky) : nullptr;
		if (!srv)
			return GpuTextures::NullView(kOrgTextureKinds[a_slot]);
		// The LUTs are graph textures in every graph mode. So is the AP shadow while its twin is the texture (PhysicalSky.h).
		if (slot.lut || (!kPhysSkySlotIds[a_slot].empty() && PublishedResources::IsExportOf(srv, kPhysSkySlotIds[a_slot])))
			return srv;
		// Native mode runs none of the D3D11 passes that write the rest.
		if (RenderGraph::Engine::Native::Active())
			return GpuTextures::NullView(kOrgTextureKinds[a_slot]);
		return nullptr;
	}

	bool ResolveOrgHandles(OrgHandles& a_out, std::vector<std::shared_ptr<const void>>& a_owners, const ReadPublished& a_read, std::string& a_why)
	{
		a_out = {};
		if (!globals::features::physicalSky.loaded)
			return true;
		for (std::size_t i = 0; i < kOrgTextureRegisters.size(); ++i) {
			const std::uint32_t r = kOrgTextureRegisters[i];
			const auto tag = GpuTextures::Tag(GpuTextures::Source::FamilyHeader, 4 + static_cast<std::uint32_t>(i));
			std::string published;
			if (auto* view = OrgView(i)) {
				// A LUT's export is read by its id. A zero view is imported.
				published = PublishedResources::ExportedId(view);
				if (published.empty()) {
					GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
					auto binding = GpuTextures::Get().ResolveBinding(view, tag, &failure);
					if (binding.index == GpuTextures::kInvalid) {
						a_why = std::format("Physical Sky's t{} cannot be imported: {}", r, GpuTextures::DescribeFailure(view, r, failure));
						return false;
					}
					a_out.textures[i] = binding.index;
					if (binding.owner)
						a_owners.push_back(std::move(binding.owner));
				}
			} else {
				// What the feature binds on D3D11: an import, a published id, or the zero view for nothing bound.
				Family::Header header;
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				ID3D11ShaderResourceView* bound = nullptr;
				const auto result = Family::ResolveHeader(r, kOrgTextureKinds[i], tag, header, a_owners, &failure, &bound);
				if (result != Family::HeaderResult::Ok) {
					a_why = std::format("Physical Sky's t{} cannot be read: {}", r,
						result == Family::HeaderResult::Buffer ? std::string("a D3D11 buffer") : GpuTextures::DescribeFailure(bound, r, failure));
					return false;
				}
				published = std::move(header.publishedId);
				a_out.textures[i] = header.index;
			}
			if (published.empty())
				continue;
			std::string why;
			if (!a_read(published, a_out.textures[i], why)) {
				a_why = std::format("Physical Sky's t{} holds '{}': {}", r, published, why);
				return false;
			}
		}
		const auto& graph = ORGPhysicalSky::Get();
		for (const auto [at, which] : { std::pair{ 0, ORGPhysicalSky::Sampler::Transmittance }, std::pair{ 1, ORGPhysicalSky::Sampler::SkyView } }) {
			std::shared_ptr<const void> owner;
			a_out.samplers[at] = graph.SamplerIndex(which, &owner);
			if (a_out.samplers[at] == ORGPhysicalSky::kNoSampler) {
				a_why = "Physical Sky's samplers are not on the render graph (its LUTs stayed on D3D11)";
				return false;
			}
			a_owners.push_back(std::move(owner));
		}
		return true;
	}

	Api::Feature Integration()
	{
		return { .name = "physical sky", .install = &Install, .status = [] { return FeatureStatus(globals::features::physicalSky); } };
	}
}
