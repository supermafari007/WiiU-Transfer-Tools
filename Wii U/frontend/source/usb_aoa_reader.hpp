#pragma once

#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/time.h>
#include <nsysuhs/uhs.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "usb_fat_reader.hpp"

namespace usbaoa {

// Protocolo binario directo WUAO sobre Android Open Accessory (AOA v1/v2)
constexpr uint32_t kMagicReq = 0x4F415557u; // "WUAO"
constexpr uint32_t kMagicOk  = 0x4B4F5557u; // "WUOK"
constexpr uint32_t kMagicErr = 0x52455557u; // "WUER"

constexpr uint32_t kCmdPing      = 1;
constexpr uint32_t kCmdListPkgs  = 2;
constexpr uint32_t kCmdOpenFile  = 3;
constexpr uint32_t kCmdReadChunk = 4;
constexpr uint32_t kCmdCloseFile = 5;

constexpr uint16_t kGoogleVid    = 0x18D1u;
constexpr uint16_t kAoaPidMin    = 0x2D00u;
constexpr uint16_t kAoaPidMax    = 0x2D05u;

constexpr uint32_t kAoaDmaSize   = 16 * 1024; // 16 KB (limite seguro en IOS-USB/nsysuhs y FunctionFS AOA)
constexpr uint32_t kAoaChunkSize = 12 * 1024; // 12 KB datos + 12 B header = 12300 B (< 16 KB y no multiplo de 512)
constexpr int32_t  kAoaMaxProfs  = 32;

struct AoaFile {
    uint32_t fileId = 0;   // ID numerico indexado en la App Android (evita fallos de rutas/UTF-8)
    std::string subPath;   // Ruta relativa dentro del paquete (ej. "title.tmd")
    std::string fullRel;   // Ruta relativa dentro de activeFolder (ej. "MiJuego/title.tmd")
    uint64_t size = 0;
};

struct AoaPkg {
    std::string name;
    std::vector<AoaFile> files;
    uint64_t totalBytes = 0;
    bool marked = false;
};

inline void Wr32LE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

inline void Wr64LE(uint8_t* p, uint64_t v) {
    Wr32LE(p, static_cast<uint32_t>(v & 0xFFFFFFFFULL));
    Wr32LE(p + 4, static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFULL));
}

class UsbAoaDevice {
public:
    class FileStream {
    public:
        FileStream() = default;
        ~FileStream() { Close(); }

        bool Open(UsbAoaDevice* dev, uint32_t fileId, uint64_t expectedSize) {
            Close();
            dev_ = dev;
            offset_ = 0;
            remaining_ = 0;
            opened_ = false;
            if (!dev_ || !dev_->IsConnected()) return false;

            uint64_t remoteSize = 0;
            if (!dev_->OpenRemoteFile(fileId, remoteSize)) {
                return false;
            }
            size_ = (remoteSize > 0) ? remoteSize : expectedSize;
            remaining_ = size_;
            opened_ = true;
            return true;
        }

        void Close() {
            // CMD_OPEN_FILE cierra automaticamente el archivo anterior en Android,
            // evitando trafico extra innecesario entre archivos.
            opened_ = false;
            dev_ = nullptr;
            remaining_ = 0;
        }

        uint64_t Remaining() const { return remaining_; }

        bool ReadExact(void* dst, uint32_t len) {
            if (!opened_ || !dev_ || len > remaining_) return false;
            if (len == 0) return true;

            auto* out = static_cast<uint8_t*>(dst);
            uint32_t got = 0;
            while (got < len) {
                uint32_t want = std::min<uint32_t>(len - got, kAoaChunkSize);
                uint32_t chunkGot = 0;
                bool ok = false;
                for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
                    if (attempt > 0) OSSleepTicks(OSMillisecondsToTicks(20));
                    if (dev_->ReadRemoteChunk(offset_, want, out + got, chunkGot) && chunkGot > 0) {
                        ok = true;
                    }
                }
                if (!ok) return false;
                offset_ += chunkGot;
                remaining_ -= chunkGot;
                got += chunkGot;
            }
            return true;
        }

    private:
        UsbAoaDevice* dev_ = nullptr;
        uint64_t size_ = 0;
        uint64_t offset_ = 0;
        uint64_t remaining_ = 0;
        bool opened_ = false;
    };

    UsbAoaDevice() = default;
    ~UsbAoaDevice() { Shutdown(); }

    UsbAoaDevice(const UsbAoaDevice&) = delete;
    UsbAoaDevice& operator=(const UsbAoaDevice&) = delete;

    bool IsConnected() const { return connected_; }
    const std::string& DeviceName() const { return deviceName_; }
    const std::string& LastError() const { return lastErr_; }

    void Disconnect() {
        if (ifAcquired_ && uhsOpened_) {
            UhsReleaseInterface(&uhsHandle_, ifHandle_, false);
            ifAcquired_ = false;
        }
        if (uhsOpened_) {
            UhsClientClose(&uhsHandle_);
            uhsOpened_ = false;
        }
        connected_ = false;
        ifHandle_ = 0;
        epIn_ = 0;
        epOut_ = 0;
    }

    void Shutdown() {
        Disconnect();
        if (dmaBuf_) {
            MEMFreeToDefaultHeap(dmaBuf_);
            dmaBuf_ = nullptr;
        }
        if (ctrlBuf_) {
            MEMFreeToDefaultHeap(ctrlBuf_);
            ctrlBuf_ = nullptr;
        }
        if (uhsCfgBuf_) {
            MEMFreeToDefaultHeap(uhsCfgBuf_);
            uhsCfgBuf_ = nullptr;
        }
        if (profilesBuf_) {
            MEMFreeToDefaultHeap(profilesBuf_);
            profilesBuf_ = nullptr;
        }
    }

    bool ProbeAndConnect(std::string& outDeviceName, std::string& outDiag) {
        outDeviceName.clear();
        outDiag.clear();
        lastErr_.clear();

        if (!EnsureBuffers()) {
            outDiag = "Sin memoria para driver USB AOA.";
            return false;
        }

        // Si ya tenemos una sesion AOA viva, verificarla con PING sin desconectar
        if (connected_) {
            std::string model;
            if (PingDevice(model, 1500)) {
                if (!model.empty()) deviceName_ = model;
                outDeviceName = deviceName_;
                return true;
            }
            Disconnect();
        }

        // 1. Buscar primero si el celular YA esta en modo AOA (0x18D1:0x2D00..0x2D05)
        if (TryAttachAoaDevice(outDeviceName, outDiag)) {
            return true;
        }

        // 2. Si no esta en AOA todavia, enviar handshake AOA (Control 51/52/53) a cualquier Android conectado
        bool switched = TrySwitchPhoneToAoa(outDiag);
        if (!switched) {
            if (outDiag.empty()) {
                outDiag = "Conecta tu celular Android por USB y abre Wii U Transfer Tools.";
            }
            return false;
        }

        // 3. Esperar a que Android re-enumere como 0x18D1:0x2D00..0x2D05 (hasta 4.0 s)
        for (int tries = 0; tries < 16; ++tries) {
            OSSleepTicks(OSMillisecondsToTicks(250));
            if (TryAttachAoaDevice(outDeviceName, outDiag)) {
                return true;
            }
        }

        if (outDiag.empty()) {
            outDiag = "Acepta el aviso USB en tu celular y pulsa [Y] otra vez.";
        }
        return false;
    }

    bool ScanWupPackages(std::vector<AoaPkg>& outPkgs, std::string& outDiag) {
        outPkgs.clear();
        outDiag.clear();
        if (!connected_) {
            outDiag = "Celular USB no conectado.";
            return false;
        }

        std::vector<uint8_t> respPayload;
        uint32_t status = 0;
        if (!ExchangeCommand(kCmdListPkgs, nullptr, 0, respPayload, status, 6000) || status != 0) {
            outDiag = lastErr_.empty()
                ? "Acepta el permiso USB en tu celular y pulsa [Y]."
                : lastErr_;
            return false;
        }

        std::string text(reinterpret_cast<const char*>(respPayload.data()), respPayload.size());
        std::istringstream iss(text);
        std::string line;
        AoaPkg* curPkg = nullptr;

        while (std::getline(iss, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
                line.pop_back();
            }
            if (line.size() < 3) continue;

            if (line[0] == 'P' && line[1] == '|') {
                // P|name|marked|totalBytes
                std::istringstream ls(line.substr(2));
                std::string name, mStr, szStr;
                std::getline(ls, name, '|');
                std::getline(ls, mStr, '|');
                std::getline(ls, szStr, '|');
                if (name.empty()) continue;

                AoaPkg pkg;
                pkg.name = name;
                pkg.marked = (mStr == "1");
                pkg.totalBytes = std::strtoull(szStr.c_str(), nullptr, 10);
                outPkgs.push_back(std::move(pkg));
                curPkg = &outPkgs.back();
            } else if (line[0] == 'F' && line[1] == '|' && curPkg != nullptr) {
                // F|fileId|subPath|size
                std::istringstream ls(line.substr(2));
                std::string idStr, sub, szStr;
                std::getline(ls, idStr, '|');
                std::getline(ls, sub, '|');
                std::getline(ls, szStr, '|');
                if (sub.empty()) continue;

                AoaFile f;
                f.fileId = static_cast<uint32_t>(std::strtoul(idStr.c_str(), nullptr, 10));
                f.subPath = sub;
                f.fullRel = curPkg->name + "/" + sub;
                f.size = std::strtoull(szStr.c_str(), nullptr, 10);
                curPkg->files.push_back(std::move(f));
            }
        }

        // Eliminar paquetes sin archivos
        outPkgs.erase(
            std::remove_if(outPkgs.begin(), outPkgs.end(),
                           [](const AoaPkg& p) { return p.files.empty(); }),
            outPkgs.end());

        if (outPkgs.empty()) {
            outDiag = "No hay juegos en la carpeta elegida en la App Android.";
        }
        return true;
    }

private:
    static void OnInterfaceEvent(void* /*ctx*/, int32_t /*arg1*/, int32_t /*arg2*/) {}

    static bool IsAoaVidPid(uint16_t vid, uint16_t pid) {
        return (vid == kGoogleVid && pid >= kAoaPidMin && pid <= kAoaPidMax);
    }

    bool EnsureBuffers() {
        if (!dmaBuf_) {
            dmaBuf_ = static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(kAoaDmaSize, 0x40));
        }
        if (!ctrlBuf_) {
            ctrlBuf_ = static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(256, 0x40));
        }
        if (!uhsCfgBuf_) {
            uhsCfgBuf_ = static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(0x1400, 0x40));
        }
        if (!profilesBuf_) {
            profilesBuf_ = static_cast<UhsInterfaceProfile*>(
                MEMAllocFromDefaultHeapEx(sizeof(UhsInterfaceProfile) * kAoaMaxProfs, 0x40));
        }
        return dmaBuf_ && ctrlBuf_ && uhsCfgBuf_ && profilesBuf_;
    }

    bool SendAoaString(uint32_t ifH, uint16_t idx, const char* str) {
        uint16_t len = static_cast<uint16_t>(std::strlen(str) + 1);
        std::memcpy(ctrlBuf_, str, len);
        DCFlushRange(ctrlBuf_, len);
        UHSStatus st = UhsSubmitControlRequest(
            &uhsHandle_, ifH, ctrlBuf_, 52, 0x40, 0, idx, len, 2000);
        return st >= 0;
    }

    bool TrySwitchPhoneToAoa(std::string& outDiag) {
        std::string seenUsb;

        for (uint32_t ctrl = 0; ctrl < 3; ++ctrl) {
            std::memset(&uhsHandle_, 0, sizeof(uhsHandle_));
            std::memset(&uhsConfig_, 0, sizeof(uhsConfig_));
            std::memset(uhsCfgBuf_, 0, 0x1400);
            uhsConfig_.controller_num = ctrl;
            uhsConfig_.buffer = uhsCfgBuf_;
            uhsConfig_.buffer_size = UHS_CONFIG_BUFFER_SIZE;
            DCFlushRange(uhsCfgBuf_, 0x1400);

            if (UhsClientOpen(&uhsHandle_, &uhsConfig_) != UHS_STATUS_OK) continue;
            uhsOpened_ = true;

            UhsInterfaceFilter filter{};
            filter.match_params = MATCH_ANY;
            std::memset(profilesBuf_, 0, sizeof(UhsInterfaceProfile) * kAoaMaxProfs);

            int32_t numIfs = static_cast<int32_t>(
                UhsQueryInterfaces(&uhsHandle_, &filter, profilesBuf_, kAoaMaxProfs));
            int32_t count = std::min<int32_t>(std::max<int32_t>(0, numIfs), kAoaMaxProfs);

            for (int32_t i = 0; i < count; ++i) {
                const UhsInterfaceProfile& prof = profilesBuf_[i];
                uint8_t cls = prof.if_desc.bInterfaceClass;

                // Saltar memorias USB, hubs, mandos HID, audio, video, impresoras, BT/WiFi
                if (cls == USBCLASS_STORAGE || cls == 0x09 || cls == USBCLASS_HID ||
                    cls == USBCLASS_AUDIO || cls == 0x0E || cls == 0x07 || cls == 0xE0) {
                    continue;
                }
                if (IsAoaVidPid(prof.dev_desc.idVendor, prof.dev_desc.idProduct)) {
                    UhsClientClose(&uhsHandle_);
                    uhsOpened_ = false;
                    return true;
                }

                char sb[48];
                std::snprintf(sb, sizeof(sb), "%04X:%04X", prof.dev_desc.idVendor, prof.dev_desc.idProduct);
                seenUsb = sb;

                if (UhsAcquireInterface(&uhsHandle_, prof.if_handle, nullptr, OnInterfaceEvent) != UHS_STATUS_OK) {
                    continue;
                }

                // 1. AOA Get Protocol (bRequest=51, IN vendor)
                std::memset(ctrlBuf_, 0, 16);
                DCInvalidateRange(ctrlBuf_, 16);
                UHSStatus st = UhsSubmitControlRequest(
                    &uhsHandle_, prof.if_handle, ctrlBuf_, 51, 0xC0, 0, 0, 2, 2000);
                DCInvalidateRange(ctrlBuf_, 16);
                uint16_t aoaVer = (st >= 2) ? usbfat::Rd16LE(ctrlBuf_) : 0;

                if (aoaVer >= 1) {
                    // 2. Enviar cadenas de identificacion AOA (coinciden con accessory_filter.xml)
                    bool okStr =
                        SendAoaString(prof.if_handle, 0, "El Diuca") &&
                        SendAoaString(prof.if_handle, 1, "Wii U Transfer Tools") &&
                        SendAoaString(prof.if_handle, 2, "Instalador USB directo de juegos Wii U") &&
                        SendAoaString(prof.if_handle, 3, "2.0.2") &&
                        SendAoaString(prof.if_handle, 4, "https://github.com/supermafari007/WiiU-Transfer-Tools") &&
                        SendAoaString(prof.if_handle, 5, "WIIU-TT-001");

                    // 3. Iniciar modo AOA (bRequest=53, OUT vendor)
                    if (okStr) {
                        UhsSubmitControlRequest(&uhsHandle_, prof.if_handle, nullptr, 53, 0x40, 0, 0, 0, 2000);
                        UhsReleaseInterface(&uhsHandle_, prof.if_handle, false);
                        UhsClientClose(&uhsHandle_);
                        uhsOpened_ = false;
                        return true;
                    }
                }

                UhsReleaseInterface(&uhsHandle_, prof.if_handle, false);
            }

            UhsClientClose(&uhsHandle_);
            uhsOpened_ = false;
        }

        if (!seenUsb.empty()) {
            outDiag = "USB " + seenUsb + " detectado: desbloquea tu celular y abre la App Android.";
        }
        return false;
    }

    bool TryAttachAoaDevice(std::string& outDeviceName, std::string& outDiag) {
        Disconnect();

        for (uint32_t ctrl = 0; ctrl < 3; ++ctrl) {
            std::memset(&uhsHandle_, 0, sizeof(uhsHandle_));
            std::memset(&uhsConfig_, 0, sizeof(uhsConfig_));
            std::memset(uhsCfgBuf_, 0, 0x1400);
            uhsConfig_.controller_num = ctrl;
            uhsConfig_.buffer = uhsCfgBuf_;
            uhsConfig_.buffer_size = UHS_CONFIG_BUFFER_SIZE;
            DCFlushRange(uhsCfgBuf_, 0x1400);

            if (UhsClientOpen(&uhsHandle_, &uhsConfig_) != UHS_STATUS_OK) continue;
            uhsOpened_ = true;

            UhsInterfaceFilter filter{};
            filter.match_params = MATCH_ANY;
            std::memset(profilesBuf_, 0, sizeof(UhsInterfaceProfile) * kAoaMaxProfs);

            int32_t numIfs = static_cast<int32_t>(
                UhsQueryInterfaces(&uhsHandle_, &filter, profilesBuf_, kAoaMaxProfs));
            int32_t count = std::min<int32_t>(std::max<int32_t>(0, numIfs), kAoaMaxProfs);

            for (int32_t i = 0; i < count; ++i) {
                const UhsInterfaceProfile& prof = profilesBuf_[i];
                if (!IsAoaVidPid(prof.dev_desc.idVendor, prof.dev_desc.idProduct)) continue;
                // En PID 0x2D01 (AOA + ADB), la interfaz 0 es AOA (subclass 0xFF, protocol 0x00)
                if (prof.if_desc.bInterfaceNumber != 0) continue;

                uint8_t bulkInNum = 0;
                uint8_t bulkOutNum = 0;
                uint32_t epMask = 0;

                for (int e = 0; e < 16; ++e) {
                    const auto& ep = prof.in_endpoints[e];
                    if (ep.bLength >= 7 && (ep.bmAttributes & 0x03u) == 0x02u && (ep.bEndpointAddress & 0x80u) != 0) {
                        uint8_t epNum = static_cast<uint8_t>(ep.bEndpointAddress & 0x0Fu);
                        if (epNum != 0) {
                            bulkInNum = epNum;
                            epMask |= (1u << (epNum + 16));
                            break;
                        }
                    }
                }
                for (int e = 0; e < 16; ++e) {
                    const auto& ep = prof.out_endpoints[e];
                    if (ep.bLength >= 7 && (ep.bmAttributes & 0x03u) == 0x02u && (ep.bEndpointAddress & 0x80u) == 0) {
                        uint8_t epNum = static_cast<uint8_t>(ep.bEndpointAddress & 0x0Fu);
                        if (epNum != 0) {
                            bulkOutNum = epNum;
                            epMask |= (1u << epNum);
                            break;
                        }
                    }
                }

                if (bulkInNum == 0 || bulkOutNum == 0) continue;

                if (UhsAcquireInterface(&uhsHandle_, prof.if_handle, nullptr, OnInterfaceEvent) != UHS_STATUS_OK) {
                    continue;
                }
                ifAcquired_ = true;
                ifHandle_ = prof.if_handle;
                epIn_ = bulkInNum;
                epOut_ = bulkOutNum;

                UHSStatus epSt = UhsAdministerEndpoint(&uhsHandle_, ifHandle_, UHS_ADMIN_EP_ENABLE, epMask, 1, kAoaDmaSize);
                if (epSt != UHS_STATUS_OK) {
                    UhsAdministerEndpoint(&uhsHandle_, ifHandle_, UHS_ADMIN_EP_ENABLE, epMask, 1, 8 * 1024);
                }
                OSSleepTicks(OSMillisecondsToTicks(50));

                // Intentar PING (damos un par de segundos por si el usuario esta tocando Aceptar en Android)
                std::string model;
                bool pingOk = false;
                for (int attempt = 0; attempt < 4 && !pingOk; ++attempt) {
                    if (attempt > 0) OSSleepTicks(OSMillisecondsToTicks(500));
                    pingOk = PingDevice(model, 1500);
                }

                if (pingOk) {
                    connected_ = true;
                    deviceName_ = model.empty() ? "Android USB Directo" : model;
                    outDeviceName = deviceName_;
                    return true;
                }

                outDiag = "Toca 'Aceptar' en el aviso USB de tu celular y pulsa [Y].";
                Disconnect();
                return false;
            }

            UhsClientClose(&uhsHandle_);
            uhsOpened_ = false;
        }
        return false;
    }

    int32_t BulkOut(const void* data, uint32_t len, int32_t timeoutMs = 4000) {
        if (!uhsOpened_ || !ifAcquired_ || len > kAoaDmaSize) return -1;
        if (data != dmaBuf_) {
            std::memcpy(dmaBuf_, data, len);
        }
        DCFlushRange(dmaBuf_, len);
        return static_cast<int32_t>(
            UhsSubmitBulkRequest(&uhsHandle_, ifHandle_, epOut_, ENDPOINT_TRANSFER_OUT,
                                 dmaBuf_, static_cast<int32_t>(len), timeoutMs));
    }

    int32_t BulkIn(uint32_t askLen, int32_t timeoutMs = 6000) {
        if (!uhsOpened_ || !ifAcquired_) return -1;
        uint32_t aligned = (askLen + 511u) & ~511u;
        if (aligned < 512u) aligned = 512u;
        uint32_t ask = std::min<uint32_t>(aligned, kAoaDmaSize);
        DCInvalidateRange(dmaBuf_, ask);
        int32_t r = static_cast<int32_t>(
            UhsSubmitBulkRequest(&uhsHandle_, ifHandle_, epIn_, ENDPOINT_TRANSFER_IN,
                                 dmaBuf_, static_cast<int32_t>(ask), timeoutMs));
        if (r == 0) {
            // Saltar posible ZLP
            DCInvalidateRange(dmaBuf_, ask);
            r = static_cast<int32_t>(
                UhsSubmitBulkRequest(&uhsHandle_, ifHandle_, epIn_, ENDPOINT_TRANSFER_IN,
                                     dmaBuf_, static_cast<int32_t>(ask), timeoutMs));
        }
        if (r > 0) {
            DCInvalidateRange(dmaBuf_, static_cast<uint32_t>(r));
        }
        return r;
    }

    bool ExchangeCommand(uint32_t cmd,
                         const void* reqPayload,
                         uint32_t reqLen,
                         std::vector<uint8_t>& outResp,
                         uint32_t& outStatus,
                         int32_t timeoutMs = 5000) {
        outResp.clear();
        outStatus = 0xFFFFFFFFu;
        lastErr_.clear();

        if (12u + reqLen > kAoaDmaSize) return false;
        uint16_t mySeq = ++seq_;
        if (mySeq == 0) mySeq = ++seq_;

        Wr32LE(dmaBuf_ + 0, kMagicReq);
        Wr32LE(dmaBuf_ + 4, (static_cast<uint32_t>(mySeq) << 16) | (cmd & 0xFFFFu));
        Wr32LE(dmaBuf_ + 8, reqLen);
        if (reqLen > 0 && reqPayload != nullptr) {
            std::memcpy(dmaBuf_ + 12, reqPayload, reqLen);
        }

        int32_t sent = BulkOut(dmaBuf_, 12u + reqLen, timeoutMs);
        if (sent < static_cast<int32_t>(12u + reqLen)) {
            char b[64];
            std::snprintf(b, sizeof(b), "AOA OUT fallo (cmd=%u, rc=%d)", cmd, sent);
            lastErr_ = b;
            return false;
        }

        for (int tries = 0; tries < 4; ++tries) {
            int32_t n = BulkIn(kAoaDmaSize, timeoutMs);
            if (n < 12) {
                char b[64];
                std::snprintf(b, sizeof(b), "AOA IN fallo (cmd=%u, n=%d)", cmd, n);
                lastErr_ = b;
                return false;
            }

            uint32_t magic = usbfat::Rd32LE(dmaBuf_ + 0);
            uint32_t statusField = usbfat::Rd32LE(dmaBuf_ + 4);
            uint32_t payLen = usbfat::Rd32LE(dmaBuf_ + 8);
            if (magic != kMagicOk && magic != kMagicErr) {
                char b[64];
                std::snprintf(b, sizeof(b), "AOA magic invalido (0x%08X)", magic);
                lastErr_ = b;
                return false;
            }

            uint16_t respSeq = static_cast<uint16_t>((statusField >> 16) & 0xFFFFu);
            uint32_t status = statusField & 0xFFFFu;

            outResp.clear();
            if (payLen > 0) {
                outResp.reserve(payLen);
                uint32_t firstCopy = std::min<uint32_t>(static_cast<uint32_t>(n - 12), payLen);
                outResp.insert(outResp.end(), dmaBuf_ + 12, dmaBuf_ + 12 + firstCopy);

                while (outResp.size() < payLen) {
                    uint32_t rem = payLen - static_cast<uint32_t>(outResp.size());
                    int32_t rn = BulkIn(std::min<uint32_t>(((rem + 511u) & ~511u), kAoaDmaSize), timeoutMs);
                    if (rn <= 0) {
                        char b[64];
                        std::snprintf(b, sizeof(b), "AOA corte datos (%u/%u, rn=%d)",
                                      static_cast<uint32_t>(outResp.size()), payLen, rn);
                        lastErr_ = b;
                        return false;
                    }
                    uint32_t take = std::min<uint32_t>(static_cast<uint32_t>(rn), rem);
                    outResp.insert(outResp.end(), dmaBuf_, dmaBuf_ + take);
                }
            }

            // Si llego un paquete retrasado de un comando anterior, descartarlo y leer el nuestro
            if (respSeq != 0 && respSeq != mySeq) {
                continue;
            }

            outStatus = (magic == kMagicOk) ? status : (status == 0 ? 1u : status);
            return true;
        }
        lastErr_ = "AOA desincronizado";
        return false;
    }

    bool PingDevice(std::string& outModel, int32_t timeoutMs = 1500) {
        std::vector<uint8_t> resp;
        uint32_t status = 0;
        if (!ExchangeCommand(kCmdPing, nullptr, 0, resp, status, timeoutMs) || status != 0) {
            return false;
        }
        outModel.assign(reinterpret_cast<const char*>(resp.data()), resp.size());
        return true;
    }

    bool OpenRemoteFile(uint32_t fileId, uint64_t& outSize) {
        uint8_t idBuf[4]{};
        Wr32LE(idBuf, fileId);
        std::vector<uint8_t> resp;
        uint32_t status = 0;
        if (!ExchangeCommand(kCmdOpenFile, idBuf, 4, resp, status, 5000)) {
            return false;
        }
        if (status != 0 || resp.size() < 8) {
            if (!resp.empty()) {
                lastErr_.assign(reinterpret_cast<const char*>(resp.data()), resp.size());
            } else {
                char b[64];
                std::snprintf(b, sizeof(b), "AOA OPEN st=%u len=%u (id=%u)",
                              status, static_cast<uint32_t>(resp.size()), fileId);
                lastErr_ = b;
            }
            return false;
        }
        uint64_t lo = usbfat::Rd32LE(resp.data());
        uint64_t hi = usbfat::Rd32LE(resp.data() + 4);
        outSize = lo | (hi << 32);
        return true;
    }

    bool CloseRemoteFile() {
        std::vector<uint8_t> resp;
        uint32_t status = 0;
        return ExchangeCommand(kCmdCloseFile, nullptr, 0, resp, status, 2000) && status == 0;
    }

    bool ReadRemoteChunk(uint64_t offset, uint32_t want, uint8_t* dst, uint32_t& outGot) {
        outGot = 0;
        lastErr_.clear();
        if (want > kAoaChunkSize) want = kAoaChunkSize;

        uint16_t mySeq = ++seq_;
        if (mySeq == 0) mySeq = ++seq_;

        // Enviar CMD_READ_CHUNK (header 12 B + payload 12 B = 24 B)
        Wr32LE(dmaBuf_ + 0, kMagicReq);
        Wr32LE(dmaBuf_ + 4, (static_cast<uint32_t>(mySeq) << 16) | (kCmdReadChunk & 0xFFFFu));
        Wr32LE(dmaBuf_ + 8, 12u);
        Wr64LE(dmaBuf_ + 12, offset);
        Wr32LE(dmaBuf_ + 20, want);

        int32_t sent = BulkOut(dmaBuf_, 24, 5000);
        if (sent < 24) {
            char b[64];
            std::snprintf(b, sizeof(b), "AOA READ OUT fallo (%d)", sent);
            lastErr_ = b;
            return false;
        }

        for (int tries = 0; tries < 4; ++tries) {
            int32_t n = BulkIn(kAoaDmaSize, 8000);
            if (n < 12) {
                char b[64];
                std::snprintf(b, sizeof(b), "AOA READ IN fallo (%d)", n);
                lastErr_ = b;
                return false;
            }

            uint32_t magic = usbfat::Rd32LE(dmaBuf_ + 0);
            uint32_t statusField = usbfat::Rd32LE(dmaBuf_ + 4);
            uint32_t payLen = usbfat::Rd32LE(dmaBuf_ + 8);
            uint16_t respSeq = static_cast<uint16_t>((statusField >> 16) & 0xFFFFu);
            uint32_t status = statusField & 0xFFFFu;

            if (respSeq != 0 && respSeq != mySeq) {
                // Descartar paquete viejo completo si hubiera resto
                uint32_t drained = std::min<uint32_t>(static_cast<uint32_t>(n - 12), payLen);
                while (drained < payLen) {
                    int32_t rn = BulkIn(kAoaDmaSize, 2000);
                    if (rn <= 0) break;
                    drained += static_cast<uint32_t>(rn);
                }
                continue;
            }

            if (magic != kMagicOk || status != 0 || payLen > want) {
                char b[64];
                std::snprintf(b, sizeof(b), "AOA READ err (0x%08X, st=%u, len=%u)", magic, status, payLen);
                lastErr_ = b;
                return false;
            }

            uint32_t firstCopy = std::min<uint32_t>(static_cast<uint32_t>(n - 12), payLen);
            if (firstCopy > 0) {
                std::memcpy(dst, dmaBuf_ + 12, firstCopy);
            }
            outGot = firstCopy;

            while (outGot < payLen) {
                uint32_t rem = payLen - outGot;
                int32_t rn = BulkIn(std::min<uint32_t>(((rem + 511u) & ~511u), kAoaDmaSize), 8000);
                if (rn <= 0) {
                    char b[64];
                    std::snprintf(b, sizeof(b), "AOA READ corte (%u/%u, rn=%d)", outGot, payLen, rn);
                    lastErr_ = b;
                    return false;
                }
                uint32_t take = std::min<uint32_t>(static_cast<uint32_t>(rn), rem);
                std::memcpy(dst + outGot, dmaBuf_, take);
                outGot += take;
            }
            return true;
        }
        lastErr_ = "AOA READ desincronizado";
        return false;
    }

    UhsHandle uhsHandle_{};
    UhsConfig uhsConfig_{};
    uint8_t* uhsCfgBuf_ = nullptr;
    UhsInterfaceProfile* profilesBuf_ = nullptr;
    uint8_t* dmaBuf_ = nullptr;
    uint8_t* ctrlBuf_ = nullptr;

    bool uhsOpened_ = false;
    bool ifAcquired_ = false;
    bool connected_ = false;

    uint32_t ifHandle_ = 0;
    uint8_t epIn_ = 0;
    uint8_t epOut_ = 0;
    uint16_t seq_ = 1;
    std::string deviceName_;
    std::string lastErr_;
};

} // namespace usbaoa
