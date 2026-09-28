// Smoke test of the WebAssembly build, driven the way the browser UI drives it.
//   node wasm/test_wasm.js        (after scripts/build_wasm.sh)
"use strict";
const path = require("path");
const createEngine = require(path.join(__dirname, "..", "ui", "engine.js"));

const REASON = ["none", "invalid id", "invalid qty", "invalid price", "duplicate id", "unknown order", "unknown symbol",
  "would cross", "book full", "user cancel", "IOC expired", "FOK unfilled", "self trade"];
let failures = 0, checks = 0;
function check(cond, msg) { checks++; if (!cond) { failures++; console.error("FAIL:", msg); } }

createEngine().then(M => {
  const submit = M.cwrap("ex_submit", "number", ["number", "number", "number", "number", "number", "number", "number", "number", "number"]);
  const evPtr = M.cwrap("ex_events", "number", []), evSize = M.cwrap("ex_event_size", "number", []);
  const init = M.cwrap("ex_init", null, []);
  const bestBid = M.cwrap("ex_best_bid", "number", []), bestAsk = M.cwrap("ex_best_ask", "number", []);
  const depth = M.cwrap("ex_depth", "number", ["number", "number"]), depthPtr = M.cwrap("ex_depth_ptr", "number", []);
  const queue = M.cwrap("ex_queue", "number", ["number", "number"]), queuePtr = M.cwrap("ex_queue_ptr", "number", []);
  const inv = M.cwrap("ex_check_invariants", "number", []);
  const bench = M.cwrap("ex_bench", "number", ["number", "number"]);

  function events(n) {
    const out = [], base = evPtr(), sz = evSize(), dv = new DataView(M.HEAPU8.buffer);
    for (let i = 0; i < n; i++) {
      const o = base + i * sz;
      out.push({ id: Number(dv.getBigUint64(o, true)), maker: Number(dv.getBigUint64(o + 8, true)), price: Number(dv.getBigInt64(o + 16, true)),
                 qty: Number(dv.getBigUint64(o + 24, true)), leaves: Number(dv.getBigUint64(o + 32, true)), type: dv.getUint8(o + 44), reason: dv.getUint8(o + 45), side: dv.getUint8(o + 46) });
    }
    return out;
  }
  const NEW = 1, CANCEL = 2, MODIFY = 3, BUY = 0, SELL = 1, LIMIT = 0, MARKET = 1, DAY = 0, IOC = 1, FOK = 2;
  const send = (type, id, side, ot, tif, post, px, qty, owner = 1) => events(submit(type, id, side, ot, tif, post, px, qty, owner));

  init();
  check(evSize() === 48, "Event is 48 bytes");

  // price-time priority: the same scenario the C++ tests use
  send(NEW, 1, SELL, LIMIT, DAY, 0, 1005, 5);
  send(NEW, 2, SELL, LIMIT, DAY, 0, 1005, 5);
  send(NEW, 3, SELL, LIMIT, DAY, 0, 1004, 5);
  const ev = send(NEW, 4, BUY, LIMIT, DAY, 0, 1005, 12);
  const trades = ev.filter(e => e.type === 3).map(e => `${e.price}x${e.qty}#${e.maker}`);
  check(trades.join(" ") === "1004x5#3 1005x5#1 1005x2#2", "priority order, got " + trades.join(" "));
  check(bestAsk() === 1005 && bestBid() === -1, "book state after sweep");
  check(depth(SELL, 5) === 1, "one ask level");
  const dv = new DataView(M.HEAPU8.buffer), d0 = depthPtr();
  check(dv.getInt32(d0, true) === 1005 && dv.getUint32(d0 + 4, true) === 3, "depth level 1005 with 3 lots");
  check(queue(SELL, 1005) === 1 && Number(dv.getBigUint64(queuePtr(), true)) === 2, "queue holds order 2");

  // modify: shrinking keeps priority, repricing loses it and can trade
  send(NEW, 10, SELL, LIMIT, DAY, 0, 1010, 10);
  send(NEW, 11, SELL, LIMIT, DAY, 0, 1010, 10);
  send(MODIFY, 10, SELL, LIMIT, DAY, 0, 1010, 4);
  check(Number(new DataView(M.HEAPU8.buffer).getBigUint64(queue(SELL, 1010) && queuePtr(), true)) === 10, "shrunk order keeps first place");
  send(MODIFY, 10, SELL, LIMIT, DAY, 0, 1010, 40);
  const q = queue(SELL, 1010), qd = new DataView(M.HEAPU8.buffer);
  check(q === 2 && Number(qd.getBigUint64(queuePtr(), true)) === 11, "size increase loses priority");

  // order types
  init();
  send(NEW, 1, SELL, LIMIT, DAY, 0, 1005, 5);
  let e = send(NEW, 2, BUY, LIMIT, IOC, 0, 1005, 8);
  check(e.some(x => x.type === 4 && REASON[x.reason] === "IOC expired" && x.qty === 3), "IOC remainder cancelled");
  e = send(NEW, 3, BUY, LIMIT, FOK, 0, 1005, 1);
  check(e.some(x => x.type === 4 && REASON[x.reason] === "FOK unfilled"), "FOK against empty book cancels");
  send(NEW, 4, SELL, LIMIT, DAY, 0, 1006, 5);
  e = send(NEW, 5, BUY, LIMIT, DAY, 1, 1006, 1);
  check(e[0].type === 2 && REASON[e[0].reason] === "would cross", "post-only rejected when it would trade");
  e = send(NEW, 6, BUY, MARKET, DAY, 0, 0, 50);
  check(e.filter(x => x.type === 3).length === 1 && e.some(x => x.type === 4), "market order sweeps then expires");
  e = send(CANCEL, 999, BUY, LIMIT, DAY, 0, 0, 0);
  check(e[0].type === 2 && REASON[e[0].reason] === "unknown order", "cancel of unknown id rejected");

  // randomized: structural invariants hold after many operations
  init();
  let id = 1;
  const rnd = (n) => Math.floor(Math.random() * n);
  for (let i = 0; i < 20000; i++) {
    if (Math.random() < 0.3) submit(CANCEL, 1 + rnd(id), BUY, LIMIT, DAY, 0, 0, 0, 1);
    else submit(NEW, id++, rnd(2), LIMIT, rnd(3), 0, 990 + rnd(21), 1 + rnd(20), 1);
  }
  check(inv() === 1, "book invariants hold after 20k random operations");

  const rate = bench(500000, 42);
  console.log(`wasm engine, 500k orders: ${(rate / 1e6).toFixed(1)} M orders/s in Node`);
  console.log(`${checks - failures}/${checks} checks passed`);
  process.exit(failures ? 1 : 0);
});
