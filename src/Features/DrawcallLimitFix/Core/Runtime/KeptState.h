#pragma once

#include <ORGModuleServices/Async/KeptState.h>

// DCLF's persistent draw state is built from OMS's kept structures (org::async, KeptState.h), described in
// dclf-architecture.md, "Persistent draw state". Storage reuse is org::async::keptStorageOptions.reuseStorage.
namespace DCLF
{
	using org::async::ChangeJournal;
	using org::async::EventLog;
	using org::async::KeptArray;
	using org::async::KeptChunkStore;
	using org::async::KeptCopyStats;
	using org::async::KeptSharing;
	using org::async::KeptView;
	using org::async::KeptWholeArray;
	using org::async::LogCursor;
	using org::async::MarkedList;
	using org::async::SharedEventLog;
}
