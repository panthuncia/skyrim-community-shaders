#include "GrassCollision.h"
#include "GrassCollision/ORGGrassCollision.h"

#include "Globals.h"
#include "GrassShared/GrassFrame.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Utils/ActorUtils.h"
#include "Utils/D3D.h"

#define I18N_KEY_PREFIX "feature.grass_collision."

static constexpr uint MAX_BOUNDING_BOXES = 64;
static constexpr uint MAX_COLLISIONS_PER_BOUNDING_BOX = 64;
static constexpr uint MAX_COLLISIONS = MAX_BOUNDING_BOXES * MAX_COLLISIONS_PER_BOUNDING_BOX;
static constexpr float MAX_ACTOR_DISTANCE = 2048.0f;
static constexpr float MAX_ACTOR_SQ_DISTANCE = MAX_ACTOR_DISTANCE * MAX_ACTOR_DISTANCE;
static constexpr float MIN_COLLISION_RADIUS_DISTANCE_SCALE = 0.001f;

// The collision texture: 512x512 cells of 8 units (4096 units across), RGBA16F.
static D3D11_TEXTURE2D_DESC CollisionTextureDesc()
{
	return {
		.Width = 512,
		.Height = 512,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
		.SampleDesc = { .Count = 1 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS
	};
}

struct GrassCollisionActorCandidate
{
	RE::ActorHandle handle;
	float sqDistance;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	GrassCollision::Settings,
	EnableGrassCollision,
	TrackRagdolls)

void GrassCollision::DrawSettings()
{
	if (ImGui::TreeNodeEx(T(TKEY("grass_collision"), "Grass Collision"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("enable"), "Enable Grass Collision"), (bool*)&settings.EnableGrassCollision);
		ImGui::TreePop();
	}
}

void GrassCollision::QueueCollisions()
{
	if (!settings.EnableGrassCollision)
		return;

	eastl::vector<GrassCollisionActorCandidate> actorCandidates{};
	RE::NiPoint3 cameraPosition = Util::GetEyePosition();

	auto addActorCandidate = [&](RE::ActorHandle a_handle) {
		auto actor = a_handle.get();
		if (actor && actor->Is3DLoaded()) {
			float sqDistance = cameraPosition.GetSquaredDistance(actor->GetPosition());
			if (sqDistance <= MAX_ACTOR_SQ_DISTANCE)
				actorCandidates.push_back({ a_handle, sqDistance });
		}
	};

	// Actor query code from po3 under MIT
	// https://github.com/powerof3/PapyrusExtenderSSE/blob/7a73b47bc87331bec4e16f5f42f2dbc98b66c3a7/include/Papyrus/Functions/Faction.h#L24C7-L46
	if (const auto processLists = RE::ProcessLists::GetSingleton(); processLists) {
		for (auto& actorHandle : processLists->highActorHandles) {
			addActorCandidate(actorHandle);
		}
	}

	if (auto player = RE::PlayerCharacter::GetSingleton()) {
		addActorCandidate(player->GetHandle());
	}

	std::sort(actorCandidates.begin(), actorCandidates.end(), [](const GrassCollisionActorCandidate& a, const GrassCollisionActorCandidate& b) {
		return a.sqDistance < b.sqDistance;
	});

	eastl::vector<BoundingBoxPacked> boundingBoxData{};
	boundingBoxData.reserve(MAX_BOUNDING_BOXES);

	eastl::vector<float4> collisionsData{};
	collisionsData.reserve(MAX_COLLISIONS);

	uint collisionIndexExtent = 0;

	for (const auto& actorCandidate : actorCandidates) {
		auto actor = actorCandidate.handle.get();
		if (actor && actor->Is3DLoaded()) {
			auto root = actor->Get3D(false);
			if (!root)
				continue;

			float distance = std::sqrt(actorCandidate.sqDistance);

			eastl::vector<float4> collisionShapes{};

			RE::BSVisit::TraverseScenegraphCollision(root, [&](RE::bhkNiCollisionObject* a_object) -> RE::BSVisit::BSVisitControl {
				RE::NiPoint3 centerPos;
				float radius;
				if (Util::GetShapeBound(a_object, centerPos, radius)) {
					if (radius < distance * MIN_COLLISION_RADIUS_DISTANCE_SCALE)
						return RE::BSVisit::BSVisitControl::kContinue;

					centerPos -= cameraPosition;

					float4 data{};
					data.x = centerPos.x;
					data.y = centerPos.y;
					data.z = centerPos.z;
					data.w = radius;

					collisionShapes.push_back(data);
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});

			std::sort(collisionShapes.begin(), collisionShapes.end(), [](const float4& a, const float4& b) {
				return a.w > b.w;
			});

			BoundingBoxPacked boundingBox;
			boundingBox.MinExtent = { FLT_MAX, FLT_MAX };
			boundingBox.MaxExtent = { -FLT_MAX, -FLT_MAX };

			boundingBox.IndexStart = collisionIndexExtent;
			boundingBox.IndexEnd = collisionIndexExtent;

			uint boundingBoxCollisions = 0;

			for (const auto& data : collisionShapes) {
				collisionsData.push_back(data);

				float2 pointMin(data.x - data.w, data.y - data.w);
				float2 pointMax(data.x + data.w, data.y + data.w);

				boundingBox.MinExtent.x = std::min(boundingBox.MinExtent.x, pointMin.x);
				boundingBox.MinExtent.y = std::min(boundingBox.MinExtent.y, pointMin.y);

				boundingBox.MaxExtent.x = std::max(boundingBox.MaxExtent.x, pointMax.x);
				boundingBox.MaxExtent.y = std::max(boundingBox.MaxExtent.y, pointMax.y);

				boundingBox.IndexEnd++;

				boundingBoxCollisions++;

				if (boundingBoxCollisions == MAX_COLLISIONS_PER_BOUNDING_BOX)
					break;
			}

			if (boundingBox.IndexStart != boundingBox.IndexEnd) {
				boundingBoxData.push_back(boundingBox);
				collisionIndexExtent = boundingBox.IndexEnd;
				if (boundingBoxData.size() == MAX_BOUNDING_BOXES)
					break;
			}
		}
	}

	queuedBoundingBoxes = std::move(boundingBoxData);
	queuedCollisions = std::move(collisionsData);
}

GrassCollision::PerFrame GrassCollision::NextPerFrame(const RE::NiPoint3& a_eye)
{
	PerFrame perFrameData{};

	auto eyePosNI = a_eye;
	if (!std::exchange(hasPrevEye, true))
		prevEyePos = eyePosNI;

	auto eyePos = float2{ eyePosNI.x, eyePosNI.y };

	float worldSize = 4096.0f;
	uint textureArrayDims = 512;

	float cellSize = worldSize / textureArrayDims;

	auto cellID = eyePos / cellSize;
	cellID = { round(cellID.x), round(cellID.y) };
	auto cellOrigin = cellID * cellSize;

	float2 cellIDDiff = prevCellID - cellID;
	prevCellID = cellID;

	perFrameData.PosOffset = cellOrigin - eyePos;

	perFrameData.ArrayOrigin = {
		((int)cellID.x - textureArrayDims / 2) % textureArrayDims,
		((int)cellID.y - textureArrayDims / 2) % textureArrayDims
	};

	perFrameData.ValidMargin = { (int)cellIDDiff.x, (int)cellIDDiff.y };

	perFrameData.TimeDelta = *globals::game::deltaTime * !globals::game::ui->GameIsPaused();

	perFrameData.CameraHeightDelta = prevEyePos.z - eyePosNI.z;

	perFrameData.BoundingBoxCount = std::min((uint)queuedBoundingBoxes.size(), MAX_BOUNDING_BOXES);

	prevEyePos = eyePosNI;
	return perFrameData;
}

void GrassCollision::Update(const RE::NiPoint3& a_eye)
{
	static Util::FrameChecker frameChecker;
	if (frameChecker.IsNewFrame()) {
		if (!collisionTexture)
			return;

		const PerFrame perFrameData = NextPerFrame(a_eye);
		LogColliders("D3D11");

		auto context = globals::d3d::context;

		if (!queuedCollisions.empty()) {
			D3D11_MAPPED_SUBRESOURCE mapped;
			DX::ThrowIfFailed(context->Map(collisionInstances->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
			size_t bytes = sizeof(float4) * queuedCollisions.size();
			memcpy_s(mapped.pData, bytes, queuedCollisions.data(), bytes);
			context->Unmap(collisionInstances->resource.get(), 0);
		}

		if (perFrameData.BoundingBoxCount > 0) {
			D3D11_MAPPED_SUBRESOURCE mapped;
			DX::ThrowIfFailed(context->Map(collisionBoundingBoxes->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
			size_t bytes = sizeof(BoundingBoxPacked) * perFrameData.BoundingBoxCount;
			memcpy_s(mapped.pData, bytes, queuedBoundingBoxes.data(), bytes);
			context->Unmap(collisionBoundingBoxes->resource.get(), 0);
		}

		queuedBoundingBoxes.clear();
		queuedCollisions.clear();

		perFrame->Update(perFrameData);
		lastPerFrame = perFrameData;

		UpdateCollisionTexture();

		ID3D11Buffer* buffers[1];
		buffers[0] = perFrame->CB();
		context->VSSetConstantBuffers(5, ARRAYSIZE(buffers), buffers);

		ID3D11ShaderResourceView* srvs[] = { collisionTexture->srv.get() };
		context->VSSetShaderResources(100, ARRAYSIZE(srvs), srvs);
	}
}

void GrassCollision::LoadSettings(json& o_json)
{
	settings = o_json;
}

void GrassCollision::SaveSettings(json& o_json)
{
	o_json = settings;
}

void GrassCollision::RestoreDefaultSettings()
{
	settings = {};
}

void GrassCollision::PostPostLoad()
{
	Hooks::Install();
	GrassShared::OnStoodDownFrame(&GrassCollision::StoodDownUpdate);
	ORGGrassCollision::Reserve();
}

ID3D11ShaderResourceView* GrassCollision::CollisionSRV() const
{
	if (auto* view = ORGGrassCollision::Get().CollisionSRV())
		return view;
	return collisionTexture ? collisionTexture->srv.get() : nullptr;
}

void GrassCollision::PublishCollision() const
{
	static_assert(sizeof(PerFrame) <= sizeof(GrassShared::Collision::perFrame));
	GrassShared::Collision published;
	published.texture = CollisionSRV();
	published.onGraph = published.texture && published.texture == ORGGrassCollision::Get().CollisionSRV();
	std::memcpy(published.perFrame.data(), &lastPerFrame, sizeof(PerFrame));
	published.frame = globals::game::graphicsState->frameCount;
	GrassShared::PublishCollision(published);
}

void GrassCollision::LogColliders(const char* a_path) const
{
	static const bool requested = [] {
		char value[8]{};
		return GetEnvironmentVariableA("CS_GRASS_COLLISION_STATS", value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}();
	const auto frame = globals::game::graphicsState->frameCount;
	if (!requested || frame % 120 != 0)
		return;
	logger::info("[GRASS COLLISION] frame {} ({}): {} colliders, {} collision shapes", frame, a_path,
		std::min((uint)queuedBoundingBoxes.size(), MAX_BOUNDING_BOXES), queuedCollisions.size());
}

void GrassCollision::StoodDownUpdate(const GrassShared::StoodDownFrame& a_frame)
{
	auto& self = globals::features::grassCollision;
	// The render graph's path updates and publishes at EarlyPrepass, before DCLF's depth commit reads what is published.
	if (!self.collisionTexture || !self.perFrame || ORGGrassCollision::Get().IsActive())
		return;

	self.Update(RE::NiPoint3(a_frame.eye[0], a_frame.eye[1], a_frame.eye[2]));
	self.PublishCollision();
}

void GrassCollision::EarlyPrepass()
{
	auto& graph = ORGGrassCollision::Get();
	if (!graph.IsActive() || !perFrame)
		return;

	// The main view's eye: posAdjust holds the last shadow pass's here (GrassShared::StoodDownFrame).
	auto* camera = RE::Main::WorldRootCamera();
	if (!camera)
		return;
	const PerFrame perFrameData = NextPerFrame(camera->world.translate);
	LogColliders("ORG");

	// Disabled: no colliders, and every cell new, so the update writes zero everywhere as the D3D11 path's clear does.
	PerFrame latched = perFrameData;
	if (!settings.EnableGrassCollision) {
		latched.BoundingBoxCount = 0;
		latched.ValidMargin = { 512, 512 };
	}
	const size_t collisions = std::min(queuedCollisions.size(), (size_t)MAX_COLLISIONS);
	graph.Stage({ .perFrame = std::as_bytes(std::span(&latched, 1)),
		.boxes = std::as_bytes(std::span(queuedBoundingBoxes.data(), latched.BoundingBoxCount)),
		.instances = std::as_bytes(std::span(queuedCollisions.data(), latched.BoundingBoxCount ? collisions : 0)) });

	queuedBoundingBoxes.clear();
	queuedCollisions.clear();

	// The engine's grass reads these from its first SetupGeometry of the frame; DCLF's grass from the depth commit.
	perFrame->Update(perFrameData);
	lastPerFrame = perFrameData;
	if (GrassShared::DclfOwned())
		PublishCollision();
}

void GrassCollision::SetupResources()
{
	perFrame = new ConstantBuffer(ConstantBufferDesc<PerFrame>(), "GrassCollision::PerFrameCB");

	const ORGGrassCollision::Limits limits{ .perFrameBytes = sizeof(PerFrame),
		.maxBoxes = MAX_BOUNDING_BOXES,
		.boxStride = sizeof(BoundingBoxPacked),
		.maxInstances = MAX_COLLISIONS,
		.instanceStride = sizeof(float4),
		.texture = CollisionTextureDesc() };
	// The render graph's collision texture replaces the D3D11 one; on a later graph failure the D3D11 path makes its own.
	if (!ORGGrassCollision::Get().Setup(limits, [this] { CreateD3D11Resources(); }))
		CreateD3D11Resources();
}

void GrassCollision::CreateD3D11Resources()
{
	if (collisionTexture)
		return;

	{
		const D3D11_TEXTURE2D_DESC texDesc = CollisionTextureDesc();

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		collisionTexture = new Texture2D(texDesc, "GrassCollision::Collision");
		collisionTexture->CreateSRV(srvDesc);
		collisionTexture->CreateUAV(uavDesc);
	}

	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(BoundingBoxPacked);
		sbDesc.ByteWidth = sizeof(BoundingBoxPacked) * MAX_BOUNDING_BOXES;
		collisionBoundingBoxes = eastl::make_unique<Buffer>(sbDesc, nullptr, "GrassCollision::BoundingBoxes");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = MAX_BOUNDING_BOXES;
		collisionBoundingBoxes->CreateSRV(srvDesc);
	}

	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(float4);
		sbDesc.ByteWidth = sizeof(float4) * MAX_COLLISIONS;
		collisionInstances = eastl::make_unique<Buffer>(sbDesc, nullptr, "GrassCollision::Instances");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = MAX_COLLISIONS;
		collisionInstances->CreateSRV(srvDesc);
	}
}

bool GrassCollision::HasShaderDefine(RE::BSShader::Type shaderType)
{
	switch (shaderType) {
	case RE::BSShader::Type::Grass:
		return true;
	default:
		return false;
	}
}

void GrassCollision::Hooks::MainUpdate_QueueCollisions::thunk()
{
	func();
	globals::features::grassCollision.QueueCollisions();
}

void GrassCollision::Hooks::BSGrassShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	auto& self = globals::features::grassCollision;
	if (ORGGrassCollision::Get().IsActive()) {
		// The render graph updated the texture at EarlyPrepass: bind it and its constants for the frame's grass.
		static Util::FrameChecker frameChecker;
		if (frameChecker.IsNewFrame() && self.perFrame) {
			auto context = globals::d3d::context;
			ID3D11Buffer* buffers[1] = { self.perFrame->CB() };
			context->VSSetConstantBuffers(5, ARRAYSIZE(buffers), buffers);
			ID3D11ShaderResourceView* srvs[] = { self.CollisionSRV() };
			context->VSSetShaderResources(100, ARRAYSIZE(srvs), srvs);
		}
	} else
		self.Update(Util::GetEyePosition());
	func(This, Pass, RenderFlags);
}

void GrassCollision::ClearShaderCache()
{
	if (collisionUpdateCS)
		collisionUpdateCS->Release();
	collisionUpdateCS = nullptr;
}

ID3D11ComputeShader* GrassCollision::GetCollisionUpdateCS()
{
	if (!collisionUpdateCS) {
		logger::debug("Compiling CollisionUpdateCS");
		collisionUpdateCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\GrassCollision\\CollisionUpdateCS.hlsl", {}, "cs_5_0"));
	}
	return collisionUpdateCS;
}

void GrassCollision::UpdateCollisionTexture()
{
	auto context = globals::d3d::context;

	if (!settings.EnableGrassCollision) {
		float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		context->ClearUnorderedAccessViewFloat(collisionTexture->uav.get(), clearColor);
		return;
	}

	{
		ID3D11Buffer* buffers[1] = { *globals::game::perFrame };
		context->CSSetConstantBuffers(12, 1, buffers);
	}

	{
		ID3D11Buffer* buffers[1] = { perFrame->CB() };
		context->CSSetConstantBuffers(0, 1, buffers);

		ID3D11ShaderResourceView* srvs[] = {
			collisionBoundingBoxes->srv.get(),
			collisionInstances->srv.get(),
		};

		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11UnorderedAccessView* uavs[] = { collisionTexture->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(GetCollisionUpdateCS(), nullptr, 0);
		globals::profiler->BeginPass("GrassCollision::CollisionUpdate");
		context->Dispatch(512 / 8, 512 / 8, 1);
		globals::profiler->EndPass();
	}

	context->CSSetShader(nullptr, nullptr, 0);

	ID3D11Buffer* null_buffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &null_buffer);

	ID3D11UnorderedAccessView* null_uavs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, null_uavs, nullptr);
}
#undef I18N_KEY_PREFIX
