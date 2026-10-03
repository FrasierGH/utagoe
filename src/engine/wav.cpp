#include "wav.hpp"

#include <cstdio>
#include <cstring>

namespace utagoe {

namespace {

struct File {
    FILE* f;
    explicit File(const std::wstring& path, const wchar_t* mode) : f(_wfopen(path.c_str(), mode)) {}
    ~File() { if (f) fclose(f); }
};

uint32_t u32(const unsigned char* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t u16(const unsigned char* p) { return (uint16_t)(p[0] | p[1] << 8); }

// Finds the fmt and data chunks.  Returns WAV_* code.
int scan(FILE* f, WavInfo* info, long* data_pos, uint32_t* data_len) {
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) return WAV_BAD_FILE;
    bool have_fmt = false;
    for (;;) {
        unsigned char ck[8];
        if (fread(ck, 1, 8, f) != 8) break;
        uint32_t len = u32(ck + 4);
        long body = ftell(f);
        if (!memcmp(ck, "fmt ", 4)) {
            unsigned char fmt[16];
            if (fread(fmt, 1, 16, f) != 16) return WAV_OPEN_FAILED;
            info->format_tag = u16(fmt);
            info->channels = u16(fmt + 2);
            info->rate = (int)u32(fmt + 4);
            info->bits = u16(fmt + 14);
            have_fmt = true;
        } else if (!memcmp(ck, "data", 4)) {
            if (!have_fmt) return WAV_BAD_FILE;
            *data_pos = body;
            *data_len = len;
            if (info->format_tag != 1 || info->channels > 2 || info->channels < 1) return WAV_BAD_FORMAT;
            return WAV_OK;
        }
        if (fseek(f, body + (long)len + (long)(len & 1), SEEK_SET)) break;
    }
    return WAV_BAD_FILE;
}

}  // namespace

int wav_info(const std::wstring& path, WavInfo* info) {
    File f(path, L"rb");
    if (!f.f) return WAV_OPEN_FAILED;
    long pos;
    uint32_t len;
    return scan(f.f, info, &pos, &len);
}

bool read_wav(const std::wstring& path, Audio* out) {
    File f(path, L"rb");
    if (!f.f) return false;
    WavInfo info;
    long pos;
    uint32_t len;
    if (scan(f.f, &info, &pos, &len) != WAV_OK || info.bits != 16) return false;
    fseek(f.f, 0, SEEK_END);
    long end = ftell(f.f);
    if (pos + (long)len > end) len = (uint32_t)(end - pos);  // truncated file: take what is there
    size_t frames = len / (2u * info.channels);
    out->rate = info.rate;
    out->channels = info.channels;
    out->data.assign(frames * info.channels, 0);
    fseek(f.f, pos, SEEK_SET);
    return fread(out->data.data(), 2, out->data.size(), f.f) == out->data.size();
}

bool write_wav(const std::wstring& path, const Audio& a) {
    File f(path, L"wb");
    if (!f.f) return false;
    uint32_t data_len = (uint32_t)(a.data.size() * 2);
    unsigned char h[44];
    auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; i++) h[at + i] = (unsigned char)(v >> (8 * i)); };
    auto put16 = [&](int at, uint16_t v) { h[at] = (unsigned char)v; h[at + 1] = (unsigned char)(v >> 8); };
    memcpy(h, "RIFF", 4);
    put32(4, 36 + data_len);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 1);
    put16(22, (uint16_t)a.channels);
    put32(24, (uint32_t)a.rate);
    put32(28, (uint32_t)(a.rate * a.channels * 2));
    put16(32, (uint16_t)(a.channels * 2));
    put16(34, 16);
    memcpy(h + 36, "data", 4);
    put32(40, data_len);
    if (fwrite(h, 1, 44, f.f) != 44) return false;
    return fwrite(a.data.data(), 2, a.data.size(), f.f) == a.data.size();
}

}  // namespace utagoe
