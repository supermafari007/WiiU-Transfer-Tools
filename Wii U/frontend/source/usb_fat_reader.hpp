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
constexpr uint32_t kMaxRawSectors   = 256; // 128 KB por IOS_Ioctlv
constexpr uint32_t kFatCacheSectors = 128; // 64 KB de cache FAT en MEM2

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

struct UsbFile {
    std::string subPath;
    uint32_t startCluster = 0;
    uint64_t size = 0;
};

struct UsbPkg {
    std::string name;
    std::vector<UsbFile> files;
    uint64_t totalBytes = 0;
    bool marked = false;
};

// Acceso crudo de SOLO LECTURA a /dev/usb0X via Mocha FSA (nunca escribe en el dispositivo)
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
    };

    class FileStream {
    public:
        FileStream() = default;

        void Open(UsbFatVolume* vol, uint32_t startCluster, uint64_t size) {
            vol_ = vol;
            curCluster_ = startCluster;
            offsetInCluster_ = 0;
            remaining_ = size;
        }

        uint64_t Remaining() const { return remaining_; }

        // Lee hasta maxBytes en dst (si dst esta alineado a 0x40, usa DMA directo por bloques contiguos)
        bool ReadExact(void* dst, uint32_t len) {
            if (!vol_ || len > remaining_) return false;
            if (len == 0) return true;

            auto* out = static_cast<uint8_t*>(dst);
            uint32_t got = 0;
            const uint32_t clusBytes = vol_->ClusterBytes();

            while (got < len) {
                if (!vol_->IsValidCluster(curCluster_)) return false;

                uint32_t want = len - got;
                // Camino rapido: alineado al inicio de sector y al menos 1 sector completo
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

                // Camino cola/desalineado (< 512 B o offset parcial dentro del sector)
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
    };

    UsbFatVolume() = default;
    ~UsbFatVolume() { Shutdown(); }

    UsbFatVolume(const UsbFatVolume&) = delete;
    UsbFatVolume& operator=(const UsbFatVolume&) = delete;

    void Unmount() {
        raw_.Close();
        mounted_ = false;
        fatCacheBaseSec_ = UINT32_MAX;
        fatCacheSecCount_ = 0;
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
    uint32_t ClusterBytes() const { return secPerClus_ * kSectorSize; }

    bool ProbeAndMount(FSAClientHandle rawClient, std::string& outDevPath, std::string& outDiag) {
        Unmount();
        outDevPath.clear();
        outDiag.clear();

        if (rawClient < 0) {
            outDiag = "Mocha/Aroma no disponible.";
            return false;
        }
        if (!fatCache_) {
            fatCache_ = static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(kFatCacheSectors * kSectorSize, 0x40));
            if (!fatCache_) {
                outDiag = "Sin memoria para cache FAT.";
                return false;
            }
        }

        const char* candidates[] = {"/dev/usb01", "/dev/usb02", "/dev/usb03", "/dev/usb04"};
        bool sawNonFatPartition = false;
        std::string nonFatKind;

        for (const char* dev : candidates) {
            if (!raw_.Open(rawClient, dev)) continue;

            uint8_t sec0[kSectorSize]{};
            if (!raw_.ReadSectors(0, 1, sec0)) {
                raw_.Close();
                continue;
            }

            // 1. Superfloppy (BPB FAT directo en LBA 0)
            if (TryParseFatBpb(0, nonFatKind)) {
                mounted_ = true;
                outDevPath = dev;
                return true;
            }
            if (!nonFatKind.empty()) sawNonFatPartition = true;

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
                    if (TryParseFatBpb(firstLba, nonFatKind)) {
                        mounted_ = true;
                        outDevPath = dev;
                        return true;
                    }
                    if (!nonFatKind.empty()) sawNonFatPartition = true;
                }

                // 3. GPT (si se detecto MBR protector 0xEE)
                if (isGpt && ProbeGptPartitions(nonFatKind)) {
                    mounted_ = true;
                    outDevPath = dev;
                    return true;
                }
                if (!nonFatKind.empty()) sawNonFatPartition = true;
            }

            raw_.Close();
        }

        if (sawNonFatPartition) {
            outDiag = "USB en " + nonFatKind + ": formatea a FAT32.";
        } else {
            outDiag = "Sin pendrive FAT32. Conecta USB frontal y [Y].";
        }
        return false;
    }

    bool ScanWupPackages(std::vector<UsbPkg>& outPkgs) {
        outPkgs.clear();
        if (!mounted_) return false;

        std::vector<DirEntry> rootEntries;
        if (!ListDirectory(isFat32_ ? rootCluster_ : 0, !isFat32_, rootEntries)) {
            return false;
        }

        auto scanContainerDir = [&](const std::vector<DirEntry>& entries) {
            for (const auto& e : entries) {
                if (!e.isDir || e.name.empty() || e.name[0] == '.') continue;
                if (EqualsCI(e.name, "System Volume Information") ||
                    EqualsCI(e.name, "$RECYCLE.BIN") ||
                    EqualsCI(e.name, "wiiu") ||
                    EqualsCI(e.name, "install") ||
                    EqualsCI(e.name, "packages")) {
                    continue;
                }

                UsbPkg pkg;
                pkg.name = e.name;
                bool looksLikeWup = false;
                CollectFilesRecursive(e.startCluster, "", 0, pkg.files, pkg.totalBytes, looksLikeWup);
                if (looksLikeWup && !pkg.files.empty()) {
                    outPkgs.push_back(std::move(pkg));
                }
            }
        };

        // 1. Carpetas WUP en la raiz del pendrive
        scanContainerDir(rootEntries);

        // 2. Carpetas WUP dentro de /install o /packages si existen
        for (const auto& e : rootEntries) {
            if (e.isDir && (EqualsCI(e.name, "install") || EqualsCI(e.name, "packages"))) {
                std::vector<DirEntry> subEntries;
                if (ListDirectory(e.startCluster, false, subEntries)) {
                    scanContainerDir(subEntries);
                }
            }
        }
        return true;
    }

private:
    bool ProbeGptPartitions(std::string& outNonFatKind) {
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

            if (TryParseFatBpb(firstLba, outNonFatKind)) {
                return true;
            }
        }
        return false;
    }

    bool TryParseFatBpb(uint64_t partLba, std::string& outNonFatKind) {
        uint8_t bpb[kSectorSize]{};
        if (!raw_.ReadSectors(partLba, 1, bpb)) return false;
        if (bpb[510] != 0x55 || bpb[511] != 0xAA) return false;

        if (std::memcmp(bpb + 3, "EXFAT   ", 8) == 0) {
            outNonFatKind = "exFAT";
            return false;
        }
        if (std::memcmp(bpb + 3, "NTFS    ", 8) == 0) {
            outNonFatKind = "NTFS";
            return false;
        }

        uint16_t bytesPerSec = Rd16LE(bpb + 11);
        uint8_t secPerClus   = bpb[13];
        uint16_t rsvdSecCnt  = Rd16LE(bpb + 14);
        uint8_t numFats      = bpb[16];
        uint16_t rootEntCnt  = Rd16LE(bpb + 17);
        uint16_t totSec16    = Rd16LE(bpb + 19);
        uint16_t fatSz16     = Rd16LE(bpb + 22);
        uint32_t totSec32    = Rd32LE(bpb + 32);
        uint32_t fatSz32     = Rd32LE(bpb + 36);
        uint32_t rootClus    = Rd32LE(bpb + 44);

        if (bytesPerSec != kSectorSize) return false;
        if (secPerClus == 0 || (secPerClus & (secPerClus - 1)) != 0 || secPerClus > 128) return false;
        if (rsvdSecCnt == 0 || numFats == 0 || numFats > 2) return false;

        uint32_t totSec = totSec16 ? static_cast<uint32_t>(totSec16) : totSec32;
        uint32_t fatSz  = fatSz16 ? static_cast<uint32_t>(fatSz16) : fatSz32;
        if (totSec == 0 || fatSz == 0) return false;

        uint32_t rootDirSectors = ((static_cast<uint32_t>(rootEntCnt) * 32u) + (kSectorSize - 1)) / kSectorSize;
        uint32_t firstDataSec   = static_cast<uint32_t>(rsvdSecCnt) + static_cast<uint32_t>(numFats) * fatSz + rootDirSectors;
        if (firstDataSec >= totSec) return false;

        uint32_t dataSec = totSec - firstDataSec;
        uint32_t clusCnt = dataSec / secPerClus;

        if (clusCnt < 4085) return false; // FAT12 no soportado
        bool fat32 = (clusCnt >= 65525);
        if (fat32) {
            if (fatSz16 != 0 || rootClus < 2) return false;
        } else {
            if (fatSz16 == 0 || rootDirSectors == 0) return false;
        }

        partStartLba_   = partLba;
        secPerClus_     = secPerClus;
        fatStartLba_    = partLba + rsvdSecCnt;
        fatSectors_     = fatSz;
        rootDirLba_     = partLba + rsvdSecCnt + static_cast<uint64_t>(numFats) * fatSz;
        rootDirSectors_ = rootDirSectors;
        firstDataSec_   = firstDataSec;
        rootCluster_    = fat32 ? rootClus : 0;
        clusterCount_   = clusCnt;
        isFat32_        = fat32;
        fatCacheBaseSec_ = UINT32_MAX;
        fatCacheSecCount_ = 0;
        return true;
    }

    bool IsValidCluster(uint32_t c) const {
        return c >= 2 && c < (clusterCount_ + 2);
    }

    bool IsEndOfChain(uint32_t val) const {
        return isFat32_ ? ((val & 0x0FFFFFFFu) >= 0x0FFFFFF8u) : (val >= 0xFFF8u);
    }

    uint64_t ClusterToLba(uint32_t cluster) const {
        return partStartLba_ + firstDataSec_ + static_cast<uint64_t>(cluster - 2) * secPerClus_;
    }

    uint32_t GetFatEntry(uint32_t cluster) {
        if (!IsValidCluster(cluster)) return 0x0FFFFFFFu;
        uint32_t byteOff = isFat32_ ? (cluster * 4u) : (cluster * 2u);
        uint32_t fatSec  = byteOff / kSectorSize;
        uint32_t secOff  = byteOff % kSectorSize;
        if (fatSec >= fatSectors_) return 0x0FFFFFFFu;

        if (fatCacheBaseSec_ == UINT32_MAX ||
            fatSec < fatCacheBaseSec_ ||
            fatSec >= fatCacheBaseSec_ + fatCacheSecCount_) {
            uint32_t toRead = std::min<uint32_t>(kFatCacheSectors, fatSectors_ - fatSec);
            if (!raw_.ReadSectors(fatStartLba_ + fatSec, toRead, fatCache_)) {
                return 0x0FFFFFFFu;
            }
            fatCacheBaseSec_ = fatSec;
            fatCacheSecCount_ = toRead;
        }

        const uint8_t* ptr = fatCache_ + static_cast<size_t>(fatSec - fatCacheBaseSec_) * kSectorSize + secOff;
        return isFat32_ ? (Rd32LE(ptr) & 0x0FFFFFFFu) : static_cast<uint32_t>(Rd16LE(ptr));
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

    bool ListDirectory(uint32_t startCluster, bool isFat16Root, std::vector<DirEntry>& outEntries) {
        outEntries.clear();

        std::vector<uint8_t> buf;
        if (isFat16Root) {
            buf.resize(static_cast<size_t>(rootDirSectors_) * kSectorSize);
            if (!raw_.ReadSectors(rootDirLba_, rootDirSectors_, buf.data())) return false;
        } else {
            uint32_t clus = startCluster;
            uint32_t clusBytes = ClusterBytes();
            uint32_t hops = 0;
            while (IsValidCluster(clus) && hops < 256) {
                size_t oldSz = buf.size();
                buf.resize(oldSz + clusBytes);
                if (!raw_.ReadSectors(ClusterToLba(clus), secPerClus_, buf.data() + oldSz)) {
                    return false;
                }
                uint32_t nxt = GetFatEntry(clus);
                if (IsEndOfChain(nxt)) break;
                clus = nxt;
                ++hops;
            }
        }

        // Reconstruccion VFAT LFN (hasta 20 segmentos de 13 caracteres UTF-16LE)
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

            if (attr & 0x08u) { // Volume ID
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
            uint32_t hi = isFat32_ ? static_cast<uint32_t>(Rd16LE(e + 20)) : 0u;
            uint32_t lo = static_cast<uint32_t>(Rd16LE(e + 26));
            de.startCluster = (hi << 16) | lo;
            de.size = de.isDir ? 0ULL : static_cast<uint64_t>(Rd32LE(e + 28));
            outEntries.push_back(std::move(de));
        }

        return true;
    }

    void CollectFilesRecursive(uint32_t dirCluster,
                               const std::string& subRel,
                               int depth,
                               std::vector<UsbFile>& outFiles,
                               uint64_t& outTotalBytes,
                               bool& outLooksLikeWup) {
        if (depth > 4) return;
        std::vector<DirEntry> entries;
        if (!ListDirectory(dirCluster, false, entries)) return;

        for (const auto& e : entries) {
            if (e.name.empty() || e.name[0] == '.') continue;
            std::string rel = subRel.empty() ? e.name : (subRel + "/" + e.name);
            if (e.isDir) {
                CollectFilesRecursive(e.startCluster, rel, depth + 1, outFiles, outTotalBytes, outLooksLikeWup);
            } else {
                if (EndsWithCI(e.name, ".tmd") || EndsWithCI(e.name, ".tik") || EndsWithCI(e.name, ".app")) {
                    outLooksLikeWup = true;
                }
                UsbFile uf;
                uf.subPath = std::move(rel);
                uf.startCluster = e.startCluster;
                uf.size = e.size;
                outTotalBytes += uf.size;
                outFiles.push_back(std::move(uf));
            }
        }
    }

    FsaRawDevice raw_;
    bool mounted_ = false;
    bool isFat32_ = true;

    uint64_t partStartLba_ = 0;
    uint32_t secPerClus_ = 1;
    uint64_t fatStartLba_ = 0;
    uint32_t fatSectors_ = 0;
    uint64_t rootDirLba_ = 0;
    uint32_t rootDirSectors_ = 0;
    uint32_t firstDataSec_ = 0;
    uint32_t rootCluster_ = 2;
    uint32_t clusterCount_ = 0;

    uint8_t* fatCache_ = nullptr;
    uint32_t fatCacheBaseSec_ = UINT32_MAX;
    uint32_t fatCacheSecCount_ = 0;
};

} // namespace usbfat
