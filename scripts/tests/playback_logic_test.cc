#include "playback_logic.h"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#ifdef IVY_PVMP3_HOST
#include "pvmp3_framedecoder.h"
#endif

static void TestEof() {
    auto complete = [](const std::vector<uint8_t>& tail, size_t expected = 100, size_t received = 100) {
        return ivy::StudyAudioComplete(true, false, expected, received, tail.data(), tail.size());
    };
    assert(complete({}));
    assert(complete({}, 0, 100));
    assert(!complete({}, 101, 100));
    assert(!ivy::StudyAudioComplete(false, false, 100, 100, nullptr, 0));
    assert(!ivy::StudyAudioComplete(true, true, 100, 100, nullptr, 0));
    for (size_t size : {size_t(1), size_t(2), size_t(3), size_t(4), size_t(100), size_t(416)}) {
        std::vector<uint8_t> frame(size, 0);
        frame[0] = 0xff;
        if (size > 1) frame[1] = 0xfb;
        if (size > 2) frame[2] = 0x90;  // MPEG1 Layer3, 128kbps, 44.1kHz: 417 bytes.
        assert(!complete(frame));  // Even when Content-Length was fully received.
    }
    std::vector<uint8_t> id3v1(128, 0);
    id3v1[0] = 'T'; id3v1[1] = 'A'; id3v1[2] = 'G';
    assert(complete(id3v1));
    auto partial = id3v1;
    partial.pop_back();
    assert(!complete(partial));
    auto bad_tail = id3v1;
    bad_tail.push_back(0xff);
    assert(!complete(bad_tail));
    std::vector<uint8_t> id3v2{'I', 'D', '3', 4, 0, 0, 0, 0, 0, 2, 1, 2};
    assert(complete(id3v2));
    partial = id3v2;
    partial.pop_back();
    assert(!complete(partial));
    partial = id3v2;
    partial[6] = 0x80;
    assert(!complete(partial));
    partial = id3v2;
    partial[5] = 1;  // Reserved flag bit is not a valid metadata tail.
    assert(!complete(partial));
    auto footer_tag = id3v2;
    footer_tag[5] = 0x10;
    footer_tag.insert(footer_tag.end(), {'3', 'D', 'I', 4, 0, 0x10, 0, 0, 0, 2});
    assert(complete(footer_tag));
    footer_tag.back() = 3;
    assert(!complete(footer_tag));
    std::vector<uint8_t> ape(32, 0);
    const std::string magic = "APETAGEX";
    std::copy(magic.begin(), magic.end(), ape.begin());
    ape[8] = 0xd0; ape[9] = 7;  // Version 2000.
    ape[12] = 32;
    assert(complete(ape));
    auto both = ape;
    both.insert(both.end(), id3v1.begin(), id3v1.end());
    assert(complete(both));
    ape[12] = 33;
    assert(!complete(ape));
    assert(complete({0, 0, 0}));
    assert(!complete({1, 2, 3}));
    assert(ivy::Mp3MetadataPrefix(id3v1.data(), id3v1.size()));
    assert(ivy::Mp3MetadataPrefix(id3v2.data(), id3v2.size()));
}

static void TestCancellation() {
    ivy::PlaybackGeneration generation;
    assert(!generation.Matches(0));
    uint32_t request = generation.Advance();
    assert(generation.Matches(request));
    std::atomic<bool> ready{false}, release{false}, committed{false};
    std::thread worker([&]() {
        assert(generation.Matches(request));
        ready = true;
        while (!release.load()) std::this_thread::yield();
        if (generation.Matches(request)) committed = true;
    });
    while (!ready.load()) std::this_thread::yield();
    uint32_t replacement = generation.Advance();
    release = true;
    worker.join();
    assert(!committed.load());
    assert(!generation.Matches(request));
    assert(generation.Matches(replacement));
    auto copied_result = [&]() { return generation.Matches(replacement); };
    generation.Advance();  // Explicit stop before main-task result/asset commit.
    assert(!copied_result());
    assert(!generation.Matches(replacement));
}

static void TestReadBudget() {
    const int64_t deadline = 15000;
    assert(ivy::RemainingReadBudgetMs(deadline, 0) == 15000);
    // A 200 response at 200ms must allow a body delayed another 1.2 seconds.
    assert(ivy::RemainingReadBudgetMs(deadline, 200) == 14800);
    assert(ivy::RemainingReadBudgetMs(deadline, 200) > 1200);
    assert(ivy::RemainingReadBudgetMs(deadline, 14000) == 1000);
    assert(ivy::RemainingReadBudgetMs(deadline, 14999) == 1);
    assert(ivy::RemainingReadBudgetMs(deadline, 15000) == 0);
    assert(ivy::RemainingReadBudgetMs(deadline, 16000) == 0);
}

static void TestApePipeline() {
    std::vector<uint8_t> frame(417, 0);
    frame[0] = 0xff; frame[1] = 0xfb; frame[2] = 0x90;
    auto put32 = [](std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
        for (int i = 0; i < 4; ++i) bytes[offset + i] = (value >> (8 * i)) & 0xff;
    };
    auto ape_tag = [&](size_t value_size) {
        std::vector<uint8_t> tag(8, 0);
        put32(tag, 0, value_size);
        put32(tag, 4, 2);  // APE binary item.
        tag.insert(tag.end(), {'b', 'i', 'n', 'a', 'r', 'y', 0});
        size_t value = tag.size();
        tag.resize(value + value_size, 0x12);
        tag[value + 5] = 0xff; tag[value + 6] = 0xfb;
        tag[value + 7] = 0x90; tag[value + 8] = 0;
        size_t footer = tag.size();
        tag.resize(footer + 32, 0);
        const std::string magic = "APETAGEX";
        std::copy(magic.begin(), magic.end(), tag.begin() + footer);
        put32(tag, footer + 8, 2000);
        put32(tag, footer + 12, tag.size());
        put32(tag, footer + 16, 1);
        return tag;  // No optional APE header.
    };
    auto run = [](const std::vector<uint8_t>& stream, size_t chunk_size,
                  const std::vector<uint8_t>& expected_tail, bool completed) {
        std::vector<uint8_t> buffer;
        bool pcm_started = false, tail_pending = false;
        size_t frames = 0;
        for (size_t position = 0; position < stream.size();) {
            size_t count = std::min(chunk_size, stream.size() - position);
            assert(ivy::AppendStudyBytes(buffer, stream.data() + position, count));
            position += count;
            size_t offset = 0, frame_len = 0, skip = 0;
            while (ivy::NextStudyMp3Frame(buffer, offset, pcm_started, tail_pending, frame_len, skip)) {
                assert(skip == 0);
                assert(frame_len == 417);
                // Stand in for successful PCM decoding; candidate selection is the firmware's code.
                offset += frame_len;
                pcm_started = true;
                ++frames;
            }
            buffer.erase(buffer.begin(), buffer.begin() + offset);
        }
        assert(frames == 2);  // The binary item's false sync must never reach the decoder.
        assert(buffer == expected_tail);  // Every byte before the footer survived scanning.
        assert(ivy::StudyAudioComplete(pcm_started, false, stream.size(), stream.size(),
                                      buffer.data(), buffer.size()) == completed);
    };
    for (size_t value_size : {size_t(24), size_t(512)}) {
        auto tag = ape_tag(value_size);
        size_t length = 0, skip = 0;
        assert(ivy::FindNextMp3Frame(tag, 0, length, skip));
        assert(skip > 0 && length == 417);  // Reproduce the original destructive scan.
        for (size_t chunk : {size_t(1), size_t(3), size_t(4), size_t(64), size_t(4096)}) {
            auto stream = frame;
            stream.insert(stream.end(), frame.begin(), frame.end());
            stream.insert(stream.end(), tag.begin(), tag.end());
            run(stream, chunk, tag, true);
            stream.pop_back();
            auto truncated = tag;
            truncated.pop_back();
            run(stream, chunk, truncated, false);
        }
    }
    auto partial_frame = frame;
    partial_frame.resize(100);
    auto stream = frame;
    stream.insert(stream.end(), frame.begin(), frame.end());
    stream.insert(stream.end(), partial_frame.begin(), partial_frame.end());
    run(stream, 64, partial_frame, false);

    std::vector<uint8_t> bounded;
    std::vector<uint8_t> garbage(ivy::kStudyEncodedLimit, 0x12);
    assert(ivy::AppendStudyBytes(bounded, garbage.data(), garbage.size()));
    bool tail = false;
    size_t length = 0, skip = 0;
    assert(!ivy::NextStudyMp3Frame(bounded, 0, true, tail, length, skip));
    assert(!ivy::AppendStudyBytes(bounded, garbage.data(), 1));
    assert(bounded == garbage && bounded.capacity() == ivy::kStudyEncodedLimit);
    assert(!ivy::StudyAudioComplete(true, false, bounded.size(), bounded.size(),
                                   bounded.data(), bounded.size()));
}

static void TestStopOwner() {
    ivy::PlaybackGeneration owners;
    ivy::StopNotification stopped;
    assert(!stopped.Take());
    uint32_t study = owners.Advance();
    stopped.Begin(study);
    assert(stopped.Take());
    uint32_t callback_owner = stopped.Owner();
    assert(!stopped.Take());  // Redundant queued kStop after stream exit.
    uint32_t music = owners.Advance();
    assert(!owners.Matches(callback_owner));  // Delayed study callback cannot stop new dance.
    stopped.Begin(music);
    assert(stopped.Take());
    assert(owners.Matches(stopped.Owner()));
    assert(!stopped.Take());
}

static int AnalyzeArtifact(const char* path, size_t chunk_size) {
    if (!chunk_size || chunk_size > 4096) return 2;
    std::ifstream input(path, std::ios::binary);
    if (!input) return 2;
    std::vector<uint8_t> buffer;
    std::vector<char> chunk(chunk_size);
    bool pcm_started = false, tail_pending = false, overflow = false;
    size_t received = 0, consumed = 0, frames = 0;
    double seconds = 0;
#ifdef IVY_PVMP3_HOST
    std::vector<uint64_t> decoder_memory((pvmp3_decoderMemRequirements() + 7) / 8);
    tPVMP3DecoderExternal decoder{};
    pvmp3_InitDecoder(&decoder, decoder_memory.data());
    std::vector<int16_t> pcm(4096);
#endif
    while (input.read(chunk.data(), chunk.size()) || input.gcount()) {
        size_t count = input.gcount();
        received += count;
        if (!ivy::AppendStudyBytes(buffer, reinterpret_cast<const uint8_t*>(chunk.data()), count)) {
            overflow = true;
            break;
        }
        size_t offset = 0, length = 0, skip = 0;
        bool was_pending = tail_pending;
        while (ivy::NextStudyMp3Frame(buffer, offset, pcm_started, tail_pending, length, skip)) {
            size_t start = offset + skip;
#ifdef IVY_PVMP3_HOST
            decoder.pInputBuffer = buffer.data() + start;
            decoder.inputBufferCurrentLength = buffer.size() - start;
            decoder.inputBufferUsedLength = 0;
            decoder.outputFrameSize = pcm.size();
            decoder.pOutputBuffer = pcm.data();
            int error = pvmp3_framedecoder(&decoder, decoder_memory.data());
            if (error) {
                std::cout << "pv_error=" << error << " offset=" << consumed + start
                          << " frame_seconds=" << seconds << " available=" << buffer.size() - start
                          << " frame_length=" << length << " used=" << decoder.inputBufferUsedLength << '\n';
                tail_pending = true;
                break;
            }
#endif
            int ver = (buffer[start + 1] >> 3) & 3;
            int sr = (buffer[start + 2] >> 2) & 3;
            const int rates[4] = {44100, 48000, 32000, 0};
            int rate = rates[sr] / (ver == 3 ? 1 : ver == 2 ? 2 : 4);
            seconds += double(ver == 3 ? 1152 : 576) / rate;
            offset = start + length;
#ifdef IVY_PVMP3_HOST
            offset = start + decoder.inputBufferUsedLength;
#endif
            pcm_started = true;
            ++frames;
        }
        if (!was_pending && tail_pending) {
            std::cout << "first_tail_offset=" << consumed + offset << " frame=" << frames
                      << " frame_seconds=" << seconds << " prefix=";
            for (size_t i = offset; i < buffer.size() && i < offset + 16; ++i) {
                std::cout << std::hex << std::setw(2) << std::setfill('0') << unsigned(buffer[i]);
            }
            std::cout << std::dec << '\n';
        }
        consumed += offset;
        buffer.erase(buffer.begin(), buffer.begin() + offset);
    }
    bool completed = !overflow && input.eof() && ivy::StudyAudioComplete(
        pcm_started, false, received, received, buffer.data(), buffer.size());
    std::cout << "chunk=" << chunk_size << " received=" << received << " consumed=" << consumed
              << " frames=" << frames << " frame_seconds=" << seconds << " tail=" << buffer.size()
              << " overflow=" << overflow << " completed=" << completed << '\n';
    return completed ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "artifact") {
        return AnalyzeArtifact(argv[2], std::stoul(argv[3]));
    }
    assert(argc == 2);
    std::string test = argv[1];
    if (test == "eof") TestEof();
    else if (test == "cancellation") TestCancellation();
    else if (test == "stop_owner") TestStopOwner();
    else if (test == "read_budget") TestReadBudget();
    else if (test == "ape_pipeline") TestApePipeline();
    else return 1;
}
