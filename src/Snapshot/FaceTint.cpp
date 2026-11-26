#include "Snapshot/FaceTint.h"

#include "Globals.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/FrameClock.h"
#include "Snapshot/Publication.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>

namespace Snapshot::FaceTint
{
	namespace
	{
		namespace Tripwire = RenderGraph::Engine::Tripwire;

		// The job AE 27040 / SE 26454 makes (0x288 bytes): sixteen ImageSpaceTextures (the masks), the ImageSpaceShaderParam at +0x200
		// (ISAlphaBlend's colours), the head's NiSourceTexture at +0x280.
		constexpr std::size_t kJobMasks = 16, kJobParam = 0x200, kJobHead = 0x280;

		bool installed = false;
		std::atomic<std::uint64_t> g_done{ 0 };  // the newest serial the render side copied, read by the capture

		struct Game
		{
			std::vector<std::shared_ptr<Bake>> pending;  // serial order
			std::uint64_t serial = 0;
		};
		Game& GameSide()
		{
			static Game game;
			return game;
		}

		struct Render
		{
			std::uint64_t staged = 0, stagedFrame = 0, done = 0;
		};
		Render& RenderSide()
		{
			static Render render;
			return render;
		}

		struct Stats
		{
			std::atomic<std::uint64_t> queued{ 0 }, handed{ 0 }, drawn{ 0 }, copied{ 0 }, waits{ 0 };
		};
		Stats stats;

		using Texture = RE::NiTexture::RendererData;

		Texture* CreateTexture(std::uint32_t a_width, std::uint32_t a_height)
		{
			using Create = Texture*(RE::BSGraphics::Renderer*, std::uint32_t, std::uint32_t, const void*, D3D11_USAGE, DXGI_FORMAT, bool);
			static REL::Relocation<Create> create{ REL::RelocationID(75511, 77303) };
			return create(globals::game::renderer, a_width, a_height, nullptr, D3D11_USAGE_DEFAULT, DXGI_FORMAT_R8G8B8A8_UNORM, false);
		}

		void ReleaseTexture(void* a_texture)
		{
			using Release = void(RE::BSGraphics::Renderer*, void*);
			static REL::Relocation<Release> release{ REL::RelocationID(75527, 77322) };
			release(globals::game::renderer, a_texture);
		}

		/** @brief Frees a job as UpdatePending does: the head's reference, the param, the masks, then the job's memory. */
		void FreeJob(std::byte* a_job)
		{
			reinterpret_cast<RE::NiPointer<RE::NiSourceTexture>*>(a_job + kJobHead)->reset();
			using ParamDestructor = void(void*);
			static REL::Relocation<ParamDestructor> destroyParam{ REL::RelocationID(100194, 106900) };
			destroyParam(a_job + kJobParam);
			auto* masks = reinterpret_cast<RE::ImageSpaceTexture*>(a_job);
			for (std::size_t i = 0; i < kJobMasks; ++i)
				std::destroy_at(masks + i);
			RE::free(a_job);
		}

		/** @brief A job's bake: its masks, its rows, the head and a new texture for the result, as UpdatePending's copy makes it. */
		std::shared_ptr<Bake> MakeBake(std::byte* a_job)
		{
			auto* head = reinterpret_cast<RE::NiPointer<RE::NiSourceTexture>*>(a_job + kJobHead)->get();
			// UpdatePending draws a job with no head and keeps nothing of it.
			if (!head)
				return nullptr;
			auto bake = std::make_shared<Bake>();
			bake->serial = ++GameSide().serial;
			bake->head = EngineRef(head);
			const auto* masks = reinterpret_cast<const RE::ImageSpaceTexture*>(a_job);
			for (std::size_t i = 0; i < kJobMasks; ++i) {
				const auto& mask = masks[i];
				// A slot naming a render target or a depth texture reads the null texture, as the step's own staging does.
				if (!mask.texture || static_cast<std::int32_t>(mask.renderTarget) >= 0 || static_cast<std::int32_t>(mask.unk0C) >= 0)
					continue;
				bake->masks[i] = EngineRef(mask.texture.get());
				const auto* data = mask.texture->rendererTexture;
				bake->views[i] = data ? Tripwire::Unwrap(data->resourceView) : nullptr;
			}
			if (const char* why = RenderGraph::Engine::ImageSpace::BuildTintRows(a_job + kJobParam, bake->rows))
				RenderGraph::Fatal("face tint", fmt::format("a face tint job's rows: {}", why));
			// The manager is the object at this address (CommonLib's GetSingleton reads it as a pointer).
			static REL::Relocation<const RE::BSGraphics::RenderTargetManager*> managerAt{ REL::RelocationID(524970, 411451) };
			const auto& properties = managerAt->renderTargetData[RE::RENDER_TARGET::kPLAYER_FACEGEN_TINT];
			auto* source = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kPLAYER_FACEGEN_TINT].texture;
			auto* texture = CreateTexture(properties.width, properties.height);
			if (!source || !texture || !texture->texture || !texture->resourceView)
				RenderGraph::Fatal("face tint", fmt::format("the face tint's texture ({}x{}) or kPLAYER_FACEGEN_TINT is missing", properties.width, properties.height));
			bake->texture = texture;
			bake->destination = Tripwire::Unwrap(texture->texture);
			bake->source = Tripwire::Unwrap(source);
			return bake;
		}

		/** @brief Hands a done bake's texture to its head, releasing the head's old one, as UpdatePending does after its copy. */
		void HandOver(Bake& a_bake)
		{
			auto* head = reinterpret_cast<RE::NiSourceTexture*>(a_bake.head.Key());
			if (auto* old = std::exchange(head->rendererTexture, static_cast<RE::BSGraphics::Texture*>(a_bake.texture)))
				ReleaseTexture(old);
			a_bake.texture = nullptr;
			stats.handed.fetch_add(1, std::memory_order_relaxed);
		}

		/** @brief The game half, in every capture: done bakes go to their heads, then the engine's queue becomes bakes. */
		void CaptureBlock(Builder& a_builder)
		{
			auto& game = GameSide();
			const auto done = g_done.load(std::memory_order_acquire);
			std::size_t finished = 0;
			for (; finished < game.pending.size() && game.pending[finished]->serial <= done; ++finished) {
				HandOver(*game.pending[finished]);
				logger::info("[Snapshot] face tint bake {} handed to its head", game.pending[finished]->serial);
			}
			game.pending.erase(game.pending.begin(), game.pending.begin() + static_cast<std::ptrdiff_t>(finished));

			// The queue's lock is the one AE 27040 and UpdatePending take; the queue may be filled from loader threads.
			static REL::Relocation<RE::BSSpinLock*> lock{ REL::RelocationID(515577, 401752) };
			static REL::Relocation<RE::BSTArray<std::byte*>*> queue{ REL::RelocationID(515574, 401749) };
			lock->Lock();
			for (auto* job : *queue) {
				if (!job)
					continue;
				if (auto bake = MakeBake(job)) {
					logger::info("[Snapshot] face tint bake {} queued", bake->serial);
					game.pending.push_back(std::move(bake));
					stats.queued.fetch_add(1, std::memory_order_relaxed);
				}
				FreeJob(job);
			}
			queue->clear();
			lock->Unlock();

			Block block;
			block.bakes.assign(game.pending.begin(), game.pending.end());
			a_builder.Set(block);
		}

		/** @brief Turns a CALL to the engine's UpdatePending into a five-byte NOP, after checking it is the engine's own call. */
		void RemoveCall(const char* a_what, std::uintptr_t a_site)
		{
			const auto target = REL::RelocationID(26455, 27041).address();
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_site);
			std::int32_t relative = 0;
			std::memcpy(&relative, bytes + 1, sizeof(relative));
			if (bytes[0] != 0xE8 || a_site + 5 + relative != target)
				RenderGraph::Fatal("face tint", fmt::format("{} at {:#x} is not the engine's call; another plugin may have patched it", a_what, a_site));
			REL::safe_write(a_site, REL::NOP5, sizeof(REL::NOP5));
		}
	}

	Bake::~Bake()
	{
		// A bake never handed over still owns its texture.
		if (texture)
			ReleaseTexture(texture);
	}

	void InstallHooks()
	{
		if (REL::Module::IsVR() || !RenderGraph::Engine::Native::Requested())
			return;
		RemoveCall("Main::DrawFrame's FaceGen::UpdatePending", REL::RelocationID(35556, 36555).address() + REL::Relocate(0x15D, 0x162));
		RemoveCall("the loading loop's FaceGen::UpdatePending", REL::RelocationID(38085, 39039).address() + 0x19F);
		Snapshot::RegisterCapture(&CaptureBlock);
		installed = true;
		logger::info("[Snapshot] face tint bakes: the queue is drained in the capture and drawn by CS at world frames; the engine's UpdatePending calls are off (native)");
	}

	void RenderFrame()
	{
		if (!installed || RenderGraph::Frame::Driver::Current().kind != RenderGraph::Frame::Kind::World)
			return;
		const auto& snapshot = Snapshot::Current();
		const auto* block = snapshot ? snapshot->Block<Block>() : nullptr;
		auto& render = RenderSide();
		const auto frame = RenderGraph::FrameClock::BeginFrame();
		const auto find = [&](std::uint64_t a_serial) -> const Bake* {
			if (block)
				for (const auto& bake : block->bakes)
					if (bake->serial == a_serial)
						return bake.get();
			return nullptr;
		};
		// The bake drawn in an earlier frame: that frame's start stage is submitted, so the copy reads what it drew.
		if (render.staged && frame > render.stagedFrame) {
			if (const auto* bake = find(render.staged)) {
				globals::d3d::context->CopyResource(bake->destination, bake->source);
				stats.copied.fetch_add(1, std::memory_order_relaxed);
			}
			render.done = std::exchange(render.staged, 0);
			g_done.store(render.done, std::memory_order_release);
		}
		if (render.staged || !block)
			return;
		// One bake a frame: they share kPLAYER_FACEGEN_TINT.
		for (const auto& bake : block->bakes) {
			if (bake->serial <= render.done)
				continue;
			if (RenderGraph::Engine::ImageSpace::StageFaceTint(bake->views, bake->rows)) {
				render.staged = bake->serial;
				render.stagedFrame = frame;
				stats.drawn.fetch_add(1, std::memory_order_relaxed);
			} else {
				stats.waits.fetch_add(1, std::memory_order_relaxed);
			}
			break;
		}
	}

	std::string TakeStatsLine()
	{
		if (!installed)
			return {};
		return fmt::format("face tint bakes queued {}, drawn {}, copied {}, handed to heads {} ({} frames waited for the step)", stats.queued.exchange(0),
			stats.drawn.exchange(0), stats.copied.exchange(0), stats.handed.exchange(0), stats.waits.exchange(0));
	}
}
