#!/usr/bin/env node
/* engine-ab.js -- interleaved A/B for fixed-time Rapfi searches.
 *
 * Why: Lazy SMP makes single runs noisy (+-1 depth, +-10% nps). This driver
 * interleaves variants per repetition (same position, same rep, back-to-back)
 * so both candidates see the same thermal/scheduler state, then reports medians.
 *
 * Protocol matches the desktop host: SHOW_DETAIL 2 (app default), one engine
 * process per sample, fixed TIMEOUT_TURN.
 *
 * usage:
 *   node tools/engine-ab.js --turn 10000 --reps 3 --pos 0,1,2 \
 *        --variants "pre=Meter Gomoku/rapfi-native/RapfiEngine-avx512vnni.exe,new=<path>" \
 *        --out _engine_bench/ab-10s.json
 */
'use strict';
const path = require('path');
const cp = require('child_process');
const fs = require('fs');

const args = process.argv.slice(2);
const argOf = (n, d) => { const i = args.indexOf('--' + n); return i >= 0 ? args[i + 1] : d; };
const TURN = +argOf('turn', 10000);
const REPS = +argOf('reps', 3);
const THREADS = +argOf('threads', 8);
const HASHKB = +argOf('hashkb', 1536 * 1024);
const DETAIL = +argOf('detail', 2);
const OUT = argOf('out', '');
const POSPICK = argOf('pos', '0,1,2,3,4,5').split(',').map(Number);
const MAXDEPTH = +argOf('maxdepth', 0);   // >0: fixed-depth search (deterministic eval/bestmove drift check)
const TOPN = +argOf('topn', 1);           // >1: multi-PV search (analysis-point count)
const RULE = +argOf('rule', 0);           // 0: freestyle, 2: renju (must match the reference config when comparing)
const CAUTION = +argOf('caution', -1);    // >=0: send INFO CAUTION_FACTOR before START

const POS = [
  [[7, 7], [8, 8], [8, 7], [6, 7], [9, 6]],
  [[7, 7], [7, 8], [8, 8], [6, 7], [8, 7], [6, 8], [9, 9]],
  [[7, 7], [8, 8], [8, 7], [6, 7], [9, 6], [7, 6], [6, 6], [9, 7], [5, 8]],
  [[5, 5], [9, 9], [5, 9], [9, 5], [7, 7], [6, 6], [8, 8], [4, 4], [10, 10], [3, 3], [11, 11]],
  [[7, 7], [8, 8], [9, 9], [6, 6], [10, 10], [5, 5], [8, 7], [7, 8], [9, 7]],
  [[7, 7], [7, 6], [7, 8], [6, 7], [8, 7], [6, 6], [8, 8], [5, 5], [9, 9], [6, 8]],
];

const VARIANTS = (argOf('variants', '') || '').split(',').map(s => s.trim()).filter(Boolean).map(s => {
  const eq = s.indexOf('=');
  let name = s.slice(0, eq);
  let rest = s.slice(eq + 1);
  let env = null;
  // Optional per-variant environment: name=exe|KEY=VAL;KEY2=VAL2
  const bar = rest.indexOf('|');
  if (bar >= 0) {
    env = {};
    rest.slice(bar + 1).split(';').forEach(kv => {
      const k = kv.slice(0, kv.indexOf('='));
      const v = kv.slice(kv.indexOf('=') + 1);
      if (k) env[k.trim()] = v.trim();
    });
    rest = rest.slice(0, bar);
  }
  return { name, exe: path.resolve(rest), env,
           hashkb: env && env.HASHKB ? +env.HASHKB : 0,
           threads: env && env.THREADS ? +env.THREADS : 0 };
});
if (!VARIANTS.length) { console.error('usage: --variants "name=exe,path2=exe2"'); process.exit(1); }

const median = a => { const b = a.slice().sort((x, y) => x - y); const n = b.length; return n ? b[(n - 1) >> 1] : 0; };
const rmedian = a => { const b = a.slice().sort((x, y) => x - y); const n = b.length; return n ? (n % 2 ? b[(n - 1) / 2] : (b[n / 2 - 1] + b[n / 2]) / 2) : 0; };

function once(v, pts) {
  return new Promise((resolve) => {
    const cwd = path.dirname(v.exe);
    const p = cp.spawn(v.exe, [], {
      cwd,
      stdio: ['pipe', 'pipe', 'ignore'],
      windowsHide: true,
      env: v.env ? Object.assign({}, process.env, v.env) : process.env,
    });
    let buf = '', d = 0, sd = 0, nodes = 0, speed = 0, ttime = 0, ev = '', best = null;
    let done = false;
    const t0 = Date.now();
    const fin = () => {
      if (done) return; done = true;
      try { p.kill(); } catch (e) {}
      setTimeout(() => resolve({ variant: v.name, pos: pts.length, d, sd, nodes, speed, ttime, ev, best, wall: Date.now() - t0 }), 120);
    };
    p.on('error', fin);
    p.on('exit', () => { if (!best) setTimeout(fin, 30); });
    p.stdout.on('data', (ch) => {
      buf += ch.toString();
      let i;
      while ((i = buf.indexOf('\n')) >= 0) {
        const ln = buf.slice(0, i).replace(/\r$/, '');
        buf = buf.slice(i + 1);
        if (!ln) continue;
        if (!TOPN || TOPN === 1) {
          if (/^[0-9]+,[0-9]+(\s+[0-9]+,[0-9]+)?$/.test(ln.trim())) { best = ln.trim(); fin(); break; }
        } else if (/\s[0-9]+,[0-9]+/.test(ln) && !ln.includes('INFO') && !ln.includes('MESSAGE')) {
          // multi-PV: keep only the first (best) line as the reported move
          if (!best) best = ln.trim();
        }
        if (ln.startsWith('MESSAGE Speed ')) {
          // 引擎的收尾汇总（比最后一条 INFO 更可靠）：Speed 1879K | Depth 22-37 | Eval -214 | Node 5567K | Time 2962ms
          const ms = /Speed\s+(\d+)K/.exec(ln); if (ms) speed = +ms[1] * 1000;
          const mn = /Node(?:s)?\s+(\d+)K/.exec(ln); if (mn) nodes = +mn[1] * 1000;
          const md = /Depth\s+(\d+)(?:-(\d+))?/.exec(ln); if (md) { d = +md[1]; sd = +(md[2] || md[1]); }
          const me = /Eval\s+([^|]+)/.exec(ln); if (me) ev = me[1].trim();
          const mt = /Time\s+(\d+)ms/.exec(ln); if (mt) ttime = +mt[1];
          continue;
        }
        if (ln.startsWith('MESSAGE Depth ')) {
          const md = /Depth\s+(\d+)(?:-(\d+))?/.exec(ln); if (md) { d = +md[1]; sd = +(md[2] || md[1]); }
          const me = /Eval\s+([^|]+)/.exec(ln); if (me) ev = me[1].trim();
          const mt = /Time\s+(\d+)ms/.exec(ln); if (mt) ttime = +mt[1];
          continue;
        }
        if (ln.startsWith('INFO DEPTH ')) { const m = /(\d+)\s*[-/]\s*(\d+)/.exec(ln.slice(11)); if (m) { d = Math.max(d, +m[1]); sd = Math.max(sd, +m[2]); } else { const x = parseInt(ln.slice(11), 10); if (x) d = Math.max(d, x); } }
        else if (ln.startsWith('INFO SELDEPTH ')) sd = Math.max(sd, parseInt(ln.slice(14), 10) || sd);
        else if (ln.startsWith('INFO TOTALNODES ')) nodes = parseInt(ln.slice(16), 10) || nodes;
        else if (ln.startsWith('INFO SPEED ')) speed = parseInt(ln.slice(11), 10) || speed;
        else if (ln.startsWith('INFO TOTALTIME ')) ttime = parseInt(ln.slice(16), 10) || ttime;
        else if (ln.startsWith('INFO EVAL ')) ev = ln.slice(10).trim();
      }
    });
    const cmds = [
      'START 15', 'INFO RULE ' + RULE, 'INFO SHOW_DETAIL ' + DETAIL,
      'INFO THREAD_NUM ' + (v.threads || THREADS), 'INFO HASH_SIZE ' + (v.hashkb || HASHKB),
      'INFO PONDERING 0', 'INFO TIMEOUT_MATCH 6000000', 'INFO TIME_LEFT 6000000',
      'INFO TIMEOUT_TURN ' + TURN,
    ];
    if (CAUTION >= 0) cmds.unshift('INFO CAUTION_FACTOR ' + CAUTION);
    if (MAXDEPTH > 0) cmds.push('INFO MAX_DEPTH ' + MAXDEPTH);
    cmds.push('YXBOARD ' + pts.map((m, i) => m[0] + ',' + m[1] + ',' + (i % 2 === 0 ? 1 : 2)).join(' ') + ' DONE');
    cmds.push('YXNBEST ' + TOPN);
    p.stdin.write(cmds.join('\n') + '\n');
    setTimeout(fin, TURN + 30000);
    if (TOPN > 1) setTimeout(fin, TURN + 500);  // multi-PV: stop after the fixed budget
  });
}

(async () => {
  const all = [];
  const hdr = 'turn=' + TURN + 'ms reps=' + REPS + ' threads=' + THREADS + ' hash=' + Math.round(HASHKB / 1024) + 'MB detail=' + DETAIL + (MAXDEPTH ? ' maxdepth=' + MAXDEPTH : '');
  console.log(hdr);
  console.log('variants: ' + VARIANTS.map(v => v.name + '=' + v.exe).join('  '));
  for (const pi of POSPICK) {
    for (let r = 0; r < REPS; r++) {
      // rotate order each rep so no variant always runs first
      const ord = VARIANTS.map((v, i) => VARIANTS[(i + r) % VARIANTS.length]);
      for (const v of ord) {
        const res = await once(v, POS[pi]);
        res.pi = pi; res.rep = r;
        all.push(res);
        console.log('  pos' + pi + ' rep' + r + ' ' + v.name.padEnd(18) +
          ' d=' + res.d + '-' + res.sd + ' ' + (res.speed / 1000).toFixed(0) + 'kn/s ' +
          'nodes=' + (res.nodes / 1e6).toFixed(2) + 'M ' + res.best + ' eval=' + res.ev + ' wall=' + res.wall + 'ms');
      }
    }
  }
  console.log('\n== medians ==');
  const summary = {};
  for (const v of VARIANTS) {
    const a = all.filter(x => x.variant === v.name);
    const m = f => rmedian(a.map(x => f(x)));
    summary[v.name] = {
      n: a.length, depth: m(x => x.d), seldepth: m(x => x.sd),
      nps: m(x => x.speed), nodes: m(x => x.nodes), wall: m(x => x.wall),
      best: a.reduce((acc, x) => { acc[x.best] = (acc[x.best] || 0) + 1; return acc; }, {}),
      evals: a.map(x => x.ev),
    };
    console.log('  ' + v.name.padEnd(18) + ' depth ' + summary[v.name].depth + '-' + summary[v.name].seldepth +
      '  nps ' + (summary[v.name].nps / 1000).toFixed(0) + 'k  nodes ' + (summary[v.name].nodes / 1e6).toFixed(2) +
      'M  moves ' + JSON.stringify(summary[v.name].best));
  }
  if (OUT) {
    fs.mkdirSync(path.dirname(OUT), { recursive: true });
    fs.writeFileSync(OUT, JSON.stringify({ turn: TURN, reps: REPS, threads: THREADS, hashKB: HASHKB, detail: DETAIL, variants: VARIANTS, samples: all, summary }, null, 1));
    console.log('wrote ' + OUT);
  }
  process.exit(0);
})();
