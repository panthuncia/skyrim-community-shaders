#pragma once

#include <BasicTelemetry/ThreadBudget.h>

#include <cstdint>
#include <string>

namespace DCLF
{
	/**
	 * @brief DCLF's render-thread time per frame, by bucket. Every DCLF hook the render thread enters is a Hook, counted by site.
	 * Parts nested in it go to their bucket, the epochs are split by ORG's totals, and the rest is Other. Per-draw hooks and the hooks
	 * on job threads report their own times. Render thread only. The counting is BasicTelemetry's ThreadBudget.
	 */
	class RenderThreadBudget
	{
	public:
		enum class Bucket : std::uint8_t
		{
			Exchange,  // the frame start's exchange with the scene
			SceneCaptures,  // the frame start's engine captures for the scene
			Capture,  // copying state only the render thread can read
			Submit,  // ORG's submission proper
			OrgOverhead,  // ORG's other render-thread work in an epoch
			EngineBoundary,  // engine code DCLF calls and engine writes it applies
			Other,  // work that belongs off the render thread
			Count
		};

		enum class Site : std::uint8_t
		{
			Present,
			SceneFrame,
			BeforeShadowMaps,
			AfterShadowMaps,
			ShadowViewCapture,
			Occlusion,
			EarlyPrepass,
			Prepass,
			ZPrepass,
			BeforeOpaque,
			AfterOpaque,
			ReflectionFace,
			Count
		};

		using Budget = basic_telemetry::ThreadBudget<Bucket, Site, 2>;

		static RenderThreadBudget& Get();

		/** @brief A DCLF hook's whole call. Nested hooks count once, to the outermost's site. */
		class Hook
		{
		public:
			explicit Hook(Site a_site) :
				hook(Get().budget, a_site) {}
			Hook(const Hook&) = delete;
			Hook& operator=(const Hook&) = delete;

		private:
			Budget::Hook hook;
		};

		/** @brief A part of a hook counted to a_bucket rather than Other. */
		class Part
		{
		public:
			explicit Part(Bucket a_bucket) :
				part(Get().budget, a_bucket) {}
			Part(const Part&) = delete;
			Part& operator=(const Part&) = delete;

		private:
			Budget::Part part;
		};

		/** @brief One frame's total and buckets, in microseconds. */
		using FrameTime = Budget::FrameTime;

		/** @brief At Present. The frame's total joins the report's maximum. */
		void EndFrame() { budget.EndFrame(); }

		/** @brief Render thread. The last frame EndFrame closed. */
		const FrameTime& LastFrame() const { return budget.LastFrame(); }

		/** @brief A bucket's name in the report. */
		static const char* BucketName(Bucket a_bucket);

		/** @brief The report line since the last one, or empty when no frame ended. */
		std::string Report();

	private:
		RenderThreadBudget();

		Budget budget;
	};
}
