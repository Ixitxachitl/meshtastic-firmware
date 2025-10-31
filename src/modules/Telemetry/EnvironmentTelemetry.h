#include "configuration.h"

#if !MESHTASTIC_EXCLUDE_ENVIRONMENTAL_SENSOR

#pragma once

#ifndef ENVIRONMENTAL_TELEMETRY_MODULE_ENABLE
#define ENVIRONMENTAL_TELEMETRY_MODULE_ENABLE 0
#endif

#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "NodeDB.h"
#include "ProtobufModule.h"
#include "detect/ScanI2CConsumer.h"
#include <OLEDDisplay.h>
#include <OLEDDisplayUi.h>

class EnvironmentTelemetryModule : private concurrency::OSThread,
                                   public ScanI2CConsumer,
                                   public ProtobufModule<meshtastic_Telemetry>
{
    CallbackObserver<EnvironmentTelemetryModule, const meshtastic::Status *> nodeStatusObserver =
        CallbackObserver<EnvironmentTelemetryModule, const meshtastic::Status *>(this,
                                                                                 &EnvironmentTelemetryModule::handleStatusUpdate);

  public:
    EnvironmentTelemetryModule()
        : concurrency::OSThread("EnvironmentTelemetry"), ScanI2CConsumer(),
          ProtobufModule("EnvironmentTelemetry", meshtastic_PortNum_TELEMETRY_APP, &meshtastic_Telemetry_msg)
    {
        lastMeasurementPacket = nullptr;
        nodeStatusObserver.observe(&nodeStatus->onNewStatus);
        setIntervalFromNow(10 * 1000);
    }
    virtual bool wantUIFrame() override;
#if !HAS_SCREEN
    void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y);
#else
    virtual void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) override;
#endif

    void setEnvDisplaySource(uint32_t nodenum) { selectedSource = nodenum; } // 0 = Auto

    void clearEnvCache()
    {
        for (auto &kv : lastBySource)
            if (kv.second)
                packetPool.release(kv.second);
        lastBySource.clear();
    }
    MeshModule *asMesh() { return this; }
    std::vector<uint32_t> getSourcesWithTelemetry() const;

  protected:
    /** Called to handle a particular incoming message
    @return true if you've guaranteed you've handled this message and no other handlers should be considered for it
    */
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Telemetry *p) override;
    virtual int32_t runOnce() override;
    /** Called to get current Environment telemetry data
    @return true if it contains valid data
    */
    bool getEnvironmentTelemetry(meshtastic_Telemetry *m);
    virtual meshtastic_MeshPacket *allocReply() override;
    /**
     * Send our Telemetry into the mesh
     */
    bool sendTelemetry(NodeNum dest = NODENUM_BROADCAST, bool wantReplies = false);

    virtual AdminMessageHandleResult handleAdminMessageForModule(const meshtastic_MeshPacket &mp,
                                                                 meshtastic_AdminMessage *request,
                                                                 meshtastic_AdminMessage *response) override;

    void i2cScanFinished(ScanI2C *i2cScanner);

  private:
    bool firstTime = 1;
    meshtastic_MeshPacket *lastMeasurementPacket;
    uint32_t sendToPhoneIntervalMs = SECONDS_IN_MINUTE * 1000;  // Send to phone every minute
    uint32_t screenUpdateIntervalMs = SECONDS_IN_MINUTE * 1000; // Update screen data every minute
    uint32_t lastSentToMesh = 0;
    uint32_t lastSentToPhone = 0;
    uint32_t lastScreenUpdate = 0;
    uint32_t meshBroadcastStartTime = 0; // When mesh broadcasts should start (after stagger delay)
    uint32_t sensor_read_error_count = 0;
    std::unordered_map<uint32_t, meshtastic_MeshPacket *> lastBySource;
    uint32_t selectedSource = 0; // 0 = Auto (most recent), otherwise a nodenum
};

extern EnvironmentTelemetryModule *environmentTelemetryModule;

#endif