// 16-bit PCM WAV input/output and the header check of TWaveData (0x40d6e8).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace utagoe {

struct Audio {
    int rate = 44100;
    int channels = 2;
    std::vector<int16_t> data;  // interleaved
    size_t frames() const { return channels ? data.size() / channels : 0; }
};

struct WavInfo {
    int format_tag = 0, channels = 0, rate = 0, bits = 0;
};

enum { WAV_OK = 0, WAV_OPEN_FAILED = 1, WAV_BAD_FILE = 2, WAV_BAD_FORMAT = 3 };

// 0 or WAV_OPEN_FAILED / WAV_BAD_FILE (not RIFF WAVE, damaged) / WAV_BAD_FORMAT
// (not PCM or more than two channels).
int wav_info(const std::wstring& path, WavInfo* info);

bool read_wav(const std::wstring& path, Audio* out);   // 16-bit PCM only
bool write_wav(const std::wstring& path, const Audio& audio);

}  // namespace utagoe
