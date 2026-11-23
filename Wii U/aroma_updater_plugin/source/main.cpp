#include <wups.h>

#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/title.h>
#include <nn/ac.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

WUPS_PLUGIN_NAME("Wii U Transfer Updater");
WUPS_PLUGIN_DESCRIPTION("Actualiza WiiU_Transfer.wuhb en segundo plano desde GitHub, Android o PC cuando la app no esta en uso");
WUPS_PLUGIN_VERSION("v1.0.0");
WUPS_PLUGIN_AUTHOR("El Diuca");
WUPS_PLUGIN_LICENSE("MIT");

WUPS_USE_WUT_DEVOPTAB();

namespace {

constexpr const char* kVersionJsonUrl =
    "https://raw.githubusercontent.com/supermafari007/WiiU-Transfer-Tools/main/version.json";
constexpr const char* kDefaultWuhbUrl =
    "https://github.com/supermafari007/WiiU-Transfer-Tools/releases/download/v2.0.3/WiiU_Transfer.wuhb";

constexpr const char* kAppsDir      = "fs:/vol/external01/wiiu/apps";
constexpr const char* kStageDir     = "fs:/vol/external01/wiiu/apps/.wiiu_transfer_stage";
constexpr const char* kStageWuhb    = "fs:/vol/external01/wiiu/apps/.wiiu_transfer_stage/WiiU_Transfer.wuhb";
constexpr const char* kStageDlTmp   = "fs:/vol/external01/wiiu/apps/.wiiu_transfer_stage/WiiU_Transfer.dl";
constexpr const char* kWuhbPath     = "fs:/vol/external01/wiiu/apps/WiiU_Transfer.wuhb";
constexpr const char* kWuhbRoot     = "fs:/vol/external01/WiiU_Transfer.wuhb";
constexpr const char* kWuhbOldDup   = "fs:/vol/external01/wiiu/apps/WiiU_Transfer_Update.wuhb";
constexpr const char* kVerFilePath  = "fs:/vol/external01/wiiu/apps/.wiiu_transfer_stage/version.txt";
constexpr const char* kLogFilePath  = "fs:/vol/external01/wiiu/apps/.wiiu_transfer_stage/updater.log";

constexpr uint16_t kPushPort        = 7333;
constexpr uint32_t kWorkerStackSize = 64 * 1024;

std::atomic<bool> gAbort{false};
std::atomic<bool> gWorkerRunning{false};
bool gCheckedGitHubThisBoot = false;
bool gThreadCreated = false;
OSThread* gWorkerThread = nullptr;
uint8_t* gWorkerStack = nullptr;

bool IsWiiUMenu() {
    uint64_t tid = OSGetTitleID();
    return tid == 0x0005001010040000ULL || // Wii U Menu JPN
           tid == 0x0005001010040100ULL || // Wii U Menu USA
           tid == 0x0005001010040200ULL;   // Wii U Menu EUR
}

void EnsureStageDir() {
    mkdir("fs:/vol/external01/wiiu", 0777);
    mkdir(kAppsDir, 0777);
    mkdir(kStageDir, 0777);
}

void WriteLog(const char* msg) {
    EnsureStageDir();
    FILE* fp = std::fopen(kLogFilePath, "wb");
    if (!fp) return;
    std::fprintf(fp, "%s\n", msg);
    std::fclose(fp);
}

bool HasValidWuhbHeader(const char* path, long* outSize = nullptr) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) return false;
    uint8_t hdr[4]{};
    size_t n = std::fread(hdr, 1, 4, fp);
    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    std::fclose(fp);
    if (outSize) *outSize = sz;
    return (n == 4 && hdr[0] == 'W' && hdr[1] == 'U' && hdr[2] == 'H' && hdr[3] == 'B' &&
            sz >= 64 * 1024 && sz <= 32 * 1024 * 1024);
}

bool CopyFileRaw(const char* srcPath, const char* dstPath) {
    FILE* src = std::fopen(srcPath, "rb");
    FILE* dst = std::fopen(dstPath, "wb");
    if (!src || !dst) {
        if (src) std::fclose(src);
        if (dst) std::fclose(dst);
        return false;
    }
    uint8_t buf[32 * 1024];
    bool ok = true;
    while (!gAbort.load()) {
        size_t r = std::fread(buf, 1, sizeof(buf), src);
        if (r == 0) break;
        if (std::fwrite(buf, 1, r, dst) != r) {
            ok = false;
            break;
        }
    }
    std::fclose(dst);
    std::fclose(src);
    return ok && !gAbort.load();
}

bool ReplaceWuhbSafely(const char* newFilePath) {
    if (!HasValidWuhbHeader(newFilePath)) return false;
    std::remove(kWuhbPath);
    if (std::rename(newFilePath, kWuhbPath) == 0) {
        return true;
    }
    if (CopyFileRaw(newFilePath, kWuhbPath)) {
        std::remove(newFilePath);
        return true;
    }
    return false;
}

// Reemplaza WiiU_Transfer.wuhb si existe una actualizacion en la carpeta transitoria
// (enviada desde Android o PC mientras la app estaba abierta)
bool ApplyStagedUpdates() {
    std::remove(kWuhbOldDup);
    bool applied = false;
    if (HasValidWuhbHeader(kStageWuhb)) {
        if (ReplaceWuhbSafely(kStageWuhb)) {
            WriteLog("Actualizacion aplicada desde carpeta transitoria (.wiiu_transfer_stage).");
            applied = true;
        }
    }
    if (HasValidWuhbHeader(kWuhbRoot)) {
        if (ReplaceWuhbSafely(kWuhbRoot)) {
            WriteLog("Actualizacion aplicada desde SD:/WiiU_Transfer.wuhb.");
            applied = true;
        }
    }
    return applied;
}

void ParseSemVer(const std::string& s, int& maj, int& min, int& pat) {
    maj = 0; min = 0; pat = 0;
    const char* p = s.c_str();
    while (*p && !std::isdigit(static_cast<unsigned char>(*p))) ++p;
    if (*p) maj = std::strtol(p, const_cast<char**>(&p), 10);
    if (*p == '.') { ++p; min = std::strtol(p, const_cast<char**>(&p), 10); }
    if (*p == '.') { ++p; pat = std::strtol(p, const_cast<char**>(&p), 10); }
}

bool IsRemoteNewer(const std::string& localVer, const std::string& remoteVer) {
    int l1, l2, l3, r1, r2, r3;
    ParseSemVer(localVer, l1, l2, l3);
    ParseSemVer(remoteVer, r1, r2, r3);
    if (r1 != l1) return r1 > l1;
    if (r2 != l2) return r2 > l2;
    return r3 > l3;
}

std::string ExtractJsonString(const std::string& json, const char* key) {
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    ++pos;
    size_t end = json.find('"', pos);
    if (end == std::string::npos) return {};
    return json.substr(pos, end - pos);
}

void ReadLocalStamp(std::string& outVer, long& outSize) {
    outVer.clear();
    outSize = 0;
    FILE* fp = std::fopen(kVerFilePath, "rb");
    if (!fp) return;
    char buf[64]{};
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, fp);
    std::fclose(fp);
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) {
        s.pop_back();
    }
    size_t bar = s.find('|');
    if (bar != std::string::npos) {
        outVer = s.substr(0, bar);
        outSize = std::strtol(s.substr(bar + 1).c_str(), nullptr, 10);
    } else {
        outVer = s;
    }
}

void WriteLocalStamp(const std::string& ver, long fileSize) {
    EnsureStageDir();
    FILE* fp = std::fopen(kVerFilePath, "wb");
    if (!fp) return;
    std::fprintf(fp, "%s|%ld\n", ver.c_str(), fileSize);
    std::fclose(fp);
}

int CurlProgressCb(void* /*clientp*/, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/,
                   curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    return gAbort.load() ? 1 : 0;
}

long FetchRemoteFileSizeHead(const std::string& url) {
    CURL* h = curl_easy_init();
    if (!h) return -1;
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_USERAGENT, "WiiU-Transfer-Updater-Plugin/1.0");
    curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(h, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, CurlProgressCb);

    CURLcode res = curl_easy_perform(h);
    long httpCode = 0;
    curl_off_t cl = -1;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_getinfo(h, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
    curl_easy_cleanup(h);

    if (res == CURLE_OK && httpCode == 200 && cl > 0) {
        return static_cast<long>(cl);
    }
    return -1;
}

size_t CurlWriteStringCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    if (gAbort.load()) return 0;
    auto* out = static_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    if (out->size() + total > 32 * 1024) return 0;
    out->append(ptr, total);
    return total;
}

size_t CurlWriteFileCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    if (gAbort.load()) return 0;
    auto* fp = static_cast<FILE*>(userdata);
    return std::fwrite(ptr, size, nmemb, fp);
}

bool SleepAbortableMs(int totalMs) {
    int elapsed = 0;
    while (elapsed < totalMs) {
        if (gAbort.load()) return false;
        OSSleepTicks(OSMillisecondsToTicks(100));
        elapsed += 100;
    }
    return !gAbort.load();
}

bool RecvExactAbortable(int sock, void* dst, uint32_t len, int timeoutMs) {
    auto* out = static_cast<uint8_t*>(dst);
    uint32_t got = 0;
    int waited = 0;
    while (got < len && !gAbort.load()) {
        struct pollfd pfd{sock, POLLIN, 0};
        int pr = poll(&pfd, 1, 100);
        if (pr > 0) {
            ssize_t n = recv(sock, out + got, len - got, 0);
            if (n <= 0) return false;
            got += static_cast<uint32_t>(n);
            waited = 0;
        } else if (pr == 0) {
            waited += 100;
            if (waited >= timeoutMs) return false;
        } else {
            return false;
        }
    }
    return (got == len) && !gAbort.load();
}

void RunGitHubUpdateCheck() {
    std::string jsonBody;
    CURL* curl = curl_easy_init();
    if (!curl) return;

    curl_easy_setopt(curl, CURLOPT_URL, kVersionJsonUrl);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "WiiU-Transfer-Updater-Plugin/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteStringCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &jsonBody);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlProgressCb);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || httpCode != 200 || jsonBody.empty() || gAbort.load()) {
        return;
    }

    std::string remoteVer = ExtractJsonString(jsonBody, "version");
    std::string wuhbUrl   = ExtractJsonString(jsonBody, "wiiu_wuhb_url");
    if (remoteVer.empty()) return;
    if (wuhbUrl.empty()) wuhbUrl = kDefaultWuhbUrl;

    long localFileSize = 0;
    bool wuhbExists = HasValidWuhbHeader(kWuhbPath, &localFileSize);
    std::string localVer;
    long stampedSize = 0;
    ReadLocalStamp(localVer, stampedSize);

    bool needDownload = false;
    if (!wuhbExists) {
        needDownload = true;
    } else if (localVer.empty() || stampedSize <= 0 || localFileSize != stampedSize) {
        // Primera vez con el plugin o el usuario copio una version distinta (ej. v1.0.0 / v2.0.0 / v2.0.2):
        // comprobar el tamano exacto del .wuhb publicado en GitHub mediante HTTP HEAD
        long remoteSize = FetchRemoteFileSizeHead(wuhbUrl);
        if (gAbort.load()) return;
        if (remoteSize > 0 && localFileSize == remoteSize) {
            WriteLocalStamp(remoteVer, localFileSize);
            std::string okMsg = "Verificado por tamano: ya tienes v" + remoteVer;
            WriteLog(okMsg.c_str());
            return;
        }
        // Si el tamano difiere (ej. v1.0.0 vs v2.0.3) o no pudo verificarse, actualizar automaticamente
        needDownload = true;
    } else if (IsRemoteNewer(localVer, remoteVer)) {
        needDownload = true;
    }

    if (!needDownload) {
        std::string okMsg = "GitHub al dia: local=" + localVer + " remote=" + remoteVer;
        WriteLog(okMsg.c_str());
        return;
    }

    EnsureStageDir();
    std::remove(kStageDlTmp);
    FILE* dlFp = std::fopen(kStageDlTmp, "wb");
    if (!dlFp) return;

    CURL* dl = curl_easy_init();
    if (!dl) {
        std::fclose(dlFp);
        std::remove(kStageDlTmp);
        return;
    }

    curl_easy_setopt(dl, CURLOPT_URL, wuhbUrl.c_str());
    curl_easy_setopt(dl, CURLOPT_USERAGENT, "WiiU-Transfer-Updater-Plugin/1.0");
    curl_easy_setopt(dl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(dl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(dl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(dl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(dl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(dl, CURLOPT_WRITEFUNCTION, CurlWriteFileCb);
    curl_easy_setopt(dl, CURLOPT_WRITEDATA, dlFp);
    curl_easy_setopt(dl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(dl, CURLOPT_XFERINFOFUNCTION, CurlProgressCb);

    CURLcode dlRes = curl_easy_perform(dl);
    long dlHttp = 0;
    curl_easy_getinfo(dl, CURLINFO_RESPONSE_CODE, &dlHttp);
    curl_easy_cleanup(dl);
    std::fclose(dlFp);

    long newSize = 0;
    if (dlRes == CURLE_OK && dlHttp == 200 && !gAbort.load() &&
        HasValidWuhbHeader(kStageDlTmp, &newSize) && ReplaceWuhbSafely(kStageDlTmp)) {
        WriteLocalStamp(remoteVer, newSize);
        std::string msg = "Actualizado WiiU_Transfer.wuhb a v" + remoteVer + " desde GitHub.";
        WriteLog(msg.c_str());
    } else {
        std::remove(kStageDlTmp);
    }
}

// Escucha pasivamente en TCP/UDP 7333 mientras el usuario esta en el Menu Wii U
// para recibir e instalar actualizaciones .wuhb enviadas desde Android o PC
void RunMenuPushListenerLoop() {
    int udpSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udpSock >= 0) {
        int yes = 1;
        setsockopt(udpSock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kPushPort);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(udpSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(udpSock);
            udpSock = -1;
        }
    }

    int tcpSock = socket(AF_INET, SOCK_STREAM, 0);
    if (tcpSock >= 0) {
        int yes = 1;
        setsockopt(tcpSock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kPushPort);
        addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(tcpSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            listen(tcpSock, 2) < 0) {
            close(tcpSock);
            tcpSock = -1;
        }
    }

    if (udpSock < 0 && tcpSock < 0) return;

    while (!gAbort.load()) {
        struct pollfd pfds[2]{};
        int nfds = 0;
        int udpIdx = -1, tcpIdx = -1;
        if (udpSock >= 0) { udpIdx = nfds; pfds[nfds++] = {udpSock, POLLIN, 0}; }
        if (tcpSock >= 0) { tcpIdx = nfds; pfds[nfds++] = {tcpSock, POLLIN, 0}; }

        int pr = poll(pfds, nfds, 200);
        if (pr <= 0) continue;

        if (udpIdx >= 0 && (pfds[udpIdx].revents & POLLIN)) {
            char buf[64]{};
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            ssize_t n = recvfrom(udpSock, buf, sizeof(buf) - 1, 0,
                                 reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n > 0 && std::strncmp(buf, "WIIU_FIND_CONSOLE", 17) == 0) {
                const char* reply = "WIIU_CONSOLE_HERE|PLUGIN";
                sendto(udpSock, reply, std::strlen(reply), 0,
                       reinterpret_cast<sockaddr*>(&from), fromLen);
            }
        }

        if (tcpIdx >= 0 && (pfds[tcpIdx].revents & POLLIN)) {
            sockaddr_in cliAddr{};
            socklen_t cliLen = sizeof(cliAddr);
            int cli = accept(tcpSock, reinterpret_cast<sockaddr*>(&cliAddr), &cliLen);
            if (cli >= 0) {
                uint8_t hdr[8]{};
                if (RecvExactAbortable(cli, hdr, 8, 5000) && std::memcmp(hdr, "WUPU", 4) == 0) {
                    uint32_t sz = static_cast<uint32_t>(hdr[4]) |
                                  (static_cast<uint32_t>(hdr[5]) << 8) |
                                  (static_cast<uint32_t>(hdr[6]) << 16) |
                                  (static_cast<uint32_t>(hdr[7]) << 24);
                    if (sz >= 64 * 1024 && sz <= 32 * 1024 * 1024) {
                        EnsureStageDir();
                        std::remove(kStageDlTmp);
                        FILE* fp = std::fopen(kStageDlTmp, "wb");
                        bool ok = (fp != nullptr);
                        uint8_t chunkBuf[16 * 1024];
                        uint32_t gotTotal = 0;
                        while (ok && gotTotal < sz && !gAbort.load()) {
                            uint32_t want = std::min<uint32_t>(sizeof(chunkBuf), sz - gotTotal);
                            if (!RecvExactAbortable(cli, chunkBuf, want, 10000) ||
                                std::fwrite(chunkBuf, 1, want, fp) != want) {
                                ok = false;
                                break;
                            }
                            gotTotal += want;
                        }
                        if (fp) std::fclose(fp);

                        if (ok && gotTotal == sz && ReplaceWuhbSafely(kStageDlTmp)) {
                            const char* okMsg = "OK\n";
                            send(cli, okMsg, std::strlen(okMsg), 0);
                            WriteLog("Actualizacion .wuhb recibida por Red (Android/PC) y aplicada en /wiiu/apps/.");
                        } else {
                            std::remove(kStageDlTmp);
                            const char* errMsg = "ERR_WRITE\n";
                            send(cli, errMsg, std::strlen(errMsg), 0);
                        }
                    }
                }
                close(cli);
            }
        }
    }

    if (udpSock >= 0) close(udpSock);
    if (tcpSock >= 0) close(tcpSock);
}

int UpdaterWorkerEntry(int /*argc*/, void* /*argv*/) {
    // 1. Esperar 5 segundos a que cargue el Menu Wii U y la red
    if (!SleepAbortableMs(5000)) {
        gWorkerRunning.store(false);
        return 0;
    }

    // 2. Comprobar red activa
    uint32_t myIp = 0;
    bool hasNet = false;
    for (int tries = 0; tries < 5 && !gAbort.load(); ++tries) {
        if (NNResult_IsSuccess(ACInitialize())) {
            if (NNResult_IsSuccess(ACGetAssignedAddress(&myIp)) && myIp != 0) {
                hasNet = true;
            }
            ACFinalize();
        }
        if (hasNet) break;
        if (!SleepAbortableMs(2000)) {
            gWorkerRunning.store(false);
            return 0;
        }
    }

    if (!hasNet || gAbort.load()) {
        gWorkerRunning.store(false);
        return 0;
    }

    // 3. Una vez por arranque, comprobar actualizaciones en GitHub
    if (!gCheckedGitHubThisBoot) {
        gCheckedGitHubThisBoot = true;
        RunGitHubUpdateCheck();
    }

    // 4. Quedarse escuchando en segundo plano (puerto 7333) por si Android o PC envian un .wuhb
    if (!gAbort.load()) {
        RunMenuPushListenerLoop();
    }

    gWorkerRunning.store(false);
    return 0;
}

void StopWorkerCleanly() {
    gAbort.store(true);
    if (gThreadCreated && gWorkerThread) {
        int res = 0;
        OSJoinThread(gWorkerThread, &res);
        gThreadCreated = false;
    }
    if (gWorkerThread) {
        MEMFreeToDefaultHeap(gWorkerThread);
        gWorkerThread = nullptr;
    }
    if (gWorkerStack) {
        MEMFreeToDefaultHeap(gWorkerStack);
        gWorkerStack = nullptr;
    }
    gWorkerRunning.store(false);
}

} // namespace

INITIALIZE_PLUGIN() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    gCheckedGitHubThisBoot = false;
}

DEINITIALIZE_PLUGIN() {
    StopWorkerCleanly();
    curl_global_cleanup();
}

ON_APPLICATION_START() {
    if (!IsWiiUMenu()) {
        return;
    }

    // 1. Siempre aplicar de inmediato cualquier .wuhb guardado en la carpeta transitoria
    //    (enviado por Android USB/Wi-Fi o PC mientras WiiU_Transfer.wuhb estaba abierta)
    ApplyStagedUpdates();

    // 2. Iniciar hilo en segundo plano (chequeo GitHub 1 vez por arranque + receptor puerto 7333 en Menu)
    if (gWorkerRunning.load()) {
        return;
    }
    StopWorkerCleanly();

    gWorkerThread = static_cast<OSThread*>(MEMAllocFromDefaultHeapEx(sizeof(OSThread), 0x20));
    gWorkerStack  = static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(kWorkerStackSize, 0x20));
    if (!gWorkerThread || !gWorkerStack) {
        StopWorkerCleanly();
        return;
    }

    gAbort.store(false);
    gWorkerRunning.store(true);
    if (OSCreateThread(gWorkerThread, UpdaterWorkerEntry, 0, nullptr,
                       gWorkerStack + kWorkerStackSize, kWorkerStackSize,
                       24, OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
        gThreadCreated = true;
        OSResumeThread(gWorkerThread);
    } else {
        StopWorkerCleanly();
    }
}

ON_APPLICATION_ENDS() {
    StopWorkerCleanly();
}
