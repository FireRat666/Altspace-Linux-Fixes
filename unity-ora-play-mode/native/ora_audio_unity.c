// libora-audio.so: the Linux stand-in for Ora's ora-audio.dll, as Unity's SideQuest.Ora.dll calls it
// (DllImport "ora-audio"). The OraBridge_* functions are the browser link (oralink.c). The OraAudio_*
// functions are stubs that report no native audio, so Ora falls back to letting the browser play the
// page's audio itself, as it does when ora-audio is missing.
#include "oralink.h"

#include <signal.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

// Structs as SideQuest.Ora declares them (sequential layout).
typedef struct { uint32_t structSize, flags, arenaBytes, role; } OraLinkConfig;
typedef struct { uint32_t structSize, profile, u2hBytes, h2uBytes; } OraChannelConfig;
typedef struct {
    uint32_t structSize, unityState, hostState, refuseReason, creatorAbi, hostAbi, creatorPid, hostPid, linkEpoch, flags;
    int32_t peerExited, peerExitCode;
    uint64_t unityBeatNs, hostNativeBeatNs, hostLoopBeatNs, nowNs, creatorStartTime, hostStartTime;
} OraLinkState;
typedef struct {
    uint32_t structSize, seq, flags, bufferGen, texW, texH, paintSeq, loadState;
    uint64_t platformTex, hostBeatNs;
    uint32_t audioState;
    float zoom;
    uint32_t reserved0, reserved1;
} OraBridgeViewState;
typedef struct {
    uint32_t structSize, channelState, txRingState, rxRingState, txCorruptReason, rxCorruptReason, txDataBytes, rxDataBytes;
    uint64_t txRecords, txBytes, txSpillBytes, txSpillHighWater, txCoalesced, txDropped, txRefused, rxMessages, rxBytes,
        rxFragMessages, rxHighWater, peerSpillBytes;
} OraBridgeStats;

_Static_assert(sizeof(OraLinkState) == 96, "OraLinkState");
_Static_assert(sizeof(OraBridgeViewState) == 64, "OraBridgeViewState");
_Static_assert(sizeof(OraBridgeStats) == 128, "OraBridgeStats");

static OlLink *unity_link(int64_t h) {
    OlLink *L = ol_get(h);
    if (L && !L->isHost) ol_hdr(L)->unityBeatNs = ol_now_ns();
    return L;
}

EXPORT uint32_t OraBridge_AbiVersion(void) { return OL_ABI; }
EXPORT int32_t OraBridge_SelfTest(void) { return ST_OK; }
EXPORT int64_t OraBridge_NowNs(void) { return (int64_t)ol_now_ns(); }

EXPORT int64_t OraBridge_CreateLink(OraLinkConfig *cfg, uint8_t *nameOut, int32_t nameCap) {
    return ol_create(cfg ? cfg->role : 'e', (char *)nameOut, nameCap);
}
EXPORT int64_t OraBridge_FindLink(const uint8_t *name) { return ol_find((const char *)name); }
EXPORT int32_t OraBridge_ReapLinks(uint32_t role, const uint8_t *keep) { return ol_reap(role, (const char *)keep); }
EXPORT void OraBridge_CloseLink(int64_t link, int32_t reason) {
    OlLink *L = unity_link(link);
    if (L) ol_close_link(L, reason);
}
EXPORT int32_t OraBridge_SetUnityState(int64_t link, int32_t state) {
    OlLink *L = unity_link(link);
    if (!L) return ST_NOTFOUND;
    __atomic_store_n(&ol_hdr(L)->unityState, (uint32_t)state, __ATOMIC_RELEASE);
    ol_wake_peer(L, DIR_U2H);
    return ST_OK;
}
EXPORT int32_t OraBridge_WatchPeer(int64_t link, int32_t pid) {
    OlLink *L = unity_link(link);
    if (!L) return ST_NOTFOUND;
    L->watchPid = pid;
    L->peerExited = 0;
    return ST_OK;
}
EXPORT int32_t OraBridge_GetLinkState(int64_t link, OraLinkState *s) {
    OlLink *L = unity_link(link);
    if (!L || !s) return ST_NOTFOUND;
    OlHeader *h = ol_hdr(L);
    uint32_t size = s->structSize;
    memset(s, 0, sizeof *s);
    s->structSize = size ? size : sizeof *s;
    s->unityState = h->unityState;
    s->hostState = __atomic_load_n(&h->hostState, __ATOMIC_ACQUIRE);
    s->refuseReason = h->refuseReason;
    s->creatorAbi = h->abi;
    s->hostAbi = h->hostAbi;
    s->creatorPid = h->creatorPid;
    s->hostPid = h->hostPid;
    s->linkEpoch = h->linkEpoch;
    s->flags = h->flags;
    // The browser Unity started (the launcher, or flatpak-spawn under Flatpak): gone once kill(0) fails.
    if (L->watchPid > 0 && !L->peerExited && kill(L->watchPid, 0) != 0 && errno == ESRCH) L->peerExited = 1;
    s->peerExited = L->peerExited;
    s->peerExitCode = 0;
    s->unityBeatNs = h->unityBeatNs;
    s->hostNativeBeatNs = h->hostNativeBeatNs;
    s->hostLoopBeatNs = h->hostLoopBeatNs;
    s->nowNs = ol_now_ns();
    s->creatorStartTime = h->creatorStartNs;
    s->hostStartTime = h->hostStartNs;
    return ST_OK;
}
EXPORT int32_t OraBridge_OpenChannel(int64_t link, OraChannelConfig *cfg) {
    OlLink *L = unity_link(link);
    if (!L) return ST_NOTFOUND;
    return ol_open_channel(L, cfg ? cfg->profile : 1);
}
EXPORT int32_t OraBridge_CloseChannel(int64_t link, int32_t ch, int32_t reason) {
    OlLink *L = unity_link(link);
    return L ? ol_close_channel(L, DIR_U2H, ch, reason) : ST_NOTFOUND;
}
EXPORT int32_t OraBridge_Write(int64_t link, int32_t ch, const uint8_t *head, const uint8_t *payload, int32_t offset, int32_t length) {
    OlLink *L = unity_link(link);
    if (!L) return ST_NOTFOUND;
    if (!head || offset < 0 || length < 0 || (length > 0 && !payload)) return ST_INVALID;
    return ol_write(L, DIR_U2H, ch, head, length ? payload + offset : NULL, (uint32_t)length);
}
EXPORT int32_t OraBridge_FlushSpill(int64_t link, int32_t ch) {
    OlLink *L = unity_link(link);
    if (!L) return ST_NOTFOUND;
    int64_t left = ol_flush(L, DIR_U2H, ch);
    return left > 0x7fffffff ? 0x7fffffff : (int32_t)left;
}
EXPORT uint64_t OraBridge_TakeReady(int64_t link) {
    OlLink *L = unity_link(link);
    return L ? ol_take_dirty(L, DIR_U2H) : 0;
}
EXPORT int32_t OraBridge_MarkReady(int64_t link, uint64_t slots) {
    OlLink *L = unity_link(link);
    if (!L) return ST_NOTFOUND;
    ol_mark_dirty(L, DIR_H2U, slots);
    return ST_OK;
}
EXPORT int32_t OraBridge_Take(int64_t link, int32_t ch, uint8_t *dst, int32_t cap, int32_t maxMsgs, int32_t *needed) {
    OlLink *L = unity_link(link);
    if (needed) *needed = 0;
    if (!L) return ST_NOTFOUND;
    int slot = ol_check_ch(L, ch);
    if (slot < 0) return ST_CLOSED;
    int r = ol_take(L, DIR_U2H, ch, dst, cap, maxMsgs > 0 ? maxMsgs : 64, needed);
    // The browser closed its side and everything it sent has been read.
    if (r == 0 && (!needed || *needed == 0) && (__atomic_load_n(&ol_chan(L, slot)->state, __ATOMIC_ACQUIRE) & CS_CLOSED_H))
        return ST_CLOSED;
    return r;
}
EXPORT int32_t OraBridge_GetViewState(int64_t link, int32_t ch, OraBridgeViewState *out) {
    OlLink *L = unity_link(link);
    if (!L || !out) return ST_NOTFOUND;
    int slot = ol_check_ch(L, ch);
    if (slot < 0) return ST_NOTFOUND;
    OlView *v = ol_view(L, slot);
    OlView copy;
    for (int tries = 0; tries < 8; tries++) {
        uint32_t s1 = __atomic_load_n(&v->seq, __ATOMIC_ACQUIRE);
        if (s1 & 1) continue;
        memcpy(&copy, (const void *)v, sizeof copy);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&v->seq, __ATOMIC_ACQUIRE) == s1) break;
    }
    uint32_t size = out->structSize;
    memset(out, 0, sizeof *out);
    out->structSize = size ? size : sizeof *out;
    out->seq = copy.seq >> 1;
    out->flags = copy.flags;
    out->bufferGen = copy.bufferGen;
    out->texW = copy.texW;
    out->texH = copy.texH;
    out->paintSeq = copy.paintSeq;
    out->loadState = copy.loadState;
    out->platformTex = copy.platformTex;
    out->hostBeatNs = copy.hostBeatNs;
    out->audioState = copy.audioState;
    out->zoom = copy.zoom;
    return ST_OK;
}
EXPORT int32_t OraBridge_GetStats(int64_t link, int32_t ch, OraBridgeStats *s) {
    OlLink *L = unity_link(link);
    if (!L || !s) return ST_NOTFOUND;
    int slot = ol_check_ch(L, ch);
    if (slot < 0) return ST_NOTFOUND;
    uint32_t size = s->structSize;
    memset(s, 0, sizeof *s);
    s->structSize = size ? size : sizeof *s;
    OlChan *c = ol_chan(L, slot);
    s->channelState = c->state;
    s->txRingState = ol_ring(L, slot, DIR_U2H)->state;
    s->rxRingState = ol_ring(L, slot, DIR_H2U)->state;
    s->txDataBytes = s->rxDataBytes = OL_RING_DATA;
    s->txRecords = L->txRecords[slot];
    s->txBytes = L->txBytes[slot];
    s->txSpillBytes = L->spBytes[slot];
    s->rxMessages = L->rxMessages[slot];
    s->rxBytes = L->rxBytes[slot];
    s->rxHighWater = L->rxHigh[slot];
    s->peerSpillBytes = c->hostSpill;
    return ST_OK;
}
EXPORT uint32_t OraBridge_NextRequestId(int64_t link) {
    OlLink *L = unity_link(link);
    return L ? ol_next_req(L, DIR_U2H) : 0;
}

// ---------------------------------------------------------------- audio: none on Linux

static const char kVersion[] = "ora-audio linux-link 0.2 (no native audio)";
EXPORT const char *OraAudio_VersionPtr(void) { return kVersion; }
EXPORT int64_t OraAudio_CreateView(int32_t rate, int32_t channels) { (void)rate; (void)channels; return 0; }
EXPORT int32_t OraAudio_Retain(int64_t h) { (void)h; return 0; }
EXPORT void OraAudio_Release(int64_t h) { (void)h; }
EXPORT int32_t OraAudio_PushChunk(int64_t h, uint64_t ns, const uint8_t *d, int32_t n) { (void)h; (void)ns; (void)d; (void)n; return ST_UNSUPPORTED; }
EXPORT void OraAudio_ResetSources(int64_t h, uint64_t ns) { (void)h; (void)ns; }
EXPORT void OraAudio_SetPaused(int64_t h, int32_t p) { (void)h; (void)p; }
EXPORT int32_t OraAudio_AttachMapping(int64_t h, const uint8_t *name) { (void)h; (void)name; return ST_UNSUPPORTED; }
EXPORT void OraAudio_DetachMapping(int64_t h) { (void)h; }
EXPORT int32_t OraAudio_Read(int64_t h, void *dst, int32_t frames, int32_t channels, int64_t key) {
    (void)h; (void)key;
    if (dst && frames > 0 && channels > 0) memset(dst, 0, (size_t)frames * (size_t)channels * sizeof(float));
    return 0;
}
EXPORT void OraAudio_SetGain(int64_t h, float g) { (void)h; (void)g; }
EXPORT void OraAudio_SetLatency(int64_t h, float ms) { (void)h; (void)ms; }
EXPORT int32_t OraAudio_GetStats(int64_t h, void *stats) { (void)h; (void)stats; return ST_UNSUPPORTED; }
EXPORT int32_t OraAudio_ValidateChunk(const uint8_t *d, int32_t n) { (void)d; (void)n; return ST_UNSUPPORTED; }
