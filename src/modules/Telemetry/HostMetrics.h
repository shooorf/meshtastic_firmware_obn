#pragma once
#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "NodeDB.h"
#include "ProtobufModule.h"

class HostMetricsModule : private concurrency::OSThread, public ProtobufModule<meshtastic_Telemetry>
{
    CallbackObserver<HostMetricsModule, const meshtastic::Status *> nodeStatusObserver =
        CallbackObserver<HostMetricsModule, const meshtastic::Status *>(this, &HostMetricsModule::handleStatusUpdate);

  public:
    HostMetricsModule()
        : concurrency::OSThread("HostMetrics"),
          ProtobufModule("HostMetrics", meshtastic_PortNum_TELEMETRY_APP, &meshtastic_Telemetry_msg)
    {
        loopbackOk = true;
        nodeStatusObserver.observe(&nodeStatus->onNewStatus);
        setIntervalFromNow(setStartDelay()); // Wait until NodeInfo is sent
    }
    virtual bool wantUIFrame() { return false; }

  protected:
    /** Called to handle a particular incoming message
    @return true if you've guaranteed you've handled this message and no other handlers should be considered for it
    */
    virtual bool handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Telemetry *p) override;
    virtual meshtastic_MeshPacket *allocReply() override;
    virtual int32_t runOnce() override;
    /**
     * Send our Telemetry into the mesh
     */
    bool sendMetrics(bool phoneOnly = false);

  private:
    meshtastic_Telemetry getHostMetrics();

    uint32_t sendToPhoneIntervalMs = SECONDS_IN_MINUTE * 1000;
};