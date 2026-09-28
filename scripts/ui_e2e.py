#!/usr/bin/env python3
"""End-to-end test of the browser UI in a real headless browser.

Loads ui/index.html (the real C++ engine compiled to WebAssembly), drives it programmatically (sends orders,
crossing trades, the -50% and +1 tick modify buttons, hover queue tooltips, post-only rejection, random flow,
reset, the benchmark button, tab switching, and that every figure on the real-data tab loads), then reads the
result back out of the DOM.

    python scripts/ui_e2e.py            # needs Chrome, Chromium or Edge (or set CHROME=/path/to/browser)

The browser runs in real time and the result is read over the DevTools protocol once the page reports it. An
earlier version used --virtual-time-budget and --dump-dom; the virtual clock could run the page's timers ahead of
the engine's WebAssembly compilation, which made the test flaky on CI.
"""
import base64, json, os, re, shutil, socket, struct, subprocess, sys, tempfile, time, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

test = r'''
<pre id="e2e" style="white-space:pre-wrap"></pre>
<script>
const out = []; const ok = (c, m) => out.push((c ? "PASS " : "FAIL ") + m);
window.addEventListener("error", e => out.push("FAIL uncaught error: " + e.message));
function click(id) { document.getElementById(id).click(); }
function wait(ms) { return new Promise(r => setTimeout(r, ms)); }
(async () => {
  for (let i = 0; i < 600 && !book; i++) await wait(50);  // real time: up to 30 s for the WebAssembly engine
  ok(!!book, "engine loaded");
  ok(document.getElementById("engine-badge").textContent.includes("WebAssembly"), "badge says WebAssembly");
  ok(document.querySelectorAll("#asks .row").length === 6 && document.querySelectorAll("#bids .row").length === 6, "seeded ladder shows 6+6 levels");
  ok(document.getElementById("k-spread").textContent === "2", "spread is 2 ticks, got " + document.getElementById("k-spread").textContent);
  // limit buy that crosses: 1001 buy 150 should hit the 100 @1001 ask and rest 50
  document.getElementById("f-price").value = 1001; document.getElementById("f-qty").value = 150;
  click("b-buy"); await wait(50);
  ok(document.getElementById("k-trades").textContent === "1", "one trade after crossing buy");
  ok(/1 order|filled immediately/.test(document.getElementById("msg").textContent) && document.getElementById("msg").textContent.includes("100 lots filled"), "message reports 100 filled: " + document.getElementById("msg").textContent);
  ok(document.querySelectorAll("#mine .o").length === 1, "one resting order of mine listed");
  // modify buttons
  document.querySelector('#mine button[data-act="half"]').click(); await wait(20);
  ok(/25 @ 1001|#\d+ buy 25 @ 1001/.test(document.getElementById("mine").textContent), "-50% halves my order: " + document.getElementById("mine").textContent.trim());
  document.querySelector('#mine button[data-act="tick"]').click(); await wait(20);
  ok(document.querySelectorAll("#mine .o").length === 0 && book.trades === 2, "+1 tick reprices onto the ask at 1002 and trades immediately (trades=" + book.trades + ")");
  // hover shows the queue
  const row = document.querySelector('#bids .row'); row.dispatchEvent(new MouseEvent("mouseover", { bubbles: true }));
  ok(row.title.includes("#") , "hover tooltip lists the queue: " + JSON.stringify(row.title));
  // market order and post-only reject
  document.getElementById("f-type").value = "market"; document.getElementById("f-type").onchange(); document.getElementById("f-qty").value = 10; click("b-sell"); await wait(20);
  ok(document.getElementById("msg").textContent.includes("filled immediately"), "market sell executes");
  document.getElementById("f-type").value = "limit"; document.getElementById("f-type").onchange();
  document.getElementById("f-post").checked = true; document.getElementById("f-price").value = 1100; document.getElementById("f-qty").value = 10; click("b-buy"); await wait(20);
  ok(document.getElementById("msg").textContent.includes("post-only"), "post-only crossing order rejected: " + document.getElementById("msg").textContent);
  // random flow ticks and invariants
  click("b-flow"); await wait(1500); click("b-flow");
  ok(book.orderCount() > 10 && book.trades > 1, "random flow produced trades, orders=" + book.orderCount() + " trades=" + book.trades);
  click("b-reset"); await wait(50);
  ok(document.getElementById("k-trades").textContent === "0", "reset clears the trade counter");
  click("b-bench"); await wait(4000);
  const b = document.getElementById("bench-out").textContent; ok(/M orders\/s/.test(b), "benchmark button ran: " + b);
  document.querySelector('.tab[data-tab="results"]').click();
  ok(document.querySelectorAll("#ch-thr .chart-row").length === 5, "results charts rendered");
  document.querySelector('.tab[data-tab="real"]').click(); await wait(200);
  ok(!document.getElementById("tab-real").classList.contains("hidden") && document.getElementById("tab-book").classList.contains("hidden"), "real-data tab shows and hides the others");
  ok(document.querySelectorAll("#tab-real .fig img").length === 6, "six figures present on the real-data tab");
  const imgs = [...document.querySelectorAll("#tab-real .fig img")];
  await Promise.all(imgs.map(i => i.complete ? 0 : new Promise(r => { i.onload = r; i.onerror = r; })));
  ok(imgs.every(i => i.naturalWidth > 100), "all six figures load (paths resolve): " + imgs.map(i => i.naturalWidth).join(","));
  document.getElementById("e2e").textContent = out.join("\n") + "\n" + (out.some(l => l.startsWith("FAIL")) ? "RESULT: FAIL" : "RESULT: ALL PASS");
})().catch(e => {  // an exception must be reported, never leave an empty result
  document.getElementById("e2e").textContent = out.join("\n") + "\nFAIL exception: " + e + "\nRESULT: FAIL";
});
</script>
'''


def find_browser():
    if os.environ.get("CHROME"):
        return os.environ["CHROME"]
    for name in ("google-chrome", "chromium", "chromium-browser", "chrome", "msedge"):
        p = shutil.which(name)
        if p:
            return p
    for p in (r"C:/Program Files/Google/Chrome/Application/chrome.exe",
              r"C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
              "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"):
        if os.path.exists(p):
            return p
    sys.exit("no browser found; set CHROME=/path/to/chrome")


class DevTools:
    """A minimal Chrome DevTools Protocol client over a WebSocket, standard library only (CI's system Python cannot
    pip-install). It only needs to send Runtime.evaluate and read the matching reply."""

    def __init__(self, ws_url):
        m = re.match(r"ws://([^:/]+):(\d+)(/.*)", ws_url)
        self.sock = socket.create_connection((m.group(1), int(m.group(2))), timeout=30)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET {m.group(3)} HTTP/1.1\r\nHost: {m.group(1)}:{m.group(2)}\r\nUpgrade: websocket\r\n"
                           f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            head += self.sock.recv(1)
        if b" 101 " not in head.split(b"\r\n")[0]:
            raise RuntimeError("DevTools handshake failed: " + head.decode(errors="replace"))
        self.next_id = 0

    def _recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise RuntimeError("DevTools connection closed")
            buf += chunk
        return buf

    def _recv_message(self):
        data = b""
        while True:
            b0, b1 = self._recv_exact(2)
            n = b1 & 0x7F
            if n == 126:
                n = struct.unpack(">H", self._recv_exact(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._recv_exact(8))[0]
            data += self._recv_exact(n)
            if b0 & 0x80:  # final fragment
                return data.decode()

    def evaluate(self, expression):
        self.next_id += 1
        payload = json.dumps({"id": self.next_id, "method": "Runtime.evaluate",
                              "params": {"expression": expression, "returnByValue": True}}).encode()
        mask = os.urandom(4)
        header = bytes([0x81]) + (bytes([0x80 | len(payload)]) if len(payload) < 126
                                  else bytes([0x80 | 126]) + struct.pack(">H", len(payload)))
        self.sock.sendall(header + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(payload)))
        while True:
            msg = json.loads(self._recv_message())
            if msg.get("id") == self.next_id:
                return msg.get("result", {}).get("result", {}).get("value")


def main():
    tmp = tempfile.mkdtemp()
    proc = None
    try:
        shutil.copytree(os.path.join(ROOT, "ui"), os.path.join(tmp, "ui"))
        shutil.copytree(os.path.join(ROOT, "docs"), os.path.join(tmp, "docs"))
        page = os.path.join(tmp, "ui", "index.html")
        html = open(page, encoding="utf8").read().replace("</body>", test + "</body>")
        open(page, "w", encoding="utf8").write(html)
        url = "file:///" + page.replace(os.sep, "/").lstrip("/")
        profile = os.path.join(tmp, "profile")
        # Real time, not --virtual-time-budget: a virtual clock can run the page's timers ahead of work that is not
        # timer-driven (compiling the WebAssembly engine), which made this test flaky on CI.
        cmd = [find_browser(), "--headless=new", "--disable-gpu", "--no-sandbox", "--allow-file-access-from-files",
               "--remote-debugging-port=0", f"--user-data-dir={profile}", "--no-first-run", url]
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        port_file = os.path.join(profile, "DevToolsActivePort")
        deadline = time.time() + 30
        while not (os.path.exists(port_file) and open(port_file).read().strip()):
            if time.time() > deadline:
                sys.exit("browser did not start its DevTools endpoint")
            time.sleep(0.1)
        port = int(open(port_file).read().split()[0])
        targets = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json/list", timeout=10))
        target = next(t for t in targets if t.get("type") == "page")
        dt = DevTools(target["webSocketDebuggerUrl"])
        text, deadline = "", time.time() + 120
        while "RESULT:" not in text and time.time() < deadline:
            time.sleep(0.25)
            text = dt.evaluate("(document.getElementById('e2e') || {}).textContent || ''") or ""
        if "RESULT:" not in text:
            sys.exit("test did not finish within 120 s; partial output:\n" + text)
        print(text)
        sys.exit(0 if "RESULT: ALL PASS" in text else 1)
    finally:
        if proc is not None:
            proc.kill()
            proc.wait()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
