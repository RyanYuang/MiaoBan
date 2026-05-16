#ifndef OYE_BLE_CODEC_H
#define OYE_BLE_CODEC_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "oye/device/v1/device.pb.h"

namespace oye::ble {

/** Reassemble Chunk writes; returns true when a full payload is ready. */
bool FeedChunk(const uint8_t* data, size_t len, std::vector<uint8_t>& complete_payload);

void ResetChunkAssembler();

bool DecodeEnvelope(const std::vector<uint8_t>& payload, oye_device_v1_Envelope& out);

bool EncodeEnvelope(const oye_device_v1_Envelope& env, std::vector<uint8_t>& out);

/** Split payload into Chunk notifications (seq 0..total-1). */
std::vector<std::vector<uint8_t>> EncodeChunks(const uint8_t* data, size_t len, size_t chunk_data_size = 180);

}  // namespace oye::ble

#endif
