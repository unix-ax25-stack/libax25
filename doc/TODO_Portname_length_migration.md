# Portname Length Migration

## Problem
`struct sockaddr` has `sa_data[14]` which truncates long logical port names like `wampes:dl9sau-test`. Tools (listen, mheardd, rxecho, net2kiss, ax25rtd) work around this with ad-hoc parsing of `sa.sa_data`.

## Goal
Store full logical port names in `struct axmon` per source, add `axmon_port_name()`, make tools use it instead of parsing `sa_data`. This removes scattered workarounds across the suite and fixes issues like `listen -a` not showing full names, mheardd duplicates, and rxecho port resolution.

## Impact Analysis
- libax25: `netax25/axmon.h`, `axmon.c`, `wampes.c`, `wampes.h`, `agwpe_sock.c`, `axsock.c` (indirect)
- ax25-tools: `ax25/mheardd.c`, `ax25/rxecho.c`, `kiss/net2kiss.c`, `ax25/beacon.c` (check usage)
- ax25-apps: `listen/listen.c`, `ax25mond/ax25mond.c`, `ax25rtd/listener.c`, others using `axmon_read()` with `sa`

## Design
- Add `char portname[AXMON_MAX_FD][AXMON_PORT_NAME_LEN]` to `struct axmon` (suggest `AXMON_PORT_NAME_LEN 64` or larger)
- Add `extern const char *axmon_port_name(const struct axmon *mon, int idx);` API
- Keep `axmon_read()` signature unchanged and still fill `sa` for compatibility
- Fill names at source: 
  - WAMPES: `wampes_mon_read()` knows node/interface; propagate full name
  - AGWPE/Shim: `axsock_raw_name()` has access to port name/bound name; copy full name
  - Kernel: resolve port/device name from config and store in `axmon.portname`
- Migrate tools to prefer `axmon_port_name()`; remove ad-hoc `sa_data` parsing where possible
- `axmon_source_name()` remains for logging/source type descriptions

## Design Details
- **Lazy + memoized caching:** `axmon_port_name(mon, idx)` returns cached `mon->portname[idx]`. Only on first call / when empty it resolves once and caches. Never scans axports/config on every call.
- **Learn from sources:** `wampes_mon_read()` and `axsock_raw_name()` write the full logical port name directly into `mon->portname[idx]` as soon as known (late attach / WAMPES starts later).
- **Multi-source aware (PF_PACKET):** For `AXMON_KERNEL` with a bundled packet socket (frames from multiple interfaces), prefer the interface/name from frame context (`sa_data`) over a single cached name. If a port is in `axports` use its logical name, else fall back to the interface name from `sa_data`.
- **Kernel fallback:** If interface not in `axports`, display the interface name from `sa_data` (e.g. `bpq1`).
- **Stale/rebuild on source changes:** If the monitor connection drops (e.g. mheardd loses ax25netd), caches can become stale. Allow rebuilding lazy caches when a new/unknown interface name appears from frames, or when sources are re-opened/retired. 
- **Index coupling:** `portname[idx]` is tied to `fd[idx]/kind[idx]`. Indices may change across restarts/reopens; within a running process stability is preferred but not guaranteed.
- **sa_data handling:** Always `memset` + NUL-terminate with `sizeof(sa_data)-1` (`strlcpy`-style). Never use markers in `sa_data`. Keep `sa_data` filled for compatibility but non-authoritative for full names.
- **No live re-resolution:** Do not re-resolve against axports on every `axmon_read()`. Only lazy-init once or when forced by new source appearance/reconnect.

## Implementation Plan
1. Extend `struct axmon` and add `axmon_port_name()` in libax25 (`netax25/axmon.h`)
2. Update `axmon.c` to store/retrieve port names per source index with lazy memoization
3. Update `wampes.c`/`wampes.h` and `agwpe_sock.c` to populate full names
4. Build and install libax25
5. Reset `ax25-tools.git/ax25/mheardd.c` to git HEAD, then adapt minimally to use `axmon_port_name()`
6. Update other tools/apps (`listen`, `rxecho`, `net2kiss`, `ax25mond`, `ax25rtd/listener.c`) to use `axmon_port_name()` and remove workarounds
7. Test on all backends (kernel, AGWPE/ax25netd, WAMPES)

## Testing
- `listen -a` without `-p` shows full untruncated names (e.g. `wampes:dl9sau-test`)
- WAMPES started after monitor starts still shows correct names (late attach)
- ax25netd restarted while monitors running handled correctly
- `mheardd` has no duplicates for long WAMPES names; counts correctly
- Kernel with interfaces not in axports falls back to interface name from `sa_data`
- `rxecho` routing works across mixed backends
- Kernel + AGWPE + WAMPES tested in parallel
- Long names like `wampes:dl9sau-test` work end-to-end

## Notes
- No need to maintain backward compatibility across trees if rebuilt together (as per current workflow)
- `sa_data[14]` remains for compatibility but becomes non-authoritative
- Avoid unnecessary calls to `axmon_port_name()` in hot paths, but the call itself must be O(1) after caching
