#!/usr/bin/env python3
"""
lock_order.py - STATIC lock-order analysis for lock ordering.

Traces the ipc_lock -> kq_lock -> wq_lock -> ipc_lock cycle in the
real source files and the opposite kq_lock -> ipc_lock order in
kevent_apply_changes. This is a STATIC analysis, not a runtime
check: the runtime check is infeasible on the x86_64 host because
kevent.c and workq.c contain aarch64-only clone trampolines
(___iosw_poller_clone, ___iosw_wq_clone, raw `mov x8, #220` /
SYS_clone asm) and target-only syscalls (epoll/eventfd/clock),
and no aarch64 runtime (qemu-aarch64) is available in this
environment. The script verifies each edge of the cycle against
the actual source text and prints the evidence with file:line.

Edges checked (forward cycle, the M7 self-deadlock):
  E0  ipc_lock is non-recursive (plain atomic exchange + futex,
      no owner or recursion count) - ipc.c
  E1  the enqueue hook fires while ipc_lock is held: a
      g_enqueue_hook( call site inside ipc_send_locked (or the
      kobject reply paths), and the hook is installed as
      kevent_machport_notify - ipc.c, kevent.c
  E2  kevent_machport_notify takes kq_lock - kevent.c
  E3  kevent_machport_notify_locked calls workq_dispatch_kevent
      - kevent.c
  E4  workq_dispatch_kevent holds wq_lock across the
      wq_spawn_worker call - workq.c
  E5  wq_spawn_worker calls ipc_port_allocate_thread, which takes
      ipc_lock - workq.c, ipc.c

Opposite-order leg:
  E6  kevent_apply_changes holds kq_lock while calling
      ipc_port_has_receive_right / ipc_port_peek_size, which take
      ipc_lock - kevent.c, ipc.c

Exit code: 0 when no lock-order CYCLE remains (the forward cycle
is absent); 1 when the cycle is present. On the BASE tree the
exit code is 1 (the cycle is real). A deadlock needs a cycle:
two opposite orders. The opposite leg E6 (kq_lock -> ipc_lock
in kevent_apply_changes) alone is a consistent order, not a
violation, once the forward edge is gone: the remaining orders
are kq_lock -> wq_lock -> ipc_lock (hook drain, poller) and
kq_lock -> ipc_lock, a total order with no path taking ipc_lock
then kq_lock. The fix resolves the cycle by deferring
the enqueue hook out of ipc_lock, so E6 may still be
present on the patched tree and the check still passes.

Usage:
  python3 lock_order.py --ipc <ipc.c> --kevent <kevent.c> \
      --workq <workq.c>
Paths default to the reference tree used by build.sh
(IPC_SRC, see build.sh).

No em dashes are used in this file.
"""
import argparse
import re
import sys


def read_lines(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def find_function(lines, name):
    """Return (start_line_0based, end_line_0based_exclusive, body_text)
    for the function named `name`, or None. The signature may span
    lines; the body starts at the first '{' after the name."""
    text = "\n".join(lines)
    m = re.search(r"(?<![\w])" + re.escape(name) + r"\s*\(", text)
    if not m:
        return None
    brace = text.find("{", m.start())
    if brace < 0:
        return None
    depth = 0
    i = brace
    while i < len(text):
        ch = text[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                body = text[brace:i + 1]
                start = text[:brace].count("\n")
                end = text[:i + 1].count("\n")
                return (start, end, body)
        i += 1
    return None


def line_no_of(text, pos):
    return text[:pos].count("\n") + 1


def find_calls(lines, pattern, start_line=None, end_line=None):
    """All (line_no_1based, text) of lines matching pattern, optionally
    restricted to a line range (0-based, end exclusive)."""
    out = []
    for i, ln in enumerate(lines):
        if start_line is not None and i < start_line:
            continue
        if end_line is not None and i >= end_line:
            continue
        if re.search(pattern, ln):
            out.append((i + 1, ln.strip()))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ipc", required=True, help="path to ipc.c")
    ap.add_argument("--kevent", required=True, help="path to kevent.c")
    ap.add_argument("--workq", required=True, help="path to workq.c")
    args = ap.parse_args()

    ipc = read_lines(args.ipc)
    kev = read_lines(args.kevent)
    wq = read_lines(args.workq)
    ipc_text = "\n".join(ipc)
    kev_text = "\n".join(kev)
    wq_text = "\n".join(wq)

    print("lock-order analysis (STATIC, not runtime)")
    print("  ipc.c:    %s" % args.ipc)
    print("  kevent.c: %s" % args.kevent)
    print("  workq.c:  %s" % args.workq)
    print("")

    problems = []

    # -- E0: ipc_lock is non-recursive. -------------------------------
    print("E0  ipc_lock non-recursive (ipc.c)")
    lk = find_function(ipc, "ipc_lock")
    if lk is None:
        print("    NOT FOUND: ipc_lock")
        problems.append("E0: ipc_lock not found")
    else:
        start, end, body = lk
        print("    ipc_lock at ipc.c:%d" % (start + 1))
        has_atomic = "__atomic_exchange_n" in body
        has_owner = bool(re.search(r"\bowner\b", body))
        has_recur = bool(re.search(r"recur", body, re.I))
        print("    atomic exchange: %s, owner tracking: %s, recursion: %s"
              % (has_atomic, has_owner, has_recur))
        if has_atomic and not has_owner and not has_recur:
            print("    E0 HOLDS: non-recursive (a second take deadlocks)")
        else:
            print("    E0 ABSENT: ipc_lock looks recursive or changed")
            problems.append("E0: ipc_lock shape changed")

    # -- E1: hook fires while ipc_lock is held. -----------------------
    print("E1  enqueue hook fires under ipc_lock (ipc.c)")
    hook_calls = find_calls(ipc, r"g_enqueue_hook\s*\(")
    # Drop the setter and the typedef line.
    hook_calls = [(n, t) for (n, t) in hook_calls
                  if "ipc_set_enqueue_hook" not in t
                  and "ipc_enqueue_hook_t" not in t]
    send_locked = find_function(ipc, "ipc_send_locked")
    in_send = []
    if send_locked is not None:
        s0, s1, _ = send_locked
        in_send = [(n, t) for (n, t) in hook_calls if s0 < n <= s1]
    for (n, t) in hook_calls:
        where = "ipc_send_locked" if (n, t) in in_send else "other"
        print("    ipc.c:%d [%s] %s" % (n, where, t))
    installed = find_calls(kev, r"ipc_set_enqueue_hook\s*\(\s*kevent_machport_notify\s*\)")
    if installed:
        for (n, t) in installed:
            print("    kevent.c:%d hook installed as kevent_machport_notify" % n)
    if in_send and installed:
        print("    E1 HOLDS: hook fires from ipc_send_locked under ipc_lock")
    else:
        print("    E1 ABSENT: no hook call under ipc_lock, or hook is not kevent_machport_notify")
        problems.append("E1: hook no longer fires under ipc_lock")

    # -- E2: kevent_machport_notify takes kq_lock. --------------------
    print("E2  kevent_machport_notify takes kq_lock (kevent.c)")
    notify = find_function(kev, "kevent_machport_notify")
    e2 = False
    if notify is None:
        print("    NOT FOUND: kevent_machport_notify")
        problems.append("E2: kevent_machport_notify not found")
    else:
        start, end, body = notify
        acq = find_calls(kev, r"kq_lock_acquire\s*\(\s*\)", start, end)
        if acq:
            for (n, t) in acq:
                print("    kevent.c:%d kq_lock_acquire()" % n)
            e2 = True
            print("    E2 HOLDS")
        else:
            print("    E2 ABSENT: no kq_lock_acquire in kevent_machport_notify")

    # -- E3: notify_locked dispatches to workq. -----------------------
    print("E3  kevent_machport_notify_locked calls workq_dispatch_kevent (kevent.c)")
    locked = find_function(kev, "kevent_machport_notify_locked")
    e3 = False
    if locked is None:
        print("    NOT FOUND: kevent_machport_notify_locked")
        problems.append("E3: kevent_machport_notify_locked not found")
    else:
        start, end, body = locked
        disp = find_calls(kev, r"workq_dispatch_kevent\s*\(", start, end)
        if disp:
            for (n, t) in disp:
                print("    kevent.c:%d %s" % (n, t))
            e3 = True
            print("    E3 HOLDS")
        else:
            print("    E3 ABSENT: no workq_dispatch_kevent call")

    # -- E4: dispatch holds wq_lock across wq_spawn_worker. -----------
    print("E4  workq_dispatch_kevent holds wq_lock across wq_spawn_worker (workq.c)")
    disp_fn = find_function(wq, "workq_dispatch_kevent")
    e4 = False
    if disp_fn is None:
        print("    NOT FOUND: workq_dispatch_kevent")
        problems.append("E4: workq_dispatch_kevent not found")
    else:
        start, end, body = disp_fn
        acq = find_calls(wq, r"wq_lock_acquire\s*\(", start, end)
        rel = find_calls(wq, r"wq_lock_release\s*\(", start, end)
        spawn = find_calls(wq, r"wq_spawn_worker\s*\(", start, end)
        for (n, t) in acq:
            print("    workq.c:%d wq_lock_acquire" % n)
        for (n, t) in spawn:
            print("    workq.c:%d wq_spawn_worker call" % n)
        for (n, t) in rel:
            print("    workq.c:%d wq_lock_release" % n)
        # The cycle needs a spawn that happens while the lock is
        # held: an acquire before the spawn with no release on the
        # path to it. A release between the acquire and the spawn
        # only breaks the chain if execution continues past it; a
        # release immediately followed by a return is an early exit
        # (the spawn is never reached on that path), so it does not
        # count. This is a control-flow heuristic, not a full CFG
        # walk, and it is documented as such.
        def _is_early_exit(line_no):
            for j in range(line_no, min(line_no + 2, len(wq))):
                stripped = wq[j].strip()
                if not stripped:
                    continue
                return stripped.startswith("return")
            return False

        for (sn, _) in spawn:
            earlier_acq = [n for (n, _) in acq if n < sn]
            if not earlier_acq:
                continue
            acq_line = earlier_acq[-1]
            live_rel = [n for (n, _) in rel
                        if acq_line < n < sn and not _is_early_exit(n)]
            if not live_rel:
                e4 = True
                print("    E4 HOLDS: spawn at workq.c:%d under wq_lock (acquired workq.c:%d)"
                      % (sn, acq_line))
                break
        if not e4:
            print("    E4 ABSENT: no wq_spawn_worker call under wq_lock")

    # -- E5: spawn calls ipc_port_allocate_thread, which takes ipc_lock.
    print("E5  wq_spawn_worker calls ipc_port_allocate_thread -> ipc_lock (workq.c, ipc.c)")
    spawn_fn = find_function(wq, "wq_spawn_worker")
    e5 = False
    if spawn_fn is None:
        print("    NOT FOUND: wq_spawn_worker")
        problems.append("E5: wq_spawn_worker not found")
    else:
        start, end, body = spawn_fn
        calls = find_calls(wq, r"ipc_port_allocate_thread\s*\(", start, end)
        for (n, t) in calls:
            print("    workq.c:%d %s" % (n, t))
        alloc = find_function(ipc, "ipc_port_allocate_thread")
        takes = False
        if alloc is not None:
            a0, a1, abody = alloc
            if re.search(r"\bipc_lock\s*\(\s*\)", abody):
                takes = True
                print("    ipc.c:%d ipc_port_allocate_thread takes ipc_lock()" % (a0 + 1))
        if calls and takes:
            e5 = True
            print("    E5 HOLDS")
        else:
            print("    E5 ABSENT: spawn does not reach ipc_lock")

    # -- E6: opposite order in kevent_apply_changes. ------------------
    print("E6  kevent_apply_changes holds kq_lock while calling ipc_port_* (kevent.c)")
    apply_fn = find_function(kev, "kevent_apply_changes")
    e6 = False
    if apply_fn is None:
        print("    NOT FOUND: kevent_apply_changes")
        problems.append("E6: kevent_apply_changes not found")
    else:
        start, end, body = apply_fn
        acq = find_calls(kev, r"kq_lock_acquire\s*\(\s*\)", start, end)
        ipc_calls = find_calls(kev, r"ipc_port_has_receive_right\s*\(|ipc_port_peek_size\s*\(",
                               start, end)
        for (n, t) in acq:
            print("    kevent.c:%d kq_lock_acquire" % n)
        for (n, t) in ipc_calls:
            print("    kevent.c:%d %s" % (n, t))
        if acq and ipc_calls and acq[0][0] < ipc_calls[0][0]:
            e6 = True
            print("    E6 HOLDS: kq_lock held across ipc_lock-taking calls")
        else:
            print("    E6 ABSENT: no ipc_port_* call under kq_lock")

    print("")
    forward = e2 and e3 and e4 and e5
    # E1 and E0 are the entry conditions for the forward cycle.
    e1_holds = not any(p.startswith("E1") for p in problems)
    e0_holds = not any(p.startswith("E0") for p in problems)
    cycle = forward and e1_holds and e0_holds
    if cycle:
        print("RESULT: forward cycle PRESENT (ipc_lock -> kq_lock -> wq_lock -> ipc_lock)")
        print("  a send whose hook path spawns a worker self-deadlocks: the")
        print("  worker blocks on the non-recursive ipc_lock held by the sender.")
    else:
        print("RESULT: forward cycle ABSENT (at least one edge is gone)")
    if e6:
        print("RESULT: opposite-order leg PRESENT (kq_lock -> ipc_lock in kevent_apply_changes)")
    else:
        print("RESULT: opposite-order leg ABSENT")

    if cycle:
        print("lock_order: FAIL (cycle present)")
        return 1
    if e6:
        print("lock_order: PASS (no cycle; E6 is the consistent order kq_lock -> ipc_lock)")
    else:
        print("lock_order: PASS (no cycle, no opposite leg)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
