#pragma once

#include "RenderGraph/Fatal.h"

#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace org
{
	class GraphHost;
}

namespace RenderGraph
{
	namespace Engine
	{
		bool GraphMode();
	}

	/**
	 * @brief The skeleton every feature port shares: the singleton, the graph-mode test, "set up", the setup prologue and the fatal
	 * helper.
	 *
	 * A port derives from Port<T>, names itself in `static constexpr std::string_view kName`, keeps its state in
	 * `std::unique_ptr<Impl> impl` (null until Setup succeeds), and declares `friend class RenderGraph::Port<T>;` so Get can
	 * make it. Outside graph mode (plain D3D11, CS_ORG=0) On() is false and impl stays
	 * null, so a port's IsActive() is false and its call sites keep the D3D11 path.
	 * Every anchor is a CS stage's, so a port needs nothing from the anchors before the graph starts.
	 */
	template <class T>
	class Port
	{
	public:
		/** @brief Graph mode: the frame runs on the render graph, so the port runs. Any thread. */
		static bool On() { return Engine::GraphMode(); }

		/** @brief The one instance. */
		static T& Get()
		{
			static T s_port;
			return s_port;
		}

		/** @brief Set up. In graph mode every setup failure is fatal, so this is false only before setup or outside graph mode. */
		bool IsActive() const { return static_cast<const T&>(*this).impl != nullptr; }

	protected:
		Port() = default;

		/**
		 * @brief Setup's prologue. Null outside graph mode or once set up, so Setup returns. Otherwise the graph's host: graph mode has
		 * no D3D11 path, so a graph that does not run by setup is fatal.
		 */
		org::GraphHost* BeginSetup() const { return On() && !IsActive() ? SetupHost() : nullptr; }

		/** @brief The graph's host, for a setup that makes the port again. Fatal when the graph does not run. */
		static org::GraphHost* SetupHost() { return &RequireGraphHost(T::kName); }

		/** @brief Ends the process for this port's failure (RenderGraph::Fatal with kName). */
		[[noreturn]] static void Fatal(std::string_view a_why) { RenderGraph::Fatal(T::kName, a_why); }
	};
}
