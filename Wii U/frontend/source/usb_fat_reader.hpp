#pragma once

#include <coreinit/cache.h>
#include <coreinit/filesystem_fsa.h>
#include <coreinit/ios.h>
#include <coreinit/memdefaultheap.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace usbfat {

constexpr uint32_t kSectorSize      = 512;
constexpr uint32_t kMaxRawSectors   = 2048; // 1 MB por IOS_Ioctlv (maximiza throughput de lectura USB)
constexpr uint32_t kFatCacheSectors = 1024; // 512 KB de cache FAT en MEM2

inline uint16_t Rd16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t Rd32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t Rd64LE(const uint8_t* p) {
    return static_cast<uint64_t>(Rd32LE(p)) |
           (static_cast<uint64_t>(Rd32LE(p + 4)) << 32);
}

inline bool EqualsCI(const std::string& a, const char* b) {
    size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

inline bool EndsWithCI(const std::string& s, const char* suffix) {
    size_t slen = std::strlen(suffix);
    if (s.size() < slen) return false;
    size_t off = s.size() - slen;
    for (size_t i = 0; i < slen; ++i) {
        if (std::tolower(static_cast<unsigned char>(s[off + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i]))) {
            return false;
        }
    }
    return true;
}

inline char FoldUtf16ToAscii(uint16_t ch) {
    if (ch >= 0x20 && ch < 0x7F) return static_cast<char>(ch);
    switch (ch) {
        case 0x00E1: case 0x00E0: case 0x00E4: case 0x00E2: return 'a';
        case 0x00E9: case 0x00E8: case 0x00EB: case 0x00EA: return 'e';
        case 0x00ED: case 0x00EC: case 0x00EF: case 0x00EE: return 'i';
        case 0x00F3: case 0x00F2: case 0x00F6: case 0x00F4: return 'o';
        case 0x00FA: case 0x00F9: case 0x00FC: case 0x00FB: return 'u';
        case 0x00F1: return 'n';
        case 0x00C1: case 0x00C0: case 0x00C4: case 0x00C2: return 'A';
        case 0x00C9: case 0x00C8: case 0x00CB: case 0x00CA: return 'E';
        case 0x00CD: case 0x00CC: case 0x00CF: case 0x00CE: return 'I';
        case 0x00D3: case 0x00D2: case 0x00D6: case 0x00D4: return 'O';
        case 0x00DA: case 0x00D9: case 0x00DC: case 0x00DB: return 'U';
        case 0x00D1: return 'N';
        default: return '_';
    }
}

struct UsbExtent {
    uint64_t lba = 0;     // LBA de inicio en sectores de 512 B (0 = sparse/ceros en NTFS)
    uint64_t byteLen = 0; // Longitud del tramo contiguo en bytes
};

struct UsbFile {
    std::string subPath;
    uint32_t startCluster = 0;
    uint64_t size = 0;
    bool exfatContiguous = false;
    std::vector<UsbExtent> extents;
    std::vector<uint8_t> residentData;
};

struct UsbPkg {
    std::string name;
    std::vector<UsbFile> files;
    uint64_t totalBytes = 0;
    bool marked = false;
};

enum class FsKind {
    None,
    FAT12,
    FAT16,
    FAT32,
    ExFAT,
    NTFS
};

// Acceso crudo de SOLO LECTURA a /dev/usb0X via Mocha FSA (nunca escribe en la Memoria USB)
class FsaRawDevice {
public:
    FsaRawDevice() = default;
    ~FsaRawDevice() { Shutdown(); }

    FsaRawDevice(const FsaRawDevice&) = delete;
    FsaRawDevice& operator=(const FsaRawDevice&) = delete;

    bool EnsureBuffers() {
        if (req_ && resp_ && vecs_ && dmaBuf_) return true;
        if (!req_)    req_    = static_cast<FSARequest*>(MEMAllocFromDefaultHeapEx(0x540, 0x40));
        if (!resp_)   resp_   = static_cast<FSAResponse*>(MEMAllocFromDefaultHeapEx(0x300, 0x40));
        if (!vecs_)   vecs_   = static_cast<IOSVec*>(MEMAllocFromDefaultHeapEx(0x40, 0x40));
        if (!dmaBuf_) dmaBuf_ = static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(kMaxRawSectors * kSectorSize, 0x40));
        return req_ && resp_ && vecs_ && dmaBuf_;
    }

    void Shutdown() {
        Close();
        if (dmaBuf_) { MEMFreeToDefaultHeap(dmaBuf_); dmaBuf_ = nullptr; }
        if (vecs_)   { MEMFreeToDefaultHeap(vecs_);   vecs_   = nullptr; }
        if (resp_)   { MEMFreeToDefaultHeap(resp_);   resp_   = nullptr; }
        if (req_)    { MEMFreeToDefaultHeap(req_);    req_    = nullptr; }
    }

    bool Open(FSAClientHandle rawClient, const char* devPath) {
        Close();
        if (rawClient < 0 || !devPath || !EnsureBuffers()) return false;

        std::memset(req_, 0, sizeof(FSARequest));
        std::memset(resp_, 0, sizeof(FSAResponse));
        std::strncpy(req_->rawOpen.path, devPath, sizeof(req_->rawOpen.path) - 1);

        DCFlushRange(req_, 0x540);
        DCInvalidateRange(resp_, 0x300);

        IOSError err = IOS_Ioctl(static_cast<IOSHandle>(rawClient),
                                 FSA_COMMAND_RAW_OPEN,
                                 req_, sizeof(FSARequest),
                                 resp_, sizeof(FSAResponse));
        DCInvalidateRange(resp_, 0x300);
        if (err != IOS_ERROR_OK || resp_->rawOpen.handle < 0) {
            return false;
        }

        client_ = rawClient;
        devHandle_ = resp_->rawOpen.handle;
        return true;
    }

    void Close() {
        if (devHandle_ >= 0 && client_ >= 0 && req_ && resp_) {
            std::memset(req_, 0, sizeof(FSARequest));
            std::memset(resp_, 0, sizeof(FSAResponse));
            req_->rawClose.handle = devHandle_;
            DCFlushRange(req_, 0x540);
            DCInvalidateRange(resp_, 0x300);
            IOS_Ioctl(static_cast<IOSHandle>(client_),
                      FSA_COMMAND_RAW_CLOSE,
                      req_, sizeof(FSARequest),
                      resp_, sizeof(FSAResponse));
        }
        devHandle_ = -1;
        client_ = -1;
    }

    bool IsOpen() const { return devHandle_ >= 0 && client_ >= 0; }

    bool ReadSectors(uint64_t lba, uint32_t count, void* dst) {
        if (!IsOpen() || !dst || count == 0) return false;
        auto* outBytes = static_cast<uint8_t*>(dst);
        uint32_t done = 0;

        while (done < count) {
            uint32_t chunk = std::min<uint32_t>(kMaxRawSectors, count - done);
            uint32_t byteLen = chunk * kSectorSize;
            uint8_t* curDst = outBytes + static_cast<size_t>(done) * kSectorSize;

            bool directDma = (reinterpret_cast<uintptr_t>(curDst) & 0x3Fu) == 0;
            uint8_t* targetBuf = directDma ? curDst : dmaBuf_;

            std::memset(req_, 0, sizeof(FSARequest));
            std::memset(resp_, 0, sizeof(FSAResponse));
            req_->emulatedError = static_cast<FSError>(0);
            req_->rawRead.blocks_offset = lba + done;
            req_->rawRead.count = chunk;
            req_->rawRead.size = kSectorSize;
            req_->rawRead.device_handle = static_cast<uint32_t>(devHandle_);

            vecs_[0].vaddr = req_;
            vecs_[0].len   = sizeof(FSARequest);
            vecs_[0].paddr = nullptr;
            vecs_[1].vaddr = targetBuf;
            vecs_[1].len   = byteLen;
            vecs_[1].paddr = nullptr;
            vecs_[2].vaddr = resp_;
            vecs_[2].len   = sizeof(FSAResponse);
            vecs_[2].paddr = nullptr;

            DCFlushRange(req_, 0x540);
            DCFlushRange(vecs_, 0x40);
            DCInvalidateRange(targetBuf, byteLen);
            DCInvalidateRange(resp_, 0x300);

            IOSError err = IOS_Ioctlv(static_cast<IOSHandle>(client_),
                                      FSA_COMMAND_RAW_READ,
                                      1, 2, vecs_);
            DCInvalidateRange(targetBuf, byteLen);
            DCInvalidateRange(resp_, 0x300);

            if (err < 0) return false;

            if (!directDma) {
                std::memcpy(curDst, dmaBuf_, byteLen);
            }
            done += chunk;
        }
        return true;
    }

private:
    FSAClientHandle client_ = -1;
    int32_t devHandle_ = -1;
    FSARequest* req_ = nullptr;
    FSAResponse* resp_ = nullptr;
    IOSVec* vecs_ = nullptr;
    uint8_t* dmaBuf_ = nullptr;
};

class UsbFatVolume {
public:
    struct DirEntry {
        std::string name;
        bool isDir = false;
        uint32_t startCluster = 0;
        uint64_t size = 0;
        bool exfatContiguous = false;
        uint64_t dirByteLen = 0;
        uint64_t ntfsMftRec = 0;
        std::vector<UsbExtent> extents;
        std::vector<uint8_t> residentData;
    };

    class FileStream {
    public:
        FileStream() = default;

        void Open(UsbFatVolume* vol, const UsbFile& file) {
            vol_ = vol;
            curCluster_ = file.startCluster;
            offsetInCluster_ = 0;
            remaining_ = file.size;
            residentPos_ = 0;
            residentData_ = file.residentData;
            extents_ = file.extents;
            extIdx_ = 0;
            offsetInExt_ = 0;

            if (vol_ && file.exfatContiguous && extents_.empty() && file.size > 0 && vol_->IsValidCluster(file.startCluster)) {
                uint64_t clusBytes = vol_->ClusterBytes();
                uint64_t numClus = (file.size + clusBytes - 1) / clusBytes;
                UsbExtent ext;
                ext.lba = vol_->ClusterToLba(file.startCluster);
                ext.byteLen = numClus * clusBytes;
                extents_.push_back(ext);
            }
            useExtents_ = !extents_.empty();
            useResident_ = !residentData_.empty();
        }

        void Open(UsbFatVolume* vol, uint32_t startCluster, uint64_t size) {
            UsbFile f;
            f.startCluster = startCluster;
            f.size = size;
            Open(vol, f);
        }

        uint64_t Remaining() const { return remaining_; }

        bool ReadExact(void* dst, uint32_t len) {
            if (!vol_ || len > remaining_) return false;
            if (len == 0) return true;

            auto* out = static_cast<uint8_t*>(dst);

            // 1. Archivo residente en MFT (NTFS pequeño)
            if (useResident_) {
                if (residentPos_ + len > residentData_.size()) {
                    size_t avail = (residentPos_ < residentData_.size()) ? (residentData_.size() - residentPos_) : 0;
                    if (avail > 0) std::memcpy(out, residentData_.data() + residentPos_, avail);
                    std::memset(out + avail, 0, len - avail);
                } else {
                    std::memcpy(out, residentData_.data() + residentPos_, len);
                }
                residentPos_ += len;
                remaining_ -= len;
                return true;
            }

            // 2. Extents pre-resueltos (NTFS data runs o exFAT NoFatChain contiguo)
            if (useExtents_) {
                uint32_t got = 0;
                while (got < len) {
                    if (extIdx_ >= extents_.size()) {
                        uint32_t rem = len - got;
                        std::memset(out + got, 0, rem);
                        got += rem;
                        remaining_ -= rem;
                        break;
                    }
                    const UsbExtent& ext = extents_[extIdx_];
                    uint64_t availInExt = (offsetInExt_ < ext.byteLen) ? (ext.byteLen - offsetInExt_) : 0;
                    if (availInExt == 0) {
                        ++extIdx_;
                        offsetInExt_ = 0;
                        continue;
                    }

                    uint32_t want = static_cast<uint32_t>(std::min<uint64_t>(len - got, availInExt));
                    if (ext.lba == 0) {
                        std::memset(out + got, 0, want);
                        got += want;
                        remaining_ -= want;
                        offsetInExt_ += want;
                    } else if ((offsetInExt_ % kSectorSize) == 0 && want >= kSectorSize) {
                        uint64_t secInExt = offsetInExt_ / kSectorSize;
                        uint32_t takeSectors = want / kSectorSize;
                        uint32_t takeBytes = takeSectors * kSectorSize;
                        if (!vol_->raw_.ReadSectors(ext.lba + secInExt, takeSectors, out + got)) {
                            return false;
                        }
                        got += takeBytes;
                        remaining_ -= takeBytes;
                        offsetInExt_ += takeBytes;
                    } else {
                        uint64_t secInExt = offsetInExt_ / kSectorSize;
                        uint32_t offInSec = static_cast<uint32_t>(offsetInExt_ % kSectorSize);
                        uint8_t secBuf[kSectorSize];
                        if (!vol_->raw_.ReadSectors(ext.lba + secInExt, 1, secBuf)) {
                            return false;
                        }
                        uint32_t take = std::min<uint32_t>(want, kSectorSize - offInSec);
                        std::memcpy(out + got, secBuf + offInSec, take);
                        got += take;
                        remaining_ -= take;
                        offsetInExt_ += take;
                    }

                    if (offsetInExt_ >= ext.byteLen) {
                        ++extIdx_;
                        offsetInExt_ = 0;
                    }
                }
                return true;
            }

            // 3. Cadena de clusters FAT12 / FAT16 / FAT32 / exFAT con FAT chain
            uint32_t got = 0;
            const uint32_t clusBytes = vol_->ClusterBytes();

            while (got < len) {
                if (!vol_->IsValidCluster(curCluster_)) return false;

                uint32_t want = len - got;
                if ((offsetInCluster_ % kSectorSize) == 0 && want >= kSectorSize) {
                    uint32_t secInClus = offsetInCluster_ / kSectorSize;
                    uint32_t availWantSectors = want / kSectorSize;

                    if (secInClus == 0 && availWantSectors >= vol_->secPerClus_) {
                        uint32_t maxClus = std::min<uint32_t>(
                            availWantSectors / vol_->secPerClus_,
                            std::max<uint32_t>(1, kMaxRawSectors / vol_->secPerClus_));
                        uint32_t nextClus = 0;
                        uint32_t runClus = vol_->CountContiguousClusters(curCluster_, maxClus, nextClus);
                        uint32_t runSectors = runClus * vol_->secPerClus_;
                        uint32_t runBytes = runSectors * kSectorSize;

                        if (!vol_->raw_.ReadSectors(vol_->ClusterToLba(curCluster_), runSectors, out + got)) {
                            return false;
                        }
                        got += runBytes;
                        remaining_ -= runBytes;
                        curCluster_ = nextClus;
                        offsetInCluster_ = 0;
                        continue;
                    }

                    uint32_t remSectorsInClus = vol_->secPerClus_ - secInClus;
                    uint32_t takeSectors = std::min<uint32_t>(availWantSectors, remSectorsInClus);
                    uint32_t takeBytes = takeSectors * kSectorSize;

                    if (!vol_->raw_.ReadSectors(vol_->ClusterToLba(curCluster_) + secInClus, takeSectors, out + got)) {
                        return false;
                    }
                    got += takeBytes;
                    remaining_ -= takeBytes;
                    offsetInCluster_ += takeBytes;
                    if (offsetInCluster_ >= clusBytes) {
                        curCluster_ = vol_->GetFatEntry(curCluster_);
                        offsetInCluster_ = 0;
                    }
                    continue;
                }

                uint32_t secInClus = offsetInCluster_ / kSectorSize;
                uint32_t offInSec  = offsetInCluster_ % kSectorSize;
                uint8_t secBuf[kSectorSize];
                if (!vol_->raw_.ReadSectors(vol_->ClusterToLba(curCluster_) + secInClus, 1, secBuf)) {
                    return false;
                }
                uint32_t take = std::min<uint32_t>(want, kSectorSize - offInSec);
                std::memcpy(out + got, secBuf + offInSec, take);
                got += take;
                remaining_ -= take;
                offsetInCluster_ += take;
                if (offsetInCluster_ >= clusBytes) {
                    curCluster_ = vol_->GetFatEntry(curCluster_);
                    offsetInCluster_ = 0;
                }
            }
            return true;
        }

    private:
        UsbFatVolume* vol_ = nullptr;
        uint32_t curCluster_ = 0;
        uint32_t offsetInCluster_ = 0;
        uint64_t remaining_ = 0;

        bool useResident_ = false;
        size_t residentPos_ = 0;
        std::vector<uint8_t> residentData_;

        bool useExtents_ = false;
        std::vector<UsbExtent> extents_;
        size_t extIdx_ = 0;
        uint64_t offsetInExt_ = 0;
    };

    UsbFatVolume() = default;
    ~UsbFatVolume() { Shutdown(); }

    UsbFatVolume(const UsbFatVolume&) = delete;
    UsbFatVolume& operator=(const UsbFatVolume&) = delete;

    void Unmount() {
        raw_.Close();
        mounted_ = false;
        fsKind_ = FsKind::None;
        fatCacheBaseSec_ = UINT32_MAX;
        fatCacheSecCount_ = 0;
        mftExtents_.clear();
    }

    void Shutdown() {
        Unmount();
        raw_.Shutdown();
        if (fatCache_) {
            MEMFreeToDefaultHeap(fatCache_);
            fatCache_ = nullptr;
        }
    }

    bool IsMounted() const { return mounted_; }
    FsKind Kind() const { return fsKind_; }
    const char* FsName() const {
        switch (fsKind_) {
            case FsKind::FAT12: return "FAT";
            case FsKind::FAT16: return "FAT";
            case FsKind::FAT32: return "FAT32";
            case FsKind::ExFAT: return "exFAT";
            case FsKind::NTFS:  return "NTFS";
            default:            return "USB";
        }
    }
    uint32_t ClusterBytes() const { return secPerClus_ * kSectorSize; }

    bool ProbeAndMount(FSAClientHandle rawClient, std::string& outDevPath, std::string& outDiag) {
        Unmount();
        outDevPath.clear();
        outDiag.clear();

        if (rawClient < 0) {
            outDiag = "Acceso USB no disponible.";
            return false;
        }
        if (!fatCache_) {
            fatCache_ = static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(kFatCacheSectors * kSectorSize, 0x40));
            if (!fatCache_) {
                outDiag = "Sin memoria para cache USB.";
                return false;
            }
        }

        const char* candidates[] = {"/dev/usb01", "/dev/usb02", "/dev/usb03", "/dev/usb04"};

        for (const char* dev : candidates) {
            if (!raw_.Open(rawClient, dev)) continue;

            uint8_t sec0[kSectorSize]{};
            if (!raw_.ReadSectors(0, 1, sec0)) {
                raw_.Close();
                continue;
            }

            // 1. Superfloppy (BPB directo en LBA 0: FAT12/16/32, exFAT o NTFS)
            if (TryParseVolumeBpb(0)) {
                mounted_ = true;
                outDevPath = dev;
                return true;
            }

            // 2. MBR (firma estandar 0x55 0xAA o UStealth 0x55 0xAB)
            bool validMbrSig = (sec0[510] == 0x55 && (sec0[511] == 0xAA || sec0[511] == 0xAB));
            if (validMbrSig) {
                bool isGpt = false;
                for (int i = 0; i < 4; ++i) {
                    const uint8_t* pe = sec0 + 446 + i * 16;
                    uint8_t ptype = pe[4];
                    uint32_t firstLba = Rd32LE(pe + 8);
                    uint32_t secCount = Rd32LE(pe + 12);
                    if (ptype == 0xEE) {
                        isGpt = true;
                        continue;
                    }
                    if (ptype == 0 || firstLba == 0 || secCount == 0) continue;
                    if (TryParseVolumeBpb(firstLba)) {
                        mounted_ = true;
                        outDevPath = dev;
                        return true;
                    }
                }

                // 3. GPT (si se detecto MBR protector 0xEE)
                if (isGpt && ProbeGptPartitions()) {
                    mounted_ = true;
                    outDevPath = dev;
                    return true;
                }
            }

            raw_.Close();
        }

        outDiag = "Sin Memoria USB (exFAT / FAT32 / NTFS / FAT).";
        return false;
    }

    bool ScanWupPackages(std::vector<UsbPkg>& outPkgs) {
        outPkgs.clear();
        if (!mounted_) return false;

        std::vector<DirEntry> rootEntries;
        if (!ListRootDirectory(rootEntries)) {
            return false;
        }

        auto scanContainerDir = [&](const std::vector<DirEntry>& entries) {
            for (const auto& e : entries) {
                if (!e.isDir || e.name.empty() || e.name[0] == '.') continue;
                if (EqualsCI(e.name, "System Volume Information") ||
                    EqualsCI(e.name, "$RECYCLE.BIN") ||
                    EqualsCI(e.name, "LOST.DIR") ||
                    EqualsCI(e.name, "Android") ||
                    EqualsCI(e.name, "wiiu") ||
                    EqualsCI(e.name, "install") ||
                    EqualsCI(e.name, "packages") ||
                    EqualsCI(e.name, "WiiU_Transfer")) {
                    continue;
                }

                UsbPkg pkg;
                pkg.name = e.name;
                bool looksLikeWup = false;
                CollectFilesRecursive(e, "", 0, pkg.files, pkg.totalBytes, looksLikeWup);
                if (looksLikeWup && !pkg.files.empty()) {
                    outPkgs.push_back(std::move(pkg));
                }
            }
        };

        // 1. Carpetas de juegos en la raiz de la Memoria USB
        scanContainerDir(rootEntries);

        // 2. Carpetas de juegos dentro de /install, /packages o /WiiU_Transfer si existen
        for (const auto& e : rootEntries) {
            if (e.isDir && (EqualsCI(e.name, "install") ||
                            EqualsCI(e.name, "packages") ||
                            EqualsCI(e.name, "WiiU_Transfer"))) {
                std::vector<DirEntry> subEntries;
                if (ListSubDirectory(e, subEntries)) {
                    scanContainerDir(subEntries);
                }
            }
        }
        return true;
    }

private:
    bool ProbeGptPartitions() {
        uint8_t hdr[kSectorSize]{};
        if (!raw_.ReadSectors(1, 1, hdr)) return false;
        if (std::memcmp(hdr, "EFI PART", 8) != 0) return false;

        uint64_t partLba = Rd64LE(hdr + 72);
        uint32_t numParts = std::min<uint32_t>(Rd32LE(hdr + 80), 32);
        uint32_t entSize  = Rd32LE(hdr + 84);
        if (entSize < 128 || entSize > 512 || numParts == 0) return false;

        uint32_t totalBytes = numParts * entSize;
        uint32_t sectors = (totalBytes + kSectorSize - 1) / kSectorSize;
        std::vector<uint8_t> table(static_cast<size_t>(sectors) * kSectorSize);
        if (!raw_.ReadSectors(partLba, sectors, table.data())) return false;

        for (uint32_t i = 0; i < numParts; ++i) {
            const uint8_t* pe = table.data() + static_cast<size_t>(i) * entSize;
            bool emptyGuid = true;
            for (int b = 0; b < 16; ++b) {
                if (pe[b] != 0) { emptyGuid = false; break; }
            }
            if (emptyGuid) continue;

            uint64_t firstLba = Rd64LE(pe + 32);
            uint64_t lastLba  = Rd64LE(pe + 40);
            if (firstLba == 0 || lastLba <= firstLba) continue;

            if (TryParseVolumeBpb(firstLba)) {
                return true;
            }
        }
        return false;
    }

    bool TryParseVolumeBpb(uint64_t partLba) {
        uint8_t bpb[kSectorSize]{};
        if (!raw_.ReadSectors(partLba, 1, bpb)) return false;
        if (bpb[510] != 0x55 || (bpb[511] != 0xAA && bpb[511] != 0xAB)) return false;

        if (std::memcmp(bpb + 3, "EXFAT   ", 8) == 0) {
            return TryParseExFatBpb(partLba, bpb);
        }
        if (std::memcmp(bpb + 3, "NTFS    ", 8) == 0) {
            return TryParseNtfsBpb(partLba, bpb);
        }
        return TryParseFatBpb(partLba, bpb);
    }

    // =========================================================================
    // 1. Parser exFAT
    // =========================================================================
    bool TryParseExFatBpb(uint64_t partLba, const uint8_t* bpb) {
        uint32_t fatOff      = Rd32LE(bpb + 80);
        uint32_t fatLen      = Rd32LE(bpb + 84);
        uint32_t clusHeapOff = Rd32LE(bpb + 88);
        uint32_t clusCnt     = Rd32LE(bpb + 92);
        uint32_t rootClus    = Rd32LE(bpb + 96);
        uint8_t  bpsShift    = bpb[108];
        uint8_t  spcShift    = bpb[109];

        if (bpsShift < 9 || bpsShift > 12) return false;
        if ((bpsShift + spcShift) > 25) return false;
        if (fatOff == 0 || fatLen == 0 || clusHeapOff == 0 || clusCnt == 0 || rootClus < 2) return false;

        uint32_t secScale = 1u << (bpsShift - 9);
        partStartLba_     = partLba;
        secPerClus_       = (1u << spcShift) * secScale;
        fatStartLba_      = partLba + static_cast<uint64_t>(fatOff) * secScale;
        fatSectors_       = fatLen * secScale;
        rootDirLba_       = 0;
        rootDirSectors_   = 0;
        firstDataSec_     = clusHeapOff * secScale;
        rootCluster_      = rootClus;
        clusterCount_     = clusCnt;
        fsKind_           = FsKind::ExFAT;
        fatCacheBaseSec_  = UINT32_MAX;
        fatCacheSecCount_ = 0;
        return true;
    }

    // =========================================================================
    // 2. Parser FAT12 / FAT16 / FAT32
    // =========================================================================
    bool TryParseFatBpb(uint64_t partLba, const uint8_t* bpb) {
        uint16_t bytesPerSec = Rd16LE(bpb + 11);
        uint8_t  secPerClus  = bpb[13];
        uint16_t rsvdSecCnt  = Rd16LE(bpb + 14);
        uint8_t  numFats     = bpb[16];
        uint16_t rootEntCnt  = Rd16LE(bpb + 17);
        uint16_t totSec16    = Rd16LE(bpb + 19);
        uint16_t fatSz16     = Rd16LE(bpb + 22);
        uint32_t totSec32    = Rd32LE(bpb + 32);
        uint32_t fatSz32     = Rd32LE(bpb + 36);
        uint32_t rootClus    = Rd32LE(bpb + 44);

        if (bytesPerSec < 512 || bytesPerSec > 4096 || (bytesPerSec & (bytesPerSec - 1)) != 0) return false;
        if (secPerClus == 0 || (secPerClus & (secPerClus - 1)) != 0 || secPerClus > 128) return false;
        if (rsvdSecCnt == 0 || numFats == 0 || numFats > 4) return false;

        uint32_t totSec = totSec16 ? static_cast<uint32_t>(totSec16) : totSec32;
        uint32_t fatSz  = fatSz16 ? static_cast<uint32_t>(fatSz16) : fatSz32;
        if (totSec == 0 || fatSz == 0) return false;

        uint32_t secScale       = bytesPerSec / kSectorSize;
        uint32_t rootDirSectors = ((static_cast<uint32_t>(rootEntCnt) * 32u) + (bytesPerSec - 1)) / bytesPerSec;
        uint32_t firstDataSec   = static_cast<uint32_t>(rsvdSecCnt) + static_cast<uint32_t>(numFats) * fatSz + rootDirSectors;
        if (firstDataSec >= totSec) return false;

        uint32_t dataSec = totSec - firstDataSec;
        uint32_t clusCnt = dataSec / secPerClus;
        if (clusCnt == 0) return false;

        FsKind kind = FsKind::FAT32;
        if (clusCnt < 4085) {
            if (fatSz16 == 0 || rootDirSectors == 0) return false;
            kind = FsKind::FAT12;
        } else if (clusCnt < 65525) {
            if (fatSz16 == 0 || rootDirSectors == 0) return false;
            kind = FsKind::FAT16;
        } else {
            if (fatSz16 != 0 || rootClus < 2) return false;
            kind = FsKind::FAT32;
        }

        partStartLba_     = partLba;
        secPerClus_       = static_cast<uint32_t>(secPerClus) * secScale;
        fatStartLba_      = partLba + static_cast<uint64_t>(rsvdSecCnt) * secScale;
        fatSectors_       = fatSz * secScale;
        rootDirLba_       = partLba + (static_cast<uint64_t>(rsvdSecCnt) + static_cast<uint64_t>(numFats) * fatSz) * secScale;
        rootDirSectors_   = rootDirSectors * secScale;
        firstDataSec_     = firstDataSec * secScale;
        rootCluster_      = (kind == FsKind::FAT32) ? rootClus : 0;
        clusterCount_     = clusCnt;
        fsKind_           = kind;
        fatCacheBaseSec_  = UINT32_MAX;
        fatCacheSecCount_ = 0;
        return true;
    }

    // =========================================================================
    // 3. Parser NTFS ($MFT, Data Runs, $INDEX_ROOT + $INDEX_ALLOCATION)
    // =========================================================================
    bool TryParseNtfsBpb(uint64_t partLba, const uint8_t* bpb) {
        uint16_t bytesPerSec = Rd16LE(bpb + 11);
        uint8_t  spcRaw      = bpb[13];
        uint64_t totSec64    = Rd64LE(bpb + 40);
        uint64_t mftClus     = Rd64LE(bpb + 48);
        int8_t   cpmr        = static_cast<int8_t>(bpb[64]);
        int8_t   cpib        = static_cast<int8_t>(bpb[68]);

        if (bytesPerSec < 512 || bytesPerSec > 4096 || (bytesPerSec & (bytesPerSec - 1)) != 0) return false;
        uint32_t clusBytes = 0;
        if (spcRaw > 0 && spcRaw <= 128 && (spcRaw & (spcRaw - 1)) == 0) {
            clusBytes = static_cast<uint32_t>(bytesPerSec) * spcRaw;
        } else if (spcRaw >= 240) {
            clusBytes = 1u << (256 - spcRaw);
        } else {
            return false;
        }
        if (clusBytes < kSectorSize || (clusBytes % kSectorSize) != 0 || totSec64 == 0 || mftClus == 0) {
            return false;
        }

        uint32_t mftRecBytes = (cpmr > 0)
            ? (static_cast<uint32_t>(cpmr) * clusBytes)
            : ((cpmr < 0 && cpmr >= -16) ? (1u << (-cpmr)) : 0u);
        if (mftRecBytes < 512 || mftRecBytes > 4096 || (mftRecBytes % kSectorSize) != 0) {
            return false;
        }

        uint32_t idxBlkBytes = (cpib > 0)
            ? (static_cast<uint32_t>(cpib) * clusBytes)
            : ((cpib < 0 && cpib >= -16) ? (1u << (-cpib)) : 4096u);
        if (idxBlkBytes < 512 || idxBlkBytes > 65536 || (idxBlkBytes % kSectorSize) != 0) {
            idxBlkBytes = 4096u;
        }

        partStartLba_      = partLba;
        secPerClus_        = clusBytes / kSectorSize;
        mftRecordBytes_    = mftRecBytes;
        ntfsIndexBlkBytes_ = idxBlkBytes;
        mftExtents_.clear();

        // Leer MFT Record 0 ($MFT) para obtener los extents de toda la tabla MFT
        uint64_t mft0Lba = partStartLba_ + mftClus * secPerClus_;
        uint32_t recSecs = mftRecordBytes_ / kSectorSize;
        std::vector<uint8_t> rec0(mftRecordBytes_);
        if (!raw_.ReadSectors(mft0Lba, recSecs, rec0.data())) return false;
        if (std::memcmp(rec0.data(), "FILE", 4) != 0) return false;
        if (!ApplyNtfsUsaFixup(rec0.data(), mftRecordBytes_)) return false;

        uint16_t attrOff = Rd16LE(rec0.data() + 20);
        while (attrOff + 24 <= mftRecordBytes_) {
            const uint8_t* a = rec0.data() + attrOff;
            uint32_t aType = Rd32LE(a);
            if (aType == 0xFFFFFFFFu) break;
            uint32_t aLen = Rd32LE(a + 4);
            if (aLen < 24 || attrOff + aLen > mftRecordBytes_) break;

            uint8_t nonRes  = a[8];
            uint8_t nameLen = a[9];
            if (aType == 0x80u && nameLen == 0 && nonRes == 1) {
                uint16_t runOff = Rd16LE(a + 32);
                if (runOff < aLen) {
                    DecodeNtfsDataRuns(a + runOff, aLen - runOff, mftExtents_);
                }
            }
            attrOff += aLen;
        }

        if (mftExtents_.empty()) return false;
        fsKind_ = FsKind::NTFS;
        return true;
    }

    static bool ApplyNtfsUsaFixup(uint8_t* rec, uint32_t recSize) {
        uint16_t usaOff = Rd16LE(rec + 4);
        uint16_t usaCnt = Rd16LE(rec + 6);
        uint32_t numSecs = recSize / kSectorSize;
        if (usaCnt < numSecs + 1 || static_cast<uint32_t>(usaOff) + static_cast<uint32_t>(usaCnt) * 2u > recSize) {
            return false;
        }
        uint16_t usn = Rd16LE(rec + usaOff);
        for (uint32_t s = 0; s < numSecs; ++s) {
            uint8_t* tail = rec + (s + 1) * kSectorSize - 2;
            if (Rd16LE(tail) != usn) return false;
            const uint8_t* repl = rec + usaOff + (s + 1) * 2;
            tail[0] = repl[0];
            tail[1] = repl[1];
        }
        return true;
    }

    bool DecodeNtfsDataRuns(const uint8_t* runData, uint32_t maxLen, std::vector<UsbExtent>& outExtents) const {
        uint32_t pos = 0;
        int64_t curLcn = 0;
        const uint64_t clusBytes = ClusterBytes();

        while (pos < maxLen) {
            uint8_t hdr = runData[pos++];
            if (hdr == 0) break;
            uint32_t lenBytes = hdr & 0x0Fu;
            uint32_t offBytes = (hdr >> 4) & 0x0Fu;
            if (lenBytes == 0 || lenBytes > 8 || offBytes > 8 || pos + lenBytes + offBytes > maxLen) {
                return false;
            }

            uint64_t runClusters = 0;
            for (uint32_t i = 0; i < lenBytes; ++i) {
                runClusters |= (static_cast<uint64_t>(runData[pos + i]) << (i * 8));
            }
            pos += lenBytes;

            if (offBytes == 0) {
                // Tramo sparse (ceros)
                UsbExtent ext;
                ext.lba = 0;
                ext.byteLen = runClusters * clusBytes;
                outExtents.push_back(ext);
            } else {
                uint64_t rawDelta = 0;
                for (uint32_t i = 0; i < offBytes; ++i) {
                    rawDelta |= (static_cast<uint64_t>(runData[pos + i]) << (i * 8));
                }
                if (runData[pos + offBytes - 1] & 0x80u) {
                    for (uint32_t i = offBytes; i < 8; ++i) {
                        rawDelta |= (0xFFULL << (i * 8));
                    }
                }
                pos += offBytes;
                curLcn += static_cast<int64_t>(rawDelta);
                if (curLcn < 0) return false;

                UsbExtent ext;
                ext.lba = partStartLba_ + static_cast<uint64_t>(curLcn) * secPerClus_;
                ext.byteLen = runClusters * clusBytes;
                outExtents.push_back(ext);
            }
        }
        return true;
    }

    bool ReadBytesFromExtents(const std::vector<UsbExtent>& extents,
                              uint64_t byteOffset,
                              uint32_t byteLen,
                              uint8_t* dst) {
        uint64_t curOff = 0;
        uint32_t got = 0;
        for (const auto& ext : extents) {
            if (byteOffset >= curOff + ext.byteLen) {
                curOff += ext.byteLen;
                continue;
            }
            uint64_t offInExt = (byteOffset > curOff) ? (byteOffset - curOff) : 0;
            uint64_t avail = ext.byteLen - offInExt;
            uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(byteLen - got, avail));

            if (ext.lba == 0) {
                std::memset(dst + got, 0, take);
            } else if ((offInExt % kSectorSize) == 0 && (take % kSectorSize) == 0) {
                if (!raw_.ReadSectors(ext.lba + (offInExt / kSectorSize), take / kSectorSize, dst + got)) {
                    return false;
                }
            } else {
                uint64_t firstSec = offInExt / kSectorSize;
                uint64_t lastSec  = (offInExt + take - 1) / kSectorSize;
                uint32_t secCnt   = static_cast<uint32_t>(lastSec - firstSec + 1);
                std::vector<uint8_t> tmp(static_cast<size_t>(secCnt) * kSectorSize);
                if (!raw_.ReadSectors(ext.lba + firstSec, secCnt, tmp.data())) {
                    return false;
                }
                std::memcpy(dst + got, tmp.data() + (offInExt % kSectorSize), take);
            }

            got += take;
            byteOffset += take;
            curOff += ext.byteLen;
            if (got >= byteLen) return true;
        }
        return false;
    }

    bool ReadNtfsMftRecord(uint64_t recNum, std::vector<uint8_t>& outRec) {
        outRec.resize(mftRecordBytes_);
        uint64_t byteOff = recNum * static_cast<uint64_t>(mftRecordBytes_);
        if (!ReadBytesFromExtents(mftExtents_, byteOff, mftRecordBytes_, outRec.data())) {
            return false;
        }
        if (std::memcmp(outRec.data(), "FILE", 4) != 0) return false;
        return ApplyNtfsUsaFixup(outRec.data(), mftRecordBytes_);
    }

    static void ParseNtfsIndexNode(const uint8_t* nodeHdr,
                                   uint32_t maxNodeBytes,
                                   uint64_t selfMftRec,
                                   std::vector<uint64_t>& ioChildRecs) {
        if (maxNodeBytes < 16) return;
        uint32_t entriesOff = Rd32LE(nodeHdr + 0);
        uint32_t totalSize  = Rd32LE(nodeHdr + 4);
        if (totalSize > maxNodeBytes) totalSize = maxNodeBytes;

        uint32_t pos = entriesOff;
        while (pos + 16 <= totalSize) {
            const uint8_t* ent = nodeHdr + pos;
            uint64_t fileRef = Rd64LE(ent + 0);
            uint16_t entLen  = Rd16LE(ent + 8);
            uint16_t keyLen  = Rd16LE(ent + 10);
            uint32_t flags   = Rd32LE(ent + 12);

            if (entLen < 16 || pos + entLen > totalSize) break;
            if (flags & 0x02u) break; // Ultima entrada del nodo

            uint64_t childMft = fileRef & 0x0000FFFFFFFFFFFFULL;
            if (keyLen >= 66 && childMft >= 16 && childMft != selfMftRec) {
                if (std::find(ioChildRecs.begin(), ioChildRecs.end(), childMft) == ioChildRecs.end()) {
                    ioChildRecs.push_back(childMft);
                }
            }
            pos += entLen;
        }
    }

    void CollectNtfsExtensionRecords(const std::vector<uint8_t>& baseRec,
                                     uint64_t baseMftNum,
                                     uint32_t wantAttrType,
                                     std::vector<uint64_t>& outExtRecs) {
        uint16_t attrOff = Rd16LE(baseRec.data() + 20);
        while (attrOff + 24 <= mftRecordBytes_) {
            const uint8_t* a = baseRec.data() + attrOff;
            uint32_t aType = Rd32LE(a);
            if (aType == 0xFFFFFFFFu) break;
            uint32_t aLen = Rd32LE(a + 4);
            if (aLen < 24 || attrOff + aLen > mftRecordBytes_) break;

            if (aType == 0x20u) { // $ATTRIBUTE_LIST
                std::vector<uint8_t> listBuf;
                if (a[8] == 0) {
                    uint32_t valLen = Rd32LE(a + 16);
                    uint16_t valOff = Rd16LE(a + 20);
                    if (static_cast<uint32_t>(valOff) + valLen <= aLen) {
                        listBuf.assign(a + valOff, a + valOff + valLen);
                    }
                } else {
                    uint16_t runOff = Rd16LE(a + 32);
                    uint64_t realSz = Rd64LE(a + 48);
                    std::vector<UsbExtent> listExts;
                    if (runOff < aLen && realSz > 0 && realSz <= 262144 &&
                        DecodeNtfsDataRuns(a + runOff, aLen - runOff, listExts)) {
                        listBuf.resize(static_cast<size_t>(realSz));
                        if (!ReadBytesFromExtents(listExts, 0, static_cast<uint32_t>(realSz), listBuf.data())) {
                            listBuf.clear();
                        }
                    }
                }

                struct VcnRecPair { uint64_t vcn; uint64_t rec; };
                std::vector<VcnRecPair> pairs;
                size_t p = 0;
                while (p + 26 <= listBuf.size()) {
                    uint32_t entType = Rd32LE(listBuf.data() + p);
                    uint16_t entLen  = Rd16LE(listBuf.data() + p + 4);
                    uint8_t  nameLen = listBuf[p + 6];
                    if (entLen < 26 || p + entLen > listBuf.size()) break;
                    uint64_t startVcn = Rd64LE(listBuf.data() + p + 8);
                    uint64_t refMft   = Rd64LE(listBuf.data() + p + 16) & 0x0000FFFFFFFFFFFFULL;
                    if (entType == wantAttrType && refMft != baseMftNum) {
                        if (wantAttrType != 0x80u || nameLen == 0) {
                            pairs.push_back({startVcn, refMft});
                        }
                    }
                    p += entLen;
                }
                std::sort(pairs.begin(), pairs.end(), [](const VcnRecPair& x, const VcnRecPair& y) {
                    return x.vcn < y.vcn;
                });
                for (const auto& pr : pairs) {
                    if (std::find(outExtRecs.begin(), outExtRecs.end(), pr.rec) == outExtRecs.end()) {
                        outExtRecs.push_back(pr.rec);
                    }
                }
            }
            attrOff += aLen;
        }
    }

    bool ListNtfsDirectory(uint64_t dirMftRec, std::vector<DirEntry>& outEntries) {
        outEntries.clear();
        std::vector<uint8_t> dirRec;
        if (!ReadNtfsMftRecord(dirMftRec, dirRec)) return false;

        uint32_t idxBlockSize = ntfsIndexBlkBytes_;
        std::vector<uint64_t> childRecs;
        std::vector<UsbExtent> idxAllocExtents;

        auto parseDirAttrs = [&](const std::vector<uint8_t>& rec) {
            uint16_t attrOff = Rd16LE(rec.data() + 20);
            while (attrOff + 24 <= mftRecordBytes_) {
                const uint8_t* a = rec.data() + attrOff;
                uint32_t aType = Rd32LE(a);
                if (aType == 0xFFFFFFFFu) break;
                uint32_t aLen = Rd32LE(a + 4);
                if (aLen < 24 || attrOff + aLen > mftRecordBytes_) break;

                uint8_t nonRes = a[8];
                if (aType == 0x90u && nonRes == 0) { // $INDEX_ROOT
                    uint32_t valLen = Rd32LE(a + 16);
                    uint16_t valOff = Rd16LE(a + 20);
                    if (valLen >= 32 && static_cast<uint32_t>(valOff) + valLen <= aLen) {
                        const uint8_t* ir = a + valOff;
                        uint32_t bsz = Rd32LE(ir + 8);
                        if (bsz >= 512 && bsz <= 65536 && (bsz % kSectorSize) == 0) {
                            idxBlockSize = bsz;
                        }
                        ParseNtfsIndexNode(ir + 16, valLen - 16, dirMftRec, childRecs);
                    }
                } else if (aType == 0xA0u && nonRes == 1) { // $INDEX_ALLOCATION
                    uint16_t runOff = Rd16LE(a + 32);
                    if (runOff < aLen) {
                        DecodeNtfsDataRuns(a + runOff, aLen - runOff, idxAllocExtents);
                    }
                }
                attrOff += aLen;
            }
        };

        parseDirAttrs(dirRec);

        std::vector<uint64_t> extDirRecs;
        CollectNtfsExtensionRecords(dirRec, dirMftRec, 0xA0u, extDirRecs);
        for (uint64_t extMft : extDirRecs) {
            std::vector<uint8_t> extRec;
            if (ReadNtfsMftRecord(extMft, extRec)) {
                parseDirAttrs(extRec);
            }
        }

        if (!idxAllocExtents.empty() && idxBlockSize >= 512) {
            uint64_t totalIdxBytes = 0;
            for (const auto& e : idxAllocExtents) totalIdxBytes += e.byteLen;
            totalIdxBytes = std::min<uint64_t>(totalIdxBytes, 4ULL * 1024 * 1024); // Cap seguridad 4 MB por directorio

            std::vector<uint8_t> indxBlk(idxBlockSize);
            for (uint64_t off = 0; off + idxBlockSize <= totalIdxBytes; off += idxBlockSize) {
                if (!ReadBytesFromExtents(idxAllocExtents, off, idxBlockSize, indxBlk.data())) break;
                if (std::memcmp(indxBlk.data(), "INDX", 4) != 0) continue;
                if (!ApplyNtfsUsaFixup(indxBlk.data(), idxBlockSize)) continue;
                ParseNtfsIndexNode(indxBlk.data() + 24, idxBlockSize - 24, dirMftRec, childRecs);
            }
        }

        for (uint64_t childMft : childRecs) {
            std::vector<uint8_t> cRec;
            if (!ReadNtfsMftRecord(childMft, cRec)) continue;
            uint16_t recFlags = Rd16LE(cRec.data() + 22);
            if ((recFlags & 0x01u) == 0) continue; // No esta en uso

            DirEntry de;
            de.isDir = (recFlags & 0x02u) != 0;
            de.ntfsMftRec = childMft;
            int bestNameRank = -1;

            auto parseChildAttrs = [&](const std::vector<uint8_t>& rec) {
                uint16_t attrOff = Rd16LE(rec.data() + 20);
                while (attrOff + 24 <= mftRecordBytes_) {
                    const uint8_t* a = rec.data() + attrOff;
                    uint32_t aType = Rd32LE(a);
                    if (aType == 0xFFFFFFFFu) break;
                    uint32_t aLen = Rd32LE(a + 4);
                    if (aLen < 24 || attrOff + aLen > mftRecordBytes_) break;

                    uint8_t nonRes  = a[8];
                    uint8_t nameLen = a[9];

                    if (aType == 0x30u && nonRes == 0) { // $FILE_NAME
                        uint32_t valLen = Rd32LE(a + 16);
                        uint16_t valOff = Rd16LE(a + 20);
                        if (valLen >= 66 && static_cast<uint32_t>(valOff) + valLen <= aLen) {
                            const uint8_t* fn = a + valOff;
                            uint64_t parentRef = Rd64LE(fn + 0) & 0x0000FFFFFFFFFFFFULL;
                            uint32_t fnFlags   = Rd32LE(fn + 56);
                            uint8_t  fnChars   = fn[64];
                            uint8_t  fnNs      = fn[65];

                            if ((fnFlags & 0x10000000u) != 0) de.isDir = true;
                            int rank = (parentRef == dirMftRec ? 10 : 0) + ((fnNs != 2) ? 5 : 1);
                            if (rank > bestNameRank && 66u + static_cast<uint32_t>(fnChars) * 2u <= valLen) {
                                std::string candidate;
                                candidate.reserve(fnChars);
                                for (uint8_t k = 0; k < fnChars; ++k) {
                                    uint16_t ch = Rd16LE(fn + 66 + k * 2);
                                    candidate.push_back(FoldUtf16ToAscii(ch));
                                }
                                if (!candidate.empty()) {
                                    de.name = std::move(candidate);
                                    bestNameRank = rank;
                                }
                            }
                        }
                    } else if (aType == 0x80u && nameLen == 0 && !de.isDir) { // $DATA (stream principal sin nombre)
                        if (nonRes == 0) {
                            uint32_t valLen = Rd32LE(a + 16);
                            uint16_t valOff = Rd16LE(a + 20);
                            if (static_cast<uint32_t>(valOff) + valLen <= aLen) {
                                de.size = valLen;
                                de.residentData.assign(a + valOff, a + valOff + valLen);
                            }
                        } else {
                            uint64_t startVcn = Rd64LE(a + 16);
                            uint16_t runOff   = Rd16LE(a + 32);
                            uint64_t realSz   = Rd64LE(a + 48);
                            if (startVcn == 0) {
                                de.size = realSz;
                            }
                            if (runOff < aLen) {
                                DecodeNtfsDataRuns(a + runOff, aLen - runOff, de.extents);
                            }
                        }
                    }
                    attrOff += aLen;
                }
            };

            parseChildAttrs(cRec);

            // Si el archivo tiene $ATTRIBUTE_LIST (0x20), leer records adicionales para $FILE_NAME y $DATA
            std::vector<uint64_t> fnExtRecs;
            if (de.name.empty()) {
                CollectNtfsExtensionRecords(cRec, childMft, 0x30u, fnExtRecs);
                for (uint64_t r : fnExtRecs) {
                    std::vector<uint8_t> er;
                    if (ReadNtfsMftRecord(r, er)) parseChildAttrs(er);
                }
            }
            if (!de.isDir) {
                std::vector<uint64_t> dataExtRecs;
                CollectNtfsExtensionRecords(cRec, childMft, 0x80u, dataExtRecs);
                for (uint64_t r : dataExtRecs) {
                    std::vector<uint8_t> er;
                    if (ReadNtfsMftRecord(r, er)) parseChildAttrs(er);
                }
            }

            if (de.name.empty() || de.name == "." || de.name == ".." || de.name[0] == '$') continue;
            outEntries.push_back(std::move(de));
        }
        return true;
    }

    // =========================================================================
    // Funciones comunes de navegacion de directorios y FAT
    // =========================================================================
    bool IsValidCluster(uint32_t c) const {
        return c >= 2 && c < (clusterCount_ + 2);
    }

    bool IsEndOfChain(uint32_t val) const {
        switch (fsKind_) {
            case FsKind::FAT12: return val >= 0x0FF8u || val < 2;
            case FsKind::FAT16: return val >= 0xFFF8u || val < 2;
            case FsKind::FAT32: return (val & 0x0FFFFFFFu) >= 0x0FFFFFF8u || (val & 0x0FFFFFFFu) < 2;
            case FsKind::ExFAT: return val >= 0xFFFFFFF8u || val < 2;
            default:            return true;
        }
    }

    uint64_t ClusterToLba(uint32_t cluster) const {
        return partStartLba_ + firstDataSec_ + static_cast<uint64_t>(cluster - 2) * secPerClus_;
    }

    uint32_t GetFatEntry(uint32_t cluster) {
        if (!IsValidCluster(cluster)) return 0xFFFFFFFFu;

        if (fsKind_ == FsKind::FAT12) {
            uint32_t byteOff = cluster + (cluster / 2u);
            uint32_t fatSec  = byteOff / kSectorSize;
            uint32_t secOff  = byteOff % kSectorSize;
            if (fatSec >= fatSectors_) return 0x0FFFu;

            // La tabla FAT12 completa cabe en fatCache_ desde el sector 0
            if (fatCacheBaseSec_ != 0 || fatCacheSecCount_ == 0) {
                uint32_t toRead = std::min<uint32_t>(kFatCacheSectors, fatSectors_);
                if (!raw_.ReadSectors(fatStartLba_, toRead, fatCache_)) {
                    return 0x0FFFu;
                }
                fatCacheBaseSec_ = 0;
                fatCacheSecCount_ = toRead;
            }
            if (byteOff + 1 >= fatCacheSecCount_ * kSectorSize) return 0x0FFFu;
            uint16_t v16 = Rd16LE(fatCache_ + fatSec * kSectorSize + secOff);
            return (cluster & 1u) ? (v16 >> 4) : (v16 & 0x0FFFu);
        }

        uint32_t byteOff = (fsKind_ == FsKind::FAT16) ? (cluster * 2u) : (cluster * 4u);
        uint32_t fatSec  = byteOff / kSectorSize;
        uint32_t secOff  = byteOff % kSectorSize;
        if (fatSec >= fatSectors_) return 0xFFFFFFFFu;

        if (fatCacheBaseSec_ == UINT32_MAX ||
            fatSec < fatCacheBaseSec_ ||
            fatSec >= fatCacheBaseSec_ + fatCacheSecCount_) {
            uint32_t toRead = std::min<uint32_t>(kFatCacheSectors, fatSectors_ - fatSec);
            if (!raw_.ReadSectors(fatStartLba_ + fatSec, toRead, fatCache_)) {
                return 0xFFFFFFFFu;
            }
            fatCacheBaseSec_ = fatSec;
            fatCacheSecCount_ = toRead;
        }

        const uint8_t* ptr = fatCache_ + static_cast<size_t>(fatSec - fatCacheBaseSec_) * kSectorSize + secOff;
        if (fsKind_ == FsKind::FAT16) {
            return static_cast<uint32_t>(Rd16LE(ptr));
        }
        if (fsKind_ == FsKind::ExFAT) {
            return Rd32LE(ptr);
        }
        return Rd32LE(ptr) & 0x0FFFFFFFu;
    }

    uint32_t CountContiguousClusters(uint32_t startCluster, uint32_t maxClusters, uint32_t& outNextCluster) {
        uint32_t run = 1;
        uint32_t cur = startCluster;
        while (run < maxClusters) {
            uint32_t nxt = GetFatEntry(cur);
            if (nxt != cur + 1 || !IsValidCluster(nxt)) {
                outNextCluster = nxt;
                return run;
            }
            cur = nxt;
            ++run;
        }
        outNextCluster = GetFatEntry(cur);
        return run;
    }

    static uint8_t ComputeShortNameChecksum(const uint8_t* shortName11) {
        uint8_t sum = 0;
        for (int i = 0; i < 11; ++i) {
            sum = static_cast<uint8_t>(((sum & 1u) ? 0x80u : 0u) + (sum >> 1) + shortName11[i]);
        }
        return sum;
    }

    static std::string DecodeShortName(const uint8_t* e) {
        bool lowerBase = (e[12] & 0x08u) != 0;
        bool lowerExt  = (e[12] & 0x10u) != 0;

        int baseLen = 8;
        while (baseLen > 0 && e[baseLen - 1] == ' ') --baseLen;
        int extLen = 3;
        while (extLen > 0 && e[8 + extLen - 1] == ' ') --extLen;

        std::string out;
        out.reserve(13);
        for (int i = 0; i < baseLen; ++i) {
            char c = static_cast<char>(e[i]);
            if (lowerBase) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            out.push_back(c);
        }
        if (extLen > 0) {
            out.push_back('.');
            for (int i = 0; i < extLen; ++i) {
                char c = static_cast<char>(e[8 + i]);
                if (lowerExt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                out.push_back(c);
            }
        }
        return out;
    }

    bool ReadDirectoryBytes(uint32_t startCluster,
                            bool isFixedFatRoot,
                            bool exfatContiguous,
                            uint64_t dirByteLen,
                            std::vector<uint8_t>& outBuf) {
        outBuf.clear();
        if (isFixedFatRoot) {
            outBuf.resize(static_cast<size_t>(rootDirSectors_) * kSectorSize);
            return raw_.ReadSectors(rootDirLba_, rootDirSectors_, outBuf.data());
        }

        uint32_t clus = startCluster;
        uint32_t clusBytes = ClusterBytes();
        uint32_t maxClustersToRead = 256;
        if (exfatContiguous && dirByteLen > 0) {
            maxClustersToRead = static_cast<uint32_t>(
                std::min<uint64_t>(256, (dirByteLen + clusBytes - 1) / clusBytes));
        }

        uint32_t hops = 0;
        while (IsValidCluster(clus) && hops < maxClustersToRead) {
            // Para clusters gigantes en exFAT (ej. 1 MB - 4 MB), leemos en bloques de hasta 128 KB
            // y nos detenemos si encontramos fin de directorio (entryType == 0x00)
            uint32_t secDone = 0;
            bool hitEndMarker = false;
            while (secDone < secPerClus_) {
                uint32_t secChunk = std::min<uint32_t>(256u, secPerClus_ - secDone);
                uint32_t chunkBytes = secChunk * kSectorSize;
                size_t oldSz = outBuf.size();
                outBuf.resize(oldSz + chunkBytes);
                if (!raw_.ReadSectors(ClusterToLba(clus) + secDone, secChunk, outBuf.data() + oldSz)) {
                    return false;
                }
                for (size_t off = oldSz; off < outBuf.size(); off += 32) {
                    if (outBuf[off] == 0x00) {
                        hitEndMarker = true;
                        break;
                    }
                }
                secDone += secChunk;
                if (hitEndMarker || outBuf.size() >= 1024 * 1024) break;
            }
            if (hitEndMarker || outBuf.size() >= 1024 * 1024) break;

            ++hops;
            if (exfatContiguous) {
                if (hops >= maxClustersToRead) break;
                ++clus;
            } else {
                uint32_t nxt = GetFatEntry(clus);
                if (IsEndOfChain(nxt)) break;
                clus = nxt;
            }
        }
        return !outBuf.empty();
    }

    bool ListExFatDirectory(uint32_t startCluster,
                            bool exfatContiguous,
                            uint64_t dirByteLen,
                            std::vector<DirEntry>& outEntries) {
        outEntries.clear();
        std::vector<uint8_t> buf;
        if (!ReadDirectoryBytes(startCluster, false, exfatContiguous, dirByteLen, buf)) {
            return false;
        }

        size_t totalEnt = buf.size() / 32;
        for (size_t i = 0; i < totalEnt; ++i) {
            const uint8_t* e = buf.data() + i * 32;
            uint8_t etype = e[0];
            if (etype == 0x00) break;
            if (etype != 0x85) continue; // File Directory Entry activa

            uint8_t secCount = e[1];
            uint16_t fileAttr = Rd16LE(e + 4);
            if (secCount < 2 || i + secCount >= totalEnt) continue;

            const uint8_t* s = buf.data() + (i + 1) * 32;
            if (s[0] != 0xC0) continue; // Stream Extension Directory Entry

            uint8_t genFlags = s[1];
            uint8_t nameChars = s[3];
            uint64_t validDataLen = Rd64LE(s + 8);
            uint32_t firstClus = Rd32LE(s + 20);
            uint64_t dataLen = Rd64LE(s + 24);

            std::string name;
            name.reserve(nameChars);
            uint8_t charsLeft = nameChars;

            for (uint8_t fnIdx = 2; fnIdx <= secCount && charsLeft > 0; ++fnIdx) {
                const uint8_t* fn = buf.data() + (i + fnIdx) * 32;
                if (fn[0] != 0xC1) break; // File Name Directory Entry
                for (int k = 0; k < 15 && charsLeft > 0; ++k) {
                    uint16_t ch = Rd16LE(fn + 2 + k * 2);
                    if (ch == 0x0000) { charsLeft = 0; break; }
                    name.push_back(FoldUtf16ToAscii(ch));
                    --charsLeft;
                }
            }

            i += secCount;
            if (name.empty() || name == "." || name == "..") continue;

            DirEntry de;
            de.name = std::move(name);
            de.isDir = (fileAttr & 0x10u) != 0;
            de.startCluster = firstClus;
            de.exfatContiguous = (genFlags & 0x02u) != 0; // NoFatChain bit
            uint64_t effectiveLen = (validDataLen > 0) ? validDataLen : dataLen;
            de.size = de.isDir ? 0ULL : effectiveLen;
            de.dirByteLen = de.isDir ? dataLen : 0ULL;
            outEntries.push_back(std::move(de));
        }
        return true;
    }

    bool ListFatDirectory(uint32_t startCluster, bool isFixedFatRoot, std::vector<DirEntry>& outEntries) {
        outEntries.clear();
        std::vector<uint8_t> buf;
        if (!ReadDirectoryBytes(startCluster, isFixedFatRoot, false, 0, buf)) {
            return false;
        }

        std::string lfnParts[20];
        bool lfnActive = false;
        uint8_t lfnChecksum = 0;
        int lfnCount = 0;

        size_t totalEnt = buf.size() / 32;
        for (size_t i = 0; i < totalEnt; ++i) {
            const uint8_t* e = buf.data() + i * 32;
            if (e[0] == 0x00) break;
            if (e[0] == 0xE5) {
                lfnActive = false;
                continue;
            }

            uint8_t attr = e[11];
            if (attr == 0x0F) {
                uint8_t ord = e[0];
                int seq = static_cast<int>(ord & 0x1Fu);
                if (seq >= 1 && seq <= 20) {
                    if (ord & 0x40u) {
                        for (auto& p : lfnParts) p.clear();
                        lfnActive = true;
                        lfnChecksum = e[13];
                        lfnCount = seq;
                    } else if (!lfnActive || e[13] != lfnChecksum) {
                        lfnActive = false;
                        continue;
                    }

                    std::string seg;
                    const int offsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                    for (int k = 0; k < 13; ++k) {
                        uint16_t ch = Rd16LE(e + offsets[k]);
                        if (ch == 0x0000 || ch == 0xFFFF) break;
                        seg.push_back(FoldUtf16ToAscii(ch));
                    }
                    lfnParts[seq - 1] = std::move(seg);
                } else {
                    lfnActive = false;
                }
                continue;
            }

            if (attr & 0x08u) {
                lfnActive = false;
                continue;
            }

            std::string name;
            if (lfnActive && ComputeShortNameChecksum(e) == lfnChecksum) {
                for (int k = 0; k < lfnCount; ++k) {
                    name += lfnParts[k];
                }
            }
            lfnActive = false;

            if (name.empty()) {
                name = DecodeShortName(e);
            }
            if (name == "." || name == ".." || name.empty()) continue;

            DirEntry de;
            de.name = std::move(name);
            de.isDir = (attr & 0x10u) != 0;
            uint32_t hi = (fsKind_ == FsKind::FAT32) ? static_cast<uint32_t>(Rd16LE(e + 20)) : 0u;
            uint32_t lo = static_cast<uint32_t>(Rd16LE(e + 26));
            de.startCluster = (hi << 16) | lo;
            de.size = de.isDir ? 0ULL : static_cast<uint64_t>(Rd32LE(e + 28));
            outEntries.push_back(std::move(de));
        }

        return true;
    }

    bool ListRootDirectory(std::vector<DirEntry>& outEntries) {
        switch (fsKind_) {
            case FsKind::FAT12:
            case FsKind::FAT16:
                return ListFatDirectory(0, true, outEntries);
            case FsKind::FAT32:
                return ListFatDirectory(rootCluster_, false, outEntries);
            case FsKind::ExFAT:
                return ListExFatDirectory(rootCluster_, false, 0, outEntries);
            case FsKind::NTFS:
                return ListNtfsDirectory(5, outEntries); // MFT Record 5 = Root directory en NTFS
            default:
                return false;
        }
    }

    bool ListSubDirectory(const DirEntry& dir, std::vector<DirEntry>& outEntries) {
        switch (fsKind_) {
            case FsKind::FAT12:
            case FsKind::FAT16:
            case FsKind::FAT32:
                return ListFatDirectory(dir.startCluster, false, outEntries);
            case FsKind::ExFAT:
                return ListExFatDirectory(dir.startCluster, dir.exfatContiguous, dir.dirByteLen, outEntries);
            case FsKind::NTFS:
                return ListNtfsDirectory(dir.ntfsMftRec, outEntries);
            default:
                return false;
        }
    }

    void CollectFilesRecursive(const DirEntry& dir,
                               const std::string& subRel,
                               int depth,
                               std::vector<UsbFile>& outFiles,
                               uint64_t& outTotalBytes,
                               bool& outLooksLikeWup) {
        if (depth > 4) return;
        std::vector<DirEntry> entries;
        if (!ListSubDirectory(dir, entries)) return;

        for (const auto& e : entries) {
            if (e.name.empty() || e.name[0] == '.') continue;
            std::string rel = subRel.empty() ? e.name : (subRel + "/" + e.name);
            if (e.isDir) {
                CollectFilesRecursive(e, rel, depth + 1, outFiles, outTotalBytes, outLooksLikeWup);
            } else {
                if (EndsWithCI(e.name, ".tmd") || EndsWithCI(e.name, ".tik") || EndsWithCI(e.name, ".app")) {
                    outLooksLikeWup = true;
                }
                UsbFile uf;
                uf.subPath = std::move(rel);
                uf.startCluster = e.startCluster;
                uf.size = e.size;
                uf.exfatContiguous = e.exfatContiguous;
                uf.extents = e.extents;
                uf.residentData = e.residentData;
                outTotalBytes += uf.size;
                outFiles.push_back(std::move(uf));
            }
        }
    }

    FsaRawDevice raw_;
    bool mounted_ = false;
    FsKind fsKind_ = FsKind::None;

    uint64_t partStartLba_ = 0;
    uint32_t secPerClus_ = 1;
    uint64_t fatStartLba_ = 0;
    uint32_t fatSectors_ = 0;
    uint64_t rootDirLba_ = 0;
    uint32_t rootDirSectors_ = 0;
    uint32_t firstDataSec_ = 0;
    uint32_t rootCluster_ = 2;
    uint32_t clusterCount_ = 0;

    uint32_t mftRecordBytes_ = 1024;
    uint32_t ntfsIndexBlkBytes_ = 4096;
    std::vector<UsbExtent> mftExtents_;

    uint8_t* fatCache_ = nullptr;
    uint32_t fatCacheBaseSec_ = UINT32_MAX;
    uint32_t fatCacheSecCount_ = 0;
};

} // namespace usbfat
