// The scene diagnostics state, one instance.

#include "SceneState.h"

namespace DCLF::Diagnostics
{
	SceneState& SceneDiagnostics()
	{
		static SceneState state;
		return state;
	}
}
