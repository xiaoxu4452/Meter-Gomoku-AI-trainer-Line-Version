#!/usr/bin/env node
/* call-layer.js - protocol-level latency / memory / efficiency probes.
 * Same harness for our engine and the reference engine (Rapfi 0.43.01).
 * Measures: spawn->first output, search command->first INFO DEPTH, STOP->bestmove,
 * RSS growth during a 20 s search, and reported nodes vs engine-reported time.
 */
'use strict';
const cp = require('child_process');
const path = require('path');

const args = process.argv.slice(2);
const argOf = (n, d) => { const i = args.indexOf('--' + n); return i >= 0 ? args[i + 1] : d; };
const OUT = argOf('out', '');
const THREADS = +argOf('threads', 8);
const HASHKB = +argOf('hashkb', 524288);
const TRIALS = +argOf('trials', 5);

const POS = [[7, 7], [8, 8], [8, 7], [6, 7], [9, 6]];
const VARIANTS = (argOf('variants', '') || '').split(',').map(s => s.trim()).filter(Boolean).map(s => {
  const eq = s.indexOf('=');
  return { name: s.slice(0, eq), exe: path.resolve(s.slice(eq + 1)) };
});

const now = () => Number(process.hrtime.bigint()) / 1e6;
const sleep = ms => new Promise(r => setTimeout(r, ms));
const median = a => { const b = a.slice().sort((x, y) => x - y); const n = b.length; return n ? (n % 2 ? b[(n - 1) / 2] : (b[n / 2 - 1] + b[n / 2]) / 2) : 0; };

function rss(pid) {
  try {
    const out = cp.execFileSync('powershell', ['-NoProfile', '-Command',
      `(Get-Process -Id ${pid} -ErrorAction SilentlyContinue).WorkingSet64`], { encoding: 'utf8', windowsHide: true });
    const v = parseInt(out.trim(), 10);
    return Number.isFinite(v) ? v : 0;
  } catch (e) { return 0; }
}

function boardCmds(turnMs) {
  return [
    'START 15', 'INFO RULE 0', 'INFO SHOW_DETAIL 2',
    'INFO THREAD_NUM ' + THREADS, 'INFO HASH_SIZE ' + HASHKB,
    'INFO PONDERING 0', 'INFO TIMEOUT_MATCH 6000000', 'INFO TIME_LEFT 6000000',
    'INFO TIMEOUT_TURN ' + turnMs,
    'YXBOARD ' + POS.map((m, i) => m[0] + ',' + m[1] + ',' + (i % 2 === 0 ? 1 : 2)).join(' ') + ' DONE',
    'YXNBEST 1',
  ].join('\n') + '\n';
}

function spawnEngine(v) {
  const p = cp.spawn(v.exe, [], { cwd: path.dirname(v.exe), stdio: ['pipe', 'pipe', 'ignore'], windowsHide: true });
  const st = { p, firstOut: 0, firstInfo: 0, best: 0, nodes: 0, speed: 0, etime: 0, buf: '', t0: now(), depth: 0 };
  p.stdout.on('data', ch => {
    st.buf += ch.toString();
    let i;
    while ((i = st.buf.indexOf('\n')) >= 0) {
      const ln = st.buf.slice(0, i).replace(/\r$/, '');
      st.buf = st.buf.slice(i + 1);
      if (!ln) continue;
      if (!st.firstOut) st.firstOut = now() - st.t0;
      if (!st.firstInfo && (ln.startsWith('INFO DEPTH') || ln.startsWith('INFO NODES'))) st.firstInfo = now() - st.t0;
      if (!/^\d+,\d+/i.test(ln.trim()) && !ln.includes('INFO') && !ln.includes('MESSAGE')) { /* prompt */ }
      if (/^\d+,\d+(\s+[0-9]+,[0-9]+)?$/.test(ln.trim()) && !st.best) st.best = now() - st.tStopAt;
      const mn = /Node(?:s)?\s+(\d+)K/.exec(ln); if (mn) st.nodes = +mn[1] * 1000;
      const tn = /^INFO TOTALNODES (\d+)/.exec(ln); if (tn) st.nodes = +tn[1];
      const ms = /Speed\s+(\d+)K/.exec(ln); if (ms) st.speed = +ms[1] * 1000;
      const mt = /Time\s+(\d+)ms/.exec(ln); if (mt) st.etime = +mt[1];
      const md = /Depth\s+(\d+)/.exec(ln); if (md) st.depth = +md[1];
      if (ln.startsWith('INFO DEPTH')) { const dm = /(\d+)\s*[-/]\s*(\d+)/.exec(ln.slice(11)); if (dm) st.depth = Math.max(st.depth, +dm[1]); }
    }
  });
  p.on('error', () => {});
  return st;
}

async function latencyTrial(v, waitMs) {
  const st = spawnEngine(v);
  await sleep(120);                       // let the process start exec
  st.t0 = now();
  st.p.stdin.write(boardCmds(20000));
  const tSearchStart = st.t0;
  // wait for first INFO
  while (now() - tSearchStart < 6000 && !st.firstInfo) await sleep(10);
  // keep searching, then STOP
  await sleep(waitMs);
  const tStop = now();
  st.tStopAt = tStop;
  st.p.stdin.write('STOP\n');
  while (now() - tStop < 6000 && !st.best) await sleep(5);
  const res = { name: v.name, firstOut: st.firstOut, firstInfo: st.firstInfo, best: st.best, depth: st.depth, nodes: st.nodes, speed: st.speed, etime: st.etime };
  try { st.p.kill(); } catch (e) {}
  await sleep(150);
  return res;
}

async function memoryRun(v) {
  const st = spawnEngine(v);
  await sleep(150);
  st.t0 = now();
  st.p.stdin.write(boardCmds(60000));
  const samples = [];
  for (const at of [1000, 5000, 10000, 20000, 30000]) {
    await sleep(at - (now() - st.t0));
    samples.push({ at, rss: rss(st.p.pid), depth: st.depth, nodes: st.nodes, speed: st.speed, etime: st.etime });
  }
  const final = { name: v.name, samples, nodes: st.nodes, speed: st.speed, etime: st.etime, wall: now() - st.t0 };
  try { st.p.kill(); } catch (e) {}
  await sleep(150);
  return final;
}

(async () => {
  const lat = [];
  for (const v of VARIANTS) {
    for (let i = 0; i < TRIALS; i++) {
      const r = await latencyTrial(v, 2000);
      r.trial = i;
      lat.push(r);
      console.log(`${v.name} t${i}: init=${Math.round(r.firstOut)}ms firstInfo=${Math.round(r.firstInfo)}ms stop=${Math.round(r.best)}ms d=${r.depth} nodes=${(r.nodes / 1e6).toFixed(2)}M`);
    }
  }
  const mem = [];
  for (const v of VARIANTS) {
    const r = await memoryRun(v);
    mem.push(r);
    console.log(`${v.name} mem: ` + r.samples.map(s => `${s.at / 1000}s=${Math.round(s.rss / 1048576)}MB`).join(' ') + ` nodes=${(r.nodes / 1e6).toFixed(2)}M etime=${Math.round(r.etime)}ms wall=${Math.round(r.wall)}ms`);
  }
  const summary = {};
  for (const v of VARIANTS) {
    const a = lat.filter(x => x.name === v.name);
    summary[v.name] = {
      initMs: median(a.map(x => x.firstOut || 0)),
      firstInfoMs: median(a.map(x => x.firstInfo || 0)),
      stopMs: median(a.map(x => x.best || 0)),
      depth: median(a.map(x => x.depth)),
    };
  }
  console.log('\n== medians ==');
  for (const k of Object.keys(summary)) console.log(k.padEnd(20), JSON.stringify(summary[k]));
  if (OUT) {
    require('fs').mkdirSync(path.dirname(OUT), { recursive: true });
    require('fs').writeFileSync(OUT, JSON.stringify({ threads: THREADS, hashKB: HASHKB, variants: VARIANTS, lat, mem, summary }, null, 1));
    console.log('wrote ' + OUT);
  }
  process.exit(0);
})();
