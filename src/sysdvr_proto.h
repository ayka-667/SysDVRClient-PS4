#pragma once
// Implementation client du protocole SysDVR "TCP Bridge" (protocole 02 / 03).
// Reference : https://github.com/exelix11/SysDVR/blob/master/protocol.md
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SDVR_PORT_VIDEO     9911
#define SDVR_PORT_AUDIO     9922
#define SDVR_PORT_DISCOVERY 19999

#define SDVR_PACKET_MAGIC   0xCCCCCCCCu
#define SDVR_REQUEST_MAGIC  0xAAAAAAAAu
#define SDVR_HANDSHAKE_OK   6

// Taille max du contenu d'un paquet : max(VbufSz, AbufSz * (1 + MaxABatching))
#define SDVR_VBUF_SIZE      0x54000
#define SDVR_ABUF_SIZE      0x1000
#define SDVR_MAX_PAYLOAD    SDVR_VBUF_SIZE

enum {
    SDVR_META_VIDEO    = 1 << 0,
    SDVR_META_AUDIO    = 1 << 1,
    SDVR_META_DATA     = 1 << 2,
    SDVR_META_REPLAY   = 1 << 3,
    SDVR_META_MULTINAL = 1 << 4,
    SDVR_META_ERROR    = 1 << 5,
};

#pragma pack(push, 1)
typedef struct {
    uint32_t Magic;
    uint32_t DataSize;
    uint64_t Timestamp; // microsecondes
    uint8_t  MetaData;
    uint8_t  ReplaySlot;
} SdvrPacketHeader;

typedef struct {
    uint32_t Magic;
    uint16_t ProtoVer;     // deux caracteres ASCII, ex: '0','2'
    uint8_t  MetaFlags;    // 1 = video, 2 = audio
    uint8_t  VideoFlags;   // 1 = NalHash, 2 = InjectPPSSPS, 4 = NalHashOnlyIDR
    uint8_t  AudioBatching;
    uint8_t  FeatureFlags; // 1 = eteindre l'ecran (proto 03)
    uint8_t  Reserved[6];
} SdvrHandshakeRequest;
#pragma pack(pop)

_Static_assert(sizeof(SdvrPacketHeader) == 18, "header size");
_Static_assert(sizeof(SdvrHandshakeRequest) == 16, "handshake size");

typedef struct {
    bool video;
    int  audio_batching;
    bool turn_off_screen;
} SdvrHandshakeOptions;

// SPS/PPS par defaut du flux de la Switch (copie du client officiel)
extern const uint8_t SDVR_SPS[20];
extern const uint8_t SDVR_PPS[8];

// Connexion TCP avec timeout. Retourne le fd ou -1.
int sdvr_connect(const char *ip, int port, int timeout_ms, char *err, size_t errlen);

// Effectue le handshake. Retourne 0 si OK.
int sdvr_handshake(int fd, const SdvrHandshakeOptions *opt, char *err, size_t errlen);

// Lit le prochain paquet. buf doit faire au moins SDVR_MAX_PAYLOAD octets.
// Retourne 1 si un paquet a ete lu, 0 si timeout (aucune donnee), -1 en cas d'erreur/deconnexion.
// *stop est verifie a chaque timeout pour permettre d'interrompre.
int sdvr_read_packet(int fd, SdvrPacketHeader *hdr, uint8_t *buf, volatile bool *stop);

// Ecoute les annonces UDP de la Switch. Retourne 1 si trouvee (ip/serial remplis), 0 sinon.
int sdvr_discover(int timeout_ms, char *ip, size_t iplen, char *serial, size_t seriallen);

// Formate un paquet d'erreur envoye par la console
void sdvr_format_error(const uint8_t *data, uint32_t size, char *out, size_t outlen);

void sdvr_close(int fd);
