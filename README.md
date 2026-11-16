# sober-exec

Roblox script executor for Linux, targeting the Sober flatpak
(`org.vinegarhq.Sober`).

## Architecture

```
          host                              sober flatpak sandbox
 ┌──────────────────────┐            ┌─────────────────────────────────┐
 │ sober-cli            │            │ RobloxPlayerBeta                │
 │  └─ unix socket ─────┼────────────┤  └─ libsoberpay.so (LD_PRELOAD) │
 │                      │            │        ├─ luau api (sigscanned) │
 │ sober-exec           │ ptrace     │        ├─ ipc_server (unix)     │
 │  └─ ptrace injector ─┼────(opt)──▶│        ├─ scheduler drain       │
 │                      │            │        └─ exec_api (globals)    │
 └──────────────────────┘            └─────────────────────────────────┘
```

Two injection paths:

- **LD_PRELOAD (recommended for Sober).** The flatpak sandbox blocks
  ptrace from the host. Launch Sober via `sober-exec-run.sh`, which
  copies the payload under the app's data dir and sets
  `LD_PRELOAD` through `flatpak run --env`.
- **ptrace.** For non-flatpak Linux builds of Roblox (or when running
  Sober under `--devel`/permissive `yama.ptrace_scope`). Use
  `sober-exec inject`.

## Build

```bash
make
```

Outputs under `build/`:

- `libsoberpay.so` — the injected payload.
- `sober-exec`     — process locator + ptrace injector.
- `sober-cli`      — script dispatcher over unix socket.

## Launch & use

```bash
./sober-exec-run.sh &                  # starts Sober with payload preloaded
./build/sober-cli ping                 # verify socket is up
./build/sober-cli exec hello.lua       # run a script
./build/sober-cli exec -s 'print(identifyexecutor())'
```

Socket path: `$XDG_RUNTIME_DIR/sober-exec.sock`, fallback
`/tmp/sober-exec.sock`.

## Executor API surface

Bound into the elevated thread globals once the payload resolves Luau
and the first script arrives:

- `loadstring(src, chunkname?)`
- `identifyexecutor()` / `getexecutorname()`
- `getgenv()` / `getrenv()`
- `checkcaller()`
- `setreadonly` / `isreadonly`
- `getrawmetatable` / `setrawmetatable`
- `getnamecallmethod` / `setnamecallmethod`
- `hookfunction` / `replaceclosure` / `hookmetamethod` / `restorefunction`
- `newcclosure` / `iscclosure` / `islclosure`
- `gethui`
- `HttpGet` / `httpget`
- `queue_on_teleport` / `queueonteleport`
- `writefile` / `readfile` / `isfile` / `delfile`
- `getscripts` / `getloadedmodules` / `getconnections` / `getinstances`
  / `getnilinstances`
- `crypt.base64_encode` / `crypt.base64_decode` / `crypt.generatebytes`
- `base64.encode` / `base64.decode`

A chunk sent over the socket is compiled with `luau_compile`, loaded on
a fresh coroutine off the elevated thread, elevated to identity 8, then
`pcall`'d. Errors are written to the payload's stderr (visible in the
Sober flatpak's log).

## Signature maintenance

Pattern slots live in `payload/luau_api.cpp` under `kSigs[]`. On each
Roblox client update:

1. Pull the live `RobloxPlayerBeta` from the running flatpak
   (`~/.var/app/org.vinegarhq.Sober/data/sober/.../RobloxPlayerBeta`).
2. Load into IDA / Ghidra / Binary Ninja.
3. Locate each Luau C-API prologue, update the hex signature.
4. Rebuild.

The pattern slots in this tree are placeholders — they will not match
a current build unseen. Substitute before use.

## Flatpak notes

- The payload lives at `~/.var/app/org.vinegarhq.Sober/data/sober-exec/libsoberpay.so`
  after `sober-exec-run.sh` runs.
- `XDG_RUNTIME_DIR` is shared host↔sandbox by default, so the socket
  bridges without extra bind mounts.
- File-IO globals (`writefile` etc.) rewrite paths under `/app/exec/`
  inside the sandbox; change in `exec_api.cpp` if you want a host-visible
  workspace (e.g. `${XDG_DATA_HOME}/sober-exec/workspace`).

## Troubleshooting

- `[payload] signature set out of date` — patterns need refresh.
- `connect(...): No such file or directory` — payload didn't load or
  the socket path mismatches. Check Sober's stderr for
  `[payload] ipc listening on ...`.
- `yama.ptrace_scope` — ptrace injection route only; for LD_PRELOAD
  path, irrelevant.
