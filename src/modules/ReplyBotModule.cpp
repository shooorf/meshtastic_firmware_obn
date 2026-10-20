#include "configuration.h"
#if !MESHTASTIC_EXCLUDE_REPLYBOT
/*
 * ReplyBotModule.cpp
 *
 * This module implements an auto-reply bot configured via CannedMessageModule settings
 * (moduleConfig.canned_message.enabled and pipe-separated templates in cannedMessageModuleConfig.messages).
 * When a matching text message is received (DM or broadcast), it replies to the same destination
 * after a random 5-30s delay with SNR, RSSI, and hop count, enforcing a per-sender cooldown.
 */

#include "CannedMessageModule.h"
#include "Channels.h"
#include "FSCommon.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "ReplyBotModule.h"
#include "SPILock.h"
#include "Throttle.h"
#include "mesh/MeshTypes.h"

#include <Arduino.h>
#include <cstring>

struct ReplyBotCooldownEntry {
    uint32_t from = 0;
    uint32_t lastMs = 0;
    bool used = false;
};

static constexpr uint8_t REPLYBOT_COOLDOWN_SLOTS = 8;
static constexpr uint32_t REPLYBOT_COOLDOWN_MS = 30 * 1000;
static constexpr uint32_t REPLYBOT_DELAY_MIN_MS = 5 * 1000;
static constexpr uint32_t REPLYBOT_DELAY_MAX_MS = 30 * 1000;

static ReplyBotCooldownEntry replybotCooldown[REPLYBOT_COOLDOWN_SLOTS];
static uint8_t replybotCooldownIdx = 0;

static bool replybotRateLimited(uint32_t from, uint32_t cooldownMs)
{
    const uint32_t now = Time::getMillis();
    for (auto &e : replybotCooldown) {
        if (e.used && e.from == from) {
            if (Throttle::isWithinTimespanMs(e.lastMs, cooldownMs)) {
                return true;
            }
            e.lastMs = now;
            return false;
        }
    }
    replybotCooldown[replybotCooldownIdx].from = from;
    replybotCooldown[replybotCooldownIdx].lastMs = now;
    replybotCooldown[replybotCooldownIdx].used = true;
    replybotCooldownIdx = (replybotCooldownIdx + 1) % REPLYBOT_COOLDOWN_SLOTS;
    return false;
}

static bool isAsciiSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

ReplyBotModule::ReplyBotModule()
    : SinglePortModule("replybot", meshtastic_PortNum_TEXT_MESSAGE_APP), concurrency::OSThread("ReplyBot")
{
    isPromiscuous = true;
    disable();
#if HAS_SCREEN && !MESHTASTIC_EXCLUDE_CANNEDMESSAGES
    if (!cannedMessageModule)
#endif
    {
        nodeDB->loadProto("/prefs/cannedConf.proto", meshtastic_CannedMessageModuleConfig_size,
                          sizeof(meshtastic_CannedMessageModuleConfig), &meshtastic_CannedMessageModuleConfig_msg,
                          &cannedMessageModuleConfig);
    }
}

void ReplyBotModule::setup() {}

bool ReplyBotModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return (p && p->decoded.portnum == ourPortNum);
}

ProcessMessage ReplyBotModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    if (!moduleConfig.canned_message.enabled || cannedMessageModuleConfig.messages[0] == '\0') {
        return ProcessMessage::CONTINUE;
    }

    const NodeNum ourNode = nodeDB->getNodeNum();
    const NodeNum from = getFrom(&mp);
    if (from == 0 || from == ourNode) {
        return ProcessMessage::CONTINUE;
    }

    const bool isDM = (mp.to == ourNode);
    const bool isBcast = isBroadcast(mp.to);
    if (!isDM && !isBcast) {
        return ProcessMessage::CONTINUE;
    }

    if (mp.decoded.payload.size == 0) {
        return ProcessMessage::CONTINUE;
    }

    char buf[260];
    size_t n = mp.decoded.payload.size;
    if (n > sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    memcpy(buf, mp.decoded.payload.bytes, n);
    buf[n] = '\0';

    if (!isCommand(buf)) {
        return ProcessMessage::CONTINUE;
    }

    if (replybotRateLimited(from, REPLYBOT_COOLDOWN_MS)) {
        return ProcessMessage::CONTINUE;
    }

    const NodeNum replyTo = isDM ? from : NODENUM_BROADCAST;
    const uint32_t delayMs = random(REPLYBOT_DELAY_MIN_MS, REPLYBOT_DELAY_MAX_MS + 1);
    enqueueReply(mp, replyTo, delayMs);
    return ProcessMessage::CONTINUE;
}

bool ReplyBotModule::isCommand(const char *msg) const
{
    if (!msg || cannedMessageModuleConfig.messages[0] == '\0')
        return false;

    while (*msg && isAsciiSpace(*msg))
        msg++;
    const char *msgEnd = msg + strlen(msg);
    while (msgEnd > msg && isAsciiSpace(*(msgEnd - 1)))
        msgEnd--;

    const size_t msgLen = static_cast<size_t>(msgEnd - msg);
    if (msgLen == 0)
        return false;

    const char *p = cannedMessageModuleConfig.messages;
    while (*p) {
        const char *sep = strchr(p, '|');
        const char *tokStart = p;
        const char *tokEnd = sep ? sep : (p + strlen(p));

        while (tokStart < tokEnd && isAsciiSpace(*tokStart))
            tokStart++;
        while (tokEnd > tokStart && isAsciiSpace(*(tokEnd - 1)))
            tokEnd--;

        const size_t tokLen = static_cast<size_t>(tokEnd - tokStart);
        if (tokLen > 0 && tokLen == msgLen && memcmp(tokStart, msg, msgLen) == 0) {
            return true;
        }

        if (!sep)
            break;
        p = sep + 1;
    }
    return false;
}

void ReplyBotModule::enqueueReply(const meshtastic_MeshPacket &rx, NodeNum to, uint32_t delayMs)
{
    const uint32_t now = Time::getMillis();
    PendingReply *slot = nullptr;
    for (auto &entry : pendingReplies_) {
        if (!entry.active) {
            slot = &entry;
            break;
        }
    }
    if (!slot)
        return;

    int rssi = rx.rx_rssi;
    if (rssi > 0) {
        rssi -= 200;
    }

    slot->active = true;
    slot->queuedAtMs = now;
    slot->delayMs = delayMs;
    slot->to = to;
    slot->channel = rx.channel;
    slot->replyId = rx.id;
    slot->snr = rx.rx_snr;
    slot->rssi = rssi;
    slot->hopsAway = getHopsAway(rx, 0);

    uint32_t nextRunMs = delayMs;
    for (const auto &entry : pendingReplies_) {
        if (entry.active) {
            const uint32_t elapsed = now - entry.queuedAtMs;
            const uint32_t rem = (elapsed < entry.delayMs) ? (entry.delayMs - elapsed) : 0;
            if (rem < nextRunMs)
                nextRunMs = rem;
        }
    }
    setIntervalFromNow(nextRunMs);
    enabled = true;
}

int32_t ReplyBotModule::runOnce()
{
    const uint32_t now = Time::getMillis();
    uint32_t nextRunMs = UINT32_MAX;
    bool anyActive = false;

    for (auto &entry : pendingReplies_) {
        if (!entry.active)
            continue;
        if (Throttle::hasElapsed(entry.queuedAtMs, entry.delayMs)) {
            entry.active = false;
            sendReply(entry);
        } else {
            anyActive = true;
            const uint32_t elapsed = now - entry.queuedAtMs;
            const uint32_t rem = entry.delayMs - elapsed;
            if (rem < nextRunMs)
                nextRunMs = rem;
        }
    }

    if (!anyActive) {
        return disable();
    }
    return static_cast<int32_t>(nextRunMs);
}

void ReplyBotModule::sendReply(const PendingReply &item)
{
    if (!moduleConfig.canned_message.enabled)
        return;

    char reply[128];
    snprintf(reply, sizeof(reply), "Слышу тебя. SNR %.1f RSSI %d, %d хопов", item.snr, static_cast<int>(item.rssi),
             static_cast<int>(item.hopsAway));

    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p)
        return;
    p->to = item.to;
    p->channel = item.channel;
    p->decoded.reply_id = item.replyId;
    p->want_ack = !isBroadcast(item.to);
    p->decoded.want_response = false;
    size_t len = strlen(reply);
    if (len > sizeof(p->decoded.payload.bytes)) {
        len = sizeof(p->decoded.payload.bytes);
    }
    p->decoded.payload.size = len;
    memcpy(p->decoded.payload.bytes, reply, len);
    service->sendToMesh(p, RX_SRC_LOCAL, true);
}

AdminMessageHandleResult ReplyBotModule::handleAdminMessageForModule(const meshtastic_MeshPacket &mp,
                                                                     meshtastic_AdminMessage *request,
                                                                     meshtastic_AdminMessage *response)
{
#if HAS_SCREEN && !MESHTASTIC_EXCLUDE_CANNEDMESSAGES
    if (cannedMessageModule)
        return AdminMessageHandleResult::NOT_HANDLED;
#endif
    switch (request->which_payload_variant) {
    case meshtastic_AdminMessage_get_canned_message_module_messages_request_tag:
        if (mp.decoded.want_response) {
            response->which_payload_variant = meshtastic_AdminMessage_get_canned_message_module_messages_response_tag;
            strncpy(response->get_canned_message_module_messages_response, cannedMessageModuleConfig.messages,
                    sizeof(response->get_canned_message_module_messages_response));
        }
        return AdminMessageHandleResult::HANDLED_WITH_RESPONSE;

    case meshtastic_AdminMessage_set_canned_message_module_messages_tag:
        if (*request->set_canned_message_module_messages &&
            strcmp(cannedMessageModuleConfig.messages, request->set_canned_message_module_messages) != 0) {
            strncpy(cannedMessageModuleConfig.messages, request->set_canned_message_module_messages,
                    sizeof(cannedMessageModuleConfig.messages));
            cannedMessageModuleConfig.messages[sizeof(cannedMessageModuleConfig.messages) - 1] = '\0';
#ifdef FSCom
            spiLock->lock();
            FSCom.mkdir("/prefs");
            spiLock->unlock();
#endif
            nodeDB->saveProto("/prefs/cannedConf.proto", meshtastic_CannedMessageModuleConfig_size,
                              &meshtastic_CannedMessageModuleConfig_msg, &cannedMessageModuleConfig);
        }
        return AdminMessageHandleResult::HANDLED;

    default:
        return AdminMessageHandleResult::NOT_HANDLED;
    }
}
#endif // MESHTASTIC_EXCLUDE_REPLYBOT