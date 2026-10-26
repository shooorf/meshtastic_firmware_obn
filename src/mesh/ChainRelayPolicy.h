#pragma once

#include "MeshTypes.h"
#include "RadioInterface.h"
#include <stdint.h>

namespace ChainRelayPolicy
{
// Returns true if the queued rebroadcast of packet p should be preserved despite hearing a duplicate.
bool shouldKeepQueuedRebroadcast(const meshtastic_MeshPacket *p, RadioInterface *iface);

// Returns rxSnr shifted by -3 dB per effective neighbor (starting from the 3rd), down to -10 dB.
float getAdjustedRxSnr(float rxSnr);

// Total number of packets saved from duplicate cancellation since boot.
uint32_t getTriggeredCount();
} // namespace ChainRelayPolicy

