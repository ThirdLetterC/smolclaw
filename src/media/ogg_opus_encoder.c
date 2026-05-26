#include "media/media_encoder_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "sc/api.h"

#ifdef SC_HAVE_OPUS
#include <opus/opus.h>

enum {
    opus_output_sample_rate = 48'000,
    opus_frame_samples = 960,
    opus_max_packet_bytes = 4'000,
    ogg_max_lacing_segments = 255,
};

typedef struct wav_info {
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    const uint8_t *data;
    size_t data_len;
} wav_info;

typedef struct ogg_writer {
    sc_bytes bytes;
    size_t max_output_bytes;
    uint32_t serial;
    uint32_t sequence;
} ogg_writer;

static sc_status parse_wav(sc_buf wav, wav_info *out);
static uint16_t read_le16(const uint8_t *ptr);
static uint32_t read_le32(const uint8_t *ptr);
static void write_le16(uint8_t *ptr, uint16_t value);
static void write_le32(uint8_t *ptr, uint32_t value);
static void write_le64(uint8_t *ptr, uint64_t value);
static sc_status wav_to_mono_pcm(sc_allocator *alloc, const wav_info *wav, int16_t **samples_out, size_t *sample_count_out);
static sc_status resample_to_48k(sc_allocator *alloc,
                                 const int16_t *input,
                                 size_t input_count,
                                 uint32_t input_rate,
                                 int16_t **samples_out,
                                 size_t *sample_count_out);
static uint32_t ogg_serial_for_input(sc_buf wav);
static sc_status ogg_writer_init(ogg_writer *writer, sc_allocator *alloc, sc_buf wav, size_t max_output_bytes);
static sc_status ogg_write_opus_headers(ogg_writer *writer, uint16_t pre_skip);
static sc_status ogg_write_opus_head(ogg_writer *writer, uint16_t pre_skip);
static sc_status ogg_write_opus_tags(ogg_writer *writer);
static sc_status ogg_write_page(ogg_writer *writer, sc_buf packet, uint8_t flags, uint64_t granule_position);
static sc_status append_checked(ogg_writer *writer, sc_buf input);
static uint32_t ogg_crc(sc_buf bytes);
static uint32_t ogg_crc_update(uint32_t crc, uint8_t value);
#endif

// cppcheck-suppress constParameterPointer
sc_status sc_media_wav_to_ogg_opus(sc_allocator *alloc, sc_buf wav, size_t max_output_bytes, sc_bytes *out)
{
#ifndef SC_HAVE_OPUS
    (void)alloc;
    (void)wav;
    (void)max_output_bytes;
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.media.opus.invalid_argument");
    }
    return sc_status_unsupported("sc.media.opus.unavailable");
#else
    wav_info info = {0};
    int16_t *mono = nullptr;
    int16_t *pcm48 = nullptr;
    size_t mono_count = 0;
    size_t pcm48_count = 0;
    ogg_writer writer = {0};
    OpusEncoder *encoder = nullptr;
    size_t encoder_size = 0;
    int opus_status;
    int lookahead = 0;
    sc_status status;

    if (out == nullptr || (wav.len > 0 && wav.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.media.opus.invalid_argument");
    }
    *out = (sc_bytes){0};
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;

    status = parse_wav(wav, &info);
    if (sc_status_is_ok(status)) {
        status = wav_to_mono_pcm(alloc, &info, &mono, &mono_count);
    }
    if (sc_status_is_ok(status)) {
        status = resample_to_48k(alloc, mono, mono_count, info.sample_rate, &pcm48, &pcm48_count);
    }
    if (sc_status_is_ok(status)) {
        encoder_size = (size_t)opus_encoder_get_size(1);
        encoder = sc_alloc(alloc, encoder_size, _Alignof(max_align_t));
        if (encoder == nullptr) {
            status = sc_status_no_memory();
        }
    }
    if (sc_status_is_ok(status)) {
        opus_status = opus_encoder_init(encoder, opus_output_sample_rate, 1, OPUS_APPLICATION_VOIP);
        if (opus_status != OPUS_OK) {
            status = sc_status_io("sc.media.opus.encoder_init_failed");
        }
    }
    if (sc_status_is_ok(status)) {
        (void)opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24'000));
        (void)opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_status = opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&lookahead));
        if (opus_status != OPUS_OK || lookahead < 0 || lookahead > UINT16_MAX) {
            status = sc_status_io("sc.media.opus.encoder_init_failed");
        }
    }
    if (sc_status_is_ok(status)) {
        status = ogg_writer_init(&writer, alloc, wav, max_output_bytes);
    }
    if (sc_status_is_ok(status)) {
        status = ogg_write_opus_headers(&writer, (uint16_t)lookahead);
    }
    for (size_t offset = 0; sc_status_is_ok(status) && offset < pcm48_count; offset += opus_frame_samples) {
        int16_t frame[opus_frame_samples] = {0};
        uint8_t packet[opus_max_packet_bytes] = {0};
        size_t available = pcm48_count - offset;
        int encoded = 0;
        uint64_t granule_position = 0;
        uint8_t flags = 0;

        if (available > opus_frame_samples) {
            available = opus_frame_samples;
        } else {
            flags = 0x04;
        }
        (void)memcpy(frame, &pcm48[offset], available * sizeof(frame[0]));
        encoded = opus_encode(encoder, frame, opus_frame_samples, packet, sizeof(packet));
        if (encoded <= 0) {
            status = sc_status_io("sc.media.opus.encode_failed");
            break;
        }
        granule_position = (uint64_t)lookahead + (uint64_t)offset + (uint64_t)available;
        status = ogg_write_page(&writer, sc_buf_from_parts(packet, (size_t)encoded), flags, granule_position);
    }
    if (sc_status_is_ok(status)) {
        *out = writer.bytes;
        writer.bytes = (sc_bytes){0};
    }

    sc_bytes_clear(&writer.bytes);
    if (pcm48 != nullptr) {
        sc_free(alloc, pcm48, pcm48_count * sizeof(*pcm48), _Alignof(int16_t));
    }
    if (mono != nullptr) {
        sc_free(alloc, mono, mono_count * sizeof(*mono), _Alignof(int16_t));
    }
    if (encoder != nullptr) {
        sc_free(alloc, encoder, encoder_size, _Alignof(max_align_t));
    }
    return status;
#endif
}

#ifdef SC_HAVE_OPUS
static sc_status parse_wav(sc_buf wav, wav_info *out)
{
    size_t offset = 12;
    bool have_fmt = false;
    bool have_data = false;

    if (out == nullptr || wav.ptr == nullptr || wav.len < 44) {
        return sc_status_parse("sc.media.wav.malformed");
    }
    if (memcmp(wav.ptr, "RIFF", 4) != 0 || memcmp(wav.ptr + 8, "WAVE", 4) != 0) {
        return sc_status_parse("sc.media.wav.malformed");
    }

    while (offset + 8 <= wav.len) {
        const uint8_t *chunk = wav.ptr + offset;
        uint32_t chunk_size = read_le32(chunk + 4);
        size_t data_offset = offset + 8;
        size_t padded_size = (size_t)chunk_size + ((chunk_size & 1U) != 0 ? 1U : 0U);

        if (data_offset > wav.len || (size_t)chunk_size > wav.len - data_offset) {
            return sc_status_parse("sc.media.wav.malformed");
        }
        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                return sc_status_parse("sc.media.wav.malformed");
            }
            out->format = read_le16(wav.ptr + data_offset);
            out->channels = read_le16(wav.ptr + data_offset + 2);
            out->sample_rate = read_le32(wav.ptr + data_offset + 4);
            out->bits_per_sample = read_le16(wav.ptr + data_offset + 14);
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            out->data = wav.ptr + data_offset;
            out->data_len = chunk_size;
            have_data = true;
        }
        if (padded_size > wav.len - data_offset) {
            return sc_status_parse("sc.media.wav.malformed");
        }
        offset = data_offset + padded_size;
    }

    if (!have_fmt || !have_data || out->data_len == 0) {
        return sc_status_parse("sc.media.wav.malformed");
    }
    if (out->format != 1 || out->bits_per_sample != 16 || out->channels == 0 || out->channels > 8 || out->sample_rate == 0) {
        return sc_status_unsupported("sc.media.wav.unsupported_format");
    }
    return sc_status_ok();
}

static uint16_t read_le16(const uint8_t *ptr)
{
    return (uint16_t)((uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8));
}

static uint32_t read_le32(const uint8_t *ptr)
{
    return (uint32_t)ptr[0] |
           ((uint32_t)ptr[1] << 8) |
           ((uint32_t)ptr[2] << 16) |
           ((uint32_t)ptr[3] << 24);
}

static void write_le16(uint8_t *ptr, uint16_t value)
{
    ptr[0] = (uint8_t)(value & 0xffU);
    ptr[1] = (uint8_t)((value >> 8) & 0xffU);
}

static void write_le32(uint8_t *ptr, uint32_t value)
{
    ptr[0] = (uint8_t)(value & 0xffU);
    ptr[1] = (uint8_t)((value >> 8) & 0xffU);
    ptr[2] = (uint8_t)((value >> 16) & 0xffU);
    ptr[3] = (uint8_t)((value >> 24) & 0xffU);
}

static void write_le64(uint8_t *ptr, uint64_t value)
{
    for (size_t i = 0; i < 8; i += 1) {
        ptr[i] = (uint8_t)((value >> (i * 8U)) & 0xffU);
    }
}

static sc_status wav_to_mono_pcm(sc_allocator *alloc, const wav_info *wav, int16_t **samples_out, size_t *sample_count_out)
{
    size_t frame_bytes = 0;
    size_t frame_count = 0;
    int16_t *samples = nullptr;

    if (wav == nullptr || samples_out == nullptr || sample_count_out == nullptr) {
        return sc_status_invalid_argument("sc.media.opus.invalid_argument");
    }
    if (sc_size_mul_overflow((size_t)wav->channels, sizeof(int16_t), &frame_bytes) || frame_bytes == 0) {
        return sc_status_no_memory();
    }
    if ((wav->data_len % frame_bytes) != 0) {
        return sc_status_parse("sc.media.wav.malformed");
    }
    frame_count = wav->data_len / frame_bytes;
    if (frame_count == 0 || frame_count > SIZE_MAX / sizeof(*samples)) {
        return sc_status_security_denied("sc.media.opus.input_too_large");
    }

    samples = sc_alloc(alloc, frame_count * sizeof(*samples), _Alignof(int16_t));
    if (samples == nullptr) {
        return sc_status_no_memory();
    }
    for (size_t i = 0; i < frame_count; i += 1) {
        int32_t sum = 0;
        for (size_t channel = 0; channel < wav->channels; channel += 1) {
            size_t index = (i * (size_t)wav->channels + channel) * sizeof(int16_t);
            uint16_t raw = read_le16(wav->data + index);
            sum += (int16_t)raw;
        }
        samples[i] = (int16_t)(sum / (int32_t)wav->channels);
    }

    *samples_out = samples;
    *sample_count_out = frame_count;
    return sc_status_ok();
}

static sc_status resample_to_48k(sc_allocator *alloc,
                                 const int16_t *input,
                                 size_t input_count,
                                 uint32_t input_rate,
                                 int16_t **samples_out,
                                 size_t *sample_count_out)
{
    size_t output_count = 0;
    int16_t *samples = nullptr;

    if (input == nullptr || samples_out == nullptr || sample_count_out == nullptr || input_count == 0 || input_rate == 0) {
        return sc_status_invalid_argument("sc.media.opus.invalid_argument");
    }
    if (input_count > (SIZE_MAX / opus_output_sample_rate) - (size_t)input_rate) {
        return sc_status_security_denied("sc.media.opus.input_too_large");
    }
    output_count = ((input_count * opus_output_sample_rate) + (size_t)input_rate - 1U) / (size_t)input_rate;
    if (output_count == 0 || output_count > SIZE_MAX / sizeof(*samples)) {
        return sc_status_security_denied("sc.media.opus.input_too_large");
    }

    samples = sc_alloc(alloc, output_count * sizeof(*samples), _Alignof(int16_t));
    if (samples == nullptr) {
        return sc_status_no_memory();
    }
    for (size_t i = 0; i < output_count; i += 1) {
        size_t source = ((i * (size_t)input_rate) + (opus_output_sample_rate / 2U)) / opus_output_sample_rate;
        if (source >= input_count) {
            source = input_count - 1U;
        }
        samples[i] = input[source];
    }

    *samples_out = samples;
    *sample_count_out = output_count;
    return sc_status_ok();
}

static uint32_t ogg_serial_for_input(sc_buf wav)
{
    uint32_t hash = 2'166'136'261U;
    for (size_t i = 0; i < wav.len; i += 1) {
        hash ^= wav.ptr[i];
        hash *= 16'777'619U;
    }
    return hash == 0 ? 1U : hash;
}

static sc_status ogg_writer_init(ogg_writer *writer, sc_allocator *alloc, sc_buf wav, size_t max_output_bytes)
{
    if (writer == nullptr) {
        return sc_status_invalid_argument("sc.media.opus.invalid_argument");
    }
    sc_bytes_init(&writer->bytes, alloc);
    writer->max_output_bytes = max_output_bytes;
    writer->serial = ogg_serial_for_input(wav);
    writer->sequence = 0;
    return sc_status_ok();
}

static sc_status ogg_write_opus_headers(ogg_writer *writer, uint16_t pre_skip)
{
    sc_status status = ogg_write_opus_head(writer, pre_skip);
    if (sc_status_is_ok(status)) {
        status = ogg_write_opus_tags(writer);
    }
    return status;
}

static sc_status ogg_write_opus_head(ogg_writer *writer, uint16_t pre_skip)
{
    uint8_t packet[19] = {
        'O', 'p', 'u', 's', 'H', 'e', 'a', 'd',
        1,
        1,
        0, 0,
        0, 0, 0, 0,
        0, 0,
        0,
    };
    write_le16(packet + 10, pre_skip);
    write_le32(packet + 12, opus_output_sample_rate);
    return ogg_write_page(writer, sc_buf_from_parts(packet, sizeof(packet)), 0x02, 0);
}

static sc_status ogg_write_opus_tags(ogg_writer *writer)
{
    const char vendor[] = "SmolClaw";
    uint8_t packet[8 + 4 + sizeof(vendor) - 1 + 4] = {0};
    size_t offset = 0;

    (void)memcpy(packet + offset, "OpusTags", 8);
    offset += 8;
    write_le32(packet + offset, (uint32_t)(sizeof(vendor) - 1));
    offset += 4;
    (void)memcpy(packet + offset, vendor, sizeof(vendor) - 1);
    offset += sizeof(vendor) - 1;
    write_le32(packet + offset, 0);
    return ogg_write_page(writer, sc_buf_from_parts(packet, sizeof(packet)), 0, 0);
}

static sc_status ogg_write_page(ogg_writer *writer, sc_buf packet, uint8_t flags, uint64_t granule_position)
{
    uint8_t header[27 + ogg_max_lacing_segments] = {0};
    size_t lacing_count = 0;
    size_t remaining = packet.len;
    sc_status status;
    uint32_t crc = 0;
    size_t page_start = 0;

    if (writer == nullptr || (packet.len > 0 && packet.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.media.opus.invalid_argument");
    }
    lacing_count = (packet.len / 255U) + 1U;
    if (lacing_count > ogg_max_lacing_segments) {
        return sc_status_security_denied("sc.media.opus.packet_too_large");
    }

    (void)memcpy(header, "OggS", 4);
    header[5] = flags;
    write_le64(header + 6, granule_position);
    write_le32(header + 14, writer->serial);
    write_le32(header + 18, writer->sequence);
    header[26] = (uint8_t)lacing_count;
    for (size_t i = 0; i < lacing_count; i += 1) {
        uint8_t segment = remaining >= 255U ? 255U : (uint8_t)remaining;
        header[27 + i] = segment;
        remaining -= segment;
    }

    page_start = writer->bytes.len;
    status = append_checked(writer, sc_buf_from_parts(header, 27U + lacing_count));
    if (sc_status_is_ok(status)) {
        status = append_checked(writer, packet);
    }
    if (!sc_status_is_ok(status)) {
        return status;
    }

    crc = ogg_crc(sc_buf_from_parts(writer->bytes.ptr + page_start, writer->bytes.len - page_start));
    write_le32(writer->bytes.ptr + page_start + 22U, crc);
    writer->sequence += 1;
    return sc_status_ok();
}

static sc_status append_checked(ogg_writer *writer, sc_buf input)
{
    sc_status status = sc_bytes_append(&writer->bytes, input);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (writer->max_output_bytes > 0 && writer->bytes.len > writer->max_output_bytes) {
        return sc_status_security_denied("sc.media.opus.output_too_large");
    }
    return sc_status_ok();
}

static uint32_t ogg_crc(sc_buf bytes)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < bytes.len; i += 1) {
        crc = ogg_crc_update(crc, bytes.ptr[i]);
    }
    return crc;
}

static uint32_t ogg_crc_update(uint32_t crc, uint8_t value)
{
    crc ^= (uint32_t)value << 24U;
    for (size_t i = 0; i < 8; i += 1) {
        crc = (crc & 0x80000000U) != 0 ? (crc << 1U) ^ 0x04c11db7U : crc << 1U;
    }
    return crc;
}
#endif
