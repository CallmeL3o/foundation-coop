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
} N;

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
    if (N.sock != INVALID_SOCKET) { closesocket(N.sock); N.sock = INVALID_SOCKET; logf_("lsnet: connection closed (%s)", why); }
    N.ready = 0; N.hs_state = 0; N.rlen = 0; N.send_ctr = N.recv_ctr = 0;
}

static int lsn_send_raw(const void *p, size_t n) {
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
                if (!strcmp(N.allow_ip, "*")) { strcpy(N.allow_ip, N.cur_ip); logf_("lsnet: from now on only %s may connect", N.allow_ip); }
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

// accept / connect, handshake, receive; calls h for every authenticated message
static void lsn_poll(lsn_handler h) {
    if (!N.role) return;
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
    return lsn_send_raw(head, 16) && (!n || lsn_send_raw(p, n)) && lsn_send_raw(mac, 16);
}
