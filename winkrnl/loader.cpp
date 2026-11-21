// winkrnl/loader.cpp — user-mode client for the KrnlExec driver.
//
// Build: cl /std:c++17 /EHsc loader.cpp /link
//
// Usage:
//   krnlexec attach <pid>
//   krnlexec status
//   krnlexec exec <path.lua>
//   krnlexec exec -s "print(1)"
//   krnlexec release

#include <windows.h>
#include <winioctl.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#define DEV_PATH L"\\\\.\\KrnlExec"

#define IOCTL_INIT    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_EXECUTE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_STATUS  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RELEASE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 1)
struct InitRequest { ULONG Pid; };
struct ExecRequest { ULONG Pid; ULONG Size; CHAR Script[1]; };
struct StatusReply {
    ULONGLONG LoadString;
    ULONGLONG ScriptContext;
    ULONGLONG DataModel;
    ULONG     Pid;
    ULONG     Ready;
};
#pragma pack(pop)

static HANDLE open_dev() {
    HANDLE h = CreateFileW(DEV_PATH, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        std::fprintf(stderr, "open \\\\.\\KrnlExec failed (%lu). Is the driver loaded?\n",
                     GetLastError());
    return h;
}

static DWORD find_roblox_pid() {
    HWND hwnd = FindWindowW(L"ROBLOX", nullptr);
    if (!hwnd) hwnd = FindWindowW(nullptr, L"Roblox");
    if (!hwnd) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}

static int cmd_attach(int argc, wchar_t** argv) {
    DWORD pid = 0;
    if (argc >= 1) pid = _wtol(argv[0]);
    if (!pid) pid = find_roblox_pid();
    if (!pid) { std::fprintf(stderr, "no pid provided and no Roblox window found\n"); return 2; }

    HANDLE h = open_dev();
    if (h == INVALID_HANDLE_VALUE) return 2;

    InitRequest r{ pid };
    DWORD out = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_INIT, &r, sizeof(r), nullptr, 0, &out, nullptr);
    CloseHandle(h);
    if (!ok) { std::fprintf(stderr, "IOCTL_INIT failed: %lu\n", GetLastError()); return 3; }
    std::printf("attached pid %lu\n", pid);
    return 0;
}

static int cmd_status() {
    HANDLE h = open_dev();
    if (h == INVALID_HANDLE_VALUE) return 2;
    StatusReply s{};
    DWORD out = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STATUS, nullptr, 0, &s, sizeof(s), &out, nullptr);
    CloseHandle(h);
    if (!ok) { std::fprintf(stderr, "IOCTL_STATUS failed: %lu\n", GetLastError()); return 3; }
    std::printf("pid:            %lu\n", s.Pid);
    std::printf("ready:          %s\n", s.Ready ? "yes" : "no");
    std::printf("LoadString:     0x%016llx\n", s.LoadString);
    std::printf("DataModel:      0x%016llx\n", s.DataModel);
    std::printf("ScriptContext:  0x%016llx\n", s.ScriptContext);
    return 0;
}

static int cmd_exec(int argc, wchar_t** argv) {
    if (argc < 1) { std::fprintf(stderr, "usage: krnlexec exec <path> | exec -s <code>\n"); return 1; }

    std::string src;
    if (!wcscmp(argv[0], L"-s")) {
        if (argc < 2) { std::fprintf(stderr, "-s requires code\n"); return 1; }
        int n = WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, nullptr, 0, nullptr, nullptr);
        src.resize(n ? n - 1 : 0);
        WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, src.data(), n, nullptr, nullptr);
    } else {
        std::ifstream f(argv[0]);
        if (!f) { std::fwprintf(stderr, L"cannot open %s\n", argv[0]); return 1; }
        std::stringstream ss; ss << f.rdbuf();
        src = ss.str();
    }
    if (src.empty()) { std::fprintf(stderr, "empty script\n"); return 1; }

    // Grab current attached pid.
    StatusReply s{};
    {
        HANDLE h = open_dev();
        if (h == INVALID_HANDLE_VALUE) return 2;
        DWORD out = 0;
        if (!DeviceIoControl(h, IOCTL_STATUS, nullptr, 0, &s, sizeof(s), &out, nullptr)) {
            std::fprintf(stderr, "status read failed: %lu\n", GetLastError());
            CloseHandle(h);
            return 3;
        }
        CloseHandle(h);
    }
    if (!s.Ready) { std::fprintf(stderr, "not attached. run: krnlexec attach <pid>\n"); return 4; }

    size_t total = offsetof(ExecRequest, Script) + src.size();
    std::vector<uint8_t> buf(total);
    auto* r = reinterpret_cast<ExecRequest*>(buf.data());
    r->Pid  = s.Pid;
    r->Size = (ULONG)src.size();
    std::memcpy(r->Script, src.data(), src.size());

    HANDLE h = open_dev();
    if (h == INVALID_HANDLE_VALUE) return 2;
    DWORD out = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_EXECUTE, buf.data(), (DWORD)buf.size(),
                              nullptr, 0, &out, nullptr);
    CloseHandle(h);
    if (!ok) { std::fprintf(stderr, "IOCTL_EXECUTE failed: %lu\n", GetLastError()); return 3; }
    std::printf("queued (%zu bytes)\n", src.size());
    return 0;
}

static int cmd_release() {
    HANDLE h = open_dev();
    if (h == INVALID_HANDLE_VALUE) return 2;
    DWORD out = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_RELEASE, nullptr, 0, nullptr, 0, &out, nullptr);
    CloseHandle(h);
    if (!ok) { std::fprintf(stderr, "IOCTL_RELEASE failed: %lu\n", GetLastError()); return 3; }
    std::puts("released");
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fwprintf(stderr,
            L"krnlexec commands:\n"
            L"  attach [pid]            attach to pid (or auto-detect Roblox)\n"
            L"  status                  show resolved state\n"
            L"  exec <path>             run a .lua file\n"
            L"  exec -s <code>          run a string\n"
            L"  release                 detach + drop refs\n");
        return 1;
    }
    std::wstring cmd = argv[1];
    if (cmd == L"attach")  return cmd_attach(argc - 2, argv + 2);
    if (cmd == L"status")  return cmd_status();
    if (cmd == L"exec")    return cmd_exec(argc - 2, argv + 2);
    if (cmd == L"release") return cmd_release();
    std::fwprintf(stderr, L"unknown command: %s\n", cmd.c_str());
    return 1;
}
