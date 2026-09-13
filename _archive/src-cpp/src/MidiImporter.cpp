#include "MidiImporter.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>

// Standard MIDI File (SMF) parser, written against the MIDI 1.0 file spec:
// big-endian chunk framing, variable-length delta times, running status,
// note-on-velocity-0-as-note-off. Only note timing/pitch/velocity and the
// tempo/track-name meta events are extracted; everything else (CC, pitch
// bend, sysex, ...) is skipped by its spec-defined length since the additive
// engine has nothing to map it onto yet.

namespace {

struct ByteReader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;

    bool ok(size_t n = 1) const { return pos + n <= size; }
    uint8_t u8() { return data[pos++]; }
    uint8_t peek() const { return data[pos]; }
    uint16_t u16() { uint16_t v = (data[pos] << 8) | data[pos + 1]; pos += 2; return v; }
    uint32_t u32() {
        uint32_t v = (static_cast<uint32_t>(data[pos]) << 24) | (data[pos + 1] << 16) |
                     (data[pos + 2] << 8) | data[pos + 3];
        pos += 4;
        return v;
    }
    // Variable-length quantity: 7 bits per byte, MSB set = continue (max 4 bytes).
    bool vlq(uint32_t& out) {
        out = 0;
        for (int i = 0; i < 4 && ok(); ++i) {
            uint8_t b = u8();
            out = (out << 7) | (b & 0x7F);
            if (!(b & 0x80)) return true;
        }
        return false;
    }
};

// A sounding note awaiting its note-off. Same-pitch overlaps are stacked
// LIFO — matches how most sequencers pair them, and either pairing yields
// identical total coverage on the piano roll anyway.
struct PendingNote {
    uint32_t startTick;
    uint8_t velocity;
};

} // namespace

namespace MidiImporter {

std::optional<Result> ImportFile(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "MidiImporter: could not open " << filePath << "\n";
        return std::nullopt;
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(std::max<std::streamsize>(size, 0)));
    if (size <= 0 || !file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        std::cerr << "MidiImporter: failed to read " << filePath << "\n";
        return std::nullopt;
    }

    ByteReader r{bytes.data(), bytes.size()};
    if (!r.ok(14) || r.u32() != 0x4D546864 /* 'MThd' */) {
        std::cerr << "MidiImporter: not a MIDI file (missing MThd)\n";
        return std::nullopt;
    }
    uint32_t headerLen = r.u32();
    if (headerLen < 6 || !r.ok(headerLen)) {
        std::cerr << "MidiImporter: malformed MThd\n";
        return std::nullopt;
    }
    r.u16(); // format (0/1/2) — irrelevant here: all MTrk chunks are parsed the same way
    uint16_t trackCount = r.u16();
    uint16_t division = r.u16();
    r.pos += headerLen - 6; // skip any spec-future header extension bytes
    if (division & 0x8000) {
        std::cerr << "MidiImporter: SMPTE time division not supported\n";
        return std::nullopt;
    }
    if (division == 0) {
        std::cerr << "MidiImporter: invalid division 0\n";
        return std::nullopt;
    }
    const float ticksPerBeat = static_cast<float>(division);

    Result result;

    for (uint16_t trackIdx = 0; trackIdx < trackCount && r.ok(8); ++trackIdx) {
        uint32_t chunkType = r.u32();
        uint32_t chunkLen = r.u32();
        if (!r.ok(chunkLen)) {
            std::cerr << "MidiImporter: truncated chunk in track " << trackIdx << "\n";
            break; // keep whatever complete tracks we already have
        }
        size_t chunkEnd = r.pos + chunkLen;
        if (chunkType != 0x4D54726B /* 'MTrk' */) {
            r.pos = chunkEnd; // alien chunk: spec says skip it
            --trackIdx;       // doesn't count against MThd's track count
            continue;
        }

        std::string trackName;
        // Per-channel accumulation: format 0 packs a whole arrangement's
        // channels into one MTrk, and even format 1 tracks occasionally carry
        // stray events on a second channel.
        std::array<std::vector<Note>, 16> channelNotes;
        std::array<std::array<std::vector<PendingNote>, 128>, 16> pending;

        uint32_t tick = 0;
        uint8_t runningStatus = 0;
        bool trackTerminated = false;

        auto closeNote = [&](int ch, int pitch, uint32_t endTick) {
            auto& stack = pending[ch][pitch];
            if (stack.empty()) return; // unmatched note-off: ignore
            PendingNote pn = stack.back();
            stack.pop_back();
            Note n;
            n.startBeat = static_cast<float>(pn.startTick) / ticksPerBeat;
            n.lengthBeats = std::max(0.01f, static_cast<float>(endTick - pn.startTick) / ticksPerBeat);
            n.pitch = static_cast<uint8_t>(pitch);
            n.velocity = pn.velocity;
            channelNotes[ch].push_back(n);
        };

        while (r.pos < chunkEnd && !trackTerminated) {
            uint32_t delta;
            if (!r.vlq(delta) || r.pos >= chunkEnd) break;
            tick += delta;

            uint8_t status = r.peek();
            if (status & 0x80) {
                r.u8();
                if (status < 0xF0) runningStatus = status;
            } else {
                if (runningStatus == 0) {
                    std::cerr << "MidiImporter: data byte with no running status in track " << trackIdx << "\n";
                    break;
                }
                status = runningStatus;
            }

            if (status == 0xFF) { // meta event
                if (!r.ok(1)) break;
                uint8_t metaType = r.u8();
                uint32_t len;
                if (!r.vlq(len) || !r.ok(len)) break;
                size_t dataStart = r.pos;
                if (metaType == 0x2F) { // End of Track
                    trackTerminated = true;
                } else if (metaType == 0x51 && len >= 3 && result.bpm == 0.0f) { // Set Tempo
                    uint32_t usPerQuarter = (bytes[dataStart] << 16) | (bytes[dataStart + 1] << 8) | bytes[dataStart + 2];
                    if (usPerQuarter > 0) result.bpm = 60000000.0f / static_cast<float>(usPerQuarter);
                } else if (metaType == 0x03 && trackName.empty()) { // Sequence/Track Name
                    trackName.assign(reinterpret_cast<const char*>(bytes.data() + dataStart), len);
                }
                r.pos = dataStart + len;
            } else if (status == 0xF0 || status == 0xF7) { // sysex: VLQ length, then payload
                uint32_t len;
                if (!r.vlq(len) || !r.ok(len)) break;
                r.pos += len;
            } else if (status >= 0x80 && status < 0xF0) { // channel voice message
                uint8_t kind = status & 0xF0;
                int ch = status & 0x0F;
                int dataLen = (kind == 0xC0 || kind == 0xD0) ? 1 : 2;
                if (!r.ok(dataLen)) break;
                uint8_t d1 = r.u8();
                uint8_t d2 = dataLen == 2 ? r.u8() : 0;

                if (kind == 0x90 && d2 > 0) {
                    pending[ch][d1 & 0x7F].push_back({tick, static_cast<uint8_t>(d2 & 0x7F)});
                } else if (kind == 0x80 || (kind == 0x90 && d2 == 0)) {
                    closeNote(ch, d1 & 0x7F, tick);
                }
                // CC / program / aftertouch / pitch bend: consumed above, unused
            } else {
                // System common/realtime (0xF1-0xF6, 0xF8-0xFE) — essentially
                // never stored in files; 0xF1/0xF3 carry 1 data byte, 0xF2
                // carries 2, the rest none.
                int dataLen = (status == 0xF2) ? 2 : (status == 0xF1 || status == 0xF3) ? 1 : 0;
                if (!r.ok(dataLen)) break;
                r.pos += dataLen;
            }
        }

        // Anything still sounding at end-of-track gets closed at the last tick
        // (a common quirk of loop-exported files that omit final note-offs).
        for (int ch = 0; ch < 16; ++ch)
            for (int pitch = 0; pitch < 128; ++pitch)
                while (!pending[ch][pitch].empty())
                    closeNote(ch, pitch, tick);

        int channelsWithNotes = 0;
        for (const auto& notes : channelNotes)
            if (!notes.empty()) ++channelsWithNotes;

        for (int ch = 0; ch < 16; ++ch) {
            if (channelNotes[ch].empty()) continue;
            ImportedTrack t;
            t.isPercussion = (ch == 9); // MIDI channel 10
            if (!trackName.empty())
                t.name = trackName;
            else
                t.name = "MIDI Ch " + std::to_string(ch + 1);
            if (!trackName.empty() && channelsWithNotes > 1)
                t.name += " (Ch " + std::to_string(ch + 1) + ")";
            if (t.isPercussion)
                t.name += " [drums]";
            std::sort(channelNotes[ch].begin(), channelNotes[ch].end(),
                      [](const Note& a, const Note& b) {
                          return a.startBeat < b.startBeat || (a.startBeat == b.startBeat && a.pitch < b.pitch);
                      });
            t.notes = std::move(channelNotes[ch]);
            result.tracks.push_back(std::move(t));
        }

        r.pos = chunkEnd; // re-sync even if this track's events were malformed
    }

    if (result.tracks.empty()) {
        std::cerr << "MidiImporter: no notes found in " << filePath << "\n";
        return std::nullopt;
    }
    return result;
}

} // namespace MidiImporter
