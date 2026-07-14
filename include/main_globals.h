/**
 * main_globals.h — Centralized extern declarations for globals defined
 * in main.cpp and module .cpp files.
 *
 * Eliminates duplicate extern declarations scattered across translation
 * units.  Each consumer includes its own type-definition headers; this
 * file only forward-declares enough for the extern statements.
 */

#pragma once

// ============================================================================
// Forward declarations (avoids pulling in full module headers)
// ============================================================================

// Loop phase modules (src/modules/system/)
class LoopConnectionEarlyModule;
class LoopSettingsPrepModule;
class LoopPreIngestModule;
class LoopIngestModule;
class LoopDisplayModule;
class LoopPostDisplayModule;
class LoopRuntimeSnapshotModule;
class LoopPowerTouchModule;
class LoopTailModule;
class LoopTelemetryModule;
class PeriodicMaintenanceModule;
class WifiRuntimeModule;
class WifiAutoStartModule;
class WifiPriorityPolicyModule;
class WifiVisualSyncModule;
class WifiProcessCadenceModule;
class ConnectionCycleCoordinatorModule;
class DisplayRestoreModule;
class VolumeFadeModule;
class SpeedMuteModule;
class AlpEventLatch;
class QualificationSerialModule;

// Core subsystems
class V1BLEClient;
class PacketParser;
class TouchHandler;
#include "display_mode.h" // enum class — cannot forward-declare
#include "main_runtime_state.h"
class AutoPushModule;
class TouchUiModule;
class TapGestureModule;
class PowerModule;
class AlertPersistenceModule;
class BleQueueModule;
class ConnectionRuntimeModule;
class ConnectionStateCadenceModule;
class ConnectionStateModule;
class ConnectionStateDispatchModule;
class SystemEventBus;
class QuietCoordinatorModule;
class VoiceModule;

// OBD and speed subsystems (defined in src/modules/obd and src/modules/speed)
class ObdRuntimeModule;
class ObdBleClient;
class ObdSettingsSyncModule;
class SpeedSourceSelector;

// ALP (Active Laser Protection) subsystem (defined in src/modules/alp)
class AlpRuntimeModule;
class AlpSdLogger;

// GPS subsystem (defined in src/modules/gps)
class GpsRuntimeModule;
class GpsTimePublisher;
class GpsGeoPublisher;

#ifndef UNIT_TEST
class DisplayPipelineModule;
class DisplayOrchestrationModule;
class DisplayPreviewModule;
#endif

// Perf monitoring (conditional)
#if defined(PERF_METRICS) && defined(PERF_MONITORING)
struct PerfLatency;
#endif

// ============================================================================
// Extern declarations — loop phase modules
// ============================================================================

extern LoopConnectionEarlyModule loopConnectionEarlyModule;
extern LoopSettingsPrepModule loopSettingsPrepModule;
extern LoopPreIngestModule loopPreIngestModule;
extern LoopIngestModule loopIngestModule;
extern LoopDisplayModule loopDisplayModule;
extern LoopPostDisplayModule loopPostDisplayModule;
extern LoopRuntimeSnapshotModule loopRuntimeSnapshotModule;
extern WifiRuntimeModule wifiRuntimeModule;
extern WifiAutoStartModule wifiAutoStartModule;
extern WifiPriorityPolicyModule wifiPriorityPolicyModule;
extern WifiVisualSyncModule wifiVisualSyncModule;
extern WifiProcessCadenceModule wifiProcessCadenceModule;
extern ConnectionCycleCoordinatorModule connectionCycleCoordinatorModule;
extern PeriodicMaintenanceModule periodicMaintenanceModule;
extern LoopTailModule loopTailModule;
extern LoopTelemetryModule loopTelemetryModule;
extern LoopPowerTouchModule loopPowerTouchModule;

// ============================================================================
// Extern declarations — core subsystem instances
// ============================================================================

extern V1BLEClient bleClient;
extern PacketParser parser;
extern TouchHandler touchHandler;
extern AutoPushModule autoPushModule;
extern TouchUiModule touchUiModule;
extern TapGestureModule tapGestureModule;
extern PowerModule powerModule;
extern AlertPersistenceModule alertPersistenceModule;
extern DisplayMode displayMode;
extern BleQueueModule bleQueueModule;
extern ConnectionRuntimeModule connectionRuntimeModule;
extern ConnectionStateCadenceModule connectionStateCadenceModule;
extern ConnectionStateModule connectionStateModule;
extern ConnectionStateDispatchModule connectionStateDispatchModule;
extern SystemEventBus systemEventBus;
extern VoiceModule voiceModule;
extern VolumeFadeModule volumeFadeModule;
extern QuietCoordinatorModule quietCoordinatorModule;
extern MainRuntimeState mainRuntimeState;
extern ObdRuntimeModule obdRuntimeModule;
extern ObdBleClient obdBleClient;
extern ObdSettingsSyncModule obdSettingsSyncModule;
extern SpeedSourceSelector speedSourceSelector;
extern AlpRuntimeModule alpRuntimeModule;
extern AlpSdLogger alpSdLogger;
extern AlpEventLatch alpEventLatch;
extern GpsRuntimeModule gpsRuntimeModule;
extern GpsTimePublisher gpsTimePublisher;
extern GpsGeoPublisher gpsGeoPublisher;
extern SpeedMuteModule speedMuteModule;
extern QualificationSerialModule qualificationSerialModule;

#ifndef UNIT_TEST
extern DisplayPipelineModule displayPipelineModule;
extern DisplayOrchestrationModule displayOrchestrationModule;
extern DisplayPreviewModule displayPreviewModule;
extern DisplayRestoreModule displayRestoreModule;
#endif

#if defined(PERF_METRICS) && defined(PERF_MONITORING)
extern PerfLatency perfLatency;
extern bool perfDebugEnabled;
#endif

// ============================================================================
// Extern function declarations
// ============================================================================

extern void configureTouchUiModule();
