#pragma once

// Fades' per-file registrations and settings, for the feature's own sources only.
// The install calls the registrations in a fixed order, with the writers last.

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Fades
{
	void RegisterScene(Api::InstallContext& a_context);
	void RegisterRender(Api::InstallContext& a_context);
	// Snaps register before amounts.
	void RegisterEngine(Api::InstallContext& a_context, Api::EngineChannelId& a_snaps, Api::EngineChannelId& a_amounts);
	// Must be last, to keep the order of work at Main::Update's entry.
	void RegisterWriters(Api::InstallContext& a_context);
	// The writer hooks do not chain to other plugins' hooks at those sites, so each site must still hold the engine's own code.
	void PreflightWriters();
	void InstallEarlyHooks();
	/**
	 * @brief Render thread. Rereads a newly seeded actor root's fadeAmount, because the listing may have caught it mid-ramp after a cell load.
	 * Without this the actor stays translucent until it is listed again.
	 */
	void RecheckFadeAmount(RE::NiAVObject* a_node);
	/** @brief Scene lane. True once per recheck of a_node. Only the census uses the answer. */
	bool TakeRecheck(const void* a_node);
	void RegisterDiagnostics(Api::InstallContext& a_context);

	// Settings. Readable from any thread after install.
	/** @brief CS_DCLF_FADE_PARITY. Checks the port against the engine and FadeStateCS against the port. */
	bool FadeParity();
	/** @brief CS_DCLF_EMPTY_FADE_NODE, on by default. The engine's shared empty fade node is not treated as a fade root. */
	bool EmptyFadeNodeIsNone();
}
