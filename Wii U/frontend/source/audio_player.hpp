#pragma once

#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <sndcore2/core.h>
#include <sndcore2/device.h>
#include <sndcore2/voice.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace wutaudio {

struct StereoTrack {
    int16_t* left = nullptr;
    int16_t* right = nullptr;
    uint32_t numSamples = 0;
    uint32_t sampleRate = 24000;
    bool loop = false;

    void Free() {
        if (left) {
            MEMFreeToDefaultHeap(left);
            left = nullptr;
        }
        if (right) {
            MEMFreeToDefaultHeap(right);
            right = nullptr;
        }
        numSamples = 0;
    }
};

class AudioManager {
public:
    AudioManager() = default;
    ~AudioManager() { Shutdown(); }

    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    bool Init() {
        if (!InitAxAndIntro()) return false;
        LoadBgm();
        return true;
    }

    // Inicializa AX, carga solo intro.wia (~158 KB, <20 ms) para que Pantalla Inicio suene al instante
    bool InitAxAndIntro() {
        if (!axInit_) {
            AXInitParams params{};
            params.renderer = AX_INIT_RENDERER_48KHZ;
            params.pipeline = AX_INIT_PIPELINE_SINGLE;
            AXInitWithParams(&params);
            axInit_ = true;
        }
        if (!introLoaded_) {
            introLoaded_ = LoadWiaTrack("/vol/content/intro.wia",
                                        "fs:/vol/external01/wiiu/apps/WiiU_Transfer/intro.wia",
                                        false, introTrack_);
        }
        return true;
    }

    // Decodifica bgm.wia mientras ya suena Pantalla Inicio en segundo plano
    bool LoadBgm() {
        if (!axInit_) return false;
        if (!bgmLoaded_) {
            bgmLoaded_ = LoadWiaTrack("/vol/content/bgm.wia",
                                      "fs:/vol/external01/wiiu/apps/WiiU_Transfer/bgm.wia",
                                      true, bgmTrack_);
        }
        return bgmLoaded_;
    }

    void Shutdown() {
        StopVoices();
        introTrack_.Free();
        bgmTrack_.Free();
        introLoaded_ = false;
        bgmLoaded_ = false;
        if (axInit_) {
            AXQuit();
            axInit_ = false;
        }
    }

    bool HasIntro() const { return introLoaded_ && introTrack_.numSamples > 0; }
    bool HasBgm() const { return bgmLoaded_ && bgmTrack_.numSamples > 0; }

    bool StartIntro() {
        if (!HasIntro()) return false;
        mode_ = Mode::Intro;
        return StartTrack(introTrack_, 0x8000);
    }

    bool StartBgm() {
        if (!HasBgm()) {
            StopVoices();
            mode_ = Mode::None;
            return false;
        }
        mode_ = Mode::Bgm;
        return StartTrack(bgmTrack_, 0x5800); // Volumen equilibrado de fondo (~69%)
    }

    bool IsIntroPlaying() const {
        if (mode_ != Mode::Intro || !voices_[0]) return false;
        return AXIsVoiceRunning(voices_[0]) != FALSE;
    }

    bool IsMuted() const { return muted_; }

    void SetMuted(bool mute) {
        if (muted_ == mute) return;
        muted_ = mute;
        ApplyCurrentMuteState();
    }

    void ToggleMuted() {
        SetMuted(!muted_);
    }

    // Llamar periodicamente o durante el splash para pasar de Intro a BGM si el intro termina solo
    void Update(bool hasForeground) {
        if (!axInit_) return;
        if (!hasForeground) {
            if (!pausedForBg_) {
                SetVoicesPlaying(false);
                pausedForBg_ = true;
            }
            return;
        }
        if (pausedForBg_) {
            pausedForBg_ = false;
            if (!muted_) {
                SetVoicesPlaying(true);
            }
        }
        if (mode_ == Mode::Intro && voices_[0]) {
            if (AXIsVoiceRunning(voices_[0]) == FALSE) {
                StartBgm();
            }
        }
    }

private:
    enum class Mode {
        None,
        Intro,
        Bgm
    };

    static constexpr int kImaIndexTable[16] = {
        -1, -1, -1, -1, 2, 4, 6, 8,
        -1, -1, -1, -1, 2, 4, 6, 8
    };

    static constexpr int kImaStepTable[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
        19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
        130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
        337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
        2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
        5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
    };

    static void DecodeImaChannel(const uint8_t* src, uint32_t numSamples, int16_t* dst) {
        int pred = 0;
        int idx = 0;
        uint32_t outIdx = 0;
        uint32_t byteCount = (numSamples + 1) / 2;

        for (uint32_t b = 0; b < byteCount && outIdx < numSamples; ++b) {
            uint8_t pack = src[b];
            for (int n = 0; n < 2 && outIdx < numSamples; ++n) {
                int nib = (n == 0) ? (pack & 0x0F) : ((pack >> 4) & 0x0F);
                int step = kImaStepTable[idx];

                int vpdiff = step >> 3;
                if (nib & 4) vpdiff += step;
                if (nib & 2) vpdiff += (step >> 1);
                if (nib & 1) vpdiff += (step >> 2);

                if (nib & 8) pred -= vpdiff;
                else         pred += vpdiff;

                if (pred > 32767)       pred = 32767;
                else if (pred < -32768) pred = -32768;

                idx += kImaIndexTable[nib];
                if (idx < 0)       idx = 0;
                else if (idx > 88) idx = 88;

                dst[outIdx++] = static_cast<int16_t>(pred);
            }
        }
    }

    static bool LoadWiaTrack(const char* primaryPath,
                             const char* fallbackPath,
                             bool loop,
                             StereoTrack& out) {
        out.Free();
        FILE* fp = std::fopen(primaryPath, "rb");
        if (!fp && fallbackPath) {
            fp = std::fopen(fallbackPath, "rb");
        }
        if (!fp) return false;

        uint8_t hdr[16]{};
        if (std::fread(hdr, 1, 16, fp) != 16) {
            std::fclose(fp);
            return false;
        }
        if (std::memcmp(hdr, "WIA1", 4) != 0) {
            std::fclose(fp);
            return false;
        }

        uint32_t sampleRate = (static_cast<uint32_t>(hdr[4]) << 24) |
                              (static_cast<uint32_t>(hdr[5]) << 16) |
                              (static_cast<uint32_t>(hdr[6]) << 8) |
                              static_cast<uint32_t>(hdr[7]);
        uint32_t numSamples = (static_cast<uint32_t>(hdr[8]) << 24) |
                              (static_cast<uint32_t>(hdr[9]) << 16) |
                              (static_cast<uint32_t>(hdr[10]) << 8) |
                              static_cast<uint32_t>(hdr[11]);

        if (sampleRate < 8000 || sampleRate > 96000 || numSamples < 64 || numSamples > 20000000) {
            std::fclose(fp);
            return false;
        }

        uint32_t chanAdpcmBytes = (numSamples + 1) / 2;
        uint32_t totalAdpcmBytes = chanAdpcmBytes * 2;
        auto* adpcmBuf = static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(totalAdpcmBytes, 0x40));
        if (!adpcmBuf) {
            std::fclose(fp);
            return false;
        }

        size_t got = std::fread(adpcmBuf, 1, totalAdpcmBytes, fp);
        std::fclose(fp);
        if (got != totalAdpcmBytes) {
            MEMFreeToDefaultHeap(adpcmBuf);
            return false;
        }

        uint32_t pcmBytes = numSamples * sizeof(int16_t);
        auto* leftPcm  = static_cast<int16_t*>(MEMAllocFromDefaultHeapEx(pcmBytes, 0x40));
        auto* rightPcm = static_cast<int16_t*>(MEMAllocFromDefaultHeapEx(pcmBytes, 0x40));
        if (!leftPcm || !rightPcm) {
            if (leftPcm)  MEMFreeToDefaultHeap(leftPcm);
            if (rightPcm) MEMFreeToDefaultHeap(rightPcm);
            MEMFreeToDefaultHeap(adpcmBuf);
            return false;
        }

        DecodeImaChannel(adpcmBuf, numSamples, leftPcm);
        DecodeImaChannel(adpcmBuf + chanAdpcmBytes, numSamples, rightPcm);
        MEMFreeToDefaultHeap(adpcmBuf);

        DCFlushRange(leftPcm, pcmBytes);
        DCFlushRange(rightPcm, pcmBytes);

        out.left = leftPcm;
        out.right = rightPcm;
        out.numSamples = numSamples;
        out.sampleRate = sampleRate;
        out.loop = loop;
        return true;
    }

    void StopVoices() {
        for (int c = 0; c < 2; ++c) {
            if (voices_[c]) {
                AXVoiceBegin(voices_[c]);
                AXSetVoiceState(voices_[c], AX_VOICE_STATE_STOPPED);
                AXVoiceEnd(voices_[c]);
                AXFreeVoice(voices_[c]);
                voices_[c] = nullptr;
            }
        }
    }

    void SetVoicesPlaying(bool play) {
        for (int c = 0; c < 2; ++c) {
            if (voices_[c]) {
                AXVoiceBegin(voices_[c]);
                AXSetVoiceState(voices_[c], play ? AX_VOICE_STATE_PLAYING : AX_VOICE_STATE_STOPPED);
                AXVoiceEnd(voices_[c]);
            }
        }
    }

    void ApplyCurrentMuteState() {
        uint16_t vol = muted_ ? 0 : activeVolume_;
        for (int c = 0; c < 2; ++c) {
            if (voices_[c]) {
                AXVoiceBegin(voices_[c]);
                AXVoiceVeData ve{};
                ve.volume = vol;
                ve.delta = 0;
                AXSetVoiceVe(voices_[c], &ve);
                if (!pausedForBg_ && mode_ == Mode::Bgm) {
                    AXSetVoiceState(voices_[c], muted_ ? AX_VOICE_STATE_STOPPED : AX_VOICE_STATE_PLAYING);
                }
                AXVoiceEnd(voices_[c]);
            }
        }
    }

    bool StartTrack(const StereoTrack& trk, uint16_t volume) {
        if (!axInit_ || !trk.left || !trk.right || trk.numSamples < 2) return false;
        StopVoices();
        activeVolume_ = volume;
        uint16_t initVol = muted_ ? 0 : volume;

        const int16_t* chPtrs[2] = {trk.left, trk.right};
        float inRate = static_cast<float>(AXGetInputSamplesPerSec());
        if (inRate <= 0.0f) inRate = 48000.0f;
        float ratio = static_cast<float>(trk.sampleRate) / inRate;

        for (int c = 0; c < 2; ++c) {
            voices_[c] = AXAcquireVoice(31, nullptr, nullptr);
            if (!voices_[c]) {
                StopVoices();
                return false;
            }

            AXVoiceBegin(voices_[c]);
            AXSetVoiceType(voices_[c], 0);

            AXVoiceVeData ve{};
            ve.volume = initVol;
            ve.delta = 0;
            AXSetVoiceVe(voices_[c], &ve);

            // Canal 0 -> Izquierdo (speaker 0), Canal 1 -> Derecho (speaker 1) en TV y GamePad
            AXVoiceDeviceMixData mix[6]{};
            mix[c].bus[0].volume = 0x8000;
            AXSetVoiceDeviceMix(voices_[c], AX_DEVICE_TYPE_TV, 0, mix);
            AXSetVoiceDeviceMix(voices_[c], AX_DEVICE_TYPE_DRC, 0, mix);

            AXVoiceOffsets off{};
            off.dataType = AX_VOICE_FORMAT_LPCM16;
            off.loopingEnabled = trk.loop ? AX_VOICE_LOOP_ENABLED : AX_VOICE_LOOP_DISABLED;
            off.loopOffset = 0;
            off.endOffset = trk.numSamples - 1;
            off.currentOffset = 0;
            off.data = chPtrs[c];
            AXSetVoiceOffsets(voices_[c], &off);

            AXSetVoiceSrcType(voices_[c], AX_VOICE_SRC_TYPE_LINEAR);
            AXSetVoiceSrcRatio(voices_[c], ratio);
            AXSetVoiceState(voices_[c], AX_VOICE_STATE_PLAYING);
            AXVoiceEnd(voices_[c]);
        }
        return true;
    }

    bool axInit_ = false;
    bool introLoaded_ = false;
    bool bgmLoaded_ = false;
    bool pausedForBg_ = false;
    bool muted_ = false;
    uint16_t activeVolume_ = 0x5800;
    Mode mode_ = Mode::None;
    StereoTrack introTrack_{};
    StereoTrack bgmTrack_{};
    AXVoice* voices_[2]{nullptr, nullptr};
};

} // namespace wutaudio
