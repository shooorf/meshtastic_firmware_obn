#include "HostMetrics.h"
#include "../mesh/generated/meshtastic/telemetry.pb.h"
#include "AirTime.h"
#include "ChainRelayPolicy.h"
#include "Default.h"
#include "MeshService.h"
#include "NodeDB.h"
#include "Router.h"
#include "Throttle.h"
#include "UptimeClock.h"
#include "configuration.h"
#include "gps/RTC.h"
#include "main.h"
#include "memGet.h"
#include "meshUtils.h"
#include <cmath>
#include <cstdio>
#include <cstring>

static bool isSensorOrRouterRole()
{
    return config.device.role == meshtastic_Config_DeviceConfig_Role_SENSOR ||
           config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER ||
           config.device.role == meshtastic_Config_DeviceConfig_Role_ROUTER_LATE;
}

int32_t HostMetricsModule::runOnce()
{
    bool isImpoliteRole = isSensorOrRouterRole();
    if (((lastSentToMesh == 0) || Throttle::hasElapsed(lastSentToMesh, sendToMeshIntervalMs)) && airTime &&
        airTime->isTxAllowedChannelUtil(!isImpoliteRole) && airTime->isTxAllowedAirUtil() &&
        config.device.role != meshtastic_Config_DeviceConfig_Role_CLIENT_HIDDEN) {
        if (service) {
            sendMetrics(false);
            lastSentToMesh = Time::skipZero(Time::getMillis());
        }
    } else if (service && service->isToPhoneQueueEmpty()) {
        sendMetrics(true);
    }
    return sendToPhoneIntervalMs;
}

bool HostMetricsModule::handleReceivedProtobuf(const meshtastic_MeshPacket &mp, meshtastic_Telemetry *t)
{
    if (t->which_variant == meshtastic_Telemetry_host_metrics_tag) {
#if defined(DEBUG_PORT) && !defined(DEBUG_MUTE)
        const char *sender = getSenderShortName(mp);
        if (t->variant.host_metrics.has_user_string)
            t->variant.host_metrics.user_string[sizeof(t->variant.host_metrics.user_string) - 1] = '\0';

        LOG_INFO("(Received Host Metrics from %s): uptime=%u, freemem=%lu, load=%04.2f, %04.2f, %04.2f", sender,
                 t->variant.host_metrics.uptime_seconds, (unsigned long)t->variant.host_metrics.freemem_bytes,
                 static_cast<float>(t->variant.host_metrics.load1) / 100,
                 static_cast<float>(t->variant.host_metrics.load5) / 100,
                 static_cast<float>(t->variant.host_metrics.load15) / 100);
#endif
    }
    return false; // Let others look at this message also if they want
}

meshtastic_MeshPacket *HostMetricsModule::allocReply()
{
    if (currentRequest) {
        if (isMultiHopBroadcastRequest() && !isSensorOrRouterRole()) {
            ignoreRequest = true;
            return NULL;
        }
        auto req = *currentRequest;
        const auto &p = req.decoded;
        meshtastic_Telemetry scratch;
        memset(&scratch, 0, sizeof(scratch));
        if (pb_decode_from_bytes(p.payload.bytes, p.payload.size, &meshtastic_Telemetry_msg, &scratch)) {
            if (scratch.which_variant == meshtastic_Telemetry_host_metrics_tag) {
                LOG_INFO("HostMetrics reply to request");
                return allocDataProtobuf(getHostMetrics());
            }
        } else {
            LOG_ERROR("Can't decode HostMetrics module");
        }
    }
    return NULL;
}

meshtastic_Telemetry HostMetricsModule::getHostMetrics()
{
    meshtastic_Telemetry t = meshtastic_Telemetry_init_zero;
    meshtastic_HostMetrics hm = meshtastic_HostMetrics_init_zero;
    t.time = getTime();
    t.which_variant = meshtastic_Telemetry_host_metrics_tag;
    t.variant.host_metrics = hm;
    t.variant.host_metrics.uptime_seconds = Time::getUptimeSecs();
    t.variant.host_metrics.freemem_bytes = memGet.getFreeHeap();

    uint16_t directCount = 0;
    uint16_t directSnrNegCount = 0;
    uint8_t listedCount = 0;
    char *userStr = t.variant.host_metrics.user_string;
    const size_t userStrCap = sizeof(t.variant.host_metrics.user_string);
    size_t pos = 0;
    userStr[0] = '\0';

    const NodeNum ourNode = nodeDB ? nodeDB->getNodeNum() : 0;
    const size_t totalNodes = nodeDB ? nodeDB->getNumMeshNodes() : 0;
    for (size_t i = 0; i < totalNodes; i++) {
        const meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
        if (!node || node->num == ourNode || node->num == 0 || node->num == NODENUM_BROADCAST)
            continue;
        if (!node->has_hops_away || node->hops_away != 0)
            continue;
        if (!nodeInfoLiteHasSnr(node) || nodeInfoLiteViaMqtt(node))
            continue;
        if (sinceLastSeen(node) >= NEXTHOP_NEIGHBOR_FRESH_SECS)
            continue;

        directCount++;
        if (node->snr < 0.0f) {
            directSnrNegCount++;
        }

        if (listedCount < 16) {
            char shortName[5] = "????";
            if (nodeInfoLiteHasUser(node) && node->short_name[0] != '\0') {
                strncpy(shortName, node->short_name, 4);
                shortName[4] = '\0';
                sanitizeUtf8(shortName, sizeof(shortName));
            }
            int snrInt = static_cast<int>(std::lround(node->snr));
            if (snrInt > 99)
                snrInt = 99;
            else if (snrInt < -99)
                snrInt = -99;

            const char *prefix = (listedCount > 0) ? ", " : "";
            int written = snprintf(userStr + pos, userStrCap - pos, "%s%s:%+03d", prefix, shortName, snrInt);
            if (written > 0 && pos + static_cast<size_t>(written) < userStrCap) {
                pos += static_cast<size_t>(written);
                listedCount++;
            } else {
                userStr[pos] = '\0';
                break;
            }
        }
    }

    const uint32_t chainRelayCount = ChainRelayPolicy::getTriggeredCount();
    t.variant.host_metrics.load1 = (directCount > 655) ? 65500 : static_cast<uint16_t>(directCount * 100);
    t.variant.host_metrics.load5 = (directSnrNegCount > 655) ? 65500 : static_cast<uint16_t>(directSnrNegCount * 100);
    t.variant.host_metrics.load15 = (chainRelayCount > 655) ? 65500 : static_cast<uint16_t>(chainRelayCount * 100);
    if (pos > 0) {
        t.variant.host_metrics.has_user_string = true;
    }

    return t;
}

bool HostMetricsModule::sendMetrics(bool phoneOnly)
{
    meshtastic_Telemetry telemetry = getHostMetrics();
    meshtastic_MeshPacket *p = allocDataProtobuf(telemetry);
    if (!p)
        return false;
    p->to = NODENUM_BROADCAST;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;
    if (phoneOnly) {
        service->sendToPhone(p);
    } else {
        service->sendToMesh(p, RX_SRC_LOCAL, true);
    }
    return true;
}
