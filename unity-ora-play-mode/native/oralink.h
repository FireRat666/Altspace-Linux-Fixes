// The Unity <-> Ora browser link (Ora 0.2), for Linux.
//
// Ora 0.2 joins Unity and its browser with shared-memory rings inside its native ora-audio library
// (Unity) and the ora-link.node addon (Electron), both Windows-only. This is a Linux stand-in for both
// halves: Unity loads it as libora-audio.so (ora_audio_unity.c), Electron as ora-link.node
// (ora_link_node.c). Only the two halves here ever share a link, so the memory layout is our own; what
// has to match Ora is the API each side calls and the record format inside the rings (32-byte heads,
// 8-byte aligned, as Ora's OraRecordReader and link.js read them).
//
// A link is one file on tmpfs: <dir>/<name>, where dir is /tmp/ora-link (ORA_LINK_DIR overrides it).
// Inside the Unity Hub Flatpak the sandbox's /tmp is the host's $XDG_RUNTIME_DIR/.flatpak/<app>/tmp,
// so the launcher hands Electron that path. Each of the 64 channels has two single-producer,
// single-consumer byte rings (Unity to host, host to Unity) big enough for Ora's largest message
// (32 MiB), so a message is always one record and never fragmented. The file is sparse: only pages a
// ring has used take memory, and the consumer gives whole MiBs back as it reads past them.
#ifndef ORALINK_H
#define ORALINK_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

#define OL_ABI 0x00020001u
#define OL_MAGIC 0x4C41524Fu // "ORAL"
#define OL_MAX_LINKS 16
#define OL_MAX_CH 64
#define OL_HEAD 32
#define OL_RING_DATA (40u << 20)                // > 32 MiB message + head
#define OL_DATA_BASE 65536u
#define OL_FILE_BYTES ((uint64_t)OL_DATA_BASE + (uint64_t)OL_MAX_CH * 2 * OL_RING_DATA)
#define OL_SPILL_CAP (256u << 20)
#define OL_NAME_MAX 128

// Status (link-ops.js Status)
enum { ST_OK = 0, ST_SPILLED = 1, ST_DROPPED = 2, ST_INVALID = -1, ST_FORMAT = -2, ST_NOMEM = -3, ST_UNSUPPORTED = -4,
       ST_NOTFOUND = -5, ST_FULL = -6, ST_STALE = -7, ST_CLOSED = -8, ST_TOO_BIG = -9, ST_CORRUPT = -10,
       ST_NOT_ATTACHED = -12, ST_ABI = -13, ST_SPILL_FULL = -14 };
enum { US_CLOSED = 5, US_CLOSING = 4 };                                    // UnityState
enum { HS_NONE = 0, HS_ATTACHED = 1, HS_REFUSED = 2, HS_CLOSING = 3 };     // HostState
enum { CS_OPEN = 1, CS_CLOSED_U = 2, CS_CLOSED_H = 4, CS_CORRUPT_U2H = 8, CS_CORRUPT_H2U = 16 }; // ChannelState
enum { DIR_U2H = 0, DIR_H2U = 1 };

typedef struct {               // in shared memory, 64 bytes
    uint64_t head;             // producer's cursor (bytes ever written)
    uint64_t tail;             // consumer's cursor
    uint32_t seq;              // next record seq (producer)
    uint32_t state;
    uint64_t records;
    uint8_t pad[32];
} OlRing;

typedef struct {               // 64 bytes
    uint32_t id, state, profile, gen;
    uint64_t unitySpill, hostSpill; // bytes each side holds back (spill), for stats
    uint8_t pad[32];
} OlChan;

typedef struct {               // 128 bytes, seqlock
    uint32_t seq, flags, bufferGen, texW, texH, paintSeq, loadState, audioState;
    uint64_t platformTex, hostBeatNs;
    float zoom;
    uint8_t pad[76];
} OlView;

typedef struct {               // at offset 0
    uint32_t magic, abi, role, flags;
    uint32_t creatorPid, hostPid, unityState, hostState;
    uint32_t refuseReason, hostAbi, linkEpoch, wakeHost;
    uint32_t wakeUnity, reqU, reqH, pad0;
    uint64_t u2hDirty, h2uDirty;
    uint64_t unityBeatNs, hostNativeBeatNs, hostLoopBeatNs, creatorStartNs, hostStartNs;
} OlHeader;

#define OL_OFF_CHAN 1024u
#define OL_OFF_RING 8192u
#define OL_OFF_VIEW 16384u

typedef struct OlSpill { struct OlSpill *next; uint32_t len; uint8_t data[]; } OlSpill;

typedef struct {
    int used, isHost;
    uint32_t gen;
    char name[OL_NAME_MAX];
    char path[1024];
    int fd;
    uint8_t *base;
    pthread_mutex_t mu;
    OlSpill *spHead[OL_MAX_CH], *spTail[OL_MAX_CH];
    uint64_t spBytes[OL_MAX_CH];
    uint64_t txRecords[OL_MAX_CH], txBytes[OL_MAX_CH], rxMessages[OL_MAX_CH], rxBytes[OL_MAX_CH], rxHigh[OL_MAX_CH];
    int watchPid, peerExited;
    // host side: the waiter thread
    pthread_t waiter;
    int waiterRunning;
    volatile int stop;
    volatile int armed;
    volatile int exitReported;
    uint64_t wakes;
    void *user; // ora_link_node.c's callbacks
} OlLink;

static inline OlHeader *ol_hdr(OlLink *L) { return (OlHeader *)L->base; }
static inline OlChan *ol_chan(OlLink *L, int slot) { return (OlChan *)(L->base + OL_OFF_CHAN) + slot; }
static inline OlRing *ol_ring(OlLink *L, int slot, int dir) { return (OlRing *)(L->base + OL_OFF_RING) + slot * 2 + dir; }
static inline OlView *ol_view(OlLink *L, int slot) { return (OlView *)(L->base + OL_OFF_VIEW) + slot; }
static inline uint8_t *ol_data(OlLink *L, int slot, int dir) {
    return L->base + OL_DATA_BASE + ((uint64_t)slot * 2 + dir) * OL_RING_DATA;
}

uint64_t ol_now_ns(void);
const char *ol_dir(void);
int64_t ol_handle(OlLink *L);
OlLink *ol_get(int64_t handle);

// Unity (creator) side
int64_t ol_create(uint32_t role, char *nameOut, int cap);
int64_t ol_find(const char *name);
int ol_reap(uint32_t role, const char *keep);
void ol_close_link(OlLink *L, int reason);
int ol_open_channel(OlLink *L, uint32_t profile);
// Host side
int64_t ol_attach(const char *name, int *errRefuse);
void ol_detach(OlLink *L, int reason);

// Both: `side` is the caller's direction of writing (DIR_U2H for Unity, DIR_H2U for the host).
int ol_close_channel(OlLink *L, int side, int ch, int reason);
int ol_write(OlLink *L, int side, int ch, const uint8_t head[OL_HEAD], const uint8_t *payload, uint32_t len);
int64_t ol_flush(OlLink *L, int side, int ch);
// Copies whole records into dst; returns bytes, 0 with *needed set when the next one doesn't fit, or a status.
int ol_take(OlLink *L, int side, int ch, uint8_t *dst, int cap, int maxMsgs, int *needed);
uint64_t ol_take_dirty(OlLink *L, int side);
void ol_mark_dirty(OlLink *L, int side, uint64_t slots);
int ol_check_ch(OlLink *L, int ch);
uint32_t ol_next_req(OlLink *L, int side);
void ol_wake_peer(OlLink *L, int side);
int ol_futex_wait(volatile uint32_t *addr, uint32_t seen, int timeoutMs);

#endif
