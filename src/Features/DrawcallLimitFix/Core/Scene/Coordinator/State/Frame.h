#pragma once

// The frame inputs: the newest inputs taken, the coordinator's frame and pass count, and the technique rows' witness.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorFrame
	{
		std::shared_ptr<const GeometryPort::PipelineFrame> scenePipelineFrame;  // the scene lane's: the newest taken
		FrameInputs sceneInputs;              // the newest taken
		std::uint32_t verdictsApplied = 0;    // the verdicts generation last applied (InvalidateVerdicts)
		// Lags the render thread's frame.
		std::uint32_t sceneFrame = 0;
		std::uint64_t passSerial = 0;
		std::shared_ptr<const Extensions::AncestorValueMap> ancestorMap;  // the ancestor value's map, the newest taken (Light Limit Fix's room map)
		std::uint64_t ancestorMapGeneration = ~0ull;
		Channels& channels = Channels::Get();
		// Fog is left out because a row keeps its own fog.
		struct TechniqueWitness
		{
			TechniqueInputs inputs{};
			std::uint8_t byte12 = 0, byte7 = 0;
			bool valid = false;
		} techniqueWitness;
	};
}
