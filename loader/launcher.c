// FoundationCoop.exe: one-window launcher for the Foundation co-op prototype.
//  - sets up a COPY of the game (never touches the Steam install) and puts the loader (dwmapi.dll) into it;
//  - Host: pick a city save, pick the address, get a join code; the save is copied into the co-op profile;
//  - Join: paste the join code;
//  - starts the game normally (no injection) with the co-op settings in its environment and shows the progress
//    that coop.dll writes to <instance>\status.txt.
// Join code = Crockford base32 of {version 1, IPv4[4], port[2], secret[16], check byte}. The session key is
// SHA-256("foundation-coop key v1" | secret); anyone with the code can join, so it is shared privately.
// Command line (for tests): --host <save> --addr <ip> | --join <code>, optional --game <dir>; writes the code to
// <kit>\last_code.txt and exits after starting the game.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdint.h>
#include <wchar.h>

#define GAME_SHA256 L"485003D4B4ED4408927FA41C6300C2D74D2F6ABAC4CD0896AEB17CC735277A7E"
#define DEFAULT_PORT 27810
#define B32 "0123456789ABCDEFGHJKMNPQRSTVWXYZ"

enum { ID_SETUP = 100, ID_CHOOSE_GAME, ID_CHOOSE_SAVE, ID_ADDR, ID_HOST, ID_CODE_OUT, ID_COPY, ID_CODE_IN, ID_JOIN,
       ID_OPEN, ID_HELP, ID_TIMER = 1, WM_COPY_PROGRESS = WM_APP + 1, WM_COPY_DONE };

static HWND g_wnd, g_game_lbl, g_save_lbl, g_addr, g_code_out, g_code_in, g_status, g_btn_setup, g_btn_host, g_btn_join;
static HFONT g_font, g_bold;
static wchar_t g_kit[MAX_PATH], g_ini[MAX_PATH], g_game[MAX_PATH], g_save[MAX_PATH], g_inst[MAX_PATH];
static wchar_t g_steam_game[MAX_PATH];
static int g_game_ok;                 // 0 none, 1 ok, 2 wrong version
static HANDLE g_proc;                 // running game
static int g_cli;                     // command-line mode
static volatile LONG g_copying;

// ---------- small helpers ----------
static void path_join(wchar_t *out, const wchar_t *a, const wchar_t *b) { _snwprintf(out, MAX_PATH - 1, L"%ls\\%ls", a, b); out[MAX_PATH - 1] = 0; }
static int file_exists(const wchar_t *p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
static int dir_exists(const wchar_t *p) { DWORD a = GetFileAttributesW(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static void mkdirs(const wchar_t *p) {
    wchar_t t[MAX_PATH]; wcsncpy(t, p, MAX_PATH - 1); t[MAX_PATH - 1] = 0;
    for (wchar_t *c = t + 3; *c; c++) if (*c == L'\\') { *c = 0; CreateDirectoryW(t, NULL); *c = L'\\'; }
    CreateDirectoryW(t, NULL);
}
static void set_text(HWND h, const wchar_t *fmt, ...) {
    wchar_t b[1024]; va_list ap; va_start(ap, fmt); _vsnwprintf(b, 1023, fmt, ap); va_end(ap); b[1023] = 0;
    SetWindowTextW(h, b);
}
static void status(const wchar_t *fmt, ...) {
    wchar_t b[1024]; va_list ap; va_start(ap, fmt); _vsnwprintf(b, 1023, fmt, ap); va_end(ap); b[1023] = 0;
    if (g_cli) { fwprintf(stderr, L"%ls\n", b); return; }
    SetWindowTextW(g_status, b);
}

static int sha256(const void *a, size_t an, const void *b, size_t bn, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE h; int ok = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return 0;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        ok = BCryptHashData(h, (PUCHAR)a, (ULONG)an, 0) == 0 && (!bn || BCryptHashData(h, (PUCHAR)b, (ULONG)bn, 0) == 0) &&
             BCryptFinishHash(h, out, 32, 0) == 0;
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}
static int sha256_file(const wchar_t *p, wchar_t hex[65]) {
    HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE h; int ok = 0; uint8_t d[32];
    static uint8_t buf[1 << 20]; DWORD got;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0) {
        if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
            ok = 1;
            while (ok && ReadFile(f, buf, sizeof buf, &got, NULL) && got) ok = BCryptHashData(h, buf, got, 0) == 0;
            ok = ok && BCryptFinishHash(h, d, 32, 0) == 0;
            BCryptDestroyHash(h);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    CloseHandle(f);
    if (ok) for (int i = 0; i < 32; i++) swprintf(hex + 2 * i, 3, L"%02X", d[i]);
    return ok;
}

// ---------- settings ----------
static void ini_get(const wchar_t *k, wchar_t *v, DWORD n) { GetPrivateProfileStringW(L"coop", k, L"", v, n, g_ini); }
static void ini_set(const wchar_t *k, const wchar_t *v) { WritePrivateProfileStringW(L"coop", k, v, g_ini); }

// ---------- Steam / game folder ----------
static void find_steam_game(void) {
    wchar_t steam[MAX_PATH] = L""; DWORD n = sizeof steam;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, NULL, steam, &n) != ERROR_SUCCESS) {
        n = sizeof steam;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", RRF_RT_REG_SZ, NULL, steam, &n) != ERROR_SUCCESS) return;
    }
    for (wchar_t *c = steam; *c; c++) if (*c == L'/') *c = L'\\';
    wchar_t libs[16][MAX_PATH]; int nl = 0;
    wcscpy(libs[nl++], steam);
    wchar_t vdf[MAX_PATH]; path_join(vdf, steam, L"steamapps\\libraryfolders.vdf");
    FILE *f = _wfopen(vdf, L"rb");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof line, f) && nl < 16) {
            char *p = strstr(line, "\"path\"");
            if (!p) continue;
            p = strchr(p + 6, '"'); if (!p) continue; p++;
            char out[MAX_PATH]; int k = 0;
            for (; *p && *p != '"' && k < MAX_PATH - 1; p++) { if (*p == '\\' && p[1] == '\\') p++; out[k++] = *p; }
            out[k] = 0;
            MultiByteToWideChar(CP_UTF8, 0, out, -1, libs[nl++], MAX_PATH);
        }
        fclose(f);
    }
    for (int i = 0; i < nl; i++) {
        wchar_t g[MAX_PATH], exe[MAX_PATH];
        path_join(g, libs[i], L"steamapps\\common\\Foundation"); path_join(exe, g, L"foundation.exe");
        if (file_exists(exe)) { GetFullPathNameW(g, MAX_PATH, g_steam_game, NULL); return; }
    }
}
static int is_steam_install(const wchar_t *dir) {
    wchar_t full[MAX_PATH]; GetFullPathNameW(dir, MAX_PATH, full, NULL);
    size_t n = wcslen(full); while (n && full[n - 1] == L'\\') full[--n] = 0;
    if (g_steam_game[0] && !_wcsicmp(full, g_steam_game)) return 1;
    wchar_t low[MAX_PATH]; wcscpy(low, full); _wcslwr(low);
    return wcsstr(low, L"\\steamapps\\common\\") != NULL;
}

// keeps the loader in the game copy up to date; checks the game version
static void check_game(void) {
    g_game_ok = 0;
    wchar_t exe[MAX_PATH]; path_join(exe, g_game, L"foundation.exe");
    if (!g_game[0] || !file_exists(exe)) {
        set_text(g_game_lbl, L"Game copy: not set up yet. Click \"Set up game copy\" (copies the game, about 4 GB).");
        return;
    }
    wchar_t hex[65] = L"", cached[80] = L"", key[80];
    WIN32_FILE_ATTRIBUTE_DATA fa; GetFileAttributesExW(exe, GetFileExInfoStandard, &fa);
    swprintf(key, 80, L"%lu-%lu-%lu", fa.nFileSizeLow, fa.ftLastWriteTime.dwLowDateTime, fa.ftLastWriteTime.dwHighDateTime);
    ini_get(L"ExeStamp", cached, 80);
    if (!wcscmp(cached, key)) ini_get(L"ExeHash", hex, 65);
    else if (sha256_file(exe, hex)) { ini_set(L"ExeStamp", key); ini_set(L"ExeHash", hex); }
    wchar_t appid[MAX_PATH]; path_join(appid, g_game, L"steam_appid.txt");
    if (!file_exists(appid)) { FILE *f = _wfopen(appid, L"wb"); if (f) { fputs("690830", f); fclose(f); } }
    wchar_t src[MAX_PATH], dst[MAX_PATH]; path_join(src, g_kit, L"dwmapi.dll"); path_join(dst, g_game, L"dwmapi.dll");
    CopyFileW(src, dst, FALSE);
    g_game_ok = _wcsicmp(hex, GAME_SHA256) == 0 ? 1 : 2;
    if (g_game_ok == 1) set_text(g_game_lbl, L"Game copy: %ls  (version OK)", g_game);
    else set_text(g_game_lbl, L"Game copy: %ls\nWRONG GAME VERSION: this kit needs Foundation 1.12.0.18. Update Steam, then set up the copy again.", g_game);
}

struct copy_job { wchar_t src[MAX_PATH], dst[MAX_PATH]; ULONGLONG total, done; };
static ULONGLONG dir_size(const wchar_t *d) {
    wchar_t pat[MAX_PATH]; path_join(pat, d, L"*"); WIN32_FIND_DATAW fd; ULONGLONG s = 0;
    HANDLE h = FindFirstFileW(pat, &fd); if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wchar_t p[MAX_PATH]; path_join(p, d, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) s += dir_size(p);
        else s += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h); return s;
}
static struct copy_job *g_job;
static ULONGLONG g_file_base;
static DWORD CALLBACK copy_cb(LARGE_INTEGER tot, LARGE_INTEGER done, LARGE_INTEGER a, LARGE_INTEGER b, DWORD c, DWORD d, HANDLE e, HANDLE f, LPVOID u) {
    (void)tot; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)u;
    g_job->done = g_file_base + (ULONGLONG)done.QuadPart;
    static DWORD last; if (GetTickCount() - last > 300) { last = GetTickCount(); PostMessageW(g_wnd, WM_COPY_PROGRESS, 0, 0); }
    return PROGRESS_CONTINUE;
}
static int copy_tree(const wchar_t *s, const wchar_t *d) {
    CreateDirectoryW(d, NULL);
    wchar_t pat[MAX_PATH]; path_join(pat, s, L"*"); WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd); if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = 1;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wchar_t ps[MAX_PATH], pd[MAX_PATH]; path_join(ps, s, fd.cFileName); path_join(pd, d, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { ok = copy_tree(ps, pd); continue; }
        ULONGLONG sz = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        WIN32_FILE_ATTRIBUTE_DATA da;
        if (GetFileAttributesExW(pd, GetFileExInfoStandard, &da) && da.nFileSizeHigh == fd.nFileSizeHigh && da.nFileSizeLow == fd.nFileSizeLow &&
            CompareFileTime(&da.ftLastWriteTime, &fd.ftLastWriteTime) == 0) { g_job->done += sz; continue; }   // already copied
        g_file_base = g_job->done;
        if (!CopyFileExW(ps, pd, copy_cb, NULL, NULL, 0)) ok = 0;
        g_job->done = g_file_base + sz;
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h); return ok;
}
static DWORD WINAPI copy_thread(void *arg) {
    struct copy_job *j = arg; g_job = j;
    j->total = dir_size(j->src);
    int ok = copy_tree(j->src, j->dst);
    PostMessageW(g_wnd, WM_COPY_DONE, ok, 0);
    return 0;
}
static void setup_game_copy(void) {
    if (g_copying) return;
    if (!g_steam_game[0]) {
        MessageBoxW(g_wnd, L"Foundation was not found in your Steam libraries.\nInstall it with Steam, or use \"Choose folder...\" to pick an existing copy of the game folder.",
                    L"Foundation Co-op", MB_ICONINFORMATION);
        return;
    }
    static struct copy_job j; memset(&j, 0, sizeof j);
    wcscpy(j.src, g_steam_game); path_join(j.dst, g_kit, L"game");
    wchar_t msg[1024];
    swprintf(msg, 1024, L"Copy the game from\n%ls\nto\n%ls ?\n\nThis needs about 4 GB and a few minutes. Your Steam installation is not changed.", j.src, j.dst);
    if (MessageBoxW(g_wnd, msg, L"Foundation Co-op", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    InterlockedExchange(&g_copying, 1);
    EnableWindow(g_btn_setup, FALSE);
    status(L"Copying the game... 0%%");
    CloseHandle(CreateThread(NULL, 0, copy_thread, &j, 0, NULL));
}
static void choose_game_folder(void) {
    BROWSEINFOW bi = {0}; bi.hwndOwner = g_wnd; bi.lpszTitle = L"Choose your COPY of the Foundation game folder (the folder with foundation.exe)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST id = SHBrowseForFolderW(&bi); if (!id) return;
    wchar_t p[MAX_PATH]; SHGetPathFromIDListW(id, p); CoTaskMemFree(id);
    wchar_t exe[MAX_PATH]; path_join(exe, p, L"foundation.exe");
    if (!file_exists(exe)) { MessageBoxW(g_wnd, L"There is no foundation.exe in that folder.", L"Foundation Co-op", MB_ICONWARNING); return; }
    if (is_steam_install(p)) { MessageBoxW(g_wnd, L"That is the Steam installation. Please use a copy (\"Set up game copy\" makes one).", L"Foundation Co-op", MB_ICONWARNING); return; }
    wcscpy(g_game, p); ini_set(L"GameDir", g_game); check_game();
}

// ---------- addresses ----------
static int addr_rank(const char *ip) {
    unsigned a, b; if (sscanf(ip, "%u.%u", &a, &b) != 2) return 9;
    if (a == 100 && b >= 64 && b < 128) return 0;          // Tailscale / CGNAT VPN
    if (a == 192 && b == 168) return 1;
    if (a == 10 || (a == 172 && b >= 16 && b < 32)) return 2;
    if (a == 127 || a == 169) return 8;
    return 3;
}
static void fill_addresses(void) {
    SendMessageW(g_addr, CB_RESETCONTENT, 0, 0);
    ULONG sz = 32768; IP_ADAPTER_ADDRESSES *aa = malloc(sz);
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, NULL, aa, &sz) != NO_ERROR) { free(aa); return; }
    struct { char ip[32]; wchar_t label[160]; int rank; } items[32]; int n = 0;
    for (IP_ADAPTER_ADDRESSES *a = aa; a && n < 32; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && n < 32; u = u->Next) {
            char ip[32]; inet_ntop(AF_INET, &((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr, ip, sizeof ip);
            int r = addr_rank(ip); if (r == 8) continue;
            strcpy(items[n].ip, ip); items[n].rank = r;
            const wchar_t *kind = r == 0 ? L"VPN, e.g. Tailscale - works over the internet" : r <= 2 ? L"home network" : L"other";
            swprintf(items[n].label, 160, L"%hs   (%ls: %ls)", ip, kind, a->FriendlyName);
            n++;
        }
    }
    free(aa);
    SendMessageW(g_addr, CB_ADDSTRING, 0, (LPARAM)L"Steam   (recommended: works over the internet, no VPN, no port setup)");
    for (int r = 0; r <= 3; r++) for (int i = 0; i < n; i++) if (items[i].rank == r) SendMessageW(g_addr, CB_ADDSTRING, 0, (LPARAM)items[i].label);
    SendMessageW(g_addr, CB_SETCURSEL, 0, 0);
}

// ---------- join code ----------
static void code_encode(const uint8_t *in, int n, char *out) {
    int bits = 0, k = 0, groups = 0; uint32_t acc = 0; char raw[64];
    for (int i = 0; i < n; i++) { acc = (acc << 8) | in[i]; bits += 8; while (bits >= 5) { raw[k++] = B32[(acc >> (bits - 5)) & 31]; bits -= 5; } }
    if (bits) raw[k++] = B32[(acc << (5 - bits)) & 31];
    int o = 0; out[o++] = 'F'; out[o++] = 'C'; out[o++] = '-';
    for (int i = 0; i < k; i++) { if (i && i % 5 == 0) out[o++] = '-'; out[o++] = raw[i]; groups++; }
    out[o] = 0;
}
static int code_decode(const wchar_t *s, uint8_t *out, int n) {
    if ((s[0] == L'F' || s[0] == L'f') && (s[1] == L'C' || s[1] == L'c')) s += 2;
    int bits = 0, k = 0; uint32_t acc = 0;
    for (; *s; s++) {
        wchar_t c = towupper(*s);
        if (c == L'-' || c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') continue;
        if (c == L'O') c = L'0'; if (c == L'I' || c == L'L') c = L'1';
        const char *p = c < 128 ? strchr(B32, (char)c) : NULL;
        if (!p || !c) return 0;
        acc = (acc << 5) | (uint32_t)(p - B32); bits += 5;
        if (bits >= 8) { if (k < n) out[k] = (uint8_t)(acc >> (bits - 8)); k++; bits -= 8; }
    }
    return k == n;
}
static uint8_t check_byte(const uint8_t *p, int n) { uint8_t c = 0x5a; for (int i = 0; i < n; i++) c = (uint8_t)((c << 1 | c >> 7) ^ p[i]); return c; }

static int write_key(const wchar_t *inst, const uint8_t secret[16]) {
    uint8_t k[32]; const char *salt = "foundation-coop key v1";
    if (!sha256(salt, strlen(salt), secret, 16, k)) return 0;
    wchar_t p[MAX_PATH]; path_join(p, inst, L"coop_key.txt");
    FILE *f = _wfopen(p, L"wb"); if (!f) return 0;
    for (int i = 0; i < 32; i++) fprintf(f, "%02x", k[i]);
    fclose(f); return 1;
}

// ---------- start the game ----------
static int game_running(void) { return g_proc && WaitForSingleObject(g_proc, 0) == WAIT_TIMEOUT; }
static int steam_running(void) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); PROCESSENTRY32W pe = { sizeof pe }; int found = 0;
    if (s == INVALID_HANDLE_VALUE) return 1;
    for (BOOL ok = Process32FirstW(s, &pe); ok; ok = Process32NextW(s, &pe)) if (!_wcsicmp(pe.szExeFile, L"steam.exe")) { found = 1; break; }
    CloseHandle(s); return found;
}
// SteamID64 of the account logged in to the running Steam client (0 if none)
static uint64_t steam_user(void) {
    DWORD v = 0, n = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser", RRF_RT_REG_DWORD, NULL, &v, &n) != ERROR_SUCCESS || !v) return 0;
    return 76561197960265728ull + v;
}
static uint64_t g_steam_host;          // client in Steam mode: the host's SteamID; host in Steam mode: 1
static int start_game(int host, const char *ip, int port) {
    wchar_t exe[MAX_PATH], coop[MAX_PATH], ws[64], st[MAX_PATH];
    path_join(exe, g_game, L"foundation.exe"); path_join(coop, g_kit, L"coop.dll");
    path_join(st, g_inst, L"status.txt"); DeleteFileW(st);
    SetEnvironmentVariableW(L"FCOOP_PROXY", L"1");
    SetEnvironmentVariableW(L"FCOOP_LOBBY", L"1");
    SetEnvironmentVariableW(L"FCOOP_DEV", NULL);
    SetEnvironmentVariableW(L"FCOOP_INSTANCE_DIR", g_inst);
    SetEnvironmentVariableW(L"FCOOP_COOP_DLL", coop);
    SetEnvironmentVariableW(L"FCOOP_ROLE", host ? L"host" : L"client");
    swprintf(ws, 64, L"%d", port); SetEnvironmentVariableW(L"FCOOP_LS_PORT", ws);
    SetEnvironmentVariableW(L"FCOOP_NET", g_steam_host ? L"steam" : NULL);
    SetEnvironmentVariableW(L"FCOOP_STEAM_HOST", NULL);
    if (g_steam_host && !host) { swprintf(ws, 64, L"%llu", (unsigned long long)g_steam_host); SetEnvironmentVariableW(L"FCOOP_STEAM_HOST", ws); }
    if (host) {
        if (!GetEnvironmentVariableW(L"FCOOP_LS_BIND", ws, 64)) SetEnvironmentVariableW(L"FCOOP_LS_BIND", L"0.0.0.0");   // tests: 127.0.0.1
        SetEnvironmentVariableW(L"FCOOP_LS_ALLOW", L"*");
        SetEnvironmentVariableW(L"FCOOP_SAVE", g_save);
    } else {
        swprintf(ws, 64, L"%hs", ip); SetEnvironmentVariableW(L"FCOOP_LS_HOST", ws);
    }
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    wchar_t cmd[MAX_PATH + 4]; swprintf(cmd, MAX_PATH + 4, L"\"%ls\"", exe);
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, g_game, &si, &pi)) { status(L"Could not start the game (error %lu).", GetLastError()); return 0; }
    CloseHandle(pi.hThread);
    if (g_proc) CloseHandle(g_proc);
    g_proc = pi.hProcess;
    return 1;
}

static int precheck(void) {
    if (game_running()) { status(L"The co-op game is already running. Close it first to start a new session."); return 0; }
    if (g_game_ok != 1) { status(g_game_ok == 2 ? L"Wrong game version (see above)." : L"Set up the game copy first."); return 0; }
    if (!steam_running()) { status(L"Steam is not running. Start Steam first, then try again."); return 0; }
    return 1;
}

// join codes: v1 (24 bytes) {1, IPv4, port BE, secret16, check}; v2 (26 bytes) {2, SteamID64 LE, secret16, check}
static int do_host_sel(const wchar_t *sel) {
    if (!precheck()) return 0;
    if (!g_save[0]) { status(L"Choose a city save first (\"Choose city save...\")."); return 0; }
    char ip[64] = "";
    WideCharToMultiByte(CP_UTF8, 0, sel, -1, ip, sizeof ip, NULL, NULL);
    char *sp = strchr(ip, ' '); if (sp) *sp = 0;
    struct in_addr ia; uint64_t me = 0;
    if (!_stricmp(ip, "steam")) {
        me = steam_user();
        if (!me) { status(L"No Steam account found. Start Steam and log in, then try again."); return 0; }
    } else if (inet_pton(AF_INET, ip, &ia) != 1) { status(L"\"%ls\" is not an IPv4 address.", sel); return 0; }
    path_join(g_inst, g_kit, L"instance-host"); mkdirs(g_inst);
    // copy the chosen save into the co-op profile (the original file is only read)
    wchar_t dir[MAX_PATH], dst[MAX_PATH];
    path_join(dir, g_inst, L"Documents\\Polymorph Games\\Foundation\\Save Game\\COOP"); mkdirs(dir);
    path_join(dst, dir, L"coop-session.foundation");
    if (!CopyFileW(g_save, dst, FALSE)) { status(L"Could not copy the save (error %lu).", GetLastError()); return 0; }
    { HANDLE f = CreateFileW(dst, FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING, 0, NULL);   // newest save in the list
      if (f != INVALID_HANDLE_VALUE) { FILETIME now; GetSystemTimeAsFileTime(&now); SetFileTime(f, NULL, NULL, &now); CloseHandle(f); } }
    uint8_t c[26] = {0}, secret[16]; int clen;
    BCryptGenRandom(NULL, secret, 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (me) { c[0] = 2; memcpy(c + 1, &me, 8); memcpy(c + 9, secret, 16); clen = 26; }
    else { c[0] = 1; memcpy(c + 1, &ia, 4); c[5] = DEFAULT_PORT >> 8; c[6] = DEFAULT_PORT & 255; memcpy(c + 7, secret, 16); clen = 24; }
    c[clen - 1] = check_byte(c, clen - 1);
    if (!write_key(g_inst, secret)) { status(L"Could not write the session key."); return 0; }
    char code[96]; code_encode(c, clen, code);
    wcscpy(g_save, dst);
    g_steam_host = me ? 1 : 0;
    if (!start_game(1, ip, DEFAULT_PORT)) return 0;
    wchar_t wc[96]; MultiByteToWideChar(CP_UTF8, 0, code, -1, wc, 96);
    if (g_cli) { wchar_t p[MAX_PATH]; path_join(p, g_kit, L"last_code.txt"); FILE *f = _wfopen(p, L"wb"); if (f) { fputs(code, f); fclose(f); } return 1; }
    SetWindowTextW(g_code_out, wc);
    if (me) status(L"Game starting. Send the join code to your friend PRIVATELY (anyone with it can join).\nThe connection goes through Steam: nothing else to set up.");
    else status(L"Game starting. Send the join code to your friend PRIVATELY (anyone with it can join).\nWindows may ask whether foundation.exe may use the network: allow it.");
    return 1;
}
static void do_host(void) { wchar_t sel[256]; GetWindowTextW(g_addr, sel, 256); do_host_sel(sel); }

static void do_join(const wchar_t *codew) {
    if (!precheck()) return;
    uint8_t c[26];
    path_join(g_inst, g_kit, L"instance-client");
    if (code_decode(codew, c, 26) && c[0] == 2 && check_byte(c, 25) == c[25]) {
        memcpy(&g_steam_host, c + 1, 8);
        if (g_steam_host == steam_user()) status(L"Note: this code is from your own Steam account. Steam cannot connect an account to itself; the friend must join from their account.");
        mkdirs(g_inst);
        if (!write_key(g_inst, c + 9)) { status(L"Could not write the session key."); return; }
        if (!start_game(0, "", DEFAULT_PORT)) return;
        status(L"Game starting, connecting to the host through Steam ...");
        return;
    }
    if (!code_decode(codew, c, 24) || c[0] != 1 || check_byte(c, 23) != c[23]) { status(L"That join code is not valid. Copy the whole code again (it starts with FC-)."); return; }
    g_steam_host = 0;
    char ip[32]; inet_ntop(AF_INET, c + 1, ip, sizeof ip);
    int port = (c[5] << 8) | c[6];
    mkdirs(g_inst);
    if (!write_key(g_inst, c + 7)) { status(L"Could not write the session key."); return; }
    if (!start_game(0, ip, port)) return;
    status(L"Game starting, connecting to %hs ...", ip);
}

static void choose_save(void) {
    wchar_t init[MAX_PATH] = L"", file[MAX_PATH] = L"";
    PWSTR docs = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Documents, 0, NULL, &docs))) { path_join(init, docs, L"Polymorph Games\\Foundation\\Save Game"); CoTaskMemFree(docs); }
    OPENFILENAMEW of = { sizeof of }; of.hwndOwner = g_wnd;
    of.lpstrFilter = L"Foundation saves (*.foundation)\0*.foundation\0"; of.lpstrFile = file; of.nMaxFile = MAX_PATH;
    of.lpstrInitialDir = init; of.lpstrTitle = L"Choose the city to play together (the file is only copied, never changed)";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&of)) return;
    wcscpy(g_save, file);
    const wchar_t *bn = wcsrchr(file, L'\\'); const wchar_t *city = L"";
    wchar_t parent[MAX_PATH]; wcscpy(parent, file);
    wchar_t *s1 = wcsrchr(parent, L'\\'); if (s1) { *s1 = 0; wchar_t *s2 = wcsrchr(parent, L'\\'); city = s2 ? s2 + 1 : parent; }
    set_text(g_save_lbl, L"%ls  /  %ls", city, bn ? bn + 1 : file);
}

// ---------- status polling ----------
static void poll_status(void) {
    if (!g_inst[0]) return;
    wchar_t p[MAX_PATH]; path_join(p, g_inst, L"status.txt");
    char b[600] = ""; FILE *f = _wfopen(p, L"rb");
    if (f) { size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f); }
    if (g_proc && !game_running()) {
        CloseHandle(g_proc); g_proc = NULL;
        status(L"The co-op game was closed. Logs: %ls\\coop.log", g_inst);
        return;
    }
    if (!b[0] || !g_proc) return;
    char *text = strchr(b, '|'); text = text ? text + 1 : b;
    wchar_t w[600]; MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 600);
    wchar_t cur[600]; GetWindowTextW(g_status, cur, 600);
    if (wcscmp(cur, w)) SetWindowTextW(g_status, w);
}

// ---------- window ----------
static HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(!wcscmp(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_wnd, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SETUP: setup_game_copy(); break;
        case ID_CHOOSE_GAME: choose_game_folder(); break;
        case ID_CHOOSE_SAVE: choose_save(); break;
        case ID_HOST: do_host(); break;
        case ID_JOIN: { wchar_t c[200]; GetWindowTextW(g_code_in, c, 200); do_join(c); break; }
        case ID_COPY: {
            wchar_t c[100]; int n = GetWindowTextW(g_code_out, c, 100);
            if (n && OpenClipboard(h)) {
                EmptyClipboard(); HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (n + 1) * sizeof(wchar_t));
                memcpy(GlobalLock(g), c, (n + 1) * sizeof(wchar_t)); GlobalUnlock(g);
                SetClipboardData(CF_UNICODETEXT, g); CloseClipboard(); status(L"Join code copied. Send it to your friend privately.");
            }
            break;
        }
        case ID_OPEN: ShellExecuteW(h, L"open", g_inst[0] ? g_inst : g_kit, NULL, NULL, SW_SHOWNORMAL); break;
        case ID_HELP: { wchar_t p[MAX_PATH]; path_join(p, g_kit, L"README.txt"); ShellExecuteW(h, L"open", p, NULL, NULL, SW_SHOWNORMAL); break; }
        }
        return 0;
    case WM_TIMER: poll_status(); return 0;
    case WM_COPY_PROGRESS:
        if (g_job && g_job->total) status(L"Copying the game... %d%%", (int)(g_job->done * 100 / g_job->total));
        return 0;
    case WM_COPY_DONE:
        InterlockedExchange(&g_copying, 0); EnableWindow(g_btn_setup, TRUE);
        if (wp) { wcscpy(g_game, g_job->dst); ini_set(L"GameDir", g_game); check_game(); status(L"Game copy ready."); }
        else status(L"Copying failed (disk full?). You can click \"Set up game copy\" again to continue.");
        return 0;
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_status) { SetTextColor((HDC)wp, RGB(20, 60, 120)); SetBkMode((HDC)wp, TRANSPARENT); return (LRESULT)GetSysColorBrush(COLOR_BTNFACE); }
        break;
    case WM_CLOSE:
        if (g_copying && MessageBoxW(h, L"The game is still being copied. Stop and close?", L"Foundation Co-op", MB_YESNO) != IDYES) return 0;
        DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void build_ui(void) {
    int y = 12;
    HWND t = mk(L"STATIC", L"Foundation Co-op (prototype): one city, two PCs", 0, 16, y, 560, 22, 0); SendMessageW(t, WM_SETFONT, (WPARAM)g_bold, TRUE); y += 32;
    mk(L"BUTTON", L"1. Game", BS_GROUPBOX, 10, y, 580, 92, 0);
    g_game_lbl = mk(L"STATIC", L"", 0, 22, y + 20, 556, 36, 0);
    g_btn_setup = mk(L"BUTTON", L"Set up game copy", BS_PUSHBUTTON, 22, y + 58, 150, 26, ID_SETUP);
    mk(L"BUTTON", L"Choose folder...", BS_PUSHBUTTON, 180, y + 58, 130, 26, ID_CHOOSE_GAME);
    y += 102;
    mk(L"BUTTON", L"2a. Host a game (you share your city)", BS_GROUPBOX, 10, y, 580, 160, 0);
    mk(L"BUTTON", L"Choose city save...", BS_PUSHBUTTON, 22, y + 22, 150, 26, ID_CHOOSE_SAVE);
    g_save_lbl = mk(L"STATIC", L"(no save chosen)", SS_PATHELLIPSIS, 180, y + 27, 400, 20, 0);
    mk(L"STATIC", L"Your address:", 0, 22, y + 60, 100, 20, 0);
    g_addr = mk(L"COMBOBOX", L"", CBS_DROPDOWN | WS_VSCROLL, 120, y + 56, 460, 200, ID_ADDR);
    g_btn_host = mk(L"BUTTON", L"Start hosting", BS_DEFPUSHBUTTON, 22, y + 90, 150, 28, ID_HOST);
    mk(L"STATIC", L"Join code:", 0, 22, y + 128, 90, 20, 0);
    g_code_out = mk(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL, 120, y + 125, 360, 24, ID_CODE_OUT);
    mk(L"BUTTON", L"Copy", BS_PUSHBUTTON, 488, y + 124, 92, 26, ID_COPY);
    y += 170;
    mk(L"BUTTON", L"2b. Join a game (your friend hosts)", BS_GROUPBOX, 10, y, 580, 62, 0);
    mk(L"STATIC", L"Join code:", 0, 22, y + 27, 90, 20, 0);
    g_code_in = mk(L"EDIT", L"", ES_AUTOHSCROLL, 120, y + 24, 360, 24, ID_CODE_IN);
    g_btn_join = mk(L"BUTTON", L"Join", BS_PUSHBUTTON, 488, y + 23, 92, 26, ID_JOIN);
    y += 72;
    mk(L"BUTTON", L"Status", BS_GROUPBOX, 10, y, 580, 78, 0);
    g_status = mk(L"STATIC", L"Ready.", 0, 22, y + 20, 556, 52, 0);
    y += 88;
    mk(L"BUTTON", L"Open co-op folder (logs, saves)", BS_PUSHBUTTON, 10, y, 220, 26, ID_OPEN);
    mk(L"BUTTON", L"Help", BS_PUSHBUTTON, 240, y, 80, 26, ID_HELP);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev; (void)cmdline;
    WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    GetModuleFileNameW(NULL, g_kit, MAX_PATH); *wcsrchr(g_kit, L'\\') = 0;
    path_join(g_ini, g_kit, L"coop_settings.ini");
    find_steam_game();
    ini_get(L"GameDir", g_game, MAX_PATH);
    if (!g_game[0]) { wchar_t d[MAX_PATH], e[MAX_PATH]; path_join(d, g_kit, L"game"); path_join(e, d, L"foundation.exe"); if (file_exists(e)) wcscpy(g_game, d); }

    int argc; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *a_host = NULL, *a_addr = NULL, *a_join = NULL;
    for (int i = 1; i + 1 < argc; i++) {
        if (!wcscmp(argv[i], L"--host")) a_host = argv[++i];
        else if (!wcscmp(argv[i], L"--addr")) a_addr = argv[++i];
        else if (!wcscmp(argv[i], L"--join")) a_join = argv[++i];
        else if (!wcscmp(argv[i], L"--game")) wcsncpy(g_game, argv[++i], MAX_PATH - 1);
    }
    if (a_host || a_join) {   // command-line mode (tests): no window
        g_cli = 1;
        g_game_lbl = NULL;
        check_game();
        if (g_game_ok != 1) { fwprintf(stderr, L"game copy missing or wrong version\n"); return 2; }
        if (a_host) {
            wcsncpy(g_save, a_host, MAX_PATH - 1);
            g_addr = NULL;
            return do_host_sel(a_addr ? a_addr : L"127.0.0.1") ? 0 : 5;   // --addr <ip> or --addr steam
        }
        do_join(a_join);
        return g_proc ? 0 : 6;
    }

    NONCLIENTMETRICSW ncm = { sizeof ncm }; SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont; lf.lfWeight = FW_BOLD; lf.lfHeight = lf.lfHeight * 13 / 10; g_bold = CreateFontIndirectW(&lf);
    WNDCLASSW wc = {0}; wc.lpfnWndProc = wndproc; wc.hInstance = hi; wc.lpszClassName = L"FoundationCoop";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    RECT r = { 0, 0, 600, 600 }; AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_wnd = CreateWindowW(L"FoundationCoop", L"Foundation Co-op", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                          CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, hi, NULL);
    build_ui();
    fill_addresses();
    check_game();
    SetTimer(g_wnd, ID_TIMER, 500, NULL);
    ShowWindow(g_wnd, show); UpdateWindow(g_wnd);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) { if (!IsDialogMessageW(g_wnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
    return 0;
}
