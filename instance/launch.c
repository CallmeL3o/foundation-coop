// launch.exe: test-only launcher. Usage: launch.exe <game.exe> <instance dir> [game args...]
// Starts the game suspended with FCOOP_INSTANCE_DIR set, queues LoadLibraryW(folderhook.dll)
// as an APC on the main thread (runs after imports are bound, before the exe's entry point),
// then resumes it. Prints the pid.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv) {
    if (argc < 3) { fwprintf(stderr, L"usage: launch <game.exe> <instance dir> [args]\n"); return 2; }
    wchar_t exe[MAX_PATH], inst[MAX_PATH], dll[MAX_PATH], wd[MAX_PATH], *fp;
    GetFullPathNameW(argv[1], MAX_PATH, exe, NULL);
    GetFullPathNameW(argv[2], MAX_PATH, inst, NULL);
    CreateDirectoryW(inst, NULL);
    GetModuleFileNameW(NULL, dll, MAX_PATH);
    wcscpy(wcsrchr(dll, L'\\') + 1, L"folderhook.dll");
    GetFullPathNameW(exe, MAX_PATH, wd, &fp); if (fp) *fp = 0;

    wchar_t cmd[4096];
    _snwprintf(cmd, 4095, L"\"%ls\"", exe); cmd[4095] = 0;
    for (int i = 3; i < argc; i++) { wcsncat(cmd, L" ", 4095 - wcslen(cmd)); wcsncat(cmd, argv[i], 4095 - wcslen(cmd)); }

    SetEnvironmentVariableW(L"FCOOP_INSTANCE_DIR", inst);
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, wd, &si, &pi)) {
        fwprintf(stderr, L"CreateProcess failed: %lu\n", GetLastError()); return 1;
    }
    // queue LoadLibraryW for folderhook.dll, then coop.dll if it sits next to the launcher (APCs run in order)
    wchar_t coop[MAX_PATH]; wcscpy(coop, dll); wcscpy(wcsrchr(coop, L'\\') + 1, L"coop.dll");
    const wchar_t *dlls[2] = { dll, GetFileAttributesW(coop) != INVALID_FILE_ATTRIBUTES ? coop : NULL };
    for (int i = 0; i < 2; i++) {
        if (!dlls[i]) continue;
        SIZE_T bytes = (wcslen(dlls[i]) + 1) * sizeof(wchar_t);
        void *remote = VirtualAllocEx(pi.hProcess, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!remote || !WriteProcessMemory(pi.hProcess, remote, dlls[i], bytes, NULL) ||
            !QueueUserAPC((PAPCFUNC)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"), pi.hThread, (ULONG_PTR)remote)) {
            fwprintf(stderr, L"injection setup failed: %lu\n", GetLastError());
            TerminateProcess(pi.hProcess, 1); return 1;
        }
    }
    ResumeThread(pi.hThread);
    wprintf(L"%lu\n", pi.dwProcessId);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return 0;
}
