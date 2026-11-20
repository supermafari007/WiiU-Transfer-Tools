#pragma once

/*
 * Wii U Transfer (Protocolo NFSA v1)
 * Cabeceras binarias compartidas entre PC/transfer_server.py y Wii U.
 */

#include <stdint.h>

#define NETFSA_MAGIC_0 'N'
#define NETFSA_MAGIC_1 'F'
#define NETFSA_MAGIC_2 'S'
#define NETFSA_MAGIC_3 'A'
#define NETFSA_VERSION 1

#define NETFSA_TCP_PORT 7331
#define NETFSA_UDP_PORT 7332
#define NETFSA_MOUNT_PREFIX "/vol/net01"

enum NetFsaCmd {
    NETFSA_CMD_LIST   = 0x01,
    NETFSA_CMD_STAT   = 0x02,
    NETFSA_CMD_READ   = 0x03,
    NETFSA_CMD_CLOSE  = 0x04,
    NETFSA_CMD_PING   = 0x05,
    NETFSA_CMD_STREAM = 0x06
};

enum NetFsaStatus {
    NETFSA_OK         = 0,
    NETFSA_ERR_BAD    = 1,
    NETFSA_ERR_NOENT  = 2,
    NETFSA_ERR_IO     = 3
};

#pragma pack(push, 1)
typedef struct {
    char     magic[4];    // "NFSA"
    uint8_t  version;     // 1
    uint8_t  cmdOrStatus; // Request: NetFsaCmd | Response: NetFsaStatus
    uint16_t flags;
    uint32_t payloadLen;  // Big-endian (Wii U ya es Big-Endian nativo)
} NetFsaHeader;

typedef struct {
    uint64_t offset;
    uint32_t length;
    // Seguido de ruta relativa UTF-8
} NetFsaReadReq;

typedef struct {
    uint8_t  isDir;
    uint64_t size;
} NetFsaStatResp;
#pragma pack(pop)
