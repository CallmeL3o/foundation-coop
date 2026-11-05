// folderhook.dll: test-only. Loaded into foundation.exe by launch.exe.
// Patches the exe's import of SHELL32!SHGetKnownFolderPath so that
// FOLDERID_Documents and FOLDERID_LocalAppData resolve to private folders
// under %FCOOP_INSTANCE_DIR%. Every call is logged to <instance>\folderhook.log.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <wchar.h>

typedef HRESULT(WINAPI *PFN_SHGKFP)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR *);
static PFN_SHGKFP g_real;
static wchar_t g_dir[MAX_PATH];
static wchar_t g_log[MAX_PATH];

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
    HRESULT hr;
    const wchar_t *which = L"other (passed through)";
    if (IsEqualGUID(id, &FOLDERID_Documents)) { which = L"Documents"; hr = dup_path(L"Documents", out); }
    else if (IsEqualGUID(id, &FOLDERID_LocalAppData)) { which = L"LocalAppData"; hr = dup_path(L"LocalAppData", out); }
    else hr = g_real(id, flags, tok, out);
    logw(L"SHGetKnownFolderPath %ls -> 0x%08lx %ls", which, (unsigned long)hr, SUCCEEDED(hr) && *out ? *out : L"");
    return hr;
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

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)h; (void)r;
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    if (!GetEnvironmentVariableW(L"FCOOP_INSTANCE_DIR", g_dir, MAX_PATH)) return TRUE;
    _snwprintf(g_log, MAX_PATH - 1, L"%ls\\folderhook.log", g_dir);
    int n = patch_iat(GetModuleHandleW(NULL));
    logw(L"folderhook loaded, pid %lu, patched %d import(s), instance %ls", GetCurrentProcessId(), n, g_dir);
    return TRUE;
}
