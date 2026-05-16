#include "oye_ble_codec.h"

#include <pb_decode.h>
#include <pb_encode.h>
#include <cstring>

namespace oye::ble {

namespace {

struct ChunkAssembler {
    uint32_t total = 0;
    std::vector<std::vector<uint8_t>> parts;
};

ChunkAssembler g_assembler;

}  // namespace

void ResetChunkAssembler() {
    g_assembler = {};
}

bool FeedChunk(const uint8_t* data, size_t len, std::vector<uint8_t>& complete_payload) {
    complete_payload.clear();
    oye_device_v1_Chunk chunk = oye_device_v1_Chunk_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(data, len);
    if (!pb_decode(&stream, oye_device_v1_Chunk_fields, &chunk)) {
        return false;
    }
    if (chunk.total == 0 || chunk.seq >= chunk.total) {
        return false;
    }
    if (g_assembler.total == 0) {
        g_assembler.total = chunk.total;
        g_assembler.parts.assign(chunk.total, {});
    } else if (g_assembler.total != chunk.total) {
        ResetChunkAssembler();
        return false;
    }
    g_assembler.parts[chunk.seq].assign(chunk.data.bytes, chunk.data.bytes + chunk.data.size);
    for (uint32_t i = 0; i < g_assembler.total; ++i) {
        if (g_assembler.parts[i].empty()) {
            return false;
        }
    }
    size_t total_size = 0;
    for (const auto& p : g_assembler.parts) {
        total_size += p.size();
    }
    complete_payload.reserve(total_size);
    for (const auto& p : g_assembler.parts) {
        complete_payload.insert(complete_payload.end(), p.begin(), p.end());
    }
    ResetChunkAssembler();
    return true;
}

bool DecodeEnvelope(const std::vector<uint8_t>& payload, oye_device_v1_Envelope& out) {
    out = oye_device_v1_Envelope_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload.data(), payload.size());
    return pb_decode(&stream, oye_device_v1_Envelope_fields, &out);
}

bool EncodeEnvelope(const oye_device_v1_Envelope& env, std::vector<uint8_t>& out) {
    out.resize(oye_device_v1_Envelope_size);
    pb_ostream_t stream = pb_ostream_from_buffer(out.data(), out.size());
    if (!pb_encode(&stream, oye_device_v1_Envelope_fields, &env)) {
        out.clear();
        return false;
    }
    out.resize(stream.bytes_written);
    return true;
}

std::vector<std::vector<uint8_t>> EncodeChunks(const uint8_t* data, size_t len, size_t chunk_data_size) {
    std::vector<std::vector<uint8_t>> result;
    if (len == 0) {
        oye_device_v1_Chunk chunk = oye_device_v1_Chunk_init_zero;
        chunk.seq = 0;
        chunk.total = 1;
        std::vector<uint8_t> buf(oye_device_v1_Chunk_size);
        pb_ostream_t stream = pb_ostream_from_buffer(buf.data(), buf.size());
        if (pb_encode(&stream, oye_device_v1_Chunk_fields, &chunk)) {
            buf.resize(stream.bytes_written);
            result.push_back(std::move(buf));
        }
        return result;
    }
    uint32_t total = static_cast<uint32_t>((len + chunk_data_size - 1) / chunk_data_size);
    for (uint32_t seq = 0; seq < total; ++seq) {
        size_t offset = seq * chunk_data_size;
        size_t part_len = std::min(chunk_data_size, len - offset);
        oye_device_v1_Chunk chunk = oye_device_v1_Chunk_init_zero;
        chunk.seq = seq;
        chunk.total = total;
        memcpy(chunk.data.bytes, data + offset, part_len);
        chunk.data.size = part_len;
        std::vector<uint8_t> buf(oye_device_v1_Chunk_size);
        pb_ostream_t stream = pb_ostream_from_buffer(buf.data(), buf.size());
        if (!pb_encode(&stream, oye_device_v1_Chunk_fields, &chunk)) {
            result.clear();
            return result;
        }
        buf.resize(stream.bytes_written);
        result.push_back(std::move(buf));
    }
    return result;
}

}  // namespace oye::ble
