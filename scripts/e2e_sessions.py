#!/usr/bin/env python3
"""Multi-session end-to-end test of the gateway's order-id handling (include/exsim/client_ids.hpp).

    python3 scripts/e2e_sessions.py <build-dir>

S1. Two sessions trade with each other. Both use client order id 7. The taker's fill report shows its own
    id and an anonymous counterparty (0). The maker receives an unsolicited fill report (seq 0) naming its
    own order. Each session can then cancel only its own order 7.
S2. Hash flooding. One session rests 40,000 orders whose ids all collide in the engine's order index (the
    locality hash folds id bits, and these ids fold to the same slot), then cancels them. The run is timed
    twice: with exchange-assigned ids (the default) and with --trust-client-ids, which passes client ids
    straight to the engine. It also runs with plain sequential ids as the baseline. The time compared is the
    server's CPU time, not this client's wall-clock.
"""
import os, socket, struct, subprocess, sys, time

HDR = struct.Struct("<HBBIQ")                        # length, type, version, symbol, seq
NEW = struct.Struct("<HBBIQ QqQI BBBB")              # 48 bytes
CANCEL = struct.Struct("<HBBIQ Q")                   # 24 bytes
REPORT = struct.Struct("<HBBIQ QQqQQ BBBB I")        # 64 bytes
ACCEPTED, REJECTED, TRADE, CANCELED, MODIFIED, DONE = 1, 2, 3, 4, 5, 0xFF


def new_order(seq, oid, side, px, qty, sym=0):
    return NEW.pack(NEW.size, 1, 1, sym, seq, oid, px, qty, 0, side, 0, 0, 0)


def cancel(seq, oid, sym=0):
    return CANCEL.pack(CANCEL.size, 2, 1, sym, seq, oid)


class Session:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port))
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""
        self.unsolicited = []

    def reports(self, n_done, timeout=30.0):
        """Reads until n_done Done markers; returns [(seq, type, order_id, maker_id, px, qty, leaves, reason)]."""
        out, done, end, off = [], 0, time.time() + timeout, 0
        self.s.settimeout(timeout)
        while done < n_done:
            if len(self.buf) - off < REPORT.size:
                self.buf = self.buf[off:]
                off = 0
                while len(self.buf) < REPORT.size:
                    chunk = self.s.recv(1 << 20)
                    if not chunk or time.time() > end:
                        raise RuntimeError("connection closed or timed out")
                    self.buf += chunk
            f = REPORT.unpack_from(self.buf, off)
            off += REPORT.size
            seq, etype = f[4], f[10]
            r = (seq, etype, f[5], f[6], f[7], f[8], f[9], f[11])
            if etype == DONE:
                done += 1
            elif seq == 0:
                self.unsolicited.append(r)
            else:
                out.append(r)
        self.buf = self.buf[off:]
        return out

    def poll_unsolicited(self, wait=0.3):
        self.s.settimeout(wait)
        try:
            self.buf += self.s.recv(1 << 20)
        except socket.timeout:
            pass
        while len(self.buf) >= REPORT.size:
            f = REPORT.unpack_from(self.buf)
            self.buf = self.buf[REPORT.size:]
            self.unsolicited.append((f[4], f[10], f[5], f[6], f[7], f[8], f[9], f[11]))
        return self.unsolicited


def check(cond, what):
    print(("OK   " if cond else "FAIL ") + what)
    if not cond:
        sys.exit(1)


def start_server(build, port, journal, *extra):
    p = subprocess.Popen([os.path.join(build, "exsim_server"), "--port", str(port), "--journal", journal, *extra],
                         stdout=subprocess.PIPE, text=True)
    while "READY" not in p.stdout.readline():
        pass
    return p


def s1(build, port, journal):
    srv = start_server(build, port, journal)
    try:
        a, b = Session(port), Session(port)
        a.s.sendall(new_order(1, 7, 1, 100, 5))                    # A: sell 5 @ 100, client id 7
        r = a.reports(1)
        check(r[0][1] == ACCEPTED and r[0][2] == 7, "maker's order accepted under its own client id 7")
        b.s.sendall(new_order(1, 7, 0, 100, 3))                    # B: buy 3 @ 100, also client id 7
        r = b.reports(1)
        trade = [x for x in r if x[1] == TRADE]
        check(len(trade) == 1 and trade[0][2] == 7 and trade[0][3] == 0 and trade[0][5] == 3 and trade[0][6] == 2,
              "taker's fill report: its own id 7, counterparty anonymous (0), 3 filled, maker leaves 2")
        u = [x for x in a.poll_unsolicited() if x[1] == TRADE]
        check(len(u) == 1 and u[0][0] == 0 and u[0][2] == 0 and u[0][3] == 7 and u[0][5] == 3 and u[0][6] == 2,
              "maker received an unsolicited fill (seq 0) naming its own order 7, counterparty anonymous")
        b.s.sendall(cancel(2, 7))
        r = b.reports(1)
        check(r[0][1] == REJECTED and r[0][2] == 7, "taker cannot cancel id 7: its order 7 is filled; the maker's 7 is not its own")
        a.s.sendall(cancel(2, 7))
        r = a.reports(1)
        check(r[0][1] == CANCELED and r[0][2] == 7 and r[0][5] == 2, "maker cancels its own order 7 (2 left)")
        a.s.sendall(new_order(3, 7, 0, 90, 1))
        r = a.reports(1)
        check(r[0][1] == ACCEPTED and r[0][2] == 7, "a client id is reusable once its order is gone")
    finally:
        srv.terminate()
        srv.wait()


def flood(build, port, journal, ids, *extra):
    srv = start_server(build, port, journal, *extra)
    try:
        c = Session(port)
        msgs = b"".join(new_order(i + 1, oid, 0, 1000 + (i % 50), 1) for i, oid in enumerate(ids))
        msgs += b"".join(cancel(len(ids) + i + 1, oid) for i, oid in enumerate(ids))
        c.s.sendall(msgs)
        r = c.reports(2 * len(ids), timeout=600)
        ok = sum(1 for x in r if x[1] == ACCEPTED) == len(ids) and sum(1 for x in r if x[1] == CANCELED) == len(ids)
    finally:
        srv.terminate()
        out = srv.communicate()[0]
    # The server's CPU time (user + system). This client's wall-clock is dominated by Python and by whatever
    # else the machine is doing; CPU time does not count time the server spent descheduled.
    return float(out.split("cpu_s=")[1].split()[0]), ok


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else "build/release"
    port = 20000 + os.getpid() % 20000
    journal = f"/tmp/exsim_sessions_{os.getpid()}.bin"
    print("=== S1. two sessions, one client id ===")
    s1(build, port, journal)

    print("\n=== S2. hash flooding through the gateway ===")
    n, bits = 40000, 19  # default book capacity 2^18 orders -> 2^19 index slots
    colliding = [(x << bits) | x for x in range(1, n + 1)]  # fold: low bits ^ next bits = 0 for every id
    sequential = list(range(1, n + 1))
    t_seq, ok1 = flood(build, port + 1, journal, sequential)
    t_def, ok2 = flood(build, port + 2, journal, colliding)
    t_raw, ok3 = flood(build, port + 3, journal, colliding, "--trust-client-ids")
    print(f"server CPU time for {n} rests + {n} cancels: sequential ids {t_seq:.3f} s; colliding ids {t_def:.3f} s with "
          f"exchange-assigned ids, {t_raw:.3f} s when client ids reach the engine ({t_raw / t_def:.1f}x)")
    check(ok1 and ok2 and ok3, "every order accepted and cancelled in all three runs")
    check(t_def < 1.5 * t_seq + 0.05, "colliding client ids cost nothing once the gateway assigns exchange ids")
    check(t_raw > 3 * t_def, "the attack is real when client ids reach the engine (so the check above means something)")
    os.remove(journal)
    print("\nALL SESSION CHECKS PASSED")


if __name__ == "__main__":
    main()
