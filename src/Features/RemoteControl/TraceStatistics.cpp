#include "Features/RemoteControl/TraceStatistics.h"

#include <BasicTelemetry/Sampling.h>

#include <algorithm>
#include <cmath>

namespace GpuCounters::Trace::Statistics
{
	Config ClampConfig(Config a_config)
	{
		if (!(a_config.confidence > 0.0 && a_config.confidence < 1.0))
			a_config.confidence = 0.95;
		a_config.relativeHalfWidth = std::max(0.0, a_config.relativeHalfWidth);
		a_config.absoluteHalfWidth = std::max(0.0, a_config.absoluteHalfWidth);
		a_config.minSamples = std::max<std::uint32_t>(2, a_config.minSamples);
		a_config.maxSamples = std::max(a_config.minSamples, a_config.maxSamples);
		return a_config;
	}

	Summary Summarize(const std::vector<double>& a_samples, const Config& a_config)
	{
		Summary out;
		out.samples = static_cast<std::uint32_t>(a_samples.size());
		if (a_samples.empty())
			return out;
		const auto config = ClampConfig(a_config);
		basic_telemetry::SamplingConfig sampling;
		sampling.minimumSamples = config.minSamples;
		// The series is the caller's to bound: every sample counts (the sampler would keep only the first maximumSamples).
		sampling.maximumSamples = std::max<std::uint32_t>(config.minSamples, static_cast<std::uint32_t>(a_samples.size()));
		sampling.confidenceLevel = config.confidence;
		sampling.relativeHalfWidth = config.relativeHalfWidth;
		sampling.absoluteHalfWidth = config.absoluteHalfWidth;
		basic_telemetry::StatisticalSampler sampler(sampling);
		for (double value : a_samples)
			if (std::isfinite(value))
				sampler.AddSample(value);
		const auto summary = sampler.Summarize();
		out.samples = summary.rawSampleCount;
		out.effectiveSamples = summary.effectiveSampleCount;
		out.mean = summary.mean;
		out.median = summary.median;
		out.p05 = summary.p05;
		out.p95 = summary.p95;
		out.halfWidth = 0.5 * (summary.confidenceHigh - summary.confidenceLow);
		out.relativeHalfWidth = summary.relativeHalfWidth;
		out.lag1Autocorrelation = summary.lag1Autocorrelation;
		out.madOutliers = summary.madOutlierCount;
		out.converged = summary.converged;
		return out;
	}

	bool Significant(double a_mean, double a_halfWidth, double a_baseMean, double a_baseHalfWidth)
	{
		if (!std::isfinite(a_halfWidth) || !std::isfinite(a_baseHalfWidth))
			return false;
		return std::abs(a_mean - a_baseMean) > std::sqrt(a_halfWidth * a_halfWidth + a_baseHalfWidth * a_baseHalfWidth);
	}
}
