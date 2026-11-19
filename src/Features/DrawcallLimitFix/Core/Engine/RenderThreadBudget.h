#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace DCLF
{
	/**
	 * @brief DCLF's render-thread time per frame, by bucket. Every DCLF hook the render thread enters is a Hook, counted by site.
	 * Parts nested in it go to their bucket, the epochs are split by ORG's totals, and the rest is Other. Per-draw hooks and the hooks
	 * on job threads report their own times. Render thread only.
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

		static RenderThreadBudget& Get();

		/** @brief A DCLF hook's whole call. Nested hooks count once, to the outermost's site. */
		class Hook
		{
		public:
			explicit Hook(Site a_site);
			~Hook();
			Hook(const Hook&) = delete;
			Hook& operator=(const Hook&) = delete;

		private:
			bool counted = false;
		};

		/** @brief A part of a hook counted to a_bucket rather than Other. */
		class Part
		{
		public:
			explicit Part(Bucket a_bucket);
			~Part();
			Part(const Part&) = delete;
			Part& operator=(const Part&) = delete;

		private:
			Bucket bucket;
			bool counted = false;
			std::chrono::steady_clock::time_point start;
		};

		/** @brief One frame's total and buckets, in microseconds. */
		struct FrameTime
		{
			double totalUs = 0.0;
			std::array<double, static_cast<std::size_t>(Bucket::Count)> bucketUs{};
		};

		/** @brief At Present. The frame's total joins the report's maximum. */
		void EndFrame();

		/** @brief Render thread. The last frame EndFrame closed. */
		const FrameTime& LastFrame() const { return lastFrame; }

		/** @brief A bucket's name in the report. */
		static const char* BucketName(Bucket a_bucket);

		/** @brief The report line since the last one, or empty when no frame ended. */
		std::string Report();

	private:
		RenderThreadBudget() = default;
		bool OnRenderThread() const;

		std::uint32_t renderThread = 0;
		std::uint32_t depth = 0;
		Site site = Site::Present;
		std::chrono::steady_clock::time_point hookStart;
		double hookPartsUs = 0.0;
		double epochSubmitStart = 0.0, epochOverheadStart = 0.0;

		std::array<double, static_cast<std::size_t>(Bucket::Count)> bucketUs{};
		std::array<double, static_cast<std::size_t>(Site::Count)> siteUs{};
		std::array<std::uint32_t, static_cast<std::size_t>(Site::Count)> siteCalls{};
		double frameUs = 0.0;
		std::array<double, static_cast<std::size_t>(Bucket::Count)> frameBucketUs{};
		FrameTime lastFrame;
		double frameMaxUs = 0.0;
		double totalUs = 0.0;
		std::uint32_t frames = 0;
	};
}
