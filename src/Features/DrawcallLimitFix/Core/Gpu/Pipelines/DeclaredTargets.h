#pragma once

// The formats DCLF's pipelines are built for, read once at setup from the engine target set's declarations (cleanup plan A1,
// phase 3 design 5.1). They never change in a session, so nothing watches the bound targets each frame.

#include <dxgiformat.h>

namespace DCLF
{
	struct TargetFormats;

	namespace DeclaredTargets
	{
		/** @brief The main pass's formats: the eight targets Deferred::StartDeferred binds, and kMAIN's depth view. */
		TargetFormats MainPass();

		/** @brief The shadow pipelines' depth format: the engine shadow map's depth view, else the precipitation occlusion map's. */
		DXGI_FORMAT ShadowMap();

		/**
		* @brief Render thread, at CS's setup, after the engine target set is declared and Deferred made its targets. Hands the main pass's
		* formats to DrawPipelines once, before DCLF draws. Fatal when a format is missing.
		*/
		void Init();

		/** @brief Once the game's data has loaded, when the engine's Utility shader exists: the shadow pipelines' inputs. Fatal without it. */
		void InitShadows();
	}
}
