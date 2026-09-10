#include "ncsi_protocol.h"

#include <string.h>

static void put_u16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static void put_u64(uint8_t *dst, uint64_t value)
{
    put_u32(dst, (uint32_t)value);
    put_u32(dst + 4, (uint32_t)(value >> 32));
}

static void put_i64(uint8_t *dst, int64_t value) { put_u64(dst, (uint64_t)value); }

static void put_common_header(uint8_t *dst, uint8_t type, uint16_t header_length,
                              uint32_t record_length)
{
    dst[0] = 'N';
    dst[1] = 'C';
    dst[2] = 'S';
    dst[3] = 'I';
    dst[4] = NCSI_VERSION_MAJOR;
    dst[5] = type;
    put_u16(dst + 6, header_length);
    put_u32(dst + 8, record_length);
    put_u32(dst + 12, 0);
}

static void finish_crc(uint8_t *record, size_t length)
{
    put_u32(record + 12, ncsi_crc32c(record, length));
}

uint32_t ncsi_crc32c(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0x82F63B78u & mask);
        }
    }
    return crc ^ UINT32_MAX;
}

uint8_t ncsi_sanitize_invalid_prefix(uint8_t *csi, uint16_t length,
                                     bool first_word_invalid)
{
    if (!first_word_invalid || csi == NULL || length == 0) {
        return 0;
    }
    uint8_t sanitized = length < 4 ? (uint8_t)length : 4;
    memset(csi, 0, sanitized);
    return sanitized;
}

size_t ncsi_serialize_csi(const ncsi_csi_record_t *r, uint8_t *out, size_t capacity)
{
    if (r == NULL || out == NULL || r->csi == NULL || r->csi_length == 0 ||
        r->csi_length > NCSI_MAX_CSI_BYTES || (r->csi_length & 1u) != 0 ||
        r->driver_csi_length != r->csi_length || r->sanitized_prefix_bytes > 4 ||
        r->sanitized_prefix_bytes > r->csi_length) {
        return 0;
    }
    if (r->sanitized_prefix_bytes != 0) {
        uint16_t required = NCSI_FLAG_FIRST_WORD_INVALID_REPORTED |
                            NCSI_FLAG_INVALID_PREFIX_SANITIZED;
        if ((r->csi_flags & required) != required) {
            return 0;
        }
        for (uint8_t i = 0; i < r->sanitized_prefix_bytes; ++i) {
            if (r->csi[i] != 0) return 0;
        }
    }
    size_t length = NCSI_CSI_HEADER_SIZE + r->csi_length;
    if (capacity < length) {
        return 0;
    }

    memset(out, 0, NCSI_CSI_HEADER_SIZE);
    put_common_header(out, NCSI_RECORD_CSI, NCSI_CSI_HEADER_SIZE, (uint32_t)length);
    put_u32(out + 16, r->node_id);
    memcpy(out + 20, r->receiver_mac, 6);
    memcpy(out + 26, r->source_mac, 6);
    put_u32(out + 32, r->sequence);
    put_u64(out + 36, r->timestamp_us);
    out[44] = r->channel;
    out[45] = r->secondary_channel;
    out[46] = r->bandwidth;
    out[47] = r->phy_mode;
    out[48] = (uint8_t)r->rssi_dbm;
    out[49] = (uint8_t)r->noise_floor_dbm;
    out[50] = r->antenna;
    out[51] = r->ltf_mask;
    put_u16(out + 52, r->driver_csi_length);
    put_u16(out + 54, r->csi_length);
    put_u16(out + 56, r->csi_length / 2u);
    put_u16(out + 58, r->csi_flags);
    put_u16(out + 60, r->path_id);
    out[62] = r->sanitized_prefix_bytes;
    out[63] = 1; /* IMAG_REAL_S8 */
    put_u32(out + 64, r->driver_rx_timestamp_us);
    out[68] = r->phy_rate;
    out[69] = r->mcs;
    put_u16(out + 70, r->rx_flags);
    out[72] = r->ampdu_count;
    out[73] = r->rx_state;
    put_u16(out + 74, r->packet_length);
    put_u16(out + 76, r->driver_rx_sequence);
    put_u16(out + 78, 0);
    memcpy(out + 80, r->destination_mac, 6);
    put_u16(out + 86, 0);
    memcpy(out + NCSI_CSI_HEADER_SIZE, r->csi, r->csi_length);
    finish_crc(out, length);
    return length;
}

size_t ncsi_serialize_status(const ncsi_status_record_t *r, uint8_t *out, size_t capacity)
{
    if (r == NULL || out == NULL || capacity < NCSI_STATUS_RECORD_SIZE) {
        return 0;
    }

    memset(out, 0, NCSI_STATUS_RECORD_SIZE);
    put_common_header(out, NCSI_RECORD_STATUS, NCSI_STATUS_RECORD_SIZE,
                      NCSI_STATUS_RECORD_SIZE);
    put_u32(out + 16, r->node_id);
    memcpy(out + 20, r->receiver_mac, 6);
    put_u16(out + 26, r->status_flags);
    put_u32(out + 28, r->boot_id);
    put_u32(out + 32, r->status_sequence);
    put_u64(out + 36, r->timestamp_us);
    put_u32(out + 44, r->callbacks_total);
    put_u32(out + 48, r->rate_gate_drops);
    put_u32(out + 52, r->source_filter_drops);
    put_u32(out + 56, r->accepted_total);
    put_u32(out + 60, r->ring_full_drops);
    put_u32(out + 64, r->transport_ok);
    put_u32(out + 68, r->transport_drops);
    put_u32(out + 72, r->last_csi_sequence);
    put_u16(out + 76, r->raw_target_hz);
    put_u16(out + 78, r->dsp_target_hz);
    finish_crc(out, NCSI_STATUS_RECORD_SIZE);
    return NCSI_STATUS_RECORD_SIZE;
}

size_t ncsi_serialize_diagnostic(const ncsi_diagnostic_record_t *r,
                                 uint8_t *out, size_t capacity)
{
    if (r == NULL || out == NULL || capacity < NCSI_DIAGNOSTIC_RECORD_SIZE) {
        return 0;
    }
    memset(out, 0, NCSI_DIAGNOSTIC_RECORD_SIZE);
    put_common_header(out, NCSI_RECORD_DIAGNOSTIC,
                      NCSI_DIAGNOSTIC_RECORD_SIZE,
                      NCSI_DIAGNOSTIC_RECORD_SIZE);
    put_u32(out + 16, r->node_id);
    memcpy(out + 20, r->receiver_mac, 6);
    put_u16(out + 26, r->diagnostic_flags);
    put_u32(out + 28, r->boot_id);
    put_u32(out + 32, r->diagnostic_sequence);
    put_u64(out + 36, r->timestamp_us);
    put_u32(out + 44, r->status_transport_ok);
    put_u32(out + 48, r->status_transport_drops);
    put_u32(out + 52, r->probe_tx_attempted);
    put_u32(out + 56, r->probe_tx_queued);
    put_u32(out + 60, r->probe_tx_success);
    put_u32(out + 64, r->probe_tx_link_failure);
    put_u32(out + 68, r->probe_tx_submit_failure);
    put_u32(out + 72, r->probe_tx_skipped_busy);
    put_u32(out + 76, r->probe_tx_skipped_unassociated);
    put_u32(out + 80, r->probe_rx_valid);
    put_u32(out + 84, r->probe_rx_invalid);
    put_u32(out + 88, r->path_gate_drops[0]);
    put_u32(out + 92, r->path_gate_drops[1]);
    put_u32(out + 96, r->path_gate_drops[2]);
    put_u32(out + 100, r->association_epoch);
    finish_crc(out, NCSI_DIAGNOSTIC_RECORD_SIZE);
    return NCSI_DIAGNOSTIC_RECORD_SIZE;
}

size_t ncsi_serialize_sync(const ncsi_sync_record_t *r, uint8_t *out, size_t capacity)
{
    if (r == NULL || out == NULL || capacity < NCSI_SYNC_RECORD_SIZE || r->sync_version != 1) return 0;
    memset(out, 0, NCSI_SYNC_RECORD_SIZE);
    put_common_header(out, NCSI_RECORD_SYNC, NCSI_SYNC_RECORD_SIZE, NCSI_SYNC_RECORD_SIZE);
    put_u32(out + 16, r->node_id); memcpy(out + 20, r->receiver_mac, 6);
    out[26] = r->sync_version; out[27] = r->sync_state;
    put_u32(out + 28, r->boot_id); put_u32(out + 32, r->sync_sequence);
    put_u64(out + 36, r->local_timestamp_us); put_u64(out + 44, r->leader_timestamp_us);
    put_i64(out + 52, r->raw_offset_us); put_i64(out + 60, r->smoothed_offset_us);
    put_u32(out + 68, (uint32_t)r->drift_milli_ppm);
    put_u32(out + 72, r->accepted_samples); put_u32(out + 76, r->rejected_samples);
    put_u32(out + 80, r->last_sync_age_us); put_u32(out + 84, r->leader_node_id);
    put_u32(out + 88, r->leader_session_id); put_u32(out + 92, r->follower_session_id);
    put_u32(out + 96, r->beacon_sequence); put_u32(out + 100, r->jitter_us);
    put_u32(out + 104, r->transport_rx); put_u32(out + 108, r->transport_drops);
    finish_crc(out, NCSI_SYNC_RECORD_SIZE); return NCSI_SYNC_RECORD_SIZE;
}
