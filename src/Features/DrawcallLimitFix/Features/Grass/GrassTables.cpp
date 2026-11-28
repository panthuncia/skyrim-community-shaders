// Grass's GPU tables and programs, and the depth commit hook that uploads the mirror's changes and the per-frame draw row into them.
// The draw row also carries the engine's per-frame grass values (wind, sun, fade, the camera), which DCLF's frame start takes on the
// game side into the frame's globals; the commit reads them from there.

// Helpers.h first. volk must precede every Vulkan header in the translation unit.
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Gpu/NamedRefs.h"
#include "Features/DrawcallLimitFix/Features/Grass/GrassTables.h"
#include "Features/DrawcallLimitFix/Features/Grass/GrassMirror.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Uploads.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Readback/Readbacks.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBufferSet.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "Features/DrawcallLimitFix/Features/Grass/Diagnostics/GrassEvents.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Snapshot/EngineReads.h"

#include "GrassShared/GrassFrame.h"

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <OpenRenderGraph/GraphHost.h>
#include <Resources/Buffers/Buffer.h>
#include <rhi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "Features/DrawcallLimitFix/Core/Gpu/SetupPrograms.h"

#include <cstring>
#include <memory>
#include <vector>

namespace DCLF::Features::Grass
{
	namespace
	{
		using namespace GrassInstances;

		Api::SceneResourcesId tablesId;

		constexpr std::uint32_t kInitialInstances = 256 * 1024, kInitialShapes = 4096, kInitialMeshes = 256;

		// The engine's per-frame grass values in the frame's globals (game side, DCLF's frame start). The values are reverse-engineered
		// from BSGrassShader::SetupGeometry and SetupTechnique.
		enum GameValue : std::uint32_t
		{
			kWindTimer,     // the timer at 0x142033090
			kWindStrength,  // the grass wind clamped to 60, times fWindGrassMultiplier:Display
			kShadowClamp,   // the sun's direction .w
			kFogNearW,      // the sun's colour .w
			kSun,           // 1 with ShadowSceneNode 0's sun light, then its direction (negated), diffuse and ambient
			kSunDirection,
			kSunDiffuse = kSunDirection + 3,
			kSunAmbient = kSunDiffuse + 3,
			kCamera = kSunAmbient + 3,  // 1 with the world camera, then its position and its frustum's top
			kCameraPosition,
			kCameraTop = kCameraPosition + 3,
			kAlphaParams,  // two floats
			kFadeClock = kAlphaParams + 2,  // the engine's frame times (BSShaderManager::State timerValues[1]) summed over the captures
			kGameValues,
		};
		Api::FrameGlobalsSlot gameSlot;  // install

		void CaptureGameValues(float* a_out)
		{
			a_out[kWindTimer] = Engine::Global<float>(Address::kWindTimer);
			a_out[kWindStrength] = std::min(Engine::Global<float>(Address::kWindSpeed), 60.0f) * Engine::Global<float>(Address::kWindGrassMultiplier);
			a_out[kShadowClamp] = Engine::Global<float>(Address::kShadowClampValue);
			a_out[kFogNearW] = Engine::Global<float>(Address::kFogNearColorW);
			auto* state = CS_ENGINE_READ(ShaderManagerState, CapturePoint, globals::game::smState);
			auto* scene = state ? state->shadowSceneNode[0] : nullptr;
			auto* sun = scene ? scene->GetRuntimeData().sunLight : nullptr;
			if (auto* light = sun ? static_cast<RE::NiDirectionalLight*>(sun->light.get()) : nullptr) {
				const auto& direction = light->GetWorldDirection();
				const auto& data = light->GetLightRuntimeData();
				a_out[kSun] = 1.0f;
				const float values[9]{ -direction.x, -direction.y, -direction.z, data.diffuse.red, data.diffuse.green, data.diffuse.blue, data.ambient.red,
					data.ambient.green, data.ambient.blue };
				std::memcpy(a_out + kSunDirection, values, sizeof(values));
			}
			if (auto* camera = CS_ENGINE_READ(Camera, CapturePoint, RE::Main::WorldRootCamera())) {
				a_out[kCamera] = 1.0f;
				const auto& translate = camera->world.translate;
				a_out[kCameraPosition] = translate.x;
				a_out[kCameraPosition + 1] = translate.y;
				a_out[kCameraPosition + 2] = translate.z;
				a_out[kCameraTop] = std::abs(camera->GetRuntimeData2().viewFrustum.fTop);
			}
			a_out[kAlphaParams] = Engine::Global<float>(Address::kAlphaParams);
			a_out[kAlphaParams + 1] = Engine::Global<float>(Address::kAlphaParams + 4);
			// Once per capture (each game update's frame start), so a repeated render or a ticket that is never rendered changes nothing.
			static float fadeClock = 0.0f;
			if (state)
				fadeClock += state->timerValues[1];
			a_out[kFadeClock] = fadeClock;
		}

		/** @brief A game value as the frame's ticket took it. */
		float Taken(std::uint32_t a_value)
		{
			return gameSlot ? FrameGlobals::Current().features[gameSlot.first + a_value] : 0.0f;
		}

		// The shader multiplies this by the shape's wave period. The timer scaled to 1/600 of a turn as the engine does.
		float WindTimeBase()
		{
			return Taken(kWindTimer) * (1.0f / 600.0f) * 6.28318f;
		}

		// The direction lives in each bucket row.
		void SampleWind(float (&a_wind)[4])
		{
			a_wind[0] = 0.0f;
			a_wind[1] = 0.0f;
			a_wind[2] = Taken(kWindStrength);
			a_wind[3] = 0.0f;
		}

		// The sun as SetupGeometry reads it, from ShadowSceneNode 0's sun light.
		void SampleLight(DrawRow& a_row)
		{
			a_row.dirLightDirection[3] = Taken(kShadowClamp);
			a_row.dirLightColor[3] = Taken(kFogNearW);
			if (Taken(kSun) == 0.0f)
				return;
			for (std::uint32_t c = 0; c < 3; ++c) {
				a_row.dirLightDirection[c] = Taken(kSunDirection + c);
				a_row.dirLightColor[c] = Taken(kSunDiffuse + c);
				a_row.ambientColor[c] = Taken(kSunAmbient + c);
			}
		}

		float IniFloat(const char* a_name, float a_fallback)
		{
			auto* setting = RE::GetINISetting(a_name);
			return setting ? setting->GetFloat() : a_fallback;
		}

		// The cull values follow Grass Optimizations' CullParams. Returns false without a camera.
		bool SampleCull(DrawRow& a_row)
		{
			auto* renderer = CS_ENGINE_READ(Renderer, CapturePoint, globals::game::renderer);
			if (Taken(kCamera) == 0.0f || !renderer)
				return false;
			const auto settings = GrassShared::Settings();
			static const float startFade = IniFloat("fGrassStartFadeDistance:Grass", 6000.0f);
			static const float vanillaMax = startFade + IniFloat("fGrassFadeRange:Grass", 2000.0f);
			const float maxDistance = settings.RenderDistanceOverride > 0.0f ? settings.RenderDistanceOverride : vanillaMax;
			for (std::uint32_t c = 0; c < 3; ++c)
				a_row.eye[c] = Taken(kCameraPosition + c);
			auto& cull = a_row.cull;
			cull.minPixelSize = settings.MinPixelSize;
			cull.fullDetailPx = settings.FullDetailPixelSize;
			cull.minKeep = std::clamp(settings.MinDensity, 0.0f, 1.0f);
			cull.fadeBand = 0.15f;
			cull.costBias = settings.MeshCostBias;
			cull.costBiasStart = std::max(0.0f, settings.CostBiasStartDistance);
			const auto [screenWidth, screenHeight] = renderer->GetScreenSize();
			const float top = Taken(kCameraTop);
			cull.projScale = top > 0.0f ? static_cast<float>(screenHeight) / (2.0f * top) : 0.0f;
			cull.maxDistSq = maxDistance * maxDistance;
			cull.edgeFadeStart = std::clamp(settings.EdgeFadeStart, 0.0f, 1.0f);
			cull.alphaParam1 = Taken(kAlphaParams);
			cull.alphaParam2 = Taken(kAlphaParams + 1);
			cull.invisibleFadeCull = settings.InvisibleFadeCull;
			cull.simpleShadingPx = std::max(0.0f, settings.SimpleShadingPixelSize);
			const float collision = std::max(0.0f, settings.CollisionDistance);
			cull.collisionDistSq = collision * collision;
			cull.midLodPx = settings.MidLODPixelSize;
			cull.farLodPx = settings.EnableMidLOD ? std::min(settings.FarLODPixelSize, settings.MidLODPixelSize) : settings.FarLODPixelSize;
			cull.lodBandPx = std::max(0.0f, settings.MeshLODBandPixels);
			cull.occlusionDepthBias = std::max(0.0f, settings.OcclusionDepthBias);
			cull.occlusionTightness = std::clamp(settings.OcclusionTightness, 0.0f, 1.0f);
			cull.flags = (settings.EnableMeshLOD && settings.EnableMidLOD ? kCullMidLod : 0u) | (settings.EnableMeshLOD && settings.EnableFarLOD ? kCullFarLod : 0u) |
			             (settings.EnableOcclusionCulling ? kCullOcclusion : 0u);
			return true;
		}

		ID3D11ShaderResourceView* ViewOf(const RE::NiSourceTexture* a_texture)
		{
			return a_texture && a_texture->rendererTexture ? reinterpret_cast<ID3D11ShaderResourceView*>(a_texture->rendererTexture->resourceView) : nullptr;
		}

		// Stands in for a PBR bucket's missing maps, and for the shadow mask without bShadowsOnGrass.
		ID3D11ShaderResourceView* DefaultView(bool a_normal)
		{
			auto* state = CS_ENGINE_READ(GraphicsState, RecordTime, globals::game::graphicsState);
			if (!state)
				return nullptr;
			auto& data = state->GetRuntimeData();
			return ViewOf(a_normal ? data.defaultTextureNormalMap.get() : data.defaultTextureWhite.get());
		}

		template <class Row>
		bool Bind(OwnedRow<Row>& a_row, DescriptorIndex Row::*a_field, ID3D11ShaderResourceView* a_view)
		{
			return a_view && a_row.Bind(a_field, GpuTextures::Get().ResolveBinding(a_view, 0));
		}

		template <class Row>
		bool BindSampler(OwnedRow<Row>& a_row, DescriptorIndex Row::*a_field, std::uint32_t a_address, std::uint32_t a_filter)
		{
			return a_row.Bind(a_field, GpuTextures::Get().SamplerBinding(a_address, a_filter));
		}

		// Rebinds when the binding generation moves. Returns false while the diffuse has no view, and the row keeps its last bindings.
		bool BindBucket(Bucket& a_bucket, std::uint32_t a_generation)
		{
			if (a_bucket.bound && a_bucket.bindingGeneration == a_generation)
				return true;
			DCLF_ZONE("CS.DCLF.Grass.BindBucket");
			a_bucket.bound = false;
			OwnedRow<BucketRow> row(a_bucket.row.Plain());
			auto& source = a_bucket.source;
			const std::uint32_t address = std::min(a_bucket.key.clampMode, 3u);
			// Always clamp the diffuse, as BSGrassShader::SetupTechnique does. With wrap, the opposite edge's stems bleed into the
			// card's corners and pass the alpha test.
			constexpr std::uint32_t kDiffuseAddress = 0;
			bool bound = Bind(row, &BucketRow::diffuseIndex, ViewOf(source.diffuse.get())) && BindSampler(row, &BucketRow::depthSampler, kDiffuseAddress, 2) &&
			             BindSampler(row, &BucketRow::colourSampler, kDiffuseAddress, 3);
			if (bound && a_bucket.key.pbr) {
				auto viewOr = [](const RE::NiPointer<RE::NiSourceTexture>& a_texture, bool a_normal) {
					auto* view = ViewOf(a_texture.get());
					return view ? view : DefaultView(a_normal);
				};
				bound = Bind(row, &BucketRow::normalIndex, viewOr(source.normal, true)) && Bind(row, &BucketRow::rmaosIndex, viewOr(source.rmaos, false)) &&
				        Bind(row, &BucketRow::featuresIndex, viewOr(source.features0, false)) && Bind(row, &BucketRow::fuzzIndex, viewOr(source.features1, false)) &&
				        Bind(row, &BucketRow::emissiveIndex, viewOr(source.emissive, false)) && BindSampler(row, &BucketRow::pbrSampler, address, 3);
			}
			if (!bound)
				return false;
			a_bucket.row = std::move(row);
			a_bucket.bindingGeneration = a_generation;
			a_bucket.bound = true;
			return true;
		}

		// Rebind buckets whose textures moved. A superseded import still holds its old storage until then.
		void FollowRelocatedTextures(GrassTables& a_tables, Mirror& a_mirror)
		{
			DCLF_ZONE("CS.DCLF.Grass.FollowRelocatedTextures");
			std::vector<ID3D11ShaderResourceView*> moved;
			const bool complete = GpuTextures::Get().RelocatedViews(a_tables.relocationCursor, moved);
			if (complete && moved.empty())
				return;
			const ankerl::unordered_dense::set<ID3D11ShaderResourceView*> views(moved.begin(), moved.end());
			for (std::uint32_t slot = 0; slot < a_mirror.BucketSlots(); ++slot) {
				auto* bucket = a_mirror.SlotBucket(slot);
				if (!bucket || !bucket->bound)
					continue;
				const auto& source = bucket->source;
				bool names = !complete || views.contains(ViewOf(source.diffuse.get()));
				if (bucket->key.pbr) {
					auto uses = [&](const RE::NiPointer<RE::NiSourceTexture>& a_texture, bool a_normal) {
						auto* view = ViewOf(a_texture.get());
						return views.contains(view ? view : DefaultView(a_normal));
					};
					names = names || uses(source.normal, true) || uses(source.rmaos, false) || uses(source.features0, false) || uses(source.features1, false) || uses(source.emissive, false);
				}
				if (names) {
					bucket->bound = false;
					a_mirror.MarkBucket(slot);
				}
			}
		}

		const SetupPrograms::Compute kCullProgram{ "DCLF grass cull", [] { return ComputeProgram::Desc{ .source = kGrassCullShader, .constantWords = kGrassCullConstantWords }; } };
		const SetupPrograms::Compute kArgsProgram{ "DCLF grass arguments", [] { return ComputeProgram::Desc{ .source = kGrassArgsShader, .constantWords = kGrassArgsConstantWords }; } };

		std::shared_ptr<void> Setup(const Api::SceneResourcesSetup& a_setup)
		{
			auto tables = std::make_shared<GrassTables>();
			tables->cull = kCullProgram.Get();
			tables->args = kArgsProgram.Get();
			const bool smallStart = a_setup.smallStart;
			tables->smallStart = smallStart;
			const std::uint32_t instances = smallStart ? 4u : kInitialInstances;
			// Instance tables grow by a quarter, not to a power of two. Large lists reach millions of instances at 68 bytes each.
			auto make = [](std::uint32_t a_units, std::uint32_t a_unitBytes, bool a_uav, const char* a_name, org::services::GrowthPolicy a_policy = {}) {
				return Draws::MakeVersioned(Draws::CreateWords(std::uint64_t(a_units) * a_unitBytes / 4, a_uav, a_name), { .extent = { 0, a_unitBytes, 4 }, .policy = a_policy });
			};
			constexpr auto instanceGrowth = org::services::GrowthPolicy::Headroom(25, 1024);
			tables->instances = make(instances, sizeof(Instance), false, "cs.dclf.grass-instances", instanceGrowth);
			tables->owners = make(instances, 4, false, "cs.dclf.grass-owners", instanceGrowth);
			tables->frame = make(instances, sizeof(InstanceFrame), true, "cs.dclf.grass-frame", instanceGrowth);
			tables->visible = make(instances, kVisibleUnitBytes, true, "cs.dclf.grass-visible", instanceGrowth);
			tables->shapes = make(smallStart ? 4u : kInitialShapes, sizeof(ShapeRow), false, "cs.dclf.grass-shapes");
			tables->buckets = make(smallStart ? 4u : kMaxBuckets, sizeof(BucketRow), false, "cs.dclf.grass-buckets");
			tables->meshes = make(smallStart ? 4u : kInitialMeshes, sizeof(MeshRow), false, "cs.dclf.grass-meshes");
			tables->draw = Draws::CreateWords(sizeof(DrawRow) / 4, false, "cs.dclf.grass-draw");
			tables->draws = Draws::CreateWords(kDrawsBytes / 4, true, "cs.dclf.grass-draws");
			auto device = a_setup.device;
			// The versioned tables' refs are made here only to check that refs can be made. Each frame's draw row carries the real ones.
			auto refOf = [&](org::Buffer& a_buffer) { return RenderGraph::DeclaredRefOf(device, a_buffer); };
			tables->drawRef = refOf(*tables->draw);
			bool addressed = static_cast<bool>(tables->drawRef);
			for (const auto* table : { &tables->instances, &tables->owners, &tables->frame, &tables->visible, &tables->shapes, &tables->buckets, &tables->meshes })
				addressed = addressed && static_cast<bool>(refOf(*(*table)->Get()));
			if (!addressed) {
				logger::warn("[DCLF] Grass's tables have no buffer view ref; grass stays native");
				return nullptr;
			}
			if (auto* host = RenderGraphRuntime::Get().Host()) {
				auto counts = std::make_shared<GrassCounts>();
				counts->readback = org::RunReadback::CreateRegions("cs.dclf.grass-counts-readback", sizeof(DrawsHeader), Draws::kReadbackRanges,
					RenderGraph::FatalHandler("Drawcall Limit Fix"));
				counts->filled.assign(counts->readback->Regions(), 0u);
				tables->counts = std::move(counts);
			}
			return tables;
		}

		void RegisterGraph(const Api::SceneResourcesGraph& a_graph)
		{
			const auto& tables = a_graph.State<GrassTables>();
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.grass-instances"), tables.instances);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.grass-owners"), tables.owners);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.grass-frame"), tables.frame);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.grass-visible"), tables.visible);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.grass-shapes"), tables.shapes);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.grass-buckets"), tables.buckets);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.grass-meshes"), tables.meshes);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.grass-draw"), tables.draw);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.grass-draws"), tables.draws);
		}

		// Never below the first capacity. Shrinking in an interior left the first exterior commits unable to fit grass, so none drew.
		// The floors cost about 17 MB.
		void Require(const Api::SceneResourcesRequirements& a_requirements)
		{
			const auto& tables = a_requirements.State<GrassTables>();
			const auto& inputs = a_requirements.inputs;
			auto& set = Draws::SceneBufferSet::Get();
			const auto floor = [&](std::uint32_t a_input, std::uint32_t a_initial) { return std::max<std::uint64_t>(inputs[a_input], tables.smallStart ? 0u : a_initial); };
			Draws::UnitGroup{}.Add(tables.instances).Add(tables.owners).Add(tables.frame).Add(tables.visible).Require(a_requirements.build,
				floor(kInstanceSlotsInput, kInitialInstances));
			set.Require(a_requirements.build, tables.shapes, floor(kShapeSlotsInput, kInitialShapes));
			set.Require(a_requirements.build, tables.buckets, kMaxBuckets);
			set.Require(a_requirements.build, tables.meshes, floor(kMeshSlotsInput, kInitialMeshes));
		}

		void PostInputs(std::span<std::uint32_t> a_words)
		{
			const auto& mirror = RenderMirror();
			a_words[kInstanceSlotsInput] = mirror.InstanceSlots();
			a_words[kShapeSlotsInput] = mirror.ShapeSlots();
			a_words[kBucketSlotsInput] = mirror.BucketSlots();
			a_words[kMeshSlotsInput] = mirror.MeshSlots();
		}

		void Latched(Api::SceneResourcesLatch& a_latch)
		{
			const auto& tables = a_latch.State<GrassTables>();
			a_latch.Latch(tables.draw, sizeof(DrawRow));
			a_latch.Latch(tables.draws, sizeof(DrawsHeader));
		}

		bool Fits(const Draws::NamedVersions& a_sealed, const Mirror& a_mirror, const GrassTables& a_tables)
		{
			const std::uint64_t instances = std::min({ a_sealed.Of(*a_tables.instances).units, a_sealed.Of(*a_tables.owners).units, a_sealed.Of(*a_tables.frame).units,
				a_sealed.Of(*a_tables.visible).units });
			return a_mirror.InstanceSlots() <= instances && a_mirror.ShapeSlots() <= a_sealed.Of(*a_tables.shapes).units &&
			       a_mirror.BucketSlots() <= std::min<std::uint64_t>(a_sealed.Of(*a_tables.buckets).units, kMaxBuckets) &&
			       a_mirror.MeshSlots() <= a_sealed.Of(*a_tables.meshes).units;
		}

		// A bucket whose texture has no view yet is sent with no instances and retried at the next commit.
		void SendChanges(const Draws::NamedVersions& a_sealed, Mirror& a_mirror, GrassTables& a_tables, Draws::LatchedUploads& a_uploads)
		{
			auto& filled = a_tables.filled;
			const auto& instances = a_sealed.Of(*a_tables.instances);
			const auto& owners = a_sealed.Of(*a_tables.owners);
			const auto& shapes = a_sealed.Of(*a_tables.shapes);
			const auto& buckets = a_sealed.Of(*a_tables.buckets);
			const auto& meshes = a_sealed.Of(*a_tables.meshes);
			const std::uint32_t generation = GpuTextures::Get().Generation();
			if (!filled[0].Holds(instances) || !filled[1].Holds(owners) || !filled[2].Holds(shapes) || !filled[3].Holds(buckets) || !filled[4].Holds(meshes) ||
				generation != a_tables.bindingGeneration)
				a_mirror.MarkAllChanged();
			a_tables.bindingGeneration = generation;
			for (const std::uint32_t slot : std::exchange(a_tables.unbound, {}))
				a_mirror.MarkBucket(slot);
			FollowRelocatedTextures(a_tables, a_mirror);
			Mirror::Changes changes;
			a_mirror.TakeChanges(changes);
			for (const auto& [first, count] : changes.freedRanges) {
				const std::vector<std::uint32_t> none(count, ~0u);
				a_uploads(a_tables.owners, none.data(), none.size() * 4, std::uint64_t(first) * 4);
			}
			for (const std::uint32_t slot : changes.shapes) {
				const auto& row = a_mirror.SlotRow(slot);
				a_uploads(a_tables.shapes, &row, sizeof(row), std::uint64_t(slot) * sizeof(ShapeRow));
				const auto* records = a_mirror.SlotInstances(slot);
				if (!records || records->empty())
					continue;
				a_uploads(a_tables.instances, records->data(), records->size() * sizeof(Instance), std::uint64_t(row.firstInstance) * sizeof(Instance));
				const std::vector<std::uint32_t> owner(records->size(), slot);
				a_uploads(a_tables.owners, owner.data(), owner.size() * 4, std::uint64_t(row.firstInstance) * 4);
			}
			for (const std::uint32_t mesh : changes.meshes) {
				const auto& row = a_mirror.MeshSlotRow(mesh);
				a_uploads(a_tables.meshes, &row, sizeof(row), std::uint64_t(mesh) * sizeof(MeshRow));
			}
			for (const std::uint32_t slot : changes.buckets) {
				const std::uint64_t offset = std::uint64_t(slot) * sizeof(BucketRow);
				auto* bucket = a_mirror.SlotBucket(slot);
				if (!bucket) {
					const BucketRow none{};
					a_uploads(a_tables.buckets, &none, sizeof(none), offset);
					continue;
				}
				if (!BindBucket(*bucket, generation)) {
					a_tables.unbound.push_back(slot);
					++a_tables.unboundBuckets;
				}
				OwnedRow<BucketRow> row = bucket->row;
				if (!bucket->bound)
					row.Plain().instanceCount = 0;
				a_uploads(a_tables.buckets, row, offset);
			}
			for (std::size_t i = 0; const auto* named : { &instances, &owners, &shapes, &buckets, &meshes })
				filled[i++].Filled(*named);
			a_tables.shapesSent += changes.shapes.size();
			a_tables.bucketsSent += changes.buckets.size();
		}

		// Prefer the graph texture's own descriptor when the view is its export. Otherwise import the view.
		bool BindCollision(OwnedRow<DrawRow>& a_row, const GrassShared::Collision& a_collision)
		{
			static constexpr std::string_view kIds[] = { GrassShared::kCollisionGraphId };
			std::uint32_t index = 0;
			if (Extensions::GraphTextureIndex(Extensions::GraphTextureReader::Grass, 0, "grass collision", a_collision.texture, kIds, index))
				return a_row.Bind(&DrawRow::collisionIndex, GpuTextures::Binding{ .index = index });
			return Bind(a_row, &DrawRow::collisionIndex, a_collision.texture);
		}

		bool BindFrameTextures(OwnedRow<DrawRow>& a_row)
		{
			static auto* shadowsOnGrass = CS_ENGINE_READ(Ini, RecordTime, RE::GetINISetting("bShadowsOnGrass:Display"));
			ID3D11ShaderResourceView* mask = DefaultView(false);
			bool engineMask = false;
			if ((!shadowsOnGrass || shadowsOnGrass->GetBool()) && CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)) {
				mask = reinterpret_cast<ID3D11ShaderResourceView*>(CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSHADOW_MASK].SRV);
				engineMask = true;
			}
			// The engine's mask is written this frame, so bind its graph texture rather than the view.
			std::uint32_t maskIndex = 0;
			if (engineMask && Extensions::EngineTargetTextureIndex(Extensions::GraphTextureReader::Grass, 1, "grass shadow mask", mask,
								  Extensions::ShadowMaskGraphTextureIds(), maskIndex)) {
				if (!a_row.Bind(&DrawRow::shadowMaskIndex, GpuTextures::Binding{ .index = maskIndex }))
					return false;
			} else if (!Bind(a_row, &DrawRow::shadowMaskIndex, mask)) {
				return false;
			}
			GrassShared::Collision collision;
			if (GrassShared::LatestCollision(collision) && BindCollision(a_row, collision))
				a_row.Plain().collision = collision.perFrame;
			else
				a_row.Unbind(&DrawRow::collisionIndex, DescriptorIndex::Sentinel(~0u));
			return true;
		}

		// Counts are zeroed once here when grass becomes DCLF's. After that grass.args-colour zeroes them every frame.
		void WriteDrawRow(const Draws::NamedVersions& a_sealed, const Mirror& a_mirror, GrassTables& a_tables, Draws::LatchedUploads& a_uploads, bool a_draw,
			std::uint32_t a_frame)
		{
			OwnedRow<DrawRow> owned;
			auto& row = owned.Plain();
			row.instances = DCLF::RefOf(a_sealed.Of(*a_tables.instances));
			row.shapes = DCLF::RefOf(a_sealed.Of(*a_tables.shapes));
			row.buckets = DCLF::RefOf(a_sealed.Of(*a_tables.buckets));
			row.meshes = DCLF::RefOf(a_sealed.Of(*a_tables.meshes));
			row.owners = DCLF::RefOf(a_sealed.Of(*a_tables.owners));
			row.frame = DCLF::RefOf(a_sealed.Of(*a_tables.frame));
			row.visible = DCLF::RefOf(a_sealed.Of(*a_tables.visible));
			SampleWind(row.windVector);
			// A repeated render's time is its ticket's, so it draws the wind still. New tables start without motion.
			if (const auto last = std::exchange(a_tables.timeFrame, a_frame); last != a_frame) {
				const float now = WindTimeBase();
				a_tables.prevTimeBase = last == ~0u ? now : a_tables.timeBase;
				a_tables.timeBase = now;
			}
			row.timeBase = a_tables.timeBase;
			row.prevTimeBase = a_tables.prevTimeBase;
			row.fadeNow = a_mirror.FadeClock();
			static const float fadeIn = IniFloat("fGrassFadeInTime:Grass", 0.0f);
			row.fadeInTimeRcp = fadeIn > 0.0f ? 1.0f / fadeIn : 1e6f;
			SampleLight(row);
			const bool sampled = SampleCull(row) && BindFrameTextures(owned);
			if (a_draw && sampled && a_tables.sentInstanceSlots) {
				row.instanceSlots = a_tables.sentInstanceSlots;
				row.shapeSlots = a_tables.sentShapeSlots;
				row.bucketSlots = a_tables.sentBucketSlots;
			}
			a_uploads(a_tables.draw, owned, 0);
			a_tables.row = row;
			const DrawsHeader header{};
			a_uploads(a_tables.draws, &header, sizeof(header), 0);
			if (!a_tables.countsZeroed) {
				const std::vector<std::uint32_t> zero(2 * kMaxDrawSlots, 0u);
				a_uploads(a_tables.draws, zero.data(), zero.size() * 4, kCountsOffset);
				a_tables.countsZeroed = true;
			}
			if (row.instanceSlots)
				a_tables.ready.store(true, std::memory_order_release);
		}

		// When the mirror outgrows the tables, it keeps its changes and the commit draws the last sent state until growth is adopted.
		void UploadGrass(const Draws::NamedVersions& a_sealed, Mirror& a_mirror, GrassTables& a_tables, Draws::LatchedUploads& a_uploads,
			std::vector<std::shared_ptr<const void>>& a_owners, bool a_owned, std::uint32_t a_frame)
		{
			a_tables.ready.store(false, std::memory_order_release);
			if (!a_tables.cull || !a_tables.instances)
				return;
			if (a_owned && !a_tables.owned)
				a_tables.countsZeroed = false;
			a_tables.owned = a_owned;
			const bool fits = Fits(a_sealed, a_mirror, a_tables);
			if (fits) {
				for (auto& owner : a_mirror.TakeRetired())
					a_owners.push_back(std::move(owner));
				SendChanges(a_sealed, a_mirror, a_tables, a_uploads);
				a_tables.sentInstanceSlots = a_mirror.InstanceSlots();
				a_tables.sentShapeSlots = a_mirror.ShapeSlots();
				a_tables.sentBucketSlots = a_mirror.BucketSlots();
			} else if (a_mirror.BucketSlots() > kMaxBuckets)
				++a_tables.overBuckets;
			else
				++a_tables.outgrown;
			++a_tables.uploads;
			WriteDrawRow(a_sealed, a_mirror, a_tables, a_uploads, a_owned && a_mirror.BucketSlots() <= kMaxBuckets, a_frame);
		}

		// See dclf-architecture.md, "Grass: the draws".
		void Commit(const Api::CommitContext& a_commit)
		{
			if (a_commit.kind != Api::CommitKind::Depth)
				return;
			auto* tables = TablesOf(a_commit.scene);
			if (!tables)
				return;
			auto& mirror = RenderMirror();
			const bool owned = IndirectDraws::Get().DrawFamilyOwned(Api::DrawFamily::Grass);
			UploadGrass(a_commit.adopted, mirror, *tables, *a_commit.latched, a_commit.executionOwners, owned, a_commit.frame);
			// The engine's passes were withheld, so grass that cannot draw here leaves a visible hole.
			if (owned && mirror.InstanceSlots() && !tables->ready.load(std::memory_order_acquire))
				Diagnostics::Notify(GrassMissed{ a_commit.frame, mirror.InstanceSlots(), a_commit.adopted.Of(*tables->instances).units, mirror.BucketSlots(),
					a_commit.adopted.Of(*tables->buckets).units, mirror.ShapeSlots(), a_commit.adopted.Of(*tables->shapes).units, mirror.MeshSlots(),
					a_commit.adopted.Of(*tables->meshes).units });
		}
	}


	GrassTables* TablesOf(const Draws::SceneBuffers& a_scene)
	{
		return Extensions::SceneResourcesOf<GrassTables>(a_scene, tablesId);
	}

	std::shared_ptr<GrassTables> SharedTablesOf(const Draws::SceneBuffers& a_scene)
	{
		return Extensions::SharedSceneResourcesOf<GrassTables>(a_scene, tablesId);
	}

	float TakenFadeClock()
	{
		return Taken(kFadeClock);
	}

	Api::SceneResourcesId TablesId()
	{
		return tablesId;
	}

	void RegisterTables(Api::InstallContext& a_context)
	{
		gameSlot = a_context.RegisterFrameGlobals({ .name = "grass", .floats = kGameValues, .capture = &CaptureGameValues });
		tablesId = a_context.RegisterSceneResources({ .name = "grass", .setup = &Setup, .registerGraph = &RegisterGraph, .require = &Require, .postInputs = &PostInputs,
			.inputWords = kInputWords, .latched = &Latched });
		a_context.RegisterCommitHook({ .name = "grass", .commit = &Commit });
	}
}
