/*
 * Wii U Transfer - Frontend Wii U (WUT + OSScreen Nativo 720p TV / GamePad)
 *
 * Modos de transferencia:
 *   1. NetFSA (Cero-SD / Mocha): Desbloquea /dev/fsa vía MochaPayload (Aroma),
 *      hace streaming TCP continuo directo a usr/tmp en USB/NAND usando doble
 *      buffer multi-core (1 MB en CPU2) e instala vía MCP limpiando al terminar.
 *   2. Direct-Sync (Cache SD): Usa el mismo streaming TCP multi-core hacia un
 *      temporal en la tarjeta SD, instala con MCP y limpia la SD automáticamente.
 */

#include <coreinit/cache.h>
#include <coreinit/filesystem_fsa.h>
#include <coreinit/ios.h>
#include <coreinit/mcp.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/messagequeue.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <nn/ac.h>
#include <nn/nets2/somemopt.h>
#include <padscore/kpad.h>
#include <sysapp/launch.h>
#include <vpad/input.h>
#include <whb/proc.h>
#include <whb/sdcard.h>

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "../../protocol/netfsa_protocol.h"
#include "audio_player.hpp"
#include "ui_canvas.hpp"
#include "usb_fat_reader.hpp"
#include "usb_mtp_reader.hpp"

namespace {

constexpr uint32_t STREAM_BUF_COUNT  = 4;               // Quad-buffer (4 x 2 MB = 8 MB de pipeline en MEM2)
constexpr uint32_t STREAM_BUF_SIZE   = 2 * 1024 * 1024; // 2 MB por buffer para máxima velocidad de escritura FSA/USB
constexpr uint32_t USB_SLICE_SIZE    = 1024 * 1024;     // 1 MB por lectura USB (evita fragmentar DMA de clusters contiguos)
constexpr uint32_t NET_SLICE_SIZE    = 512 * 1024;      // 512 KB por slice TCP con SO_RUSRBUF activo (Wi-Fi / LAN USB)
constexpr uint32_t FSA_WRITE_CHUNK   = 1024 * 1024;     // 1 MB por llamada FSAWriteFile
constexpr uint32_t STDIO_BUF_SIZE    = 512 * 1024;      // 512 KB de setvbuf para escritura rápida en SD
constexpr uint32_t SOMEMOPT_BUF_SIZE = 0x200000;        // 2 MB para pool TCP nsysnet (Wi-Fi y LAN USB a maxima velocidad)
constexpr uint32_t SOMEMOPT_STACK_SZ = 16 * 1024;
constexpr uint32_t WRITER_STACK_SIZE = 32 * 1024;
constexpr uint32_t UI_REFRESH_MS     = 450;
constexpr const char* APP_VERSION_STR = "v2.0.1";

wutaudio::AudioManager* gAudioMgr = nullptr;

class SocketMemoryBooster {
public:
    void Init() {
        if (initialized_) return;
        buf_ = static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(SOMEMOPT_BUF_SIZE, 0x40));
        stack_ = static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(SOMEMOPT_STACK_SZ, 0x20));
        thread_ = static_cast<OSThread*>(MEMAllocFromDefaultHeapEx(sizeof(OSThread), 0x20));
        if (!buf_ || !stack_ || !thread_) {
            CleanupAllocs();
            return;
        }
        std::memset(buf_, 0, SOMEMOPT_BUF_SIZE);
        DCFlushRange(buf_, SOMEMOPT_BUF_SIZE);

        if (!OSCreateThread(thread_, ThreadEntry, 0, this,
                            stack_ + SOMEMOPT_STACK_SZ, SOMEMOPT_STACK_SZ,
                            16, OS_THREAD_ATTRIB_AFFINITY_CPU0)) {
            CleanupAllocs();
            return;
        }
        OSThread* t = thread_;
        OSDetachThread(t);
        OSResumeThread(t);
        // Esperar confirmacion real de nsysnet antes de abrir sockets con SO_RUSRBUF
        int waitRes = somemopt(SOMEMOPT_REQUEST_WAIT_FOR_INIT, nullptr, 0, SOMEMOPT_FLAGS_NONE);
        initialized_ = true;
        ready_ = (waitRes >= 0);
    }

    static bool IsReady() { return ready_; }

private:
    static int ThreadEntry(int /*argc*/, void* argv) {
        auto* self = static_cast<SocketMemoryBooster*>(argv);
        somemopt(SOMEMOPT_REQUEST_INIT, self->buf_, SOMEMOPT_BUF_SIZE, SOMEMOPT_FLAGS_BIG_BUFFERS);
        return 0;
    }

    void CleanupAllocs() {
        if (thread_) { MEMFreeToDefaultHeap(thread_); thread_ = nullptr; }
        if (stack_)  { MEMFreeToDefaultHeap(stack_);  stack_  = nullptr; }
        if (buf_)    { MEMFreeToDefaultHeap(buf_);    buf_    = nullptr; }
    }

    uint8_t* buf_ = nullptr;
    uint8_t* stack_ = nullptr;
    OSThread* thread_ = nullptr;
    bool initialized_ = false;
    static bool ready_;
};

bool SocketMemoryBooster::ready_ = false;

constexpr uint32_t IPC_CUSTOM_GET_MOCHA_API_VERSION = 0xF8;
constexpr uint32_t MOCHA_IOCTL_UNLOCK_FSA           = 0x28;

constexpr const char* SD_TMP_BASE_FS  = "fs:/vol/external01/install/_wiiu_transfer_tmp";
constexpr const char* SD_TMP_BASE_MCP = "/vol/external01/install/_wiiu_transfer_tmp";
constexpr const char* SERVER_IP_CFG   = "fs:/vol/external01/wiiu/apps/WiiU_Transfer/server_ip.txt";

struct RemotePkg {
    std::string name;
    uint64_t totalBytes = 0;
    int fileCount = 0;
    bool marked = false;
};

struct RemoteFile {
    std::string relPath; // Relativo a la raíz del servidor (ej. "MiJuego/title.tmd")
    std::string subPath; // Relativo al paquete (ej. "title.tmd")
    uint64_t size = 0;
};

enum class TransferMode {
    NetFsaMocha  = 0,
    DirectSyncSd = 1
};

enum class PkgSource {
    NetworkPc  = 0,
    UsbFat     = 1,
    AndroidMtp = 2
};

struct AppState {
    PkgSource source = PkgSource::NetworkPc;

    bool connected = false;
    std::string serverIp;
    std::string serverHost;
    uint16_t serverPort = NETFSA_TCP_PORT;

    std::vector<RemotePkg> pkgs;
    std::vector<usbfat::UsbPkg> usbPkgs;
    bool usbFatConnected = false;
    std::string usbFatDevPath;
    usbfat::UsbFatVolume usbFat;

    std::vector<usbmtp::MtpPkg> mtpPkgs;
    bool mtpConnected = false;
    std::string mtpDeviceName;
    usbmtp::UsbMtpDevice mtpDev;

    int selectedIdx = 0;
    int scrollOffset = 0;

    bool targetUsb = true;
    TransferMode mode = TransferMode::DirectSyncSd;

    bool mochaUnlocked = false;
    FSAClientHandle fsaHandle = -1;
    FSAClientHandle fsaRawHandle = -1;
    bool usbAvailable = false;
    std::string usbMountRoot;
    uint64_t usbFreeBytes = 0;
    uint64_t nandFreeBytes = 0;

    uint64_t sdFreeBytes = 0;
    bool sdMounted = false;

    bool installing = false;
    bool cancelling = false;
    int queueCurrent = 0;
    int queueTotal = 0;
    float progress = 0.0f;
    double speedMBs = 0.0;
    std::string statusLine = "Listo para transferir.";
    std::string subStatusLine;
    bool statusIsError = false;
    bool statusIsSuccess = false;

    int ActivePkgCount() const {
        if (source == PkgSource::UsbFat) return static_cast<int>(usbPkgs.size());
        if (source == PkgSource::AndroidMtp) return static_cast<int>(mtpPkgs.size());
        return static_cast<int>(pkgs.size());
    }

    bool ActiveSourceConnected() const {
        if (source == PkgSource::UsbFat) return usbFatConnected;
        if (source == PkgSource::AndroidMtp) return mtpConnected;
        return connected;
    }

    const std::string& PkgName(int idx) const {
        if (source == PkgSource::UsbFat) return usbPkgs[idx].name;
        if (source == PkgSource::AndroidMtp) return mtpPkgs[idx].name;
        return pkgs[idx].name;
    }

    int PkgFileCount(int idx) const {
        if (source == PkgSource::UsbFat) return static_cast<int>(usbPkgs[idx].files.size());
        if (source == PkgSource::AndroidMtp) return static_cast<int>(mtpPkgs[idx].files.size());
        return pkgs[idx].fileCount;
    }

    uint64_t PkgTotalBytes(int idx) const {
        if (source == PkgSource::UsbFat) return usbPkgs[idx].totalBytes;
        if (source == PkgSource::AndroidMtp) return mtpPkgs[idx].totalBytes;
        return pkgs[idx].totalBytes;
    }

    bool IsMarked(int idx) const {
        if (idx < 0 || idx >= ActivePkgCount()) return false;
        if (source == PkgSource::UsbFat) return usbPkgs[idx].marked;
        if (source == PkgSource::AndroidMtp) return mtpPkgs[idx].marked;
        return pkgs[idx].marked;
    }

    void SetMarked(int idx, bool v) {
        if (idx < 0 || idx >= ActivePkgCount()) return;
        if (source == PkgSource::UsbFat) usbPkgs[idx].marked = v;
        else if (source == PkgSource::AndroidMtp) mtpPkgs[idx].marked = v;
        else pkgs[idx].marked = v;
    }

    void ToggleMarked(int idx) {
        SetMarked(idx, !IsMarked(idx));
    }

    int MarkedCount() const {
        int c = 0;
        int n = ActivePkgCount();
        for (int i = 0; i < n; ++i) {
            if (IsMarked(i)) ++c;
        }
        return c;
    }

    uint64_t MarkedTotalBytes() const {
        uint64_t total = 0;
        int n = ActivePkgCount();
        for (int i = 0; i < n; ++i) {
            if (IsMarked(i)) {
                total += PkgTotalBytes(i);
            }
        }
        return total;
    }

    void ToggleMarkAll() {
        int n = ActivePkgCount();
        if (n <= 0) return;
        bool target = (MarkedCount() < n);
        for (int i = 0; i < n; ++i) SetMarked(i, target);
    }

    void ClampSelection() {
        int cnt = ActivePkgCount();
        if (cnt <= 0) {
            selectedIdx = 0;
            scrollOffset = 0;
            return;
        }
        selectedIdx = std::clamp(selectedIdx, 0, cnt - 1);
        int maxScroll = std::max(0, cnt - 4);
        scrollOffset = std::clamp(scrollOffset, 0, maxScroll);
        if (selectedIdx < scrollOffset) scrollOffset = selectedIdx;
        if (selectedIdx >= scrollOffset + 4) scrollOffset = selectedIdx - 3;
    }
};

enum AppAction : uint32_t {
    ACT_NONE     = 0,
    ACT_UP       = 1u << 0,
    ACT_DOWN     = 1u << 1,
    ACT_LEFT     = 1u << 2,
    ACT_RIGHT    = 1u << 3,
    ACT_A        = 1u << 4,
    ACT_B        = 1u << 5,
    ACT_Y        = 1u << 6,
    ACT_MODE_L   = 1u << 7,
    ACT_MODE_R   = 1u << 8,
    ACT_EXIT     = 1u << 9,
    ACT_X        = 1u << 10,
    ACT_MARK     = 1u << 11,
    ACT_MARK_ALL = 1u << 12,
    ACT_MUTE     = 1u << 13
};

struct InputSnapshot {
    uint32_t triggered = 0;
    uint32_t held = 0;
    uint32_t anyHeld = 0;
    uint32_t repeated = 0;
    bool isRepeatPulse = false;
    bool touchTap = false;
    int tapX = 0;
    int tapY = 0;
    bool cancelTouchHit = false;
};

class InputManager {
public:
    static bool IsInsideCancelRect(int x, int y) {
        return x >= 660 && x <= 1234 && y >= 386 && y <= 460;
    }

    void Init() {
        VPADSetButtonProcMode(VPAD_CHAN_0, VPAD_BUTTON_PROC_MODE_TIGHT);
        KPADInit();
        WPADEnableURCC(TRUE);
        for (int c = 0; c < 4; ++c) {
            KPADSetButtonProcMode(static_cast<KPADChan>(c), KPAD_BUTTON_PROC_MODE_TIGHT);
        }
    }

    void Shutdown() {
        KPADShutdown();
    }

    void Flush() {
        (void)SampleAll(false);
        repeatAction_ = ACT_NONE;
    }

    void BeginInstallGuard() {
        Flush();
        waitTouchReleaseForCancel_ = (touchHeld_ || touchLatch_);
    }

    void EndInstallGuard() {
        Flush();
        waitReleaseForMenu_ = true;
    }

    InputSnapshot Poll(bool hasForeground) {
        if (!hasForeground) {
            Flush();
            return InputSnapshot{};
        }

        InputSnapshot snap = SampleAll(true);
        OSTime now = OSGetSystemTime();

        // Auto-repeticion para Navegacion Vertical (420 ms inicial, 110 ms pulso)
        snap.repeated = snap.triggered;
        snap.isRepeatPulse = false;
        uint32_t heldDir = ACT_NONE;
        if ((snap.held & ACT_UP) && !(snap.held & ACT_DOWN)) heldDir = ACT_UP;
        else if ((snap.held & ACT_DOWN) && !(snap.held & ACT_UP)) heldDir = ACT_DOWN;

        if (snap.triggered & (ACT_UP | ACT_DOWN)) {
            repeatAction_ = heldDir;
            repeatNextTicks_ = now + OSMillisecondsToTicks(420);
        } else if (heldDir != ACT_NONE && heldDir == repeatAction_) {
            if (now >= repeatNextTicks_) {
                snap.repeated |= heldDir;
                snap.isRepeatPulse = true;
                repeatNextTicks_ = now + OSMillisecondsToTicks(110);
            }
        } else if (heldDir != ACT_NONE) {
            repeatAction_ = heldDir;
            repeatNextTicks_ = now + OSMillisecondsToTicks(420);
        } else {
            repeatAction_ = ACT_NONE;
        }

        if (waitReleaseForMenu_) {
            snap.triggered &= ~(ACT_A | ACT_B);
            snap.repeated &= ~(ACT_A | ACT_B);
            snap.touchTap = false;
            if ((snap.held & (ACT_A | ACT_B)) == 0 && !touchHeld_ && !touchLatch_) {
                waitReleaseForMenu_ = false;
            }
        }
        return snap;
    }

private:
    static uint32_t MapVpad(uint32_t bits) {
        uint32_t act = ACT_NONE;
        if (bits & (VPAD_BUTTON_UP | VPAD_STICK_L_EMULATION_UP))       act |= ACT_UP;
        if (bits & (VPAD_BUTTON_DOWN | VPAD_STICK_L_EMULATION_DOWN))   act |= ACT_DOWN;
        if (bits & (VPAD_BUTTON_LEFT | VPAD_STICK_L_EMULATION_LEFT))   act |= ACT_LEFT;
        if (bits & (VPAD_BUTTON_RIGHT | VPAD_STICK_L_EMULATION_RIGHT)) act |= ACT_RIGHT;
        if (bits & VPAD_BUTTON_A)                                      act |= ACT_A;
        if (bits & VPAD_BUTTON_B)                                      act |= ACT_B;
        if (bits & VPAD_BUTTON_X)                                      act |= ACT_X;
        if (bits & VPAD_BUTTON_Y)                                      act |= ACT_Y;
        if (bits & VPAD_BUTTON_L)                                      act |= ACT_MODE_L;
        if (bits & VPAD_BUTTON_R)                                      act |= ACT_MODE_R;
        if (bits & (VPAD_BUTTON_MINUS | VPAD_BUTTON_ZL))               act |= ACT_MARK;
        if (bits & VPAD_BUTTON_ZR)                                     act |= ACT_MARK_ALL;
        if (bits & VPAD_BUTTON_STICK_R)                                act |= ACT_MUTE;
        if (bits & VPAD_BUTTON_PLUS)                                   act |= ACT_EXIT;
        return act;
    }

    static uint32_t MapKpadPro(uint32_t bits) {
        uint32_t act = ACT_NONE;
        if (bits & (WPAD_PRO_BUTTON_UP | WPAD_PRO_STICK_L_EMULATION_UP))       act |= ACT_UP;
        if (bits & (WPAD_PRO_BUTTON_DOWN | WPAD_PRO_STICK_L_EMULATION_DOWN))   act |= ACT_DOWN;
        if (bits & (WPAD_PRO_BUTTON_LEFT | WPAD_PRO_STICK_L_EMULATION_LEFT))   act |= ACT_LEFT;
        if (bits & (WPAD_PRO_BUTTON_RIGHT | WPAD_PRO_STICK_L_EMULATION_RIGHT)) act |= ACT_RIGHT;
        if (bits & WPAD_PRO_BUTTON_A)                                          act |= ACT_A;
        if (bits & WPAD_PRO_BUTTON_B)                                          act |= ACT_B;
        if (bits & WPAD_PRO_BUTTON_X)                                          act |= ACT_X;
        if (bits & WPAD_PRO_BUTTON_Y)                                          act |= ACT_Y;
        if (bits & WPAD_PRO_BUTTON_L)                                          act |= ACT_MODE_L;
        if (bits & WPAD_PRO_BUTTON_R)                                          act |= ACT_MODE_R;
        if (bits & (WPAD_PRO_BUTTON_MINUS | WPAD_PRO_BUTTON_ZL))               act |= ACT_MARK;
        if (bits & WPAD_PRO_BUTTON_ZR)                                         act |= ACT_MARK_ALL;
        if (bits & WPAD_PRO_BUTTON_STICK_R)                                    act |= ACT_MUTE;
        if (bits & WPAD_PRO_BUTTON_PLUS)                                       act |= ACT_EXIT;
        return act;
    }

    static uint32_t MapKpadClassic(uint32_t bits) {
        uint32_t act = ACT_NONE;
        if (bits & (WPAD_CLASSIC_BUTTON_UP | WPAD_CLASSIC_STICK_L_EMULATION_UP))       act |= ACT_UP;
        if (bits & (WPAD_CLASSIC_BUTTON_DOWN | WPAD_CLASSIC_STICK_L_EMULATION_DOWN))   act |= ACT_DOWN;
        if (bits & (WPAD_CLASSIC_BUTTON_LEFT | WPAD_CLASSIC_STICK_L_EMULATION_LEFT))   act |= ACT_LEFT;
        if (bits & (WPAD_CLASSIC_BUTTON_RIGHT | WPAD_CLASSIC_STICK_L_EMULATION_RIGHT)) act |= ACT_RIGHT;
        if (bits & WPAD_CLASSIC_BUTTON_A)                                              act |= ACT_A;
        if (bits & WPAD_CLASSIC_BUTTON_B)                                              act |= ACT_B;
        if (bits & WPAD_CLASSIC_BUTTON_X)                                              act |= ACT_X;
        if (bits & WPAD_CLASSIC_BUTTON_Y)                                              act |= ACT_Y;
        if (bits & WPAD_CLASSIC_BUTTON_L)                                              act |= ACT_MODE_L;
        if (bits & WPAD_CLASSIC_BUTTON_R)                                              act |= ACT_MODE_R;
        if (bits & (WPAD_CLASSIC_BUTTON_MINUS | WPAD_CLASSIC_BUTTON_ZL))               act |= ACT_MARK;
        if (bits & WPAD_CLASSIC_BUTTON_ZR)                                             act |= ACT_MARK_ALL;
        if (bits & WPAD_CLASSIC_BUTTON_PLUS)                                           act |= ACT_EXIT;
        return act;
    }

    static uint32_t MapKpadCore(uint32_t bits) {
        uint32_t act = ACT_NONE;
        if (bits & WPAD_BUTTON_UP)    act |= ACT_UP;
        if (bits & WPAD_BUTTON_DOWN)  act |= ACT_DOWN;
        if (bits & WPAD_BUTTON_LEFT)  act |= ACT_LEFT;
        if (bits & WPAD_BUTTON_RIGHT) act |= ACT_RIGHT;
        if (bits & WPAD_BUTTON_A)     act |= ACT_A;
        if (bits & WPAD_BUTTON_B)     act |= ACT_B;
        if (bits & WPAD_BUTTON_1)     act |= ACT_Y;
        if (bits & WPAD_BUTTON_MINUS) act |= ACT_MARK;
        if (bits & WPAD_BUTTON_2)     act |= ACT_MODE_R;
        if (bits & WPAD_BUTTON_PLUS)  act |= ACT_EXIT;
        return act;
    }

    static bool TryGetCalibratedTouch(const VPADStatus& s, int& outX, int& outY) {
        if (s.tpFiltered1.touched && s.tpFiltered1.validity == VPAD_VALID) {
            VPADTouchData tp{};
            VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_1280X720, &tp, &s.tpFiltered1);
            if (tp.touched && tp.validity == VPAD_VALID) {
                outX = static_cast<int>(tp.x);
                outY = static_cast<int>(tp.y);
                return true;
            }
        }
        if (s.tpNormal.touched && s.tpNormal.validity == VPAD_VALID) {
            VPADTouchData tp{};
            VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_1280X720, &tp, &s.tpNormal);
            if (tp.touched && tp.validity == VPAD_VALID) {
                outX = static_cast<int>(tp.x);
                outY = static_cast<int>(tp.y);
                return true;
            }
        }
        return false;
    }

    InputSnapshot SampleAll(bool emitEvents) {
        InputSnapshot snap{};
        OSTime now = OSGetSystemTime();

        // 1. GamePad (VPAD): vaciar hasta 16 muestras en orden cronologico
        VPADReadError vErr = VPAD_READ_SUCCESS;
        int32_t vn = VPADRead(VPAD_CHAN_0, vpadBuf_, 16, &vErr);
        if (vn > 0 && vErr == VPAD_READ_SUCCESS) {
            int32_t count = std::min<int32_t>(vn, 16);
            uint32_t vTrig = 0;
            uint32_t vAnyHeld = 0;
            for (int32_t i = count - 1; i >= 0; --i) {
                const VPADStatus& s = vpadBuf_[i];
                vTrig |= MapVpad(s.trigger);
                vAnyHeld |= MapVpad(s.hold);

                int tx = 0;
                int ty = 0;
                if (TryGetCalibratedTouch(s, tx, ty)) {
                    if (emitEvents && !waitTouchReleaseForCancel_ && IsInsideCancelRect(tx, ty)) {
                        snap.cancelTouchHit = true;
                    }
                    if (!touchLatch_) {
                        touchLatch_ = true;
                        if (emitEvents && !snap.touchTap) {
                            snap.touchTap = true;
                            snap.tapX = tx;
                            snap.tapY = ty;
                        }
                    }
                }
            }
            vpadHeld_ = MapVpad(vpadBuf_[0].hold);
            snap.triggered |= vTrig;
            snap.anyHeld |= vAnyHeld;

            bool newestRawTouched = (vpadBuf_[0].tpFiltered1.touched != 0) ||
                                    (vpadBuf_[0].tpNormal.touched != 0);
            touchHeld_ = newestRawTouched;
            if (newestRawTouched) {
                wasRawTouched_ = true;
            } else {
                if (wasRawTouched_) {
                    wasRawTouched_ = false;
                    untouchedSince_ = now;
                } else if (OSTicksToMilliseconds(now - untouchedSince_) >= 45) {
                    touchLatch_ = false;
                    waitTouchReleaseForCancel_ = false;
                }
            }
        } else if (vErr == VPAD_READ_NO_SAMPLES || vErr == VPAD_READ_BUSY) {
            if (!touchHeld_ && !wasRawTouched_ &&
                OSTicksToMilliseconds(now - untouchedSince_) >= 45) {
                touchLatch_ = false;
                waitTouchReleaseForCancel_ = false;
            }
        } else {
            vpadHeld_ = 0;
            touchHeld_ = false;
            wasRawTouched_ = false;
            touchLatch_ = false;
            waitTouchReleaseForCancel_ = false;
        }

        // 2. Controles adicionales (Pro Controller, Classic, Wii Remote) via KPAD
        for (int c = 0; c < 4; ++c) {
            KPADError kErr = KPAD_ERROR_OK;
            uint32_t kn = KPADReadEx(static_cast<KPADChan>(c), kpadBuf_, 16, &kErr);
            if (kn > 0 && kErr == KPAD_ERROR_OK) {
                int32_t count = static_cast<int32_t>(std::min<uint32_t>(kn, 16));
                uint32_t kTrig = 0;
                uint32_t kAnyHeld = 0;
                uint32_t latestHeld = 0;
                for (int32_t i = count - 1; i >= 0; --i) {
                    const KPADStatus& ks = kpadBuf_[i];
                    if (ks.extensionType == WPAD_EXT_DEV_NOT_FOUND) continue;

                    uint32_t stTrig = 0;
                    uint32_t stHeld = 0;
                    if (ks.extensionType == WPAD_EXT_PRO_CONTROLLER) {
                        stTrig = MapKpadPro(ks.pro.trigger);
                        stHeld = MapKpadPro(ks.pro.hold);
                    } else {
                        stTrig = MapKpadCore(ks.trigger);
                        stHeld = MapKpadCore(ks.hold);
                        if (ks.extensionType == WPAD_EXT_CLASSIC ||
                            ks.extensionType == WPAD_EXT_MPLUS_CLASSIC) {
                            stTrig |= MapKpadClassic(ks.classic.trigger);
                            stHeld |= MapKpadClassic(ks.classic.hold);
                        }
                    }
                    kTrig |= stTrig;
                    kAnyHeld |= stHeld;
                    if (i == 0) latestHeld = stHeld;
                }
                kpadHeld_[c] = latestHeld;
                snap.triggered |= kTrig;
                snap.anyHeld |= kAnyHeld;
            } else if (kErr != KPAD_ERROR_NO_SAMPLES && kErr != KPAD_ERROR_BUSY) {
                kpadHeld_[c] = 0;
            }
        }

        snap.held = vpadHeld_;
        for (int c = 0; c < 4; ++c) {
            snap.held |= kpadHeld_[c];
        }
        snap.anyHeld |= snap.held;
        if (!emitEvents) {
            snap.triggered = 0;
            snap.touchTap = false;
            snap.cancelTouchHit = false;
        }
        return snap;
    }

    VPADStatus vpadBuf_[16]{};
    KPADStatus kpadBuf_[16]{};

    uint32_t vpadHeld_ = 0;
    uint32_t kpadHeld_[4]{};

    bool touchHeld_ = false;
    bool wasRawTouched_ = false;
    bool touchLatch_ = false;
    OSTime untouchedSince_ = 0;

    bool waitTouchReleaseForCancel_ = false;
    bool waitReleaseForMenu_ = false;

    uint32_t repeatAction_ = ACT_NONE;
    OSTime repeatNextTicks_ = 0;
};

enum WriterMsgCmd : uint32_t {
    WCMD_WRITE = 0,
    WCMD_SYNC  = 1,
    WCMD_QUIT  = 2
};

struct WriterContext {
    bool useFsa = true;
    FSAClientHandle fsaHandle = -1;
    FSAFileHandle fsaFile = 0;
    FILE* stdioFile = nullptr;

    uint8_t* bufs[STREAM_BUF_COUNT]{nullptr, nullptr, nullptr, nullptr};
    OSMessageQueue freeQ{};
    OSMessageQueue writeQ{};
    OSMessageQueue ackQ{};
    OSMessage freeMsgs[STREAM_BUF_COUNT]{};
    OSMessage writeMsgs[16]{};
    OSMessage ackMsgs[4]{};

    volatile bool writeError = false;
    volatile bool cancelRequested = false;
    volatile FSError lastFsaErr = static_cast<FSError>(0);
};

int WriterThreadEntry(int /*argc*/, void* argv) {
    auto* ctx = static_cast<WriterContext*>(argv);
    OSMessage msg{};
    while (OSReceiveMessage(&ctx->writeQ, &msg, OS_MESSAGE_FLAGS_BLOCKING)) {
        uint32_t cmd = msg.args[1];
        if (cmd == WCMD_QUIT) {
            break;
        }
        if (cmd == WCMD_SYNC) {
            OSMessage ack{};
            OSSendMessage(&ctx->ackQ, &ack, OS_MESSAGE_FLAGS_BLOCKING);
            continue;
        }

        uintptr_t idx = reinterpret_cast<uintptr_t>(msg.message);
        uint32_t bytes = msg.args[0];

        if (!ctx->writeError && !ctx->cancelRequested && bytes > 0) {
            if (ctx->useFsa) {
                DCFlushRange(ctx->bufs[idx], bytes);
                uint32_t written = 0;
                while (written < bytes && !ctx->cancelRequested) {
                    uint32_t chunk = std::min<uint32_t>(FSA_WRITE_CHUNK, bytes - written);
                    FSError wRes = FSAWriteFile(ctx->fsaHandle,
                                                ctx->bufs[idx] + written,
                                                1, chunk,
                                                ctx->fsaFile, FSA_WRITE_FLAG_NONE);
                    if (wRes < 0 || static_cast<uint32_t>(wRes) != chunk) {
                        ctx->lastFsaErr = wRes;
                        ctx->writeError = true;
                        break;
                    }
                    written += chunk;
                }
            } else {
                if (std::fwrite(ctx->bufs[idx], 1, bytes, ctx->stdioFile) != bytes) {
                    ctx->writeError = true;
                }
            }
        }

        OSMessage retMsg{};
        retMsg.message = reinterpret_cast<void*>(idx);
        OSSendMessage(&ctx->freeQ, &retMsg, OS_MESSAGE_FLAGS_BLOCKING);
    }
    return 0;
}

struct NetCancelCtx {
    wutui::UiCanvas* ui = nullptr;
    AppState* st = nullptr;
    InputManager* input = nullptr;
    OSTime* lastDrawTicks = nullptr;
    bool cancelled = false;
};

bool PollCancelAndRender(wutui::UiCanvas& ui,
                         AppState& st,
                         InputManager& input,
                         OSTime& lastDrawTicks);

bool CheckNetAlive(NetCancelCtx* ctx) {
    if (!ctx) return WHBProcIsRunning() != FALSE;
    if (ctx->cancelled) return false;
    if (!PollCancelAndRender(*ctx->ui, *ctx->st, *ctx->input, *ctx->lastDrawTicks)) {
        ctx->cancelled = true;
        return false;
    }
    return true;
}

bool SendExact(int sock, const void* data, size_t len, int timeoutMs = 8000, NetCancelCtx* ctx = nullptr) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    OSTime lastActivity = OSGetSystemTime();
    OSTime lastCheck = lastActivity;

    while (sent < len) {
        OSTime now = OSGetSystemTime();
        if (OSTicksToMilliseconds(now - lastCheck) >= 35) {
            lastCheck = now;
            if (!CheckNetAlive(ctx)) return false;
        }

        ssize_t n = send(sock, ptr + sent, len - sent, MSG_DONTWAIT);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            lastActivity = OSGetSystemTime();
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == 6)) {
            if (!CheckNetAlive(ctx)) return false;
            lastCheck = OSGetSystemTime();
            if (OSTicksToMilliseconds(lastCheck - lastActivity) >= static_cast<uint64_t>(std::max(0, timeoutMs))) {
                return false;
            }

            struct pollfd pfd{sock, POLLOUT, 0};
            int pr = poll(&pfd, 1, 15);
            if (pr < 0 && errno != EINTR && errno != EAGAIN) return false;
            continue;
        }
        return false;
    }
    return true;
}

bool RecvExact(int sock, void* data, size_t len, int timeoutMs = 15000, NetCancelCtx* ctx = nullptr) {
    uint8_t* ptr = static_cast<uint8_t*>(data);
    size_t got = 0;
    OSTime lastActivity = OSGetSystemTime();
    OSTime lastCheck = lastActivity;

    while (got < len) {
        size_t want = std::min<size_t>(len - got, 128 * 1024);
        ssize_t n = recv(sock, ptr + got, want, MSG_DONTWAIT);
        if (n > 0) {
            got += static_cast<size_t>(n);
            lastActivity = OSGetSystemTime();
            if (OSTicksToMilliseconds(lastActivity - lastCheck) >= 180) {
                lastCheck = lastActivity;
                if (!CheckNetAlive(ctx)) return false;
            }
            continue;
        }
        if (n == 0) return false;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == 6) {
            OSTime now = OSGetSystemTime();
            if (OSTicksToMilliseconds(now - lastCheck) >= 180) {
                lastCheck = now;
                if (!CheckNetAlive(ctx)) return false;
            }
            if (OSTicksToMilliseconds(now - lastActivity) >= static_cast<uint64_t>(std::max(0, timeoutMs))) {
                return false;
            }
            struct pollfd pfd{sock, POLLIN, 0};
            int pr = poll(&pfd, 1, 100);
            if (pr < 0 && errno != EINTR && errno != EAGAIN) return false;
            continue;
        }
        return false;
    }
    return true;
}

int ConnectTcp(const std::string& ip, uint16_t port, int timeoutMs = 4000, NetCancelCtx* ctx = nullptr) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    setsockopt(sock, SOL_SOCKET, SO_WINSCALE, &yes, sizeof(yes));
    setsockopt(sock, SOL_SOCKET, SO_TCPSACK, &yes, sizeof(yes));
    setsockopt(sock, SOL_SOCKET, SO_NOSLOWSTART, &yes, sizeof(yes));
    if (SocketMemoryBooster::IsReady()) {
        setsockopt(sock, SOL_SOCKET, SO_RUSRBUF, &yes, sizeof(yes));
    }
    int rcvBuf = 256 * 1024;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf)) < 0) {
        rcvBuf = 128 * 1024;
        if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf)) < 0) {
            rcvBuf = 65536;
            setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf));
        }
    }
    int sndBuf = 65536;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndBuf, sizeof(sndBuf));
    setsockopt(sock, SOL_TCP, TCP_NODELAY, &yes, sizeof(yes));
    setsockopt(sock, SOL_TCP, TCP_NOACKDELAY, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        close(sock);
        return -1;
    }

    int nb = 1;
    setsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    int cr = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (cr < 0) {
        if (errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EAGAIN) {
            close(sock);
            return -1;
        }

        bool connected = false;
        OSTime start = OSGetSystemTime();
        uint64_t limitMs = static_cast<uint64_t>(std::max(0, timeoutMs));
        while (OSTicksToMilliseconds(OSGetSystemTime() - start) < limitMs) {
            if (!CheckNetAlive(ctx)) {
                close(sock);
                return -1;
            }
            struct pollfd pfd{sock, POLLOUT, 0};
            int pr = poll(&pfd, 1, 40);
            if (pr > 0) {
                int soErr = 0;
                socklen_t errLen = sizeof(soErr);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &soErr, &errLen) == 0 && soErr == 0 &&
                    (pfd.revents & POLLOUT) != 0 &&
                    (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0) {
                    connected = true;
                }
                break;
            }
            if (pr < 0 && errno != EINTR) {
                break;
            }
        }
        if (!connected) {
            close(sock);
            return -1;
        }
    }

    nb = 0;
    setsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));
    return sock;
}

bool SendCmdAndHeader(int sock,
                      uint8_t cmd,
                      const void* payload,
                      uint32_t len,
                      NetFsaHeader& outResp,
                      NetCancelCtx* ctx = nullptr) {
    NetFsaHeader hdr{};
    hdr.magic[0] = NETFSA_MAGIC_0;
    hdr.magic[1] = NETFSA_MAGIC_1;
    hdr.magic[2] = NETFSA_MAGIC_2;
    hdr.magic[3] = NETFSA_MAGIC_3;
    hdr.version  = NETFSA_VERSION;
    hdr.cmdOrStatus = cmd;
    hdr.flags    = 0;
    hdr.payloadLen = len;

    if (!SendExact(sock, &hdr, sizeof(hdr), 5000, ctx)) return false;
    if (len > 0 && !SendExact(sock, payload, len, 5000, ctx)) return false;
    if (!RecvExact(sock, &outResp, sizeof(outResp), 10000, ctx)) return false;

    return outResp.magic[0] == NETFSA_MAGIC_0 &&
           outResp.magic[1] == NETFSA_MAGIC_1 &&
           outResp.magic[2] == NETFSA_MAGIC_2 &&
           outResp.magic[3] == NETFSA_MAGIC_3;
}

void EnsureDirRecursive(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] == '/' || i + 1 == path.size()) {
            if (cur == "fs:" || cur == "fs:/" || cur == "fs:/vol" || cur == "fs:/vol/external01") {
                continue;
            }
            mkdir(cur.c_str(), 0777);
        }
    }
}

void RemoveDirRecursive(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return;
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        std::string child = path + "/" + ent->d_name;
        struct stat st{};
        if (stat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            RemoveDirRecursive(child);
        } else {
            remove(child.c_str());
        }
    }
    closedir(dir);
    rmdir(path.c_str());
}

void UpdateAllFreeSpace(AppState& st) {
    struct statvfs vfs{};
    if (statvfs("fs:/vol/external01/", &vfs) == 0) {
        st.sdMounted = true;
        st.sdFreeBytes = static_cast<uint64_t>(vfs.f_bavail) * static_cast<uint64_t>(vfs.f_frsize);
    }

    st.usbAvailable = false;
    st.usbMountRoot.clear();
    st.usbFreeBytes = 0;
    st.nandFreeBytes = 0;

    if (!st.mochaUnlocked || st.fsaHandle < 0) return;

    // En Aroma, FSAGetFreeSpaceSize sobre /vol/external01 evita falsos 0 B cuando statvfs falla
    uint64_t sdFree = 0;
    if (FSAGetFreeSpaceSize(st.fsaHandle, "/vol/external01", &sdFree) >= 0) {
        st.sdMounted = true;
        st.sdFreeBytes = sdFree;
    }

    const char* usbCandidates[] = {"/vol/storage_usb01", "/vol/storage_usb02"};
    for (const char* cand : usbCandidates) {
        uint64_t freeB = 0;
        if (FSAGetFreeSpaceSize(st.fsaHandle, cand, &freeB) >= 0) {
            st.usbAvailable = true;
            st.usbMountRoot = cand;
            st.usbFreeBytes = freeB;
            break;
        }
    }

    uint64_t nandFree = 0;
    if (FSAGetFreeSpaceSize(st.fsaHandle, "/vol/storage_mlc01", &nandFree) >= 0) {
        st.nandFreeBytes = nandFree;
    }
}

void InitMochaFsa(AppState& st) {
    st.mochaUnlocked = false;
    st.fsaHandle = -1;
    st.fsaRawHandle = -1;

    IOSHandle mcpFd = static_cast<IOSHandle>(IOS_Open("/dev/mcp", IOS_OPEN_READ));
    if (mcpFd < 0) return;

    alignas(0x40) uint32_t ioBuf[0x40 / 4]{};
    ioBuf[0] = IPC_CUSTOM_GET_MOCHA_API_VERSION;
    IOSError iErr = IOS_Ioctl(mcpFd, 100, ioBuf, 4, ioBuf, 8);
    IOS_Close(mcpFd);

    if (iErr != IOS_ERROR_OK || ioBuf[0] != 0xCAFEBABE || ioBuf[1] < 1) {
        return;
    }

    FSAInit();
    FSAClientHandle h = FSAAddClient(nullptr);
    if (h < 0) return;

    alignas(0x40) int dummy[0x40 / 4]{};
    if (IOS_Ioctl(static_cast<IOSHandle>(h), MOCHA_IOCTL_UNLOCK_FSA, dummy, sizeof(dummy), dummy, sizeof(dummy)) != IOS_ERROR_OK) {
        FSADelClient(h);
        return;
    }

    st.fsaHandle = h;
    st.mochaUnlocked = true;
    st.mode = TransferMode::NetFsaMocha;

    // Segundo cliente FSA independiente para lecturas crudas del pendrive FAT32 en paralelo con CPU2
    FSAClientHandle rh = FSAAddClient(nullptr);
    if (rh >= 0) {
        alignas(0x40) int dummyRaw[0x40 / 4]{};
        if (IOS_Ioctl(static_cast<IOSHandle>(rh), MOCHA_IOCTL_UNLOCK_FSA,
                      dummyRaw, sizeof(dummyRaw), dummyRaw, sizeof(dummyRaw)) == IOS_ERROR_OK) {
            st.fsaRawHandle = rh;
        } else {
            FSADelClient(rh);
        }
    }
}

void ShutdownMochaFsa(AppState& st) {
    st.mtpDev.Shutdown();
    st.mtpConnected = false;
    st.usbFat.Shutdown();
    st.usbFatConnected = false;
    if (st.fsaRawHandle >= 0) {
        FSADelClient(st.fsaRawHandle);
        st.fsaRawHandle = -1;
    }
    if (st.fsaHandle >= 0) {
        FSADelClient(st.fsaHandle);
        st.fsaHandle = -1;
    }
    st.mochaUnlocked = false;
}

bool ScanAndroidMtp(AppState& st) {
    st.mtpDev.Disconnect();
    st.mtpPkgs.clear();
    st.mtpConnected = false;
    st.mtpDeviceName.clear();

    std::string devName;
    std::string diag;
    if (!st.mtpDev.ProbeAndConnect(devName, diag)) {
        st.statusIsError = true;
        st.statusIsSuccess = false;
        st.statusLine = diag;
        st.subStatusLine = "Conecta tu Memoria USB o celular al puerto USB frontal.";
        return false;
    }

    st.mtpConnected = true;
    st.mtpDeviceName = devName;
    std::string scanDiag;
    st.mtpDev.ScanWupPackages(st.mtpPkgs, scanDiag);
    st.selectedIdx = 0;
    st.scrollOffset = 0;

    st.statusIsError = false;
    st.statusIsSuccess = true;
    st.statusLine = "Celular conectado (" + devName + ")";
    st.subStatusLine = st.mtpPkgs.empty()
        ? "No se encontraron juegos en /WiiU_Transfer o /Download."
        : (std::to_string(st.mtpPkgs.size()) + " juego(s) listo(s) para instalar.");
    return true;
}

bool ScanUsbPendrive(AppState& st) {
    st.usbFat.Unmount();
    st.usbPkgs.clear();
    st.usbFatConnected = false;
    st.usbFatDevPath.clear();

    if (!st.mochaUnlocked || st.fsaRawHandle < 0) {
        st.statusIsError = true;
        st.statusIsSuccess = false;
        st.statusLine = "No se pudo acceder al puerto USB.";
        st.subStatusLine.clear();
        return false;
    }

    std::string devPath;
    std::string diag;
    if (!st.usbFat.ProbeAndMount(st.fsaRawHandle, devPath, diag)) {
        st.statusIsError = true;
        st.statusIsSuccess = false;
        st.statusLine = diag;
        st.subStatusLine = "Conecta tu Memoria USB o celular al puerto USB frontal.";
        return false;
    }

    st.usbFatConnected = true;
    st.usbFatDevPath = devPath;
    st.usbFat.ScanWupPackages(st.usbPkgs);
    st.selectedIdx = 0;
    st.scrollOffset = 0;

    st.statusIsError = false;
    st.statusIsSuccess = true;
    st.statusLine = std::string("Memoria USB detectada (") + st.usbFat.FsName() + ")";
    st.subStatusLine = st.usbPkgs.empty()
        ? "No se encontraron carpetas de juegos en la Memoria USB."
        : (std::to_string(st.usbPkgs.size()) + " juego(s) listo(s) para instalar.");
    return true;
}

// Autodetección unificada para "Modo USB" (Memoria USB exFAT/FAT32/NTFS/FAT o Celular Android MTP)
bool ScanUnifiedUsb(AppState& st) {
    st.usbFat.Unmount();
    st.usbPkgs.clear();
    st.usbFatConnected = false;
    st.usbFatDevPath.clear();

    st.mtpDev.Disconnect();
    st.mtpPkgs.clear();
    st.mtpConnected = false;
    st.mtpDeviceName.clear();

    // 1. Intentar primero Memoria USB (exFAT, FAT32, NTFS o FAT)
    if (st.mochaUnlocked && st.fsaRawHandle >= 0) {
        std::string devPath;
        std::string diag;
        if (st.usbFat.ProbeAndMount(st.fsaRawHandle, devPath, diag)) {
            st.source = PkgSource::UsbFat;
            st.usbFatConnected = true;
            st.usbFatDevPath = devPath;
            st.usbFat.ScanWupPackages(st.usbPkgs);
            st.selectedIdx = 0;
            st.scrollOffset = 0;
            st.statusIsError = false;
            st.statusIsSuccess = true;
            st.statusLine = std::string("Memoria USB detectada (") + st.usbFat.FsName() + ")";
            st.subStatusLine = st.usbPkgs.empty()
                ? "Copia tus carpetas de juegos en la Memoria USB."
                : (std::to_string(st.usbPkgs.size()) + " juego(s) listo(s) para instalar.");
            return true;
        }
    }

    // 2. Si no hay Memoria USB, buscar Celular Android por USB MTP
    std::string mtpName;
    std::string mtpDiag;
    if (st.mtpDev.ProbeAndConnect(mtpName, mtpDiag)) {
        st.source = PkgSource::AndroidMtp;
        st.mtpConnected = true;
        st.mtpDeviceName = mtpName;
        std::string scanDiag;
        st.mtpDev.ScanWupPackages(st.mtpPkgs, scanDiag);
        st.selectedIdx = 0;
        st.scrollOffset = 0;
        st.statusIsError = false;
        st.statusIsSuccess = true;
        st.statusLine = "Celular conectado (" + mtpName + ")";
        st.subStatusLine = st.mtpPkgs.empty()
            ? "Pon tus juegos en /WiiU_Transfer o /Download del celular."
            : (std::to_string(st.mtpPkgs.size()) + " juego(s) listo(s) en el celular.");
        return true;
    }

    st.source = PkgSource::UsbFat;
    st.statusIsError = true;
    st.statusIsSuccess = false;
    st.statusLine = "No se detecto Memoria USB ni Celular por USB.";
    st.subStatusLine = !mtpDiag.empty()
        ? mtpDiag
        : "Soporta exFAT, FAT32, NTFS, FAT y Android. Pulsa [Y].";
    return false;
}

void FsaEnsureDirRecursive(FSAClientHandle fsa, const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur.push_back(path[i]);
        if (path[i] == '/' || i + 1 == path.size()) {
            std::string trimmed = cur;
            while (trimmed.size() > 1 && trimmed.back() == '/') trimmed.pop_back();
            if (trimmed.empty() || trimmed == "/" || trimmed == "/vol" ||
                trimmed == "/vol/storage_usb01" || trimmed == "/vol/storage_usb02" ||
                trimmed == "/vol/storage_mlc01") {
                continue;
            }
            FSAMakeDir(fsa, trimmed.c_str(), static_cast<FSMode>(0x666));
        }
    }
}

void FsaRemoveDirRecursive(FSAClientHandle fsa, const std::string& path) {
    FSADirectoryHandle dh = 0;
    if (FSAOpenDir(fsa, path.c_str(), &dh) < 0) return;

    FSDirectoryEntry entry{};
    while (FSAReadDir(fsa, dh, &entry) == 0) {
        if (std::strcmp(entry.name, ".") == 0 || std::strcmp(entry.name, "..") == 0) {
            continue;
        }
        std::string child = path + "/" + entry.name;
        if ((entry.info.flags & FS_STAT_DIRECTORY) != 0) {
            FsaRemoveDirRecursive(fsa, child);
        } else {
            FSARemove(fsa, child.c_str());
        }
    }
    FSACloseDir(fsa, dh);
    FSARemove(fsa, path.c_str());
}

bool TryReadFixedIp(std::string& outIp) {
    FILE* fp = std::fopen(SERVER_IP_CFG, "rb");
    if (!fp) return false;
    char buf[64]{};
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, fp);
    std::fclose(fp);
    if (n == 0) return false;
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) {
        s.pop_back();
    }
    if (s.empty()) return false;
    outIp = s;
    return true;
}

bool DiscoverServer(AppState& st) {
    st.connected = false;
    st.serverIp.clear();
    st.serverHost.clear();

    std::string fixedIp;
    if (TryReadFixedIp(fixedIp)) {
        int ts = ConnectTcp(fixedIp, NETFSA_TCP_PORT);
        if (ts >= 0) {
            NetFsaHeader resp{};
            bool ok = SendCmdAndHeader(ts, NETFSA_CMD_PING, nullptr, 0, resp);
            if (ok && resp.payloadLen > 0 && resp.payloadLen < 64) {
                char dummy[64];
                RecvExact(ts, dummy, resp.payloadLen);
            }
            close(ts);
            if (ok && resp.cmdOrStatus == NETFSA_OK) {
                st.connected = true;
                st.serverIp = fixedIp;
                st.serverHost = "IP Manual";
                st.serverPort = NETFSA_TCP_PORT;
                return true;
            }
        }
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

    const char* msg = "NETFSA_DISCOVER";
    uint32_t myIp = 0;
    bool hasMyIp = NNResult_IsSuccess(ACGetAssignedAddress(&myIp)) && myIp != 0;

    auto sendDiscoveryBurst = [&]() {
        if (hasMyIp) {
            sockaddr_in subBcast{};
            subBcast.sin_family = AF_INET;
            subBcast.sin_port = htons(NETFSA_UDP_PORT);
            subBcast.sin_addr.s_addr = (myIp & 0xFFFFFF00U) | 0x000000FFU;
            sendto(sock, msg, std::strlen(msg), 0, reinterpret_cast<sockaddr*>(&subBcast), sizeof(subBcast));
        }
        sockaddr_in globBcast{};
        globBcast.sin_family = AF_INET;
        globBcast.sin_port = htons(NETFSA_UDP_PORT);
        globBcast.sin_addr.s_addr = INADDR_BROADCAST;
        sendto(sock, msg, std::strlen(msg), 0, reinterpret_cast<sockaddr*>(&globBcast), sizeof(globBcast));
    };

    sendDiscoveryBurst();
    OSTime waitStart = OSGetSystemTime();
    OSTime lastBurst = waitStart;
    while (OSTicksToMilliseconds(OSGetSystemTime() - waitStart) < 2200) {
        if (!WHBProcIsRunning()) {
            close(sock);
            return false;
        }
        if (gAudioMgr) gAudioMgr->Update(true);
        OSTime now = OSGetSystemTime();
        if (OSTicksToMilliseconds(now - lastBurst) >= 450) {
            lastBurst = now;
            sendDiscoveryBurst();
        }
        struct pollfd pfd{sock, POLLIN, 0};
        int pr = poll(&pfd, 1, 40);
        if (pr > 0) {
            char buf[256]{};
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n > 0 && std::strncmp(buf, "NETFSA_HERE|", 12) == 0) {
                st.serverIp = inet_ntoa(from.sin_addr);
                std::string rest(buf + 12);
                size_t bar = rest.find('|');
                if (bar != std::string::npos) {
                    st.serverHost = rest.substr(0, bar);
                    int p = std::atoi(rest.substr(bar + 1).c_str());
                    st.serverPort = (p > 0 && p < 65536) ? static_cast<uint16_t>(p) : NETFSA_TCP_PORT;
                } else {
                    st.serverHost = rest;
                    st.serverPort = NETFSA_TCP_PORT;
                }
                st.connected = true;
                break;
            }
        } else if (pr < 0 && errno != EINTR && errno != EAGAIN) {
            break;
        }
    }

    close(sock);
    return st.connected;
}

bool FetchDirectoryEntries(int sock,
                           const std::string& subDir,
                           std::string& outBody,
                           NetCancelCtx* ctx = nullptr) {
    NetFsaHeader resp{};
    if (!SendCmdAndHeader(sock, NETFSA_CMD_LIST, subDir.data(),
                          static_cast<uint32_t>(subDir.size()), resp, ctx)) {
        return false;
    }
    if (resp.cmdOrStatus != NETFSA_OK) return false;
    outBody.assign(resp.payloadLen, '\0');
    if (resp.payloadLen > 0) {
        if (!RecvExact(sock, outBody.data(), resp.payloadLen, 10000, ctx)) return false;
    }
    return true;
}

bool FetchPackageList(AppState& st) {
    st.pkgs.clear();
    if (!st.connected) return false;

    int sock = ConnectTcp(st.serverIp, st.serverPort);
    if (sock < 0) {
        st.connected = false;
        return false;
    }

    std::string body;
    bool ok = FetchDirectoryEntries(sock, "", body);
    close(sock);
    if (!ok) return false;

    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.size() < 3 || line[0] != 'D') continue;
        std::istringstream ls(line);
        std::string kind, name, sz, cnt;
        std::getline(ls, kind, '|');
        std::getline(ls, name, '|');
        std::getline(ls, sz, '|');
        std::getline(ls, cnt, '|');

        RemotePkg p;
        p.name = name;
        p.totalBytes = std::strtoull(sz.c_str(), nullptr, 10);
        p.fileCount = std::atoi(cnt.c_str());
        st.pkgs.push_back(std::move(p));
    }
    st.selectedIdx = 0;
    st.scrollOffset = 0;
    return true;
}

bool CollectRemoteFilesRecursive(int sock,
                                 const std::string& pkgRoot,
                                 const std::string& subRel,
                                 std::vector<RemoteFile>& outFiles,
                                 NetCancelCtx* ctx = nullptr) {
    if (!CheckNetAlive(ctx)) return false;

    std::string query = subRel.empty() ? pkgRoot : (pkgRoot + "/" + subRel);
    std::string body;
    if (!FetchDirectoryEntries(sock, query, body, ctx)) return false;

    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.size() < 3) continue;
        std::istringstream ls(line);
        std::string kind, name, sz, cnt;
        std::getline(ls, kind, '|');
        std::getline(ls, name, '|');
        std::getline(ls, sz, '|');
        std::getline(ls, cnt, '|');

        std::string nextSub = subRel.empty() ? name : (subRel + "/" + name);
        if (kind == "D") {
            if (!CollectRemoteFilesRecursive(sock, pkgRoot, nextSub, outFiles, ctx)) return false;
        } else if (kind == "F") {
            RemoteFile rf;
            rf.relPath = pkgRoot + "/" + nextSub;
            rf.subPath = nextSub;
            rf.size = std::strtoull(sz.c_str(), nullptr, 10);
            outFiles.push_back(std::move(rf));
        }
    }
    return true;
}

void DrawUI(wutui::UiCanvas& ui, const AppState& st) {
    ui.Clear(wutui::COL_BG);
    bool fromUsb = (st.source == PkgSource::UsbFat);
    bool fromMtp = (st.source == PkgSource::AndroidMtp);
    bool isUsbMode = (fromUsb || fromMtp);
    int pkgCount = st.ActivePkgCount();
    int markedCount = st.MarkedCount();

    // 1. Header Oscuro con acento Cyan (0..96)
    ui.FillRect(0, 0, wutui::LOGICAL_W, 96, wutui::COL_HEADER);
    ui.FillRect(0, 94, wutui::LOGICAL_W, 2, wutui::COL_CYAN);
    ui.DrawGamepadIcon(28, 10);
    ui.DrawText(146, 16, "Wii U Transfer Tools", wutui::COL_WHITE, wutui::FontId::Header);
    ui.DrawText(146, 58, "por El Diuca  *  Ver. 2.0.1  *  Instalador de Juegos", wutui::COL_CYAN_SUB, wutui::FontId::Body);

    // Selector simplificado de 2 pestañas [X]: Modo USB | Modo Red (x=536..872)
    ui.FillRoundedRect(536, 24, 336, 48, 24, wutui::COL_PILL_OFF);
    int segX = isUsbMode ? 540 : 706;
    ui.FillRoundedRect(segX, 28, 162, 40, 20, wutui::COL_CYAN);
    ui.DrawTextCentered(540, 28, 162, 40, "Modo USB",
                        isUsbMode ? wutui::COL_WHITE : wutui::COL_CYAN_SUB, wutui::FontId::Body);
    ui.DrawTextCentered(706, 28, 162, 40, "Modo Red",
                        !isUsbMode ? wutui::COL_WHITE : wutui::COL_CYAN_SUB, wutui::FontId::Body);

    // Botón de Música [R-Stick] o táctil (x=888..1020)
    bool musicOn = (gAudioMgr != nullptr) && !gAudioMgr->IsMuted();
    ui.FillRoundedRect(888, 24, 132, 48, 24, musicOn ? wutui::COL_PILL_OFF : wutui::COL_RED_DARK);
    ui.DrawRoundedOutline(888, 24, 132, 48, 24, 2, musicOn ? wutui::COL_CYAN : wutui::COL_RED);
    ui.DrawTextCentered(888, 24, 132, 48, musicOn ? "Musica: SI" : "Musica: NO",
                        wutui::COL_WHITE, wutui::FontId::Small);

    // Píldora de estado derecha (x=1034..1256)
    const int px = 1034;
    const int pw = 222;
    if (isUsbMode) {
        if (st.usbFatConnected) {
            ui.FillRoundedRect(px, 24, pw, 48, 24, wutui::COL_GREEN);
            ui.DrawTextCentered(px, 24, pw, 48, "Memoria USB Lista", wutui::COL_WHITE, wutui::FontId::Body);
        } else if (st.mtpConnected) {
            ui.FillRoundedRect(px, 24, pw, 48, 24, wutui::COL_GREEN);
            ui.DrawTextCentered(px, 24, pw, 48, "Celular Conectado", wutui::COL_WHITE, wutui::FontId::Body);
        } else {
            ui.FillRoundedRect(px, 24, pw, 48, 24, wutui::COL_PILL_OFF);
            ui.DrawTextCentered(px, 24, pw, 48, "Sin USB [Y] Buscar", wutui::COL_WHITE, wutui::FontId::Small);
        }
    } else {
        if (st.connected) {
            std::string pill = "Red: " + st.serverIp;
            ui.FillRoundedRect(px, 24, pw, 48, 24, wutui::COL_GREEN);
            ui.DrawTextCentered(px, 24, pw, 48, pill, wutui::COL_WHITE, wutui::FontId::Small);
        } else {
            ui.FillRoundedRect(px, 24, pw, 48, 24, wutui::COL_PILL_OFF);
            ui.DrawTextCentered(px, 24, pw, 48, "Sin Red [Y] Buscar", wutui::COL_WHITE, wutui::FontId::Small);
        }
    }

    // 2. Panel Izquierdo: Lista de Juegos (x=36, y=120, w=594, h=530)
    ui.FillRoundedRect(36, 120, 594, 530, 20, wutui::COL_CARD);
    ui.DrawRoundedOutline(36, 120, 594, 530, 20, 2, wutui::COL_BORDER);

    char hdrBuf[64];
    std::snprintf(hdrBuf, sizeof(hdrBuf), "Juegos encontrados (%d)", pkgCount);
    ui.DrawText(60, 140, hdrBuf, wutui::COL_TEXT, wutui::FontId::Title, 345);

    if (pkgCount > 0) {
        bool allMarked = (markedCount == pkgCount);
        ui.FillRoundedRect(414, 134, 96, 34, 10, allMarked ? wutui::COL_CYAN_LT : wutui::COL_BTN_OFF);
        ui.DrawRoundedOutline(414, 134, 96, 34, 10, allMarked ? 2 : 1,
                              allMarked ? wutui::COL_CYAN : wutui::COL_BORDER);
        ui.DrawTextCentered(414, 134, 96, 34, allMarked ? "Limpiar" : "Todos",
                            allMarked ? wutui::COL_CYAN_DK : wutui::COL_TEXT, wutui::FontId::Small);
    }

    if (pkgCount > 4) {
        bool canUp = (st.scrollOffset > 0);
        bool canDown = (st.scrollOffset + 4 < pkgCount);

        ui.FillRoundedRect(520, 134, 38, 34, 10, canUp ? wutui::COL_CYAN_LT : wutui::COL_BTN_OFF);
        ui.DrawRoundedOutline(520, 134, 38, 34, 10, canUp ? 2 : 1,
                              canUp ? wutui::COL_CYAN : wutui::COL_BORDER);
        ui.FillTriangleUp(539, 146, 7, 10, canUp ? wutui::COL_CYAN_DK : wutui::COL_MUTED);

        ui.FillRoundedRect(566, 134, 38, 34, 10, canDown ? wutui::COL_CYAN_LT : wutui::COL_BTN_OFF);
        ui.DrawRoundedOutline(566, 134, 38, 34, 10, canDown ? 2 : 1,
                              canDown ? wutui::COL_CYAN : wutui::COL_BORDER);
        ui.FillTriangleDown(585, 146, 7, 10, canDown ? wutui::COL_CYAN_DK : wutui::COL_MUTED);
    }

    constexpr int kMaxVisible = 4;
    if (pkgCount == 0) {
        if (!isUsbMode) {
            ui.DrawText(60, 210, st.connected
                ? "No hay juegos en la carpeta compartida."
                : "Abre el programa en tu PC o App en tu Celular", wutui::COL_MUTED, wutui::FontId::Body);
            ui.DrawText(60, 244, st.connected
                ? "Agrega tus juegos y pulsa [Y] para actualizar."
                : "y pulsa [Y] para buscar juegos por Red.", wutui::COL_MUTED, wutui::FontId::Body);
        } else {
            bool anyUsbConn = (st.usbFatConnected || st.mtpConnected);
            ui.DrawText(60, 210, anyUsbConn
                ? "No se encontraron juegos en el dispositivo USB."
                : "Conecta tu Memoria USB o Celular Android al", wutui::COL_MUTED, wutui::FontId::Body);
            ui.DrawText(60, 244, anyUsbConn
                ? "Copialos en la carpeta /install o /WiiU_Transfer."
                : "puerto USB frontal y pulsa [Y] para buscar.", wutui::COL_MUTED, wutui::FontId::Body);
            if (!anyUsbConn) {
                ui.DrawText(60, 282, "Formatos USB: exFAT, FAT32, NTFS, FAT y Android.", wutui::COL_CYAN_SUB, wutui::FontId::Small);
            }
        }
    } else {
        for (int i = 0; i < kMaxVisible; ++i) {
            int idx = st.scrollOffset + i;
            if (idx >= pkgCount) break;

            const std::string& pName = st.PkgName(idx);
            uint64_t pBytes = st.PkgTotalBytes(idx);
            bool marked = st.IsMarked(idx);

            int cy = 186 + i * 112;
            bool sel = (idx == st.selectedIdx);

            uint32_t cardFill = sel ? wutui::COL_CYAN_LT : wutui::COL_CARD_ALT;
            uint32_t cardOut  = sel ? wutui::COL_CYAN : (marked ? wutui::COL_CYAN_DK : wutui::COL_BORDER);
            ui.FillRoundedRect(60, cy, 546, 98, 14, cardFill);
            ui.DrawRoundedOutline(60, cy, 546, 98, 14, sel ? 3 : 2, cardOut);

            // Casilla de seleccion multiple (x=74..102, y=cy+35..cy+63)
            if (marked) {
                ui.FillRoundedRect(74, cy + 35, 28, 28, 7, wutui::COL_CYAN);
                ui.DrawCheckmark(74, cy + 35, 28, wutui::COL_WHITE);
            } else {
                ui.FillRoundedRect(74, cy + 35, 28, 28, 7, wutui::COL_BOX_SOFT);
                ui.DrawRoundedOutline(74, cy + 35, 28, 28, 7, 2,
                                      sel ? wutui::COL_CYAN : wutui::COL_BORDER);
            }

            ui.DrawText(114, cy + 16, pName, wutui::COL_TEXT, wutui::FontId::Title, 474);
            char sub[96];
            std::snprintf(sub, sizeof(sub), "%s  *  Listo para instalar",
                          wutui::UiCanvas::FormatBytes(pBytes).c_str());
            ui.DrawText(114, cy + 54, sub, sel ? wutui::COL_CYAN_DK : wutui::COL_MUTED, wutui::FontId::Body, 474);
        }
    }

    // 3. Panel Derecho: Opciones de instalacion (x=650, y=120, w=594, h=530)
    ui.FillRoundedRect(650, 120, 594, 530, 20, wutui::COL_CARD);
    ui.DrawRoundedOutline(650, 120, 594, 530, 20, 2, wutui::COL_BORDER);

    ui.DrawTextCentered(650, 130, 594, 32, "Opciones de instalacion", wutui::COL_TEXT, wutui::FontId::Title);

    // Caja resumen centrada (y=168..268)
    ui.FillRoundedRect(678, 168, 538, 100, 14, wutui::COL_BOX_SOFT);
    ui.DrawRoundedOutline(678, 168, 538, 100, 14, 1, wutui::COL_BORDER);
    if (markedCount > 0) {
        std::string mTitle = std::to_string(markedCount) +
                             (markedCount == 1 ? " juego seleccionado (" : " juegos seleccionados (") +
                             wutui::UiCanvas::FormatBytes(st.MarkedTotalBytes()) + ")";
        ui.DrawTextCentered(688, 174, 518, 28, mTitle, wutui::COL_CYAN_DK, wutui::FontId::Title);
    } else if (pkgCount > 0 && st.selectedIdx >= 0 && st.selectedIdx < pkgCount) {
        const std::string& pName = st.PkgName(st.selectedIdx);
        uint64_t pBytes = st.PkgTotalBytes(st.selectedIdx);
        std::string sTitle = pName + " (" + wutui::UiCanvas::FormatBytes(pBytes) + ")";
        ui.DrawTextCentered(688, 174, 518, 28, sTitle, wutui::COL_CYAN_DK, wutui::FontId::Body);
    } else {
        ui.DrawTextCentered(688, 174, 518, 28, "Ningun juego seleccionado",
                            wutui::COL_MUTED, wutui::FontId::Body);
    }

    std::string usbStr = st.mochaUnlocked
        ? (st.usbAvailable ? wutui::UiCanvas::FormatBytes(st.usbFreeBytes) : "No conectado")
        : "?";
    std::string nandStr = st.mochaUnlocked ? wutui::UiCanvas::FormatBytes(st.nandFreeBytes) : "?";
    ui.DrawTextCentered(688, 208, 518, 22,
                        "Espacio libre  *  Disco USB: " + usbStr + "  |  Consola: " + nandStr,
                        wutui::COL_TEXT, wutui::FontId::Small);

    std::string sdStr = "Tarjeta SD: " + wutui::UiCanvas::FormatBytes(st.sdFreeBytes) + " libres";
    ui.DrawTextCentered(688, 234, 518, 22, sdStr, wutui::COL_GREEN_HI, wutui::FontId::Small);

    // Donde guardar (Disco USB vs Memoria Consola) (y=276..342)
    ui.DrawText(678, 276, "Donde guardar [Izq / Der]:", wutui::COL_MUTED, wutui::FontId::Small);

    ui.FillRoundedRect(678, 298, 260, 42, 21, st.targetUsb ? wutui::COL_CYAN_LT : wutui::COL_BTN_OFF);
    if (st.targetUsb) ui.DrawRoundedOutline(678, 298, 260, 42, 21, 3, wutui::COL_CYAN);
    ui.DrawTextCentered(678, 298, 260, 42, "Disco USB",
                        st.targetUsb ? wutui::COL_WHITE : wutui::COL_MUTED, wutui::FontId::Body);

    ui.FillRoundedRect(956, 298, 260, 42, 21, !st.targetUsb ? wutui::COL_CYAN_LT : wutui::COL_BTN_OFF);
    if (!st.targetUsb) ui.DrawRoundedOutline(956, 298, 260, 42, 21, 3, wutui::COL_CYAN);
    ui.DrawTextCentered(956, 298, 260, 42, "Memoria Consola",
                        !st.targetUsb ? wutui::COL_WHITE : wutui::COL_MUTED, wutui::FontId::Body);

    // Metodo de copia compacto en 1 linea (y=347..379)
    ui.DrawText(678, 352, "Metodo de copia [L / R]:", wutui::COL_MUTED, wutui::FontId::Small);
    bool isMocha = (st.mode == TransferMode::NetFsaMocha);
    const char* modeLabel = isMocha ? "Directo (Sin usar tarjeta SD)" : "Usando tarjeta SD temporal";

    ui.FillRoundedRect(868, 347, 348, 32, 16, isMocha ? wutui::COL_CYAN_LT : wutui::COL_CARD_ALT);
    ui.DrawRoundedOutline(868, 347, 348, 32, 16, isMocha ? 2 : 1,
                          isMocha ? wutui::COL_CYAN : wutui::COL_BORDER);
    ui.DrawTextCentered(868, 347, 348, 32, modeLabel, wutui::COL_TEXT, wutui::FontId::Small);

    // Botón principal Instalar / Cancelar (y=392..450)
    bool canInstall = (!st.installing && st.ActiveSourceConnected() && pkgCount > 0);
    uint32_t btnCol = st.cancelling ? wutui::COL_RED_DARK
                    : (st.installing ? wutui::COL_RED
                                     : (canInstall ? wutui::COL_CYAN : wutui::COL_BTN_OFF));
    std::string btnLabel;
    if (st.cancelling) {
        btnLabel = "Cancelando...";
    } else if (st.installing) {
        btnLabel = "Cancelar instalacion [B]";
    } else if (markedCount > 1) {
        btnLabel = "Instalar " + std::to_string(markedCount) + " juegos [A]";
    } else {
        btnLabel = "Instalar juego [A]";
    }
    ui.FillRoundedRect(678, 392, 538, 58, 16, btnCol);
    ui.DrawTextCentered(678, 392, 538, 58, btnLabel,
                        (canInstall || st.installing) ? wutui::COL_WHITE : wutui::COL_MUTED,
                        wutui::FontId::Title);

    // Tarjeta espaciosa de estado y progreso (x=678, y=464, w=538, h=168)
    ui.FillRoundedRect(678, 464, 538, 168, 14, wutui::COL_BOX_SOFT);
    ui.DrawRoundedOutline(678, 464, 538, 168, 14, 1, wutui::COL_BORDER);

    uint32_t stCol = st.statusIsError ? wutui::COL_RED
                   : (st.statusIsSuccess ? wutui::COL_GREEN_HI : wutui::COL_TEXT);
    ui.DrawText(696, 476, st.statusLine, stCol, wutui::FontId::Body, 502);
    if (!st.subStatusLine.empty()) {
        uint32_t subCol = st.statusIsSuccess ? wutui::COL_CYAN_DK : wutui::COL_MUTED;
        ui.DrawText(696, 504, st.subStatusLine, subCol, wutui::FontId::Small, 502);
    }

    ui.FillRoundedRect(696, 536, 502, 18, 9, wutui::COL_TRACK);
    float clamped = std::clamp(st.progress, 0.0f, 1.0f);
    int fillW = static_cast<int>(502.0f * clamped);
    if (fillW > 0) {
        uint32_t barCol = st.statusIsError ? wutui::COL_RED
                        : (st.statusIsSuccess ? wutui::COL_GREEN_HI : wutui::COL_CYAN);
        ui.FillRoundedRect(696, 536, fillW, 18, 9, barCol);
    }

    if (st.installing) {
        char sp[96];
        if (st.speedMBs > 0.01) {
            if (st.queueTotal > 1) {
                std::snprintf(sp, sizeof(sp), "Velocidad: %.2f MB/s (%d%%)  |  Juego %d de %d",
                              st.speedMBs, static_cast<int>(clamped * 100.0f),
                              st.queueCurrent, st.queueTotal);
            } else {
                std::snprintf(sp, sizeof(sp), "Velocidad: %.2f MB/s (%d%%)",
                              st.speedMBs, static_cast<int>(clamped * 100.0f));
            }
        } else if (st.queueTotal > 1) {
            std::snprintf(sp, sizeof(sp), "Progreso: %d%%  |  Juego %d de %d",
                          static_cast<int>(clamped * 100.0f), st.queueCurrent, st.queueTotal);
        } else {
            std::snprintf(sp, sizeof(sp), "Progreso: %d%%", static_cast<int>(clamped * 100.0f));
        }
        ui.DrawTextCentered(696, 574, 502, 32, sp, wutui::COL_CYAN_DK, wutui::FontId::Body);
    } else if (isUsbMode && st.usbFatConnected) {
        ui.FillRoundedRect(696, 566, 502, 50, 10, wutui::COL_CARD_ALT);
        ui.DrawRoundedOutline(696, 566, 502, 50, 10, 1, wutui::COL_CYAN_DK);
        ui.DrawTextCentered(696, 566, 502, 50,
                            "Aviso: Retira la memoria USB antes de salir al Menu",
                            wutui::COL_CYAN_SUB, wutui::FontId::Small);
    } else if (isUsbMode && st.mtpConnected) {
        ui.FillRoundedRect(696, 566, 502, 50, 10, wutui::COL_CARD_ALT);
        ui.DrawRoundedOutline(696, 566, 502, 50, 10, 1, wutui::COL_CYAN_DK);
        ui.DrawTextCentered(696, 566, 502, 50,
                            "Celular conectado por USB: instalacion directa",
                            wutui::COL_CYAN_SUB, wutui::FontId::Small);
    } else {
        ui.DrawTextCentered(696, 574, 502, 32,
                            "Formatos USB: exFAT, FAT32, NTFS, FAT y Android",
                            wutui::COL_MUTED, wutui::FontId::Small);
    }

    // 4. Footer controles simples (y=668..720)
    const char* footerText = st.installing
        ? "[B] o tocar 'Cancelar' para detener   |   [HOME] Menu Wii U"
        : "[Arr/Abj] Elegir  [-/ZL] Marcar  [X] USB/Red  [R-Stick] Musica  [A] Instalar  [Y] Buscar  [+] Salir";
    ui.FillRect(0, 668, wutui::LOGICAL_W, 52, wutui::COL_FOOTER);
    ui.DrawTextCentered(0, 668, wutui::LOGICAL_W, 52, footerText, wutui::COL_MUTED, wutui::FontId::Small);

    ui.Present();
}

void PresentDoubleBuffered(wutui::UiCanvas& ui, const AppState& st) {
    DrawUI(ui, st);
    DrawUI(ui, st);
}

bool PollCancelAndRender(wutui::UiCanvas& ui,
                         AppState& st,
                         InputManager& input,
                         OSTime& lastDrawTicks) {
    if (!WHBProcIsRunning()) {
        st.cancelling = true;
        return false;
    }

    InputSnapshot in = input.Poll(ui.HasForeground());
    if (in.triggered & ACT_MUTE) {
        if (gAudioMgr) gAudioMgr->ToggleMuted();
    }
    bool wantCancel = (((in.triggered | in.anyHeld) & ACT_B) != 0) || in.cancelTouchHit;
    if (wantCancel) {
        if (!st.cancelling) {
            st.cancelling = true;
            st.statusLine = "Cancelando...";
            st.subStatusLine = "Deteniendo instalacion...";
            st.speedMBs = 0.0;
            PresentDoubleBuffered(ui, st);
            lastDrawTicks = OSGetSystemTime();
        }
        return false;
    }

    if (ui.ConsumeResumedFlag()) {
        input.Flush();
        PresentDoubleBuffered(ui, st);
        lastDrawTicks = OSGetSystemTime();
    } else if (ui.HasForeground()) {
        OSTime now = OSGetSystemTime();
        if (OSTicksToMilliseconds(now - lastDrawTicks) >= UI_REFRESH_MS) {
            DrawUI(ui, st);
            lastDrawTicks = now;
        }
    }
    if (gAudioMgr) {
        gAudioMgr->Update(ui.HasForeground());
    }
    return true;
}

bool RunMcpInstallPhase(wutui::UiCanvas& ui,
                        AppState& st,
                        InputManager& input,
                        const std::string& mcpSourceDir,
                        float baseProg,
                        float progSpan,
                        OSTime& lastDrawTicks,
                        bool& outCancelled) {
    outCancelled = false;
    st.statusLine = st.targetUsb ? "Instalando en Disco USB..." : "Instalando en Memoria Consola...";
    st.subStatusLine = "Registrando juego en la consola...";
    st.speedMBs = 0.0;
    st.progress = baseProg;
    PresentDoubleBuffered(ui, st);
    lastDrawTicks = OSGetSystemTime();

    int32_t mcp = MCP_Open();
    if (mcp < 0) {
        st.statusLine = "No se pudo iniciar el instalador del sistema.";
        return false;
    }

    MCP_InstallSetTargetDevice(mcp, st.targetUsb ? MCP_INSTALL_TARGET_USB : MCP_INSTALL_TARGET_MLC);
    MCP_InstallSetTargetUsb(mcp, st.targetUsb ? 1 : 0);

    auto* infoBuf = static_cast<MCPInstallTitleInfo*>(MEMAllocFromDefaultHeapEx(sizeof(MCPInstallTitleInfo), 0x40));
    auto* progBuf = static_cast<MCPInstallProgress*>(MEMAllocFromDefaultHeapEx(sizeof(MCPInstallProgress), 0x40));
    if (!infoBuf || !progBuf) {
        if (infoBuf) MEMFreeToDefaultHeap(infoBuf);
        if (progBuf) MEMFreeToDefaultHeap(progBuf);
        MCP_Close(mcp);
        st.statusLine = "Memoria insuficiente para instalar.";
        return false;
    }
    std::memset(infoBuf, 0, sizeof(MCPInstallTitleInfo));
    std::memset(progBuf, 0, sizeof(MCPInstallProgress));

    bool ok = true;
    MCPError mErr = MCP_InstallTitleAsync(mcp, mcpSourceDir.c_str(), infoBuf);
    if (mErr < 0) {
        ok = false;
        char ebuf[64];
        std::snprintf(ebuf, sizeof(ebuf), "Error al instalar juego (%d)", mErr);
        st.statusLine = ebuf;
    } else {
        while (true) {
            if (!PollCancelAndRender(ui, st, input, lastDrawTicks)) {
                MCP_InstallTitleAbort(mcp);
                outCancelled = true;
                ok = false;
                break;
            }
            MCPError pErr = MCP_InstallGetProgress(mcp, progBuf);
            if (pErr == 0) {
                if (progBuf->inProgress == 0) break;
                if (progBuf->sizeTotal > 0) {
                    float mcpRatio = static_cast<float>(progBuf->sizeProgress) /
                                     static_cast<float>(progBuf->sizeTotal);
                    st.progress = baseProg + progSpan * mcpRatio;
                }
            } else if (pErr < 0) {
                ok = false;
                char ebuf[64];
                std::snprintf(ebuf, sizeof(ebuf), "Error durante la instalacion (%d)", pErr);
                st.statusLine = ebuf;
                break;
            }
            OSSleepTicks(OSMillisecondsToTicks(30));
        }
    }

    MEMFreeToDefaultHeap(infoBuf);
    MEMFreeToDefaultHeap(progBuf);
    MCP_Close(mcp);
    return ok;
}

struct InstallSessionGuard {
    AppState& st;
    InputManager& input;
    InstallSessionGuard(AppState& s, InputManager& in) : st(s), input(in) {
        st.cancelling = false;
        st.installing = true;
        input.BeginInstallGuard();
    }
    ~InstallSessionGuard() {
        st.cancelling = false;
        st.installing = false;
        st.queueCurrent = 0;
        st.queueTotal = 0;
        input.EndInstallGuard();
    }
};

bool RunSinglePackageInstall(wutui::UiCanvas& ui,
                             AppState& st,
                             InputManager& input,
                             int pkgIdx,
                             bool& outCancelled) {
    outCancelled = false;
    bool fromUsb = (st.source == PkgSource::UsbFat);
    bool fromMtp = (st.source == PkgSource::AndroidMtp);
    bool useFsa = (st.mode == TransferMode::NetFsaMocha);

    if (pkgIdx < 0 || pkgIdx >= st.ActivePkgCount()) return false;

    std::string pkgName;
    uint64_t pkgTotalBytes = 0;
    usbfat::UsbPkg usbPkgCopy;
    usbmtp::MtpPkg mtpPkgCopy;
    if (fromUsb) {
        usbPkgCopy = st.usbPkgs[pkgIdx];
        pkgName = usbPkgCopy.name;
        pkgTotalBytes = usbPkgCopy.totalBytes;
    } else if (fromMtp) {
        mtpPkgCopy = st.mtpPkgs[pkgIdx];
        pkgName = mtpPkgCopy.name;
        pkgTotalBytes = mtpPkgCopy.totalBytes;
    } else {
        pkgName = st.pkgs[pkgIdx].name;
        pkgTotalBytes = st.pkgs[pkgIdx].totalBytes;
    }

    st.progress = 0.0f;
    st.speedMBs = 0.0;
    st.statusIsError = false;
    st.statusIsSuccess = false;
    st.statusLine = fromMtp
        ? "Preparando copia desde Celular..."
        : (fromUsb
            ? "Preparando copia desde Memoria USB..."
            : "Preparando descarga por Red...");
    st.subStatusLine = pkgName;
    PresentDoubleBuffered(ui, st);

    OSTime lastDrawTicks = OSGetSystemTime();
    NetCancelCtx cancelCtx{&ui, &st, &input, &lastDrawTicks, false};

    UpdateAllFreeSpace(st);
    std::string stageFsDir;
    std::string stageMcpDir;

    if (useFsa) {
        if (!st.mochaUnlocked || st.fsaHandle < 0) {
            st.statusIsError = true;
            st.statusLine = "Modo directo no disponible.";
            st.subStatusLine = "Cambia el metodo de copia con [L / R].";
            return false;
        }
        const uint64_t padUsb  = 64ULL * 1024 * 1024;
        const uint64_t padNand = 128ULL * 1024 * 1024;
        std::string stagingBase;

        if (st.targetUsb) {
            if (!st.usbAvailable) {
                st.statusIsError = true;
                st.statusLine = "Disco USB de Wii U no conectado.";
                return false;
            }
            if (st.usbFreeBytes >= pkgTotalBytes * 2 + padUsb) {
                stagingBase = st.usbMountRoot + "/usr/tmp/wiiu_transfer";
            } else if (st.nandFreeBytes >= pkgTotalBytes + padNand) {
                stagingBase = "/vol/storage_mlc01/usr/tmp/wiiu_transfer";
            } else {
                st.statusIsError = true;
                st.statusLine = "Espacio insuficiente para copia directa.";
                st.subStatusLine = "Cambia a 'Usando tarjeta SD temporal' con [L/R].";
                return false;
            }
        } else {
            if (st.usbAvailable && st.usbFreeBytes >= pkgTotalBytes + padUsb) {
                stagingBase = st.usbMountRoot + "/usr/tmp/wiiu_transfer";
            } else if (st.nandFreeBytes >= pkgTotalBytes * 2 + padNand) {
                stagingBase = "/vol/storage_mlc01/usr/tmp/wiiu_transfer";
            } else {
                st.statusIsError = true;
                st.statusLine = "Espacio insuficiente en la consola.";
                st.subStatusLine = "Cambia a 'Usando tarjeta SD temporal' con [L/R].";
                return false;
            }
        }
        stageFsDir = stagingBase + "/" + pkgName;
        stageMcpDir = stageFsDir;
    } else {
        if (!st.sdMounted) {
            st.statusIsError = true;
            st.statusLine = "Error: Tarjeta SD no detectada.";
            return false;
        }
        if (pkgTotalBytes > 0 && st.sdFreeBytes < pkgTotalBytes + (16ULL * 1024 * 1024)) {
            st.statusIsError = true;
            st.statusLine = "Espacio insuficiente en la Tarjeta SD.";
            st.subStatusLine = "Necesita " + wutui::UiCanvas::FormatBytes(pkgTotalBytes);
            return false;
        }
        stageFsDir  = std::string(SD_TMP_BASE_FS) + "/" + pkgName;
        stageMcpDir = std::string(SD_TMP_BASE_MCP) + "/" + pkgName;
    }

    if (!PollCancelAndRender(ui, st, input, lastDrawTicks)) {
        outCancelled = true;
        return false;
    }

    int sock = -1;
    std::vector<RemoteFile> remoteFiles;
    if (!fromUsb && !fromMtp) {
        sock = ConnectTcp(st.serverIp, st.serverPort, 3000, &cancelCtx);
        if (sock < 0) {
            if (cancelCtx.cancelled) {
                outCancelled = true;
            } else {
                st.statusIsError = true;
                st.statusLine = "No se pudo conectar por Red.";
            }
            return false;
        }

        st.statusLine = "Preparando archivos del juego...";
        PresentDoubleBuffered(ui, st);
        lastDrawTicks = OSGetSystemTime();

        if (!CollectRemoteFilesRecursive(sock, pkgName, "", remoteFiles, &cancelCtx) || remoteFiles.empty()) {
            shutdown(sock, SHUT_RDWR);
            close(sock);
            if (cancelCtx.cancelled) {
                outCancelled = true;
            } else {
                st.statusIsError = true;
                st.statusLine = "Carpeta del juego vacia o error de lectura.";
            }
            return false;
        }
    } else if (fromUsb) {
        if (!st.usbFat.IsMounted() || usbPkgCopy.files.empty()) {
            st.statusIsError = true;
            st.statusLine = "Memoria USB desconectada o sin archivos.";
            st.subStatusLine = "Pulsa [Y] para volver a buscar.";
            return false;
        }
    } else {
        if (!st.mtpDev.IsConnected() || mtpPkgCopy.files.empty()) {
            st.statusIsError = true;
            st.statusLine = "Celular desconectado o sin archivos.";
            st.subStatusLine = "Pulsa [Y] para volver a buscar.";
            return false;
        }
    }

    // Asignar quad-buffer de 2 MB (8 MB en total) alineado a 0x40 + stack de hilo escritor en CPU2
    uint8_t* streamBufs[STREAM_BUF_COUNT]{nullptr, nullptr, nullptr, nullptr};
    auto* stdioVbuf = !useFsa ? static_cast<char*>(MEMAllocFromDefaultHeapEx(STDIO_BUF_SIZE, 0x40)) : nullptr;
    auto* stack = static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(WRITER_STACK_SIZE, 0x20));
    auto* wThread = static_cast<OSThread*>(MEMAllocFromDefaultHeapEx(sizeof(OSThread), 0x20));

    bool allocOk = (stack != nullptr) && (wThread != nullptr);
    for (uint32_t bi = 0; bi < STREAM_BUF_COUNT; ++bi) {
        streamBufs[bi] = static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(STREAM_BUF_SIZE, 0x40));
        if (!streamBufs[bi]) allocOk = false;
    }

    if (!allocOk) {
        for (uint32_t bi = 0; bi < STREAM_BUF_COUNT; ++bi) {
            if (streamBufs[bi]) MEMFreeToDefaultHeap(streamBufs[bi]);
        }
        if (stdioVbuf) MEMFreeToDefaultHeap(stdioVbuf);
        if (stack) MEMFreeToDefaultHeap(stack);
        if (wThread) MEMFreeToDefaultHeap(wThread);
        if (sock >= 0) close(sock);
        st.statusIsError = true;
        st.statusLine = "Memoria insuficiente para iniciar la copia.";
        return false;
    }

    if (useFsa) {
        FsaRemoveDirRecursive(st.fsaHandle, stageFsDir);
        FsaEnsureDirRecursive(st.fsaHandle, stageFsDir);
    } else {
        RemoveDirRecursive(stageFsDir);
        EnsureDirRecursive(stageFsDir);
    }

    WriterContext wctx{};
    wctx.useFsa = useFsa;
    wctx.fsaHandle = st.fsaHandle;
    for (uint32_t bi = 0; bi < STREAM_BUF_COUNT; ++bi) {
        wctx.bufs[bi] = streamBufs[bi];
    }
    OSInitMessageQueue(&wctx.freeQ, wctx.freeMsgs, STREAM_BUF_COUNT);
    OSInitMessageQueue(&wctx.writeQ, wctx.writeMsgs, 16);
    OSInitMessageQueue(&wctx.ackQ, wctx.ackMsgs, 4);

    for (uintptr_t i = 0; i < STREAM_BUF_COUNT; ++i) {
        OSMessage m{};
        m.message = reinterpret_cast<void*>(i);
        OSSendMessage(&wctx.freeQ, &m, OS_MESSAGE_FLAGS_BLOCKING);
    }

    OSCreateThread(wThread, WriterThreadEntry, 0, &wctx,
                   stack + WRITER_STACK_SIZE, WRITER_STACK_SIZE,
                   16, OS_THREAD_ATTRIB_AFFINITY_CPU2);
    OSResumeThread(wThread);

    uint64_t totalBytes = 0;
    size_t fileCount = fromMtp ? mtpPkgCopy.files.size()
                               : (fromUsb ? usbPkgCopy.files.size() : remoteFiles.size());
    if (fromMtp) {
        for (const auto& f : mtpPkgCopy.files) totalBytes += f.size;
    } else if (fromUsb) {
        for (const auto& f : usbPkgCopy.files) totalBytes += f.size;
    } else {
        for (const auto& f : remoteFiles) totalBytes += f.size;
    }

    uint64_t totalDownloaded = 0;
    bool ok = true;
    OSTime startTicks = OSGetSystemTime();
    lastDrawTicks = startTicks;
    std::string lastCreatedDir;

    for (size_t fi = 0; fi < fileCount && ok; ++fi) {
        if (!PollCancelAndRender(ui, st, input, lastDrawTicks)) {
            outCancelled = true;
            ok = false;
            wctx.cancelRequested = true;
            break;
        }

        const std::string& subPath = fromMtp ? mtpPkgCopy.files[fi].subPath
                                             : (fromUsb ? usbPkgCopy.files[fi].subPath : remoteFiles[fi].subPath);
        uint64_t fileSize = fromMtp ? mtpPkgCopy.files[fi].size
                                    : (fromUsb ? usbPkgCopy.files[fi].size : remoteFiles[fi].size);

        std::string localPath = stageFsDir + "/" + subPath;
        size_t slashPos = localPath.find_last_of('/');
        if (slashPos != std::string::npos) {
            std::string parentDir = localPath.substr(0, slashPos);
            if (parentDir != lastCreatedDir) {
                if (useFsa) FsaEnsureDirRecursive(st.fsaHandle, parentDir);
                else EnsureDirRecursive(parentDir);
                lastCreatedDir = std::move(parentDir);
            }
        }

        if (useFsa) {
            wctx.fsaFile = 0;
            FSError fErr = static_cast<FSError>(-1);
            if (fileSize > 0 && fileSize <= 0xFFFFFFFFULL) {
                fErr = FSAOpenFileEx(st.fsaHandle, localPath.c_str(), "wb",
                                     static_cast<FSMode>(0x666), FS_OPEN_FLAG_PREALLOC_SIZE,
                                     static_cast<uint32_t>(fileSize), &wctx.fsaFile);
            }
            if (fErr < 0) {
                fErr = FSAOpenFileEx(st.fsaHandle, localPath.c_str(), "wb",
                                     static_cast<FSMode>(0x666), FS_OPEN_FLAG_NONE, 0, &wctx.fsaFile);
            }
            if (fErr < 0) {
                ok = false;
                char ebuf[64];
                std::snprintf(ebuf, sizeof(ebuf), "Error preparando archivo (%d)", fErr);
                st.statusLine = ebuf;
                st.subStatusLine = subPath;
                break;
            }
        } else {
            wctx.stdioFile = std::fopen(localPath.c_str(), "wb");
            if (!wctx.stdioFile) {
                ok = false;
                st.statusLine = "Error creando archivo en la Tarjeta SD.";
                st.subStatusLine = subPath;
                break;
            }
            if (stdioVbuf) {
                std::setvbuf(wctx.stdioFile, stdioVbuf, _IOFBF, STDIO_BUF_SIZE);
            }
        }

        const char* prefix = fromMtp ? "Copiando del Celular (" : (fromUsb ? "Copiando de Memoria USB (" : "Recibiendo por Red (");
        st.statusLine = std::string(prefix) +
                        std::to_string(fi + 1) + "/" + std::to_string(fileCount) + ") " + pkgName;
        st.subStatusLine = subPath;

        uint64_t streamSize = 0;
        usbfat::UsbFatVolume::FileStream usbStream;
        usbmtp::UsbMtpDevice::FileStream mtpStream;
        if (fromMtp) {
            streamSize = fileSize;
            mtpStream.Open(&st.mtpDev, mtpPkgCopy.files[fi].objectHandle, fileSize);
        } else if (fromUsb) {
            streamSize = fileSize;
            usbStream.Open(&st.usbFat, usbPkgCopy.files[fi]);
        } else {
            bool streamStarted = false;
            for (int attempt = 0; attempt < 2 && !streamStarted; ++attempt) {
                if (attempt > 0) {
                    if (sock >= 0) {
                        shutdown(sock, SHUT_RDWR);
                        close(sock);
                    }
                    sock = ConnectTcp(st.serverIp, st.serverPort, 4000, &cancelCtx);
                    if (sock < 0) break;
                }
                NetFsaHeader resp{};
                const std::string& relPath = remoteFiles[fi].relPath;
                if (SendCmdAndHeader(sock, NETFSA_CMD_STREAM, relPath.data(),
                                     static_cast<uint32_t>(relPath.size()), resp, &cancelCtx) &&
                    resp.cmdOrStatus == NETFSA_OK && resp.payloadLen == sizeof(uint64_t) &&
                    RecvExact(sock, &streamSize, sizeof(streamSize), 15000, &cancelCtx)) {
                    streamStarted = true;
                } else if (cancelCtx.cancelled) {
                    break;
                }
            }
            if (!streamStarted) {
                ok = false;
                wctx.cancelRequested = true;
                if (cancelCtx.cancelled) {
                    outCancelled = true;
                } else {
                    st.statusLine = "Error iniciando descarga por Red.";
                }
                if (useFsa) FSACloseFile(st.fsaHandle, wctx.fsaFile);
                else std::fclose(wctx.stdioFile);
                break;
            }
        }

        uint64_t remaining = streamSize;
        const uint32_t activeSliceSize = fromUsb ? USB_SLICE_SIZE : NET_SLICE_SIZE;
        while (remaining > 0 && ok) {
            OSMessage freeMsg{};
            while (!OSReceiveMessage(&wctx.freeQ, &freeMsg, OS_MESSAGE_FLAGS_NONE)) {
                if (!PollCancelAndRender(ui, st, input, lastDrawTicks)) {
                    outCancelled = true;
                    ok = false;
                    wctx.cancelRequested = true;
                    break;
                }
                OSSleepTicks(OSMillisecondsToTicks(2));
            }
            if (!ok) break;

            uintptr_t bufIdx = reinterpret_cast<uintptr_t>(freeMsg.message);

            if (wctx.writeError) {
                ok = false;
                char ebuf[64];
                std::snprintf(ebuf, sizeof(ebuf),
                              useFsa ? "Error guardando datos (%d)" : "Error guardando en Tarjeta SD",
                              static_cast<int>(wctx.lastFsaErr));
                st.statusLine = ebuf;
                OSMessage ret{};
                ret.message = reinterpret_cast<void*>(bufIdx);
                OSSendMessage(&wctx.freeQ, &ret, OS_MESSAGE_FLAGS_BLOCKING);
                break;
            }

            uint32_t blockLen = static_cast<uint32_t>(std::min<uint64_t>(STREAM_BUF_SIZE, remaining));
            uint32_t blockReceived = 0;

            while (blockReceived < blockLen) {
                if (!PollCancelAndRender(ui, st, input, lastDrawTicks)) {
                    outCancelled = true;
                    ok = false;
                    wctx.cancelRequested = true;
                    break;
                }
                uint32_t slice = std::min<uint32_t>(activeSliceSize, blockLen - blockReceived);
                if (fromMtp) {
                    if (!mtpStream.ReadExact(wctx.bufs[bufIdx] + blockReceived, slice)) {
                        ok = false;
                        wctx.cancelRequested = true;
                        st.statusLine = "Error leyendo datos del Celular.";
                        break;
                    }
                } else if (fromUsb) {
                    if (!usbStream.ReadExact(wctx.bufs[bufIdx] + blockReceived, slice)) {
                        ok = false;
                        wctx.cancelRequested = true;
                        st.statusLine = "Error leyendo Memoria USB.";
                        break;
                    }
                } else {
                    if (!RecvExact(sock, wctx.bufs[bufIdx] + blockReceived, slice, 15000, &cancelCtx)) {
                        ok = false;
                        wctx.cancelRequested = true;
                        if (cancelCtx.cancelled) {
                            outCancelled = true;
                        } else {
                            st.statusLine = "Se perdio la conexion de Red.";
                        }
                        break;
                    }
                }
                blockReceived += slice;
                totalDownloaded += slice;

                uint64_t ms = OSTicksToMilliseconds(OSGetSystemTime() - startTicks);
                if (ms > 0) {
                    st.speedMBs = (static_cast<double>(totalDownloaded) / (1024.0 * 1024.0)) /
                                  (static_cast<double>(ms) / 1000.0);
                }
                if (totalBytes > 0) {
                    st.progress = 0.65f * (static_cast<float>(totalDownloaded) / static_cast<float>(totalBytes));
                }
            }

            if (ok && blockReceived > 0) {
                OSMessage wMsg{};
                wMsg.message = reinterpret_cast<void*>(bufIdx);
                wMsg.args[0] = blockReceived;
                wMsg.args[1] = WCMD_WRITE;
                OSSendMessage(&wctx.writeQ, &wMsg, OS_MESSAGE_FLAGS_BLOCKING);
                remaining -= blockReceived;
            } else {
                OSMessage ret{};
                ret.message = reinterpret_cast<void*>(bufIdx);
                OSSendMessage(&wctx.freeQ, &ret, OS_MESSAGE_FLAGS_BLOCKING);
            }
        }

        if (!ok) {
            wctx.cancelRequested = true;
            if (sock >= 0) {
                shutdown(sock, SHUT_RDWR);
                close(sock);
                sock = -1;
            }
        }

        OSMessage syncMsg{};
        syncMsg.args[1] = WCMD_SYNC;
        OSSendMessage(&wctx.writeQ, &syncMsg, OS_MESSAGE_FLAGS_BLOCKING);
        OSMessage ack{};
        while (!OSReceiveMessage(&wctx.ackQ, &ack, OS_MESSAGE_FLAGS_NONE)) {
            WHBProcIsRunning();
            OSSleepTicks(OSMillisecondsToTicks(2));
        }

        if (wctx.writeError && ok) {
            ok = false;
            st.statusLine = useFsa ? "Error guardando datos en la consola." : "Error guardando en la Tarjeta SD.";
        }

        if (useFsa) FSACloseFile(st.fsaHandle, wctx.fsaFile);
        else std::fclose(wctx.stdioFile);
    }

    if (sock >= 0) {
        if (!ok) shutdown(sock, SHUT_RDWR);
        close(sock);
        sock = -1;
    }
    if (!ok) {
        wctx.cancelRequested = true;
    }
    OSMessage quitMsg{};
    quitMsg.args[1] = WCMD_QUIT;
    OSSendMessage(&wctx.writeQ, &quitMsg, OS_MESSAGE_FLAGS_BLOCKING);
    int threadRes = 0;
    OSJoinThread(wThread, &threadRes);

    MEMFreeToDefaultHeap(wThread);
    MEMFreeToDefaultHeap(stack);
    if (stdioVbuf) MEMFreeToDefaultHeap(stdioVbuf);
    for (uint32_t bi = 0; bi < STREAM_BUF_COUNT; ++bi) {
        if (streamBufs[bi]) MEMFreeToDefaultHeap(streamBufs[bi]);
    }

    if (ok) {
        ok = RunMcpInstallPhase(ui, st, input, stageMcpDir, 0.66f, 0.34f, lastDrawTicks, outCancelled);
    }

    st.subStatusLine = "Limpiando archivos temporales...";
    PresentDoubleBuffered(ui, st);
    if (useFsa) FsaRemoveDirRecursive(st.fsaHandle, stageFsDir);
    else RemoveDirRecursive(stageFsDir);
    UpdateAllFreeSpace(st);

    return ok;
}

void RunInstallQueue(wutui::UiCanvas& ui, AppState& st, InputManager& input) {
    bool fromUsb = (st.source == PkgSource::UsbFat);
    bool fromMtp = (st.source == PkgSource::AndroidMtp);

    std::vector<int> queue;
    int pkgCount = st.ActivePkgCount();
    for (int i = 0; i < pkgCount; ++i) {
        if (st.IsMarked(i)) queue.push_back(i);
    }
    if (queue.empty() && st.selectedIdx >= 0 && st.selectedIdx < pkgCount) {
        queue.push_back(st.selectedIdx);
    }
    if (queue.empty()) return;

    InstallSessionGuard sessionGuard(st, input);
    st.queueTotal = static_cast<int>(queue.size());
    st.queueCurrent = 0;

    int installedOk = 0;
    bool cancelled = false;
    bool hadError = false;

    for (size_t qi = 0; qi < queue.size(); ++qi) {
        int idx = queue[qi];
        st.queueCurrent = static_cast<int>(qi) + 1;
        st.selectedIdx = idx;
        st.ClampSelection();

        bool pkgCancelled = false;
        bool ok = RunSinglePackageInstall(ui, st, input, idx, pkgCancelled);
        if (ok) {
            ++installedOk;
            st.SetMarked(idx, false);
        } else if (pkgCancelled) {
            cancelled = true;
            break;
        } else {
            hadError = true;
            break;
        }
    }

    st.cancelling = false;
    st.installing = false;
    st.speedMBs = 0.0;
    int totalQ = static_cast<int>(queue.size());
    st.queueCurrent = 0;
    st.queueTotal = 0;

    if (!cancelled && !hadError && installedOk == totalQ) {
        st.progress = 1.0f;
        st.statusIsError = false;
        st.statusIsSuccess = true;
        if (totalQ > 1) {
            st.statusLine = "Se instalaron " + std::to_string(installedOk) + " de " + std::to_string(totalQ) + " juegos!";
        } else if (fromMtp) {
            st.statusLine = "Juego instalado desde tu Celular!";
        } else if (fromUsb) {
            st.statusLine = "Juego instalado desde Memoria USB!";
        } else {
            st.statusLine = "Juego instalado correctamente!";
        }
        st.subStatusLine = fromUsb
            ? "Retira la memoria USB antes de salir al Menu Wii U."
            : (st.targetUsb ? "Guardado en Disco USB." : "Guardado en Memoria Consola.");
    } else if (cancelled) {
        st.progress = 0.0f;
        st.statusIsError = false;
        st.statusIsSuccess = false;
        if (totalQ > 1) {
            st.statusLine = "Instalacion cancelada (" + std::to_string(installedOk) + "/" + std::to_string(totalQ) + " listos).";
        } else {
            st.statusLine = "Instalacion cancelada.";
        }
        st.subStatusLine = fromUsb
            ? "Archivos temporales borrados. Retira la memoria USB al salir."
            : "Archivos temporales borrados.";
    } else {
        st.statusIsError = true;
        st.statusIsSuccess = false;
    }
}

void HandleTouch(int x,
                 int y,
                 AppState& st,
                 bool& triggerScan,
                 bool& triggerSwitchSource,
                 PkgSource& targetSource,
                 bool& triggerInstall,
                 bool& dirty) {
    // Header: Selector Modo USB | Modo Red (x=532..876, y<=96)
    if (y <= 96 && x >= 532 && x <= 876) {
        bool currentlyUsb = (st.source != PkgSource::NetworkPc);
        bool wantUsb = (x < 704);
        if (wantUsb != currentlyUsb) {
            targetSource = wantUsb
                ? (st.mtpConnected && !st.usbFatConnected ? PkgSource::AndroidMtp : PkgSource::UsbFat)
                : PkgSource::NetworkPc;
            triggerSwitchSource = true;
        }
        return;
    }
    // Header: Botón Musica SI/NO (x=884..1024, y<=96)
    if (y <= 96 && x >= 884 && x <= 1024) {
        if (gAudioMgr) {
            gAudioMgr->ToggleMuted();
            dirty = true;
        }
        return;
    }
    // Pill header derecha (Buscar Memoria USB / Celular / Red) (x>=1030, y<=96)
    if (x >= 1030 && y <= 96) {
        triggerScan = true;
        return;
    }
    int pkgCount = st.ActivePkgCount();
    // Boton "Todos / Limpiar" en encabezado izquierdo (x=408..512, y=126..176)
    if (pkgCount > 0 && y >= 126 && y <= 176 && x >= 408 && x <= 512) {
        st.ToggleMarkAll();
        dirty = true;
        return;
    }
    // Botones scroll lista (si hay mas de 4 juegos)
    if (pkgCount > 4 && y >= 126 && y <= 176) {
        if (x >= 514 && x <= 562) {
            if (st.scrollOffset > 0) {
                --st.scrollOffset;
                if (st.selectedIdx >= st.scrollOffset + 4) {
                    st.selectedIdx = st.scrollOffset + 3;
                }
                dirty = true;
            }
            return;
        }
        if (x >= 563 && x <= 614) {
            int maxOffset = pkgCount - 4;
            if (st.scrollOffset < maxOffset) {
                ++st.scrollOffset;
                if (st.selectedIdx < st.scrollOffset) {
                    st.selectedIdx = st.scrollOffset;
                }
                dirty = true;
            }
            return;
        }
    }
    // Tarjetas de juegos (x=54..612, y=180..628)
    if (x >= 54 && x <= 612 && y >= 180 && y <= 628 && pkgCount > 0) {
        int slot = std::clamp((y - 186) / 112, 0, 3);
        int idx = st.scrollOffset + slot;
        if (idx >= 0 && idx < pkgCount) {
            bool hitCheckbox = (x <= 116);
            if (hitCheckbox || st.selectedIdx == idx) {
                st.selectedIdx = idx;
                st.ToggleMarked(idx);
            } else {
                st.selectedIdx = idx;
            }
            dirty = true;
        }
        return;
    }
    // Donde guardar: Disco USB / Memoria Consola (y=292..344)
    if (y >= 292 && y <= 344) {
        if (x >= 668 && x <= 947) {
            st.targetUsb = true;
            dirty = true;
            return;
        }
        if (x >= 948 && x <= 1226) {
            st.targetUsb = false;
            dirty = true;
            return;
        }
    }
    // Selector Metodo de copia (y=345..384)
    if (x >= 668 && x <= 1226 && y >= 345 && y <= 384) {
        st.mode = (st.mode == TransferMode::NetFsaMocha)
            ? TransferMode::DirectSyncSd
            : TransferMode::NetFsaMocha;
        dirty = true;
        return;
    }
    // Botón Instalar (y=388..456)
    if (x >= 664 && x <= 1230 && y >= 388 && y <= 456) {
        triggerInstall = true;
    }
}

} // namespace

int main() {
    WHBProcInit();
    ACInitialize();

    static SocketMemoryBooster sockBooster;
    sockBooster.Init();

    wutui::UiCanvas ui;
    if (!ui.Init()) {
        ACFinalize();
        WHBProcShutdown();
        return 1;
    }

    // Mostrar pantalla de inicio inmediatamente (misma imagen que el splash del .wuhb)
    ui.DrawSplashScreen();
    ui.Present();
    ui.DrawSplashScreen();
    ui.Present();

    // Iniciar sonido "Pantalla Inicio" de inmediato al abrir la app (solo carga 158 KB, <20 ms)
    wutaudio::AudioManager audio;
    audio.InitAxAndIntro();
    gAudioMgr = &audio;
    audio.StartIntro();

    InputManager input;
    input.Init();

    AppState st;
    bool sdMountedByUs = (WHBMountSdCard() != FALSE);

    // Mientras suena "Pantalla Inicio" en la pantalla de carga, decodificamos BGM e inicializamos Mocha, USB y Red
    audio.LoadBgm();
    InitMochaFsa(st);
    UpdateAllFreeSpace(st);
    bool foundUsbOnBoot = ScanUnifiedUsb(st);
    bool foundPcOnBoot = false;
    if (!foundUsbOnBoot) {
        st.source = PkgSource::NetworkPc;
        st.statusIsError = false;
        foundPcOnBoot = DiscoverServer(st);
        if (foundPcOnBoot) {
            FetchPackageList(st);
        }
    }

    // Una vez lista la herramienta, pasar directo a la interfaz de uso (sin esperar ningun boton)
    // y comenzar a reproducir "Tema de Fondo" en bucle continuo
    audio.StartBgm();

    if (foundUsbOnBoot) {
        // ScanUnifiedUsb ya configuró el mensaje de Memoria USB o Celular conectado
    } else if (foundPcOnBoot) {
        st.statusLine = "Conectado por Red a " + st.serverHost;
        st.subStatusLine = std::to_string(st.pkgs.size()) + " juego(s) listo(s) para instalar.";
        st.statusIsSuccess = true;
    } else {
        st.statusLine = "Listo. Elige Modo USB o Modo Red con [X].";
        st.subStatusLine = "Pulsa [Y] para buscar juegos en USB o en Red.";
    }
    PresentDoubleBuffered(ui, st);
    input.Flush();

    int redrawFrames = 2;

    while (WHBProcIsRunning()) {
        audio.Update(ui.HasForeground());
        bool triggerScan = false;
        bool triggerSwitchSource = false;
        bool currentlyUsb = (st.source != PkgSource::NetworkPc);
        PkgSource nextSource = currentlyUsb
            ? PkgSource::NetworkPc
            : (st.mtpConnected && !st.usbFatConnected ? PkgSource::AndroidMtp : PkgSource::UsbFat);
        bool triggerInstall = false;
        bool dirty = false;

        if (ui.HasForeground()) {
            InputSnapshot in = input.Poll(true);
            int pkgCount = st.ActivePkgCount();

            if (in.triggered & ACT_MUTE) {
                audio.ToggleMuted();
                dirty = true;
            }

            if ((in.repeated & ACT_DOWN) && pkgCount > 0) {
                st.selectedIdx = in.isRepeatPulse
                    ? std::min(pkgCount - 1, st.selectedIdx + 1)
                    : ((st.selectedIdx + 1) % pkgCount);
                st.ClampSelection();
                dirty = true;
            } else if ((in.repeated & ACT_UP) && pkgCount > 0) {
                st.selectedIdx = in.isRepeatPulse
                    ? std::max(0, st.selectedIdx - 1)
                    : ((st.selectedIdx - 1 + pkgCount) % pkgCount);
                st.ClampSelection();
                dirty = true;
            }

            if (in.triggered & ACT_LEFT) {
                if (!st.targetUsb) {
                    st.targetUsb = true;
                    dirty = true;
                }
            } else if (in.triggered & ACT_RIGHT) {
                if (st.targetUsb) {
                    st.targetUsb = false;
                    dirty = true;
                }
            }

            if ((in.triggered & ACT_MARK) && pkgCount > 0) {
                st.ToggleMarked(st.selectedIdx);
                dirty = true;
            }
            if ((in.triggered & ACT_MARK_ALL) && pkgCount > 0) {
                st.ToggleMarkAll();
                dirty = true;
            }

            if (in.triggered & ACT_X) {
                triggerSwitchSource = true;
            }
            if (in.triggered & (ACT_MODE_L | ACT_MODE_R)) {
                st.mode = (st.mode == TransferMode::NetFsaMocha)
                    ? TransferMode::DirectSyncSd
                    : TransferMode::NetFsaMocha;
                dirty = true;
            }
            if (in.triggered & ACT_Y) {
                triggerScan = true;
            }
            if (in.triggered & ACT_A) {
                triggerInstall = true;
            }
            if (in.triggered & ACT_EXIT) {
                SYSLaunchMenu();
            }

            if (in.touchTap) {
                HandleTouch(in.tapX, in.tapY, st, triggerScan, triggerSwitchSource, nextSource, triggerInstall, dirty);
            }
        }

        if (triggerSwitchSource) {
            st.source = nextSource;
            st.selectedIdx = 0;
            st.scrollOffset = 0;
            st.ClampSelection();
            if (st.source != PkgSource::NetworkPc && !st.usbFatConnected && !st.mtpConnected) {
                triggerScan = true;
            } else if (st.source == PkgSource::NetworkPc && !st.connected) {
                triggerScan = true;
            } else {
                st.statusIsError = false;
                st.statusIsSuccess = st.ActiveSourceConnected();
                if (st.source == PkgSource::UsbFat) {
                    st.statusLine = std::string("Memoria USB detectada (") + st.usbFat.FsName() + ")";
                    st.subStatusLine = std::to_string(st.usbPkgs.size()) + " juego(s) listo(s) para instalar.";
                } else if (st.source == PkgSource::AndroidMtp) {
                    st.statusLine = "Celular conectado (" + st.mtpDeviceName + ")";
                    st.subStatusLine = std::to_string(st.mtpPkgs.size()) + " juego(s) listo(s) para instalar.";
                } else {
                    st.statusLine = st.connected
                        ? ("Conectado por Red a " + st.serverHost)
                        : "Sin conexion de Red. Pulsa [Y] para buscar.";
                    st.subStatusLine.clear();
                }
            }
            dirty = true;
        }

        if (triggerScan) {
            st.statusIsError = false;
            st.statusIsSuccess = false;
            if (st.source != PkgSource::NetworkPc) {
                st.statusLine = "Buscando Memoria USB o Celular Android...";
                st.subStatusLine = "Detectando exFAT, FAT32, NTFS, FAT o Android...";
                PresentDoubleBuffered(ui, st);
                ScanUnifiedUsb(st);
            } else {
                st.statusLine = "Buscando juegos en Red (PC o Celular)...";
                st.subStatusLine.clear();
                PresentDoubleBuffered(ui, st);
                if (DiscoverServer(st)) {
                    FetchPackageList(st);
                    st.statusIsSuccess = true;
                    st.statusLine = "Conectado por Red a " + st.serverHost;
                    st.subStatusLine = std::to_string(st.pkgs.size()) + " juego(s) encontrado(s).";
                } else {
                    st.statusIsError = true;
                    st.statusLine = "No se encontro PC ni Celular en la Red.";
                    st.subStatusLine = "Abre el programa en tu PC o App en tu Celular.";
                }
            }
            input.Flush();
            dirty = true;
        }

        if (triggerInstall && st.ActiveSourceConnected() && st.ActivePkgCount() > 0) {
            RunInstallQueue(ui, st, input);
            dirty = true;
        }

        if (ui.ConsumeResumedFlag()) {
            input.Flush();
            dirty = true;
        }
        if (dirty) redrawFrames = 2;
        if (redrawFrames > 0 && ui.HasForeground()) {
            DrawUI(ui, st);
            --redrawFrames;
        }

        OSSleepTicks(OSMillisecondsToTicks(16));
    }

    gAudioMgr = nullptr;
    audio.Shutdown();
    input.Shutdown();
    ShutdownMochaFsa(st);
    if (sdMountedByUs) WHBUnmountSdCard();
    ui.Shutdown();
    ACFinalize();
    WHBProcShutdown();
    return 0;
}
