#include "RenderGraph/Engine/Tripwire/GuestDevice.h"

#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Switches.h"

#include <atomic>

namespace RenderGraph::Engine::Tripwire
{
	namespace
	{
		/** @brief The installed handles. Never released: the engine keeps using them until the process ends. */
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
		static const bool requested = Switches::On("CS_ORG_TRIPWIRE_GUEST");
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
		// Both globals are written once by the device creation (SE 75589 / AE 77396), from the same objects as RendererData's fields.
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

		// The handles take their own references to DXVK's objects. The engine's reference on each moves to the handle it now holds
		// (one each: the globals and RendererData's fields are copies of one pointer), and the extra reference kept here is never
		// dropped. So an engine Release at shutdown frees nothing that is still used, and DXVK's objects outlive the handles.
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
		// The engine holds its own references; the ones taken here are never dropped, as the guest's are not.
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

	ID3D11Device* GuestDeviceHandle()
	{
		return GetGuest().device.load(std::memory_order_acquire);
	}

	ID3D11DeviceContext* GuestContextHandle()
	{
		return GetGuest().context.load(std::memory_order_acquire);
	}
}
