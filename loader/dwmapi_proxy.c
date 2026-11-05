// dwmapi.dll (proxy): placed in a COPY of the game folder. Windows loads it instead of System32\dwmapi.dll
// because the game imports dwmapi and the game folder is searched first. Every dwmapi function is forwarded to
// the real System32 dwmapi.dll (jump stubs in dwmapi_fwd.S, table filled here).
//
// Only when the co-op launcher started the game (FCOOP_PROXY=1 and FCOOP_INSTANCE_DIR set) it also:
//  1. redirects the game's Documents and LocalAppData folders to <instance>\Documents / LocalAppData
//     (patches the exe's import of SHELL32!SHGetKnownFolderPath), so co-op never touches the normal saves;
//  2. loads coop.dll from FCOOP_COOP_DLL.
// Started any other way, the game copy runs unmodified.
// This replaces the old launch.exe, which injected the DLLs into a suspended game process (a method antivirus
// programs distrust).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <wchar.h>
#include "dwmapi_tab.h"

void *fcoop_dwm_tab[FCOOP_DWM_N];
static HRESULT WINAPI missing_export(void) { return E_NOTIMPL; }

typedef HRESULT(WINAPI *PFN_SHGKFP)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR *);
static PFN_SHGKFP g_real;
static wchar_t g_dir[MAX_PATH], g_log[MAX_PATH];

static void logw(const wchar_t *fmt, ...) {
    HANDLE f = CreateFileW(g_log, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    wchar_t w[1024]; char a[2048];
    va_list ap; va_start(ap, fmt); _vsnwprintf(w, 1023, fmt, ap); va_end(ap); w[1023] = 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, a, sizeof a, NULL, NULL);
    DWORD wr; if (n > 1) WriteFile(f, a, n - 1, &wr, NULL);
    WriteFile(f, "\r\n", 2, &wr, NULL);
    CloseHandle(f);
}

static HRESULT dup_path(const wchar_t *sub, PWSTR *out) {
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH - 1, L"%ls\\%ls", g_dir, sub); p[MAX_PATH - 1] = 0;
    CreateDirectoryW(p, NULL);
    size_t bytes = (wcslen(p) + 1) * sizeof(wchar_t);
    *out = (PWSTR)CoTaskMemAlloc(bytes);
    if (!*out) return E_OUTOFMEMORY;
    memcpy(*out, p, bytes);
    return S_OK;
}

static HRESULT WINAPI hook_SHGetKnownFolderPath(REFKNOWNFOLDERID id, DWORD flags, HANDLE tok, PWSTR *out) {
    if (IsEqualGUID(id, &FOLDERID_Documents)) return dup_path(L"Documents", out);
    if (IsEqualGUID(id, &FOLDERID_LocalAppData)) return dup_path(L"LocalAppData", out);
    return g_real(id, flags, tok, out);
}

static int patch_iat(HMODULE mod) {
    BYTE *base = (BYTE *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int patched = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((char *)(base + d->Name), "SHELL32.dll") != 0) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((char *)ibn->Name, "SHGetKnownFolderPath") != 0) continue;
            DWORD old;
            VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
            g_real = (PFN_SHGKFP)slots->u1.Function;
            slots->u1.Function = (ULONG_PTR)hook_SHGetKnownFolderPath;
            VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
            patched++;
        }
    }
    return patched;
}

static void resolve_forwards(void) {
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH - 16);
    wcscpy(sys + n, L"\\dwmapi.dll");
    HMODULE real = LoadLibraryW(sys);
    for (int i = 0; i < FCOOP_DWM_N; i++) {
        FARPROC p = NULL;
        if (real) p = GetProcAddress(real, fcoop_dwm_exports[i].name ? fcoop_dwm_exports[i].name
                                                                     : (LPCSTR)(ULONG_PTR)fcoop_dwm_exports[i].ord);
        fcoop_dwm_tab[i] = p ? (void *)p : (void *)missing_export;
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)r;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    resolve_forwards();
    wchar_t on[8] = L"";
    if (!GetEnvironmentVariableW(L"FCOOP_PROXY", on, 8) || wcscmp(on, L"1") != 0) return TRUE;
    if (!GetEnvironmentVariableW(L"FCOOP_INSTANCE_DIR", g_dir, MAX_PATH)) return TRUE;
    CreateDirectoryW(g_dir, NULL);
    _snwprintf(g_log, MAX_PATH - 1, L"%ls\\loader.log", g_dir); g_log[MAX_PATH - 1] = 0;
    DeleteFileW(g_log);
    int n = patch_iat(GetModuleHandleW(NULL));
    logw(L"loader: pid %lu, folder redirect %ls, instance %ls", GetCurrentProcessId(), n ? L"on" : L"FAILED", g_dir);
    wchar_t coop[MAX_PATH];
    if (GetEnvironmentVariableW(L"FCOOP_COOP_DLL", coop, MAX_PATH)) {
        HMODULE m = LoadLibraryW(coop);
        logw(L"loader: coop.dll %ls (%lu)", m ? L"loaded" : L"NOT loaded", m ? 0ul : GetLastError());
    }
    return TRUE;
}
