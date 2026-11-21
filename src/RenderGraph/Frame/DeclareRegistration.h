#pragma once

#include "RenderGraph/FrameAnchors.h"

#include <source_location>

/**
* @brief The anchors' registration, private to Declare. Features register through Frame::DeclareClient, so a registration and
* the stage builds that hold it switch in at one Begin. Only Declare.cpp includes this.
*/
namespace FrameAnchors
{
	/** @brief Registers or replaces a client. Render thread. Returns false, logged, on a bad desc or an inactive runtime.
	*  The shared constants are made at setup, so a client that reads them never waits. Without them it fails (FailClients). */
	bool Register(ClientDesc a_client, const std::source_location& a_where = std::source_location::current());
}
