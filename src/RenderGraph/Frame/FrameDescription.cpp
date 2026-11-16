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
		case SegmentId::Start:
			return "start";
		case SegmentId::PreDepth:
			return "pre-depth";
		case SegmentId::PreMain:
			return "pre-main";
		case SegmentId::Deferred:
			return "deferred+water";
		case SegmentId::Post:
			return "post";
		case SegmentId::Ui:
			return "UI";
		case SegmentId::MenuUi:
			return "menu UI";
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
		for (std::size_t i = 0; i < a_fields.instances.size(); i += 8) {
			std::uint64_t word = 0;
			for (std::size_t k = 0; k < 8; ++k)
				word |= static_cast<std::uint64_t>(a_fields.instances[i + k]) << (8 * k);
			mix(word);
		}
		mix(a_fields.foreignOn);
		mix(a_fields.passes);
		for (const auto word : a_fields.rows)
			mix(word);
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
		const auto instances = [](const SegmentFields& a_fields) {
			std::string text;
			for (std::size_t i = 0; i < a_fields.instances.size(); ++i)
				if (a_fields.instances[i])
					text += fmt::format("{}{}:{}", text.empty() ? "" : " ", i, a_fields.instances[i]);
			return text.empty() ? std::string("none") : text;
		};
		Append(out, "instances", instances(a_before), instances(a_after));
		Append(out, "foreignOn", fmt::format("{:#x}", a_before.foreignOn), fmt::format("{:#x}", a_after.foreignOn));
		Append(out, "passes", fmt::format("{:#x}", a_before.passes), fmt::format("{:#x}", a_after.passes));
		const auto rows = [](const SegmentFields& a_fields) {
			return fmt::format("{:016x}{:016x}{:016x}{:016x}", a_fields.rows[3], a_fields.rows[2], a_fields.rows[1], a_fields.rows[0]);
		};
		Append(out, "rows", rows(a_before), rows(a_after));
		return out;
	}

	std::string DiffWorld(const WorldFields& a_before, const WorldFields& a_after, bool a_parity)
	{
		std::string out;
		const auto flag = [&](const char* a_name, bool a_old, bool a_new) { Append(out, a_name, a_old ? "yes" : "no", a_new ? "yes" : "no"); };
		const auto number = [&](const char* a_name, std::uint32_t a_old, std::uint32_t a_new) { Append(out, a_name, std::to_string(a_old), std::to_string(a_new)); };
		flag("read", a_before.read, a_after.read);
		number("sao levels", a_before.saoLevels, a_after.saoLevels);
		number("sao contrast level", a_before.saoContrast, a_after.saoContrast);
		flag("sao temporal", a_before.saoTemporal, a_after.saoTemporal);
		flag("sao half", a_before.saoHalf, a_after.saoHalf);
		number("sao form", a_before.saoForm, a_after.saoForm);
		flag("ssr", a_before.ssr, a_after.ssr);
		number("ssr downsample", a_before.ssrDownsample, a_after.ssrDownsample);
		flag("ssr blur", a_before.ssrBlur, a_after.ssrBlur);
		flag("snow", a_before.snow, a_after.snow);
		if (a_parity) {
			number("sao parity", a_before.saoParity, a_after.saoParity);
			number("normals", a_before.normals, a_after.normals);
			number("water history", a_before.waterHistory, a_after.waterHistory);
		}
		return out;
	}

	std::string DiffPost(const PostFields& a_before, const PostFields& a_after)
	{
		std::string out;
		const auto flag = [&](const char* a_name, bool a_old, bool a_new) { Append(out, a_name, a_old ? "yes" : "no", a_new ? "yes" : "no"); };
		const auto number = [&](const char* a_name, std::uint32_t a_old, std::uint32_t a_new) { Append(out, a_name, std::to_string(a_old), std::to_string(a_new)); };
		flag("read", a_before.read, a_after.read);
		flag("volumetric", a_before.volumetric, a_after.volumetric);
		flag("sil", a_before.sil, a_after.sil);
		flag("lens flares", a_before.lensFlares, a_after.lensFlares);
		flag("eof", a_before.eof, a_after.eof);
		flag("ldr", a_before.ldr, a_after.ldr);
		Append(out, "slots", fmt::format("{:#x}", a_before.slots), fmt::format("{:#x}", a_after.slots));
		flag("hdr", a_before.hdr, a_after.hdr);
		flag("effects11", a_before.effects11, a_after.effects11);
		number("bloom", a_before.bloom, a_after.bloom);
		flag("fade", a_before.fade, a_after.fade);
		flag("adapt", a_before.adapt, a_after.adapt);
		number("luminance levels", a_before.lumLevels, a_after.lumLevels);
		flag("taa", a_before.taa, a_after.taa);
		number("taa form", a_before.taaMode, a_after.taaMode);
		flag("taa water", a_before.taaWater, a_after.taaWater);
		flag("dynamic resolution", a_before.dynres, a_after.dynres);
		flag("dof compute", a_before.dofCompute, a_after.dofCompute);
		flag("screenshot", a_before.screenshot, a_after.screenshot);
		flag("upscaler", a_before.upscaler, a_after.upscaler);
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
