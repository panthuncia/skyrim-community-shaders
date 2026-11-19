#include "RenderGraph/Engine/Tripwire/GuestDevice.h"

#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Switches.h"

#include <atomic>

namespace RenderGraph::Engine::Tripwire
{
	namespace
	{
		/** @brief The installed handles. Never released, since the engine uses them until the process ends. */
		struct Guest
		{
			std::atomic<ID3D11Device*> device{ nullptr };
			std::atomic<ID3D11DeviceContext*> context{ nullptr };
		};

		Guest& GetGuest()
		{
			static Guest guest;
			return guest;
		}
	}

	bool GuestRequested()
	{
		static const bool requested = Native::GraphMode() && !Native::Requested();
		return requested;
	}

	void InstallGuestDevice()
	{
		auto& guest = GetGuest();
		if (!GuestRequested() || guest.context.load(std::memory_order_acquire))
			return;
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer) {
			logger::warn("[ORG] tripwire guest: no renderer yet, nothing installed");
			return;
		}
		auto& data = renderer->GetRuntimeData();
		auto* device = reinterpret_cast<ID3D11Device*>(data.forwarder);
		auto* context = reinterpret_cast<ID3D11DeviceContext*>(data.context);
		// Device creation (SE 75589 / AE 77396) writes both globals once, from the same objects as RendererData's fields.
		REL::Relocation<ID3D11Device**> deviceGlobal{ REL::RelocationID(524729, 411348) };
		REL::Relocation<ID3D11DeviceContext**> contextGlobal{ REL::RelocationID(524771, 411391) };
		if (!device || !context || *deviceGlobal != device || *contextGlobal != context) {
			logger::warn("[ORG] tripwire guest: the engine's device and context pointers do not match (device {} / {}, context {} / {}), nothing installed",
				fmt::ptr(device), fmt::ptr(*deviceGlobal), fmt::ptr(context), fmt::ptr(*contextGlobal));
			return;
		}
		if (IsHandle(device) || IsHandle(context)) {
			logger::warn("[ORG] tripwire guest: the engine's device or context is a handle already, nothing installed");
			return;
		}

		// The engine's reference moves to the handle, and the extra reference here is never dropped.
		// So an engine Release at shutdown frees nothing still in use, and DXVK's objects outlive the handles.
		auto guestDevice = CreateDevice({ .name = "Engine::Device" }, device);
		auto guestContext = CreateDeviceContext({ .name = "Engine::ImmediateContext" }, context);
		PairDeviceAndContext(guestDevice.get(), guestContext.get());
		guestDevice->AddRef();
		guestContext->AddRef();

		*deviceGlobal = guestDevice.get();
		*contextGlobal = guestContext.get();
		data.forwarder = reinterpret_cast<REX::W32::ID3D11Device*>(guestDevice.get());
		data.context = reinterpret_cast<REX::W32::ID3D11DeviceContext*>(guestContext.get());

		guest.device.store(guestDevice.detach(), std::memory_order_release);
		guest.context.store(guestContext.detach(), std::memory_order_release);
		logger::info(
			"[ORG] tripwire guest: the engine's device and immediate context are wrapping handles (device global, context global, "
			"RendererData +0x38/+0x40); CS keeps DXVK's own ({} / {})",
			fmt::ptr(device), fmt::ptr(context));
	}

	void InstallNativeDevice(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
		auto& guest = GetGuest();
		if (!a_device || !a_context || guest.context.load(std::memory_order_acquire))
			return;
		// The reference taken here is never dropped, as with the guest's.
		a_device->AddRef();
		a_context->AddRef();
		guest.device.store(a_device, std::memory_order_release);
		guest.context.store(a_context, std::memory_order_release);
		logger::info("[ORG] tripwire: the engine's device and immediate context are native handles ({} / {})", fmt::ptr(a_device), fmt::ptr(a_context));
	}

	bool GuestContextInstalled()
	{
		return GetGuest().context.load(std::memory_order_acquire) != nullptr;
	}
}
