#pragma once

#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/time.h>
#include <nsysuhs/uhs.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "usb_fat_reader.hpp"

namespace usbmtp {

// Contenedores USB Still Image / MTP (PIMA 15740, Little-Endian en bus USB)
constexpr uint16_t kMtpContCommand  = 1;
constexpr uint16_t kMtpContData     = 2;
constexpr uint16_t kMtpContResponse = 3;

// Operaciones MTP / PTP
constexpr uint16_t kMtpOpGetDeviceInfo    = 0x1001;
constexpr uint16_t kMtpOpOpenSession      = 0x1002;
constexpr uint16_t kMtpOpCloseSession     = 0x1003;
constexpr uint16_t kMtpOpGetStorageIDs    = 0x1004;
constexpr uint16_t kMtpOpGetObjectHandles = 0x1007;
constexpr uint16_t kMtpOpGetObjectInfo    = 0x1008;
constexpr uint16_t kMtpOpGetObject        = 0x1009;
constexpr uint16_t kMtpOpGetPartialObject = 0x101B;

// Respuestas MTP / PTP
constexpr uint16_t kMtpRespOk                   = 0x2001;
constexpr uint16_t kMtpRespSessionAlreadyOpen   = 0x201E;
constexpr uint16_t kMtpRespOpNotSupported       = 0x2005;

constexpr uint16_t kMtpFormatAssociation = 0x3001; // Carpeta / Directorio

constexpr uint32_t kMtpDmaBufSize   = 64 * 1024;   // 64 KB por transaccion USB Bulk (limite seguro en IOS-USB/nsysuhs)
constexpr uint32_t kMtpPartialChunk = 60 * 1024;   // 60 KB de carga util por GetPartialObject
constexpr int32_t  kMtpMaxProfiles  = 32;          // Hasta 32 interfaces USB (soporta telefonos compuestos Motorola/Samsung/Xiaomi)

struct MtpFile {
    std::string subPath;
    uint32_t objectHandle = 0;
    uint64_t size = 0;
};

struct MtpPkg {
    std::string name;
    std::vector<MtpFile> files;
    uint64_t totalBytes = 0;
    bool marked = false;
};

inline void Wr16LE(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

inline void Wr32LE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

class UsbMtpDevice {
public:
    class FileStream {
    public:
        FileStream() = default;

        void Open(UsbMtpDevice* dev, uint32_t objectHandle, uint64_t size) {
            dev_ = dev;
            handle_ = objectHandle;
            size_ = size;
            offset_ = 0;
            remaining_ = size;
            fallbackBuf_.clear();
            fallbackPos_ = 0;
            usingFallback_ = false;
        }

        uint64_t Remaining() const { return remaining_; }

        bool ReadExact(void* dst, uint32_t len) {
            if (!dev_ || len > remaining_) return false;
            if (len == 0) return true;

            auto* out = static_cast<uint8_t*>(dst);
            uint32_t got = 0;

            while (got < len) {
                if (usingFallback_) {
                    size_t avail = (fallbackPos_ < fallbackBuf_.size())
                        ? (fallbackBuf_.size() - fallbackPos_)
                        : 0;
                    uint32_t take = static_cast<uint32_t>(std::min<size_t>(len - got, avail));
                    if (take == 0) return false;
                    std::memcpy(out + got, fallbackBuf_.data() + fallbackPos_, take);
                    fallbackPos_ += take;
                    offset_ += take;
                    remaining_ -= take;
                    got += take;
                    continue;
                }

                uint32_t want = std::min<uint32_t>(len - got, kMtpPartialChunk);
                // GetPartialObject (0x101B) usa offset de 32 bits (archivos WUP .app en Wii U son <= 2 GB)
                if (offset_ <= 0xFFFFFFFFULL && dev_->supportsPartial_) {
                    uint32_t chunkGot = 0;
                    bool readOk = false;
                    for (int attempt = 0; attempt < 3 && !readOk; ++attempt) {
                        if (attempt > 0) {
                            dev_->RecoverEndpoints();
                        }
                        if (dev_->ReadPartialObject(handle_, static_cast<uint32_t>(offset_), want, out + got, chunkGot) &&
                            chunkGot > 0) {
                            readOk = true;
                        }
                    }
                    if (readOk) {
                        offset_ += chunkGot;
                        remaining_ -= chunkGot;
                        got += chunkGot;
                        continue;
                    }
                    // Si el primer intento a offset 0 falla, probamos GetObject completo para archivos pequeños/medianos
                    if (offset_ == 0 && size_ <= 16ULL * 1024 * 1024) {
                        dev_->supportsPartial_ = false;
                    } else {
                        return false;
                    }
                }

                // Fallback GetObject (0x1009) para archivos <= 16 MB si el movil no soporta GetPartialObject
                if (offset_ == 0 && size_ <= 16ULL * 1024 * 1024) {
                    if (!dev_->ReadFullObject(handle_, static_cast<uint32_t>(size_), fallbackBuf_)) {
                        return false;
                    }
                    usingFallback_ = true;
                    fallbackPos_ = 0;
                    continue;
                }
                return false;
            }
            return true;
        }

    private:
        UsbMtpDevice* dev_ = nullptr;
        uint32_t handle_ = 0;
        uint64_t size_ = 0;
        uint64_t offset_ = 0;
        uint64_t remaining_ = 0;
        std::vector<uint8_t> fallbackBuf_;
        size_t fallbackPos_ = 0;
        bool usingFallback_ = false;
    };

    UsbMtpDevice() = default;
    ~UsbMtpDevice() { Shutdown(); }

    UsbMtpDevice(const UsbMtpDevice&) = delete;
    UsbMtpDevice& operator=(const UsbMtpDevice&) = delete;

    bool IsConnected() const { return connected_; }
    const std::string& DeviceName() const { return deviceName_; }

    void Disconnect() {
        if (connected_) {
            if (sessionOpen_) {
                uint32_t params[1]{};
                uint16_t respCode = 0;
                SendSimpleCommand(kMtpOpCloseSession, params, 0, respCode);
                sessionOpen_ = false;
            }
            if (ifAcquired_ && uhsOpened_) {
                UhsReleaseInterface(&uhsHandle_, ifHandle_, false);
                ifAcquired_ = false;
            }
            connected_ = false;
        }
        if (uhsOpened_) {
            UhsClientClose(&uhsHandle_);
            uhsOpened_ = false;
        }
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
        Disconnect();
        outDeviceName.clear();
        outDiag.clear();

        if (!EnsureBuffers()) {
            outDiag = "Sin memoria para driver USB del celular.";
            return false;
        }

        std::string lastSeenUsb;

        // Escanear los 3 controladores USB host de la Wii U (0=EHCI High-Speed, 1 y 2=OHCI)
        for (uint32_t ctrl = 0; ctrl < 3; ++ctrl) {
            std::memset(&uhsHandle_, 0, sizeof(uhsHandle_));
            std::memset(&uhsConfig_, 0, sizeof(uhsConfig_));
            std::memset(uhsCfgBuf_, 0, 0x1400);
            uhsConfig_.controller_num = ctrl;
            uhsConfig_.buffer = uhsCfgBuf_;
            uhsConfig_.buffer_size = UHS_CONFIG_BUFFER_SIZE;
            DCFlushRange(uhsCfgBuf_, 0x1400);

            if (UhsClientOpen(&uhsHandle_, &uhsConfig_) != UHS_STATUS_OK) {
                continue;
            }
            uhsOpened_ = true;

            UhsInterfaceFilter filter{};
            filter.match_params = MATCH_ANY;
            std::memset(profilesBuf_, 0, sizeof(UhsInterfaceProfile) * kMtpMaxProfiles);

            int32_t numIfs = static_cast<int32_t>(
                UhsQueryInterfaces(&uhsHandle_, &filter, profilesBuf_, kMtpMaxProfiles));
            if (numIfs <= 0) {
                UhsClientClose(&uhsHandle_);
                uhsOpened_ = false;
                continue;
            }

            int32_t count = std::min<int32_t>(numIfs, kMtpMaxProfiles);
            for (int32_t i = 0; i < count; ++i) {
                const UhsInterfaceProfile& prof = profilesBuf_[i];
                uint8_t cls = prof.if_desc.bInterfaceClass;

                // Evitar explicitamente discos/memorias USB (0x08), hubs (0x09), mandos HID (0x03), audio (0x01), video (0x0E)
                if (cls == USBCLASS_STORAGE || cls == 0x09 || cls == USBCLASS_HID ||
                    cls == USBCLASS_AUDIO || cls == 0x0E || cls == 0x07 || cls == 0xE0) {
                    continue;
                }

                // MTP en Android se anuncia como Still Image (0x06) o Vendor Specific (0xFF, ej. Motorola/Samsung/Qualcomm)
                bool isMtpCandidate = (cls == 0x06 || cls == 0xFF || cls == 0x00);
                if (!isMtpCandidate) continue;

                uint8_t bulkInNum = 0;
                uint8_t bulkOutNum = 0;
                uint32_t epMask = 0;

                for (int e = 0; e < 16; ++e) {
                    const auto& ep = prof.in_endpoints[e];
                    if (ep.bLength >= 7 && (ep.bmAttributes & 0x03u) == 0x02u && (ep.bEndpointAddress & 0x80u) != 0) {
                        uint8_t epNum = static_cast<uint8_t>(ep.bEndpointAddress & 0x0Fu);
                        if (epNum != 0) {
                            bulkInNum = epNum; // nsysuhs espera el numero de endpoint (1..15), NO 0x81!
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

                char seenBuf[48];
                std::snprintf(seenBuf, sizeof(seenBuf), "%04X:%04X (cls %02X)",
                              prof.dev_desc.idVendor, prof.dev_desc.idProduct, cls);
                lastSeenUsb = seenBuf;

                if (UhsAcquireInterface(&uhsHandle_, prof.if_handle, nullptr, OnInterfaceEvent) != UHS_STATUS_OK) {
                    continue;
                }
                ifAcquired_ = true;
                ifHandle_ = prof.if_handle;
                epIn_ = bulkInNum;
                epOut_ = bulkOutNum;

                // Habilitar endpoints con tamano seguro en IOS-USB (64 KB, fallback 16 KB)
                UHSStatus epSt = UhsAdministerEndpoint(&uhsHandle_, ifHandle_, UHS_ADMIN_EP_ENABLE, epMask, 1, kMtpDmaBufSize);
                if (epSt != UHS_STATUS_OK) {
                    epSt = UhsAdministerEndpoint(&uhsHandle_, ifHandle_, UHS_ADMIN_EP_ENABLE, epMask, 1, 16 * 1024);
                }
                UhsClearEndpointHalt(&uhsHandle_, ifHandle_, epIn_, ENDPOINT_TRANSFER_IN);
                UhsClearEndpointHalt(&uhsHandle_, ifHandle_, epOut_, ENDPOINT_TRANSFER_OUT);

                txId_ = 1;
                sessionOpen_ = false;
                supportsPartial_ = true;

                uint32_t sessParams[1] = {1u};
                uint16_t respCode = 0;
                bool openOk = SendSimpleCommand(kMtpOpOpenSession, sessParams, 1, respCode) &&
                              (respCode == kMtpRespOk || respCode == kMtpRespSessionAlreadyOpen);
                if (!openOk) {
                    // Si habia una sesion colgada en el telefono, cerrarla y reintentar una vez
                    SendSimpleCommand(kMtpOpCloseSession, nullptr, 0, respCode);
                    txId_ = 1;
                    openOk = SendSimpleCommand(kMtpOpOpenSession, sessParams, 1, respCode) &&
                             (respCode == kMtpRespOk || respCode == kMtpRespSessionAlreadyOpen);
                }
                if (!openOk) {
                    UhsReleaseInterface(&uhsHandle_, ifHandle_, false);
                    ifAcquired_ = false;
                    continue;
                }
                sessionOpen_ = true;

                char vidPid[40];
                std::snprintf(vidPid, sizeof(vidPid), "Android USB (%04X:%04X)",
                              prof.dev_desc.idVendor, prof.dev_desc.idProduct);
                deviceName_ = vidPid;

                std::string friendlyModel;
                if (QueryDeviceModel(friendlyModel) && !friendlyModel.empty()) {
                    deviceName_ = friendlyModel;
                }

                connected_ = true;
                outDeviceName = deviceName_;
                return true;
            }

            UhsClientClose(&uhsHandle_);
            uhsOpened_ = false;
        }

        if (!lastSeenUsb.empty()) {
            outDiag = "USB " + lastSeenUsb + " detectado: desbloquea y elige 'Transferencia de archivos'.";
        } else {
            outDiag = "Sin celular USB. Activa 'Transferencia de archivos' en Android.";
        }
        return false;
    }

    void RecoverEndpoints() {
        if (!uhsOpened_ || !ifAcquired_) return;
        UhsClearEndpointHalt(&uhsHandle_, ifHandle_, epIn_, ENDPOINT_TRANSFER_IN);
        UhsClearEndpointHalt(&uhsHandle_, ifHandle_, epOut_, ENDPOINT_TRANSFER_OUT);
        OSSleepTicks(OSMillisecondsToTicks(25));
    }

    bool ScanWupPackages(std::vector<MtpPkg>& outPkgs, std::string& outDiag) {
        outPkgs.clear();
        outDiag.clear();
        if (!connected_ || !sessionOpen_) {
            outDiag = "Sesion USB del celular no activa.";
            return false;
        }

        std::vector<uint32_t> storageIds;
        if (!GetStorageIds(storageIds) || storageIds.empty()) {
            outDiag = "Celular bloqueado: desbloquea la pantalla y elige 'Transferencia de archivos'.";
            return false;
        }

        // Leer manifiesto automatico generado por la App Android (WiiU_Transfer/wiiu_queue.txt)
        // para conocer la carpeta elegida por el usuario (ej. /Download o cualquier directorio)
        // y los juegos ya seleccionados en el celular.
        std::string customFolderRel;
        std::vector<std::string> preMarkedNames;
        bool hasQueueManifest = false;

        for (uint32_t sid : storageIds) {
            std::vector<MtpObjMeta> rootObjs;
            if (!ListDirectoryObjects(sid, 0xFFFFFFFFu, rootObjs)) continue;

            // Buscar WiiU_Transfer/wiiu_queue.txt
            for (const auto& ro : rootObjs) {
                if (ro.isDir && usbfat::EqualsCI(ro.name, "WiiU_Transfer")) {
                    std::vector<MtpObjMeta> wtObjs;
                    if (ListDirectoryObjects(sid, ro.handle, wtObjs)) {
                        for (const auto& f : wtObjs) {
                            if (!f.isDir && usbfat::EqualsCI(f.name, "wiiu_queue.txt") &&
                                f.size > 0 && f.size <= 64 * 1024) {
                                std::vector<uint8_t> qData;
                                if (ReadFullObject(f.handle, static_cast<uint32_t>(f.size), qData)) {
                                    ParseQueueManifest(qData, customFolderRel, preMarkedNames);
                                    hasQueueManifest = true;
                                }
                                break;
                            }
                        }
                    }
                    break;
                }
            }

            // 1. Si la App Android indico una carpeta especifica (ej. Download, Descargas o cualquier ruta),
            //    escanearla primero con prioridad.
            if (hasQueueManifest && !customFolderRel.empty()) {
                uint32_t targetDirHandle = ResolveFolderByPath(sid, rootObjs, customFolderRel);
                if (targetDirHandle != 0) {
                    std::vector<MtpObjMeta> customObjs;
                    if (ListDirectoryObjects(sid, targetDirHandle, customObjs)) {
                        ScanCandidateContainer(sid, customObjs, outPkgs);
                    }
                }
            }

            // 2. Escanear carpetas habituales en Android (/Download, /Descargas, /WiiU_Transfer, /WUP, /install, /packages)
            for (const auto& ro : rootObjs) {
                if (!ro.isDir) continue;
                if (usbfat::EqualsCI(ro.name, "WiiU_Transfer") ||
                    usbfat::EqualsCI(ro.name, "Download") ||
                    usbfat::EqualsCI(ro.name, "Downloads") ||
                    usbfat::EqualsCI(ro.name, "Descargas") ||
                    usbfat::EqualsCI(ro.name, "WUP") ||
                    usbfat::EqualsCI(ro.name, "install") ||
                    usbfat::EqualsCI(ro.name, "packages")) {
                    std::vector<MtpObjMeta> subObjs;
                    if (ListDirectoryObjects(sid, ro.handle, subObjs)) {
                        ScanCandidateContainer(sid, subObjs, outPkgs);
                        for (const auto& so : subObjs) {
                            if (so.isDir && (usbfat::EqualsCI(so.name, "WiiU_Transfer") ||
                                             usbfat::EqualsCI(so.name, "WUP") ||
                                             usbfat::EqualsCI(so.name, "install") ||
                                             usbfat::EqualsCI(so.name, "packages"))) {
                                std::vector<MtpObjMeta> deepObjs;
                                if (ListDirectoryObjects(sid, so.handle, deepObjs)) {
                                    ScanCandidateContainer(sid, deepObjs, outPkgs);
                                }
                            }
                        }
                    }
                }
            }

            // 3. Escanear cualquier carpeta de juegos directamente en la raiz del almacenamiento
            ScanCandidateContainer(sid, rootObjs, outPkgs);
        }

        // Aplicar seleccion automatica proveniente de la App Android
        if (!preMarkedNames.empty()) {
            for (auto& pkg : outPkgs) {
                for (const auto& mn : preMarkedNames) {
                    if (usbfat::EqualsCI(pkg.name, mn.c_str())) {
                        pkg.marked = true;
                        break;
                    }
                }
            }
        }

        if (outPkgs.empty()) {
            outDiag = "No se encontraron juegos en el celular (revisa /Download o tu carpeta elegida).";
        }
        return true;
    }

private:
    struct MtpObjMeta {
        uint32_t handle = 0;
        std::string name;
        bool isDir = false;
        uint64_t size = 0;
    };

    static void OnInterfaceEvent(void* /*context*/, int32_t /*arg1*/, int32_t /*arg2*/) {}

    bool EnsureBuffers() {
        if (!dmaBuf_) {
            dmaBuf_ = static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(kMtpDmaBufSize, 0x40));
        }
        if (!uhsCfgBuf_) {
            uhsCfgBuf_ = static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(0x1400, 0x40));
        }
        if (!profilesBuf_) {
            profilesBuf_ = static_cast<UhsInterfaceProfile*>(
                MEMAllocFromDefaultHeapEx(sizeof(UhsInterfaceProfile) * kMtpMaxProfiles, 0x40));
        }
        return dmaBuf_ && uhsCfgBuf_ && profilesBuf_;
    }

    int32_t BulkOut(const void* data, uint32_t len, int32_t timeoutMs = 5000) {
        if (!uhsOpened_ || !ifAcquired_ || len > kMtpDmaBufSize) return -1;
        if (data != dmaBuf_) {
            std::memcpy(dmaBuf_, data, len);
        }
        DCFlushRange(dmaBuf_, len);
        return static_cast<int32_t>(
            UhsSubmitBulkRequest(&uhsHandle_, ifHandle_, epOut_, ENDPOINT_TRANSFER_OUT,
                                 dmaBuf_, static_cast<int32_t>(len), timeoutMs));
    }

    int32_t BulkIn(void* dst, uint32_t maxLen, int32_t timeoutMs = 10000) {
        if (!uhsOpened_ || !ifAcquired_) return -1;
        // En USB 2.0 High-Speed (EHCI/IOS-USB), las lecturas Bulk IN DEBEN ser multiplo de 512 B
        // para evitar errores de Babble/Short-Transfer Overflow cuando el celular envia paquetes de 512 B
        uint32_t aligned = (maxLen + 511u) & ~511u;
        if (aligned < 512u) aligned = 512u;
        uint32_t ask = std::min<uint32_t>(aligned, kMtpDmaBufSize);
        DCInvalidateRange(dmaBuf_, ask);
        int32_t r = static_cast<int32_t>(
            UhsSubmitBulkRequest(&uhsHandle_, ifHandle_, epIn_, ENDPOINT_TRANSFER_IN,
                                 dmaBuf_, static_cast<int32_t>(ask), timeoutMs));
        if (r > 0) {
            DCInvalidateRange(dmaBuf_, static_cast<uint32_t>(r));
            if (dst != dmaBuf_) {
                uint32_t copyLen = std::min<uint32_t>(static_cast<uint32_t>(r), maxLen);
                std::memcpy(dst, dmaBuf_, static_cast<size_t>(copyLen));
            }
        }
        return r;
    }

    int32_t BulkInSkipZlp(uint32_t askLen, int32_t timeoutMs = 10000) {
        int32_t r = BulkIn(dmaBuf_, askLen, timeoutMs);
        if (r == 0) {
            // Saltar Zero-Length Packet (ZLP) enviado por Android tras multiplos exactos de 512 B
            r = BulkIn(dmaBuf_, askLen, timeoutMs);
        }
        return r;
    }

    bool SendSimpleCommand(uint16_t opCode,
                           const uint32_t* params,
                           uint32_t numParams,
                           uint16_t& outRespCode) {
        std::vector<uint8_t> dummy;
        return TransactionIn(opCode, params, numParams, dummy, outRespCode);
    }

    bool TransactionIn(uint16_t opCode,
                       const uint32_t* params,
                       uint32_t numParams,
                       std::vector<uint8_t>& outPayload,
                       uint16_t& outRespCode) {
        outPayload.clear();
        outRespCode = 0;

        uint32_t curTx = txId_++;
        uint32_t cmdLen = 12u + numParams * 4u;
        uint8_t cmdBuf[32]{};
        Wr32LE(cmdBuf + 0, cmdLen);
        Wr16LE(cmdBuf + 4, kMtpContCommand);
        Wr16LE(cmdBuf + 6, opCode);
        Wr32LE(cmdBuf + 8, curTx);
        for (uint32_t i = 0; i < numParams; ++i) {
            Wr32LE(cmdBuf + 12 + i * 4, params[i]);
        }

        if (BulkOut(cmdBuf, cmdLen) < static_cast<int32_t>(cmdLen)) {
            return false;
        }

        // Leer siguiente contenedor (puede ser DATA o directamente RESPONSE)
        int32_t n = BulkInSkipZlp(kMtpDmaBufSize);
        if (n < 12) return false;

        uint32_t contLen  = usbfat::Rd32LE(dmaBuf_ + 0);
        uint16_t contType = usbfat::Rd16LE(dmaBuf_ + 4);
        uint16_t contCode = usbfat::Rd16LE(dmaBuf_ + 6);

        if (contType == kMtpContResponse) {
            outRespCode = contCode;
            return true;
        }
        if (contType != kMtpContData) {
            return false;
        }

        uint32_t totalDataBytes = (contLen > 12u && contLen != 0xFFFFFFFFu) ? (contLen - 12u) : 0u;
        uint32_t firstPayload = static_cast<uint32_t>(n - 12);

        uint32_t takeFirst = (totalDataBytes > 0) ? std::min<uint32_t>(firstPayload, totalDataBytes) : firstPayload;
        outPayload.insert(outPayload.end(), dmaBuf_ + 12, dmaBuf_ + 12 + takeFirst);

        // Si el contenedor RESPONSE vino pegado al final del mismo paquete Bulk IN
        if (totalDataBytes > 0 && firstPayload >= totalDataBytes + 12u) {
            const uint8_t* respPtr = dmaBuf_ + 12 + totalDataBytes;
            if (usbfat::Rd16LE(respPtr + 4) == kMtpContResponse) {
                outRespCode = usbfat::Rd16LE(respPtr + 6);
                return true;
            }
        }

        bool gotConcatResp = false;
        while (totalDataBytes > 0 && outPayload.size() < totalDataBytes) {
            uint32_t rem = totalDataBytes - static_cast<uint32_t>(outPayload.size());
            int32_t rn = BulkIn(dmaBuf_, std::min<uint32_t>(((rem + 511u) & ~511u), kMtpDmaBufSize));
            if (rn <= 0) return false;
            uint32_t takeNow = std::min<uint32_t>(static_cast<uint32_t>(rn), rem);
            outPayload.insert(outPayload.end(), dmaBuf_, dmaBuf_ + takeNow);
            if (static_cast<uint32_t>(rn) >= takeNow + 12u &&
                usbfat::Rd16LE(dmaBuf_ + takeNow + 4) == kMtpContResponse) {
                outRespCode = usbfat::Rd16LE(dmaBuf_ + takeNow + 6);
                gotConcatResp = true;
                break;
            }
        }

        if (gotConcatResp) return true;

        // Leer contenedor RESPONSE (saltando posible ZLP)
        int32_t respN = BulkInSkipZlp(512);
        if (respN < 12) return false;
        if (usbfat::Rd16LE(dmaBuf_ + 4) != kMtpContResponse) return false;
        outRespCode = usbfat::Rd16LE(dmaBuf_ + 6);
        return true;
    }

    bool ReadPartialObject(uint32_t objHandle,
                           uint32_t offset,
                           uint32_t maxBytes,
                           uint8_t* dst,
                           uint32_t& outGot) {
        outGot = 0;
        uint32_t params[3] = {objHandle, offset, maxBytes};
        uint32_t curTx = txId_++;
        uint8_t cmdBuf[24]{};
        Wr32LE(cmdBuf + 0, 24);
        Wr16LE(cmdBuf + 4, kMtpContCommand);
        Wr16LE(cmdBuf + 6, kMtpOpGetPartialObject);
        Wr32LE(cmdBuf + 8, curTx);
        Wr32LE(cmdBuf + 12, params[0]);
        Wr32LE(cmdBuf + 16, params[1]);
        Wr32LE(cmdBuf + 20, params[2]);

        if (BulkOut(cmdBuf, 24) < 24) return false;

        int32_t n = BulkInSkipZlp(kMtpDmaBufSize);
        if (n < 12) return false;

        uint32_t contLen  = usbfat::Rd32LE(dmaBuf_ + 0);
        uint16_t contType = usbfat::Rd16LE(dmaBuf_ + 4);
        uint16_t contCode = usbfat::Rd16LE(dmaBuf_ + 6);

        if (contType == kMtpContResponse) {
            return contCode == kMtpRespOk;
        }
        if (contType != kMtpContData || contLen < 12) return false;

        uint32_t expected = std::min<uint32_t>(contLen - 12u, maxBytes);
        uint32_t firstPayload = static_cast<uint32_t>(n - 12);
        uint32_t firstCopy = std::min<uint32_t>(firstPayload, expected);
        std::memcpy(dst, dmaBuf_ + 12, firstCopy);
        outGot = firstCopy;

        if (firstPayload >= expected + 12u &&
            usbfat::Rd16LE(dmaBuf_ + 12 + expected + 4) == kMtpContResponse) {
            return usbfat::Rd16LE(dmaBuf_ + 12 + expected + 6) == kMtpRespOk;
        }

        bool gotConcatResp = false;
        uint16_t concatRespCode = 0;
        while (outGot < expected) {
            uint32_t rem = expected - outGot;
            int32_t rn = BulkIn(dmaBuf_, std::min<uint32_t>(((rem + 511u) & ~511u), kMtpDmaBufSize));
            if (rn <= 0) return false;
            uint32_t copyNow = std::min<uint32_t>(static_cast<uint32_t>(rn), rem);
            std::memcpy(dst + outGot, dmaBuf_, copyNow);
            outGot += copyNow;
            if (static_cast<uint32_t>(rn) >= copyNow + 12u &&
                usbfat::Rd16LE(dmaBuf_ + copyNow + 4) == kMtpContResponse) {
                concatRespCode = usbfat::Rd16LE(dmaBuf_ + copyNow + 6);
                gotConcatResp = true;
                break;
            }
        }

        if (gotConcatResp) return concatRespCode == kMtpRespOk;

        // Recibir RESPONSE final (saltando posible ZLP si contLen fue multiplo exacto de 512 B)
        int32_t respN = BulkInSkipZlp(512);
        if (respN < 12) return false;
        uint16_t rType = usbfat::Rd16LE(dmaBuf_ + 4);
        uint16_t rCode = usbfat::Rd16LE(dmaBuf_ + 6);
        return (rType == kMtpContResponse && rCode == kMtpRespOk);
    }

    bool ReadFullObject(uint32_t objHandle, uint32_t expectedSize, std::vector<uint8_t>& outData) {
        uint32_t params[1] = {objHandle};
        uint16_t respCode = 0;
        outData.reserve(expectedSize);
        if (!TransactionIn(kMtpOpGetObject, params, 1, outData, respCode)) return false;
        return respCode == kMtpRespOk;
    }

    static std::string ParseMtpString(const uint8_t* data, size_t maxLen, size_t& ioOff) {
        if (ioOff >= maxLen) return {};
        uint8_t numChars = data[ioOff++];
        if (numChars == 0) return {};
        std::string out;
        out.reserve(numChars);
        for (uint8_t i = 0; i < numChars && (ioOff + 1) < maxLen; ++i) {
            uint16_t ch = usbfat::Rd16LE(data + ioOff);
            ioOff += 2;
            if (ch == 0) break;
            out.push_back(usbfat::FoldUtf16ToAscii(ch));
        }
        return out;
    }

    bool QueryDeviceModel(std::string& outModel) {
        outModel.clear();
        std::vector<uint8_t> payload;
        uint16_t resp = 0;
        if (!TransactionIn(kMtpOpGetDeviceInfo, nullptr, 0, payload, resp) || resp != kMtpRespOk) {
            return false;
        }
        // DeviceInfo dataset: saltar StandardVersion(2), VendorExtID(4), VendorExtVer(2), VendorExtDesc(str), FuncMode(2),
        // OperationsSupported(au16), EventsSupported(au16), DevicePropsSupported(au16), CaptureFormats(au16), ImageFormats(au16)
        size_t off = 8;
        if (off >= payload.size()) return false;
        (void)ParseMtpString(payload.data(), payload.size(), off);
        off += 2; // FunctionalMode
        for (int a = 0; a < 5; ++a) {
            if (off + 4 > payload.size()) return false;
            uint32_t cnt = usbfat::Rd32LE(payload.data() + off);
            off += 4 + static_cast<size_t>(cnt) * 2;
        }
        if (off >= payload.size()) return false;
        std::string manufacturer = ParseMtpString(payload.data(), payload.size(), off);
        std::string model = ParseMtpString(payload.data(), payload.size(), off);
        if (!model.empty()) {
            outModel = model;
            return true;
        }
        if (!manufacturer.empty()) {
            outModel = manufacturer;
            return true;
        }
        return false;
    }

    bool GetStorageIds(std::vector<uint32_t>& outIds) {
        outIds.clear();
        std::vector<uint8_t> payload;
        uint16_t resp = 0;
        if (!TransactionIn(kMtpOpGetStorageIDs, nullptr, 0, payload, resp) || resp != kMtpRespOk) {
            return false;
        }
        if (payload.size() < 4) return false;
        uint32_t count = std::min<uint32_t>(usbfat::Rd32LE(payload.data()), 16);
        for (uint32_t i = 0; i < count && (4 + (i + 1) * 4) <= payload.size(); ++i) {
            uint32_t sid = usbfat::Rd32LE(payload.data() + 4 + i * 4);
            if (sid != 0) outIds.push_back(sid);
        }
        return !outIds.empty();
    }

    bool GetObjectMeta(uint32_t handle, MtpObjMeta& outMeta) {
        uint32_t params[1] = {handle};
        std::vector<uint8_t> payload;
        uint16_t resp = 0;
        if (!TransactionIn(kMtpOpGetObjectInfo, params, 1, payload, resp) ||
            resp != kMtpRespOk || payload.size() < 52) {
            return false;
        }
        uint16_t objFormat = usbfat::Rd16LE(payload.data() + 4);
        uint32_t compSize  = usbfat::Rd32LE(payload.data() + 8);
        size_t strOff = 52;
        std::string fname = ParseMtpString(payload.data(), payload.size(), strOff);
        if (fname.empty()) return false;

        outMeta.handle = handle;
        outMeta.name = std::move(fname);
        outMeta.isDir = (objFormat == kMtpFormatAssociation);
        outMeta.size = outMeta.isDir ? 0ULL : static_cast<uint64_t>(compSize);
        return true;
    }

    bool ListDirectoryObjects(uint32_t storageId, uint32_t parentHandle, std::vector<MtpObjMeta>& outList) {
        outList.clear();
        uint32_t params[3] = {storageId, 0u, parentHandle};
        std::vector<uint8_t> payload;
        uint16_t resp = 0;
        if (!TransactionIn(kMtpOpGetObjectHandles, params, 3, payload, resp) ||
            resp != kMtpRespOk || payload.size() < 4) {
            return false;
        }
        uint32_t count = std::min<uint32_t>(usbfat::Rd32LE(payload.data()), 512);
        for (uint32_t i = 0; i < count && (4 + (i + 1) * 4) <= payload.size(); ++i) {
            uint32_t h = usbfat::Rd32LE(payload.data() + 4 + i * 4);
            MtpObjMeta meta{};
            if (GetObjectMeta(h, meta)) {
                outList.push_back(std::move(meta));
            }
        }
        return true;
    }

    void CollectWupFilesRecursive(uint32_t storageId,
                                  uint32_t dirHandle,
                                  const std::string& subRel,
                                  int depth,
                                  std::vector<MtpFile>& outFiles,
                                  uint64_t& outTotalBytes,
                                  bool& outLooksLikeWup) {
        if (depth > 3) return;
        std::vector<MtpObjMeta> entries;
        if (!ListDirectoryObjects(storageId, dirHandle, entries)) return;

        for (const auto& e : entries) {
            if (e.name.empty() || e.name[0] == '.') continue;
            std::string rel = subRel.empty() ? e.name : (subRel + "/" + e.name);
            if (e.isDir) {
                CollectWupFilesRecursive(storageId, e.handle, rel, depth + 1,
                                         outFiles, outTotalBytes, outLooksLikeWup);
            } else {
                if (usbfat::EndsWithCI(e.name, ".tmd") ||
                    usbfat::EndsWithCI(e.name, ".tik") ||
                    usbfat::EndsWithCI(e.name, ".app")) {
                    outLooksLikeWup = true;
                }
                MtpFile mf;
                mf.subPath = std::move(rel);
                mf.objectHandle = e.handle;
                mf.size = e.size;
                outTotalBytes += mf.size;
                outFiles.push_back(std::move(mf));
            }
        }
    }

    static void ParseQueueManifest(const std::vector<uint8_t>& data,
                                   std::string& outFolderRel,
                                   std::vector<std::string>& outGames) {
        std::string text(reinterpret_cast<const char*>(data.data()), data.size());
        size_t pos = 0;
        while (pos < text.size()) {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(pos, nl - pos);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
                line.pop_back();
            }
            if (line.rfind("FOLDER=", 0) == 0) {
                std::string f = line.substr(7);
                while (!f.empty() && (f.front() == '/' || f.front() == '\\')) f.erase(f.begin());
                while (!f.empty() && (f.back() == '/' || f.back() == '\\')) f.pop_back();
                outFolderRel = f;
            } else if (line.rfind("GAME=", 0) == 0) {
                std::string g = line.substr(5);
                if (!g.empty()) outGames.push_back(g);
            }
            pos = nl + 1;
        }
    }

    uint32_t ResolveFolderByPath(uint32_t storageId,
                                 const std::vector<MtpObjMeta>& rootObjs,
                                 const std::string& relPath) {
        if (relPath.empty()) return 0xFFFFFFFFu;
        std::vector<MtpObjMeta> curObjs = rootObjs;
        uint32_t curHandle = 0;
        size_t pos = 0;
        while (pos < relPath.size()) {
            size_t slash = relPath.find('/', pos);
            if (slash == std::string::npos) slash = relPath.size();
            std::string seg = relPath.substr(pos, slash - pos);
            pos = slash + 1;
            if (seg.empty()) continue;

            uint32_t found = 0;
            for (const auto& o : curObjs) {
                if (o.isDir && usbfat::EqualsCI(o.name, seg.c_str())) {
                    found = o.handle;
                    break;
                }
            }
            if (found == 0) return 0;
            curHandle = found;
            if (pos < relPath.size()) {
                std::vector<MtpObjMeta> nextObjs;
                if (!ListDirectoryObjects(storageId, curHandle, nextObjs)) return 0;
                curObjs = std::move(nextObjs);
            }
        }
        return curHandle;
    }

    void ScanCandidateContainer(uint32_t storageId,
                                const std::vector<MtpObjMeta>& entries,
                                std::vector<MtpPkg>& outPkgs) {
        for (const auto& e : entries) {
            if (!e.isDir || e.name.empty() || e.name[0] == '.') continue;
            if (usbfat::EqualsCI(e.name, "Android") ||
                usbfat::EqualsCI(e.name, "DCIM") ||
                usbfat::EqualsCI(e.name, "Pictures") ||
                usbfat::EqualsCI(e.name, "Music") ||
                usbfat::EqualsCI(e.name, "Movies") ||
                usbfat::EqualsCI(e.name, "Notifications") ||
                usbfat::EqualsCI(e.name, "Ringtones") ||
                usbfat::EqualsCI(e.name, "Alarms") ||
                usbfat::EqualsCI(e.name, "Audiobooks") ||
                usbfat::EqualsCI(e.name, "Podcasts") ||
                usbfat::EqualsCI(e.name, "Documents") ||
                usbfat::EqualsCI(e.name, "WiiU_Transfer") ||
                usbfat::EqualsCI(e.name, "WUP") ||
                usbfat::EqualsCI(e.name, "install") ||
                usbfat::EqualsCI(e.name, "packages") ||
                usbfat::EqualsCI(e.name, "Download") ||
                usbfat::EqualsCI(e.name, "Downloads") ||
                usbfat::EqualsCI(e.name, "Descargas")) {
                continue;
            }
            bool alreadyAdded = false;
            for (const auto& ex : outPkgs) {
                if (usbfat::EqualsCI(ex.name, e.name.c_str())) {
                    alreadyAdded = true;
                    break;
                }
            }
            if (alreadyAdded) continue;

            MtpPkg pkg;
            pkg.name = e.name;
            bool looksLikeWup = false;
            CollectWupFilesRecursive(storageId, e.handle, "", 0, pkg.files, pkg.totalBytes, looksLikeWup);
            if (looksLikeWup && !pkg.files.empty()) {
                outPkgs.push_back(std::move(pkg));
            }
        }
    }

    UhsHandle uhsHandle_{};
    UhsConfig uhsConfig_{};
    uint8_t* uhsCfgBuf_ = nullptr;
    UhsInterfaceProfile* profilesBuf_ = nullptr;
    uint8_t* dmaBuf_ = nullptr;

    bool uhsOpened_ = false;
    bool ifAcquired_ = false;
    bool connected_ = false;
    bool sessionOpen_ = false;
    bool supportsPartial_ = true;

    uint32_t ifHandle_ = 0;
    uint8_t epIn_ = 0;
    uint8_t epOut_ = 0;
    uint32_t txId_ = 1;
    std::string deviceName_;
};

} // namespace usbmtp
