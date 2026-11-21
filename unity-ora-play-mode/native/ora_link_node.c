// ora-link.node: the Linux stand-in for Ora's Windows ora-link.node, with the API Ora's dist/link.js
// calls (attach, startWaiter, beginWake, arm, channelInfo, take, writeBytes, writeText, writeText2,
// flushSpill, closeChannel, setViewState, detach, ...). Plain Node-API, so one build loads in any
// Electron. The link itself is oralink.c.
#define NAPI_VERSION 8
#include <node_api.h>

#include "oralink.h"

#include <stdlib.h>
#include <string.h>

#define CHECK(call) do { if ((call) != napi_ok) return NULL; } while (0)

typedef struct {
    napi_threadsafe_function onWake, onExit;
} Callbacks;

static napi_value num(napi_env env, double v) { napi_value r; napi_create_double(env, v, &r); return r; }
static napi_value u32v(napi_env env, uint32_t v) { napi_value r; napi_create_uint32(env, v, &r); return r; }
static napi_value i32v(napi_env env, int32_t v) { napi_value r; napi_create_int32(env, v, &r); return r; }
static void set(napi_env env, napi_value o, const char *k, napi_value v) { napi_set_named_property(env, o, k, v); }

static int64_t arg_i64(napi_env env, napi_value v) { int64_t r = 0; napi_get_value_int64(env, v, &r); return r; }
static uint32_t arg_u32(napi_env env, napi_value v) {
    double d = 0;
    if (napi_get_value_double(env, v, &d) != napi_ok) return 0;
    return (uint32_t)(int64_t)d;
}
static double prop_num(napi_env env, napi_value o, const char *k, double def) {
    bool has = false;
    if (napi_has_named_property(env, o, k, &has) != napi_ok || !has) return def;
    napi_value v;
    double d;
    if (napi_get_named_property(env, o, k, &v) != napi_ok || napi_get_value_double(env, v, &d) != napi_ok) return def;
    return d;
}

static OlLink *host_link(napi_env env, napi_value h) {
    OlLink *L = ol_get(arg_i64(env, h));
    return L && L->isHost ? L : NULL;
}

static napi_value throw_status(napi_env env, int code, int refuse, uint32_t creatorAbi) {
    napi_value msg, err;
    napi_create_string_utf8(env, "ora-link: cannot attach", NAPI_AUTO_LENGTH, &msg);
    napi_create_error(env, NULL, msg, &err);
    set(env, err, "code", i32v(env, code));
    set(env, err, "refuseReason", i32v(env, refuse));
    set(env, err, "creatorAbi", u32v(env, creatorAbi));
    napi_throw(env, err);
    return NULL;
}

// attach(name) -> { host, creatorAbi, creatorPid, role, linkEpoch }
static napi_value Attach(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    char name[OL_NAME_MAX] = { 0 };
    size_t n;
    if (argc < 1 || napi_get_value_string_utf8(env, argv[0], name, sizeof name, &n) != napi_ok) return throw_status(env, ST_INVALID, 0, 0);
    int refuse = 0;
    int64_t h = ol_attach(name, &refuse);
    if (h <= 0) return throw_status(env, (int)h, refuse, 0);
    OlLink *L = ol_get(h);
    OlHeader *hd = ol_hdr(L);
    napi_value o;
    napi_create_object(env, &o);
    set(env, o, "host", num(env, (double)h));
    set(env, o, "creatorAbi", u32v(env, hd->abi));
    set(env, o, "creatorPid", u32v(env, hd->creatorPid));
    set(env, o, "role", u32v(env, hd->role));
    set(env, o, "linkEpoch", u32v(env, hd->linkEpoch));
    return o;
}

static napi_value BindThread(napi_env env, napi_callback_info info) { (void)info; return i32v(env, ST_OK); }

// The waiter: sleeps on the link's wake word; when Unity wrote and the JS side is armed, schedules its drain.
static void *waiter_main(void *arg) {
    OlLink *L = arg;
    Callbacks *cb = L->user;
    OlHeader *h = ol_hdr(L);
    while (!L->stop) {
        uint32_t seen = __atomic_load_n(&h->wakeHost, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&h->u2hDirty, __ATOMIC_ACQUIRE) == 0 && !L->stop) ol_futex_wait(&h->wakeHost, seen, 100);
        if (L->stop) break;
        h->hostNativeBeatNs = ol_now_ns();
        uint32_t us = __atomic_load_n(&h->unityState, __ATOMIC_ACQUIRE);
        if ((us == US_CLOSED || us == US_CLOSING) && !L->exitReported) {
            L->exitReported = 1;
            napi_call_threadsafe_function(cb->onExit, NULL, napi_tsfn_nonblocking);
        }
        if (__atomic_load_n(&h->u2hDirty, __ATOMIC_ACQUIRE) != 0 && __atomic_exchange_n(&L->armed, 0, __ATOMIC_ACQ_REL)) {
            L->wakes++;
            napi_call_threadsafe_function(cb->onWake, NULL, napi_tsfn_nonblocking);
        }
    }
    return NULL;
}

static void call_wake(napi_env env, napi_value js, void *ctx, void *data) {
    (void)ctx; (void)data;
    if (!env || !js) return;
    napi_value undef;
    napi_get_undefined(env, &undef);
    napi_call_function(env, undef, js, 0, NULL, NULL);
}
static void call_exit(napi_env env, napi_value js, void *ctx, void *data) {
    (void)ctx; (void)data;
    if (!env || !js) return;
    napi_value undef, args[2];
    napi_get_undefined(env, &undef);
    args[0] = i32v(env, 3); // WaitResult.CLOSED: Unity closed the link
    args[1] = i32v(env, 0);
    napi_call_function(env, undef, js, 2, args, NULL);
}

// startWaiter(host, onWake, onExit) -> status
static napi_value StartWaiter(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    OlLink *L = host_link(env, argv[0]);
    if (!L) return i32v(env, ST_NOT_ATTACHED);
    if (L->waiterRunning) return i32v(env, ST_OK);
    Callbacks *cb = calloc(1, sizeof *cb);
    napi_value name;
    napi_create_string_utf8(env, "ora-link-waiter", NAPI_AUTO_LENGTH, &name);
    if (napi_create_threadsafe_function(env, argv[1], NULL, name, 0, 1, NULL, NULL, NULL, call_wake, &cb->onWake) != napi_ok ||
        napi_create_threadsafe_function(env, argv[2], NULL, name, 0, 1, NULL, NULL, NULL, call_exit, &cb->onExit) != napi_ok) {
        free(cb);
        return i32v(env, ST_INVALID);
    }
    // The waiter must not keep Electron alive by itself.
    napi_unref_threadsafe_function(env, cb->onWake);
    napi_unref_threadsafe_function(env, cb->onExit);
    L->user = cb;
    L->armed = 1;
    if (pthread_create(&L->waiter, NULL, waiter_main, L) != 0) return i32v(env, ST_NOMEM);
    L->waiterRunning = 1;
    return i32v(env, ST_OK);
}

// beat(host, source)
static napi_value Beat(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    OlLink *L = host_link(env, argv[0]);
    if (!L) return i32v(env, ST_NOT_ATTACHED);
    uint64_t now = ol_now_ns();
    if (argc > 1 && arg_u32(env, argv[1]) == 1) ol_hdr(L)->hostLoopBeatNs = now; else ol_hdr(L)->hostNativeBeatNs = now;
    return i32v(env, ST_OK);
}

// beginWake(host) -> [lo, hi]: the channels Unity wrote to since the last call.
static napi_value BeginWake(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], arr;
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    OlLink *L = host_link(env, argv[0]);
    uint64_t m = L ? ol_take_dirty(L, DIR_H2U) : 0;
    napi_create_array_with_length(env, 2, &arr);
    napi_set_element(env, arr, 0, u32v(env, (uint32_t)m));
    napi_set_element(env, arr, 1, u32v(env, (uint32_t)(m >> 32)));
    return arr;
}

// arm(host, lo, hi): re-marks unfinished channels; with none, arms the waiter: 1 = armed and empty, 0 = work arrived.
static napi_value Arm(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    OlLink *L = host_link(env, argv[0]);
    if (!L) return i32v(env, 0);
    uint64_t m = (uint64_t)arg_u32(env, argv[1]) | ((uint64_t)arg_u32(env, argv[2]) << 32);
    if (m) { ol_mark_dirty(L, DIR_U2H, m); return i32v(env, 0); }
    __atomic_store_n(&L->armed, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&ol_hdr(L)->u2hDirty, __ATOMIC_ACQUIRE) != 0) {
        __atomic_store_n(&L->armed, 0, __ATOMIC_RELEASE);
        return i32v(env, 0);
    }
    return i32v(env, 1);
}

// channelInfo(host, slot) -> { id, state, profile } or null
static napi_value ChannelInfo(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], nul;
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    napi_get_null(env, &nul);
    OlLink *L = host_link(env, argv[0]);
    uint32_t slot = arg_u32(env, argv[1]);
    if (!L || slot >= OL_MAX_CH) return nul;
    OlChan *c = ol_chan(L, (int)slot);
    uint32_t st = __atomic_load_n(&c->state, __ATOMIC_ACQUIRE);
    if (!st || !c->id) return nul;
    napi_value o;
    napi_create_object(env, &o);
    set(env, o, "id", u32v(env, c->id));
    set(env, o, "state", u32v(env, st));
    set(env, o, "profile", u32v(env, c->profile));
    set(env, o, "slot", u32v(env, slot));
    return o;
}

static napi_value utf16(napi_env env, const uint8_t *p, size_t bytes) {
    napi_value s;
    char16_t *tmp = malloc(bytes + 2);
    memcpy(tmp, p, bytes);
    napi_create_string_utf16(env, tmp, bytes / 2, &s);
    free(tmp);
    return s;
}
static napi_value utf8(napi_env env, const uint8_t *p, size_t bytes) {
    napi_value s;
    napi_create_string_utf8(env, (const char *)p, bytes, &s);
    return s;
}

// One record (32-byte head, payload) as the object link.js dispatches.
static napi_value record_obj(napi_env env, const uint8_t *r) {
    uint32_t recBytes, seq, reqId, epoch, a0, a1;
    uint16_t flags, op;
    memcpy(&recBytes, r, 4);
    memcpy(&flags, r + 6, 2);
    memcpy(&seq, r + 8, 4);
    memcpy(&op, r + 12, 2);
    memcpy(&reqId, r + 16, 4);
    memcpy(&epoch, r + 20, 4);
    memcpy(&a0, r + 24, 4);
    memcpy(&a1, r + 28, 4);
    uint8_t type = r[4], cls = r[5], codec = r[14];
    const uint8_t *p = r + OL_HEAD;
    size_t n = recBytes - OL_HEAD;
    napi_value o;
    napi_create_object(env, &o);
    set(env, o, "type", u32v(env, type));
    set(env, o, "cls", u32v(env, cls));
    set(env, o, "flags", u32v(env, flags));
    set(env, o, "seq", u32v(env, seq));
    set(env, o, "op", u32v(env, op));
    set(env, o, "codec", u32v(env, codec));
    set(env, o, "reqId", u32v(env, reqId));
    set(env, o, "epoch", u32v(env, epoch));
    set(env, o, "a0", u32v(env, a0));
    set(env, o, "a1", u32v(env, a1));
    if (codec == 1) set(env, o, "text", utf16(env, p, n & ~(size_t)1));
    else if (codec == 2) set(env, o, "text", utf8(env, p, n));
    else if ((codec == 4 || codec == 5) && n >= 4) {
        uint32_t len0;
        memcpy(&len0, p, 4);
        size_t b0 = codec == 4 ? (size_t)len0 * 2 : len0;
        if (b0 > n - 4) b0 = n - 4;
        napi_value s0 = codec == 4 ? utf16(env, p + 4, b0) : utf8(env, p + 4, b0);
        napi_value s1 = codec == 4 ? utf16(env, p + 4 + b0, (n - 4 - b0) & ~(size_t)1) : utf8(env, p + 4 + b0, n - 4 - b0);
        set(env, o, "s0", s0);
        set(env, o, "s1", s1);
    } else if (codec == 3) {
        void *buf;
        napi_value ab;
        napi_create_arraybuffer(env, n, &buf, &ab);
        if (n) memcpy(buf, p, n);
        set(env, o, "bytes", ab);
    }
    return o;
}

// take(host, ch, max) -> records[] or a status
static napi_value Take(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    OlLink *L = host_link(env, argv[0]);
    if (!L) return i32v(env, ST_NOT_ATTACHED);
    int ch = (int)arg_u32(env, argv[1]);
    int max = argc > 2 ? (int)arg_u32(env, argv[2]) : 64;
    static uint8_t *buf;
    static int cap;
    if (!buf) { cap = 1 << 20; buf = malloc((size_t)cap); }
    int needed = 0;
    int r = ol_take(L, DIR_H2U, ch, buf, cap, max, &needed);
    while (r == 0 && needed > cap) {
        int ncap = cap;
        while (ncap < needed) ncap *= 2;
        uint8_t *nb = realloc(buf, (size_t)ncap);
        if (!nb) return i32v(env, ST_NOMEM);
        buf = nb;
        cap = ncap;
        r = ol_take(L, DIR_H2U, ch, buf, cap, max, &needed);
    }
    if (r < 0) return i32v(env, r);
    napi_value arr;
    napi_create_array(env, &arr);
    uint32_t i = 0;
    for (int off = 0; off + OL_HEAD <= r;) {
        uint32_t rb;
        memcpy(&rb, buf + off, 4);
        napi_set_element(env, arr, i++, record_obj(env, buf + off));
        off += (int)((rb + 7) & ~7u);
    }
    return arr;
}

static void make_head(uint8_t h[OL_HEAD], uint32_t type, uint32_t op, uint32_t cls, uint32_t flags, uint32_t reqId, uint32_t epoch,
                      uint32_t a0, uint32_t a1, uint32_t codec) {
    memset(h, 0, OL_HEAD);
    h[4] = (uint8_t)type;
    h[5] = (uint8_t)cls;
    uint16_t f = (uint16_t)flags, o = (uint16_t)op;
    memcpy(h + 6, &f, 2);
    memcpy(h + 12, &o, 2);
    h[14] = (uint8_t)codec;
    memcpy(h + 16, &reqId, 4);
    memcpy(h + 20, &epoch, 4);
    memcpy(h + 24, &a0, 4);
    memcpy(h + 28, &a1, 4);
}

// The record type by op, as Ora's op table has it (link-ops.js OPS[].type): PAGE_SEND, PAGE_BINARY_SEND,
// PAGE_MSG and PAGE_BINARY are PAGE_MSG records (2); ORAB_TO_PAGE and ORAB_FROM_PAGE are ORAB (3); the rest CTRL (1).
static uint32_t type_of(uint32_t op) {
    if (op == 0x34 || op == 0x36 || op == 0x60 || op == 0x62) return 2;
    if (op == 0x35 || op == 0x61) return 3;
    return 1;
}

static int get_utf16(napi_env env, napi_value v, char16_t **out, size_t *units) {
    napi_value s = v;
    napi_valuetype t;
    napi_typeof(env, v, &t);
    if (t != napi_string) napi_coerce_to_string(env, v, &s);
    if (napi_get_value_string_utf16(env, s, NULL, 0, units) != napi_ok) return 0;
    *out = malloc((*units + 1) * 2);
    size_t got;
    napi_get_value_string_utf16(env, s, *out, *units + 1, &got);
    *units = got;
    return 1;
}

// writeBytes(host, ch, op, cls, flags, reqId, epoch, a0, a1, payload|null) -> status
static napi_value WriteBytes(napi_env env, napi_callback_info info) {
    size_t argc = 10;
    napi_value argv[10];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    OlLink *L = host_link(env, argv[0]);
    if (!L) return i32v(env, ST_CLOSED);
    uint32_t op = arg_u32(env, argv[2]), cls = arg_u32(env, argv[3]), flags = arg_u32(env, argv[4]);
    const uint8_t *data = NULL;
    size_t len = 0;
    char *own = NULL;
    if (argc > 9) {
        napi_valuetype t;
        napi_typeof(env, argv[9], &t);
        bool is;
        if (napi_is_arraybuffer(env, argv[9], &is) == napi_ok && is) {
            void *p;
            napi_get_arraybuffer_info(env, argv[9], &p, &len);
            data = p;
        } else if (napi_is_typedarray(env, argv[9], &is) == napi_ok && is) {
            napi_typedarray_type tt;
            size_t elems, byteOff;
            void *p;
            napi_value ab;
            napi_get_typedarray_info(env, argv[9], &tt, &elems, &p, &ab, &byteOff);
            size_t per = (tt == napi_int16_array || tt == napi_uint16_array) ? 2
                       : (tt == napi_int32_array || tt == napi_uint32_array || tt == napi_float32_array) ? 4
                       : (tt == napi_float64_array || tt == napi_bigint64_array || tt == napi_biguint64_array) ? 8 : 1;
            data = p;
            len = elems * per;
        } else if (napi_is_dataview(env, argv[9], &is) == napi_ok && is) {
            void *p;
            napi_value ab;
            size_t byteOff;
            napi_get_dataview_info(env, argv[9], &len, &p, &ab, &byteOff);
            data = p;
        } else if (t == napi_string) {
            size_t n;
            napi_get_value_string_utf8(env, argv[9], NULL, 0, &n);
            own = malloc(n + 1);
            napi_get_value_string_utf8(env, argv[9], own, n + 1, &n);
            data = (const uint8_t *)own;
            len = n;
        }
    }
    uint8_t head[OL_HEAD];
    make_head(head, type_of(op), op, cls, flags, arg_u32(env, argv[5]), arg_u32(env, argv[6]), arg_u32(env, argv[7]), arg_u32(env, argv[8]),
              len ? 3 : 0);
    int st = ol_write(L, DIR_H2U, (int)arg_u32(env, argv[1]), head, data, (uint32_t)len);
    free(own);
    return i32v(env, st);
}

// writeText(host, ch, op, cls, flags, reqId, epoch, a0, a1, text) -> status (UTF-16)
static napi_value WriteText(napi_env env, napi_callback_info info) {
    size_t argc = 10;
    napi_value argv[10];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    OlLink *L = host_link(env, argv[0]);
    if (!L) return i32v(env, ST_CLOSED);
    char16_t *s = NULL;
    size_t units = 0;
    if (argc > 9 && !get_utf16(env, argv[9], &s, &units)) return i32v(env, ST_INVALID);
    uint32_t flags = arg_u32(env, argv[4]);
    uint8_t head[OL_HEAD];
    make_head(head, type_of(arg_u32(env, argv[2])), arg_u32(env, argv[2]), arg_u32(env, argv[3]), flags, arg_u32(env, argv[5]),
              arg_u32(env, argv[6]), arg_u32(env, argv[7]), arg_u32(env, argv[8]), 1);
    int st = ol_write(L, DIR_H2U, (int)arg_u32(env, argv[1]), head, (const uint8_t *)s, (uint32_t)(units * 2));
    free(s);
    return i32v(env, st);
}

// writeText2(host, ch, op, cls, flags, reqId, epoch, s0, s1) -> status (UTF16_PAIR): page messages, type and data.
static napi_value WriteText2(napi_env env, napi_callback_info info) {
    size_t argc = 9;
    napi_value argv[9];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    OlLink *L = host_link(env, argv[0]);
    if (!L) return i32v(env, ST_CLOSED);
    char16_t *s0 = NULL, *s1 = NULL;
    size_t u0 = 0, u1 = 0;
    if (argc < 9 || !get_utf16(env, argv[7], &s0, &u0) || !get_utf16(env, argv[8], &s1, &u1)) { free(s0); return i32v(env, ST_INVALID); }
    size_t len = 4 + (u0 + u1) * 2;
    uint8_t *p = malloc(len);
    uint32_t l0 = (uint32_t)u0;
    memcpy(p, &l0, 4);
    memcpy(p + 4, s0, u0 * 2);
    memcpy(p + 4 + u0 * 2, s1, u1 * 2);
    uint32_t flags = arg_u32(env, argv[4]);
    uint8_t head[OL_HEAD];
    make_head(head, type_of(arg_u32(env, argv[2])), arg_u32(env, argv[2]), arg_u32(env, argv[3]), flags, arg_u32(env, argv[5]),
              arg_u32(env, argv[6]), 0, 0, 4);
    int st = ol_write(L, DIR_H2U, (int)arg_u32(env, argv[1]), head, p, (uint32_t)len);
    free(p);
    free(s0);
    free(s1);
    return i32v(env, st);
}

static napi_value FlushSpill(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    OlLink *L = host_link(env, argv[0]);
    return num(env, L ? (double)ol_flush(L, DIR_H2U, (int)arg_u32(env, argv[1])) : 0);
}

static napi_value CloseChannel(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    OlLink *L = host_link(env, argv[0]);
    if (!L) return i32v(env, ST_CLOSED);
    return i32v(env, ol_close_channel(L, DIR_H2U, (int)arg_u32(env, argv[1]), argc > 2 ? (int)arg_u32(env, argv[2]) : 0));
}

static napi_value Detach(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    OlLink *L = host_link(env, argv[0]);
    if (L) {
        Callbacks *cb = L->user;
        ol_detach(L, argc > 1 ? (int)arg_u32(env, argv[1]) : 0);
        if (cb) {
            napi_release_threadsafe_function(cb->onWake, napi_tsfn_abort);
            napi_release_threadsafe_function(cb->onExit, napi_tsfn_abort);
            free(cb);
        }
    }
    return i32v(env, ST_OK);
}

// setViewState(host, ch, { flags, bufferGen, texW, texH, paintSeq, loadState, audioState?, zoom?, platformTex? })
static napi_value SetViewState(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    OlLink *L = host_link(env, argv[0]);
    if (!L) return i32v(env, ST_CLOSED);
    int slot = ol_check_ch(L, (int)arg_u32(env, argv[1]));
    if (slot < 0) return i32v(env, ST_NOTFOUND);
    OlView *v = ol_view(L, slot);
    napi_value o = argv[2];
    uint32_t s = v->seq;
    __atomic_store_n(&v->seq, s + 1, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    v->flags = (uint32_t)prop_num(env, o, "flags", v->flags);
    v->bufferGen = (uint32_t)prop_num(env, o, "bufferGen", v->bufferGen);
    v->texW = (uint32_t)prop_num(env, o, "texW", v->texW);
    v->texH = (uint32_t)prop_num(env, o, "texH", v->texH);
    v->paintSeq = (uint32_t)prop_num(env, o, "paintSeq", v->paintSeq);
    v->loadState = (uint32_t)prop_num(env, o, "loadState", v->loadState);
    v->audioState = (uint32_t)prop_num(env, o, "audioState", v->audioState);
    v->zoom = (float)prop_num(env, o, "zoom", v->zoom);
    v->platformTex = (uint64_t)prop_num(env, o, "platformTex", (double)v->platformTex);
    v->hostBeatNs = ol_now_ns();
    __atomic_store_n(&v->seq, s + 2, __ATOMIC_RELEASE);
    return i32v(env, ST_OK);
}

static napi_value Stats(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2], o;
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    napi_create_object(env, &o);
    OlLink *L = host_link(env, argv[0]);
    int slot = L ? ol_check_ch(L, (int)arg_u32(env, argv[1])) : -1;
    if (slot < 0) return o;
    set(env, o, "channelState", u32v(env, ol_chan(L, slot)->state));
    set(env, o, "txRecords", num(env, (double)L->txRecords[slot]));
    set(env, o, "txBytes", num(env, (double)L->txBytes[slot]));
    set(env, o, "txSpillBytes", num(env, (double)L->spBytes[slot]));
    set(env, o, "rxMessages", num(env, (double)L->rxMessages[slot]));
    set(env, o, "rxBytes", num(env, (double)L->rxBytes[slot]));
    set(env, o, "peerSpillBytes", num(env, (double)ol_chan(L, slot)->unitySpill));
    return o;
}

static napi_value LinkState(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], o;
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    napi_create_object(env, &o);
    OlLink *L = host_link(env, argv[0]);
    if (!L) return o;
    OlHeader *h = ol_hdr(L);
    set(env, o, "unityState", u32v(env, h->unityState));
    set(env, o, "hostState", u32v(env, h->hostState));
    set(env, o, "creatorAbi", u32v(env, h->abi));
    set(env, o, "hostAbi", u32v(env, h->hostAbi));
    set(env, o, "creatorPid", u32v(env, h->creatorPid));
    set(env, o, "hostPid", u32v(env, h->hostPid));
    set(env, o, "linkEpoch", u32v(env, h->linkEpoch));
    set(env, o, "unityBeatNs", num(env, (double)h->unityBeatNs));
    set(env, o, "nowNs", num(env, (double)ol_now_ns()));
    return o;
}

static napi_value WaiterStats(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1], o;
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    napi_create_object(env, &o);
    OlLink *L = host_link(env, argv[0]);
    if (!L) return o;
    set(env, o, "wakes", num(env, (double)L->wakes));
    set(env, o, "armed", i32v(env, L->armed));
    set(env, o, "running", i32v(env, L->waiterRunning));
    return o;
}

static napi_value NextRequestId(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    CHECK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
    OlLink *L = host_link(env, argv[0]);
    return u32v(env, L ? ol_next_req(L, DIR_H2U) : 0);
}

static napi_value Init(napi_env env, napi_value exports) {
    struct { const char *name; napi_callback fn; } fns[] = {
        { "attach", Attach }, { "bindThread", BindThread }, { "startWaiter", StartWaiter }, { "beat", Beat },
        { "beginWake", BeginWake }, { "arm", Arm }, { "channelInfo", ChannelInfo }, { "take", Take },
        { "writeBytes", WriteBytes }, { "writeText", WriteText }, { "writeText2", WriteText2 }, { "flushSpill", FlushSpill },
        { "closeChannel", CloseChannel }, { "detach", Detach }, { "setViewState", SetViewState }, { "stats", Stats },
        { "linkState", LinkState }, { "waiterStats", WaiterStats }, { "nextRequestId", NextRequestId },
    };
    for (size_t i = 0; i < sizeof fns / sizeof fns[0]; i++) {
        napi_value f;
        napi_create_function(env, fns[i].name, NAPI_AUTO_LENGTH, fns[i].fn, NULL, &f);
        set(env, exports, fns[i].name, f);
    }
    set(env, exports, "abiVersion", u32v(env, OL_ABI));
    set(env, exports, "platform", utf8(env, (const uint8_t *)"linux", 5));
    return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
