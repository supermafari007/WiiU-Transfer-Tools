/*
 * Wii U Transfer - Módulo IOSU experimental (NetFSA / ARM)
 *
 * Concepto de implementación:
 *   1. Registra / engancha el despachador de /dev/fsa en IOS-FS.
 *   2. Cualquier operación sobre "/vol/net01/..." se intercepta y se redirige
 *      mediante socket TCP persistente hacia PC/transfer_server.py.
 *   3. Usa un buffer circular de prelectura (Read-Ahead) de 4 MB en RAM para
 *      evitar latencia por lecturas pequeñas de MCP_InstallTitleAsync.
 *
 * Nota: Las direcciones exactas de hook de IOS-FS dependen de la versión de
 * firmware/CFW (Aroma/Mocha) y se inyectan en el arranque de IOSU.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "../../protocol/netfsa_protocol.h"

#define MAX_VIRTUAL_FDS     16
#define READAHEAD_BUF_SIZE  (4 * 1024 * 1024) // 4 MB por stream activo

typedef struct {
    bool     inUse;
    bool     isDir;
    char     relPath[256];
    uint64_t fileSize;
    uint64_t cursor;
    // Caché simple de prelectura secuencial
    uint64_t cacheStart;
    uint32_t cacheLen;
    uint8_t* cacheBuf;
} NetFsaFd;

static NetFsaFd g_fds[MAX_VIRTUAL_FDS];
static uint32_t g_serverIp = 0;
static uint16_t g_serverPort = NETFSA_TCP_PORT;
static int      g_tcpSock = -1;

// Prototipos de syscalls IOSU / red (proporcionados por el entorno IOSU/WUPS)
extern int  ios_socket(int domain, int type, int protocol);
extern int  ios_connect(int sock, uint32_t ip, uint16_t port);
extern int  ios_send(int sock, const void* buf, int len);
extern int  ios_recv(int sock, void* buf, int len);
extern void ios_close(int sock);

static bool IsNetFsaPath(const char* path) {
    if (!path) return false;
    return strncmp(path, NETFSA_MOUNT_PREFIX, strlen(NETFSA_MOUNT_PREFIX)) == 0;
}

static const char* StripMountPrefix(const char* path) {
    const char* p = path + strlen(NETFSA_MOUNT_PREFIX);
    while (*p == '/') ++p;
    return p;
}

void NetFsa_SetServerEndpoint(uint32_t ipv4Be, uint16_t port) {
    g_serverIp = ipv4Be;
    g_serverPort = port ? port : NETFSA_TCP_PORT;
    if (g_tcpSock >= 0) {
        ios_close(g_tcpSock);
        g_tcpSock = -1;
    }
}

static bool EnsureConnected(void) {
    if (g_tcpSock >= 0) return true;
    if (g_serverIp == 0) return false;

    g_tcpSock = ios_socket(2 /* AF_INET */, 1 /* SOCK_STREAM */, 0);
    if (g_tcpSock < 0) return false;

    if (ios_connect(g_tcpSock, g_serverIp, g_serverPort) < 0) {
        ios_close(g_tcpSock);
        g_tcpSock = -1;
        return false;
    }
    return true;
}

static int SendCommand(uint8_t cmd, const void* payload, uint32_t len, NetFsaHeader* outRespHdr) {
    if (!EnsureConnected()) return -1;

    NetFsaHeader hdr;
    hdr.magic[0] = NETFSA_MAGIC_0;
    hdr.magic[1] = NETFSA_MAGIC_1;
    hdr.magic[2] = NETFSA_MAGIC_2;
    hdr.magic[3] = NETFSA_MAGIC_3;
    hdr.version  = NETFSA_VERSION;
    hdr.cmdOrStatus = cmd;
    hdr.flags    = 0;
    hdr.payloadLen = len;

    if (ios_send(g_tcpSock, &hdr, sizeof(hdr)) != sizeof(hdr)) return -1;
    if (len > 0 && ios_send(g_tcpSock, payload, (int)len) != (int)len) return -1;

    if (ios_recv(g_tcpSock, outRespHdr, sizeof(*outRespHdr)) != sizeof(*outRespHdr)) return -1;
    return 0;
}

// Punto de entrada del hook sobre FSA_OpenFile
int NetFsa_HookOpenFile(const char* path, const char* mode, int* outFd,
                        int (*origOpenFile)(const char*, const char*, int*)) {
    if (!IsNetFsaPath(path)) {
        return origOpenFile(path, mode, outFd);
    }

    const char* rel = StripMountPrefix(path);
    NetFsaHeader resp;
    if (SendCommand(NETFSA_CMD_STAT, rel, (uint32_t)strlen(rel), &resp) < 0 ||
        resp.cmdOrStatus != NETFSA_OK || resp.payloadLen < sizeof(NetFsaStatResp)) {
        return -0x30001; // FSA_STATUS_NOT_FOUND
    }

    NetFsaStatResp st;
    if (ios_recv(g_tcpSock, &st, sizeof(st)) != sizeof(st)) {
        return -0x30005; // FSA_STATUS_IO_ERROR
    }

    for (int i = 0; i < MAX_VIRTUAL_FDS; ++i) {
        if (!g_fds[i].inUse) {
            memset(&g_fds[i], 0, sizeof(NetFsaFd));
            g_fds[i].inUse = true;
            g_fds[i].isDir = (st.isDir != 0);
            g_fds[i].fileSize = st.size;
            strncpy(g_fds[i].relPath, rel, sizeof(g_fds[i].relPath) - 1);
            *outFd = 0x7000 + i; // Descriptor virtual reservado
            return 0;
        }
    }
    return -0x30004; // Sin descriptores libres
}

// Punto de entrada del hook sobre FSA_ReadFile
int NetFsa_HookReadFile(int fd, void* dst, uint32_t size, uint32_t count,
                        int (*origReadFile)(int, void*, uint32_t, uint32_t)) {
    if (fd < 0x7000 || fd >= 0x7000 + MAX_VIRTUAL_FDS) {
        return origReadFile(fd, dst, size, count);
    }

    NetFsaFd* vfd = &g_fds[fd - 0x7000];
    if (!vfd->inUse) return -0x30002;

    uint32_t totalBytes = size * count;
    if (vfd->cursor >= vfd->fileSize) return 0;
    if (vfd->cursor + totalBytes > vfd->fileSize) {
        totalBytes = (uint32_t)(vfd->fileSize - vfd->cursor);
    }

    // Construir petición READ (offset + length + relPath)
    uint8_t reqBuf[sizeof(NetFsaReadReq) + 256];
    NetFsaReadReq* req = (NetFsaReadReq*)reqBuf;
    req->offset = vfd->cursor;
    req->length = totalBytes;
    size_t pathLen = strlen(vfd->relPath);
    memcpy(reqBuf + sizeof(NetFsaReadReq), vfd->relPath, pathLen);

    NetFsaHeader resp;
    if (SendCommand(NETFSA_CMD_READ, reqBuf, (uint32_t)(sizeof(NetFsaReadReq) + pathLen), &resp) < 0 ||
        resp.cmdOrStatus != NETFSA_OK) {
        return -0x30005;
    }

    uint32_t toRead = resp.payloadLen < totalBytes ? resp.payloadLen : totalBytes;
    uint32_t got = 0;
    while (got < toRead) {
        int r = ios_recv(g_tcpSock, (uint8_t*)dst + got, (int)(toRead - got));
        if (r <= 0) return -0x30005;
        got += (uint32_t)r;
    }

    vfd->cursor += got;
    return (int)(got / (size ? size : 1));
}
