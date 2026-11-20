#pragma once

#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memfrmheap.h>
#include <coreinit/memheap.h>
#include <coreinit/memory.h>
#include <coreinit/screen.h>
#include <proc_ui/procui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "font_atlas.hpp"

namespace wutui {

constexpr int LOGICAL_W = 1280;
constexpr int LOGICAL_H = 720;
constexpr int TV_W = 1280;
constexpr int TV_H = 720;
constexpr int DRC_W = 854;
constexpr int DRC_H = 480;

// Paleta Tema Oscuro en formato RGBX32 big-endian (0xRRGGBB00)
constexpr uint32_t COL_BG         = 0x0F141B00;
constexpr uint32_t COL_HEADER     = 0x0B395400;
constexpr uint32_t COL_CARD       = 0x18202B00;
constexpr uint32_t COL_CARD_ALT   = 0x1F293700;
constexpr uint32_t COL_BOX_SOFT   = 0x12192200;
constexpr uint32_t COL_BTN_OFF    = 0x25314100;
constexpr uint32_t COL_BORDER     = 0x2E3D5000;
constexpr uint32_t COL_CYAN       = 0x00AEEF00;
constexpr uint32_t COL_CYAN_DK    = 0x4DD4FF00;
constexpr uint32_t COL_CYAN_LT    = 0x11384D00;
constexpr uint32_t COL_CYAN_SUB   = 0xA8D8EC00;
constexpr uint32_t COL_GREEN      = 0x17A95B00;
constexpr uint32_t COL_GREEN_HI   = 0x2FE08200;
constexpr uint32_t COL_RED        = 0xD92B3A00;
constexpr uint32_t COL_RED_DARK   = 0x8E1B2500;
constexpr uint32_t COL_TEXT       = 0xECF3F900;
constexpr uint32_t COL_MUTED      = 0x8EA3B800;
constexpr uint32_t COL_WHITE      = 0xFFFFFF00;
constexpr uint32_t COL_PILL_OFF   = 0x07263800;
constexpr uint32_t COL_FOOTER     = 0x131A2300;
constexpr uint32_t COL_TRACK      = 0x27344500;

enum class FontId {
    Header,
    Title,
    Body,
    Small
};

class UiCanvas {
public:
    static constexpr uint32_t kFrameHeapTag = 0x57555452; // "WUTR"

    bool Init() {
        fb_ = static_cast<uint32_t*>(
            MEMAllocFromDefaultHeapEx(static_cast<uint32_t>(LOGICAL_W * LOGICAL_H * sizeof(uint32_t)), 0x40));
        if (!fb_) return false;

        OSScreenInit();
        tvSize_  = OSScreenGetBufferSizeEx(SCREEN_TV);
        drcSize_ = OSScreenGetBufferSizeEx(SCREEN_DRC);

        if (OnProcAcquire(this) != 0 || !tvBuf_ || !drcBuf_) {
            Shutdown();
            return false;
        }
        resumedFromBg_ = false;

        OSScreenEnableEx(SCREEN_TV, TRUE);
        OSScreenEnableEx(SCREEN_DRC, TRUE);

        ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, OnProcAcquire, this, 100);
        ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, OnProcRelease, this, 100);
        return true;
    }

    void Shutdown() {
        if (hasForeground_) {
            OSScreenShutdown();
            OnProcRelease(this);
        }
        if (fb_) {
            MEMFreeToDefaultHeap(fb_);
            fb_ = nullptr;
        }
    }

    bool HasForeground() const { return hasForeground_; }

    bool ConsumeResumedFlag() {
        bool v = resumedFromBg_;
        resumedFromBg_ = false;
        return v;
    }

    void Clear(uint32_t col) {
        if (!fb_) return;
        std::fill_n(fb_, LOGICAL_W * LOGICAL_H, col);
    }

    void Present() {
        if (!hasForeground_ || !tvBuf_ || !drcBuf_ || !fb_) return;

        uint32_t tvHalfBytes = tvSize_ / 2;
        uint32_t drcHalfBytes = drcSize_ / 2;
        auto* tvWork = reinterpret_cast<uint32_t*>(
            static_cast<uint8_t*>(tvBuf_) + static_cast<size_t>(tvWorkIdx_) * tvHalfBytes);
        auto* drcWork = reinterpret_cast<uint32_t*>(
            static_cast<uint8_t*>(drcBuf_) + static_cast<size_t>(drcWorkIdx_) * drcHalfBytes);

        // 1. TV (1280x720 nativo 1:1 - copia directa sin llamadas por pixel)
        if (tvStride_ == static_cast<uint32_t>(TV_W)) {
            std::memcpy(tvWork, fb_, static_cast<size_t>(TV_W * TV_H) * sizeof(uint32_t));
        } else {
            for (int y = 0; y < TV_H; ++y) {
                std::memcpy(tvWork + static_cast<size_t>(y) * tvStride_,
                            fb_ + static_cast<size_t>(y) * TV_W,
                            static_cast<size_t>(TV_W) * sizeof(uint32_t));
            }
        }

        // 2. GamePad DRC (854x480 - box-filter 2x2 suave desde 720p)
        for (int dy = 0; dy < DRC_H; ++dy) {
            int sy0 = (dy * 3) / 2;
            int sy1 = (sy0 + 1 < LOGICAL_H) ? (sy0 + 1) : sy0;
            const uint32_t* r0 = fb_ + static_cast<size_t>(sy0) * LOGICAL_W;
            const uint32_t* r1 = fb_ + static_cast<size_t>(sy1) * LOGICAL_W;
            uint32_t* dst = drcWork + static_cast<size_t>(dy) * drcStride_;

            for (int dx = 0; dx < DRC_W; ++dx) {
                int sx0 = (dx * 3) / 2;
                int sx1 = (sx0 + 1 < LOGICAL_W) ? (sx0 + 1) : sx0;
                uint32_t p00 = r0[sx0];
                uint32_t p01 = r0[sx1];
                uint32_t p10 = r1[sx0];
                uint32_t p11 = r1[sx1];

                uint32_t r = (((p00 >> 24) & 0xFFu) + ((p01 >> 24) & 0xFFu) +
                              ((p10 >> 24) & 0xFFu) + ((p11 >> 24) & 0xFFu)) >> 2;
                uint32_t g = (((p00 >> 16) & 0xFFu) + ((p01 >> 16) & 0xFFu) +
                              ((p10 >> 16) & 0xFFu) + ((p11 >> 16) & 0xFFu)) >> 2;
                uint32_t b = (((p00 >> 8) & 0xFFu) + ((p01 >> 8) & 0xFFu) +
                              ((p10 >> 8) & 0xFFu) + ((p11 >> 8) & 0xFFu)) >> 2;
                dst[dx] = (r << 24) | (g << 16) | (b << 8);
            }
        }

        DCFlushRange(tvWork, tvHalfBytes);
        DCFlushRange(drcWork, drcHalfBytes);
        OSScreenFlipBuffersEx(SCREEN_TV);
        OSScreenFlipBuffersEx(SCREEN_DRC);

        tvWorkIdx_ ^= 1;
        drcWorkIdx_ ^= 1;
    }

    void FillRect(int x, int y, int w, int h, uint32_t col) {
        if (!fb_ || w <= 0 || h <= 0) return;
        int x0 = std::max(0, x);
        int y0 = std::max(0, y);
        int x1 = std::min(LOGICAL_W, x + w);
        int y1 = std::min(LOGICAL_H, y + h);
        for (int yy = y0; yy < y1; ++yy) {
            uint32_t* row = fb_ + yy * LOGICAL_W;
            for (int xx = x0; xx < x1; ++xx) {
                row[xx] = col;
            }
        }
    }

    void FillRoundedRect(int x, int y, int w, int h, int rad, uint32_t col) {
        if (!fb_ || w <= 0 || h <= 0) return;
        float r = static_cast<float>(std::min({rad, w / 2, h / 2}));
        if (r <= 0.5f) {
            FillRect(x, y, w, h, col);
            return;
        }

        int ri = static_cast<int>(std::ceil(r));
        FillRect(x + ri, y, w - 2 * ri, h, col);
        FillRect(x, y + ri, ri, h - 2 * ri, col);
        FillRect(x + w - ri, y + ri, ri, h - 2 * ri, col);

        auto drawCorner = [&](int bx, int by, float cx, float cy) {
            for (int yy = by; yy < by + ri; ++yy) {
                if (yy < 0 || yy >= LOGICAL_H) continue;
                for (int xx = bx; xx < bx + ri; ++xx) {
                    if (xx < 0 || xx >= LOGICAL_W) continue;
                    float dx = (static_cast<float>(xx) + 0.5f) - cx;
                    float dy = (static_cast<float>(yy) + 0.5f) - cy;
                    float dist = std::sqrt(dx * dx + dy * dy) - r;
                    if (dist <= -0.5f) {
                        fb_[yy * LOGICAL_W + xx] = col;
                    } else if (dist < 0.5f) {
                        uint8_t a = static_cast<uint8_t>((0.5f - dist) * 255.0f);
                        BlendPixel(xx, yy, col, a);
                    }
                }
            }
        };

        float fx0 = static_cast<float>(x) + r;
        float fy0 = static_cast<float>(y) + r;
        float fx1 = static_cast<float>(x + w) - r;
        float fy1 = static_cast<float>(y + h) - r;

        drawCorner(x, y, fx0, fy0);
        drawCorner(x + w - ri, y, fx1, fy0);
        drawCorner(x, y + h - ri, fx0, fy1);
        drawCorner(x + w - ri, y + h - ri, fx1, fy1);
    }

    void DrawRoundedOutline(int x, int y, int w, int h, int rad, int thick, uint32_t col) {
        if (!fb_ || w <= 0 || h <= 0 || thick <= 0) return;
        float r = static_cast<float>(std::min({rad, w / 2, h / 2}));
        float t = static_cast<float>(thick);

        int ri = static_cast<int>(std::ceil(std::max(r, t)));
        FillRect(x + ri, y, w - 2 * ri, thick, col);
        FillRect(x + ri, y + h - thick, w - 2 * ri, thick, col);
        FillRect(x, y + ri, thick, h - 2 * ri, col);
        FillRect(x + w - thick, y + ri, thick, h - 2 * ri, col);

        float rIn = std::max(0.0f, r - t);
        auto drawCorner = [&](int bx, int by, float cx, float cy) {
            for (int yy = by; yy < by + ri; ++yy) {
                if (yy < 0 || yy >= LOGICAL_H) continue;
                for (int xx = bx; xx < bx + ri; ++xx) {
                    if (xx < 0 || xx >= LOGICAL_W) continue;
                    float dx = (static_cast<float>(xx) + 0.5f) - cx;
                    float dy = (static_cast<float>(yy) + 0.5f) - cy;
                    float d = std::sqrt(dx * dx + dy * dy);
                    float aOut = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
                    float aIn  = (rIn > 0.0f) ? std::clamp(d - (rIn - 0.5f), 0.0f, 1.0f) : 1.0f;
                    float cov = aOut * aIn;
                    if (cov > 0.01f) {
                        BlendPixel(xx, yy, col, static_cast<uint8_t>(cov * 255.0f));
                    }
                }
            }
        };

        float fx0 = static_cast<float>(x) + r;
        float fy0 = static_cast<float>(y) + r;
        float fx1 = static_cast<float>(x + w) - r;
        float fy1 = static_cast<float>(y + h) - r;

        drawCorner(x, y, fx0, fy0);
        drawCorner(x + w - ri, y, fx1, fy0);
        drawCorner(x, y + h - ri, fx0, fy1);
        drawCorner(x + w - ri, y + h - ri, fx1, fy1);
    }

    void FillTriangleUp(int cx, int topY, int halfW, int h, uint32_t col) {
        if (!fb_ || h <= 0 || halfW <= 0) return;
        for (int row = 0; row < h; ++row) {
            int curHalf = (h > 1) ? ((halfW * row) / (h - 1)) : halfW;
            FillRect(cx - curHalf, topY + row, curHalf * 2 + 1, 1, col);
        }
    }

    void FillTriangleDown(int cx, int topY, int halfW, int h, uint32_t col) {
        if (!fb_ || h <= 0 || halfW <= 0) return;
        for (int row = 0; row < h; ++row) {
            int curHalf = (h > 1) ? ((halfW * (h - 1 - row)) / (h - 1)) : halfW;
            FillRect(cx - curHalf, topY + row, curHalf * 2 + 1, 1, col);
        }
    }

    void DrawCheckmark(int x, int y, int size, uint32_t col) {
        if (!fb_ || size < 8) return;
        int x0 = x + size / 5;
        int y0 = y + (size * 52) / 100;
        int xm = x + (size * 42) / 100;
        int ym = y + (size * 74) / 100;
        int x1 = x + (size * 82) / 100;
        int y1 = y + (size * 26) / 100;
        auto drawThickSeg = [&](int ax, int ay, int bx, int by) {
            int steps = std::max(std::abs(bx - ax), std::abs(by - ay));
            if (steps <= 0) return;
            for (int i = 0; i <= steps; ++i) {
                int px = ax + ((bx - ax) * i) / steps;
                int py = ay + ((by - ay) * i) / steps;
                FillRect(px - 1, py - 1, 3, 3, col);
            }
        };
        drawThickSeg(x0, y0, xm, ym);
        drawThickSeg(xm, ym, x1, y1);
    }

    void DrawGamepadIcon(int x, int y) {
        if (!fb_) return;
        for (int iy = 0; iy < atlas::kGamepadIconH; ++iy) {
            int py = y + iy;
            if (py < 0 || py >= LOGICAL_H) continue;
            uint32_t* dstRow = fb_ + py * LOGICAL_W;
            const uint32_t* srcRow = atlas::kGamepadIconRgbx + iy * atlas::kGamepadIconW;
            for (int ix = 0; ix < atlas::kGamepadIconW; ++ix) {
                int px = x + ix;
                if (px < 0 || px >= LOGICAL_W) continue;
                dstRow[px] = srcRow[ix];
            }
        }
    }

    static std::string NormalizeAscii(const std::string& utf8) {
        std::string out;
        out.reserve(utf8.size());
        for (size_t i = 0; i < utf8.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(utf8[i]);
            if (c < 0x80) {
                out.push_back(static_cast<char>(c));
            } else if (c == 0xC3 && i + 1 < utf8.size()) {
                unsigned char n = static_cast<unsigned char>(utf8[++i]);
                switch (n) {
                    case 0xA1: out.push_back('a'); break;
                    case 0xA9: out.push_back('e'); break;
                    case 0xAD: out.push_back('i'); break;
                    case 0xB3: out.push_back('o'); break;
                    case 0xBA: out.push_back('u'); break;
                    case 0xB1: out.push_back('n'); break;
                    case 0x81: out.push_back('A'); break;
                    case 0x89: out.push_back('E'); break;
                    case 0x8D: out.push_back('I'); break;
                    case 0x93: out.push_back('O'); break;
                    case 0x9A: out.push_back('U'); break;
                    case 0x91: out.push_back('N'); break;
                    default:   out.push_back('?'); break;
                }
            } else if (c == 0xC2 && i + 1 < utf8.size()) {
                unsigned char n = static_cast<unsigned char>(utf8[++i]);
                if (n == 0xA1) out.push_back('!');
                else if (n == 0xBF) out.push_back('?');
            } else if (c == 0xE2 && i + 2 < utf8.size()) {
                i += 2;
                out.push_back('*');
            }
        }
        return out;
    }

    const atlas::BitmapFont& GetFont(FontId id) const {
        switch (id) {
            case FontId::Header: return atlas::kFontHeader;
            case FontId::Title:  return atlas::kFontTitle;
            case FontId::Body:   return atlas::kFontBody;
            case FontId::Small:  return atlas::kFontSmall;
        }
        return atlas::kFontBody;
    }

    int MeasureTextWidth(const std::string& text, FontId fontId = FontId::Body) const {
        const auto& font = GetFont(fontId);
        std::string s = NormalizeAscii(text);
        int w = 0;
        for (char rawCh : s) {
            unsigned char uc = static_cast<unsigned char>(rawCh);
            int idx = (uc >= 0x20 && uc <= 0x7E) ? (uc - 0x20) : 0;
            w += font.glyphs[idx].advance;
        }
        return w;
    }

    std::string Ellipsize(const std::string& text, FontId fontId, int maxWidthPx) const {
        std::string s = NormalizeAscii(text);
        if (maxWidthPx <= 0 || MeasureTextWidth(s, fontId) <= maxWidthPx) return s;
        while (s.size() > 3 && MeasureTextWidth(s + "...", fontId) > maxWidthPx) {
            s.pop_back();
        }
        return s + "...";
    }

    void DrawText(int x, int y, const std::string& text, uint32_t col,
                  FontId fontId = FontId::Body, int maxWidthPx = 0) {
        if (!fb_) return;
        const auto& font = GetFont(fontId);
        std::string s = (maxWidthPx > 0) ? Ellipsize(text, fontId, maxWidthPx) : NormalizeAscii(text);

        int penX = x;
        for (char rawCh : s) {
            unsigned char uc = static_cast<unsigned char>(rawCh);
            int idx = (uc >= 0x20 && uc <= 0x7E) ? (uc - 0x20) : 0;
            const auto& g = font.glyphs[idx];

            int gx = penX + g.xOff;
            int gy = y + g.yOff;
            for (int row = 0; row < g.height; ++row) {
                int py = gy + row;
                if (py < 0 || py >= LOGICAL_H) continue;
                for (int c = 0; c < g.width; ++c) {
                    int px = gx + c;
                    if (px < 0 || px >= LOGICAL_W) continue;
                    uint8_t a = atlas::kGlyphAlphaData[g.dataOffset + row * g.width + c];
                    if (a > 0) {
                        BlendPixel(px, py, col, a);
                    }
                }
            }
            penX += g.advance;
        }
    }

    void DrawTextRight(int rightX, int y, const std::string& text, uint32_t col,
                       FontId fontId = FontId::Body, int maxWidthPx = 0) {
        std::string s = (maxWidthPx > 0) ? Ellipsize(text, fontId, maxWidthPx) : NormalizeAscii(text);
        int tw = MeasureTextWidth(s, fontId);
        DrawText(rightX - tw, y, s, col, fontId, 0);
    }

    void DrawTextCentered(int x, int y, int w, int h, const std::string& text,
                          uint32_t col, FontId fontId = FontId::Body) {
        const auto& font = GetFont(fontId);
        std::string s = Ellipsize(text, fontId, w - 16);
        int tw = MeasureTextWidth(s, fontId);
        int th = font.lineHeight;
        DrawText(x + (w - tw) / 2, y + (h - th) / 2, s, col, fontId, 0);
    }

    static std::string FormatBytes(uint64_t bytes) {
        char buf[32];
        if (bytes < 1024ULL) {
            std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
        } else if (bytes < 1024ULL * 1024ULL) {
            std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
        } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
            std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        } else {
            std::snprintf(buf, sizeof(buf), "%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
        }
        return buf;
    }

private:
    inline void BlendPixel(int x, int y, uint32_t srcCol, uint8_t alpha) {
        if (alpha == 255) {
            fb_[y * LOGICAL_W + x] = srcCol;
            return;
        }
        uint32_t dst = fb_[y * LOGICAL_W + x];
        uint32_t sr = (srcCol >> 24) & 0xFFu;
        uint32_t sg = (srcCol >> 16) & 0xFFu;
        uint32_t sb = (srcCol >> 8) & 0xFFu;

        uint32_t dr = (dst >> 24) & 0xFFu;
        uint32_t dg = (dst >> 16) & 0xFFu;
        uint32_t db = (dst >> 8) & 0xFFu;

        uint32_t inv = 255u - alpha;
        uint32_t rr = (sr * alpha + dr * inv) / 255u;
        uint32_t rg = (sg * alpha + dg * inv) / 255u;
        uint32_t rb = (sb * alpha + db * inv) / 255u;

        fb_[y * LOGICAL_W + x] = (rr << 24) | (rg << 16) | (rb << 8);
    }

    static void ProbeScreenLayout(OSScreenID id, void* buf, uint32_t totalSize,
                                  uint32_t minW, uint32_t maxW,
                                  int& outWorkIdx, uint32_t& outStride) {
        uint32_t halfBytes = totalSize / 2;
        std::memset(buf, 0, totalSize);
        OSScreenPutPixelEx(id, 0, 0, 0x12345600u);
        OSScreenPutPixelEx(id, 0, 1, 0x65432100u);

        auto* p0 = static_cast<uint32_t*>(buf);
        auto* p1 = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(buf) + halfBytes);

        outWorkIdx = ((p1[0] & 0xFFFFFF00u) == 0x12345600u) ? 1 : 0;
        const uint32_t* active = (outWorkIdx == 0) ? p0 : p1;

        outStride = minW;
        for (uint32_t s = minW; s <= maxW; ++s) {
            if ((active[s] & 0xFFFFFF00u) == 0x65432100u) {
                outStride = s;
                break;
            }
        }
        std::memset(buf, 0, totalSize);
        DCFlushRange(buf, totalSize);
    }

    static uint32_t OnProcAcquire(void* ctx) {
        auto* self = static_cast<UiCanvas*>(ctx);
        MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);
        MEMRecordStateForFrmHeap(heap, kFrameHeapTag);

        self->tvBuf_  = self->tvSize_ ? MEMAllocFromFrmHeapEx(heap, self->tvSize_, 0x100) : nullptr;
        self->drcBuf_ = self->drcSize_ ? MEMAllocFromFrmHeapEx(heap, self->drcSize_, 0x100) : nullptr;

        OSScreenSetBufferEx(SCREEN_TV, self->tvBuf_);
        OSScreenSetBufferEx(SCREEN_DRC, self->drcBuf_);

        self->hasForeground_ = (self->tvBuf_ != nullptr && self->drcBuf_ != nullptr);
        if (self->hasForeground_) {
            ProbeScreenLayout(SCREEN_TV, self->tvBuf_, self->tvSize_, 1280, 2048,
                              self->tvWorkIdx_, self->tvStride_);
            ProbeScreenLayout(SCREEN_DRC, self->drcBuf_, self->drcSize_, 854, 1280,
                              self->drcWorkIdx_, self->drcStride_);
        }
        self->resumedFromBg_ = true;
        return 0;
    }

    static uint32_t OnProcRelease(void* ctx) {
        auto* self = static_cast<UiCanvas*>(ctx);
        MEMHeapHandle heap = MEMGetBaseHeapHandle(MEM_BASE_HEAP_MEM1);
        MEMFreeByStateToFrmHeap(heap, kFrameHeapTag);
        self->tvBuf_ = nullptr;
        self->drcBuf_ = nullptr;
        self->hasForeground_ = false;
        return 0;
    }

    uint32_t* fb_ = nullptr;
    void* tvBuf_ = nullptr;
    void* drcBuf_ = nullptr;
    uint32_t tvSize_ = 0;
    uint32_t drcSize_ = 0;
    uint32_t tvStride_ = 1280;
    uint32_t drcStride_ = 896;
    int tvWorkIdx_ = 0;
    int drcWorkIdx_ = 0;
    bool hasForeground_ = false;
    bool resumedFromBg_ = false;
};

} // namespace wutui
