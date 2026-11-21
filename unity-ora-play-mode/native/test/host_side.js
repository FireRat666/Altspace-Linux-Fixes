// The browser's side of the loopback test: attaches like Ora's dist/link.js, drains on wake, answers
// the CreateWindow-like request, echoes the page message back as PAGE_MSG, and exits when Unity closes.
'use strict';
const addon = require(process.argv[2]);
const name = process.argv[process.argv.indexOf('--ora-link') + 1];
const info = addon.attach(name);
const host = info.host;
console.log('host: attached', name, 'creator pid', info.creatorPid);
let replied = 0;
addon.bindThread(host);
const drain = () => {
  const [lo, hi] = addon.beginWake(host);
  for (let slot = 0; slot < 64; slot++) {
    const bit = slot < 32 ? (lo >>> slot) & 1 : (hi >>> (slot - 32)) & 1;
    if (!bit) continue;
    const ci = addon.channelInfo(host, slot);
    if (!ci) continue;
    for (;;) {
      const recs = addon.take(host, ci.id, 64);
      if (typeof recs === 'number' || recs.length === 0) break;
      for (const r of recs) {
        if (r.op === 0x20) {
          addon.setViewState(host, r.a0, { flags: 1, bufferGen: 1, texW: 1024, texH: 768, paintSeq: 0, loadState: 1 });
          if (addon.writeText(host, ci.id, 0x20, 1, 0, r.reqId, 0, 0, 0, 'window-ok') !== 0) throw new Error('reply failed');
          replied++;
        } else if (r.op === 0x34) {
          if (r.s0 !== 'abc') throw new Error('bad s0 ' + r.s0);
          const st = addon.writeText2(host, ci.id, 0x60, 2, 2, 0, 0, r.s0, r.s1);
          if (st !== 0 && st !== 1) throw new Error('echo failed ' + st);
          replied++;
        }
      }
    }
  }
  if (addon.arm(host, 0, 0) !== 1) setImmediate(drain);
};
const st = addon.startWaiter(host, () => setImmediate(drain), () => {
  console.log('host: Unity closed the link after', replied, 'replies');
  addon.detach(host, 4);
  process.exit(replied === 2 ? 0 : 3);
});
if (st !== 0) throw new Error('waiter ' + st);
const keep = setInterval(() => { addon.beat(host, 1); addon.flushSpill(host, 256); }, 250);
setImmediate(drain);
setTimeout(() => { console.error('host: timed out'); process.exit(4); }, 20000);
void keep;
