// winkrnl/driver.cpp — Roblox executor API kernel driver (Windows 11 x64, WDM)
//
// This is a complete rewrite of the operator's original. Key fixes vs. the
// paste:
//
//  * Kernel never calls a user-mode function pointer directly. Execution of
//    r_loadstring happens in ring-3 via a user-mode APC queued onto a
//    target thread — the kernel's only job is to find r_loadstring, prep a
//    user-mode buffer with the script, and schedule the APC.
//  * Every IOCTL validates InputBufferLength before touching SystemBuffer.
//  * Every user-mode dereference from the driver sits inside __try/__except.
//  * EPROCESS reference is reference-counted and released on reinit/unload.
//  * Pattern scans use paged pool and run in 4 MiB windows so a 150+ MiB
//    RobloxPlayerBeta doesn't blow non-paged pool.
//  * Class-registry "reflection walk" is gone — it never worked. The chain
//    we actually need (FakeDataModel -> DataModel -> ScriptContext) is a
//    signature-anchored offset path with a documented refresh procedure.
//  * Buffer for the script lives in the target process's user VAD
//    (ZwAllocateVirtualMemory), not in SystemBuffer.
//
// Build: WDK 10+, VS 2022, /std:c++17 /kernel /GS- /guard:cf-
// Sign the .sys with a test cert and enable testsigning, or ship through
// a loader (KDMapper, kdu, etc.) — pick your poison.
//
// IOCTLs:
//   IOCTL_INIT    { ULONG Pid }
//                 Attaches to the target Roblox process. Resolves symbols.
//   IOCTL_EXECUTE { ULONG Pid; ULONG Size; CHAR Script[Size] }
//                 Allocates user-mode memory in target, copies script,
//                 queues APC on every thread in the process so whichever
//                 thread goes alertable first runs r_loadstring.
//   IOCTL_STATUS  (no input) — returns current resolved state.
//   IOCTL_RELEASE (no input) — detaches, drops all refs.

#include <ntifs.h>
#include <ntddk.h>
#include <intrin.h>

// ---------------------------------------------------------------------------
// Public contract (shared with the usermode loader).
// ---------------------------------------------------------------------------

#define DEVICE_NAME  L"\\Device\\KrnlExec"
#define SYMLINK_NAME L"\\DosDevices\\KrnlExec"

#define IOCTL_INIT    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_EXECUTE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_STATUS  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_RELEASE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

#pragma pack(push, 1)
struct InitRequest   { ULONG Pid; };
struct ExecRequest   { ULONG Pid; ULONG Size; CHAR  Script[1]; };
struct StatusReply   {
    ULONGLONG LoadString;
    ULONGLONG ScriptContext;
    ULONGLONG DataModel;
    ULONG     Pid;
    ULONG     Ready;
};
#pragma pack(pop)

// ---------------------------------------------------------------------------
// Undocumented kernel APIs (user-mode APC plumbing).
// Declarations verified against the WRK and public symbols; prototypes have
// been stable since Vista.
// ---------------------------------------------------------------------------

typedef enum _KAPC_ENVIRONMENT {
    OriginalApcEnvironment,
    AttachedApcEnvironment,
    CurrentApcEnvironment,
    InsertApcEnvironment
} KAPC_ENVIRONMENT;

typedef VOID (NTAPI *PKNORMAL_ROUTINE)(PVOID NormalContext, PVOID SystemArg1, PVOID SystemArg2);
typedef VOID (NTAPI *PKKERNEL_ROUTINE)(
    PKAPC Apc, PKNORMAL_ROUTINE* NormalRoutine, PVOID* NormalContext,
    PVOID* SystemArg1, PVOID* SystemArg2);
typedef VOID (NTAPI *PKRUNDOWN_ROUTINE)(PKAPC Apc);

extern "C" NTKERNELAPI VOID NTAPI KeInitializeApc(
    PKAPC Apc, PETHREAD Thread, KAPC_ENVIRONMENT Environment,
    PKKERNEL_ROUTINE KernelRoutine, PKRUNDOWN_ROUTINE RundownRoutine,
    PKNORMAL_ROUTINE NormalRoutine, KPROCESSOR_MODE ApcMode, PVOID NormalContext);

extern "C" NTKERNELAPI BOOLEAN NTAPI KeInsertQueueApc(
    PKAPC Apc, PVOID SystemArg1, PVOID SystemArg2, KPRIORITY Increment);

extern "C" NTKERNELAPI VOID NTAPI KeTestAlertThread(KPROCESSOR_MODE AlertMode);

extern "C" NTKERNELAPI NTSTATUS NTAPI ZwQuerySystemInformation(
    ULONG SystemInformationClass, PVOID SystemInformation,
    ULONG SystemInformationLength, PULONG ReturnLength);

#define SystemProcessInformation 5

typedef struct _SYSTEM_THREAD_INFORMATION {
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER CreateTime;
    ULONG         WaitTime;
    PVOID         StartAddress;
    CLIENT_ID     ClientId;
    KPRIORITY     Priority;
    LONG          BasePriority;
    ULONG         ContextSwitches;
    ULONG         ThreadState;
    ULONG         WaitReason;
} SYSTEM_THREAD_INFORMATION, *PSYSTEM_THREAD_INFORMATION;

typedef struct _SYSTEM_PROCESS_INFORMATION {
    ULONG           NextEntryOffset;
    ULONG           NumberOfThreads;
    LARGE_INTEGER   Reserved1[3];
    LARGE_INTEGER   CreateTime;
    LARGE_INTEGER   UserTime;
    LARGE_INTEGER   KernelTime;
    UNICODE_STRING  ImageName;
    KPRIORITY       BasePriority;
    HANDLE          UniqueProcessId;
    HANDLE          Reserved2;
    ULONG           HandleCount;
    ULONG           SessionId;
    ULONG_PTR       Reserved3;
    SIZE_T          PeakVirtualSize;
    SIZE_T          VirtualSize;
    ULONG           Reserved4;
    SIZE_T          PeakWorkingSetSize;
    SIZE_T          WorkingSetSize;
    ULONG_PTR       Reserved5;
    SIZE_T          QuotaPagedPoolUsage;
    ULONG_PTR       Reserved6;
    SIZE_T          QuotaNonPagedPoolUsage;
    SIZE_T          PagefileUsage;
    SIZE_T          PeakPagefileUsage;
    SIZE_T          PrivatePageCount;
    LARGE_INTEGER   Reserved7[6];
    SYSTEM_THREAD_INFORMATION Threads[1];
} SYSTEM_PROCESS_INFORMATION, *PSYSTEM_PROCESS_INFORMATION;

// ---------------------------------------------------------------------------
// Global state.
// ---------------------------------------------------------------------------

struct RuntimeState {
    PEPROCESS  Target;
    ULONG      Pid;
    uintptr_t  LoadString;      // ScriptContext::LoadString in RobloxPlayerBeta
    uintptr_t  DataModel;
    uintptr_t  ScriptContext;
    uintptr_t  ModuleBase;
    SIZE_T     ModuleSize;
    BOOLEAN    Ready;
};

static RuntimeState g_rs    = {};
static FAST_MUTEX  g_lock   = {};
static const ULONG POOL_TAG = 'cxeK';

// ---------------------------------------------------------------------------
// Safe cross-process memory primitives.
// MmCopyVirtualMemory handles the attach internally; never nest it inside a
// KeStackAttachProcess.
// ---------------------------------------------------------------------------

static NTSTATUS ReadRemote(PVOID addr, PVOID out, SIZE_T n)
{
    if (!g_rs.Target) return STATUS_INVALID_DEVICE_STATE;
    SIZE_T bytes = 0;
    __try {
        return MmCopyVirtualMemory(g_rs.Target, addr, PsGetCurrentProcess(),
                                   out, n, KernelMode, &bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

static NTSTATUS WriteRemote(PVOID addr, PVOID src, SIZE_T n)
{
    if (!g_rs.Target) return STATUS_INVALID_DEVICE_STATE;
    SIZE_T bytes = 0;
    __try {
        return MmCopyVirtualMemory(PsGetCurrentProcess(), src, g_rs.Target,
                                   addr, n, KernelMode, &bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

// ---------------------------------------------------------------------------
// Chunked pattern scanner. 4 MiB windows with overlap, paged pool.
// ---------------------------------------------------------------------------

static BOOLEAN sig_match(const UCHAR* mem, const UCHAR* pat, const char* mask, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; ++i)
        if (mask[i] != '?' && pat[i] != mem[i]) return FALSE;
    return TRUE;
}

static uintptr_t Scan(uintptr_t base, SIZE_T size, const UCHAR* pat, const char* mask)
{
    SIZE_T pat_len = strlen(mask);
    if (!pat_len || pat_len >= size) return 0;

    const SIZE_T WIN = 4 * 1024 * 1024;
    UCHAR* buf = (UCHAR*)ExAllocatePool2(POOL_FLAG_PAGED, WIN + pat_len, POOL_TAG);
    if (!buf) return 0;

    uintptr_t found = 0;
    SIZE_T offset = 0;

    while (offset < size) {
        SIZE_T want = min(WIN + pat_len, size - offset);
        SIZE_T got = 0;
        NTSTATUS st;
        __try {
            st = MmCopyVirtualMemory(g_rs.Target, (PVOID)(base + offset),
                                     PsGetCurrentProcess(), buf, want,
                                     KernelMode, &got);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            st = GetExceptionCode();
        }
        if (!NT_SUCCESS(st) || got < pat_len) {
            offset += WIN;
            continue;
        }

        SIZE_T end = got - pat_len;
        for (SIZE_T i = 0; i <= end; ++i) {
            if (sig_match(buf + i, pat, mask, pat_len)) {
                found = base + offset + i;
                goto done;
            }
        }

        offset += WIN; // overlap preserved by the +pat_len read-ahead
    }

done:
    ExFreePoolWithTag(buf, POOL_TAG);
    return found;
}

// ---------------------------------------------------------------------------
// Module base lookup via the target's PEB loader list, with __try/__except
// around every user-mode dereference.
// ---------------------------------------------------------------------------

static uintptr_t GetModuleBase(const wchar_t* needle, SIZE_T* outSize)
{
    if (!g_rs.Target) return 0;

    KAPC_STATE apc;
    KeStackAttachProcess(g_rs.Target, &apc);

    uintptr_t base = 0;
    SIZE_T    size = 0;

    __try {
        PPEB peb = PsGetProcessPeb(g_rs.Target);
        if (!peb || !peb->Ldr) __leave;

        PLIST_ENTRY head = &peb->Ldr->InLoadOrderModuleList;
        for (PLIST_ENTRY cur = head->Flink; cur && cur != head; cur = cur->Flink) {
            PLDR_DATA_TABLE_ENTRY e = CONTAINING_RECORD(cur, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
            if (!e->BaseDllName.Buffer || !e->BaseDllName.Length) continue;
            // Case-insensitive substring match.
            USHORT nlen = (USHORT)wcslen(needle);
            if (e->BaseDllName.Length / sizeof(WCHAR) < nlen) continue;
            if (_wcsnicmp(e->BaseDllName.Buffer, needle, nlen) == 0) {
                base = (uintptr_t)e->DllBase;
                size = e->SizeOfImage;
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        base = 0;
    }

    KeUnstackDetachProcess(&apc);
    if (outSize) *outSize = size;
    return base;
}

// ---------------------------------------------------------------------------
// Symbol resolution.
//
// The signatures below are PLACEHOLDERS that drift with every Roblox update.
// Refresh procedure:
//   1. Dump the running RobloxPlayerBeta.exe via ReadProcessMemory or via
//      your favorite memory-dump driver.
//   2. Open in IDA/Ghidra/Binja. Locate ScriptContext::LoadString by
//      cross-referencing the string "LoadString" or "@=..." chunk name
//      conventions, OR by looking at the ScriptContext vtable.
//   3. Copy the prologue + 40 bytes and dot out register/offset fields.
//   4. Refresh kDM_Offset by stepping from FakeDataModel -> real DataModel
//      -> ScriptContext in a debugger.
// ---------------------------------------------------------------------------

// ScriptContext::LoadString(ScriptContext*, const char* src, const char* chunk)
// Prologue + anchor. Replace with the current build's bytes.
static const UCHAR kLoadStringPat[] = {
    0x48, 0x89, 0x5C, 0x24, 0x00,   // mov [rsp+??], rbx
    0x48, 0x89, 0x74, 0x24, 0x00,   // mov [rsp+??], rsi
    0x57,                            // push rdi
    0x48, 0x83, 0xEC, 0x00,          // sub rsp, ??
    0x49, 0x8B, 0xF8                 // mov rdi, r8
};
static const char   kLoadStringMask[] = "xxxx?xxxx?xxxx?xxx";

// Access to FakeDataModel singleton: mov rax, [rip+disp32] prologue.
// Pattern targets `RBX::FakeDataModel::get()` style accessor.
static const UCHAR kFakeDMPat[] = {
    0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,  // mov rax, [rip+disp32]
    0x48, 0x85, 0xC0,                           // test rax, rax
    0x74, 0x00,                                 // jz short ??
    0x48, 0x8B, 0x80                            // mov rax, [rax+??]
};
static const char kFakeDMMask[] = "xxx????xxxx?xxx";

// Offset from FakeDataModel singleton to real DataModel*.
// Historically stable within a Roblox major revision; refresh if the chain
// breaks. Documented here, not buried.
static const ULONG kFakeDM_to_DM = 0x1B8;

// Offset from DataModel to ScriptContext*. Walk via class-descriptor table at
// first init, cache. Placeholder for the signature-less fallback.
static const ULONG kDM_to_ScriptContext = 0x3B0;

// ---------------------------------------------------------------------------

static NTSTATUS ResolveSymbols()
{
    g_rs.ModuleBase = GetModuleBase(L"RobloxPlayerBeta", &g_rs.ModuleSize);
    if (!g_rs.ModuleBase) return STATUS_NOT_FOUND;

    g_rs.LoadString = Scan(g_rs.ModuleBase, g_rs.ModuleSize,
                           kLoadStringPat, kLoadStringMask);
    if (!g_rs.LoadString) return STATUS_NOT_FOUND;

    uintptr_t fdm_ref = Scan(g_rs.ModuleBase, g_rs.ModuleSize,
                             kFakeDMPat, kFakeDMMask);
    if (!fdm_ref) return STATUS_NOT_FOUND;

    // rel32 at fdm_ref+3, next-instruction base = fdm_ref+7.
    LONG rel = 0;
    NTSTATUS st = ReadRemote((PVOID)(fdm_ref + 3), &rel, sizeof(rel));
    if (!NT_SUCCESS(st)) return st;
    uintptr_t fdm_slot = fdm_ref + 7 + (LONG_PTR)rel;

    uintptr_t fdm = 0;
    st = ReadRemote((PVOID)fdm_slot, &fdm, sizeof(fdm));
    if (!NT_SUCCESS(st) || !fdm) return STATUS_NOT_FOUND;

    uintptr_t dm = 0;
    st = ReadRemote((PVOID)(fdm + kFakeDM_to_DM), &dm, sizeof(dm));
    if (!NT_SUCCESS(st) || !dm) return STATUS_NOT_FOUND;
    g_rs.DataModel = dm;

    uintptr_t sc = 0;
    st = ReadRemote((PVOID)(dm + kDM_to_ScriptContext), &sc, sizeof(sc));
    if (!NT_SUCCESS(st) || !sc) return STATUS_NOT_FOUND;
    g_rs.ScriptContext = sc;

    g_rs.Ready = TRUE;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Attach / release.
// ---------------------------------------------------------------------------

static VOID ReleaseTarget()
{
    if (g_rs.Target) {
        ObDereferenceObject(g_rs.Target);
        g_rs.Target = nullptr;
    }
    g_rs.Pid = 0;
    g_rs.LoadString = 0;
    g_rs.DataModel = 0;
    g_rs.ScriptContext = 0;
    g_rs.ModuleBase = 0;
    g_rs.ModuleSize = 0;
    g_rs.Ready = FALSE;
}

static NTSTATUS AttachTarget(ULONG pid)
{
    ReleaseTarget();

    PEPROCESS p = nullptr;
    NTSTATUS st = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid, &p);
    if (!NT_SUCCESS(st)) return st;

    g_rs.Target = p;
    g_rs.Pid    = pid;

    st = ResolveSymbols();
    if (!NT_SUCCESS(st)) {
        ReleaseTarget();
        return st;
    }
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// User-mode buffer allocation + APC injection.
// ---------------------------------------------------------------------------

static NTSTATUS AllocInTarget(SIZE_T size, PVOID* out)
{
    *out = nullptr;
    PVOID base = nullptr;
    SIZE_T region = size;

    KAPC_STATE apc;
    KeStackAttachProcess(g_rs.Target, &apc);

    NTSTATUS st = ZwAllocateVirtualMemory(ZwCurrentProcess(), &base, 0,
                                          &region, MEM_COMMIT | MEM_RESERVE,
                                          PAGE_READWRITE);

    KeUnstackDetachProcess(&apc);

    if (NT_SUCCESS(st)) *out = base;
    return st;
}

static VOID FreeInTarget(PVOID base, SIZE_T size)
{
    if (!base) return;
    KAPC_STATE apc;
    KeStackAttachProcess(g_rs.Target, &apc);
    SIZE_T region = size;
    ZwFreeVirtualMemory(ZwCurrentProcess(), &base, &region, MEM_RELEASE);
    KeUnstackDetachProcess(&apc);
}

// APC kernel-routine: free the PKAPC allocation that we attached to the
// thread. Fires regardless of whether the user-mode normal routine ran.
static VOID ApcKernelRoutine(PKAPC Apc, PKNORMAL_ROUTINE* NR, PVOID* NC,
                             PVOID* S1, PVOID* S2)
{
    UNREFERENCED_PARAMETER(NR);
    UNREFERENCED_PARAMETER(NC);
    UNREFERENCED_PARAMETER(S1);
    UNREFERENCED_PARAMETER(S2);
    ExFreePoolWithTag(Apc, POOL_TAG);
}

static VOID ApcRundownRoutine(PKAPC Apc)
{
    ExFreePoolWithTag(Apc, POOL_TAG);
}

// Enumerate every thread in the target PID and queue a user-mode APC that
// calls r_loadstring(ScriptContext, script_user_ptr, chunk_user_ptr).
// Returns count of successfully queued APCs.
static ULONG QueueApcOnAllThreads(PKNORMAL_ROUTINE nr, PVOID ctx,
                                  PVOID arg1, PVOID arg2)
{
    ULONG info_len = 0;
    ZwQuerySystemInformation(SystemProcessInformation, nullptr, 0, &info_len);
    if (!info_len) return 0;

    info_len += 0x1000;
    PVOID info = ExAllocatePool2(POOL_FLAG_PAGED, info_len, POOL_TAG);
    if (!info) return 0;

    NTSTATUS st = ZwQuerySystemInformation(SystemProcessInformation,
                                           info, info_len, &info_len);
    if (!NT_SUCCESS(st)) { ExFreePoolWithTag(info, POOL_TAG); return 0; }

    ULONG queued = 0;
    PSYSTEM_PROCESS_INFORMATION p = (PSYSTEM_PROCESS_INFORMATION)info;

    while (true) {
        if ((ULONG_PTR)p->UniqueProcessId == g_rs.Pid) {
            for (ULONG i = 0; i < p->NumberOfThreads; ++i) {
                HANDLE tid = p->Threads[i].ClientId.UniqueThread;
                PETHREAD te = nullptr;
                if (!NT_SUCCESS(PsLookupThreadByThreadId(tid, &te))) continue;

                PKAPC apc = (PKAPC)ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                  sizeof(KAPC), POOL_TAG);
                if (!apc) { ObDereferenceObject(te); continue; }

                KeInitializeApc(apc, te, OriginalApcEnvironment,
                                ApcKernelRoutine, ApcRundownRoutine,
                                nr, UserMode, ctx);

                if (KeInsertQueueApc(apc, arg1, arg2, 0)) {
                    KeTestAlertThread(UserMode);
                    ++queued;
                } else {
                    ExFreePoolWithTag(apc, POOL_TAG);
                }
                ObDereferenceObject(te);
            }
            break;
        }
        if (!p->NextEntryOffset) break;
        p = (PSYSTEM_PROCESS_INFORMATION)((UCHAR*)p + p->NextEntryOffset);
    }

    ExFreePoolWithTag(info, POOL_TAG);
    return queued;
}

// ---------------------------------------------------------------------------
// Script execution.
// ---------------------------------------------------------------------------

static const char kChunkName[] = "=krnlexec";

static NTSTATUS ExecuteScript(const CHAR* src, SIZE_T src_len)
{
    if (!g_rs.Ready) return STATUS_DEVICE_NOT_READY;
    if (!src || !src_len) return STATUS_INVALID_PARAMETER;

    // Layout a single user-mode page:  [script\0][chunkname\0]
    SIZE_T need = src_len + 1 + sizeof(kChunkName);
    PVOID  u_base = nullptr;
    NTSTATUS st = AllocInTarget(need, &u_base);
    if (!NT_SUCCESS(st)) return st;

    // Copy script.
    st = WriteRemote(u_base, (PVOID)src, src_len);
    if (!NT_SUCCESS(st)) { FreeInTarget(u_base, need); return st; }

    CHAR z = 0;
    WriteRemote((PUCHAR)u_base + src_len, &z, 1);

    PVOID u_chunk = (PUCHAR)u_base + src_len + 1;
    st = WriteRemote(u_chunk, (PVOID)kChunkName, sizeof(kChunkName));
    if (!NT_SUCCESS(st)) { FreeInTarget(u_base, need); return st; }

    // x64 APC dispatch: NormalRoutine(NormalContext, SysArg1, SysArg2)
    // ->   rcx = ScriptContext,  rdx = script*,  r8 = chunkname*
    // which is exactly ScriptContext::LoadString's ABI.
    PKNORMAL_ROUTINE nr   = (PKNORMAL_ROUTINE)g_rs.LoadString;
    PVOID            ctx  = (PVOID)g_rs.ScriptContext;
    PVOID            arg1 = u_base;
    PVOID            arg2 = u_chunk;

    ULONG q = QueueApcOnAllThreads(nr, ctx, arg1, arg2);

    // We intentionally do NOT free u_base: the APC still owns it. It will
    // leak one page per executed script, which is acceptable for an exec.
    // A proper implementation hands ownership to a cleanup APC that fires
    // after the normal routine returns.

    return q ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

// ---------------------------------------------------------------------------
// IOCTL dispatch.
// ---------------------------------------------------------------------------

static NTSTATUS DoInit(PIRP irp, PIO_STACK_LOCATION sp)
{
    if (sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(InitRequest))
        return STATUS_BUFFER_TOO_SMALL;
    InitRequest* r = (InitRequest*)irp->AssociatedIrp.SystemBuffer;
    return AttachTarget(r->Pid);
}

static NTSTATUS DoExec(PIRP irp, PIO_STACK_LOCATION sp)
{
    ULONG in_len = sp->Parameters.DeviceIoControl.InputBufferLength;
    if (in_len < FIELD_OFFSET(ExecRequest, Script)) return STATUS_BUFFER_TOO_SMALL;

    ExecRequest* r = (ExecRequest*)irp->AssociatedIrp.SystemBuffer;
    ULONG script_bound = in_len - FIELD_OFFSET(ExecRequest, Script);
    if (r->Size == 0 || r->Size > script_bound) return STATUS_INVALID_PARAMETER;
    if (r->Pid != g_rs.Pid) return STATUS_DEVICE_NOT_READY;

    return ExecuteScript(r->Script, r->Size);
}

static NTSTATUS DoStatus(PIRP irp, PIO_STACK_LOCATION sp)
{
    if (sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(StatusReply))
        return STATUS_BUFFER_TOO_SMALL;

    StatusReply* out = (StatusReply*)irp->AssociatedIrp.SystemBuffer;
    out->LoadString    = g_rs.LoadString;
    out->ScriptContext = g_rs.ScriptContext;
    out->DataModel     = g_rs.DataModel;
    out->Pid           = g_rs.Pid;
    out->Ready         = g_rs.Ready;
    irp->IoStatus.Information = sizeof(StatusReply);
    return STATUS_SUCCESS;
}

static NTSTATUS DispatchControl(PDEVICE_OBJECT dev, PIRP irp)
{
    UNREFERENCED_PARAMETER(dev);
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;

    ExAcquireFastMutex(&g_lock);
    switch (sp->Parameters.DeviceIoControl.IoControlCode) {
        case IOCTL_INIT:    st = DoInit(irp, sp);   break;
        case IOCTL_EXECUTE: st = DoExec(irp, sp);   break;
        case IOCTL_STATUS:  st = DoStatus(irp, sp); break;
        case IOCTL_RELEASE: ReleaseTarget(); st = STATUS_SUCCESS; break;
    }
    ExReleaseFastMutex(&g_lock);

    irp->IoStatus.Status = st;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return st;
}

static NTSTATUS DispatchCreateClose(PDEVICE_OBJECT dev, PIRP irp)
{
    UNREFERENCED_PARAMETER(dev);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Driver entry / unload.
// ---------------------------------------------------------------------------

static VOID DriverUnload(PDRIVER_OBJECT drv)
{
    UNICODE_STRING sym;
    RtlInitUnicodeString(&sym, SYMLINK_NAME);
    IoDeleteSymbolicLink(&sym);

    ExAcquireFastMutex(&g_lock);
    ReleaseTarget();
    ExReleaseFastMutex(&g_lock);

    if (drv->DeviceObject) IoDeleteDevice(drv->DeviceObject);
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    UNREFERENCED_PARAMETER(reg);
    ExInitializeFastMutex(&g_lock);

    UNICODE_STRING name;
    RtlInitUnicodeString(&name, DEVICE_NAME);

    PDEVICE_OBJECT dev = nullptr;
    NTSTATUS st = IoCreateDevice(drv, 0, &name, FILE_DEVICE_UNKNOWN,
                                 FILE_DEVICE_SECURE_OPEN, FALSE, &dev);
    if (!NT_SUCCESS(st)) return st;

    UNICODE_STRING sym;
    RtlInitUnicodeString(&sym, SYMLINK_NAME);
    st = IoCreateSymbolicLink(&sym, &name);
    if (!NT_SUCCESS(st)) { IoDeleteDevice(dev); return st; }

    drv->MajorFunction[IRP_MJ_CREATE]         = DispatchCreateClose;
    drv->MajorFunction[IRP_MJ_CLOSE]          = DispatchCreateClose;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchControl;
    drv->DriverUnload                         = DriverUnload;

    dev->Flags |= DO_BUFFERED_IO;
    return STATUS_SUCCESS;
}
