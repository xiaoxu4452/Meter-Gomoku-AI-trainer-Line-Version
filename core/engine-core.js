// Meter Gomoku Link — v4.43
/*!
 * engine-core.js —— 引擎调用的**唯一实现**（单源）
 *
 * 背景（2026-10-18，用户：「单源化」）：
 *   引擎调用层原来有两份实现 ——
 *     A) desktop-<target>/ui/engine-ai.js  （Worker，走 fetch('/engine/…')）
 *     B) desktop-connect/ui/bridge.js 的 natEng（页面，走 chrome.webview IPC）
 *   两边各自实现了一套「解析 / 终局判定 / 预算 / STOP 静默 / 候选排序」。
 *   代价是真实的、已经发生过的：
 *     · ponder 残留行被当成终局   → 每手只算 0.3 秒（两处各修一遍才干净）
 *     · MESSAGE [Pondering] 的对手视角 Eval 被记成我方 → 评分正负抖动
 *       （bridge 修了，worker 没修 ⇒ 主窗一直在抖）
 *     · 候选首选点一个用 __final 优先、一个用 __best 优先 → 落点口径不一致
 *   本文件是这些不变量的唯一来源。任何「一手怎么算」的判断都只能写在这里；
 *   两个消费者只保留**传输**（怎么把命令送出去、怎么把行拿回来）。
 *
 * 约束：
 *   · 纯逻辑，无 IO、无 DOM、无 Worker API —— 同一份代码在 Worker 与页面里都能跑。
 *   · 不引用外部变量：盘面尺寸一律由调用方以 N 传入。
 *   · 挂载到全局 GB_Core：Worker 里 importScripts 后是 worker 全局，页面里是 window。
 */
(function (root) {
  'use strict';
  var C = {};

  /* ================= 协议解析 ================= */

  C.parseK = function (v) {
    v = String(v).trim();
    var m = v.match(/^([\d.]+)\s*([kKmM])?$/);
    if (!m) return 0;
    var n = parseFloat(m[1]);
    if (m[2] === 'k' || m[2] === 'K') return Math.round(n * 1000);
    if (m[2] === 'm' || m[2] === 'M') return Math.round(n * 1000000);
    return Math.round(n);
  };

  // FORBID 行：`FORBID 77 78` / `FORBID 7,7 8,8` / `FORBID .`（空）
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

  /**
   * 一行引擎输出 → 累积进 pvs。
   * pvs 约定：pvs[i] = {depth, seldepth, nodes, eval, winrate, bestline}，
   *           pvs.__cur / __numpv / __speed / __totalnodes / __best / __final / __forbid。
   *
   * ★★ 关键不变量（2026-10-18 重定，勿回退）：**`[Pondering]` 行一律丢弃，一个字段都不收。**
   *
   *   官方 Rapfi（search/searchoutput.cpp）在 ponder 状态下给每条 MESSAGE 加
   *   `[Pondering] ` 前缀，而 ponder 树的**根是对手**。所以这些行的含义是：
   *     · `Eval`   = 对手视角（符号与我们的相反）
   *     · `PV[0]`  = 对手要走的那一手（不是我们的候选点）
   *     · `Depth`  = 另一棵树的深度
   *   旧实现只挡住 Eval、仍然把 PV/Depth 记进 pvs[0]，于是同一个 pvs[0] 里
   *   eval 是我们的、bestline 是对手的、depth 被抬到 ponder 的深度：
   *     - `collectCandidates` 的 candidates[0] ⇒ **对手的落点**（自动落子会把它落下去）
   *     - onTick / softAt / REALTIME 兜底全都拿这个错点 ⇒「优势点被算偏、惨输」
   *     - 一旦 ponder 深度 ≥ 本手深度，真正的搜索结果反而被 `<=` 判据挡在门外
   *   用户口径：「把 Bridge 里的 eval 处理全删掉，按官方 Rapfi 重做」——
   *   正解就是：ponder 行属于**另一棵树**，不参与本手模型。
   */
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
      if (isPonder) return;                       // ★ 另一棵树：整行丢弃（见函数头注释）
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

  /**
   * 终局判定 —— 一手搜索「算完了」的唯一判据。
   *   mbest  : `best x y`（YXNBEST 模式的 ANSWER 指令）
   *   __final: 裸着法行 `x,y`（YXBOARD+YXNBEST 的最终答案）
   *   REJECT : 引擎拒答
   * 优先级：mbest > __final > __best。__best 只是搜索途中的实时最优（可能更浅），
   * 只在拿不到终局行时兜底 —— 反过来（__best 优先）会让落点停在浅层结果上。
   */
  C.doneOf = function (raw, pvs, N) {
    var mbest = raw.match(/\bbest\s+(\d+)\s+(\d+)/);
    var chosen = (mbest ? [+mbest[1], N - 1 - (+mbest[2])] : null) || pvs.__final || pvs.__best;
    var done = !!mbest || !!pvs.__final || /^\s*REJECT\s*$/m.test(raw);
    return { done: done, best: chosen };
  };

  /* ================= 评分（唯一换算层） ================= */

  /**
   * ★★ 官方口径（Rapfi search/ab/search.cpp：根节点 `moveValue = value`，
   *   negamax 返回的就是**轮到走棋那一方**的分数）：
   *     引擎给的 Eval 永远是「行棋方视角」，不是黑方视角、也不是我方视角。
   *   所以全软件只允许这一个换算：`(rootSide === viewSide) ? +v : -v`。
   *   bridge / worker 都不许再自己判断正负 —— 这正是「评分正负颠倒」的来源。
   *
   * @returns {{mate:boolean, sign:number, n:number, v:number}|null}
   *   v = 数值化（mate 用 ±90000 保留旧刻度，供平衡/胜率判断）
   */
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

  // 权威分值 → 显示串（`+M17` / `-503` / `+0`）
  C.evalText = function (a) {
    if (!a) return '-';
    if (a.mate) return (a.sign < 0 ? '-' : '+') + 'M' + a.n;
    return (a.sign < 0 ? '-' : '+') + a.n;
  };

  /**
   * 引擎原始分 → 指定视角的显示串。
   * @param raw       引擎原文（如 `-M24` / `+285`）
   * @param rootSide  我们送进引擎的 side（1=黑 2=白）＝那次搜索的行棋方
   * @param viewSide  要显示给谁看（1=黑 2=白）
   */
  C.evalFor = function (raw, rootSide, viewSide) {
    var a = C.evalAuth(raw);
    if (!a) return '-';
    if ((rootSide === 1 || rootSide === 2) && (viewSide === 1 || viewSide === 2) &&
        rootSide !== viewSide) {
      a = { mate: a.mate, sign: -a.sign, n: a.n, v: -a.v };
    }
    return C.evalText(a);
  };

  // 曲线/胜率用：clamp 到 ±1000（mate 记满）
  C.evalScore = function (raw, flip) {
    var a = C.evalAuth(raw);
    if (!a) return 0;
    var v = a.v;
    if (flip) v = -v;
    return Math.max(-1000, Math.min(1000, v));
  };

  /* ================= 批次纪元闸门 ================= */

  /**
   * ★★ 只收「本手这一次搜索」的输出。
   *
   * 官方 Rapfi 每次搜索开始时必打一行
   *   `MESSAGE OptiTime <opt> | MaxTime <max> ms`   （searchoutput.cpp printSearchStarts，
   *   条件是 timeLimit && !inPonder —— 我们每批都发 TIMEOUT_TURN 且发批前不 ponder）
   * 这就是天然的**纪元标记**：它之前的行全部属于上一轮（ponder / 收手尾巴），必须丢掉。
   * 不丢就会 0.1~0.2 秒就"算完"，并且用上一盘面的 PV、上一棵树的分。
   *
   * 但有两种情况**没有**这行（真机实测）：
   *   · 根节点已成五 / 一步杀 —— Rapfi 直接给答案，不进入计时搜索（`OptiTime=0`）；
   *   · 换构建 / 换引擎，标记不存在。
   * 所以不能"先丢后解析"，而是**先扣住**：见到标记就把扣住的全丢；超时没等到标记，
   * 就把扣住的按原序回放给 parseOne。两边都不会丢结果，也都不会把残渣当成本手。
   *
   * @param fallbackMs 等标记的上限（默认 400ms；真机标记在 0~95ms 内到）
   * @param parseOne   真正解析一行（必须有副作用）
   */
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
      staleCount: function () { return stale; },      // 被判定为上一轮残留、丢掉的行数
      replayCount: function () { return replay; },    // 没有标记时补解析的行数
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
  // 把"用哪个函数解析"收敛到一处：调用方可以换 parser 而不必重建闸门
  C._gateParse = function (fn, line) { if (typeof fn === 'function') fn(line); };

  /* ================= 盘面工具 ================= */

  C.countStones = function (board, N) {
    var n = 0;
    for (var y = 0; y < N; y++) for (var x = 0; x < N; x++) if (board[y][x] !== 0) n++;
    return n;
  };

  /**
   * 行棋方（1=黑 2=白）。**全软件唯一的轮次判据**。
   * 黑先手 ⇒ 子数相等时轮到黑。送进引擎的 `side`、本手「是不是我」、评分视角，
   * 三者必须都从这里取 —— 各自判一次就会出现「落点是对手的、分数是我的」。
   */
  C.sideToMove = function (bl, wh) { return (bl.length <= wh.length) ? 1 : 2; };

  // 保证最后一步是「对方刚走的」⇒ 行棋方 = side。★ 行棋方必须显式传，别猜。
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

  // 面板坐标（y=0 顶）→ 引擎着法序列（数字着法 = y=0 底，0 基）
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

  /* ================= 一手搜索的固定不变量 ================= */

  /**
   * 思考预算：**严格等于用户设置**。
   * 任何"对手想多久我们想多久""威胁加时 ×1.5""重算 ×1.5"之类的倍率都已删除 ——
   * 用户设 2 秒就必须是 2 秒（他曾实测到 4 秒，就是倍率叠出来的）。
   */
  C.budgetMs = function (userMs, floorMs) {
    var m = +userMs || 0;
    var f = +floorMs || 0;
    if (m < f) m = f;
    return Math.round(m);
  };

  /**
   * STOP → 等输出静默 → 清缓冲。**每手 analyze 之前必须走一遍。**
   * 不做这一步，上一轮 ponder/收手残留的裸着法行会落在缓冲里，被 doneOf 当成
   * 本手终局 ⇒ 本手在 0.2~0.4 秒"完成"且用上一盘面的 PV 落子。
   * 传输无关：调用方给出 send / 读长度 / 清缓冲 三个回调。
   *
   * @param {{send:Function, len:Function, clear:Function, quietMs?:number, maxMs?:number, tickMs?:number}} o
   * @returns {Promise<void>}
   */
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

  /**
   * pvs → 候选列表（已去重，按 PV 顺序）。
   * 所有消费方都用这一份，落点/评分的口径才不会分叉。
   */
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

  // 把终局落点排到候选首位（没有就合成一条），调用方直接取 candidates[0]。
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

  // 节流器：live 流 / 诊断上报共用（不要用重搜来"刷新"）。
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
