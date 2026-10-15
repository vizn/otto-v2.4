#ifndef IVY_PLAYBACK_LOGIC_H
#define IVY_PLAYBACK_LOGIC_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ivy {

inline int RemainingReadBudgetMs(int64_t deadline_ms, int64_t now_ms) {
    int64_t remaining = deadline_ms - now_ms;
    return remaining <= 0 ? 0 : static_cast<int>(remaining > 15000 ? 15000 : remaining);
}

constexpr size_t kStudyEncodedLimit = 64 * 1024;

inline bool AppendStudyBytes(std::vector<uint8_t>& buffer, const uint8_t* data, size_t size) {
    if (buffer.size() > kStudyEncodedLimit || size > kStudyEncodedLimit - buffer.size()) return false;
    if (buffer.capacity() < kStudyEncodedLimit) buffer.reserve(kStudyEncodedLimit);
    buffer.insert(buffer.end(), data, data + size);
    return true;
}

class PlaybackGeneration {
public:
    uint32_t Advance() { return ++value_; }
    uint32_t Current() const { return value_.load(); }
    bool Matches(uint32_t token) const { return token != 0 && token == Current(); }

private:
    std::atomic<uint32_t> value_{0};
};

// Used only by the playback task; redundant queued stops must not notify twice.
class StopNotification {
public:
    void Begin(uint32_t owner) { owner_ = owner; pending_ = true; }
    uint32_t Owner() const { return owner_; }
    bool Take() {
        bool pending = pending_;
        pending_ = false;
        return pending;
    }

private:
    uint32_t owner_ = 0;
    bool pending_ = false;
};

inline bool Mp3MetadataPrefix(const uint8_t* data, size_t size) {
    return (size >= 3 && (!std::memcmp(data, "TAG", 3) || !std::memcmp(data, "ID3", 3))) ||
           (size >= 8 && !std::memcmp(data, "APETAGEX", 8));
}

// Locate a candidate without consuming skipped bytes or an incomplete frame.
inline bool FindNextMp3Frame(const std::vector<uint8_t>& buf, size_t start,
                             size_t& frame_len, size_t& skip) {
    static const uint16_t kBitrateV1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const uint16_t kBitrateV2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const uint32_t kSampleRateV1[4] = {44100, 48000, 32000, 0};
    static const uint32_t kSampleRateV2[4] = {22050, 24000, 16000, 0};
    static const uint32_t kSampleRateV25[4] = {11025, 12000, 8000, 0};
    for (size_t i = start; i + 4 <= buf.size(); ++i) {
        if (buf[i] != 0xff || (buf[i + 1] & 0xe0) != 0xe0) continue;
        int ver = (buf[i + 1] >> 3) & 3;
        int layer = (buf[i + 1] >> 1) & 3;
        int br_idx = (buf[i + 2] >> 4) & 15;
        int sr_idx = (buf[i + 2] >> 2) & 3;
        if (ver == 1 || layer == 0 || br_idx == 0 || br_idx == 15 || sr_idx == 3) continue;
        uint32_t rate = (ver == 3 ? kBitrateV1[br_idx] : kBitrateV2[br_idx]) * 1000u;
        uint32_t sample_rate = ver == 3 ? kSampleRateV1[sr_idx]
                              : ver == 2 ? kSampleRateV2[sr_idx] : kSampleRateV25[sr_idx];
        int padding = (buf[i + 2] >> 1) & 1;
        uint32_t bytes = layer == 3 ? (12u * rate / sample_rate + padding) * 4u
                                    : (ver == 3 ? 144u : 72u) * rate / sample_rate + padding;
        if (bytes < 4 || bytes > 16384) continue;
        skip = i - start;
        frame_len = bytes;
        return true;
    }
    return false;
}

inline bool NextStudyMp3Frame(const std::vector<uint8_t>& buffer, size_t offset, bool pcm_started,
                              bool& tail_pending, size_t& frame_len, size_t& skip) {
    if (tail_pending || offset >= buffer.size()) return false;
    if (pcm_started && Mp3MetadataPrefix(buffer.data() + offset, buffer.size() - offset)) {
        tail_pending = true;
        return false;
    }
    if (!FindNextMp3Frame(buffer, offset, frame_len, skip)) return false;
    // APE without a header starts with binary items, possibly containing false MPEG sync.
    // Once audio is synchronized, never resync destructively through a possible tail.
    if (pcm_started && skip) {
        tail_pending = true;
        return false;
    }
    return frame_len <= buffer.size() - offset - skip;
}

// A decoded prefix alone is not success: every remaining byte must be known metadata
// or zero padding. In particular a partial MPEG header/frame is never accepted.
inline bool CompleteMp3Tail(const uint8_t* data, size_t size) {
    while (size) {
        if (size >= 128 && !std::memcmp(data, "TAG", 3)) {
            data += 128;
            size -= 128;
            continue;
        }
        if (size >= 10 && !std::memcmp(data, "ID3", 3)) {
            if (data[3] < 2 || data[3] > 4 || data[4] == 0xff) return false;
            uint8_t allowed_flags = data[3] == 2 ? 0xc0 : data[3] == 3 ? 0xe0 : 0xf0;
            if (data[5] & ~allowed_flags) return false;
            size_t length = 0;
            for (size_t i = 6; i < 10; ++i) {
                if (data[i] & 0x80) return false;
                length = (length << 7) | data[i];
            }
            length += 10;
            if (length > size) return false;
            if (data[3] == 4 && (data[5] & 0x10)) {
                if (size - length < 10 || std::memcmp(data + length, "3DI", 3) ||
                    std::memcmp(data + length + 3, data + 3, 7)) return false;
                length += 10;
            }
            data += length;
            size -= length;
            continue;
        }
        // APEv2 size includes the footer and items, but excludes an optional header.
        size_t ape_end = size;
        if (size >= 128 && !std::memcmp(data + size - 128, "TAG", 3)) ape_end -= 128;
        if (ape_end >= 32 && !std::memcmp(data + ape_end - 32, "APETAGEX", 8)) {
            const uint8_t* footer = data + ape_end - 32;
            auto le32 = [](const uint8_t* p) {
                return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                       (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
            };
            uint32_t version = le32(footer + 8);
            uint32_t length = le32(footer + 12);
            uint32_t flags = le32(footer + 20);
            bool header = (flags & 0x80000000u) != 0;
            if ((version != 1000 && version != 2000) || length < 32 ||
                (flags & 0x60000000u) || size_t(length) + (header ? 32 : 0) != ape_end) return false;
            if (header && (std::memcmp(data, "APETAGEX", 8) ||
                           std::memcmp(data + 8, footer + 8, 12))) return false;
            data += ape_end;
            size -= ape_end;
            continue;
        }
        for (size_t i = 0; i < size; ++i) if (data[i] != 0) return false;
        return true;
    }
    return true;
}

inline bool StudyAudioComplete(bool pcm, bool decode_failed, size_t expected, size_t received,
                               const uint8_t* tail, size_t tail_size) {
    return pcm && !decode_failed && (!expected || received >= expected) &&
           CompleteMp3Tail(tail, tail_size);
}

}  // namespace ivy

#endif
