#pragma once

// Actors: the geometry under an actor's 3D and the actor techniques (skin, faces, hair, eyes), drawn by DCLF while the toggle is on.
// Whatever the toggle, the feature also owns the actor-owned entry class, the actors' engine events (SetAlpha fades and move
// writers) and the animation stamp from DCLF's shadow views (ActorRate.cpp).
// Features/Skins decides which skinned geometry DCLF draws. Core keeps the bone palette path.
// See dclf-architecture.md, "Trees and actors".

#include "Features/DrawcallLimitFix/Api/Feature.h"

namespace DCLF::Features::Actors
{
	Api::Feature Feature();
	void InstallEngineEvents();
	// Must run at install, before the early hooks fire.
	void NeedFadeAmounts(Api::InstallContext& a_context);
	void RegisterActorRate(Api::InstallContext& a_context);
}
