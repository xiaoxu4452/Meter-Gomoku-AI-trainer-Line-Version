
(function (root) {
  'use strict';
  var C = {};

  

  C.parseK = function (v) {
    v = String(v).trim();
    var m = v.match(/^([\d.]+)\s*([kKmM])?$/);
    if (!m) return 0;
    var n = parseFloat(m[1]);
    if (m[2] === 'k' || m[2] === 'K') return Math.round(n * 1000);
    if (m[2] === 'm' || m[2] === 'M') return Math.round(n * 1000000);
    return Math.round(n);
  };

  C.parseForbidLine = function (line) {
    var t = String(line || '');
    var m = t.match(/^\s*FORBID\b(.*)$/i);
    if (!m) return null;
    var rest = m[1].trim();
    if (!rest || rest === '.') return [];
    var body = rest.replace(/\.\s*$/, '').replace(/\s+/g, '');
    var digits = body.replace(/[^0-9]/g, '');
    var out = [];
    for (var i = 0; i + 4 <= digits.length; i += 4) {
      var x = parseInt(digits.slice(i, i + 2), 10);
      var y = parseInt(digits.slice(i + 2, i + 4), 10);
      if (Number.isNaN(x) || Number.isNaN(y)) continue;
      if (x < 0 || x >= 15 || y < 0 || y >= 15) continue;
      out.push([x, 15 - 1 - y]);
    }
    return out;
  };

  
  C.parseLine = function (line, pvs, N) {
    if (!line || !line.trim()) return;
    var i = line.indexOf(' ');
    if (i === -1) {
      var t0 = line.trim();
      var pairs0 = t0.match(/\d+,\d+/g);
      if (pairs0 && pairs0.length) {
        var p0 = pairs0[0].split(',').map(Number);
        pvs.__final = [p0[0], N - 1 - p0[1]];
      }
      return;
    }
    var head = line.substring(0, i);
    var tail = line.substring(i + 1);

    if (head === 'INFO') {
      var j = tail.indexOf(' ');
      var key = j === -1 ? tail : tail.substring(0, j);
      var val = j === -1 ? '' : tail.substring(j + 1);
      if (key === 'PV') {
        if (val !== 'DONE') { pvs.__cur = +val; if (!pvs[pvs.__cur]) pvs[pvs.__cur] = {}; }
      } else if (key === 'NUMPV') { pvs.__numpv = +val; }
      else if (key === 'TOTALTIME') { pvs.__totaltime = +val; }
      else if (key === 'TOTALNODES') { pvs.__totalnodes = +val; }
      else if (key === 'SPEED') { pvs.__speed = +val; }
      else if (pvs.__cur >= 0 && pvs[pvs.__cur]) {
        if (key === 'DEPTH') pvs[pvs.__cur].depth = +val;
        else if (key === 'SELDEPTH') pvs[pvs.__cur].seldepth = +val;
        else if (key === 'NODES') pvs[pvs.__cur].nodes = +val;
        else if (key === 'EVAL') pvs[pvs.__cur].eval = val;
        else if (key === 'WINRATE') pvs[pvs.__cur].winrate = parseFloat(val);
        else if (key === 'BESTLINE') {
          pvs[pvs.__cur].bestline = (val.match(/\d+,\d+/g) || []).map(function (s) {
            var p = s.split(',').map(Number); return [p[0], N - 1 - p[1]];
          });
        }
      }
      return;
    }

    if (head === 'FORBID') {
      var pts = C.parseForbidLine(line);
      if (pts && pts.length) {
        if (!pvs.__forbid) pvs.__forbid = [];
        for (var k = 0; k < pts.length; k++) pvs.__forbid.push(pts[k]);
      }
      return;
    }

    if (head === 'MESSAGE') {
      if (tail.indexOf('REALTIME') === 0) {
        var r = tail.split(' ');
        if (r.length >= 3 && tail.indexOf('REALTIME BEST') === 0) {
          var c = r[2].split(',').map(Number);
          if (c.length === 2) pvs.__best = [c[0], N - 1 - c[1]];
        }
        return;
      }
      var isPonder = /^\s*\[/.test(tail);
      if (isPonder) return;
      var mpv = tail.match(/^\((\d+)\)\s+([^\s|]+)\s*\|\s*(\d+)(?:-(\d+))?\s*\|\s*(.*)$/);
      if (mpv) {
        var pvi = +mpv[1] - 1;
        pvs.__cur = pvi;
        if (!pvs[pvi]) pvs[pvi] = {};
        var pv = pvs[pvi];
        pv.eval = mpv[2];
        pv.depth = +mpv[3];
        if (mpv[4]) pv.seldepth = +mpv[4];
        var bl = [];
        var parts = mpv[5].trim().split(/\s+/);
        for (var a = 0; a < parts.length; a++) {
          var mc = parts[a].match(/^([A-Za-z])(\d+)$/);
          if (mc) bl.push([mc[1].toUpperCase().charCodeAt(0) - 65, (+mc[2]) - 1]);
        }
        pv.bestline = bl;
        return;
      }
      var ms = tail.match(/^Depth\s+(\d+)(?:-(\d+))?\s*\|\s*Eval\s+([^\s|]+)\s*\|\s*Time\s+[\d.]+\s*ms\s*\|\s*(.+)$/);
      if (ms) {
        var bl2 = [];
        var parts2 = ms[4].trim().split(/\s+/);
        for (var b = 0; b < parts2.length; b++) {
          var mc2 = parts2[b].match(/^([A-Za-z])(\d+)$/);
          if (mc2) bl2.push([mc2[1].toUpperCase().charCodeAt(0) - 65, (+mc2[2]) - 1]);
        }
        if (bl2.length) {
          if (!pvs[0]) pvs[0] = {};
          var newDepth = +ms[1];
          if (!pvs[0].bestline || !pvs[0].bestline.length || (pvs[0].depth || 0) <= newDepth) {
            pvs[0].bestline = bl2;
            pvs[0].eval = ms[3];
            pvs[0].depth = newDepth;
            if (ms[2]) pvs[0].seldepth = +ms[2];
          }
        }
      }
      var spd = tail.match(/^Speed\s+([\d.]+[kKmM]?)\s*\|\s*Depth\s+(\d+)(?:-(\d+))?\s*\|\s*Eval\s+([^\s|]+)\s*\|\s*Node\s+([\d.]+[kKmM]?)\s*\|\s*Time\s+[\d.]+\s*ms/i);
      if (spd) {
        pvs.__speed = C.parseK(spd[1]);
        pvs.__totalnodes = C.parseK(spd[5]);
        if (pvs[0] && !pvs[0].eval) pvs[0].eval = spd[4];
        if (pvs[0] && !pvs[0].depth) pvs[0].depth = +spd[2];
      }
      return;
    }

    if (head === 'best') {
      var cc = tail.split(/\s+/).map(Number);
      if (cc.length >= 2) pvs.__best = [cc[0], N - 1 - cc[1]];
      return;
    }
    if (/^\d+,\d+$/.test(head) && /^\d+,\d+$/.test(tail)) {
      var pp = head.split(',').map(Number);
      pvs.__final = [pp[0], N - 1 - pp[1]];
    }
  };

  
  C.doneOf = function (raw, pvs, N) {
    var mbest = raw.match(/\bbest\s+(\d+)\s+(\d+)/);
    var chosen = (mbest ? [+mbest[1], N - 1 - (+mbest[2])] : null) || pvs.__final || pvs.__best;
    var done = !!mbest || !!pvs.__final || /^\s*REJECT\s*$/m.test(raw);
    return { done: done, best: chosen };
  };

  

  
  C.evalAuth = function (raw) {
    var s = (raw == null) ? '' : String(raw).trim();
    if (!s) return null;
    var m = /^([+-]?)([Mm]?)(\d+(?:\.\d+)?)$/.exec(s);
    if (!m) return null;
    var v = parseFloat(m[3]);
    if (!isFinite(v)) return null;
    var sign = (m[1] === '-') ? -1 : 1;
    if (m[2]) return { mate: true, sign: sign, n: v, v: sign * 90000 };
    return { mate: false, sign: sign, n: v, v: sign * v };
  };

  C.evalText = function (a) {
    if (!a) return '-';
    if (a.mate) return (a.sign < 0 ? '-' : '+') + 'M' + a.n;
    return (a.sign < 0 ? '-' : '+') + a.n;
  };

  
  C.evalFor = function (raw, rootSide, viewSide) {
    var a = C.evalAuth(raw);
    if (!a) return '-';
    if ((rootSide === 1 || rootSide === 2) && (viewSide === 1 || viewSide === 2) &&
        rootSide !== viewSide) {
      a = { mate: a.mate, sign: -a.sign, n: a.n, v: -a.v };
    }
    return C.evalText(a);
  };

  C.evalScore = function (raw, flip) {
    var a = C.evalAuth(raw);
    if (!a) return 0;
    var v = a.v;
    if (flip) v = -v;
    return Math.max(-1000, Math.min(1000, v));
  };

  

  
  C.makeEpochGate = function (fallbackMs, parseOne) {
    var age = +fallbackMs || 400;
    var armedAt = 0, held = [], stale = 0, replay = 0;
    function drain(flushing) {
      var p = held; held = [];
      for (var i = 0; i < p.length; i++) {
        if (flushing) { replay++; C._gateParse(parseOne, p[i]); }
        else stale++;
      }
    }
    return {
      arm: function () { armedAt = Date.now(); held = []; stale = 0; replay = 0; },
      armed: function () { return !!armedAt; },
      staleCount: function () { return stale; },
      replayCount: function () { return replay; },
      heldCount: function () { return held.length; },
      feed: function (line) {
        if (!armedAt) { C._gateParse(parseOne, line); return; }
        var t = String(line == null ? '' : line);
        if (/^\s*MESSAGE\s+OptiTime\b/.test(t)) { armedAt = 0; drain(false); return; }
        if (Date.now() - armedAt > age) { armedAt = 0; drain(true); C._gateParse(parseOne, line); return; }
        held.push(t);
      },
    };
  };
  C._gateParse = function (fn, line) { if (typeof fn === 'function') fn(line); };

  

  C.countStones = function (board, N) {
    var n = 0;
    for (var y = 0; y < N; y++) for (var x = 0; x < N; x++) if (board[y][x] !== 0) n++;
    return n;
  };

  
  C.sideToMove = function (bl, wh) { return (bl.length <= wh.length) ? 1 : 2; };

  C.applySideToMove = function (out, side) {
    if (side !== 1 && side !== 2) return out;
    if (!out.length) return out;
    var wantLast = 3 - side;
    if (out[out.length - 1][2] === wantLast) return out;
    var idx = -1, i;
    for (i = out.length - 1; i >= 1; i--) { if (out[i][2] === wantLast) { idx = i; break; } }
    if (idx < 0) return out;
    var m = out.splice(idx, 1)[0];
    out.push(m);
    return out;
  };

  C.toEngineMoveList = function (board, moveList, side, N) {
    var black = [], white = [];
    for (var y = 0; y < N; y++) for (var x = 0; x < N; x++) {
      if (board[y][x] === 1) black.push([x, y]);
      else if (board[y][x] === 2) white.push([x, y]);
    }
    var usableList = Array.isArray(moveList) && moveList.length > 0;
    var used = {};
    var pick = function (color) {
      if (!usableList) return null;
      for (var i = 0; i < moveList.length; i++) {
        var m = moveList[i];
        if (m && typeof m.x === 'number' && typeof m.y === 'number' &&
            board[m.y] && board[m.y][m.x] === color) {
          var key = m.x + ',' + m.y;
          if (!used[key]) { used[key] = true; return [m.x, m.y]; }
        }
      }
      return null;
    };
    var bs = black.slice(), ws = white.slice();
    var seq = [];
    var maxLen = Math.max(bs.length, ws.length);
    for (var i2 = 0; i2 < maxLen; i2++) {
      if (i2 < bs.length) seq.push(1);
      if (i2 < ws.length) seq.push(2);
    }
    var out = [];
    for (var s = 0; s < seq.length; s++) {
      var color = seq[s];
      var pos = usableList ? pick(color) : null;
      if (!pos) pos = (color === 1 ? bs.shift() : ws.shift());
      else {
        var src = (color === 1 ? bs : ws);
        for (var k = 0; k < src.length; k++) if (src[k][0] === pos[0] && src[k][1] === pos[1]) { src.splice(k, 1); break; }
      }
      if (!pos) continue;
      out.push([pos[0], N - 1 - pos[1], color]);
    }
    return C.applySideToMove(out, side);
  };

  

  
  C.budgetMs = function (userMs, floorMs) {
    var m = +userMs || 0;
    var f = +floorMs || 0;
    if (m < f) m = f;
    return Math.round(m);
  };

  
  C.makeStopQuiet = function (o) {
    return new Promise(function (resolve) {
      var quietMs = o.quietMs || 110, maxMs = o.maxMs || 450, tickMs = o.tickMs || 35;
      var t0 = Date.now(), lastLen = -1, lastChange = Date.now(), done = false;
      function finish() {
        if (done) return;
        done = true;
        clearInterval(t);
        try { o.clear(); } catch (e) {}
        resolve();
      }
      try { o.send('STOP'); } catch (eS) { finish(); return; }
      var t = setInterval(function () {
        var len = 0;
        try { len = o.len() || 0; } catch (e) {}
        if (len !== lastLen) { lastLen = len; lastChange = Date.now(); }
        if (Date.now() - lastChange >= quietMs || Date.now() - t0 > maxMs) finish();
      }, tickMs);
    });
  };

  
  C.collectCandidates = function (pvs, wantTopN) {
    var top = Math.max(1, Math.min(8, wantTopN || 8));
    var out = [], seen = {};
    for (var k = 0; k < top; k++) {
      var pv = pvs[k];
      var b0 = pv && pv.bestline && pv.bestline[0];
      if (!pv || !b0) continue;
      var ck = b0[0] + ',' + b0[1];
      if (seen[ck]) continue;
      seen[ck] = 1;
      out.push({
        x: b0[0], y: b0[1],
        winrate: (pv.winrate !== undefined && pv.winrate > 0) ? pv.winrate : 0.5,
        eval: pv.eval || '', depth: pv.depth || 0,
        speed: pvs.__speed || 0, nodes: pvs.__totalnodes || pv.nodes || 0,
        line: (pv.bestline || []).slice(0, 16)
      });
    }
    return out;
  };

  C.promoteBest = function (cands, best) {
    if (!best || best.length !== 2) return cands;
    for (var q = 0; q < cands.length; q++) {
      if (cands[q].x === best[0] && cands[q].y === best[1]) {
        if (q > 0) cands.unshift(cands.splice(q, 1)[0]);
        return cands;
      }
    }
    cands.unshift({ x: best[0], y: best[1], winrate: 0.5,
                    eval: (cands[0] && cands[0].eval) || '',
                    depth: (cands[0] && cands[0].depth) || 0,
                    speed: (cands[0] && cands[0].speed) || 0,
                    nodes: (cands[0] && cands[0].nodes) || 0, line: [] });
    return cands;
  };

  C.makeTick = function (ms) {
    var last = 0;
    return function () {
      var now = Date.now();
      if (now - last < ms) return false;
      last = now;
      return true;
    };
  };

  root.GB_Core = C;
})(typeof self !== 'undefined' ? self : this);
