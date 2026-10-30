#include "ChainRelayPolicy.h"
#include "NodeDB.h"
#include "airtime.h"
#include "configuration.h"
#include <algorithm>

namespace ChainRelayPolicy
{
namespace
{
constexpr uint32_t NEIGHBOR_WINDOW_SECS = 6 * 3600;
constexpr float LOCAL_GROUP_SNR_THRESHOLD = 8.0f;
constexpr float MAX_SMOOTHED_CHANNEL_UTIL_PERCENT = 15.0f;
constexpr float MAX_INSTANT_CHANNEL_UTIL_PERCENT = 30.0f;
constexpr uint16_t MIN_EFFECTIVE_NEIGHBORS = 3;
constexpr float SNR_BONUS_PER_NEIGHBOR_DB = 3.0f;
constexpr float SNR_BONUS_FLOOR_DB = -10.0f;
constexpr size_t TRACK_RING_SIZE = 16;

struct ActivePacketTrack {
    NodeNum from = 0;
    PacketId id = 0;
    uint8_t distantRelays = 0;
    bool localGroupRelayed = false;
    bool countedInStats = false;
};

ActivePacketTrack trackRing[TRACK_RING_SIZE];
size_t nextTrackSlot = 0;
uint32_t triggeredCount = 0;

uint16_t countEffectiveNeighbors6h()
{
    if (!nodeDB)
        return 0;

    const NodeNum ourNode = nodeDB->getNodeNum();
    const size_t totalNodes = nodeDB->getNumMeshNodes();
    uint16_t distantCount = 0;
    bool hasLocalGroup = false;

    for (size_t i = 0; i < totalNodes; i++) {
        const meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
        if (!node || node->num == ourNode || node->num == 0 || node->num == NODENUM_BROADCAST)
            continue;
        if (!node->has_hops_away || node->hops_away != 0)
            continue;
        if (!nodeInfoLiteHasSnr(node) || nodeInfoLiteViaMqtt(node))
            continue;
        if (sinceLastSeen(node) >= NEIGHBOR_WINDOW_SECS)
            continue;

        if (node->snr > LOCAL_GROUP_SNR_THRESHOLD) {
            hasLocalGroup = true;
        } else {
            distantCount++;
        }
    }

    return distantCount + (hasLocalGroup ? 1 : 0);
}

ActivePacketTrack &getOrCreateTrack(NodeNum from, PacketId id)
{
    for (size_t i = 0; i < TRACK_RING_SIZE; i++) {
        if (trackRing[i].from == from && trackRing[i].id == id) {
            return trackRing[i];
        }
    }

    ActivePacketTrack &slot = trackRing[nextTrackSlot];
    nextTrackSlot = (nextTrackSlot + 1) % TRACK_RING_SIZE;
    slot.from = from;
    slot.id = id;
    slot.distantRelays = 0;
    slot.localGroupRelayed = false;
    slot.countedInStats = false;
    return slot;
}
} // namespace

bool shouldKeepQueuedRebroadcast(const meshtastic_MeshPacket *p, RadioInterface *iface)
{
    if (!p || !iface || p->id == 0)
        return false;

    if (config.device.role != meshtastic_Config_DeviceConfig_Role_CLIENT &&
        config.device.role != meshtastic_Config_DeviceConfig_Role_CLIENT_BASE) {
        return false;
    }

    const NodeNum from = getFrom(p);
    if (!iface->findInTxQueue(from, p->id))
        return false;

    if (!airTime || airTime->smoothedChannelUtilizationPercent() >= MAX_SMOOTHED_CHANNEL_UTIL_PERCENT ||
        airTime->channelUtilizationPercent() >= MAX_INSTANT_CHANNEL_UTIL_PERCENT) {
        return false;
    }

    const uint16_t effectiveNeighbors = countEffectiveNeighbors6h();
    if (effectiveNeighbors < MIN_EFFECTIVE_NEIGHBORS)
        return false;

    ActivePacketTrack &track = getOrCreateTrack(from, p->id);
    if (p->rx_snr > LOCAL_GROUP_SNR_THRESHOLD) {
        track.localGroupRelayed = true;
    } else if (track.distantRelays < 255) {
        track.distantRelays++;
    }

    const uint16_t heardRelays = static_cast<uint16_t>(track.distantRelays) + (track.localGroupRelayed ? 1 : 0);
    if (1 + heardRelays < effectiveNeighbors) {
        if (!track.countedInStats) {
            track.countedInStats = true;
            triggeredCount++;
        }
        LOG_DEBUG("[ChainRelay] Keep pkt 0x%08x from 0x%08x in TX queue (heardRelays=%u, effNeighbors=%u, total=%lu)", p->id,
                  from, heardRelays, effectiveNeighbors, static_cast<unsigned long>(triggeredCount));
        return true;
    }

    return false;
}

float getAdjustedRxSnr(float rxSnr)
{
    if (rxSnr <= SNR_BONUS_FLOOR_DB)
        return rxSnr;

    if (config.device.role != meshtastic_Config_DeviceConfig_Role_CLIENT &&
        config.device.role != meshtastic_Config_DeviceConfig_Role_CLIENT_BASE) {
        return rxSnr;
    }

    const uint16_t effectiveNeighbors = countEffectiveNeighbors6h();
    if (effectiveNeighbors < MIN_EFFECTIVE_NEIGHBORS)
        return rxSnr;

    const float bonusDb = static_cast<float>(effectiveNeighbors - (MIN_EFFECTIVE_NEIGHBORS - 1)) * SNR_BONUS_PER_NEIGHBOR_DB;
    const float adjusted = std::max(SNR_BONUS_FLOOR_DB, rxSnr - bonusDb);
    LOG_DEBUG("[ChainRelay] SNR bonus: raw=%.1f -> adj=%.1f dB (effNeighbors=%u)", rxSnr, adjusted, effectiveNeighbors);
    return adjusted;
}

uint32_t getTriggeredCount()
{
    return triggeredCount;
}
} // namespace ChainRelayPolicy

