// The link core shared by libora-audio.so (Unity) and ora-link.node (Electron). See oralink.h.
#include "oralink.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

_Static_assert(sizeof(OlRing) == 64, "ring header");
_Static_assert(sizeof(OlChan) == 64, "channel descriptor");
_Static_assert(sizeof(OlView) == 128, "view state");
_Static_assert(sizeof(OlHeader) <= OL_OFF_CHAN, "link header");

static OlLink g_links[OL_MAX_LINKS];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

#define LOAD(p) __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

static inline uint64_t align8(uint64_t n) { return (n + 7) & ~(uint64_t)7; }

uint64_t ol_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

const char *ol_dir(void) {
    const char *d = getenv("ORA_LINK_DIR");
    return d && *d ? d : "/tmp/ora-link";
}

int64_t ol_handle(OlLink *L) { return ((int64_t)L->gen << 4) | (int64_t)(L - g_links); }

OlLink *ol_get(int64_t handle) {
    if (handle <= 0) return NULL;
    int slot = (int)(handle & 15);
    OlLink *L = &g_links[slot];
    return L->used && (int64_t)L->gen == (handle >> 4) ? L : NULL;
}

static OlLink *alloc_link(void) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < OL_MAX_LINKS; i++) {
        OlLink *L = &g_links[i];
        if (L->used) continue;
        uint32_t gen = L->gen + 1;
        memset(L, 0, sizeof(*L));
        L->gen = gen ? gen : 1;
        L->used = 1;
        L->fd = -1;
        pthread_mutex_init(&L->mu, NULL);
        pthread_mutex_unlock(&g_mu);
        return L;
    }
    pthread_mutex_unlock(&g_mu);
    return NULL;
}

static void free_link(OlLink *L) {
    for (int ch = 0; ch < OL_MAX_CH; ch++) {
        for (OlSpill *s = L->spHead[ch]; s;) { OlSpill *n = s->next; free(s); s = n; }
        L->spHead[ch] = L->spTail[ch] = NULL;
    }
    if (L->base) munmap(L->base, OL_FILE_BYTES);
    if (L->fd >= 0) close(L->fd);
    L->base = NULL;
    L->fd = -1;
    pthread_mutex_lock(&g_mu);
    L->used = 0;
    pthread_mutex_unlock(&g_mu);
}

static int map_file(OlLink *L, int create) {
    L->fd = open(L->path, create ? (O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC) : (O_RDWR | O_CLOEXEC), 0600);
    if (L->fd < 0) return ST_NOTFOUND;
    if (create && ftruncate(L->fd, (off_t)OL_FILE_BYTES) != 0) return ST_NOMEM;
    struct stat st;
    if (fstat(L->fd, &st) != 0 || (uint64_t)st.st_size < OL_FILE_BYTES) return ST_FORMAT;
    void *p = mmap(NULL, OL_FILE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, L->fd, 0);
    if (p == MAP_FAILED) return ST_NOMEM;
    L->base = p;
    return ST_OK;
}

static void reset_channel(OlLink *L, int slot) {
    for (int dir = 0; dir < 2; dir++) {
        OlRing *r = ol_ring(L, slot, dir);
        // Give the old ring's pages back before it is used again.
        madvise(ol_data(L, slot, dir), OL_RING_DATA, MADV_REMOVE);
        r->head = r->tail = 0;
        r->seq = 1;
        r->records = 0;
        r->state = 1;
    }
    memset(ol_view(L, slot), 0, sizeof(OlView));
    OlChan *c = ol_chan(L, slot);
    c->unitySpill = c->hostSpill = 0;
    L->spBytes[slot] = L->txRecords[slot] = L->txBytes[slot] = L->rxMessages[slot] = L->rxBytes[slot] = L->rxHigh[slot] = 0;
}

// ---------------------------------------------------------------- futex wake

int ol_futex_wait(volatile uint32_t *addr, uint32_t seen, int timeoutMs) {
    struct timespec ts = { timeoutMs / 1000, (long)(timeoutMs % 1000) * 1000000L };
    return (int)syscall(SYS_futex, addr, FUTEX_WAIT, seen, &ts, NULL, 0);
}

void ol_wake_peer(OlLink *L, int side) {
    OlHeader *h = ol_hdr(L);
    volatile uint32_t *w = side == DIR_U2H ? &h->wakeHost : &h->wakeUnity;
    __atomic_add_fetch(w, 1, __ATOMIC_RELEASE);
    syscall(SYS_futex, w, FUTEX_WAKE, 1, NULL, NULL, 0);
}

// ---------------------------------------------------------------- Unity side: links

static void make_name(char *out, size_t cap, uint32_t role) {
    unsigned char rnd[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0 || read(fd, rnd, sizeof rnd) != (ssize_t)sizeof rnd) {
        uint64_t t = ol_now_ns();
        for (int i = 0; i < 16; i++) rnd[i] = (unsigned char)(t >> ((i % 8) * 8)) ^ (unsigned char)(i * 37);
    }
    if (fd >= 0) close(fd);
    char hex[33];
    for (int i = 0; i < 16; i++) sprintf(hex + i * 2, "%02x", rnd[i]);
    snprintf(out, cap, "OraLink.%d.%c.%s", (int)getpid(), (char)(role ? role : 'e'), hex);
}

int64_t ol_create(uint32_t role, char *nameOut, int cap) {
    OlLink *L = alloc_link();
    if (!L) return ST_FULL;
    mkdir(ol_dir(), 0700);
    make_name(L->name, sizeof L->name, role);
    snprintf(L->path, sizeof L->path, "%s/%s", ol_dir(), L->name);
    int st = map_file(L, 1);
    if (st != ST_OK) { unlink(L->path); free_link(L); return st; }
    OlHeader *h = ol_hdr(L);
    h->abi = OL_ABI;
    h->role = role;
    h->creatorPid = (uint32_t)getpid();
    h->creatorStartNs = h->unityBeatNs = ol_now_ns();
    h->linkEpoch = 1;
    // Channel 0, the app channel: id 256 (gen 1, slot 0), opened with the link.
    OlChan *c = ol_chan(L, 0);
    reset_channel(L, 0);
    c->gen = 1;
    c->id = 256;
    c->profile = 0;
    STORE(&c->state, (uint32_t)CS_OPEN);
    STORE(&h->magic, OL_MAGIC);
    if (nameOut && cap > 0) {
        strncpy(nameOut, L->name, (size_t)cap - 1);
        nameOut[cap - 1] = 0;
    }
    return ol_handle(L);
}

int64_t ol_find(const char *name) {
    if (!name || !*name) return ST_NOTFOUND;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < OL_MAX_LINKS; i++) {
        if (g_links[i].used && !g_links[i].isHost && strcmp(g_links[i].name, name) == 0) {
            int64_t hnd = ol_handle(&g_links[i]);
            pthread_mutex_unlock(&g_mu);
            return hnd;
        }
    }
    pthread_mutex_unlock(&g_mu);
    OlLink *L = alloc_link();
    if (!L) return ST_FULL;
    strncpy(L->name, name, sizeof L->name - 1);
    snprintf(L->path, sizeof L->path, "%s/%s", ol_dir(), L->name);
    if (map_file(L, 0) != ST_OK || ol_hdr(L)->magic != OL_MAGIC) { free_link(L); return ST_NOTFOUND; }
    return ol_handle(L);
}

static int pid_alive(int pid) { return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM); }

int ol_reap(uint32_t role, const char *keep) {
    DIR *d = opendir(ol_dir());
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        int pid;
        char r;
        if (sscanf(e->d_name, "OraLink.%d.%c.", &pid, &r) != 2) continue;
        if (role && (uint32_t)(unsigned char)r != role) continue;
        if (keep && strcmp(keep, e->d_name) == 0) continue;
        int mine = 0;
        for (int i = 0; i < OL_MAX_LINKS; i++)
            if (g_links[i].used && strcmp(g_links[i].name, e->d_name) == 0) mine = 1;
        if (mine || pid_alive(pid)) continue;
        char p[1200];
        snprintf(p, sizeof p, "%s/%s", ol_dir(), e->d_name);
        if (unlink(p) == 0) n++;
    }
    closedir(d);
    return n;
}

void ol_close_link(OlLink *L, int reason) {
    (void)reason;
    OlHeader *h = ol_hdr(L);
    STORE(&h->unityState, (uint32_t)US_CLOSED);
    ol_wake_peer(L, DIR_U2H);
    unlink(L->path); // the browser keeps its mapping until it lets go
    free_link(L);
}

int ol_open_channel(OlLink *L, uint32_t profile) {
    pthread_mutex_lock(&L->mu);
    for (int slot = 1; slot < OL_MAX_CH; slot++) {
        OlChan *c = ol_chan(L, slot);
        uint32_t s = LOAD(&c->state);
        if (s != 0 && (s & (CS_CLOSED_U | CS_CLOSED_H)) != (CS_CLOSED_U | CS_CLOSED_H)) continue;
        STORE(&c->state, 0u);
        for (OlSpill *sp = L->spHead[slot]; sp;) { OlSpill *n = sp->next; free(sp); sp = n; }
        L->spHead[slot] = L->spTail[slot] = NULL;
        reset_channel(L, slot);
        uint32_t gen = (c->gen + 1) & 0x7FFFFF;
        if (!gen) gen = 1;
        c->gen = gen;
        c->profile = profile;
        c->id = (gen << 8) | (uint32_t)slot;
        STORE(&c->state, (uint32_t)CS_OPEN);
        pthread_mutex_unlock(&L->mu);
        return (int)c->id;
    }
    pthread_mutex_unlock(&L->mu);
    return ST_FULL;
}

// ---------------------------------------------------------------- host side: attach

int64_t ol_attach(const char *name, int *errRefuse) {
    *errRefuse = 0;
    if (!name || !*name || strchr(name, '/')) return ST_INVALID;
    OlLink *L = alloc_link();
    if (!L) return ST_FULL;
    L->isHost = 1;
    strncpy(L->name, name, sizeof L->name - 1);
    snprintf(L->path, sizeof L->path, "%s/%s", ol_dir(), name);
    int st = map_file(L, 0);
    if (st != ST_OK) { free_link(L); return st; }
    OlHeader *h = ol_hdr(L);
    if (LOAD(&h->magic) != OL_MAGIC) { *errRefuse = 2; free_link(L); return ST_FORMAT; }
    if ((h->abi >> 16) != (OL_ABI >> 16)) {
        *errRefuse = 1;
        STORE(&h->refuseReason, 1u);
        STORE(&h->hostState, (uint32_t)HS_REFUSED);
        free_link(L);
        return ST_ABI;
    }
    h->hostPid = (uint32_t)getpid();
    h->hostAbi = OL_ABI;
    h->hostStartNs = h->hostNativeBeatNs = h->hostLoopBeatNs = ol_now_ns();
    STORE(&h->hostState, (uint32_t)HS_ATTACHED);
    ol_wake_peer(L, DIR_H2U);
    return ol_handle(L);
}

void ol_detach(OlLink *L, int reason) {
    (void)reason;
    STORE(&ol_hdr(L)->hostState, (uint32_t)HS_CLOSING);
    ol_wake_peer(L, DIR_H2U);
    L->stop = 1;
    if (L->waiterRunning) {
        ol_wake_peer(L, DIR_U2H); // ends the waiter's futex wait
        pthread_join(L->waiter, NULL);
        L->waiterRunning = 0;
    }
    free_link(L);
}

// ---------------------------------------------------------------- channels and rings

int ol_check_ch(OlLink *L, int ch) {
    int slot = ch & 0xff;
    if (ch <= 0 || slot >= OL_MAX_CH) return -1;
    OlChan *c = ol_chan(L, slot);
    return LOAD(&c->state) != 0 && c->id == (uint32_t)ch ? slot : -1;
}

void ol_mark_dirty(OlLink *L, int side, uint64_t slots) {
    OlHeader *h = ol_hdr(L);
    __atomic_or_fetch(side == DIR_U2H ? &h->u2hDirty : &h->h2uDirty, slots, __ATOMIC_ACQ_REL);
}

uint64_t ol_take_dirty(OlLink *L, int side) {
    OlHeader *h = ol_hdr(L);
    // The side reading takes what the other side wrote: Unity takes h2u, the host u2h.
    return __atomic_exchange_n(side == DIR_U2H ? &h->h2uDirty : &h->u2hDirty, 0, __ATOMIC_ACQ_REL);
}

int ol_close_channel(OlLink *L, int side, int ch, int reason) {
    (void)reason;
    pthread_mutex_lock(&L->mu);
    int slot = ol_check_ch(L, ch);
    if (slot < 0) { pthread_mutex_unlock(&L->mu); return ST_NOTFOUND; }
    OlChan *c = ol_chan(L, slot);
    __atomic_or_fetch(&c->state, (uint32_t)(side == DIR_U2H ? CS_CLOSED_U : CS_CLOSED_H), __ATOMIC_ACQ_REL);
    for (OlSpill *s = L->spHead[slot]; s;) { OlSpill *n = s->next; free(s); s = n; }
    L->spHead[slot] = L->spTail[slot] = NULL;
    L->spBytes[slot] = 0;
    ol_mark_dirty(L, side, 1ull << slot);
    pthread_mutex_unlock(&L->mu);
    ol_wake_peer(L, side);
    return ST_OK;
}

static void ring_copy_in(uint8_t *d, uint64_t pos, const uint8_t *src, uint64_t n) {
    uint64_t at = pos % OL_RING_DATA, first = OL_RING_DATA - at;
    if (first >= n) memcpy(d + at, src, n);
    else { memcpy(d + at, src, first); memcpy(d, src + first, n - first); }
}
static void ring_copy_out(const uint8_t *d, uint64_t pos, uint8_t *dst, uint64_t n) {
    uint64_t at = pos % OL_RING_DATA, first = OL_RING_DATA - at;
    if (first >= n) memcpy(dst, d + at, n);
    else { memcpy(dst, d + at, first); memcpy(dst + first, d, n - first); }
}

// One whole record into the ring, or ST_FULL.
static int ring_put(OlLink *L, int slot, int dir, uint8_t *rec, uint32_t len) {
    OlRing *r = ol_ring(L, slot, dir);
    uint64_t head = r->head, tail = LOAD(&r->tail), need = align8(len);
    if (OL_RING_DATA - (head - tail) < need) return ST_FULL;
    uint32_t seq = r->seq;
    memcpy(rec + 8, &seq, 4);
    ring_copy_in(ol_data(L, slot, dir), head, rec, len);
    r->seq = seq + 1 ? seq + 1 : 1;
    r->records++;
    STORE(&r->head, head + need);
    return ST_OK;
}

static int flush_locked(OlLink *L, int side, int slot) {
    int moved = 0;
    while (L->spHead[slot]) {
        OlSpill *s = L->spHead[slot];
        if (ring_put(L, slot, side, s->data, s->len) != ST_OK) break;
        L->spHead[slot] = s->next;
        if (!s->next) L->spTail[slot] = NULL;
        L->spBytes[slot] -= s->len;
        free(s);
        moved = 1;
    }
    OlChan *c = ol_chan(L, slot);
    if (side == DIR_U2H) c->unitySpill = L->spBytes[slot]; else c->hostSpill = L->spBytes[slot];
    return moved;
}

int ol_write(OlLink *L, int side, int ch, const uint8_t head[OL_HEAD], const uint8_t *payload, uint32_t len) {
    if ((uint64_t)len + OL_HEAD + 8 > OL_RING_DATA) return ST_TOO_BIG;
    pthread_mutex_lock(&L->mu);
    int slot = ol_check_ch(L, ch);
    if (slot < 0) { pthread_mutex_unlock(&L->mu); return ST_CLOSED; }
    uint32_t cs = LOAD(&ol_chan(L, slot)->state);
    if (cs & (side == DIR_U2H ? CS_CLOSED_U : CS_CLOSED_H)) { pthread_mutex_unlock(&L->mu); return ST_CLOSED; }
    uint32_t recBytes = OL_HEAD + len;
    uint8_t stackRec[4096];
    uint8_t *rec = recBytes <= sizeof stackRec ? stackRec : malloc(recBytes);
    if (!rec) { pthread_mutex_unlock(&L->mu); return ST_NOMEM; }
    memcpy(rec, head, OL_HEAD);
    memcpy(rec, &recBytes, 4);
    rec[15] = 0; // reserved
    if (len) memcpy(rec + OL_HEAD, payload, len);
    int moved = L->spHead[slot] ? flush_locked(L, side, slot) : 0;
    int st;
    if (!L->spHead[slot] && ring_put(L, slot, side, rec, recBytes) == ST_OK) {
        st = ST_OK;
    } else if (L->spBytes[slot] + recBytes > OL_SPILL_CAP) {
        st = ST_SPILL_FULL;
    } else {
        OlSpill *s = malloc(sizeof(OlSpill) + recBytes);
        if (!s) st = ST_NOMEM;
        else {
            s->next = NULL;
            s->len = recBytes;
            memcpy(s->data, rec, recBytes);
            if (L->spTail[slot]) L->spTail[slot]->next = s; else L->spHead[slot] = s;
            L->spTail[slot] = s;
            L->spBytes[slot] += recBytes;
            OlChan *c = ol_chan(L, slot);
            if (side == DIR_U2H) c->unitySpill = L->spBytes[slot]; else c->hostSpill = L->spBytes[slot];
            st = ST_SPILLED;
        }
    }
    if (rec != stackRec) free(rec);
    if (st == ST_OK) { L->txRecords[slot]++; L->txBytes[slot] += recBytes; }
    if (st == ST_OK || moved) ol_mark_dirty(L, side, 1ull << slot);
    pthread_mutex_unlock(&L->mu);
    if (st == ST_OK || moved) ol_wake_peer(L, side);
    return st;
}

int64_t ol_flush(OlLink *L, int side, int ch) {
    pthread_mutex_lock(&L->mu);
    int slot = ol_check_ch(L, ch);
    if (slot < 0) { pthread_mutex_unlock(&L->mu); return 0; }
    int moved = flush_locked(L, side, slot);
    int64_t left = (int64_t)L->spBytes[slot];
    if (moved) ol_mark_dirty(L, side, 1ull << slot);
    pthread_mutex_unlock(&L->mu);
    if (moved) ol_wake_peer(L, side);
    return left;
}

int ol_take(OlLink *L, int side, int ch, uint8_t *dst, int cap, int maxMsgs, int *needed) {
    if (needed) *needed = 0;
    int slot = ol_check_ch(L, ch);
    if (slot < 0) return ST_NOTFOUND;
    int dir = side == DIR_U2H ? DIR_H2U : DIR_U2H; // read what the other side wrote
    OlRing *r = ol_ring(L, slot, dir);
    const uint8_t *d = ol_data(L, slot, dir);
    uint64_t head = LOAD(&r->head), tail = r->tail, start = tail;
    int out = 0, n = 0;
    while (tail < head && n < maxMsgs) {
        uint32_t rb;
        ring_copy_out(d, tail, (uint8_t *)&rb, 4);
        uint64_t sz = align8(rb);
        if (rb < OL_HEAD || sz > head - tail) {
            __atomic_or_fetch(&ol_chan(L, slot)->state, (uint32_t)(dir == DIR_U2H ? CS_CORRUPT_U2H : CS_CORRUPT_H2U), __ATOMIC_ACQ_REL);
            return ST_CORRUPT;
        }
        if ((uint64_t)out + sz > (uint64_t)cap) {
            if (n == 0 && needed) *needed = (int)sz;
            break;
        }
        ring_copy_out(d, tail, dst + out, sz);
        out += (int)sz;
        tail += sz;
        n++;
        L->rxMessages[slot]++;
        L->rxBytes[slot] += rb;
    }
    if (head - start > L->rxHigh[slot]) L->rxHigh[slot] = head - start;
    if (tail != start) {
        // Give back whole MiBs the reader is done with, before the writer may reuse them.
        const uint64_t MiB = 1u << 20;
        uint64_t from = (start + MiB - 1) & ~(MiB - 1), to = tail & ~(MiB - 1);
        for (uint64_t p = from; p < to; p += MiB) {
            uint64_t at = p % OL_RING_DATA;
            if (at + MiB <= OL_RING_DATA) madvise((void *)(d + at), MiB, MADV_REMOVE);
        }
        STORE(&r->tail, tail);
        // The writer may be waiting for room (spill): tell it.
        ol_mark_dirty(L, side, 0);
    }
    // More left than fitted: leave the slot marked so it is taken again.
    if (tail < head) ol_mark_dirty(L, dir == DIR_U2H ? DIR_U2H : DIR_H2U, 1ull << slot);
    return out;
}

uint32_t ol_next_req(OlLink *L, int side) {
    OlHeader *h = ol_hdr(L);
    uint32_t v = __atomic_add_fetch(side == DIR_U2H ? &h->reqU : &h->reqH, 1, __ATOMIC_ACQ_REL);
    if (!v) v = __atomic_add_fetch(side == DIR_U2H ? &h->reqU : &h->reqH, 1, __ATOMIC_ACQ_REL);
    return v;
}
