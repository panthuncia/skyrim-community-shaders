#pragma once

#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

namespace DCLF::Diagnostics
{
	/**
	 * @brief Read access to Coordinator's private state for the scene observers and reports. Const for observers.
	 * The only write is reports taking and resetting their counters.
	 */
	struct SceneView
	{
		using Store = Scene::Coordinator;
		using Tracked = Scene::Coordinator::Tracked;
		using Tables = SceneTables;

		static const auto& tracked(const Store& s) { return s.tracked; }
		static const Tables& tables(const Store& s) { return s.tables; }
		static const auto& mirror(const Store& s) { return s.mirror; }
		static auto& mirror(Store& s) { return s.mirror; }
		static std::uint32_t sceneFrame(const Store& s) { return s.sceneFrame; }

		// The render thread's FrameView.
		using Frame = Scene::FrameView;
		static const auto& projectedTextures() { return Frame::Get().projectedTextures; }
		static auto& geometryStats() { return Frame::Get().geometryStats; }
		static auto& pipelinesPosted() { return Frame::Get().pipelinesPosted; }
		static auto& shadingFrames() { return Frame::Get().shadingFrames; }
		static auto& captureNs() { return Frame::Get().captureNs; }
		static auto& categoryCaptureNs() { return Frame::Get().categoryCaptureNs; }
		static auto& categoryCapturesMade() { return Frame::Get().categoryCapturesMade; }
		static auto& captureFrames() { return Frame::Get().captureFrames; }
		static auto& categoryMirrorCaptures() { return Frame::Get().categoryMirrorCaptures; }
		static auto& categoryMirrorRecords() { return Frame::Get().categoryMirrorRecords; }
		static const auto& categoryCapturesHeld() { return Frame::Get().categoryCapturesHeld; }

		// Report counters, taken by the reports.
		static auto& pumpStats(Store& s) { return s.pumpStats; }
		static auto& stats(Store& s) { return s.stats; }
		static auto& residentStats(Store& s) { return s.residentStats; }
		static auto& delta(Store& s) { return s.delta; }
		static auto& materialRecordStats(Store& s) { return s.materialRecordStats; }
		static auto& materialSnapshotStats(Store& s) { return s.materialSnapshotStats; }
		static const auto& materialSnapshots(const Store& s) { return s.materialSnapshots; }
		static auto& materialBindings(Store& s) { return s.materialBindings; }
		static auto& sharedBindings(Store& s) { return s.sharedBindings; }
		static const auto& lookups(const Store& s) { return s.lookups; }
		static auto& reportedChangeCounts(Store& s) { return s.reportedChangeCounts; }
		static void componentsReportText(Store& s, Api::ReportPoint a_point, std::string& a_text) { s.ComponentsReportText(a_point, a_text); }
		static void componentsReportLines(Store& s, Api::ReportPoint a_point, std::uint32_t a_frame) { s.ComponentsReportLines(a_point, a_frame); }
		static auto& referenceStats(Store& s) { return s.referenceStats; }
		static auto& leafStats(Store& s) { return s.leafStats; }
		static auto& subtreesPended(Store& s) { return s.subtreesPended; }
		static auto& subtreesDropped(Store& s) { return s.subtreesDropped; }
		static auto& categoryChildrenMissing(Store& s) { return s.categoryChildrenMissing; }
		static const auto& categoryTaken(const Store& s) { return s.categoryTaken; }
		static const auto& alwaysRenderRoots(const Store& s) { return s.alwaysRenderRoots; }
		static auto& sunTable(Store& s) { return s.sunTable; }
		static auto& lightTable(Store& s) { return s.lightTable; }
		static auto& rootMotion(Store& s) { return s.rootMotion; }
		using SlotDecision = Scene::Coordinator::SlotDecision;
		using MirrorRead = Scene::Coordinator::MirrorRead;
		static const auto& stats(const Store& s) { return s.stats; }
		static const auto& sunTable(const Store& s) { return s.sunTable; }
		static const auto& lightTable(const Store& s) { return s.lightTable; }
		static auto& fanoutStats(Store& s) { return s.fanoutStats; }
		static std::string attachLine(Store& s) { return s.TakeAttachLine(); }
		static auto& placementStats(Store& s) { return s.placementStats; }
		static auto& shadingStats(Store& s) { return s.shadingStats; }
		static auto& mirrorReads(Store& s) { return s.mirrorReads; }
		static constexpr const auto& kMirrorReadNames = Scene::Coordinator::kMirrorReadNames;

		static const auto& mirrorNodes(const SceneMirror& m) { return m.nodes; }
		static const auto& mirrorGeometries(const SceneMirror& m) { return m.geometries; }
		static const auto& mirrorProperties(const SceneMirror& m) { return m.properties; }
		static const auto& mirrorAlphas(const SceneMirror& m) { return m.alphas; }
		/** @brief Whether the mirror holds a node, property or alpha record of a_key. */
		static bool mirrorHolds(const SceneMirror& m, const void* a_key) { return m.nodes.contains(a_key) || m.properties.contains(a_key) || m.alphas.contains(a_key); }
		static auto& mirrorApplied(SceneMirror& m) { return m.applied; }
		static auto& mirrorDetached(SceneMirror& m) { return m.detached; }
		static auto& mirrorUpdates(SceneMirror& m) { return m.updates; }
		static auto& mirrorUpdatesUnheld(SceneMirror& m) { return m.updatesUnheld; }
		static auto& mirrorLightEvictions(SceneMirror& m) { return m.lightEvictions; }
		static auto& mirrorEvictions(SceneMirror& m) { return m.evictions; }
		static auto& mirrorEvictedRecords(SceneMirror& m) { return m.evictedRecords; }
		static auto& mirrorFirstEviction(SceneMirror& m) { return m.firstEviction; }
		static auto& mirrorReplayed(SceneMirror& m) { return m.replayed; }
		static auto& mirrorStale(SceneMirror& m) { return m.stale; }
		static auto& mirrorSuperseded(SceneMirror& m) { return m.superseded; }
		static auto& mirrorUpdated(SceneMirror& m) { return m.updated; }
	};
}
