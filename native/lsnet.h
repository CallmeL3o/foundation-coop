// lsnet.h - authenticated lockstep transport for coop.dll (included by coop.c; one host, one client).
//
// One TCP connection (reliable + ordered, which lockstep needs; the old UDP stream had no retransmission).
// Security model (playbook: authenticated session, configured peer only, known message types only):
//  - Pre-shared key: 32 random bytes as 64 hex chars in <instance>\coop_key.txt (the host makes it, the friend gets a copy
//    through a private channel). Without a key file there is no networking at all.
//  - Host binds FCOOP_LS_BIND (default 127.0.0.1) and accepts only the IP in FCOOP_LS_ALLOW (default 127.0.0.1),
//    one connection at a time; the handshake must finish within 5 s. FCOOP_LS_ALLOW=* (join-code mode) accepts any
//    address that proves the key; after the first successful handshake only that address is accepted.
//  - Handshake: host -> "FCH1" Nh[16]; client -> "FCC1" Nc[16] HMAC(K, "client"|Nh|Nc); host -> "FCS1" HMAC(K, "host"|Nh|Nc).
//    Both nonces from BCryptGenRandom. Session key S = HMAC(K, "session"|Nh|Nc).
//  - Every message: u32 bodylen | u64 counter | u32 type | payload | mac[16] = HMAC(S, dir|counter|type|payload)[0:16],
//    dir = 'H' (host->client) or 'C'. The counter must be exactly the next one (no replay, no reordering, no reflection).
//    Bad MAC, wrong counter, unknown type, wrong direction or a body over 1 MiB closes the connection.
//  - Not encrypted: game commands only (no account data); integrity and peer authentication are what matters here.
//  - FCOOP_NET=steam: the same byte stream (handshake, MAC, counters: unchanged) is carried over Steam's relay network
//    (ISteamNetworkingMessages from the game's own steam_api64.dll, reliable, channel 0) instead of TCP: no port
//    forwarding, no VPN, IP addresses stay hidden. The client knows the host's SteamID (FCOOP_STEAM_HOST, from the join
//    code) and opens an attempt with "FCR1" on channel 1; the host answers with the normal handshake. The host accepts
//    Steam sessions from anyone until the first peer proves the key, then only from that SteamID ("FCX1" on channel 1
//    tells a peer whose session the host no longer has to reconnect).
#include <ws2tcpip.h>
#include <bcrypt.h>

#define LSN_FRAME 1   // host -> client: one simulation frame (ls packet)
#define LSN_ACK   2   // client -> host: frame acknowledged
#define LSN_CMD   3   // client -> host: serialized player command
#define LSN_UIEV  4   // client -> host: UI event
#define LSN_LOBBY 5   // both: lobby (hello / ready / go) before the simulation starts
#define LSN_SAVE  6   // host -> client: a chunk of the host's save file
#define LSN_MAX_BODY (1u << 20)

typedef void (*lsn_handler)(uint32_t type, uint8_t *payload, uint32_t n);

static struct {
    int role, ready, configured;          // role 1 host, 2 client; ready = session established
    SOCKET lsock, sock;
    uint8_t key[32], skey[32], nh[16], nc[16];
    int hs_state;                          // host: 0 none, 1 sent FCH1; client: 0 none, 1 sent FCC1
    DWORD hs_start;
    uint64_t send_ctr, recv_ctr;
    uint8_t *rbuf; size_t rlen, rcap;
    char bind_ip[64], allow_ip[64], host_ip[64], cur_ip[64]; unsigned short port;
    BCRYPT_ALG_HANDLE alg;
    DWORD next_connect;
    int steam;                             // 1: Steam relay transport instead of TCP
} N;

// ---- Steam transport (flat Steamworks API of the game's steam_api64.dll) ----
#define LSN_SID_TYPE 136                   // sizeof(SteamNetworkingIdentity)
static struct {
    void *msgs, *utils, *socks;
    int (*send)(void *, const void *, const void *, uint32_t, int, int);
    int (*recv)(void *, int, void **, int);
    uint8_t (*accept)(void *, const void *);
    void (*release)(void *);
    void (*setid)(void *, uint64_t);
    uint64_t (*getid)(const void *);
    void (*initrelay)(void *);
    uint8_t (*setcfg)(void *, int, int, intptr_t, int, const void *);
    void (*runcb)(void *);
    int (*relay)(void *, void *);
    uint64_t peer, lock, self;             // client: host SteamID; host: current peer, locked peer
    uint8_t ident[LSN_SID_TYPE];           // identity of the current peer
    int relay_state; DWORD last_fcx, attempts;
} ST;
static unsigned lsn_sid_tail(uint64_t s) { return (unsigned)(s % 10000); }   // logs show only the last 4 digits
static void lsn_st_ident(uint8_t id[LSN_SID_TYPE], uint64_t sid) { memset(id, 0, LSN_SID_TYPE); ST.setid(id, sid); }
static int lsn_st_send(uint64_t sid, const void *p, uint32_t n, int channel) {
    uint8_t id[LSN_SID_TYPE]; lsn_st_ident(id, sid);
    return ST.send(ST.msgs, id, p, n, 8 | 32, channel);   // Reliable | AutoRestartBrokenSession
}
// SteamNetworkingMessagesSessionRequest_t starts with the requesting identity
static void lsn_st_on_request(void *req) {
    uint64_t s = ST.getid(req);
    int ok = N.role == 2 ? s == ST.peer : (!ST.lock || s == ST.lock);
    if (ok) ST.accept(ST.msgs, req);
    logf_("lsnet: steam session request from ...%04u %s", lsn_sid_tail(s), ok ? "accepted" : "ignored (not the session's player)");
}
static int lsn_steam_open(void) {
    HMODULE d = NULL;
    for (int t = 0; t < 600 && !(d = GetModuleHandleW(L"steam_api64.dll")); t++) Sleep(100);
    if (!d) { logf_("lsnet: steam_api64.dll not loaded"); return 0; }
#define SGET(dst, name) do { *(FARPROC *)&dst = GetProcAddress(d, name); if (!dst) { logf_("lsnet: steam_api64.dll lacks %s", name); return 0; } } while (0)
    void *(*a_msgs)(void), *(*a_utils)(void), *(*a_socks)(void), *(*a_user)(void);
    int (*huser)(void); uint64_t (*user_id)(void *);
    SGET(a_msgs, "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
    SGET(a_utils, "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
    SGET(a_socks, "SteamAPI_SteamNetworkingSockets_SteamAPI_v012");
    SGET(a_user, "SteamAPI_SteamUser_v023");
    SGET(user_id, "SteamAPI_ISteamUser_GetSteamID");
    SGET(huser, "SteamAPI_GetHSteamUser");
    SGET(ST.send, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
    SGET(ST.recv, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
    SGET(ST.accept, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
    SGET(ST.release, "SteamAPI_SteamNetworkingMessage_t_Release");
    SGET(ST.setid, "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
    SGET(ST.getid, "SteamAPI_SteamNetworkingIdentity_GetSteamID64");
    SGET(ST.initrelay, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
    SGET(ST.setcfg, "SteamAPI_ISteamNetworkingUtils_SetConfigValue");
    SGET(ST.runcb, "SteamAPI_ISteamNetworkingSockets_RunCallbacks");
    SGET(ST.relay, "SteamAPI_ISteamNetworkingUtils_GetRelayNetworkStatus");
#undef SGET
    int t = 0;
    for (; t < 1200 && !huser(); t++) Sleep(100);   // wait for the game's SteamAPI_Init
    if (!huser()) { logf_("lsnet: Steam was not initialized by the game (is Steam running?)"); return 0; }
    ST.msgs = a_msgs(); ST.utils = a_utils(); ST.socks = a_socks();
    void *user = a_user(); ST.self = user ? user_id(user) : 0;
    if (!ST.msgs || !ST.utils) { logf_("lsnet: Steam networking interfaces unavailable"); return 0; }
    void *cb = (void *)lsn_st_on_request;
    int32_t sendbuf = 8 << 20, rmin = 512 << 10, rmax = 8 << 20;
    int ok1 = ST.setcfg(ST.utils, 204, 1, 0, 5, &cb);          // Callback_MessagesSessionRequest, global, pointer
    int ok2 = ST.setcfg(ST.utils, 9, 1, 0, 1, &sendbuf);       // SendBufferSize
    int ok3 = ST.setcfg(ST.utils, 10, 1, 0, 1, &rmin);         // SendRateMin
    int ok4 = ST.setcfg(ST.utils, 11, 1, 0, 1, &rmax);         // SendRateMax
    ST.initrelay(ST.utils);
    logf_("lsnet: Steam transport ready (me ...%04u, config %d%d%d%d)", lsn_sid_tail(ST.self), ok1, ok2, ok3, ok4);
    if (!ok1) { logf_("lsnet: cannot install the Steam session callback"); return 0; }
    return 1;
}

static int lsn_hmac(const uint8_t *key, size_t klen, const void *a, size_t an, const void *b, size_t bn,
                    const void *c, size_t cn, uint8_t out[32]) {
    BCRYPT_HASH_HANDLE h = NULL;
    if (BCryptCreateHash(N.alg, &h, NULL, 0, (PUCHAR)key, (ULONG)klen, 0) != 0) return 0;
    int ok = 1;
    if (an && BCryptHashData(h, (PUCHAR)a, (ULONG)an, 0) != 0) ok = 0;
    if (ok && bn && BCryptHashData(h, (PUCHAR)b, (ULONG)bn, 0) != 0) ok = 0;
    if (ok && cn && BCryptHashData(h, (PUCHAR)c, (ULONG)cn, 0) != 0) ok = 0;
    if (ok && BCryptFinishHash(h, out, 32, 0) != 0) ok = 0;
    BCryptDestroyHash(h);
    return ok;
}
// constant-time compare
static int lsn_eq(const uint8_t *a, const uint8_t *b, size_t n) { uint8_t d = 0; for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i]; return d == 0; }

static void lsn_env(const wchar_t *name, char *dst, size_t cap, const char *def) {
    wchar_t w[64];
    if (GetEnvironmentVariableW(name, w, 64)) { WideCharToMultiByte(CP_UTF8, 0, w, -1, dst, (int)cap, NULL, NULL); return; }
    strncpy(dst, def, cap - 1); dst[cap - 1] = 0;
}

static int lsn_read_key(void) {
    wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH - 1, L"%ls\\coop_key.txt", g_dir); p[MAX_PATH - 1] = 0;
    FILE *f = _wfopen(p, L"rb"); if (!f) return 0;
    char hex[80] = {0}; size_t n = fread(hex, 1, sizeof hex - 1, f); fclose(f);
    while (n && (hex[n - 1] == '\r' || hex[n - 1] == '\n' || hex[n - 1] == ' ')) hex[--n] = 0;
    if (n != 64) return 0;
    for (int i = 0; i < 32; i++) {
        unsigned v; if (sscanf(hex + 2 * i, "%2x", &v) != 1) return 0;
        N.key[i] = (uint8_t)v;
    }
    return 1;
}

static void lsn_close(const char *why) {
    if (N.sock != INVALID_SOCKET) {
        if (!N.steam) closesocket(N.sock);
        N.sock = INVALID_SOCKET; logf_("lsnet: connection closed (%s)", why);
    }
    N.ready = 0; N.hs_state = 0; N.rlen = 0; N.send_ctr = N.recv_ctr = 0;
}

static int lsn_send_raw(const void *p, size_t n) {
    if (N.steam) {   // reliable Steam messages; split into <= 256 KiB pieces (the stream has no message boundaries)
        const uint8_t *c = (const uint8_t *)p; DWORD t0 = GetTickCount();
        while (n) {
            uint32_t k = n > (256u << 10) ? (256u << 10) : (uint32_t)n;
            int r = ST.send(ST.msgs, ST.ident, c, k, 8 | 32, 0);
            if (r == 1) { c += k; n -= k; t0 = GetTickCount(); continue; }
            if (r == 25 && GetTickCount() - t0 < 30000) { Sleep(5); continue; }   // LimitExceeded: send buffer full
            logf_("lsnet: steam send failed (result %d)", r); lsn_close("send failed"); return 0;
        }
        return 1;
    }
    const char *c = (const char *)p; DWORD t0 = GetTickCount();
    while (n) {
        int r = send(N.sock, c, (int)n, 0);
        if (r > 0) { c += r; n -= (size_t)r; t0 = GetTickCount(); continue; }
        if (WSAGetLastError() != WSAEWOULDBLOCK || GetTickCount() - t0 > 15000) { lsn_close("send failed"); return 0; }
        fd_set w; FD_ZERO(&w); FD_SET(N.sock, &w); struct timeval tv = {0, 20000}; select(0, NULL, &w, NULL, &tv);
    }
    return 1;
}

static void lsn_init(int role) {
    if (N.configured) return;
    N.configured = 1; N.role = role; N.lsock = N.sock = INVALID_SOCKET;
    if (!lsn_read_key()) { logf_("lsnet: no valid coop_key.txt (64 hex chars) in the instance folder: networking OFF"); N.role = 0; return; }
    if (BCryptOpenAlgorithmProvider(&N.alg, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        logf_("lsnet: no HMAC provider: networking OFF"); N.role = 0; return;
    }
    char port[16];
    lsn_env(L"FCOOP_LS_BIND", N.bind_ip, sizeof N.bind_ip, "127.0.0.1");
    lsn_env(L"FCOOP_LS_ALLOW", N.allow_ip, sizeof N.allow_ip, "127.0.0.1");
    lsn_env(L"FCOOP_LS_HOST", N.host_ip, sizeof N.host_ip, "127.0.0.1");
    lsn_env(L"FCOOP_LS_PORT", port, sizeof port, "27810");
    N.port = (unsigned short)atoi(port);
    char net[16]; lsn_env(L"FCOOP_NET", net, sizeof net, "tcp");
    if (!_stricmp(net, "steam")) {
        N.steam = 1;
        if (role == 2) {
            char h[32]; lsn_env(L"FCOOP_STEAM_HOST", h, sizeof h, "0");
            ST.peer = _strtoui64(h, NULL, 10);
            if (!ST.peer) { logf_("lsnet: FCOOP_STEAM_HOST missing"); N.role = 0; return; }
        }
        if (!lsn_steam_open()) { N.role = 0; return; }
        if (role == 1) logf_("lsnet: host waiting for a player over Steam");
        else logf_("lsnet: client will connect over Steam to host ...%04u", lsn_sid_tail(ST.peer));
        return;
    }
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    if (role == 1) {
        struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(N.port);
        if (inet_pton(AF_INET, N.bind_ip, &a.sin_addr) != 1) { logf_("lsnet: bad FCOOP_LS_BIND %s", N.bind_ip); N.role = 0; return; }
        N.lsock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        BOOL excl = TRUE; setsockopt(N.lsock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof excl);
        u_long nb = 1; ioctlsocket(N.lsock, FIONBIO, &nb);
        if (bind(N.lsock, (struct sockaddr *)&a, sizeof a) != 0 || listen(N.lsock, 1) != 0) {
            logf_("lsnet: host cannot listen on %s:%u (%d)", N.bind_ip, N.port, WSAGetLastError()); N.role = 0; return;
        }
        logf_("lsnet: host listening on %s:%u, allowed peer %s", N.bind_ip, N.port, N.allow_ip);
    } else logf_("lsnet: client will connect to %s:%u", N.host_ip, N.port);
}

// parse complete messages in the receive buffer
static void lsn_parse(lsn_handler h) {
    size_t off = 0;
    while (N.sock != INVALID_SOCKET) {
        if (!N.ready) {   // handshake messages (fixed sizes)
            if (N.role == 2 && N.hs_state == 0) {
                if (N.rlen - off < 20) break;
                if (memcmp(N.rbuf + off, "FCH1", 4)) { lsn_close("bad handshake"); return; }
                memcpy(N.nh, N.rbuf + off + 4, 16); off += 20;
                BCryptGenRandom(NULL, N.nc, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
                uint8_t m[52]; memcpy(m, "FCC1", 4); memcpy(m + 4, N.nc, 16);
                if (!lsn_hmac(N.key, 32, "client", 6, N.nh, 16, N.nc, 16, m + 20)) { lsn_close("hmac"); return; }
                if (!lsn_send_raw(m, sizeof m)) return;
                N.hs_state = 1; continue;
            }
            if (N.role == 2 && N.hs_state == 1) {
                if (N.rlen - off < 36) break;
                uint8_t mac[32];
                if (memcmp(N.rbuf + off, "FCS1", 4) || !lsn_hmac(N.key, 32, "host", 4, N.nh, 16, N.nc, 16, mac) ||
                    !lsn_eq(mac, N.rbuf + off + 4, 32)) { lsn_close("host failed authentication"); return; }
                off += 36;
                lsn_hmac(N.key, 32, "session", 7, N.nh, 16, N.nc, 16, N.skey);
                N.ready = 1; logf_("lsnet: session established with host"); continue;
            }
            if (N.role == 1 && N.hs_state == 1) {
                if (N.rlen - off < 52) break;
                uint8_t mac[32];
                memcpy(N.nc, N.rbuf + off + 4, 16);
                if (memcmp(N.rbuf + off, "FCC1", 4) || !lsn_hmac(N.key, 32, "client", 6, N.nh, 16, N.nc, 16, mac) ||
                    !lsn_eq(mac, N.rbuf + off + 20, 32)) { lsn_close("client failed authentication"); return; }
                off += 52;
                uint8_t m[36]; memcpy(m, "FCS1", 4);
                lsn_hmac(N.key, 32, "host", 4, N.nh, 16, N.nc, 16, m + 4);
                if (!lsn_send_raw(m, sizeof m)) return;
                lsn_hmac(N.key, 32, "session", 7, N.nh, 16, N.nc, 16, N.skey);
                N.ready = 1; logf_("lsnet: session established with client %s", N.cur_ip);
                if (N.steam) { if (!ST.lock) { ST.lock = ST.peer; logf_("lsnet: from now on only this Steam player may connect"); } }
                else if (!strcmp(N.allow_ip, "*")) { strcpy(N.allow_ip, N.cur_ip); logf_("lsnet: from now on only %s may connect", N.allow_ip); }
                continue;
            }
            break;
        }
        if (N.rlen - off < 4) break;
        uint32_t blen; memcpy(&blen, N.rbuf + off, 4);
        if (blen < 12 + 16 || blen > LSN_MAX_BODY) { lsn_close("bad message length"); return; }
        if (N.rlen - off < 4 + (size_t)blen) break;
        uint8_t *b = N.rbuf + off + 4;
        uint64_t ctr; uint32_t type; memcpy(&ctr, b, 8); memcpy(&type, b + 8, 4);
        uint8_t mac[32]; char dir = N.role == 1 ? 'C' : 'H';
        if (!lsn_hmac(N.skey, 32, &dir, 1, b, blen - 16, NULL, 0, mac) || !lsn_eq(mac, b + blen - 16, 16)) { lsn_close("bad MAC"); return; }
        if (ctr != N.recv_ctr) { lsn_close("bad counter"); return; }
        N.recv_ctr++;
        int ok_type = N.role == 1 ? (type == LSN_ACK || type == LSN_CMD || type == LSN_UIEV || type == LSN_LOBBY)
                                  : (type == LSN_FRAME || type == LSN_LOBBY || type == LSN_SAVE);
        if (!ok_type) { lsn_close("unexpected message type"); return; }
        off += 4 + (size_t)blen;
        h(type, b + 12, blen - 12 - 16);
    }
    if (off) { memmove(N.rbuf, N.rbuf + off, N.rlen - off); N.rlen -= off; }
}

static int lsn_room(size_t need) {
    if (N.rcap - N.rlen < need) {
        size_t c = N.rcap ? N.rcap : 1 << 17;
        while (c - N.rlen < need) c *= 2;
        uint8_t *b = realloc(N.rbuf, c); if (!b) return 0;
        N.rbuf = b; N.rcap = c;
    }
    return 1;
}

// SteamNetworkingMessage_t: m_pData at 0, m_cbSize at 8, m_identityPeer at 16
#define SMSG_DATA(m) (*(uint8_t **)(m))
#define SMSG_SIZE(m) (*(int *)((uint8_t *)(m) + 8))
#define SMSG_PEER(m) ((uint8_t *)(m) + 16)
static void lsn_steam_poll(void) {
    if (ST.socks) ST.runcb(ST.socks);   // delivers the session-request callback (the game's SteamAPI_RunCallbacks does too)
    int rs = ST.relay(ST.utils, NULL);
    if (rs != ST.relay_state) { logf_("lsnet: Steam relay network status %d%s", rs, rs == 100 ? " (ready)" : ""); ST.relay_state = rs; }
    void *ms[64]; int k;
    // control channel
    while ((k = ST.recv(ST.msgs, 1, ms, 64)) > 0) {
        for (int i = 0; i < k; i++) {
            uint64_t s = ST.getid(SMSG_PEER(ms[i]));
            int isreq = SMSG_SIZE(ms[i]) == 4 && !memcmp(SMSG_DATA(ms[i]), "FCR1", 4);
            int isrst = SMSG_SIZE(ms[i]) == 4 && !memcmp(SMSG_DATA(ms[i]), "FCX1", 4);
            if (N.role == 1 && isreq && (!ST.lock || s == ST.lock) && !(N.ready && s != ST.peer)) {
                if (N.sock != INVALID_SOCKET) lsn_close("player reconnects");
                ST.peer = s; lsn_st_ident(ST.ident, s);
                snprintf(N.cur_ip, sizeof N.cur_ip, "steam ...%04u", lsn_sid_tail(s));
                N.sock = (SOCKET)1; N.hs_start = GetTickCount();
                BCryptGenRandom(NULL, N.nh, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
                uint8_t m[20]; memcpy(m, "FCH1", 4); memcpy(m + 4, N.nh, 16);
                logf_("lsnet: connection from %s, authenticating", N.cur_ip);
                if (lsn_send_raw(m, sizeof m)) N.hs_state = 1;
            } else if (N.role == 2 && isrst && s == ST.peer && N.sock != INVALID_SOCKET) lsn_close("host asked to reconnect");
            ST.release(ms[i]);
        }
        if (k < 64) break;
    }
    // client: start a connection attempt
    if (N.role == 2 && N.sock == INVALID_SOCKET && GetTickCount() >= N.next_connect) {
        N.next_connect = GetTickCount() + 1000;
        lsn_st_ident(ST.ident, ST.peer);
        int r = ST.send(ST.msgs, ST.ident, "FCR1", 4, 8 | 32, 1);
        if (ST.attempts++ % 10 == 0) logf_("lsnet: asking the host to connect over Steam (attempt %lu, result %d)", ST.attempts, r);
        if (r == 1) { N.sock = (SOCKET)1; N.hs_state = 0; N.hs_start = GetTickCount(); N.rlen = 0; }
    }
    // data channel
    while ((k = ST.recv(ST.msgs, 0, ms, 64)) > 0) {
        for (int i = 0; i < k; i++) {
            uint64_t s = ST.getid(SMSG_PEER(ms[i])); int sz = SMSG_SIZE(ms[i]);
            if (N.sock != INVALID_SOCKET && s == ST.peer && sz > 0) {
                if (N.rlen + (size_t)sz > 8 * (size_t)LSN_MAX_BODY || !lsn_room((size_t)sz)) { ST.release(ms[i]); lsn_close("receive buffer overflow"); continue; }
                memcpy(N.rbuf + N.rlen, SMSG_DATA(ms[i]), (size_t)sz); N.rlen += (size_t)sz;
            } else if (N.role == 1 && (!ST.lock || s == ST.lock) && GetTickCount() - ST.last_fcx > 1000) {
                ST.last_fcx = GetTickCount(); lsn_st_send(s, "FCX1", 4, 1);   // stale session: tell the player to reconnect
            }
            ST.release(ms[i]);
        }
        if (k < 64) break;
    }
}

// accept / connect, handshake, receive; calls h for every authenticated message
static void lsn_poll(lsn_handler h) {
    if (!N.role) return;
    if (N.steam) {
        lsn_steam_poll();
        if (N.sock == INVALID_SOCKET) return;
        if (!N.ready && GetTickCount() - N.hs_start > 20000) { lsn_close("handshake timeout"); return; }
        lsn_parse(h);
        return;
    }
    if (N.sock == INVALID_SOCKET) {
        if (N.role == 1) {
            struct sockaddr_in pa; int pl = sizeof pa;
            SOCKET s = accept(N.lsock, (struct sockaddr *)&pa, &pl);
            if (s == INVALID_SOCKET) return;
            char ip[64]; inet_ntop(AF_INET, &pa.sin_addr, ip, sizeof ip);
            if (strcmp(N.allow_ip, "*") != 0 && strcmp(ip, N.allow_ip) != 0) { logf_("lsnet: rejected connection from %s (not the allowed peer)", ip); closesocket(s); return; }
            u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
            BOOL nd = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nd, sizeof nd);
            N.sock = s; N.hs_start = GetTickCount();
            BCryptGenRandom(NULL, N.nh, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
            uint8_t m[20]; memcpy(m, "FCH1", 4); memcpy(m + 4, N.nh, 16);
            logf_("lsnet: connection from %s, authenticating", ip);
            strncpy(N.cur_ip, ip, sizeof N.cur_ip - 1);
            if (!lsn_send_raw(m, sizeof m)) return;
            N.hs_state = 1;
        } else {
            if (GetTickCount() < N.next_connect) return;
            N.next_connect = GetTickCount() + 1000;
            struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(N.port);
            if (inet_pton(AF_INET, N.host_ip, &a.sin_addr) != 1) { logf_("lsnet: bad FCOOP_LS_HOST %s", N.host_ip); N.role = 0; return; }
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { closesocket(s); return; }   // retried every second
            u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
            BOOL nd = TRUE; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nd, sizeof nd);
            N.sock = s; N.hs_start = GetTickCount(); N.hs_state = 0;
            logf_("lsnet: connected to host, authenticating");
        }
    }
    if (!N.ready && GetTickCount() - N.hs_start > 5000) { lsn_close("handshake timeout"); return; }
    for (;;) {
        if (N.rcap - N.rlen < 65536) { N.rcap = N.rcap ? N.rcap * 2 : 1 << 17; N.rbuf = realloc(N.rbuf, N.rcap); }
        if (N.rcap > 4 * (size_t)LSN_MAX_BODY && N.rlen > 2 * (size_t)LSN_MAX_BODY) { lsn_close("receive buffer overflow"); return; }
        int r = recv(N.sock, (char *)N.rbuf + N.rlen, (int)(N.rcap - N.rlen), 0);
        if (r > 0) { N.rlen += (size_t)r; continue; }
        if (r == 0) { lsn_close("peer closed"); return; }
        if (WSAGetLastError() != WSAEWOULDBLOCK) { lsn_close("receive error"); return; }
        break;
    }
    lsn_parse(h);
}

static int lsn_send(uint32_t type, const void *p, uint32_t n) {
    if (!N.ready || n > LSN_MAX_BODY - 64) return 0;
    uint32_t blen = 12 + n + 16;
    uint8_t head[16]; memcpy(head, &blen, 4); memcpy(head + 4, &N.send_ctr, 8); memcpy(head + 12, &type, 4);
    uint8_t mac[32]; char dir = N.role == 1 ? 'H' : 'C';
    if (!lsn_hmac(N.skey, 32, &dir, 1, head + 4, 12, p, n, mac)) return 0;
    N.send_ctr++;
    // one buffer per message (one Steam message / one TCP send)
    static uint8_t small[8192]; size_t tot = 16 + (size_t)n + 16;
    uint8_t *b = tot <= sizeof small ? small : malloc(tot);
    if (!b) { lsn_close("out of memory"); return 0; }
    memcpy(b, head, 16); if (n) memcpy(b + 16, p, n); memcpy(b + 16 + n, mac, 16);
    int ok = lsn_send_raw(b, tot);
    if (b != small) free(b);
    return ok;
}
