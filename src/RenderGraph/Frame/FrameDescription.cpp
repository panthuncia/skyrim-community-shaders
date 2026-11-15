#include "RenderGraph/Frame/FrameDescription.h"

#include <fmt/format.h>

namespace RenderGraph::Frame
{
	const char* KindName(Kind a_kind)
	{
		switch (a_kind) {
		case Kind::World:
			return "world";
		case Kind::FrozenWorld:
			return "frozen world";
		case Kind::MainMenu:
			return "main menu";
		case Kind::Loading:
			return "loading screen";
		case Kind::Bink:
			return "Bink movie";
		default:
			return "?";
		}
	}

	const char* SegmentName(SegmentId a_segment)
	{
		switch (a_segment) {
		case SegmentId::PreDepth:
			return "pre-depth";
		default:
			return "?";
		}
	}

	std::uint64_t FieldsHash(const SegmentFields& a_fields)
	{
		// FNV-1a over the fields, one word each.
		std::uint64_t hash = 1469598103934665603ull;
		const auto mix = [&](std::uint64_t a_word) { hash = (hash ^ a_word) * 1099511628211ull; };
		mix(a_fields.declared);
		mix(a_fields.graph);
		mix(a_fields.versions);
		mix(a_fields.clients);
		mix(a_fields.clientsOn);
		return hash ? hash : 1;
	}

	namespace
	{
		void Append(std::string& a_out, const char* a_name, const std::string& a_before, const std::string& a_after)
		{
			if (a_before == a_after)
				return;
			a_out += fmt::format("{}{} {} -> {}", a_out.empty() ? "" : ", ", a_name, a_before, a_after);
		}
	}

	std::string DiffFields(const SegmentFields& a_before, const SegmentFields& a_after)
	{
		std::string out;
		Append(out, "declared", a_before.declared ? "yes" : "no", a_after.declared ? "yes" : "no");
		Append(out, "graph", std::to_string(a_before.graph), std::to_string(a_after.graph));
		Append(out, "versions", std::to_string(a_before.versions), std::to_string(a_after.versions));
		Append(out, "clients", std::to_string(a_before.clients), std::to_string(a_after.clients));
		Append(out, "clientsOn", fmt::format("{:#x}", a_before.clientsOn), fmt::format("{:#x}", a_after.clientsOn));
		return out;
	}

	std::string DiffFrame(const Description& a_before, const Description& a_after)
	{
		std::string out;
		Append(out, "kind", KindName(a_before.kind), KindName(a_after.kind));
		Append(out, "capture", a_before.capture ? "yes" : "no", a_after.capture ? "yes" : "no");
		Append(out, "DCLF draws", a_before.dclfDraws ? "yes" : "no", a_after.dclfDraws ? "yes" : "no");
		return out;
	}
}
