#!/usr/bin/env python3
"""hidden_locks: blocking waits that jaal's lock order can't see.

  spin         this_thread::yield() in a loop waiting on another thread

atomic::wait and std::call_once used to be listed here. They are types now:
maya::atomic_wait needs `unlocked`, and maya::once runs its body as a leaf
(a lock or wait inside aborts), so the banlist refuses the raw spellings and
no hand check is needed. A spin can't be made a type: each site below was
checked by hand to wait only on a syscall-length window with no jaal lock
held. A new site needs the same check, then a line here saying why.

Usage: hidden_locks.py ROOT
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
from disk_writes import strip  # noqa: E402

EXTS = (".cpp", ".hpp", ".h")

RULES = {
    "spin": (
        r"\bthis_thread::yield\s*\(",
        {
            "src/util/logx.cpp",           # only while a dup2 swap is in flight
            "src/workspace/symbols.cpp",   # a scheduler seam, not a wait
            "src/provider/chatgpt/codex_oauth.cpp",  # a cancel's shutdown() call
            "include/agentty/util/child_process.hpp",  # a stop/reap syscall
        },
    ),
}


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    root = sys.argv[1]
    compiled = {k: re.compile(p) for k, (p, _) in RULES.items()}
    hits, used = [], set()
    for sub in ("src", "include"):
        for dp, _, files in os.walk(os.path.join(root, sub)):
            for f in sorted(files):
                if not f.endswith(EXTS):
                    continue
                path = os.path.join(dp, f)
                rel = os.path.relpath(path, root).replace(os.sep, "/")
                raw = open(path, encoding="utf-8", errors="replace").read()
                lines = raw.split("\n")
                for no, line in enumerate(strip(raw).split("\n"), 1):
                    for rule, rx in compiled.items():
                        if not rx.search(line):
                            continue
                        if rel in RULES[rule][1]:
                            used.add((rule, rel))
                        else:
                            hits.append(f"{rel}:{no}: [{rule}] {lines[no - 1].strip()}")
    stale = [f"{rel} [{rule}]" for rule, (_, allow) in RULES.items()
             for rel in sorted(allow) if (rule, rel) not in used]
    print("hidden_locks: blocking waits outside jaal's lock order")
    if hits or stale:
        if hits:
            print(f"\n{len(hits)} unchecked site(s). Prove it never waits holding a "
                  "jaal lock (or use maya::guarded::wait_with), then allowlist it:")
            for h in hits:
                print("  " + h)
        if stale:
            print("\nallowlist entries with nothing left to allow (remove them):")
            for s in stale:
                print("  " + s)
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
