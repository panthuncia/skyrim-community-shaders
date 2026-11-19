#pragma once

namespace DCLF::MirrorWatch
{
	/**
	 * @brief CS_DCLF_MIRROR_WATCH. Hardware write watchpoints on one object's mirrored fields, to name writers the mirror's hooks miss.
	 * One object at a time, disarmed after 16 changes or 600 frames. "1" or a node name watches a fade node's statics
	 * (+0x108, +0x128, +0x12C, +0x150). "geometry:<name>" watches alpha +0x120, property +0x128, property flags +0x3C and fade node +0x60.
	 */
	void Arm(const RE::NiAVObject* a_node);
	void ArmGeometry(const RE::BSGeometry* a_geometry);
	/**
	 * @brief Watch modes node, transform, fade, fadetype, alpha and alphas:<form ID>|... on a named node or geometry.
	 * Every recorded change carries its writer's callers.
	 */
	void ArmNode(const RE::NiAVObject* a_node);
	/** @brief Watch modes parity, parity:alpha, parity:projected and parity:land, on a property the mirror parity found stale. Render thread. */
	void ArmProperty(const void* a_property);
	/** @brief The property record fields whose staleness arms the watch, 0 when off. */
	std::uint32_t PropertyFields();
	/**
	 * @brief missed:<field>|... The record fields of a_type whose missed differences arm the watch on that field.
	 * The next write is caught, since the missed one already happened. 0 when off.
	 */
	std::uint32_t MissedFields(std::uint8_t a_type);
	void ArmMissed(const void* a_object, std::uint8_t a_type, std::uint32_t a_fields);
	/**
	 * @brief moved: a geometry found moved while not due, watching the translation x of it and its ancestors. Any thread.
	 * ArmMovedBound watches a root whose world bound changed.
	 */
	void ArmMoved(const RE::NiAVObject* a_geometry);
	void ArmMovedBound(const RE::NiAVObject* a_root);
	bool MovedFromParity();
	/** @brief Whether a watch is armed now. */
	bool Armed();
	/** @brief parity:current, a fade node stale in currentFade at +0x130 and +0x154. Render thread. */
	void ArmCurrent(const void* a_node);
	bool CurrentFromParity();
	/** @brief An alpha property the mirror parity found stale. Render thread. */
	void ArmAlpha(const void* a_alpha);
	/** @brief The report, empty when nothing, and the disarm when due. Render thread. */
	std::string TakeReport(std::uint32_t a_frame);
	bool Enabled();
}
