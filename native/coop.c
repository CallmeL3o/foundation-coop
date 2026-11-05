// coop.dll: test-only native co-op layer for Foundation build 26138506 (foundation.exe 1.12.0.16).
// Step A: hook GamCGame::updateFrame and log per frame: frame number, dt, RNG seed, queued player commands.
// Loaded by launch.exe (APC before the exe's entry point). Does nothing unless FCOOP_INSTANCE_DIR is set
// and the code bytes at the hook site match this build.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

// ---- build-specific addresses (RVA = VA - 0x140000000), from Ghidra on build 26138506 -------------
#define RVA_UPDATE_FRAME      0x1c2bf0u   // void GamCGame::updateFrame(GamCGame*, float dt, uint32_t)
#define RVA_FRAME_COUNTER     0x3fcff48u  // int, incremented at the end of updateFrame
#define RVA_COMP_REGISTRY     0x3fd4e28u  // ComCRefManager<GamCComponent>: vector of 16-byte slots {int gen; pad; ptr}
static const uint8_t EXPECT_UPDATE_FRAME[15] = { 0x48,0x89,0x5c,0x24,0x10, 0x48,0x89,0x74,0x24,0x18, 0x57, 0x48,0x83,0xec,0x70 };
// GamCGame field offsets
#define G_REPLAY_MODE   0x3d8   // int
#define G_SEED          0x3e0   // uint64
#define G_CMD_LINKOFS   0x3e8   // pointer to link-offset table (next pointer at [tbl + 8 + node])
#define G_CMD_COUNT     0x3f0
#define G_CMD_HEAD      0x3f8
#define G_CMD_TAIL      0x400

// engine functions used for command transport (step B)
#define RVA_PUSH_PLAYER_COMMAND 0x1b3210u  // void pushPlayerCommand(GamCGame*, GamIPlayerCommand*)
#define RVA_READ_COMMAND        0x294bf0u  // GamIPlayerCommand* readCommand(GamCReplayFrame*, world, reader_holder*)
#define RVA_BIN_WRITER_CTOR     0x126b20u  // ComCBinarySerializerWriter* ctor(mem[0x70], char)
#define RVA_BIN_READER_CTOR     0x126a20u  // ComCBinarySerializerReader* ctor(mem[0x58])
#define G_REPLAY        0x3c8   // replay object pointer: updateFrame calls the recorder for every executed command when non-null
#define G_WORLD         0x1d0
#define R_WRITER_HOLDER 0x30    // replay: pointer to { writer* }
#define R_CUR_FRAME     0x50    // replay: current frame (tail of the frame list)
#define F_REC_IDS       0x48    // frame: std::vector<uint64> of written node ids (recording)
#define F_PLAY_IDS      0x30    // frame: node ids for playback (begin, end)
#define F_PLAY_INDEX    0x78    // frame: uint32 read index
#define F_ID_LIST       0x60    // frame: std::list<GUID> ids created this frame (+0x68 size, +0x70 playback cursor)
#define WRITER_GET_BYTES_SLOT 3 // bool getBytes(writer, std::vector<uint8>*)
#define READER_LOAD_SLOT      5 // bool loadFromBuffer(reader, const void*, size_t)
#define READER_FIND_SLOT     12 // +0x60: find(reader, out[2], nodeId, key*, type)  (as used by readCommand)
#define READER_READSTR_SLOT  31 // +0xf8: bool readString(reader, handle, out*)
#define RVA_KEY_TYPENAME 0x3fd28b0u // global key object used for the command type-name attribute

typedef void (*update_frame_fn)(void *game, float dt, uint32_t arg3);
typedef void (*push_cmd_fn)(void *game, void *cmd);
typedef void *(*read_cmd_fn)(void *frame, void *world, void **reader_holder);
typedef void *(*ctor_fn)(void *mem, ...);
typedef char (*get_bytes_fn)(void *writer, void *vec, void *status); // status: out struct, written at +4 by the compressor
typedef char (*load_buf_fn)(void *reader, const void *data, size_t size);
typedef void (*find_fn)(void *reader, int64_t out[2], uint64_t node, void *key, int type);
typedef char (*readstr_fn)(void *reader, int64_t handle, void **out);
static update_frame_fn g_orig;
static uint8_t *g_base;
static FILE *g_log;
static wchar_t g_dir[MAX_PATH];
static uint64_t g_frames_seen;

static void logf_(const char *fmt, ...) {
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}
static void hexline(const char *tag, const uint8_t *p, size_t n) {
    char b[200]; size_t k = 0;
    for (size_t i = 0; i < n && i < 48 && k < sizeof b - 4; i++) k += snprintf(b + k, sizeof b - k, "%02x", p[i]);
    logf_("%s %s", tag, b);
}

// MSVC x64 RTTI: vtable[-1] -> CompleteObjectLocator; COL+0x0C = RVA of TypeDescriptor; TypeDescriptor+0x10 = decorated name
static int safe_read(const void *src, void *dst, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), src, dst, n, &got) && got == n;
}
static const char *rtti_name(void *obj) {
    uint8_t *vt, *col; uint32_t td_rva;
    if (!safe_read(obj, &vt, sizeof vt)) return "?obj";
    if (!safe_read(vt - sizeof(void *), &col, sizeof col)) return "?vt";
    if (!safe_read(col + 0x0c, &td_rva, sizeof td_rva)) return "?col";
    return (const char *)(g_base + td_rva + 0x10);
}

// ---- transport (step B): role from FCOOP_ROLE = "send" (instance B) or "recv" (instance A) -------------
#include <winsock2.h>
#define PKT_MAGIC 0x50434f46u  // "FOCP"
// FCOOP_ROLE: "a" (listen 27800, send to 27801) and "b" (listen 27801, send to 27800) are symmetric peers;
// "send" / "recv" are the old one-way test roles. Localhost only.
static int g_role;            // nonzero when any networking is active
static int g_send, g_recv;
static unsigned short g_my_port = 27800, g_peer_port = 27800;
static int g_ls_role;         // lockstep: 1 host, 2 client (see the lockstep section)
static SOCKET g_sock = INVALID_SOCKET;
static uint8_t *g_fake_replay, *g_fake_frame;
static void **g_writer_holder;
static void *g_writer;          // kept separately: the engine may free the holder
// Memory the engine may own or free must come from the engine's CRT heap (ucrtbase), not this DLL's runtime.
typedef void *(*calloc_fn)(size_t, size_t);
static calloc_fn eng_calloc;
static void *ecalloc(size_t n) {
    if (!eng_calloc) eng_calloc = (calloc_fn)GetProcAddress(GetModuleHandleW(L"ucrtbase.dll"), "calloc");
    return eng_calloc(1, n);
}

#include "lsnet.h"

static void net_init(void) {
    if (g_ls_role) { lsn_init(g_ls_role); return; }   // lockstep: authenticated TCP (lsnet.h), no UDP socket
    if (g_sock != INVALID_SOCKET || !g_role) return;
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    u_long nb = 1; ioctlsocket(g_sock, FIONBIO, &nb);
    if (g_recv || g_ls_role) {     // lockstep host binds 27800 (acks), client 27801 (frames)
        struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(g_my_port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int r = bind(g_sock, (struct sockaddr *)&a, sizeof a);
        logf_("recv: bind 127.0.0.1:%d -> %d", g_my_port, r);
        int big = 8 << 20; setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (const char *)&big, sizeof big);
    }
}

// sender: arm a fake replay object so the engine serializes every command it executes this frame into our writer
static void send_arm(uint8_t *g) {
    if (!g_fake_replay) { g_fake_replay = ecalloc(0x400); g_fake_frame = ecalloc(0x200); }
    memset(g_fake_frame, 0, 0x200);
    // frame+0x60: std::list<GUID> of object ids created this frame (FUN_1401a5e30 appends while a replay is attached).
    // MSVC layout { head sentinel*, size }; sentinel node { next, prev, value[16] } pointing at itself.
    void **sentinel = ecalloc(0x20);
    sentinel[0] = sentinel; sentinel[1] = sentinel;
    *(void **)(g_fake_frame + F_ID_LIST) = sentinel;
    *(uint64_t *)(g_fake_frame + F_ID_LIST + 8) = 0;
    void *w = ecalloc(0x100);
    ((ctor_fn)(g_base + RVA_BIN_WRITER_CTOR))(w, 1);
    g_writer_holder = ecalloc(0x10);   // the engine frees this 16-byte holder after writing (MSVC sized delete 0x10)
    g_writer_holder[0] = w;
    g_writer = w;
    *(void **)(g_fake_replay + R_WRITER_HOLDER) = g_writer_holder;
    *(void **)(g_fake_replay + R_CUR_FRAME) = g_fake_frame;
    *(void **)(g + G_REPLAY) = g_fake_replay;
}

static void send_flush(uint8_t *g, int frame) {
    *(void **)(g + G_REPLAY) = NULL;
    void *w = g_writer; g_writer = NULL;
    if (!w) return;
    uint64_t *ids_b = *(uint64_t **)(g_fake_frame + F_REC_IDS), *ids_e = *(uint64_t **)(g_fake_frame + F_REC_IDS + 8);
    size_t nids = ids_b ? (size_t)(ids_e - ids_b) : 0;
    // getBytes only serializes the writer's top-level node list (head at writer+0x58, next link at node+4).
    // The recorder leaves its command nodes detached, so chain them in, in order.
    {
        uint8_t *buf = *(uint8_t **)((uint8_t *)w + 0x38);
        for (size_t i = 0; i < nids; i++)
            *(uint32_t *)(buf + ids_b[i] + 4) = (i + 1 < nids) ? (uint32_t)ids_b[i + 1] : 0;
        if (nids) *(uint64_t *)((uint8_t *)w + 0x58) = ids_b[0];
    }
    struct { uint8_t *b, *e, *c; } vec = {0};
    uint32_t status[8] = {0};
    char ok = ((get_bytes_fn)((*(void ***)w)[WRITER_GET_BYTES_SLOT]))(w, &vec, status);
    size_t nbytes = vec.b ? (size_t)(vec.e - vec.b) : 0;
    logf_("send: frame %d serialized %zu command(s), %zu bytes, ok=%d status %u/%u, new object ids this frame %llu", frame, nids, nbytes, ok, status[0], status[1],
          (unsigned long long)*(uint64_t *)(g_fake_frame + F_ID_LIST + 8));
    if (!ok || !nids || nids > 64 || nbytes > 60000) return;
    static uint8_t pkt[65536];
    size_t n = 0;
    *(uint32_t *)(pkt + n) = PKT_MAGIC; n += 4;
    *(uint32_t *)(pkt + n) = (uint32_t)nids; n += 4;
    memcpy(pkt + n, ids_b, nids * 8); n += nids * 8;
    memcpy(pkt + n, vec.b, nbytes); n += nbytes;
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(g_peer_port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int r = sendto(g_sock, (const char *)pkt, (int)n, 0, (struct sockaddr *)&a, sizeof a);
    logf_("send: packet %zu bytes -> %d, node ids[0]=%llu", n, r, (unsigned long long)ids_b[0]);
    hexline("send: bytes", vec.b, nbytes);
    {   // full packet payload to <instance>\pkt_<frame>.bin for offline analysis
        wchar_t pth[MAX_PATH]; _snwprintf(pth, MAX_PATH - 1, L"%ls\\pkt_%d.bin", g_dir, frame); pth[MAX_PATH - 1] = 0;
        FILE *pf = _wfopen(pth, L"wb"); if (pf) { fwrite(vec.b, 1, nbytes, pf); fclose(pf); }
        uint8_t *wd = NULL; uint32_t wsz = 0;
        if (safe_read((uint8_t *)w + 0x38, &wd, 8) && wd) {   // writer's raw data buffer, 256 bytes
            static uint8_t raw[256];
            if (safe_read(wd, raw, sizeof raw)) {
                _snwprintf(pth, MAX_PATH - 1, L"%ls\\wraw_%d.bin", g_dir, frame);
                pf = _wfopen(pth, L"wb"); if (pf) { fwrite(raw, 1, sizeof raw, pf); fclose(pf); }
            }
        }
        (void)wsz;
    }
    // diagnostics: compare the writer's data buffer with what a reader reconstructs from the same bytes
    {
        uint8_t *wdata = NULL, dump[64];
        if (safe_read((uint8_t *)w + 0x38, &wdata, 8) && wdata && safe_read(wdata, dump, 64)) hexline("diag: writer data", dump, 64);
        void *rd = ecalloc(0x100);
        ((ctor_fn)(g_base + RVA_BIN_READER_CTOR))(rd);
        char lok = ((load_buf_fn)((*(void ***)rd)[READER_LOAD_SLOT]))(rd, vec.b, nbytes);
        uint8_t *rdata = NULL; uint64_t f[11] = {0};
        safe_read(rd, f, sizeof f);
        logf_("diag: reader load ok=%d fields 08=%llx 10=%llx 18=%llx 30=%llx 38=%llx 40=%llx 48=%llx 50=%llx", lok,
              (unsigned long long)f[1], (unsigned long long)f[2], (unsigned long long)f[3], (unsigned long long)f[6],
              (unsigned long long)f[7], (unsigned long long)f[8], (unsigned long long)f[9], (unsigned long long)f[10]);
        rdata = (uint8_t *)f[6];
        if (rdata && safe_read(rdata, dump, 64)) hexline("diag: reader data", dump, 64);
        uint64_t st[3] = {0};
        if (f[1] && safe_read((void *)f[1], st, 24)) logf_("diag: reader string[0] = {%llx %llx %llx}", (unsigned long long)st[0], (unsigned long long)st[1], (unsigned long long)st[2]);
    }
    // writer and byte vector are leaked on purpose in this prototype (engine allocator ownership unknown)
}

// receiver: turn received packets back into command objects and push them as if the local player had clicked
static void recv_create_building(const uint8_t asset_guid_[16]);
static int g_in_recv;
// lockstep (defined further down)
static int g_ls_role, g_ls_go, g_ls_started, g_ls_desyncs, g_ls_qn;
static uint8_t *g_play_replay;
static uint64_t g_ls_expect_seed1; static uint32_t g_ls_cur_seq;
static uint32_t g_rand0, g_ls_expect_rand1; static int g_ls_rand_desyncs;   // lockstep rand() (see hook_rand)
static int g_dev;           // FCOOP_DEV=1: test diagnostics (creation log, lot dumps, RNG caller log)
static int g_local_depth;   // >0 while a known non-simulation system runs inside the frame (see hook_pool_spawn)
static int g_ui_depth;      // >0 while a replicated UI call is applied (no capture)
#define EV_XF_MATRIX 4
#define EV_XF_MATRIX_SIZE (24 + 64)
// the engine's second global random generator: 64-bit LCG, state at 0x143fce520 (FUN_140163e10, 254 callers, e.g. the
// house lot seed in FUN_141315040). Synced like the game seed: host state at frame start goes with the frame.
#define RVA_GLOBAL_RNG 0x3fce520u
static uint64_t g_grng0, g_ls_expect_grng1; static int g_ls_grng_desyncs;
// Other threads draw from the same global while the frame runs, so the simulation thread gets its own copy:
// next(state*) FUN_140163e10 and range(state*, n) FUN_1401633d0 are redirected to g_ls_grng for the global state.
static uint64_t g_ls_grng = 0x9E3779B97F4A7C15ull;
static volatile int g_in_frame;
// UI createBuilding as a lockstep event (defined with the GUID hook / lockstep section)
static int g_cb_mode; static uint8_t g_cb_ids[16 * 4096]; static uint32_t g_cb_n, g_cb_i;
static uint8_t g_ev[900000]; static uint32_t g_ev_n, g_ev_bytes;
static uint8_t *g_game;
static uint64_t g_cnt_create, g_cnt_guid, g_cnt_guid_in_create;   // set while the original updateFrame runs (main thread)
static void ls_check_go(void);
static void ls_host_flush(uint8_t *g, float dt, uint32_t arg3, uint64_t seed0);
static void ls_client_drain(void);
static void ls_host_throttle(void);
static void ls_client_ack(void);
// ordered GUID stream (defined with the GUID generator hook)
static uint8_t *g_id_buf; static size_t g_id_n, g_id_cap, g_id_head;
static void id_push(const uint8_t g16[16]);
static void sigq_push(uint64_t sig, const uint8_t g16[16], uint32_t seq);
static void recv_poll(uint8_t *g, int frame) {
    static uint8_t pkt[65536];
    for (int k = 0; k < 8; k++) {
        int n = recv(g_sock, (char *)pkt, sizeof pkt, 0);
        if (n == 20 && *(uint32_t *)pkt == 0x42434f46u) { recv_create_building(pkt + 4); continue; }  // "FOCB"
        if (n <= 8 || *(uint32_t *)pkt != PKT_MAGIC) return;
        uint32_t nids = *(uint32_t *)(pkt + 4);
        if (nids == 0 || nids > 64 || 8 + nids * 8 > (uint32_t)n) return;
        uint64_t *ids = ecalloc(nids * 8); memcpy(ids, pkt + 8, nids * 8);
        size_t nbytes = n - 8 - nids * 8;
        void *rd = ecalloc(0x100);
        ((ctor_fn)(g_base + RVA_BIN_READER_CTOR))(rd);
        char ok = ((load_buf_fn)((*(void ***)rd)[READER_LOAD_SLOT]))(rd, pkt + 8 + nids * 8, nbytes);
        logf_("recv: frame %d packet %d bytes, %u command(s), load ok=%d, node ids[0]=%llu", frame, n, nids, ok, (unsigned long long)ids[0]);
        hexline("recv: bytes", pkt + 8 + nids * 8, nbytes);
        if (!ok) return;
        // node ids on the reader side: walk the reader's top-level list (head reader+0x40, next at data+id+4)
        {
            uint8_t *rdata = *(uint8_t **)((uint8_t *)rd + 0x30);
            uint32_t rsize = *(uint32_t *)((uint8_t *)rd + 0x38);
            uint64_t id = *(uint64_t *)((uint8_t *)rd + 0x40);
            uint32_t k = 0;
            while (id && id + 8 <= rsize && k < nids) { ids[k++] = id; id = *(uint32_t *)(rdata + id + 4); }
            logf_("recv: reader top-level nodes %u (sender said %u), first %llu, data size %u", k, nids, (unsigned long long)ids[0], rsize);
            if (k == 0) return;
            nids = k;
        }
        // check that the type-name attribute is found before letting the engine instantiate anything
        int64_t found[2] = {0, 0}; void *sv = NULL;
        ((find_fn)((*(void ***)rd)[READER_FIND_SLOT]))(rd, found, ids[0], g_base + RVA_KEY_TYPENAME, 7);
        char got = found[0] ? ((readstr_fn)((*(void ***)rd)[READER_READSTR_SLOT]))(rd, found[0], &sv) : 0;
        logf_("recv: type-name attribute handle %llx read=%d", (unsigned long long)found[0], got);
        if (!found[0] || !got) { logf_("recv: skipping packet (type name not found)"); return; }
        uint8_t *fr = ecalloc(0x200);
        *(uint64_t **)(fr + F_PLAY_IDS) = ids; *(uint64_t **)(fr + F_PLAY_IDS + 8) = ids + nids;
        *(uint32_t *)(fr + F_PLAY_INDEX) = 0;
        // reader context: [0] = binary reader, +0x28 = GUID->object map used to resolve object references
        // (FUN_1402993e0 -> unordered_map find FUN_140185400). Test version: a valid EMPTY MSVC unordered_map,
        // so references to objects A does not know resolve to null instead of crashing.
        void **holder = ecalloc(0x80); holder[0] = rd;
        {
            uint8_t *map = ecalloc(0x40);
            void **sentinel = ecalloc(0x30); sentinel[0] = sentinel; sentinel[1] = sentinel;
            void **buckets = ecalloc(8 * 16);
            for (int b = 0; b < 16; b++) buckets[b] = sentinel;      // 8 buckets x {first,last}
            *(void **)(map + 0x08) = sentinel;                        // list head (end marker)
            *(uint64_t *)(map + 0x10) = 0;                            // size
            *(void **)(map + 0x18) = buckets;                         // bucket vector begin
            *(void **)(map + 0x20) = buckets + 16;                    // end
            *(void **)(map + 0x28) = buckets + 16;                    // capacity
            *(uint64_t *)(map + 0x30) = 7;                            // mask
            *(uint64_t *)(map + 0x38) = 8;                            // max index
            holder[5] = map;                                          // holder + 0x28
        }
        for (uint32_t i = 0; i < nids; i++) {
            g_in_recv = 1;
            void *cmd = ((read_cmd_fn)(g_base + RVA_READ_COMMAND))(fr, *(void **)(g + G_WORLD), holder);
            g_in_recv = 0;
            logf_("recv:   command %u -> %p %s", i, cmd, cmd ? rtti_name(cmd) : "(null)");
            // execute directly like updateFrame does (vtable slot 9 = execute(world), slot 1 = deleting destructor),
            // so peer commands never enter the local queue and are never echoed back by the sender path
            if (cmd) {
                void **vt = *(void ***)cmd;
                ((void (*)(void *, void *))vt[9])(cmd, *(void **)(g + G_WORLD));
                ((void (*)(void *, int))vt[1])(cmd, 1);
            }
        }
        fflush(g_log);
    }
}

static void ls_host_apply_deferred(void);
// CRT rand()/srand() (~90 callers, among them house lot generation) keep per-process state that nothing syncs.
// Inside the simulation frame on the simulation thread, rand() comes from a lockstep generator whose state the host
// sends with every frame; srand() there is ignored. Everywhere else the real CRT functions run.
#define IAT_RAND  0x2c67df0u
#define IAT_SRAND 0x2c67e00u
typedef int (*rand_fn)(void);
typedef void (*srand_fn)(unsigned);
static rand_fn g_orig_rand; static srand_fn g_orig_srand;
static uint32_t g_ls_rand = 0x2545F491u;
static DWORD g_sim_tid;
static uint64_t g_rand_calls;
static int hook_rand(void) {
    if (g_ls_go && g_in_frame && !g_local_depth && GetCurrentThreadId() == g_sim_tid) {
        g_ls_rand = g_ls_rand * 214013u + 2531011u; g_rand_calls++;
        return (int)((g_ls_rand >> 16) & 0x7fff);
    }
    return g_orig_rand();
}
static void hook_srand(unsigned seed) {
    if (g_ls_go && g_in_frame && GetCurrentThreadId() == g_sim_tid) return;
    g_orig_srand(seed);
}
// diagnostics: who draws OS randomness inside the frame (besides the GUID generator)
#define IAT_BCRYPTGENRANDOM 0x2c67e18u
#define IAT_CRYPTGENRANDOM  0x2c66048u
typedef LONG (WINAPI *bcgr_fn)(void *alg, unsigned char *buf, ULONG n, ULONG flags);
typedef BOOL (WINAPI *cgr_fn)(ULONG_PTR prov, DWORD n, BYTE *buf);
static bcgr_fn g_orig_bcgr; static cgr_fn g_orig_cgr;
static void log_rng_caller(const char *what, ULONG n) {
    if (!(g_ls_go && g_in_frame && GetCurrentThreadId() == g_sim_tid)) return;
    static uintptr_t seen[64]; static int ns;
    void *bt[8]; USHORT nfr = RtlCaptureStackBackTrace(1, 8, bt, NULL);
    char sig[160]; int k = 0, m = 0; uintptr_t key = 0;
    for (USHORT i = 0; i < nfr && m < 6; i++) {
        uintptr_t a = (uintptr_t)bt[i];
        if (a >= (uintptr_t)g_base && a < (uintptr_t)g_base + 0x5000000) {
            k += snprintf(sig + k, sizeof sig - k, " %06llx", (unsigned long long)(a - (uintptr_t)g_base)); m++;
            key = key * 31 + a;
        }
    }
    for (int i = 0; i < ns; i++) if (seen[i] == key) return;
    if (ns < 64) seen[ns++] = key;
    logf_("rng: %s(%lu) in frame from%s", what, n, sig);
}
static LONG WINAPI hook_bcgr(void *alg, unsigned char *buf, ULONG n, ULONG flags) { log_rng_caller("BCryptGenRandom", n); return g_orig_bcgr(alg, buf, n, flags); }
static BOOL WINAPI hook_cgr(ULONG_PTR prov, DWORD n, BYTE *buf) { log_rng_caller("CryptGenRandom", n); return g_orig_cgr(prov, n, buf); }
static void patch_iat(uint32_t rva, void *hook, void **orig) {
    void **slot = (void **)(g_base + rva); DWORD old;
    VirtualProtect(slot, 8, PAGE_READWRITE, &old); *orig = *slot; *slot = hook; VirtualProtect(slot, 8, old, &old);
}
// Thread-pool push (FUN_1401f8c40(queue, std::function<void()>*), 148 callers, e.g. navmesh processing in the
// "Heavy Algo" module). A job submitted by the simulation inside a frame would finish on a worker at a time that
// differs per machine; in lockstep it runs right here instead (same inputs, same order, same frame on both sides).
static void *hook_at(uint32_t rva, const uint8_t *expect, size_t len, void *hook, const char *name);
#define RVA_JOB_PUSH 0x1f8c40u
static const uint8_t EXPECT_JOB_PUSH[15] = { 0x48,0x89,0x5c,0x24,0x08, 0x48,0x89,0x6c,0x24,0x10, 0x48,0x89,0x74,0x24,0x18 };
typedef void (*job_push_fn)(void *queue, void *fn);
static job_push_fn g_orig_job_push;
static uint64_t g_jobs_inline;
static void hook_job_push(void *queue, void *fn) {
    // only simulation jobs (navigation mesh / zone processing, 0x1407b0000-0x140800000); render jobs must stay on the pool
    uintptr_t ra = (uintptr_t)__builtin_return_address(0) - (uintptr_t)g_base;
    // navmesh/zone processing 0x1407b0000-0x140800000; desirability grid / residential / lots 0x1412e0000-0x141350000
    // resource containers 0x140b00000-0x140b20000; game-object jobs 0x140fa0000-0x140ff0000
    int sim_job = (ra >= 0x7b0000 && ra < 0x800000) || (ra >= 0x12e0000 && ra < 0x1350000) ||
                  (ra >= 0xb00000 && ra < 0xb20000) || (ra >= 0xfa0000 && ra < 0xff0000);
    if (g_ls_go && g_in_frame && GetCurrentThreadId() == g_sim_tid && fn && !sim_job) {
        static uintptr_t seen[32]; static int ns; int known = 0;
        for (int i = 0; i < ns; i++) if (seen[i] == ra) known = 1;
        if (!known && ns < 32) { seen[ns++] = ra; logf_("lockstep: in-frame job from %llx left on the thread pool", (unsigned long long)ra); }
    }
    if (g_ls_go && g_in_frame && GetCurrentThreadId() == g_sim_tid && fn && sim_job) {
        void *impl = *(void **)((uint8_t *)fn + 0x38);   // MSVC std::function: _Impl* at +0x38, _Do_call = vtable slot 2
        if (impl) {
            if (g_jobs_inline < 20) {
                void *bt[4]; USHORT n = RtlCaptureStackBackTrace(1, 4, bt, NULL);
                logf_("lockstep: job run inline (from %llx %llx)", n > 0 ? (unsigned long long)((uint8_t *)bt[0] - g_base) : 0ull,
                      n > 1 ? (unsigned long long)((uint8_t *)bt[1] - g_base) : 0ull);
            }
            g_jobs_inline++;
            ((void (*)(void *))(*(void ***)impl)[2])(impl);
            return;
        }
    }
    g_orig_job_push(queue, fn);
}
// diagnostics: PlaILotConstructor::build(ctor, flag, lotdata) - dump both objects to find where the lot seed lives
#define RVA_LOT_BUILD 0x12fdc50u
static const uint8_t EXPECT_LOT_BUILD[14] = { 0x48,0x89,0x5c,0x24,0x10, 0x55, 0x56, 0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56 };
typedef void (*lot_build_fn)(void *ctor, char flag, void *data);
static lot_build_fn g_orig_lot_build;
static void hook_lot_build(void *ctor, char flag, void *data) {
    g_orig_lot_build(ctor, flag, data);
    static int n;
    if (g_ls_go && n < 40) {   // dump after build: the lot (with its seed) exists now
        static uint8_t raw[0x800];
        wchar_t pth[MAX_PATH];
        if (safe_read(ctor, raw, sizeof raw)) {
            _snwprintf(pth, MAX_PATH - 1, L"%ls\\lot_%02d_ctor.bin", g_dir, n); pth[MAX_PATH - 1] = 0;
            FILE *f = _wfopen(pth, L"wb"); if (f) { fwrite(raw, 1, sizeof raw, f); fclose(f); }
        }
        logf_("lot build #%d at seq %u: ctor %p (%s)", n, g_ls_cur_seq, ctor, rtti_name(ctor));
        n++;
    }
}
#define RVA_GRNG_NEXT  0x163e10u
#define RVA_GRNG_RANGE 0x1633d0u
static const uint8_t EXPECT_GRNG_NEXT[17]  = { 0x48,0x8b,0x01, 0x48,0xba,0x65,0x89,0x07,0x6c,0x65,0x8b,0x58,0x5d, 0x48,0x0f,0xaf,0xc2 };
static const uint8_t EXPECT_GRNG_RANGE[14] = { 0x48,0x89,0x5c,0x24,0x08, 0x55, 0x56, 0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56 };
typedef uint64_t (*grng_next_fn)(uint64_t *st);
typedef uint32_t (*grng_range_fn)(uint64_t *st, uint32_t n);
static grng_next_fn g_orig_grng_next; static grng_range_fn g_orig_grng_range;
static uint64_t *grng_state(uint64_t *st) {
    if (st == (uint64_t *)(g_base + RVA_GLOBAL_RNG) && g_ls_go && g_in_frame && !g_local_depth && GetCurrentThreadId() == g_sim_tid) return &g_ls_grng;
    return st;
}
static uint64_t hook_grng_next(uint64_t *st) { return g_orig_grng_next(grng_state(st)); }
static uint32_t hook_grng_range(uint64_t *st, uint32_t n) { return g_orig_grng_range(grng_state(st), n); }
// FUN_140279840: a pooled object spawner driven by real time (weights/timers): it runs inside the frame but at a
// different frame on each machine, so everything it does is local (own GUIDs, real random generators).
#define RVA_POOL_SPAWN 0x279840u
static const uint8_t EXPECT_POOL_SPAWN[15] = { 0x48,0x89,0x5c,0x24,0x10, 0x48,0x89,0x6c,0x24,0x18, 0x48,0x89,0x74,0x24,0x20 };
typedef void (*pool_spawn_fn)(void *a, void *b);
static pool_spawn_fn g_orig_pool_spawn;
static void hook_pool_spawn(void *a, void *b) { g_local_depth++; g_orig_pool_spawn(a, b); g_local_depth--; }
// diagnostics: who sets game object transforms (to find the build tool's preview moves)
#define RVA_SET_GMATRIX 0x4cabc0u   // GameObject setGlobalMatrix(go, float m[16]) (setGlobalTransform builds the matrix)
static const uint8_t EXPECT_SET_GMATRIX[16] = { 0x48,0x8b,0xc4, 0x48,0x89,0x58,0x08, 0x48,0x89,0x78,0x10, 0x55, 0x48,0x8d,0x68,0xc8 };
typedef void (*set_gmatrix_fn)(void *go, const float *m);
static set_gmatrix_fn g_orig_set_gmatrix;
static void log_xf_caller(const char *what) {
    if (!g_ls_go) return;
    static uintptr_t seen[128]; static int ns;
    void *bt[4]; USHORT n = RtlCaptureStackBackTrace(2, 3, bt, NULL);
    uintptr_t a = n > 0 ? (uintptr_t)bt[0] - (uintptr_t)g_base : 0, b = n > 1 ? (uintptr_t)bt[1] - (uintptr_t)g_base : 0;
    uintptr_t key = a * 131 + b;
    for (int i = 0; i < ns; i++) if (seen[i] == key) return;
    if (ns < 128) seen[ns++] = key;
    logf_("xf: %s from %llx %llx%s", what, (unsigned long long)a, (unsigned long long)b, g_in_frame ? " [frame]" : " [outside]");
}
// game object by GUID (registry scan, small cache)
static void *find_object_by_guid(const uint8_t guid[16]) {
    static uint8_t cg[8][16]; static void *co[8]; static int ci;
    for (int i = 0; i < 8; i++) if (co[i] && !memcmp(cg[i], guid, 16)) {
        uint8_t og[16]; if (safe_read((uint8_t *)co[i] + 0x1d0, og, 16) && !memcmp(og, guid, 16)) return co[i];
    }
    uint8_t **reg = (uint8_t **)(g_base + RVA_COMP_REGISTRY);
    for (uint8_t *sl = reg[0]; sl && sl < reg[1]; sl += 16) {
        uint8_t *comp = *(uint8_t **)(sl + 8), *owner = NULL, og[16];
        if (!comp || !safe_read(comp + 0x28, &owner, 8) || !owner || !safe_read(owner + 0x1d0, og, 16)) continue;
        if (!memcmp(og, guid, 16)) { memcpy(cg[ci], guid, 16); co[ci] = owner; ci = (ci + 1) & 7; return owner; }
    }
    return NULL;
}
// PlaCBuildableMover (the build tool moving a preview) runs inside the frame on the player's machine only. Its matrix
// writes are deferred to the shared frame boundary like the tool's setEditMode calls.
static uint8_t g_mdefer[256][EV_XF_MATRIX_SIZE]; static int g_nmdefer;
static void ls_client_send_event(const uint8_t *e, size_t len);
static void ls_client_send_event_g(const uint8_t *e, const uint8_t *guids, uint32_t n);
static void hook_set_gmatrix(void *go, const float *m) {
    uintptr_t ra = (uintptr_t)__builtin_return_address(0) - (uintptr_t)g_base;
    int mover = (ra >= 0xc47070 && ra < 0xc471a7) || (ra >= 0xcbf4c0 && ra < 0xcc031e);
    if (mover && g_ls_go && g_in_frame && g_ui_depth == 0 && !g_local_depth && (g_ls_role == 1 || (g_ls_role == 2 && g_ls_started))) {
        uint8_t ev[EV_XF_MATRIX_SIZE] = {0};
        *(uint32_t *)ev = EV_XF_MATRIX; ev[23] = 1;
        if (safe_read((uint8_t *)go + 0x1d0, ev + 4, 16)) {
            memcpy(ev + 24, m, 64);
            if (g_ls_role == 2) { static int nlog; if (nlog++ < 5) logf_("lockstep client: preview move deferred (sent to host)"); ls_client_send_event_g(ev, NULL, 0); }
            else if (g_nmdefer < 256) memcpy(g_mdefer[g_nmdefer++], ev, sizeof ev);
            return;
        }
    }
    g_orig_set_gmatrix(go, m);
}
// PlaCCompBuilding::destroyBuilding(comp) FUN_140c85bd0 (diagnostics first: who destroys buildings, in or out of frame)
static void hexguid(char *dst, const uint8_t g[16]);
#define RVA_DESTROY_BUILDING 0xc85bd0u
static const uint8_t EXPECT_DESTROY_BUILDING[15] = { 0x48,0x89,0x5c,0x24,0x10, 0x48,0x89,0x6c,0x24,0x18, 0x48,0x89,0x74,0x24,0x20 };
typedef void (*destroy_building_fn)(void *comp);
static destroy_building_fn g_orig_destroy_building;
// The build tool cancelling a preview (FUN_140c86470 -> destroyBuilding, inside the frame, player's machine only)
// is deferred to the shared frame boundary like the tool's setEditMode calls (event type 5 = destroy building).
#define RA_TOOL_DESTROY 0xc86493u
static uint8_t g_defer[64][24]; static int g_ndefer;
static void hook_destroy_building(void *comp) {
    uintptr_t ra = (uintptr_t)__builtin_return_address(0) - (uintptr_t)g_base;
    if (g_ls_go && g_in_frame && g_ui_depth == 0 && ra == RA_TOOL_DESTROY && (g_ls_role == 1 || (g_ls_role == 2 && g_ls_started))) {
        uint8_t ev[24] = {0}, *owner = NULL;
        *(uint32_t *)ev = 5; ev[23] = 1;
        if (safe_read((uint8_t *)comp + 0x28, &owner, 8) && owner && safe_read(owner + 0x1d0, ev + 4, 16)) {
            char gs[33]; hexguid(gs, ev + 4);
            logf_("lockstep %s: tool cancel (destroyBuilding %s) deferred", g_ls_role == 1 ? "host" : "client", gs);
            if (g_ls_role == 2) ls_client_send_event_g(ev, NULL, 0);
            else if (g_ndefer < 64) memcpy(g_defer[g_ndefer++], ev, 24);
            return;
        }
    }
    g_orig_destroy_building(comp);
}
static void install_rand_hooks(void) {
    g_orig_destroy_building = (destroy_building_fn)hook_at(RVA_DESTROY_BUILDING, EXPECT_DESTROY_BUILDING, sizeof EXPECT_DESTROY_BUILDING, (void *)hook_destroy_building, "destroyBuilding");
    g_orig_set_gmatrix = (set_gmatrix_fn)hook_at(RVA_SET_GMATRIX, EXPECT_SET_GMATRIX, sizeof EXPECT_SET_GMATRIX, (void *)hook_set_gmatrix, "setGlobalMatrix");
    g_orig_pool_spawn = (pool_spawn_fn)hook_at(RVA_POOL_SPAWN, EXPECT_POOL_SPAWN, sizeof EXPECT_POOL_SPAWN, (void *)hook_pool_spawn, "poolSpawn");
    g_orig_grng_next = (grng_next_fn)hook_at(RVA_GRNG_NEXT, EXPECT_GRNG_NEXT, sizeof EXPECT_GRNG_NEXT, (void *)hook_grng_next, "globalRngNext");
    g_orig_grng_range = (grng_range_fn)hook_at(RVA_GRNG_RANGE, EXPECT_GRNG_RANGE, sizeof EXPECT_GRNG_RANGE, (void *)hook_grng_range, "globalRngRange");
    if (g_dev) g_orig_lot_build = (lot_build_fn)hook_at(RVA_LOT_BUILD, EXPECT_LOT_BUILD, sizeof EXPECT_LOT_BUILD, (void *)hook_lot_build, "lotBuild");
    g_orig_job_push = (job_push_fn)hook_at(RVA_JOB_PUSH, EXPECT_JOB_PUSH, sizeof EXPECT_JOB_PUSH, (void *)hook_job_push, "jobPush");
    if (g_dev) {
        patch_iat(IAT_BCRYPTGENRANDOM, (void *)hook_bcgr, (void **)&g_orig_bcgr);
        patch_iat(IAT_CRYPTGENRANDOM, (void *)hook_cgr, (void **)&g_orig_cgr);
    }
    void **slot_r = (void **)(g_base + IAT_RAND), **slot_s = (void **)(g_base + IAT_SRAND);
    HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    void *real_r = ucrt ? (void *)GetProcAddress(ucrt, "rand") : NULL, *real_s = ucrt ? (void *)GetProcAddress(ucrt, "srand") : NULL;
    if (!real_r || *slot_r != real_r || !real_s || *slot_s != real_s) {
        logf_("rand hooks NOT installed (IAT %p/%p, ucrt %p/%p)", *slot_r, *slot_s, real_r, real_s); return;
    }
    DWORD old;
    VirtualProtect(slot_r, 16 + (IAT_SRAND - IAT_RAND), PAGE_READWRITE, &old);
    g_orig_rand = (rand_fn)real_r; g_orig_srand = (srand_fn)real_s;
    *slot_r = (void *)hook_rand; *slot_s = (void *)hook_srand;
    VirtualProtect(slot_r, 16 + (IAT_SRAND - IAT_RAND), old, &old);
    logf_("rand hooks installed (IAT rand/srand)");
}
static void hook_update_frame(void *game, float dt, uint32_t arg3) {
    uint8_t *g = (uint8_t *)game;
    int frame = *(int *)(g_base + RVA_FRAME_COUNTER);
    net_init();
    g_game = g;
    g_sim_tid = GetCurrentThreadId();
    if (g_ls_role) {
        if ((g_frames_seen % 30) == 0) ls_check_go();
        if (g_ls_role == 1 && g_ls_go) {                       // host: record and stream every frame
            ls_host_throttle();
            uint64_t seed0 = *(uint64_t *)(g + G_SEED);
            if (*(void **)(g + G_REPLAY) == NULL && *(int *)(g + G_REPLAY_MODE) == 0) send_arm(g);
            g_rand0 = g_ls_rand;
            g_grng0 = g_ls_grng;
            g_in_frame = 1; g_orig(game, dt, arg3); g_in_frame = 0;
            ls_host_flush(g, dt, arg3, seed0);
            ls_host_apply_deferred();   // UI calls deferred out of this frame: applied now, streamed with the next frame
            g_frames_seen++;
            return;
        }
        if (g_ls_role == 2 && g_ls_go) {                       // client: replay the host's frames (engine mode 2)
            ls_client_drain();
            if (!g_ls_started && g_ls_qn > 0) {
                if (!g_play_replay) g_play_replay = ecalloc(0x400);
                *(void **)(g + G_REPLAY) = g_play_replay;
                *(int *)(g + G_REPLAY_MODE) = 2;
                g_ls_started = 1;
                logf_("lockstep client: started (playback mode 2), %d host frames queued", g_ls_qn);
            }
            if (g_ls_started) {
                g_in_recv = 1; g_in_frame = 1;
                g_orig(game, dt, arg3);
                g_in_recv = 0; g_in_frame = 0;
                ls_client_ack();
                uint64_t s = *(uint64_t *)(g + G_SEED);
                if (s != g_ls_expect_seed1 && g_ls_desyncs < 20) {
                    g_ls_desyncs++;
                    logf_("lockstep client: DESYNC at host seq %u: seed %016llx, host %016llx", g_ls_cur_seq,
                          (unsigned long long)s, (unsigned long long)g_ls_expect_seed1);
                }
                if (g_ls_grng != g_ls_expect_grng1 && g_ls_grng_desyncs < 20) {
                    g_ls_grng_desyncs++;
                    logf_("lockstep client: GLOBAL RNG DESYNC at host seq %u: %016llx, host %016llx", g_ls_cur_seq,
                          (unsigned long long)g_ls_grng, (unsigned long long)g_ls_expect_grng1);
                }
                if (g_ls_rand != g_ls_expect_rand1 && g_ls_rand_desyncs < 20) {
                    g_ls_rand_desyncs++;
                    logf_("lockstep client: RAND DESYNC at host seq %u: %08x, host %08x", g_ls_cur_seq, g_ls_rand, g_ls_expect_rand1);
                }
                if ((g_frames_seen % 600) == 0)
                    logf_("lockstep client: at host seq %u seed %016llx (host %016llx) queue %d desyncs %d", g_ls_cur_seq,
                          (unsigned long long)s, (unsigned long long)g_ls_expect_seed1, g_ls_qn, g_ls_desyncs);
                g_frames_seen++;
                return;
            }
        }
    }
    if (g_recv) recv_poll(g, frame);
    int armed = 0;
    if (g_send && *(int64_t *)(g + G_CMD_COUNT) > 0 && *(void **)(g + G_REPLAY) == NULL && *(int *)(g + G_REPLAY_MODE) == 0) {
        send_arm(g); armed = 1;
    }
    uint64_t seed_before = *(uint64_t *)(g + G_SEED);
    int64_t count = *(int64_t *)(g + G_CMD_COUNT);
    if (count > 0) {
        uint8_t *tbl = *(uint8_t **)(g + G_CMD_LINKOFS);
        char line[2048]; int n = snprintf(line, sizeof line, "frame %d dt %.5f seed %016llx cmds %lld:", frame, dt,
                                          (unsigned long long)seed_before, (long long)count);
        uint8_t *node = *(uint8_t **)(g + G_CMD_HEAD);
        for (int i = 0; node && i < 32 && n < (int)sizeof line - 128; i++) {
            const char *nm = rtti_name(node);
            n += snprintf(line + n, sizeof line - n, " %s", nm);
            if (strstr(nm, "CreateBuildingPart") || strstr(nm, "BuildMonument")) {
                uint64_t h = 0; safe_read(node + 0x50, &h, 8);
                n += snprintf(line + n, sizeof line - n, "[building handle %llx]", (unsigned long long)h);
                // diagnostics: dump the building's owner game object to find its transform fields offline
                if (strstr(nm, "CreateBuildingPart") && h) {
                    uint8_t **reg = (uint8_t **)(g_base + RVA_COMP_REGISTRY);
                    uint32_t idx = (uint32_t)h; void *comp = NULL, *owner = NULL;
                    if (reg[0] && (size_t)idx < (size_t)(reg[1] - reg[0]) / 16) safe_read(reg[0] + (size_t)idx * 16 + 8, &comp, 8);
                    if (comp) safe_read((uint8_t *)comp + 0x28, &owner, 8);
                    static uint8_t raw[0x400];
                    if (owner && safe_read(owner, raw, sizeof raw)) {
                        wchar_t pth[MAX_PATH]; _snwprintf(pth, MAX_PATH - 1, L"%ls\\owner_f%d.bin", g_dir, frame); pth[MAX_PATH - 1] = 0;
                        FILE *pf = _wfopen(pth, L"wb"); if (pf) { fwrite(raw, 1, sizeof raw, pf); fclose(pf); }
                    }
                }
            }
            node = *(uint8_t **)(tbl + 8 + (intptr_t)node);
        }
        logf_("%s", line);
    }
    g_orig(game, dt, arg3);
    if (armed) send_flush(g, frame);
    g_frames_seen++;
    if (g_frames_seen % 600 == 1)
        logf_("frame %d dt %.5f seed_after %016llx mode %d", frame, dt,
              (unsigned long long)*(uint64_t *)(g + G_SEED), *(int *)(g + G_REPLAY_MODE));
    if (count > 0 || g_frames_seen % 600 == 1) fflush(g_log);
}

// 14-byte absolute jump: FF 25 00 00 00 00 <imm64>
static void write_abs_jmp(uint8_t *at, void *to) {
    at[0] = 0xFF; at[1] = 0x25; *(uint32_t *)(at + 2) = 0; *(uint64_t *)(at + 6) = (uint64_t)to;
}

// Generic inline hook: the first `len` (>= 14) bytes at rva must equal `expect` and be position independent.
static void *hook_at(uint32_t rva, const uint8_t *expect, size_t len, void *hook, const char *name) {
    uint8_t *site = g_base + rva;
    if (len < 14 || memcmp(site, expect, len) != 0) {
        logf_("hook %s NOT installed: code bytes do not match build 26138506", name);
        return NULL;
    }
    uint8_t *tramp = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return NULL;
    memcpy(tramp, site, len);                                        // relocated prologue
    write_abs_jmp(tramp + len, site + len);
    DWORD old;
    VirtualProtect(site, len, PAGE_EXECUTE_READWRITE, &old);
    write_abs_jmp(site, hook);
    for (size_t i = 14; i < len; i++) site[i] = 0x90;                // pad
    VirtualProtect(site, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, len);
    logf_("hook %s installed", name);
    return tramp;
}

// ---- object identity (step C): observe PlaCCompBuildingManager::createBuilding ---------------------------
#define RVA_CREATE_BUILDING  0xc81540u   // COMP_BUILDING* createBuilding(manager, asset**, parentHandle*)
#define RVA_ASSET_MANAGER    0x2186c0u   // asset manager singleton getter
#define RVA_ASSET_BY_GUID    0x1a1a80u   // assetNode* find(assetManager, const guid[16]); asset object at node+0xf0
#define RVA_COMP_REGISTRY    0x3fd4e28u  // ComCRefManager<GamCComponent>: vector of 16-byte slots {int gen; pad; ptr}
static const uint8_t EXPECT_CREATE_BUILDING[15] = { 0x48,0x89,0x5c,0x24,0x08, 0x48,0x89,0x74,0x24,0x18, 0x48,0x89,0x7c,0x24,0x20 };
typedef void *(*create_building_fn)(void *mgr, void **asset, void *parent);
typedef void *(*noarg_fn)(void);
typedef void *(*asset_find_fn)(void *mgr, const uint8_t *guid);
static create_building_fn g_orig_create_building;
static void *g_building_manager;   // last manager seen (this process)

// handle of a component = (generation << 32) | slot index in the global component registry
static uint64_t component_handle(void *comp) {
    uint8_t **reg = (uint8_t **)(g_base + RVA_COMP_REGISTRY);
    uint8_t *b = reg[0], *e = reg[1];
    for (uint8_t *s = b; s && s < e; s += 16)
        if (*(void **)(s + 8) == comp) return ((uint64_t)*(uint32_t *)s << 32) | (uint64_t)((s - b) / 16);
    return 0;
}

// find the offset of an asset's GUID inside the asset object by asking the engine's own GUID lookup
static int asset_guid(void *asset, uint8_t out[16]) {
    void *am = ((noarg_fn)(g_base + RVA_ASSET_MANAGER))();
    for (int off = 8; off < 0x200; off += 4) {
        uint8_t cand[16];
        if (!safe_read((uint8_t *)asset + off, cand, 16)) return -1;
        int zero = 1; for (int i = 0; i < 16; i++) if (cand[i]) { zero = 0; break; }
        if (zero) continue;
        uint8_t *node = ((asset_find_fn)(g_base + RVA_ASSET_BY_GUID))(am, cand);
        void *obj = NULL;
        if (node && safe_read(node + 0xf0, &obj, 8) && obj == asset) { memcpy(out, cand, 16); return off; }
    }
    return -1;
}

static void hexguid(char *dst, const uint8_t g[16]) { for (int i = 0; i < 16; i++) sprintf(dst + 2 * i, "%02x", g[i]); }

#define PKT_MAGIC_CB 0x42434f46u  // "FOCB": the sender's UI created a building of asset <guid>
static int g_in_remote_create;    // receiver: building creation on behalf of the peer (do not echo)

// receiver: buildings created for the peer that are not yet bound to the peer's GUID, and the bindings
#define MAX_BIND 256
static uint64_t g_pending[MAX_BIND]; static int g_npending;
static struct { uint8_t guid[16]; uint64_t handle; } g_bind[MAX_BIND]; static int g_nbind;

static void send_create_building(const uint8_t asset_guid_[16]) {
    uint8_t pkt[4 + 16];
    *(uint32_t *)pkt = PKT_MAGIC_CB; memcpy(pkt + 4, asset_guid_, 16);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(g_peer_port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int r = sendto(g_sock, (const char *)pkt, sizeof pkt, 0, (struct sockaddr *)&a, sizeof a);
    logf_("send: CB packet -> %d", r);
}

// lockstep client: send one of the local player's UI events to the host (which applies it, the client already has)
static void ls_client_send_event(const uint8_t *e, size_t len) {
    static uint8_t pkt[4 + 128 + 4 + 16 * 4096];
    if (len > sizeof pkt - 4) return;
    *(uint32_t *)pkt = 0x454c4f46u; memcpy(pkt + 4, e, len);   // "FOLE"
    int r = lsn_send(LSN_UIEV, pkt, (uint32_t)(4 + len));
    logf_("lockstep client: sent own UI event %u (%zu bytes) to host -> %d", *(const uint32_t *)e, len, r);
}

static void *hook_create_building(void *mgr, void **asset, void *parent) {
    // lockstep: a UI-created building becomes an event with exactly the GUIDs this creation generated
    // (host: streamed to the client with the next frame; client: sent to the host right away)
    // only the player's UI creates buildings outside the frame; inside the frame it is the simulation itself (e.g. houses
    // in a residential zone), which both sides run, with object GUIDs from the frame stream
    int capture = (g_ls_go && (g_ls_role == 1 || (g_ls_role == 2 && g_ls_started)) && !g_in_remote_create && g_cb_mode == 0 && !g_in_frame);
    if (capture) { g_cb_mode = 1; g_cb_n = 0; }
    void *comp = g_orig_create_building(mgr, asset, parent);
    if (capture) {
        g_cb_mode = 0;
        uint8_t ag[16];
        static uint8_t ev[24 + 16 * 4096];
        if (asset && *asset && asset_guid(*asset, ag) >= 0) {
            *(uint32_t *)ev = 1; memcpy(ev + 4, ag, 16); *(uint32_t *)(ev + 20) = g_cb_n; memcpy(ev + 24, g_cb_ids, 16 * g_cb_n);
            size_t len = 24 + 16 * (size_t)g_cb_n;
            if (g_ls_role == 2) ls_client_send_event(ev, len);
            else if (g_ev_bytes + len <= sizeof g_ev) {
                memcpy(g_ev + g_ev_bytes, ev, len); g_ev_bytes += (uint32_t)len; g_ev_n++;
                logf_("lockstep host: UI createBuilding captured as event (%u guids)", g_cb_n);
            }
        }
    }
    g_building_manager = mgr;
    void *a = asset ? *asset : NULL;
    uint8_t ag[16] = {0}; char ags[33] = "?";
    int aoff = a ? asset_guid(a, ag) : -1;
    if (aoff >= 0) hexguid(ags, ag);
    uint64_t h = comp ? component_handle(comp) : 0;
    logf_("createBuilding: manager %p asset %p (%s, guid@+0x%x %s) parent %p -> comp %p (%s) handle %llx%s%s",
          mgr, a, a ? rtti_name(a) : "-", aoff, ags, parent, comp, comp ? rtti_name(comp) : "-",
          (unsigned long long)h, g_in_remote_create ? " [for peer]" : "", g_in_frame ? " [inside updateFrame]" : " [outside updateFrame]");
    if (g_send && !g_in_remote_create && aoff >= 0 && g_sock != INVALID_SOCKET) send_create_building(ag);
    if (comp) {   // dump the component so its GUID offset can be matched against command packets offline
        static int n; uint8_t raw[0x200];
        if (safe_read(comp, raw, sizeof raw)) {
            wchar_t pth[MAX_PATH]; _snwprintf(pth, MAX_PATH - 1, L"%ls\\comp_building_%d.bin", g_dir, n++); pth[MAX_PATH - 1] = 0;
            FILE *pf = _wfopen(pth, L"wb"); if (pf) { fwrite(raw, 1, sizeof raw, pf); fclose(pf); }
        }
    }
    return comp;
}

// receiver: create the building the peer's UI created (same asset), remember its handle as pending
static void recv_create_building(const uint8_t asset_guid_[16]) {
    char gs[33]; hexguid(gs, asset_guid_);
    void *am = ((noarg_fn)(g_base + RVA_ASSET_MANAGER))();
    uint8_t *node = ((asset_find_fn)(g_base + RVA_ASSET_BY_GUID))(am, asset_guid_);
    void *asset = NULL;
    if (node) safe_read(node + 0xf0, &asset, 8);
    if (!asset || !g_building_manager || !g_orig_create_building) {
        logf_("recv: CB asset %s -> asset %p manager %p: cannot create", gs, asset, g_building_manager);
        return;
    }
    uint64_t parent[2] = {0, 0};
    g_in_remote_create = 1;
    void *comp = g_orig_create_building(g_building_manager, &asset, parent);
    g_in_remote_create = 0;
    uint64_t h = comp ? component_handle(comp) : 0;
    logf_("recv: CB asset %s (%s) -> created %p handle %llx", gs, rtti_name(asset), comp, (unsigned long long)h);
    // the reference resolver must return the owning GAME OBJECT pointer (component+0x28); the engine then looks up
    // the component of the property's type on it (FUN_14028e8d0) and derives the weak handle itself
    void *owner = NULL;
    if (comp) safe_read((uint8_t *)comp + 0x28, &owner, 8);
    logf_("recv: CB building owner object %p (%s)", owner, owner ? rtti_name(owner) : "-");
    if (owner) {   // diagnostics: dump the owner to find the GUID offset (compare with Lua getId())
        static int n; static uint8_t raw[0x400];
        if (safe_read(owner, raw, sizeof raw)) {
            wchar_t pth[MAX_PATH]; _snwprintf(pth, MAX_PATH - 1, L"%ls\\peer_owner_%d.bin", g_dir, n++); pth[MAX_PATH - 1] = 0;
            FILE *pf = _wfopen(pth, L"wb"); if (pf) { fwrite(raw, 1, sizeof raw, pf); fclose(pf); }
        }
    }
    if (owner && g_npending < MAX_BIND) g_pending[g_npending++] = (uint64_t)owner;
}

// receiver: object reference resolver hook (FUN_1402993e0(objReader, propertyNode, char *missing) -> handle).
// While a peer command is being deserialized, GUIDs this process does not know are bound to buildings created for the peer.
#define RVA_RESOLVE_REF   0x2993e0u
#define RVA_KEY_VALUE     0x3fd2a00u   // key object of the GUID attribute
#define READER_READGUID_SLOT 34        // +0x110: bool readGuid(reader, handle, out[16])
static const uint8_t EXPECT_RESOLVE_REF[15] = { 0x48,0x89,0x5c,0x24,0x10, 0x48,0x89,0x6c,0x24,0x18, 0x48,0x89,0x74,0x24,0x20 };
typedef uint64_t (*resolve_fn)(void *objreader, uint64_t node, char *missing);
typedef char (*readguid_fn)(void *reader, int64_t handle, uint8_t out[16]);
static resolve_fn g_orig_resolve;
static int g_in_recv;

// live object with this GUID: scan the component registry for a component whose owner game object has the GUID (+0x1d0)
static void *find_live_object(const uint8_t guid[16]) {
    uint8_t **reg = (uint8_t **)(g_base + RVA_COMP_REGISTRY);
    for (uint8_t *s = reg[0]; s && s < reg[1]; s += 16) {
        uint8_t *comp = *(uint8_t **)(s + 8), *owner = NULL, og[16];
        if (!comp || !safe_read(comp + 0x28, &owner, 8) || !owner) continue;
        if (safe_read(owner + 0x1d0, og, 16) && !memcmp(og, guid, 16)) return owner;
    }
    return NULL;
}

static uint64_t hook_resolve(void *objreader, uint64_t node, char *missing) {
    uint64_t r = g_orig_resolve(objreader, node, missing);
    if (!g_in_recv || r != 0) return r;
    void *reader = *(void **)((uint8_t *)objreader + 8);
    int64_t found[2] = {0, 0}; uint8_t guid[16] = {0};
    ((find_fn)((*(void ***)reader)[READER_FIND_SLOT]))(reader, found, node, g_base + RVA_KEY_VALUE, 8);
    if (!found[0] || !((readguid_fn)((*(void ***)reader)[READER_READGUID_SLOT]))(reader, found[0], guid)) return r;
    char gs[33]; hexguid(gs, guid);
    for (int i = 0; i < g_nbind; i++)
        if (!memcmp(g_bind[i].guid, guid, 16)) { *missing = 0; logf_("resolve: %s -> bound handle %llx", gs, (unsigned long long)g_bind[i].handle); return g_bind[i].handle; }
    void *live = find_live_object(guid);   // same GUID exists here (shared map objects, lockstep-created objects)
    if (live) { *missing = 0; logf_("resolve: %s -> live object %p", gs, live); return (uint64_t)live; }
    if (g_npending > 0 && g_nbind < MAX_BIND) {
        uint64_t h = g_pending[0];
        memmove(g_pending, g_pending + 1, (size_t)(--g_npending) * sizeof g_pending[0]);
        memcpy(g_bind[g_nbind].guid, guid, 16); g_bind[g_nbind].handle = h; g_nbind++;
        *missing = 0;
        logf_("resolve: peer guid %s -> new binding to pending building handle %llx", gs, (unsigned long long)h);
        // publish "peerGuid localGuid" for the Lua layer (game object GUID at +0x1d0, same byte order as Lua getId())
        uint8_t lg[16]; char lgs[33];
        if (safe_read((uint8_t *)h + 0x1d0, lg, 16)) {
            hexguid(lgs, lg);
            wchar_t pth[MAX_PATH]; _snwprintf(pth, MAX_PATH - 1, L"%ls\\bindings.txt", g_dir); pth[MAX_PATH - 1] = 0;
            FILE *bf = _wfopen(pth, L"a"); if (bf) { fprintf(bf, "%s %s\n", gs, lgs); fclose(bf); }
        }
        return h;
    }
    logf_("resolve: peer guid %s unknown, no pending object", gs);
    return r;
}

// ---- lockstep (stage 1: the client mirrors the host's simulation exactly) ---------------------------------
// FCOOP_ROLE=host / client. Starts when the file <instance>\lockstep_go exists (both games loaded, paused).
#define LS_MAGIC 0x534c4f46u          // "FOLS"
#define LS_PORT_CLIENT 27801
#define RVA_PLAY_NEXT_FRAME 0x294db0u // GamCReplayFrame* nextFrame(replay)
static const uint8_t EXPECT_PLAY_NEXT_FRAME[14] = { 0x4c,0x8b,0x41,0x58, 0x4c,0x8b,0xc9, 0x4d,0x85,0xc0, 0x74,0x49, 0x48,0x8b };
typedef struct { uint32_t magic, seq; float dt; uint32_t arg3; uint64_t seed0, seed1; uint32_t nids, nnodes, nbytes, nev, evbytes, ncli; uint32_t rand0, rand1; uint64_t grng0, grng1; } ls_hdr;
// packet: hdr | nids x (sig, guid) | command bytes | events | ncli x u32 (indices of the frame's commands that the client issued)
static uint32_t g_cli_idx[256], g_cli_n;   // host: this frame's client-issued command indices
// host: lockstep events since the last packet (type 1 = createBuilding {asset guid, n, guids})
static uint8_t g_ev[900000]; static uint32_t g_ev_n, g_ev_bytes;
static int g_ls_role;                 // 1 host, 2 client
static int g_ls_go, g_ls_started;
static uint32_t g_ls_seq;
static uint8_t *g_play_replay;        // client: fake replay object for mode 2
static uint8_t **g_ls_q; static int g_ls_qn, g_ls_qcap;
static uint64_t g_ls_expect_seed1; static uint32_t g_ls_cur_seq; static int g_ls_desyncs;

static void *make_empty_map(void) {
    uint8_t *map = ecalloc(0x40);
    void **sentinel = ecalloc(0x30); sentinel[0] = sentinel; sentinel[1] = sentinel;
    void **buckets = ecalloc(8 * 16);
    for (int b = 0; b < 16; b++) buckets[b] = sentinel;
    *(void **)(map + 0x08) = sentinel; *(void **)(map + 0x18) = buckets;
    *(void **)(map + 0x20) = buckets + 16; *(void **)(map + 0x28) = buckets + 16;
    *(uint64_t *)(map + 0x30) = 7; *(uint64_t *)(map + 0x38) = 8;
    return map;
}

// diagnostics: when <instance>\dump_now exists, dump every building / building-part component (0x400 bytes each)
static void dev_dump_components(void) {
    wchar_t pth[MAX_PATH]; _snwprintf(pth, MAX_PATH - 1, L"%ls\\dump_now", g_dir); pth[MAX_PATH - 1] = 0;
    if (GetFileAttributesW(pth) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileW(pth);
    uint8_t **reg = (uint8_t **)(g_base + RVA_COMP_REGISTRY);
    // pass 1: owners of buildings / building parts; pass 2: every component on those owners (0xa00 bytes each)
    static uint8_t *owners[512]; int no = 0, n = 0;
    for (uint8_t *s = reg[0]; s && s < reg[1] && no < 512; s += 16) {
        uint8_t *comp = *(uint8_t **)(s + 8), *owner = NULL;
        if (!comp) continue;
        { const char *nm = rtti_name(comp); if (!strstr(nm, "PlaCCompBuilding@@") && !strstr(nm, "PlaCCompBuildingPart@@")) continue; }
        if (safe_read(comp + 0x28, &owner, 8) && owner) owners[no++] = owner;
    }
    for (uint8_t *s = reg[0]; s && s < reg[1]; s += 16) {
        uint8_t *comp = *(uint8_t **)(s + 8), *owner = NULL, og[16]; static uint8_t raw[0xa00];
        if (!comp || !safe_read(comp + 0x28, &owner, 8) || !owner) continue;
        int hit = 0; for (int i = 0; i < no; i++) if (owners[i] == owner) hit = 1;
        if (!hit || !safe_read(owner + 0x1d0, og, 16) || !safe_read(comp, raw, sizeof raw)) continue;
        char tag[64]; const char *nm = rtti_name(comp); int k = 0;
        for (const char *c = nm; *c && k < 60; c++) if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z')) tag[k++] = *c;
        tag[k] = 0;
        char gs[33]; hexguid(gs, og);
        wchar_t f[MAX_PATH]; _snwprintf(f, MAX_PATH - 1, L"%ls\\dump_%hs_%hs.bin", g_dir, gs, tag); f[MAX_PATH - 1] = 0;
        FILE *fp = _wfopen(f, L"wb"); if (fp) { fwrite(raw, 1, sizeof raw, fp); fclose(fp); n++; }
    }
    logf_("dev: dumped %d components of %d building objects", n, no);
}

static void *find_component_rtti(const char *needle);
static void ls_check_go(void) {
    dev_dump_components();
    if (g_ls_go || !g_ls_role) return;
    wchar_t pth[MAX_PATH]; _snwprintf(pth, MAX_PATH - 1, L"%ls\\lockstep_go", g_dir); pth[MAX_PATH - 1] = 0;
    if (GetFileAttributesW(pth) == INVALID_FILE_ATTRIBUTES) return;
    // only inside a loaded city (the main game loop component exists); "go" in the main menu waits
    static int warned;
    if (!find_component_rtti("PlaCCompMainGameLoop@@")) { if (!warned++) logf_("lockstep: go requested, waiting for a loaded city"); return; }
    g_ls_go = 1; logf_("lockstep: go (role %d)", g_ls_role);
}

// host: after the frame, send everything the client needs to replay it
static void ls_host_flush(uint8_t *g, float dt, uint32_t arg3, uint64_t seed0) {
    *(void **)(g + G_REPLAY) = NULL;
    void *w = g_writer; g_writer = NULL;
    static uint8_t pkt[(1 << 20) - 1024];   // events can carry many GUIDs; lsnet limit is 1 MiB
    ls_hdr *h = (ls_hdr *)pkt;
    memset(h, 0, sizeof *h);
    h->magic = LS_MAGIC; h->seq = g_ls_seq++; h->dt = dt; h->arg3 = arg3; h->seed0 = seed0;
    h->seed1 = *(uint64_t *)(g + G_SEED);
    h->rand0 = g_rand0; h->rand1 = g_ls_rand;
    h->grng0 = g_grng0; h->grng1 = g_ls_grng;
    size_t n = sizeof *h;
    // object GUIDs generated since the last packet, in creation order (the frame-independent id stream)
    while (g_id_head < g_id_n && n + 24 <= sizeof pkt - 4096) {
        memcpy(pkt + n, g_id_buf + 24 * g_id_head++, 24); n += 24; h->nids++;
    }
    if (g_id_head == g_id_n) g_id_head = g_id_n = 0;   // all sent (otherwise the rest goes with the next packet)
    // commands
    uint64_t *ids_b = *(uint64_t **)(g_fake_frame + F_REC_IDS), *ids_e = *(uint64_t **)(g_fake_frame + F_REC_IDS + 8);
    size_t nn = ids_b ? (size_t)(ids_e - ids_b) : 0;
    if (w && nn) {
        uint8_t *buf = *(uint8_t **)((uint8_t *)w + 0x38);
        for (size_t i = 0; i < nn; i++) *(uint32_t *)(buf + ids_b[i] + 4) = (i + 1 < nn) ? (uint32_t)ids_b[i + 1] : 0;
        *(uint64_t *)((uint8_t *)w + 0x58) = ids_b[0];
        struct { uint8_t *b, *e, *c; } vec = {0}; uint32_t status[8] = {0};
        if (((get_bytes_fn)((*(void ***)w)[WRITER_GET_BYTES_SLOT]))(w, &vec, status) && vec.b) {
            size_t nb = (size_t)(vec.e - vec.b);
            if (n + nb <= sizeof pkt) { memcpy(pkt + n, vec.b, nb); n += nb; h->nbytes = (uint32_t)nb; h->nnodes = (uint32_t)nn; }
            else logf_("lockstep: frame %u commands too big (%zu bytes), dropped (desync expected)", h->seq, nb);
        }
    }
    // lockstep events (UI building creations) since the last packet
    if (g_ev_n && n + g_ev_bytes <= sizeof pkt) {
        memcpy(pkt + n, g_ev, g_ev_bytes); n += g_ev_bytes; h->nev = g_ev_n; h->evbytes = g_ev_bytes;
        logf_("lockstep host: seq %u carries %u event(s)", h->seq, g_ev_n);
        g_ev_n = g_ev_bytes = 0;
    }
    if (g_cli_n && n + 4 * g_cli_n <= sizeof pkt) { memcpy(pkt + n, g_cli_idx, 4 * g_cli_n); n += 4 * g_cli_n; h->ncli = g_cli_n; }
    g_cli_n = 0;
    lsn_send(LSN_FRAME, pkt, (uint32_t)n);
    if (h->nnodes || (h->seq % 600) == 0)
        logf_("lockstep host: seq %u dt %.5f seed %016llx->%016llx ids %u cmds %u (%u bytes) [creates %llu guids %llu in-create %llu]", h->seq, dt,
              (unsigned long long)seed0, (unsigned long long)h->seed1, h->nids, h->nnodes, h->nbytes,
              (unsigned long long)g_cnt_create, (unsigned long long)g_cnt_guid, (unsigned long long)g_cnt_guid_in_create);
}

// client: receive host frames into a queue
// a host frame is accepted only when every section length adds up exactly
static int ls_frame_valid(const uint8_t *buf, uint32_t n) {
    if (n < sizeof(ls_hdr)) return 0;
    const ls_hdr *h = (const ls_hdr *)buf;
    if (h->magic != LS_MAGIC || h->nids > 65536 || h->nnodes > 4096 || h->nev > 4096 || h->ncli > 256) return 0;
    uint64_t need = sizeof *h + 24ull * h->nids + (uint64_t)h->nbytes + h->evbytes + 4ull * h->ncli;
    return need == n;
}
static void ls_client_on_msg(uint32_t type, uint8_t *buf, uint32_t n) {
    if (type != LSN_FRAME) return;
    if (!ls_frame_valid(buf, n)) { logf_("lockstep client: malformed host frame (%u bytes) dropped", n); return; }
    {
        if (g_ls_qn == g_ls_qcap) { g_ls_qcap = g_ls_qcap ? g_ls_qcap * 2 : 256; g_ls_q = realloc(g_ls_q, g_ls_qcap * sizeof *g_ls_q); }
        uint8_t *p = malloc(n); memcpy(p, buf, n); g_ls_q[g_ls_qn++] = p;
        // host GUIDs join the client's id FIFO as soon as they arrive (objects may be created a frame early or late)
        ls_hdr *h = (ls_hdr *)p;
        for (uint32_t i = 0; i < h->nids && sizeof *h + 24 * (size_t)(i + 1) <= (size_t)n; i++) {
            uint64_t sg; memcpy(&sg, p + sizeof *h + 24 * i, 8);
            sigq_push(sg, p + sizeof *h + 24 * i + 8, h->seq);
        }
    }
}
static void ls_client_drain(void) { lsn_poll(ls_client_on_msg); }

// client: turn a host packet into an engine replay frame
static uint8_t *g_play_fr; static uint32_t g_play_cli[256], g_play_cli_n;   // client: current frame's own-command indices
static uint8_t *ls_make_frame(uint8_t *pkt) {
    ls_hdr *h = (ls_hdr *)pkt;
    uint8_t *fr = ecalloc(0x200);
    g_play_fr = fr; g_play_cli_n = h->ncli < 256 ? h->ncli : 256;
    memcpy(g_play_cli, pkt + sizeof *h + 24 * (size_t)h->nids + h->nbytes + h->evbytes, 4 * (size_t)g_play_cli_n);
    *(float *)(fr + 0x18) = h->dt;
    *(uint64_t *)(fr + 0x20) = h->seed0;
    *(uint64_t *)(fr + 0x28) = h->seed1;
    // per-frame id list stays EMPTY: ids come from the ordered GUID stream (hook_guid_gen)
    void **sent = ecalloc(0x20); sent[0] = sent; sent[1] = sent;
    uint8_t *ids = pkt + sizeof *h;
    *(void **)(fr + F_ID_LIST) = sent; *(uint64_t *)(fr + F_ID_LIST + 8) = 0;
    *(void **)(fr + 0x70) = sent;       // cursor == end
    // commands
    uint64_t *nodes = NULL; uint32_t k = 0;
    if (h->nbytes) {
        uint8_t *bytes = ids + 24 * h->nids;
        void *rd = ecalloc(0x100);
        ((ctor_fn)(g_base + RVA_BIN_READER_CTOR))(rd);
        if (((load_buf_fn)((*(void ***)rd)[READER_LOAD_SLOT]))(rd, bytes, h->nbytes)) {
            uint8_t *rdata = *(uint8_t **)((uint8_t *)rd + 0x30);
            uint32_t rsize = *(uint32_t *)((uint8_t *)rd + 0x38);
            uint64_t id = *(uint64_t *)((uint8_t *)rd + 0x40);
            nodes = ecalloc(8 * (h->nnodes + 1));
            while (id && id + 8 <= rsize && k < h->nnodes) { nodes[k++] = id; id = *(uint32_t *)(rdata + id + 4); }
            void **holder = ecalloc(0x80); holder[0] = rd; holder[5] = make_empty_map();
            *(void **)(g_play_replay + 0x28) = holder;
        }
    }
    *(uint64_t **)(fr + F_PLAY_IDS) = nodes; *(uint64_t **)(fr + F_PLAY_IDS + 8) = nodes ? nodes + k : NULL;
    *(uint32_t *)(fr + F_PLAY_INDEX) = 0;
    g_ls_expect_seed1 = h->seed1; g_ls_cur_seq = h->seq;
    g_ls_rand = h->rand0; g_ls_expect_rand1 = h->rand1;
    g_ls_grng = h->grng0; g_ls_expect_grng1 = h->grng1;
    if (k) logf_("lockstep client: seq %u dt %.5f ids %u cmds %u", h->seq, h->dt, h->nids, k);
    return fr;
}

// flow control: the client acknowledges every replayed host frame; the host waits when it is too far ahead
#define LS_ACK_MAGIC 0x414c4f46u   // "FOLA"
#define LS_WINDOW 24
static int64_t g_ls_last_ack = -1;
static void ls_client_ack(void) {
    uint32_t pkt[2] = { LS_ACK_MAGIC, g_ls_cur_seq };
    lsn_send(LSN_ACK, pkt, sizeof pkt);
}
static uint8_t *g_game;   // GamCGame*, set every frame by the hook
static void ls_host_client_cmd(uint8_t *g, uint8_t *pkt, int n);
#define LS_UIEV_MAGIC 0x454c4f46u  // "FOLE": one UI event from the client {magic, event bytes}
static void ls_host_client_event(uint8_t *pkt, int n);
static void ls_host_on_msg(uint32_t type, uint8_t *buf, uint32_t n) {
    uint32_t *pkt = (uint32_t *)buf;
    if (n < 8) return;
    if (type == LSN_ACK && n == 8 && pkt[0] == LS_ACK_MAGIC) { if ((int64_t)pkt[1] > g_ls_last_ack) g_ls_last_ack = pkt[1]; return; }
    if (type == LSN_CMD && pkt[0] == 0x434c4f46u && g_game) { ls_host_client_cmd(g_game, buf, (int)n); return; }   // "FOLC"
    if (type == LSN_UIEV && pkt[0] == LS_UIEV_MAGIC && g_ls_go) { ls_host_client_event(buf, (int)n); return; }
    logf_("lockstep host: unexpected client message type %u (%u bytes) dropped", type, n);
}
static void ls_host_drain_acks(void) { lsn_poll(ls_host_on_msg); }
static void ls_host_throttle(void) {
    ls_host_drain_acks();
    DWORD t0 = GetTickCount(); int warned = 0;
    while ((int64_t)g_ls_seq - g_ls_last_ack > LS_WINDOW) {   // includes "client not started yet" (last_ack = -1)
        Sleep(1);
        ls_host_drain_acks();
        if (!warned && GetTickCount() - t0 > 3000) { logf_("lockstep host: waiting for client (seq %u, ack %lld)", g_ls_seq, (long long)g_ls_last_ack); warned = 1; }
    }
}

// ---- lockstep stage 2: client actions go to the host --------------------------------------------------------
#define LS_CMD_MAGIC 0x434c4f46u   // "FOLC"
#define RVA_REC_COMMAND 0x298250u  // void recordCommand(replay, cmd): serialize cmd into replay's writer / current frame
static const uint8_t EXPECT_PUSH_CMD[15] = { 0x48,0x89,0x5c,0x24,0x10, 0x48,0x89,0x6c,0x24,0x18, 0x48,0x89,0x74,0x24,0x20 };
static push_cmd_fn g_orig_push_cmd;
static const uint8_t EXPECT_READ_CMD[18] = { 0x4c,0x8b,0xdc, 0x49,0x89,0x5b,0x10, 0x56, 0x57, 0x41,0x56, 0x48,0x81,0xec,0x00,0x01,0x00,0x00 };
static const uint8_t EXPECT_RECORD_CMD[14] = { 0x48,0x89,0x5c,0x24,0x18, 0x48,0x89,0x74,0x24,0x20, 0x55, 0x57, 0x41,0x54 };

// serialize one command without executing it (client) -> bytes + node count
static int serialize_cmd(void *cmd, uint8_t *out, size_t cap, uint32_t *nnodes) {
    uint8_t *R = ecalloc(0x400), *fr = ecalloc(0x200);
    void **sentinel = ecalloc(0x20); sentinel[0] = sentinel; sentinel[1] = sentinel;
    *(void **)(fr + F_ID_LIST) = sentinel;
    void *w = ecalloc(0x100);
    ((ctor_fn)(g_base + RVA_BIN_WRITER_CTOR))(w, 1);
    void **holder = ecalloc(0x10); holder[0] = w;
    *(void **)(R + R_WRITER_HOLDER) = holder; *(void **)(R + R_CUR_FRAME) = fr;
    ((void (*)(void *, void *))(g_base + RVA_REC_COMMAND))(R, cmd);
    uint64_t *ids_b = *(uint64_t **)(fr + F_REC_IDS), *ids_e = *(uint64_t **)(fr + F_REC_IDS + 8);
    size_t nn = ids_b ? (size_t)(ids_e - ids_b) : 0;
    if (!nn) return -1;
    uint8_t *buf = *(uint8_t **)((uint8_t *)w + 0x38);
    for (size_t i = 0; i < nn; i++) *(uint32_t *)(buf + ids_b[i] + 4) = (i + 1 < nn) ? (uint32_t)ids_b[i + 1] : 0;
    *(uint64_t *)((uint8_t *)w + 0x58) = ids_b[0];
    struct { uint8_t *b, *e, *c; } vec = {0}; uint32_t status[8] = {0};
    if (!((get_bytes_fn)((*(void ***)w)[WRITER_GET_BYTES_SLOT]))(w, &vec, status) || !vec.b) return -1;
    size_t nb = (size_t)(vec.e - vec.b);
    if (nb > cap) return -1;
    memcpy(out, vec.b, nb); *nnodes = (uint32_t)nn;
    return (int)nb;
}

#define OWN_MAX 256
static void *g_own[OWN_MAX]; static int g_own_n;   // client: own commands sent to the host, oldest first
static void *g_cli_cmds[OWN_MAX]; static int g_cli_cmds_n;   // host: queued commands that came from the client

// readCommand: on the client, a command the host marked as ours is replaced by our original object
static read_cmd_fn g_orig_read_cmd;
static void *hook_read_cmd(void *frame, void *world, void **holder) {
    uint32_t idx = *(uint32_t *)((uint8_t *)frame + 0x78);
    void *cmd = g_orig_read_cmd(frame, world, holder);
    if (g_ls_role != 2 || frame != g_play_fr) return cmd;
    int mine = 0;
    for (uint32_t i = 0; i < g_play_cli_n; i++) if (g_play_cli[i] == idx) mine = 1;
    if (!mine) return cmd;
    if (!g_own_n) { logf_("lockstep client: own command #%u came back but none is pending", idx); return cmd; }
    void *own = g_own[0];
    memmove(g_own, g_own + 1, (size_t)(--g_own_n) * sizeof *g_own);
    // only commands whose UI listeners matter use the original (the build tool waits for "part created")
    if (cmd && !strstr(rtti_name(own), "PlaCPlayerCommandCreateBuildingPart@@")) {
        ((void (*)(void *, int))(*(void ***)own)[1])(own, 1);
        return cmd;
    }
    if (cmd && strcmp(rtti_name(cmd), rtti_name(own)) != 0) {
        logf_("lockstep client: own command mismatch (%s vs %s), using the host's copy", rtti_name(own), rtti_name(cmd));
        ((void (*)(void *, int))(*(void ***)own)[1])(own, 1);
        return cmd;
    }
    if (cmd) ((void (*)(void *, int))(*(void ***)cmd)[1])(cmd, 1);
    logf_("lockstep client: executing own original %s", rtti_name(own));
    return own;
}

// ComCEvent<PlaCCompBuildingPart*>::trigger: when a player command's own event (cmd+0x28) fires inside the frame,
// its listeners are the local UI (only the issuing side has them): run them outside the deterministic simulation
// (RNG seed restored, created objects get local GUIDs instead of stream ones)
#define RVA_PART_EVENT_FIRE 0x9ffa70u
static const uint8_t EXPECT_PART_EVENT_FIRE[15] = { 0x48,0x89,0x5c,0x24,0x10, 0x48,0x89,0x74,0x24,0x18, 0x48,0x89,0x7c,0x24,0x20 };
typedef void (*part_event_fire_fn)(void *ev, void *arg);
static part_event_fire_fn g_orig_part_event_fire;
// true when obj looks like a PlaCPlayerCommand* (vtable and type descriptor inside the exe image)
static int is_player_command(uint8_t *obj) {
    uint8_t *vt, *col; uint32_t td_rva; char nm[24];
    const uintptr_t lo = (uintptr_t)g_base, hi = lo + 0x5000000;
    if (!safe_read(obj, &vt, 8) || (uintptr_t)vt < lo || (uintptr_t)vt >= hi) return 0;
    if (!safe_read(vt - 8, &col, 8) || (uintptr_t)col < lo || (uintptr_t)col >= hi) return 0;
    if (!safe_read(col + 0x0c, &td_rva, 4) || td_rva >= 0x5000000) return 0;
    if (!safe_read(g_base + td_rva + 0x10, nm, sizeof nm - 1)) return 0;
    nm[sizeof nm - 1] = 0;
    return strncmp(nm, ".?AVPlaCPlayerCommand", 21) == 0;
}
static void hook_part_event_fire(void *ev, void *arg) {
    if (g_ls_go && g_in_frame && g_game && is_player_command((uint8_t *)ev - 0x28)) {
        uint64_t seed = *(uint64_t *)(g_game + G_SEED);
        g_in_frame = 0;
        g_orig_part_event_fire(ev, arg);
        g_in_frame = 1;
        if (*(uint64_t *)(g_game + G_SEED) != seed) logf_("lockstep: command UI listener used the RNG (restored)");
        *(uint64_t *)(g_game + G_SEED) = seed;
        return;
    }
    g_orig_part_event_fire(ev, arg);
}

// recordCommand: on the host, note the frame index of commands that came from the client
typedef void (*record_cmd_fn)(uint8_t *R, void *cmd);
static record_cmd_fn g_orig_record_cmd;
static void hook_record_cmd(uint8_t *R, void *cmd) {
    if (g_ls_role == 1 && g_cli_cmds_n) {
        for (int i = 0; i < g_cli_cmds_n; i++) if (g_cli_cmds[i] == cmd) {
            uint8_t *fr = *(uint8_t **)(R + R_CUR_FRAME);
            uint64_t *b = fr ? *(uint64_t **)(fr + F_REC_IDS) : NULL, *e = fr ? *(uint64_t **)(fr + F_REC_IDS + 8) : NULL;
            if (g_cli_n < 256) g_cli_idx[g_cli_n++] = b ? (uint32_t)(e - b) : 0;
            g_cli_cmds[i] = g_cli_cmds[--g_cli_cmds_n];
            break;
        }
    }
    g_orig_record_cmd(R, cmd);
}

// client: commands the local player issues while lockstep runs are sent to the host instead of being executed
static void hook_push_cmd(void *game, void *cmd) {
    if (g_ls_role == 2 && g_ls_started && cmd) {
        static uint8_t pkt[65000];
        uint32_t nn = 0;
        int nb = serialize_cmd(cmd, pkt + 12, sizeof pkt - 12, &nn);
        const char *nm = rtti_name(cmd);
        if (nb > 0) {
            ((uint32_t *)pkt)[0] = LS_CMD_MAGIC; ((uint32_t *)pkt)[1] = nn; ((uint32_t *)pkt)[2] = (uint32_t)nb;
            int r = lsn_send(LSN_CMD, pkt, (uint32_t)(12 + nb));
            logf_("lockstep client: sent own command %s (%d bytes) to host -> %d", nm, nb, r);
        } else logf_("lockstep client: could not serialize own command %s", nm);
        // keep the original: it carries the UI's callbacks (e.g. the build tool's "part created" at cmd+0x28);
        // when the host's frame stream brings this command back, the original is executed instead of the copy
        if (nb > 0 && g_own_n < OWN_MAX) g_own[g_own_n++] = cmd;
        else ((void (*)(void *, int))(*(void ***)cmd)[1])(cmd, 1);
        return;
    }
    g_orig_push_cmd(game, cmd);
}

// host: commands from the client are deserialized and queued like local ones (then recorded + streamed back)
static void ls_host_client_cmd(uint8_t *g, uint8_t *pkt, int n) {
    uint32_t nn = ((uint32_t *)pkt)[1], nb = ((uint32_t *)pkt)[2];
    if (n < 12 || 12 + (uint64_t)nb != (uint64_t)n || nn == 0 || nn > 64) { logf_("lockstep host: malformed client command dropped"); return; }
    void *rd = ecalloc(0x100);
    ((ctor_fn)(g_base + RVA_BIN_READER_CTOR))(rd);
    if (!((load_buf_fn)((*(void ***)rd)[READER_LOAD_SLOT]))(rd, pkt + 12, nb)) { logf_("lockstep host: bad client command packet"); return; }
    uint8_t *rdata = *(uint8_t **)((uint8_t *)rd + 0x30); uint32_t rsize = *(uint32_t *)((uint8_t *)rd + 0x38);
    uint64_t id = *(uint64_t *)((uint8_t *)rd + 0x40);
    uint64_t *nodes = ecalloc(8 * (nn + 1)); uint32_t k = 0;
    while (id && id + 8 <= rsize && k < nn) { nodes[k++] = id; id = *(uint32_t *)(rdata + id + 4); }
    uint8_t *fr = ecalloc(0x200);
    *(uint64_t **)(fr + F_PLAY_IDS) = nodes; *(uint64_t **)(fr + F_PLAY_IDS + 8) = nodes + k;
    void **holder = ecalloc(0x80); holder[0] = rd; holder[5] = make_empty_map();
    for (uint32_t i = 0; i < k; i++) {
        g_in_recv = 1;
        void *cmd = ((read_cmd_fn)(g_base + RVA_READ_COMMAND))(fr, *(void **)(g + G_WORLD), holder);
        g_in_recv = 0;
        logf_("lockstep host: client command %s queued", cmd ? rtti_name(cmd) : "(null)");
        if (cmd) { if (g_cli_cmds_n < OWN_MAX) g_cli_cmds[g_cli_cmds_n++] = cmd; g_orig_push_cmd(g, cmd); }
    }
}

// first live component whose RTTI name contains `needle`
static void *find_component_rtti(const char *needle) {
    uint8_t **reg = (uint8_t **)(g_base + RVA_COMP_REGISTRY);
    for (uint8_t *s = reg[0]; s && s < reg[1]; s += 16) {
        void *comp = *(void **)(s + 8);
        if (comp && strstr(rtti_name(comp), needle)) return comp;
    }
    return NULL;
}

// UI state changes outside the simulation frame (build-tool preview) that the simulation later depends on.
// Event layout (24 bytes): {u32 type, owner guid[16], u8 a, u8 b, u8 c, u8 pad}
#define EV_EDIT_MODE   2               // PlaCCompBuilding::setEditMode(comp, mode, force, notify)
#define EV_PART_PLACED 3               // PlaCCompBuildingPart::setIsPlaced(comp, placed, recursive)
#define RVA_SET_EDIT_MODE  0xcd1fa0u
#define RVA_SET_IS_PLACED  0xcd3b90u
static const uint8_t EXPECT_SET_EDIT_MODE[14] = { 0x48,0x8b,0xc4, 0x44,0x88,0x48,0x20, 0x44,0x88,0x40,0x18, 0x88,0x50,0x10 };
static const uint8_t EXPECT_SET_IS_PLACED[16] = { 0x48,0x89,0x6c,0x24,0x18, 0x56, 0x48,0x83,0xec,0x30, 0x0f,0xb6,0xea, 0x48,0x8b,0xf1 };
typedef void (*set_edit_mode_fn)(void *comp, uint8_t mode, uint8_t force, uint8_t notify);
typedef void (*set_is_placed_fn)(void *comp, uint8_t placed, uint8_t recursive);
static set_edit_mode_fn g_orig_set_edit_mode;
static set_is_placed_fn g_orig_set_is_placed;
static int g_ui_depth;                 // only the outermost call is an event (both setters recurse)
// EV_PART_PLACED carries the part's global transform (the build tool moves the preview outside the simulation):
// 24-byte head + pos[3] + quat[4] + pad = 56 bytes
#define EV_PART_PLACED_SIZE 56
#define RVA_GO_GET_POS  0x4bf320u      // void getGlobalPosition(GameObject*, float out[3])
#define RVA_GO_GET_ROT  0x4bf2a0u      // void getGlobalOrientation(GameObject*, float out[4])
#define RVA_GO_SET_XF   0x4cb3a0u      // void setGlobalTransform(GameObject*, const float pos[3], const float quat[4])
// EV_XF_MATRIX (4): build-tool preview move: {head 24 (owner = game object GUID), float m[16]}
static size_t ev_size(const uint8_t *e) {
    uint32_t t = *(const uint32_t *)e;
    return t == EV_PART_PLACED ? EV_PART_PLACED_SIZE : t == EV_XF_MATRIX ? EV_XF_MATRIX_SIZE : 24;
}

static void ls_client_send_event_g(const uint8_t *e, const uint8_t *guids, uint32_t n);
static void ls_host_append_event(const uint8_t *e, const uint8_t *guids, uint32_t n);
// build the event for a UI setter call; returns 0 if the component has no owner
static int ls_make_ui_event(uint8_t *ev, uint32_t type, void *comp, uint8_t a, uint8_t b, uint8_t c) {
    uint8_t *owner = NULL, og[16];
    if (!comp || !safe_read((uint8_t *)comp + 0x28, &owner, 8) || !owner || !safe_read(owner + 0x1d0, og, 16)) return 0;
    memset(ev, 0, EV_PART_PLACED_SIZE);
    *(uint32_t *)ev = type; memcpy(ev + 4, og, 16); ev[20] = a; ev[21] = b; ev[22] = c; ev[23] = 0;
    if (type == EV_PART_PLACED) {
        ((void (*)(void *, float *))(g_base + RVA_GO_GET_POS))(owner, (float *)(ev + 24));
        ((void (*)(void *, float *))(g_base + RVA_GO_GET_ROT))(owner, (float *)(ev + 36));
    }
    char gs[33]; hexguid(gs, og);
    logf_("lockstep %s: UI %s %s (%u,%u,%u) captured", g_ls_role == 1 ? "host" : "client",
          type == EV_EDIT_MODE ? "setEditMode" : "setIsPlaced", gs, a, b, c);
    return 1;
}
// after the original call ran with GUID capture: host streams the event, client sends it to the host
static void ls_emit_ui_event(const uint8_t *ev) {
    if (g_ls_role == 2) ls_client_send_event_g(ev, g_cb_ids, g_cb_n);
    else ls_host_append_event(ev, g_cb_ids, g_cb_n);
}
// only UI calls (outside the frame, not part of a replicated createBuilding, not a replayed event)
static int ui_capture(void) {
    return g_ls_go && (g_ls_role == 1 || (g_ls_role == 2 && g_ls_started)) && !g_in_frame && g_ui_depth == 0
        && g_cb_mode == 0 && !g_in_remote_create;
}
// The build tool's own update runs INSIDE the simulation frame on the issuing machine only (FUN_140c28be0 ->
// FUN_140c396a0 -> setEditMode, e.g. leaving edit mode after Bauen). Such calls are deferred to the frame boundary
// both sides share: the host applies them after the frame and streams them; the client sends them to the host and
// applies them when they come back (event flag byte 23 = 1).
#define RA_TOOL_SET_EDIT_MODE 0xc39730u    // return address of the setEditMode call in FUN_140c396a0
static void ls_client_ui_event(const uint8_t *e);
static void ls_make_edit_event(uint8_t ev[24], void *comp, uint8_t a, uint8_t b, uint8_t c, uint8_t deferred) {
    uint8_t *owner = NULL; memset(ev, 0, 24);
    *(uint32_t *)ev = EV_EDIT_MODE; ev[20] = a; ev[21] = b; ev[22] = c; ev[23] = deferred;
    if (safe_read((uint8_t *)comp + 0x28, &owner, 8) && owner) safe_read(owner + 0x1d0, ev + 4, 16);
}
static void ls_apply_event(const uint8_t *e, int mode, const uint8_t *feed, uint32_t nfeed);
static void ls_host_apply_deferred(void) {
    for (int i = 0; i < g_nmdefer; i++) { ls_apply_event(g_mdefer[i], 1, NULL, 0); ls_host_append_event(g_mdefer[i], g_cb_ids, g_cb_n); }
    g_nmdefer = 0;
    for (int i = 0; i < g_ndefer; i++) { ls_apply_event(g_defer[i], 1, NULL, 0); ls_host_append_event(g_defer[i], g_cb_ids, g_cb_n); }
    g_ndefer = 0;
}
static void hook_set_edit_mode(void *comp, uint8_t mode, uint8_t force, uint8_t notify) {
    if (g_ls_go && g_in_frame && g_ui_depth == 0 && (uint8_t *)__builtin_return_address(0) == g_base + RA_TOOL_SET_EDIT_MODE
        && (g_ls_role == 1 || (g_ls_role == 2 && g_ls_started))) {
        uint8_t ev[24]; ls_make_edit_event(ev, comp, mode, force, notify, 1);
        char gs[33]; hexguid(gs, ev + 4);
        logf_("lockstep %s: in-frame tool setEditMode %s (%u,%u,%u) deferred", g_ls_role == 1 ? "host" : "client", gs, mode, force, notify);
        if (g_ls_role == 2) ls_client_send_event_g(ev, NULL, 0);
        else if (g_ndefer < 64) memcpy(g_defer[g_ndefer++], ev, 24);
        return;
    }
    uint8_t ev[EV_PART_PLACED_SIZE];
    int cap = ui_capture() && ls_make_ui_event(ev, EV_EDIT_MODE, comp, mode, force, notify);
    if (cap) { g_cb_mode = 1; g_cb_n = 0; }
    g_ui_depth++; g_orig_set_edit_mode(comp, mode, force, notify); g_ui_depth--;
    if (cap) { g_cb_mode = 0; ls_emit_ui_event(ev); }
}
static void hook_set_is_placed(void *comp, uint8_t placed, uint8_t recursive) {
    uint8_t ev[EV_PART_PLACED_SIZE];
    int cap = ui_capture() && ls_make_ui_event(ev, EV_PART_PLACED, comp, placed, recursive, 0);
    if (cap) { g_cb_mode = 1; g_cb_n = 0; }
    g_ui_depth++; g_orig_set_is_placed(comp, placed, recursive); g_ui_depth--;
    if (cap) { g_cb_mode = 0; ls_emit_ui_event(ev); }
}

// live component of an RTTI kind whose owner game object has this GUID
static void *find_component_by_owner(const uint8_t guid[16], const char *needle) {
    uint8_t **reg = (uint8_t **)(g_base + RVA_COMP_REGISTRY);
    for (uint8_t *s = reg[0]; s && s < reg[1]; s += 16) {
        uint8_t *comp = *(uint8_t **)(s + 8), *owner = NULL, og[16];
        if (!comp || !safe_read(comp + 0x28, &owner, 8) || !owner || !safe_read(owner + 0x1d0, og, 16)) continue;
        if (!memcmp(og, guid, 16) && strstr(rtti_name(comp), needle)) return comp;
    }
    return NULL;
}
// Applying an event can create objects (e.g. construction-site objects when edit mode ends). Whoever applies it
// first records the GUIDs generated meanwhile; they travel behind the event (u32 n, n GUIDs) and the other side
// applies it with exactly those GUIDs. mode: 1 = capture (result in g_cb_ids/g_cb_n), 2 = feed (from the trailer)
static void ls_apply_core(const uint8_t *e) {
    uint32_t type = *(const uint32_t *)e;
    if (type == EV_XF_MATRIX) {
        void *go = find_object_by_guid(e + 4);
        if (!go) { static int n; if (n++ < 10) logf_("lockstep: preview move for unknown object"); return; }
        float m[16]; memcpy(m, e + 24, 64);
        g_orig_set_gmatrix(go, m);
        return;
    }
    char gs[33]; hexguid(gs, e + 4);
    if (type == 5) {
        void *b = find_component_by_owner(e + 4, "PlaCCompBuilding@@");
        if (!b) { logf_("lockstep: destroy event for %s: building not found", gs); return; }
        g_orig_destroy_building(b);
        logf_("lockstep: UI event destroyBuilding %s applied", gs);
        return;
    }
    void *comp = find_component_by_owner(e + 4, type == EV_EDIT_MODE ? "PlaCCompBuilding@@" : "PlaCCompBuildingPart@@");
    if (!comp || !g_orig_set_edit_mode || !g_orig_set_is_placed) { logf_("lockstep: UI event %u for %s: component not found", type, gs); return; }
    if (type == EV_EDIT_MODE) g_orig_set_edit_mode(comp, e[20], e[21], e[22]);
    else {
        void *owner = NULL; safe_read((uint8_t *)comp + 0x28, &owner, 8);
        float pos[3], rot[4]; memcpy(pos, e + 24, 12); memcpy(rot, e + 36, 16);
        if (owner) ((void (*)(void *, const float *, const float *))(g_base + RVA_GO_SET_XF))(owner, pos, rot);
        g_orig_set_is_placed(comp, e[20], e[21]);
    }
    logf_("lockstep: UI event %s %s (%u,%u,%u) applied", type == EV_EDIT_MODE ? "setEditMode" : "setIsPlaced", gs, e[20], e[21], e[22]);
}
static void ls_apply_event(const uint8_t *e, int mode, const uint8_t *feed, uint32_t nfeed) {
    int saved_frame = g_in_frame;
    g_in_frame = 0;                       // UI-event semantics on both sides (no frame-stream ids, real RNG)
    g_ui_depth++;
    if (mode == 1) { g_cb_mode = 1; g_cb_n = 0; }
    else { if (nfeed > 4096) nfeed = 4096; if (nfeed) memcpy(g_cb_ids, feed, 16 * (size_t)nfeed); g_cb_n = nfeed; g_cb_i = 0; g_cb_mode = 2; }
    g_in_remote_create = 1;
    ls_apply_core(e);
    g_in_remote_create = 0;
    g_cb_mode = 0;
    g_ui_depth--;
    g_in_frame = saved_frame;
}
// total length of a type 2/3/4 event including its GUID trailer, 0 if it does not fit
static size_t ev_total(const uint8_t *e, const uint8_t *end) {
    size_t base = ev_size(e);
    if (e + base + 4 > end) return 0;
    uint32_t n = *(const uint32_t *)(e + base);
    if (n > 4096 || e + base + 4 + 16 * (size_t)n > end) return 0;
    return base + 4 + 16 * (size_t)n;
}
// host: append an event + GUID trailer to the next frame packet
static void ls_host_append_event(const uint8_t *e, const uint8_t *guids, uint32_t n) {
    size_t base = ev_size(e), len = base + 4 + 16 * (size_t)n;
    if (g_ev_bytes + len > sizeof g_ev) { logf_("lockstep host: event buffer full, event dropped (desync likely)"); return; }
    memcpy(g_ev + g_ev_bytes, e, base); memcpy(g_ev + g_ev_bytes + base, &n, 4);
    if (n) memcpy(g_ev + g_ev_bytes + base + 4, guids, 16 * (size_t)n);
    g_ev_bytes += (uint32_t)len; g_ev_n++;
}
// client: send an event + GUID trailer to the host
static void ls_client_send_event_g(const uint8_t *e, const uint8_t *guids, uint32_t n) {
    static uint8_t buf[128 + 4 + 16 * 4096];
    size_t base = ev_size(e);
    memcpy(buf, e, base); memcpy(buf + base, &n, 4);
    if (n) memcpy(buf + base + 4, guids, 16 * (size_t)n);
    ls_client_send_event(buf, base + 4 + 16 * (size_t)n);
}
static void ls_client_ui_event(const uint8_t *e) { ls_apply_event(e, 2, NULL, 0); }

// host: apply one UI event the client's player made (the client already applied it locally; no echo)
static void ls_host_client_event(uint8_t *pkt, int n) {
    if (n < 4 + 24) return;
    uint8_t *e = pkt + 4;
    uint32_t type = *(uint32_t *)e;
    if (type == EV_XF_MATRIX || type == EV_EDIT_MODE || type == EV_PART_PLACED || type == 5) {
        size_t tot = ev_total(e, pkt + n);
        if (!tot || (size_t)n != 4 + tot) { logf_("lockstep host: malformed client event dropped"); return; }
        size_t base = ev_size(e); uint32_t ng = *(uint32_t *)(e + base);
        if (type == EV_XF_MATRIX || e[23] == 1) {
            // deferred in-frame call: the client has not applied it; the host applies it first (capturing GUIDs)
            // and streams it; the client applies it with the next host frame
            ls_apply_event(e, 1, NULL, 0);
            ls_host_append_event(e, g_cb_ids, g_cb_n);
        } else ls_apply_event(e, 2, e + base + 4, ng);   // the client applied it already: reuse its GUIDs
        return;
    }
    if (type != 1) { logf_("lockstep host: unknown client event type %u dropped", type); return; }
    uint32_t ng = *(uint32_t *)(e + 20);
    if (ng > 4096 || (size_t)n != 4 + 24 + 16 * (size_t)ng) { logf_("lockstep host: malformed client createBuilding dropped"); return; }
    if (!g_building_manager) g_building_manager = find_component_rtti("PlaCCompBuildingManager@@");
    void *am = ((noarg_fn)(g_base + RVA_ASSET_MANAGER))();
    uint8_t *node = ((asset_find_fn)(g_base + RVA_ASSET_BY_GUID))(am, e + 4);
    void *asset = NULL; if (node) safe_read(node + 0xf0, &asset, 8);
    if (!asset || !g_building_manager || !strstr(rtti_name(asset), "PlaCAssetBuilding@@")) {
        logf_("lockstep host: client createBuilding: no building asset (%p)", asset); return;
    }
    memcpy(g_cb_ids, e + 24, 16 * (size_t)ng); g_cb_n = ng; g_cb_i = 0;
    uint64_t parent[2] = {0, 0};
    g_cb_mode = 2; g_in_remote_create = 1;
    void *comp = g_orig_create_building(g_building_manager, &asset, parent);
    g_cb_mode = 0; g_in_remote_create = 0;
    logf_("lockstep host: client createBuilding %s -> %p (fed %u of %u client guids)", rtti_name(asset), comp, g_cb_i, ng);
}

// client: execute the host's lockstep events carried by this packet (UI createBuilding with the host's GUIDs)
static void ls_client_events(uint8_t *pkt) {
    ls_hdr *h = (ls_hdr *)pkt;
    if (!h->nev) return;
    uint8_t *e = pkt + sizeof *h + 24 * (size_t)h->nids + h->nbytes, *end = e + h->evbytes;
    if (!g_building_manager) g_building_manager = find_component_rtti("PlaCCompBuildingManager@@");
    for (uint32_t i = 0; i < h->nev && e + 24 <= end; i++) {
        uint32_t type = *(uint32_t *)e, ng = *(uint32_t *)(e + 20);
        if (type == EV_XF_MATRIX || type == EV_EDIT_MODE || type == EV_PART_PLACED || type == 5) {
            size_t tot = ev_total(e, end); if (!tot) break;
            size_t base = ev_size(e);
            ls_apply_event(e, 2, e + base + 4, *(uint32_t *)(e + base));
            e += tot; continue;
        }
        if (type != 1 || e + 24 + 16 * (size_t)ng > end || ng > 4096) break;
        void *am = ((noarg_fn)(g_base + RVA_ASSET_MANAGER))();
        uint8_t *node = ((asset_find_fn)(g_base + RVA_ASSET_BY_GUID))(am, e + 4);
        void *asset = NULL; if (node) safe_read(node + 0xf0, &asset, 8);
        if (asset && g_building_manager) {
            memcpy(g_cb_ids, e + 24, 16 * (size_t)ng); g_cb_n = ng; g_cb_i = 0;
            uint64_t parent[2] = {0, 0};
            g_cb_mode = 2; g_in_remote_create = 1;
            void *comp = g_orig_create_building(g_building_manager, &asset, parent);
            g_cb_mode = 0; g_in_remote_create = 0;
            logf_("lockstep client: event createBuilding %s -> %p (fed %u of %u host guids)", rtti_name(asset), comp, g_cb_i, ng);
        } else logf_("lockstep client: event createBuilding failed (asset %p manager %p)", asset, g_building_manager);
        e += 24 + 16 * (size_t)ng;
    }
}

// replacement for GamCReplay::nextFrame (FUN_140294db0): original logic for real replays, host frames for ours
typedef uint8_t *(*next_frame_fn)(uint8_t *replay);
static uint8_t *hook_play_next_frame(uint8_t *R) {
    if (R && R == g_play_replay) {
        DWORD t0 = GetTickCount(); int warned = 0;
        while (g_ls_qn == 0) {             // lockstep: wait for the host (never return null, that ends playback)
            ls_client_drain();
            if (g_ls_qn) break;
            if (!warned && GetTickCount() - t0 > 3000) { logf_("lockstep client: waiting for host frame…"); warned = 1; }
            Sleep(1);
        }
        uint8_t *pkt = g_ls_q[0];
        memmove(g_ls_q, g_ls_q + 1, (size_t)(--g_ls_qn) * sizeof *g_ls_q);
        ls_client_events(pkt);              // events happened between the host's previous frame and this one
        uint8_t *fr = ls_make_frame(pkt);
        free(pkt);
        return fr;
    }
    uint8_t *f = R ? *(uint8_t **)(R + 0x58) : NULL;   // original FUN_140294db0
    if (f) {
        *(uint8_t **)(R + 0x58) = *(uint8_t **)(*(uint8_t **)(R + 0x38) + 8 + (intptr_t)f);
        *(float *)(R + 0x6c) += *(float *)(f + 0x18);
        *(void **)(f + 0x70) = **(void ***)(f + 0x60);
        *(uint32_t *)(f + 0x78) = 0;
        if (*(float *)(R + 0x70) <= *(float *)(R + 0x6c)) *(uint32_t *)(R + 0x70) = 0xbf800000u;
    }
    return f;
}

// diagnostics: who creates objects (and so consumes replay ids) during lockstep frames
#define RVA_CREATE_OBJECT_ID 0x1a5e30u   // GamCLevel::internalCreateObject (id assignment)
static const uint8_t EXPECT_CREATE_OBJECT_ID[15] = { 0x48,0x89,0x5c,0x24,0x10, 0x48,0x89,0x6c,0x24,0x18, 0x48,0x89,0x74,0x24,0x20 };
typedef void *(*create_obj_fn)(void *level);
static create_obj_fn g_orig_create_obj;
static FILE *g_idlog;
static int g_in_create;

// Object GUIDs as an ordered stream (frame-independent): the host records every GUID generated while an object is
// created, the client hands out the same GUIDs in the same order.
#define RVA_GUID_GEN 0x16cdb0u        // uint64_t *newGuid(uint64_t out[2]) (boost uuid v4 via BCryptGenRandom)
static const uint8_t EXPECT_GUID_GEN[16] = { 0x48,0x89,0x5c,0x24,0x08, 0x57, 0x48,0x81,0xec,0xa0,0x00,0x00,0x00, 0x48,0x8b,0xd9 };
typedef uint64_t *(*guid_gen_fn)(uint64_t *out);
static guid_gen_fn g_orig_guid_gen;
static uint8_t *g_id_buf; static size_t g_id_n, g_id_cap, g_id_head;   // host: out buffer of (sig, guid) records
static uint64_t g_id_taken, g_id_starved;
static uint64_t g_cur_sig;            // creation-path signature of the object being created (set by hook_create_obj)
// host: record (signature, guid); 24 bytes per record
static void id_push_sig(uint64_t sig, const uint8_t g16[16]) {
    if (g_id_n == g_id_cap) { g_id_cap = g_id_cap ? g_id_cap * 2 : 4096; g_id_buf = realloc(g_id_buf, g_id_cap * 24); }
    memcpy(g_id_buf + 24 * g_id_n, &sig, 8); memcpy(g_id_buf + 24 * g_id_n + 8, g16, 16); g_id_n++;
}
static void id_push(const uint8_t g16[16]) { id_push_sig(g_cur_sig, g16); }
// client: one FIFO per creation-path signature (open addressing); entries {u32 host frame, guid[16]}.
// A host id is only used for a creation in (almost) the same frame: objects that some system creates at a different
// time on each side (pooled visuals driven by real time) then get local ids WITHOUT shifting the ids of later objects.
#define SIGQ_SLOTS 4096
#define SIGQ_ENT 20
#define ID_WINDOW 2   // frames
static struct sigq { uint64_t sig; uint8_t *ids; uint32_t n, cap, head; } g_sigq[SIGQ_SLOTS];
static struct sigq *sigq_get(uint64_t sig, int create) {
    uint32_t i = (uint32_t)(sig ^ (sig >> 29)) & (SIGQ_SLOTS - 1);
    for (int k = 0; k < SIGQ_SLOTS; k++, i = (i + 1) & (SIGQ_SLOTS - 1)) {
        if (g_sigq[i].sig == sig && (g_sigq[i].cap || !create)) return g_sigq[i].cap ? &g_sigq[i] : NULL;
        if (!g_sigq[i].cap) { if (!create) return NULL; g_sigq[i].sig = sig; g_sigq[i].cap = 64; g_sigq[i].ids = malloc(64 * SIGQ_ENT); return &g_sigq[i]; }
    }
    return NULL;
}
static void sigq_push(uint64_t sig, const uint8_t g16[16], uint32_t seq) {
    struct sigq *q = sigq_get(sig, 1); if (!q) return;
    if (q->head > 1024 && q->head * 2 > q->n) {   // compact consumed entries
        memmove(q->ids, q->ids + SIGQ_ENT * (size_t)q->head, SIGQ_ENT * (size_t)(q->n - q->head)); q->n -= q->head; q->head = 0;
    }
    if (q->n == q->cap) { q->cap *= 2; q->ids = realloc(q->ids, (size_t)q->cap * SIGQ_ENT); }
    uint8_t *e = q->ids + SIGQ_ENT * (size_t)q->n++;
    memcpy(e, &seq, 4); memcpy(e + 4, g16, 16);
}
static uint64_t g_id_local, g_id_dropped;
// client: the host id for this creation path made within ID_WINDOW frames of the current frame, if any
static int sigq_take(uint64_t sig, uint32_t f, uint8_t out[16]) {
    struct sigq *q = sigq_get(sig, 0);
    if (!q) return 0;
    while (q->head < q->n) {
        uint32_t s; memcpy(&s, q->ids + SIGQ_ENT * (size_t)q->head, 4);
        if (s + ID_WINDOW < f) { q->head++; g_id_dropped++; continue; }   // host-only (or much earlier) object
        if (s > f + ID_WINDOW) return 0;                                  // host creates it later: not this one
        memcpy(out, q->ids + SIGQ_ENT * (size_t)q->head + 4, 16); q->head++;
        return 1;
    }
    return 0;
}
static uint64_t g_cnt_create, g_cnt_guid, g_cnt_guid_in_create;
// UI building creation as a lockstep event: the host captures the GUIDs of one createBuilding call,
// the client replays that call and is fed exactly those GUIDs (mode 1 = host capture, 2 = client feed)
static int g_cb_mode;
static uint8_t g_cb_ids[16 * 4096]; static uint32_t g_cb_n, g_cb_i;
static uint64_t *ls_stream_guid(uint64_t *out);
static uint64_t *hook_guid_gen(uint64_t *out) {
    g_cnt_guid++;
    if (g_in_create) g_cnt_guid_in_create++;
    if (g_cb_mode == 1) { g_orig_guid_gen(out); if (g_cb_n < 4096) memcpy(g_cb_ids + 16 * g_cb_n++, out, 16); return out; }
    if (g_cb_mode == 2) { if (g_cb_i < g_cb_n) { memcpy(out, g_cb_ids + 16 * g_cb_i++, 16); return out; } return g_orig_guid_gen(out); }
    // the stream covers every GUID generated inside the simulation frame: object ids, and GUIDs the simulation uses
    // directly (e.g. house lot seeds); UI objects outside the frame stay local
    if (!(g_ls_go && g_in_frame) || g_local_depth) return g_orig_guid_gen(out);
    uint64_t saved_sig = g_cur_sig;
    if (!g_in_create) {   // a plain GUID request: key it by its own call path (FNV-1a over 6 exe return addresses)
        uint64_t sig = 1099511628211ull * 7;
        void *bt[16]; USHORT nfr = RtlCaptureStackBackTrace(1, 16, bt, NULL); int m = 0;
        for (USHORT i = 0; i < nfr && m < 6; i++) {
            uintptr_t a = (uintptr_t)bt[i];
            if (a >= (uintptr_t)g_base && a < (uintptr_t)g_base + 0x5000000) {
                uint64_t r = (uint64_t)(a - (uintptr_t)g_base);
                for (int b = 0; b < 8; b++) { sig ^= (r >> (8 * b)) & 0xff; sig *= 1099511628211ull; }
                m++;
            }
        }
        g_cur_sig = sig;
        static int logged; if (logged < 30) { logged++; logf_("lockstep: plain in-frame GUID request, path %016llx (ret %p)", (unsigned long long)sig, nfr ? (void *)((uintptr_t)bt[0] - (uintptr_t)g_base) : NULL); }
    }
    uint64_t *r = ls_stream_guid(out);
    g_cur_sig = saved_sig;
    return r;
}
static uint64_t *ls_stream_guid(uint64_t *out) {
    if (g_ls_role == 1) { g_orig_guid_gen(out); id_push((uint8_t *)out); return out; }
    if (g_ls_role == 2) {
        if (sigq_take(g_cur_sig, g_ls_cur_seq, (uint8_t *)out)) { g_id_taken++; return out; }
        // the frames up to ID_WINDOW ahead may still be in flight: wait briefly for them
        DWORD t0 = GetTickCount();
        while (GetTickCount() - t0 < 100) {
            ls_client_drain();
            if (sigq_take(g_cur_sig, g_ls_cur_seq, (uint8_t *)out)) { g_id_taken++; return out; }
            int have_ahead = 0;
            for (int i = 0; i < g_ls_qn; i++) if (((ls_hdr *)g_ls_q[i])->seq >= g_ls_cur_seq + ID_WINDOW) { have_ahead = 1; break; }
            if (have_ahead) break;
            Sleep(1);
        }
        g_id_local++;
        if (g_id_local < 200 || (g_id_local % 1000) == 0)
            logf_("lockstep client: local id for creation path %016llx at host seq %u (taken %llu, local %llu, dropped %llu)",
                  (unsigned long long)g_cur_sig, g_ls_cur_seq, (unsigned long long)g_id_taken, (unsigned long long)g_id_local,
                  (unsigned long long)g_id_dropped);
    }
    return g_orig_guid_gen(out);
}

static void *hook_create_obj(void *level) {
    if (g_ls_go && g_idlog) {
        void *bt[24]; USHORT nfr = RtlCaptureStackBackTrace(1, 24, bt, NULL);
        char sig[256]; int k = 0, m = 0;
        for (USHORT i = 0; i < nfr && m < 16 && k < 240; i++) {
            uintptr_t a = (uintptr_t)bt[i];
            if (a >= (uintptr_t)g_base && a < (uintptr_t)g_base + 0x5000000) { k += snprintf(sig + k, sizeof sig - k, " %06llx", (unsigned long long)(a - (uintptr_t)g_base)); m++; }
        }
        sig[k] = 0;
        uint32_t seq = g_ls_role == 1 ? g_ls_seq : g_ls_cur_seq;
        uint8_t flag = 0; safe_read((uint8_t *)level + 0x254, &flag, 1);
        static DWORD main_tid; if (!main_tid && g_in_frame) main_tid = GetCurrentThreadId();
        fprintf(g_idlog, "%u%s%s%s%s\n", seq, flag ? " [lvlflag]" : "", g_in_frame ? " [frame]" : " [outside]",
                GetCurrentThreadId() == main_tid ? "" : " [other-thread]", sig);
    }
    g_cnt_create++;
    if (!g_ls_go) return g_orig_create_obj(level);
    // creation-path signature: FNV-1a over the first 6 return addresses inside the exe (as RVAs)
    uint64_t sig = 1469598103934665603ull, saved_sig = g_cur_sig;
    {
        void *bt[16]; USHORT nfr = RtlCaptureStackBackTrace(1, 16, bt, NULL); int m = 0;
        for (USHORT i = 0; i < nfr && m < 6; i++) {
            uintptr_t a = (uintptr_t)bt[i];
            if (a >= (uintptr_t)g_base && a < (uintptr_t)g_base + 0x5000000) {
                uint64_t r = (uint64_t)(a - (uintptr_t)g_base);
                for (int b = 0; b < 8; b++) { sig ^= (r >> (8 * b)) & 0xff; sig *= 1099511628211ull; }
                m++;
            }
        }
    }
    g_cur_sig = sig;
    // client: hide the replay frame so the engine skips its per-frame id list and calls the (hooked) GUID generator
    uint8_t *game = NULL; void *saved = NULL;
    if (g_ls_role == 2 && safe_read((uint8_t *)level + 0x210, &game, 8) && game) { saved = *(void **)(game + 0x3d0); *(void **)(game + 0x3d0) = NULL; }
    g_in_create++;
    void *r = g_orig_create_obj(level);
    g_in_create--;
    g_cur_sig = saved_sig;
    if (game && g_ls_role == 2) *(void **)(game + 0x3d0) = saved;
    return r;
}

static int install_hook(void) {
    g_orig = (update_frame_fn)hook_at(RVA_UPDATE_FRAME, EXPECT_UPDATE_FRAME, sizeof EXPECT_UPDATE_FRAME, (void *)hook_update_frame, "updateFrame");
    g_orig_create_building = (create_building_fn)hook_at(RVA_CREATE_BUILDING, EXPECT_CREATE_BUILDING, sizeof EXPECT_CREATE_BUILDING,
                                                          (void *)hook_create_building, "createBuilding");
    g_orig_resolve = (resolve_fn)hook_at(RVA_RESOLVE_REF, EXPECT_RESOLVE_REF, sizeof EXPECT_RESOLVE_REF, (void *)hook_resolve, "resolveRef");
    hook_at(RVA_PLAY_NEXT_FRAME, EXPECT_PLAY_NEXT_FRAME, sizeof EXPECT_PLAY_NEXT_FRAME, (void *)hook_play_next_frame, "playNextFrame (replaced)");
    g_orig_push_cmd = (push_cmd_fn)hook_at(RVA_PUSH_PLAYER_COMMAND, EXPECT_PUSH_CMD, sizeof EXPECT_PUSH_CMD, (void *)hook_push_cmd, "pushPlayerCommand");
    if (g_ls_role) {
        wchar_t pth[MAX_PATH]; _snwprintf(pth, MAX_PATH - 1, L"%ls\\idlog.txt", g_dir); pth[MAX_PATH - 1] = 0;
        if (g_dev) g_idlog = _wfopen(pth, L"w");
        g_orig_create_obj = (create_obj_fn)hook_at(RVA_CREATE_OBJECT_ID, EXPECT_CREATE_OBJECT_ID, sizeof EXPECT_CREATE_OBJECT_ID, (void *)hook_create_obj, "createObjectId");
        install_rand_hooks();
        g_orig_guid_gen = (guid_gen_fn)hook_at(RVA_GUID_GEN, EXPECT_GUID_GEN, sizeof EXPECT_GUID_GEN, (void *)hook_guid_gen, "guidGen");
        g_orig_part_event_fire = (part_event_fire_fn)hook_at(RVA_PART_EVENT_FIRE, EXPECT_PART_EVENT_FIRE, sizeof EXPECT_PART_EVENT_FIRE, (void *)hook_part_event_fire, "partEventFire");
        g_orig_read_cmd = (read_cmd_fn)hook_at(RVA_READ_COMMAND, EXPECT_READ_CMD, sizeof EXPECT_READ_CMD, (void *)hook_read_cmd, "readCommand");
        g_orig_record_cmd = (record_cmd_fn)hook_at(RVA_REC_COMMAND, EXPECT_RECORD_CMD, sizeof EXPECT_RECORD_CMD, (void *)hook_record_cmd, "recordCommand");
        g_orig_set_edit_mode = (set_edit_mode_fn)hook_at(RVA_SET_EDIT_MODE, EXPECT_SET_EDIT_MODE, sizeof EXPECT_SET_EDIT_MODE, (void *)hook_set_edit_mode, "setEditMode");
        g_orig_set_is_placed = (set_is_placed_fn)hook_at(RVA_SET_IS_PLACED, EXPECT_SET_IS_PLACED, sizeof EXPECT_SET_IS_PLACED, (void *)hook_set_is_placed, "setIsPlaced");
    }
    return g_orig != NULL;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)h; (void)r;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    if (!GetEnvironmentVariableW(L"FCOOP_INSTANCE_DIR", g_dir, MAX_PATH)) return TRUE;
    wchar_t path[MAX_PATH]; _snwprintf(path, MAX_PATH - 1, L"%ls\\coop.log", g_dir); path[MAX_PATH - 1] = 0;
    g_log = _wfopen(path, L"w");
    _snwprintf(path, MAX_PATH - 1, L"%ls\\bindings.txt", g_dir);
    { FILE *bf = _wfopen(path, L"w"); if (bf) fclose(bf); }   // fresh binding table per run
    g_base = (uint8_t *)GetModuleHandleW(NULL);
    wchar_t role[16] = L"";
    GetEnvironmentVariableW(L"FCOOP_ROLE", role, 16);
    { wchar_t dv[8] = L""; GetEnvironmentVariableW(L"FCOOP_DEV", dv, 8); g_dev = dv[0] == L'1'; }
    if (!wcscmp(role, L"a"))         { g_send = g_recv = 1; g_my_port = 27800; g_peer_port = 27801; }
    else if (!wcscmp(role, L"b"))    { g_send = g_recv = 1; g_my_port = 27801; g_peer_port = 27800; }
    else if (!wcscmp(role, L"send")) { g_send = 1; g_peer_port = 27800; }
    else if (!wcscmp(role, L"recv")) { g_recv = 1; g_my_port = 27800; }
    else if (!wcscmp(role, L"host"))   { g_ls_role = 1; g_peer_port = 27801; }
    else if (!wcscmp(role, L"client")) { g_ls_role = 2; g_my_port = 27801; }
    g_role = g_send || g_recv || g_ls_role;
    logf_("role %ls: send=%d recv=%d my_port=%u peer_port=%u", role, g_send, g_recv, g_my_port, g_peer_port);
    int ok = install_hook();
    logf_("coop.dll loaded, pid %lu, base %p, updateFrame hook %s", GetCurrentProcessId(), g_base, ok ? "installed" : "FAILED");
    if (g_log) fflush(g_log);
    return TRUE;
}
