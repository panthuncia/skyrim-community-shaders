// Native's water work as start steps: the displacement simulation and the flow copies the game side planned (Native/WaterEffects.h),
// staged with CS's rows and no engine call. Included by ImageSpace.cpp after NativeWorld.inl.

namespace NativeStart
{
	using Step = Native::WaterEffects::DisplacementStep;

	Effects::ImageSpaceEffectEnum EffectOf(Step::Kind a_kind)
	{
		switch (a_kind) {
		case Step::Kind::TexOffset:
			return Effects::ISWaterDisplacementTexOffset;
		case Step::Kind::Ripple:
			return Effects::ISWaterDisplacementWadingRipple;
		default:
			return Effects::ISWaterWadingHeightmap;
		}
	}

	/**
	* @brief A step's rows: the effect's PS rows with every id the param holds, in id order, as BuildRows copies the param; a ripple's two
	* vertex rows (TranslationMatrix0 and 1 at c1 and c2, ISWaterDisplacement.hlsl), which its vertex stage reads from the same rows.
	*/
	NativeSteps::Rows RowsOf(const Step& a_step, std::size_t a_effect)
	{
		if (a_step.kind == Step::Kind::Ripple) {
			auto rows = NativePost::ExplicitRows(3);
			for (std::uint32_t id = 0; id < 2; ++id)
				NativePost::SetId(rows, id + 1, a_step.ids[id][0], a_step.ids[id][1], a_step.ids[id][2], a_step.ids[id][3]);
			return rows;
		}
		auto rows = NativePost::RowsOf(a_effect);
		for (std::uint32_t id = 0; id < a_step.count; ++id)
			NativePost::SetId(rows, id, a_step.ids[id][0], a_step.ids[id][1], a_step.ids[id][2], a_step.ids[id][3]);
		return rows;
	}

	/** @brief Arms the native step for a_effect with a_rows. */
	void Arm(std::size_t a_effect, const NativeSteps::Rows& a_rows, bool a_compute)
	{
		auto& native = NativeSteps::Get();
		native.rows = a_rows;
		native.rowsAt = {};
		native.sprite = {};
		native.effect = a_effect;
		native.shader = ShaderOf(a_effect);
		native.computeRows = a_compute;
		native.armed = true;
	}

	void Disarm()
	{
		auto& native = NativeSteps::Get();
		native.armed = false;
		native.computeRows = false;
		native.flowSource = nullptr;
	}

	/** @brief The displacement steps in order: the restart's clear, the texture offset, the ripples, the heightmap steps. */
	void StageDisplacement(const Native::WaterEffects::Displacement& a_displacement)
	{
		auto& start = Start();
		EnterScope(ScopeKind::Water);
		start.scope = WorldScope::WaterDisplacement;
		start.target = a_displacement.target;
		start.swapped = false;
		start.ripples = 0;
		start.heightmaps = 0;
		if (a_displacement.clear) {
			const auto effect = NativePost::EffectIndex(Effects::ISWaterDisplacementClearSimulation);
			Arm(effect, NativePost::RowsOf(effect), false);
			TakeStartStep(effect, false, {});
			Disarm();
		}
		for (const auto& step : a_displacement.steps) {
			const auto effect = NativePost::EffectIndex(EffectOf(step.kind));
			Arm(effect, RowsOf(step, effect), false);
			if (step.kind == Step::Kind::Ripple) {
				if (const char* why = NativeSteps::BuildShape(a_displacement.rippleGeometry, NativeSteps::Get().sprite)) {
					Disarm();
					Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, Frame::SegmentId::Start, fmt::format("a native water ripple has no quad: {}", why));
					continue;
				}
			}
			TakeStartStep(effect, false, {});
			Disarm();
		}
		start.scope = WorldScope::None;
		LeaveScope();
	}

	/** @brief The flow copies (ISCopySubRegionCS): the CS b0 block is the region, one group per 8x8 pixels. */
	void StageFlowCopies(const std::vector<Native::WaterEffects::FlowCopy>& a_copies)
	{
		auto& start = Start();
		EnterScope(ScopeKind::FlowMap);
		start.scope = WorldScope::FlowCopy;
		const auto effect = NativePost::EffectIndex(Effects::ISCopySubRegionCS);
		for (const auto& copy : a_copies) {
			auto rows = NativePost::ExplicitRows(2);
			for (std::uint32_t i = 0; i < 8; ++i)
				NativePost::SetBits(rows, i / 4, i % 4, copy.region[i]);
			Arm(effect, rows, true);
			NativeSteps::Get().flowSource = copy.source.get();
			TakeStartStep(effect, true, { (copy.region[2] + 7) >> 3, (copy.region[3] + 7) >> 3, 1 });
			Disarm();
		}
		start.scope = WorldScope::None;
		LeaveScope();
	}

	void Stage(const Native::WaterEffects::Work& a_work)
	{
		ZoneScopedN("CS.PostNative.Water");
		if (!Usable() || !IS().on || !Start().group.Registered() || !FrameAnchors::IsRenderOwner()) {
			static Util::ReasonLog log("[ORG native] water effects: the water work is not staged");
			log.Report(!IS().on ? "the ImageSpace steps are off" : !Start().group.Registered() ? "the start steps are not registered" :
																								 "not the render owner");
			return;
		}
		// The water update reads the displacement pair, the flow map and the timer the shape holds. Their change is an event.
		const auto raised = RaisedShape();
		auto water = raised;
		WaterShapeNow(water);
		if (!(water == raised))
			RaiseShape("ImageSpace: the water update's textures or timer changed");
		if (a_work.displacement.clear || !a_work.displacement.steps.empty())
			StageDisplacement(a_work.displacement);
		if (!a_work.copies.empty())
			StageFlowCopies(a_work.copies);
	}
}
