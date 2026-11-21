# winkrnl — Roblox executor API kernel driver (Windows 11 x64)

Kernel-mode companion to the Linux `sober-exec` stack. On Windows the
sandbox sits in a different place: instead of `LD_PRELOAD`ing into the
flatpak, we attach to the live `RobloxPlayerBeta.exe` from ring-0 and
execute Luau source via `ScriptContext::LoadString`, using a user-mode
APC so the call actually runs in user mode.

## Files

- `driver.cpp`   — the WDM driver (one file, ~500 LoC).
- `loader.cpp`   — user-mode CLI that drives the driver via IOCTLs.
- `KrnlExec.inf` — install script.

## Build

With the WDK installed and VS 2022:

```
# driver
cl /std:c++17 /kernel /GS- /guard:cf- /c driver.cpp
link /subsystem:native /driver /entry:DriverEntry \
     driver.obj ntoskrnl.lib hal.lib /out:KrnlExec.sys

# loader
cl /std:c++17 /EHsc /O2 loader.cpp /link /out:krnlexec.exe
```

Or build from the WDK sample template (`Driver > WDM Empty`) — drop
`driver.cpp` in and set target to KMDF off / WDM.

## Install

Test-sign and load manually, OR use a manual mapper (KDMapper, kdu, et al):

```
# one-time, as admin
bcdedit /set testsigning on
# reboot

# install
sc.exe create KrnlExec type= kernel binPath= C:\path\to\KrnlExec.sys
sc.exe start KrnlExec

# or inf
pnputil /add-driver KrnlExec.inf /install
```

## Usage

```
krnlexec attach               # auto-finds the Roblox window
krnlexec status               # dumps resolved state
krnlexec exec test.lua
krnlexec exec -s "print(identifyexecutor())"
krnlexec release              # detach, drop process ref
```

## Architecture

```
  user                    kernel                     target process
 ┌───────────┐  IOCTL   ┌───────────────────────┐  ┌─────────────────────┐
 │ krnlexec  │ ────────▶│  KrnlExec.sys         │  │ RobloxPlayerBeta    │
 │  attach   │          │  ├─ resolve r_loadstr │  │                     │
 │  exec     │          │  │   via sig scan     │──┤ .text / PEB         │
 │  status   │          │  ├─ walk FakeDM→DM→SC │  │                     │
 │  release  │          │  ├─ ZwAllocateVM in   │  │  user VAD (script)  │
 └───────────┘          │  │   target for script│──▶                     │
                        │  ├─ KeInitializeApc + │  │ main scheduler      │
                        │  │   KeInsertQueueApc │  │   thread fires APC  │
                        │  │   on all threads   │──▶  r_loadstring(...)  │
                        │  └─ UserMode dispatch │  │                     │
                        └───────────────────────┘  └─────────────────────┘
```

Why APC: kernel code cannot call a user-mode function pointer directly
(SMEP + CPL mismatch → bugcheck). The correct mechanism is to insert a
user-mode normal routine that fires when any thread next enters an
alertable wait. x64 APC dispatch places `NormalContext`, `SysArg1`,
`SysArg2` in `rcx, rdx, r8`, which is exactly the ABI for
`ScriptContext::LoadString(ScriptContext*, const char* src, const char* chunk)`
— so no shellcode is required.

## Signature maintenance

`driver.cpp` holds three refresh-prone values:

- `kLoadStringPat` — prologue of `ScriptContext::LoadString`.
- `kFakeDMPat`     — a mov-from-rip access to the FakeDataModel singleton.
- `kFakeDM_to_DM` + `kDM_to_ScriptContext` — offsets along the chain.

On each Roblox client update:

1. Attach your debugger to the running `RobloxPlayerBeta.exe`.
2. Find `ScriptContext::LoadString` via xref to `"=LoadString"` chunk
   convention or via the `ScriptContext` vtable. Grab 20–30 bytes of
   prologue with register/offset slots masked as `?`.
3. Find the `FakeDataModel::get()` accessor (singleton read). Pattern
   anchors to the `mov rax, [rip+disp32]; test rax,rax; jz; mov rax,[rax+?]`
   skeleton — update the disp32 and jz offset if the compiler shuffled.
4. Step from the FakeDataModel through to the real DataModel, note the
   offset. From DataModel to ScriptContext, note the offset.
5. Replace the four constants, recompile.

## Correctness checklist vs. the original paste

| issue                                           | status |
|-------------------------------------------------|--------|
| Kernel directly calls user-mode function pointer| fixed (APC) |
| No IOCTL `InputBufferLength` validation         | fixed |
| `PsLookupProcessByProcessId` ref never released | fixed |
| Non-paged alloc of full module size             | fixed (paged, chunked) |
| No `__try/__except` around PEB / user derefs    | fixed |
| Nested `KeStackAttachProcess` + `MmCopyVirtualMemory` | fixed |
| Script pointer was kernel-space                 | fixed (ZwAllocateVirtualMemory in target) |
| Class-registry "reflection" scan (broken by design) | removed |
| `r_loadstring` 9-byte prologue signature        | widened + masked |
| Comment claimed KMDF, code was WDM              | fixed |

## Known limits

- APC fires on whichever thread next enters an alertable wait; if that
  happens to be a thread other than the Luau task scheduler and
  `ScriptContext::LoadString` requires the scheduler thread, the call can
  race. In practice LoadString itself marshals internally, but under
  heavy load you can see delayed-execution. Fix is to signature-anchor
  the scheduler thread specifically and queue only there.
- No hooking of the Hyperion anti-cheat is attempted. If Hyperion is
  active, LoadString may be gated behind integrity checks that refuse
  calls originating outside the normal script pipeline. Resolution is
  separate work (Hyperion bypass, out of scope here).
- User-mode allocations made per `exec` are intentionally leaked (one
  page each). A production build should queue a cleanup APC after
  LoadString returns.
- Device ACL is `FILE_DEVICE_SECURE_OPEN` but no explicit SDDL — any
  elevated process can open the device. Tighten with
  `IoCreateDeviceSecure` + an SDDL string if you need lock-down.
