# concurrency audit: agentty → maya → jaal

Written from the code. Each item names where to look.

## what jaal enforces

- Lock order is a type. `guarded<T,R>::with` needs `below<R>`, and a lock
  body gets `held<R>`. Taking locks out of rank order doesn't compile. A TLS
  `held_top` check also aborts at the first out-of-order lock or wait if a
  capability was forged.
- Every blocking wait needs `unlocked`: `guarded::wait_with`, pool and
  worker_group shutdown, scope, delay_for, stop_group, and now the std
  waits too: `job_result::get/wait_for`, `jaal::atomic_wait`, `jaal::wait`
  (kernel/waits_for.hpp).
- `jaal::once` runs its body as if it held a leaf lock. Any guarded lock or
  jaal wait inside it aborts on the spot, so a `call_once` body can't be
  half of a deadlock, and the call itself needs nothing.
- `published<T>` checks what it hands out. A `published<T>` needs `Sync<T>`.
  A `published<const T>` needs `ConstSync<T>`: plain fields Frozen, and any
  `mutable` field Sync (core/sync.hpp). Before this, `published` accepted
  anything.
- Sendable, Sync, Frozen and JoinFree walkers cover task args, job args,
  lock-body args and results, co_owned contents and guarded contents.
- `guarded::with_foreign` is the one sanctioned borrow under a lock. It
  covers a callback whose shape a library fixed. It's still rank-checked,
  and it replaced a fake Sendable wrapper around a stack pointer.
- Only the kernel can arm a loop thread (`loop_identity` needs an
  `arm_key`). Tests ask for `arm_loop_for_tests()` by name, and the banlist
  flags that name outside tests.
- The banlist also bans `std::call_once`, `std::future<` and raw atomic
  waits outside jaal's own implementation files.

Each of these has a compile-fail test in jaal (`tests/compile_fail/`,
cases capability 19-24 and loop_bound 8).

## what was wrong and is now fixed

| where | problem | now |
|---|---|---|
| jaal published.hpp | no check on T | Sync / ConstSync required |
| tool/registry.cpp | `wire_tools()` returned a `const&` out of a dropped shared_ptr. It was only safe because every snapshot was kept forever via a `prev` chain, so the catalog leaked once per reload | `Catalog` is a `maya::shared` handle, `find()` returns `Found` (handle plus def), `prev` is gone |
| tool/mcp_tools_backends.cpp | user agent store: same `prev` leak, `const AgentType&` held for a whole run | `ResolvedAgent` holds its generation; `prev` is gone |
| tool/mcp_tools_bridge.cpp | `MAYA_SENDABLE(ToolState)` (false: it holds shared Sessions) and `FnRef{&f}` laundering a stack pointer as Sendable | `guarded::with_foreign`; both opt-ins deleted |
| provider/acp_provider_adapter.cpp | `release_acp_agents` reset a LiveAgent's backend and peer in place while a turn could still be inside `prompt()` | it only drops the cache; the last holder tears down in order (`~LiveAgent`) |
| rpc/peer.hpp | `default_timeout_` was a plain field, written after `start()` while the reader read it | atomic |
| acp/server.hpp | `stream_to_`/`reauth_` were set by setters, and `MAYA_SYNC` relied on "before serve()" | constructor `Extras`, fields const |
| maya theme.hpp | two `set_live` calls could interleave their theme and epoch stores, and a projection then cached the old palette under the new epoch for good. The first `projected()` could do the same against a concurrent set | writers serialise on `intern_mu`; `seen` starts at `~0u` |
| maya core/executor.hpp | `exec::post(std::function)` bypassed Sendable entirely | `post(void(*)(S&), shared_ptr<S>)`: a function pointer plus co-owned state, no closure |
| mcp/connection.cpp, mcp/bridge.cpp, run_cli.cpp | hand-made locks with `atomic::wait`, checked only by a regex allowlist | `maya::atomic_wait(cap, ...)` |
| 12 `std::call_once` sites | allowlisted by hand in hidden_locks.py | `maya::once`; that allowlist is gone |
| acp/server.cpp, mcp/bridge.cpp | `future::get/wait_for` with no capability | `job_result::get(cap)/wait_for(cap, d)` |

## what's still a human claim

These can't be proven by jaal's walkers. Each one sits next to the type
with the reason it holds:

- `thread_root` sites (reviewed roots, in tests/lint/allowlist.txt). The
  TLS check catches a lie at the next jaal lock or wait. Every std wait now
  goes through one of those.
- Opt-ins for library types jaal can't see into: `mcp::cap::Registry`
  (Frozen), `rpc::Peer`, `Connection::Live`, `LiveAgent`, `AgentServer`
  (Sync), `AgentType` (Frozen), `ToolBody`, `util::Callback` (Frozen).
- `this_thread::yield` spins (hidden_locks.py, the only rule left there).
- maya's own `call_once` in terminal.hpp: maya's core can't depend on jaal
  (tests/seam.sh). It's allowlisted, and the body is sigaction plus
  set_terminate.
