#pragma once
#include "configuration.h"
#if !MESHTASTIC_EXCLUDE_REPLYBOT
#include "SinglePortModule.h"
#include "concurrency/OSThread.h"
#include "mesh/generated/meshtastic/mesh.pb.h"

class ReplyBotModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    ReplyBotModule();
    void setup() override;
    bool wantPacket(const meshtastic_MeshPacket *p) override;
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    AdminMessageHandleResult handleAdminMessageForModule(const meshtastic_MeshPacket &mp, meshtastic_AdminMessage *request,
                                                         meshtastic_AdminMessage *response) override;

  protected:
    int32_t runOnce() override;
    bool isCommand(const char *msg) const;

  private:
    struct PendingReply {
        bool active = false;
        uint32_t queuedAtMs = 0;
        uint32_t delayMs = 0;
        NodeNum to = 0;
        ChannelIndex channel = 0;
        PacketId replyId = 0;
        float snr = 0.0f;
        int32_t rssi = 0;
        int8_t hopsAway = 0;
    };

    static constexpr uint8_t MAX_PENDING_REPLIES = 4;
    PendingReply pendingReplies_[MAX_PENDING_REPLIES];

    void enqueueReply(const meshtastic_MeshPacket &rx, NodeNum to, uint32_t delayMs);
    void sendReply(const PendingReply &item);
};
#endif // MESHTASTIC_EXCLUDE_REPLYBOT