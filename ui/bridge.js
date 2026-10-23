// Meter Gomoku Link — v4.43
/* Meter Gomoku Trainer - desktop overlay data bridge. Exposes page-facing APIs for board sync, engine analysis, balance-point suggestions and auto-move; the C++ host drives screen scanning and click injection. */


(function () {
  'use strict';

  var S = window.GB_S || (window.GB_S = {});

  var GB_VER = 'v4.43';
  // 自检钩子（必须尽早挂上：后面任何一句抛异常都不影响它；正常流程不用）
  try { window.__gbTacTest = engineTacTest; } catch (e) {}
  var UI = window.GBUI || {};
  var N = 15;

  // 引擎/识别通信已统一为「纯 C++ 硬链接」：引擎走宿主 /engine/* 进程内拦截（零 TCP），
  // 识别结果由宿主 postMessage 推送。旧的 :8964 / :8975 HTTP 直连已移除。

  var HOST = !!(window.chrome && window.chrome.webview && window.chrome.webview.addEventListener);
  // ★★ 2026-10-19 回归 4.36：解析 / 终局判定 / 候选拼装 / 预算全部**自含**（见下方实现），
  //   与 4.36 Trainer 的调用口径一致；engine-core.js 单源层整体退役。
  var GB_CORE = null;

  var uiWarned = {};
  function ui(name, a, b) {
    var f = UI[name];
    if (typeof f !== 'function') return undefined;
    try { return f(a, b); } catch (e) {
      if (!uiWarned[name]) { uiWarned[name] = 1; console.warn('[bridge] UI.' + name + ' 失败：', e && e.message); }
      return undefined;
    }
  }
  function saveNow() { try { if (typeof UI.save === 'function') UI.save(); } catch (e) {} }
  function T(k) { try { return UI.T ? UI.T(k) : k; } catch (e) { return k; } }

  function tellHost(o) {
    try {
      var cv = window.chrome && window.chrome.webview;
      if (cv && cv.postMessage) cv.postMessage(JSON.stringify(o));
    } catch (e) {}
  }

  // ★★ 2026-10-18：**模块级诊断**。以前 bridgeDiag 定义在 natEng 的函数体里，而
  //   analyze()/runSwapEval() 在模块作用域 ⇒ 那 6 处调用是未定义标识符，被外层
  //   try{}catch{} 静默吞掉（日志里 LINK_BUDGET / LINK_TACTIC / LINK_ANALYZE 从未出现）。
  //   诊断必须永远可用：这里直接走 tellHost，宿主按 [bridge-ui] 记一行。
  function bridgeDiag(text) {
    tellHost({ type: 'status', text: String(text) });
  }

  (function wrapStatusForDiag() {
    var orig = UI.setStatus;
    if (typeof orig !== 'function' || orig.__gbWrapped) return;
    var wrapped = function (html, tone) {
      try {
        var text = String(html == null ? '' : html)
          .replace(/<[^>]*>/g, '').replace(/\s+/g, ' ').trim();
        if (text) tellHost({ type: 'status', text: text.slice(0, 160) });
      } catch (e) {}
      return orig.call(UI, html, tone);
    };
    wrapped.__gbWrapped = 1;
    UI.setStatus = wrapped;
  })();

  (function wrapLookForHost() {
    var FNS = ['applyDark', 'applyLang', 'applyOpacity', 'applyBlur'];
    function report() {
      try {
        tellHost({
          type: 'uiLook',
          dark: !!S.dark,
          lang: S.lang === 'en' ? 'en' : 'zh',
          opacity: (typeof S.opacity === 'number') ? S.opacity : 100,
          blur: (typeof S.blur === 'number') ? S.blur : 0,
        });
      } catch (e) {}
    }
    for (var i = 0; i < FNS.length; i++) {
      (function (name) {
        var orig = UI[name];
        if (typeof orig !== 'function' || orig.__gbWrapped) return;
        var w = function () { var r = orig.apply(UI, arguments); report(); return r; };
        w.__gbWrapped = 1;
        UI[name] = w;
      })(FNS[i]);
    }
    window.GB_REPORT_LOOK = report;
  })();

  var COL_SCREEN = '#2FD8FF';   
  var COL_BOARD = '#8B5CF6';    
  var frame = document.getElementById('__gb_frame');
  var fctx = frame.getContext('2d');
  
  if (HOST) frame.style.display = 'none';
  var boardRect = null;         
  var boardGeo = null;          

  function resizeCanvas() {
    bootStep('2a resizeCanvas 进入');
    
    if (HOST) { bootStep('2a0 宿主模式跳过页面画布'); return; }
    var dpr = window.devicePixelRatio || 1;
    var w = window.innerWidth, h = window.innerHeight;
    bootStep('2b 尺寸 w=' + w + ' h=' + h + ' dpr=' + dpr);
    frame.width = Math.round(w * dpr);
    frame.height = Math.round(h * dpr);
    bootStep('2c canvas 位图已设 ' + frame.width + 'x' + frame.height);
    frame.style.width = w + 'px';
    frame.style.height = h + 'px';
    fctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    bootStep('2d setTransform 完成');
    paintFrame();
    bootStep('2e paintFrame 完成');
  }

  function drawLBracket(x0, y0, x1, y1, color, arm, thick, dash) {
    var w = x1 - x0, h = y1 - y0;
    var a = Math.min(arm, w / 2.4, h / 2.4);
    fctx.save();
    fctx.strokeStyle = color;
    fctx.lineCap = 'butt';
    fctx.globalAlpha = 0.55;
    fctx.lineWidth = 1;
    fctx.setLineDash(dash);
    fctx.beginPath();
    fctx.moveTo(x0 + a, y0); fctx.lineTo(x1 - a, y0);
    fctx.moveTo(x0 + a, y1); fctx.lineTo(x1 - a, y1);
    fctx.moveTo(x0, y0 + a); fctx.lineTo(x0, y1 - a);
    fctx.moveTo(x1, y0 + a); fctx.lineTo(x1, y1 - a);
    fctx.stroke();
    fctx.setLineDash([]);
    fctx.globalAlpha = 1;
    fctx.lineWidth = thick;
    fctx.shadowColor = color;
    fctx.shadowBlur = thick * 2.4;
    fctx.beginPath();
    fctx.moveTo(x0, y0 + a); fctx.lineTo(x0, y0); fctx.lineTo(x0 + a, y0);
    fctx.moveTo(x1 - a, y0); fctx.lineTo(x1, y0); fctx.lineTo(x1, y0 + a);
    fctx.moveTo(x1, y1 - a); fctx.lineTo(x1, y1); fctx.lineTo(x1 - a, y1);
    fctx.moveTo(x0 + a, y1); fctx.lineTo(x0, y1); fctx.lineTo(x0, y1 - a);
    fctx.stroke();
    fctx.restore();
  }

  function paintFrame() {
    if (HOST || !fctx) return;    
    var w = window.innerWidth, h = window.innerHeight;
    fctx.clearRect(0, 0, w, h);
    if (S.hideFrame) return;
    var m = 2;
    var arm = Math.max(64, Math.min(132, Math.min(w, h) * 0.075));
    drawLBracket(m, m, w - m, h - m, COL_SCREEN, arm, 3, [6, 9]);
    if (boardRect) {
      var r = boardRect, e = 6;
      
      drawLBracket(r.x0 - e, r.y0 - e, r.x1 + e, r.y1 + e, COL_BOARD,
        Math.max(26, Math.min(60, Math.min(r.x1 - r.x0, r.y1 - r.y0) * 0.17)), 3.4, [5, 7]);
    }
  }
  window.addEventListener('resize', function () { resizeCanvas(); reportPanelRect(); });

  var panel = document.getElementById('__gb_panel');

  var hdrEl = document.getElementById('__gb_hdr');

  function applyMaxHeight() {
    if (!panel) return;
    try {
      var avail = (window.screen && window.screen.availHeight) || 0;
      if (avail < 200) return;                    
      var want = Math.max(240, Math.round(avail + 240)) + 'px';
      if (panel.style.maxHeight !== want) panel.style.maxHeight = want;
    } catch (e) {}
  }

  function reportPanelRect() {
    if (!panel) return;
    applyMaxHeight();
    var r = panel.getBoundingClientRect();
    var hidden = (r.width < 4) || (panel.style.display === 'none');
    
    var hdrH = 34;
    try {
      if (hdrEl) { var hr = hdrEl.getBoundingClientRect(); if (hr.height >= 16 && hr.height <= 120) hdrH = hr.height; }
    } catch (e) {}
    
    var dpr = window.devicePixelRatio || 1;
    
    var dragW = 0, btnX = '';
    try {
      if (hdrEl) {
        var hr2 = hdrEl.getBoundingClientRect();
        var ctrls = hdrEl.querySelectorAll('button,select,input,[role="button"]');
        var minLeft = Infinity, parts = [];
        for (var i = 0; i < ctrls.length; i++) {
          var cr = ctrls[i].getBoundingClientRect();
          if (cr.width <= 0) continue;
          if (cr.left < minLeft) minLeft = cr.left;
          
          var l = Math.max(0, Math.round(cr.left - r.left - 2));
          var rr = Math.round(cr.right - r.left + 2);
          parts.push(l + ':' + rr);
        }
        dragW = (minLeft === Infinity)
          ? Math.max(40, hr2.width - 8)
          : Math.max(40, minLeft - r.left - 4);
        parts.sort(function (a, b) { return parseInt(a, 10) - parseInt(b, 10); });
        btnX = parts.join(',');
      }
    } catch (e) {}
    
    var hNat = 0, hMin = 0;
    try {
      var bodyEl = document.getElementById('__gb_body');
      var barEl = document.getElementById('__gb_bottombar');
      var hdrPx = hdrEl ? hdrEl.offsetHeight : 0;
      var barPx = barEl ? barEl.offsetHeight : 0;
      if (bodyEl && bodyEl.scrollHeight > 0) hNat = hdrPx + bodyEl.scrollHeight;
      
      var hashLbl = bodyEl ? bodyEl.querySelector('[data-i18n="hash"]') : null;
      var hashRow = (hashLbl && hashLbl.closest) ? hashLbl.closest('.__gb_row') : null;
      if (hashRow) {
        hMin = hdrPx + hashRow.offsetTop + hashRow.offsetHeight + barPx;
      } else if (bodyEl) {
        hMin = hdrPx + Math.round(bodyEl.scrollHeight * 0.45);   
      }
    } catch (e) {}
    var cv = window.chrome && window.chrome.webview;
    if (cv && cv.postMessage) {
      cv.postMessage(JSON.stringify({
        type: 'panelRect',
        w: Math.round(r.width), h: Math.round(r.height),
        hdrH: Math.round(hdrH), dpr: Math.round(dpr * 1000) / 1000,
        dragW: Math.round(dragW), btnX: btnX, hidden: hidden,
        hNat: Math.round(hNat), hMin: Math.round(hMin),
      }));
    }
  }

  try {
    if (hdrEl) {
      hdrEl.addEventListener('mousedown', function (ev) {
        if (ev.button !== 0) return;
        if (!HOST) return;                              
        
        var t = ev.target;
        if (t && t.closest && t.closest('button,select,input,textarea,a,[role="button"]')) return;
        if (ev.preventDefault) ev.preventDefault();     
        tellHost({ type: 'dragStart' });
      });
    }
  } catch (e) {}

var lastBoardKey = '';
var emptyStreak = 0;
// ★ 2026-10-18（用户：「TT 要跨手复用，别让识别抖动误判新局而清表」）：
//   空盘持续计时。整盘积累的置换表一旦被 YXHASHCLEAR 扔掉，后面每一手都从零开始搜，
//   表现出来就是「中后盘忽然变弱」。
var emptySinceMs = 0;                 

var pendingKey = '';
var pendingN = 0;
var STABLE_FRAMES = 2;               
var forceScan = false;               
var busy = false, engineBusy = false;
var analyzeRetryTimer = null, analyzeRetryCount = 0;

var pendingEvalKey = null;
var analyzeRetryTimer = null;          

var lastBl = [], lastWh = [];        
var lastAdoptBl = [], lastAdoptWh = [];   

// ★ 2026-10-18（用户：「科学且激进且疯狂的调用引擎（疯狂不代表着超标越界）」）：
//   连线一手的搜索时长不再只吃用户设定的那个固定值。依据两条**事实**：
//   ① 对手思考期间棋盘不动、我们的引擎闲着（只有 ponder 在跑）⇒ 那段时间是**免费思考窗口**；
//   ② 对手实际思考多久，识别帧的节奏里就写着多久（我方落子被确认 → 对手新子出现）。
//   所以本手预算 = max(用户设定, 对手上一手思考时长)，再夹在 LINK_TURN_CAP_MS 之下。
//   「不超标越界」的边界就在这里：一分不多占（对手想多久我们就想多久）+ 单手上限 15s，
//   线程/哈希/ponder 全部走既有的满核 + 大表 + PONDERING 通路，不额外改引擎配置。
var linkOppTurnAt = 0;        // 轮次交给对手的时刻（0 = 当前轮到我方）
var linkOppThinkMs = 0;       // 对手上一手实际思考时长（墙钟）
var LINK_TURN_CAP_MS = 15000; // 单手搜索上限
var lastEvalFp = '';          // S.myEval 是哪一帧盘面的评估（M 快速出子只认当前盘面）

var balBusy = false;

var LBAL = {
  zh: {
    bal1: '平衡一手', bal2: '平衡两手',
    bal1ing: '平衡一手计算中（引擎官方平衡迭代）…', bal2ing: '平衡两手计算中（五手两打）…',
    balSwap: '引擎判定：此际应当换手（SWAP）——当前没有比换手更优的平衡点',
    balNone: '引擎未返回平衡点（盘面可能已无可选点）',
    balFail: '平衡选点失败：', balDone: '平衡点：',
    balPreview: '浏览器预览模式不画圈',
    
    balWon: '盘面已有成五点（%1）—— 必胜局面没有平衡可言，以上是引擎直接给出的必胜点',
    
    adBtn: '自动落子', swBalBtn: '交换平衡点', swEvalBtn: '交换评估',
    balPointBtn: '点数', balPointSet: '平衡点数已设为 %1（空盘固定 3）',
    
    adOn: '自动落子已开启（盯推荐第一点）', adOff: '自动落子已关闭',
    
    adOffNewGame: '新的一局开始 —— 自动落子已关闭（需要时请重新点开）',
    
    engineBusyTimeout: '引擎等待超时（同一通道上还有别的算路在跑）——本次已放弃，盘面一变会自动重算',
    analyzeRetry: '引擎正忙（平衡/交换计算中）——评估稍候自动重试，无需操作',
    analyzeSlow: '引擎响应超时——评估稍候自动重试，无需操作',
    
    swBalNeedAd: '未开启自动落子：请先开启「自动落子」自动摆完平衡点，或对照圆圈在棋盘上手动落子',
    adBusyQueue: '平衡点摆点中，请稍候…',
    engineUnreachable: '连不上本地引擎（硬链接）—— 桌面助手正在自动重启它，通常几秒内恢复；反复出现请检查安全软件是否拦截',
    
    swBalFilled: '（其中 %1 点为估算补位，引擎未在 4.5 秒内返回）',
    adCount: '落子次数：', adFired: '自动落子：', adDropped: '已自动落下',
    adRetry: 'PostMessage 未生效，改用系统级点击重试…',
    adRetryRound: '三种点击方式都没等到棋盘回帧，正在重试第 %1 轮（请让目标窗口保持在前台、不要最小化）…',
    adGiveUp: '自动落子连续 3 轮未生效，已暂停（请确认目标窗口在前台；若是自绘/防外挂窗口，先手动点一次再开）',
    adPtUnreachable: '这一手没落上（目标窗口是否在前台？）—— 等棋盘变化自动重试',
    
    swBalEmpty: '空盘交换平衡计算中（1 白 + 2 黑三点组）…', swBalPair: '交换平衡计算中（1 白 + 1 黑）…',
    swBalMax: '交换平衡点仅限开局五手（3 手 + 2 手），当前盘面已超出交换阶段',
    swBalStage: '交换平衡点只在空盘（两黑一白 3 点）或已有 3 子（一黑一白 2 点）时可用；当前手数不在交换窗口',
    swBalDone: '交换平衡点已标出（浅蓝=白、浅紫=黑）：', swPlacing: '自动摆点：第 %1/%2 个（',
    // ★ 2026-10-13：连线窗以前**从不报平衡分**，所以用户只能看面板那条「对弈引擎评估」
    //   （禁手下实测 +546，被当成「平衡点评估」来看 →「接近 1000 分的误差」）。现在把
    //   平衡引擎自己那一对的分报出来：|评估| ≤ 50 才是官方 balanceWindow 内的「平」。
    swBalEval: '（平衡引擎判定：|评估| ≈ %1，≤ 50 才算平）',
    swBalEvalNone: '（平衡引擎未回传评估分）',
    // ★ 2026-10-13（用户）：开局定平衡点该评估几个 —— 官方指定开局只有三手（黑1 天元 ·
    //   白2 在 3×3 · 黑3 在 5×5，中国五子棋竞赛规则 2025 / RIF），所以开局评估 3 个最合理。
    //   ★ 2026-10-13（用户：「这个提示文案是多条的，是灵活多变的，不应该只是一个大段话，
    //     这个文案在连线器中要精简」）：连线窗位置窄，改成**短句池 + 每次随机 1 条**。
    swBalOpenHints: [
      '官方开局 3 手，评估 3 个点最合理（此处固定 3 个）。',
      '评估 4 个以上已走出开局，分数不可靠。',
      '禁手 / 无禁手的平衡点不同，结果只对当前规则有效。',
      '塔拉山口-10 走满 5 手才可评估到 5 个。',
      '天元固定，不用评估。',
    ],
    swPlaced: '交换平衡摆子完成（%1 个点已自动落完）',
    swSwapNow: '引擎判定：此际应当换手（SWAP）——不摆子，直接换',
    swEvaling: '交换评估计算中…', swEvalBlack: '对弈引擎（非平衡口径）：黑优（%1）—— 建议选黑更有优势',
    swEvalWhite: '对弈引擎（非平衡口径）：白优（%1）—— 建议选白更有优势', swEvalEven: '对弈引擎（非平衡口径）：两边接近（%1）—— 选黑选白差别不大',
    
    swAdvB: '黑优', swAdvW: '白优',
    swEvalEmpty: '棋盘上还没有子 —— 等对手摆完（或先点「交换平衡点」）再评估',
    swEvalFail: '交换评估失败：', swEvalDone: '交换评估完成（数来自对弈引擎，不是平衡点评估）：',
    swEvalLate: '已过交换窗口（超过 5 手）—— 交换评估只在开局阶段有意义',
  },
  en: {
    bal1: 'Balance move', bal2: 'Balance pair',
    bal1ing: 'Balancing (official engine iteration)…', bal2ing: 'Balancing pair (swap-two)…',
    balSwap: 'Engine says: SWAP now — no more balanced point to place',
    balNone: 'No balanced point returned (board may be exhausted)',
    balFail: 'Balance failed: ', balDone: 'Balance points: ',
    balPreview: 'no circles in browser preview',
    balWon: 'Board already has a winning five (%1) — no balance exists in a won position; these are the engine\'s direct winning points',
    
    adBtn: 'Auto drop', swBalBtn: 'Swap balance point', swEvalBtn: 'Swap eval',
    balPointBtn: 'Points', balPointSet: 'Balance point count set to %1 (3 fixed on empty board)',
    
    adOn: 'Auto-drop ON (watching top recommendation)', adOff: 'Auto-drop OFF',
    
    adOffNewGame: 'New game started — auto-drop is OFF (click it again when you need it)',
    engineBusyTimeout: 'Engine timed out (another search is still running on the same lane) — gave up this time; it re-runs on the next board change',
    analyzeRetry: 'Engine busy (balance/swap computing) — evaluation retries automatically in a moment',
    analyzeSlow: 'Engine response timed out — evaluation retries automatically in a moment',
    swBalNeedAd: 'Auto-drop is off: turn on Auto-drop to place the balance points automatically, or place them manually on the board',
    adBusyQueue: 'Placing balance points — please wait…',
    engineUnreachable: "Can't reach the local engine (hard-link) — restarting it automatically; it usually recovers within seconds. If this keeps happening, check your antivirus",
    swBalFilled: ' (%1 point(s) estimated — engine did not answer within 4.5s)',
    adCount: 'Click count: ', adFired: 'Auto-drop: ', adDropped: 'dropped automatically',
    adRetry: 'PostMessage ignored — retrying with real click…',
    adRetryRound: 'No board change confirmed through any click method — retrying round %1 (keep the target window focused and visible)…',
    adGiveUp: 'Auto-drop failed 3 rounds, paused (make sure the target window is in the foreground; for custom-drawn/anti-macro windows, click once by hand first)',
    adPtUnreachable: 'Move did not land (is the target window in front?) — will retry when the board changes',
    
    swBalEmpty: 'Balancing 3-point swap set (1 white + 2 black)…', swBalPair: 'Balancing swap pair (1 white + 1 black)…',
    swBalMax: 'Swap balance applies only to the 5-move opening (3 + 2); this position is past the exchange phase',
    swBalStage: 'Swap balance only works on an empty board (2 black + 1 white, 3 points) or after 3 stones (a black-white pair, 2 points); the current move count is outside the swap window',
    swBalDone: 'Swap balance points marked (light blue=white, light purple=black): ', swPlacing: 'Auto-placing point %1/%2 (',
    swBalEval: ' (balance engine verdict: |eval| ≈ %1; only ≤ 50 counts as balanced)',
    swBalEvalNone: ' (balance engine returned no eval)',
    swBalOpenHints: [
      'Opening is 3 moves - evaluating 3 points is sensible (3 fixed here).',
      '4+ points leaves the opening; the eval stops being reliable.',
      'Renju / freestyle balance points differ - result holds only for the current rule.',
      'Taraguchi-10 (5-move opening) allows up to 5.',
      'Tengen is fixed and needs no evaluation.',
    ],
    swPlaced: 'Swap placement finished (%1 points dropped)',
    swSwapNow: 'Engine says: SWAP now — skip placing, take the swap',
    swEvaling: 'Evaluating swap…', swEvalBlack: 'Playing engine (NOT the balance verdict): Black is better (%1) — taking Black is advised',
    swEvalWhite: 'Playing engine (NOT the balance verdict): White is better (%1) — taking White is advised', swEvalEven: 'Playing engine (NOT the balance verdict): nearly even (%1) — either side is fine',
    swAdvB: 'Black is better', swAdvW: 'White is better',
    swEvalEmpty: 'Board is empty — place stones (or run Swap Balance) first',
    swEvalFail: 'Swap evaluation failed: ', swEvalDone: 'Swap evaluation (number comes from the playing engine, not the balance verdict): ',
    swEvalLate: 'Swap window has passed (over 5 moves) — swap evaluation only matters in the opening',
  }
};
function LT(k) { var d = LBAL[S.lang === 'en' ? 'en' : 'zh'] || LBAL.zh; return d[k] || k; }

  function resetForEmpty() {
    S.history = []; S.myEval = ''; S.lastStat = null; S.candCache = [];
    lastBest = null; lastMainC0 = null;
    balOff();                                 
    ui('drawChart');
  }

  function boardKey(bl, wh) {
    function fmt(a) { return a.map(function (p) { return p.x + ',' + p.y; }).sort().join(';'); }
    return bl.length + ':' + fmt(bl) + '|' + wh.length + ':' + fmt(wh);
  }
  
  function deltaStones(a, b) {
    var kb = {};
    for (var i = 0; i < b.length; i++) kb[b[i].x + ',' + b[i].y] = 1;
    var out = [];
    for (var j = 0; j < a.length; j++) {
      var k = a[j].x + ',' + a[j].y;
      if (!kb[k]) out.push(k);
    }
    return out;
  }

  // （已移除）fetchJSON：页面侧不再直连 HTTP。
    var mainChain = Promise.resolve();
  function onMainLane(fn) {
    var run = mainChain.then(fn, fn);              
    mainChain = run.then(function () {}, function () {});
    return run;
  }

  var lastDepsPing = 0;
  function pingEnsureDeps() {
    if (!HOST) return;
    var now = Date.now();
    if (now - lastDepsPing < 5000) return;
    lastDepsPing = now;
    try { tellHost({ type: 'ensureDeps' }); } catch (ePing) {}
  }
  function errText(e) {
    var m = (e && e.message) ? e.message : String(e);
    
    if (isWaitTimeout(e)) return LT('engineBusyTimeout');
    if (e instanceof TypeError || /failed to fetch|fetch failed|networkerror|load failed/i.test(m)) {
      pingEnsureDeps();
      // ★ 2026-10-16：把**真实原因**附在后面。以前只返回「连不上本地引擎（硬链接）—— 桌面
      //   助手正在自动重启它」这一句，用户/日志都拿不到任何可定位的信息；现在原始 message
      //   一起显示（并随 status 日志落到宿主供取证）。
      return LT('engineUnreachable') + '（' + m + '）';
    }
    if (/connection lost/i.test(m)) { pingEnsureDeps(); return LT('engineUnreachable') + '（' + m + '）'; }
    return m;
  }

  function isWaitTimeout(e) {
    if (e && e.name === 'AbortError') return true;
    var m = String((e && e.message) || e || '');
    return /abort|ws timeout/i.test(m);
  }

  // ───────────────────────────────────────────────────────────────────────
  // 引擎通道（2026-10-19 重建）：宿主单实例 Rapfi「极速版」。
  //   页面只发 ask / stop / caps / config 四种 op；协议编排与解析 100% 在 C++
  //   （desktop-connect/src/engine_host.cpp）。页面里不再有任何协议文本解析、
  //   不再有车道（main/sub/fwd）、不再有轮询、不再有 HTTP / WebSocket。
  //   宿主推回两条消息：
  //     {"type":"engineAck","id":N,"op":"ask","ok":bool,["err":..,]"res":{...}}
  //     {"type":"engineAsk","id":N,"phase":"tick","res":{...}}   ← 中间过程
  //   返回口径与旧 doAnalyze 完全一致（candidates[].line/speed/winrate、
  //   best / elapsed / superseded / error），故 analyze()/heat/swap 调用点无需改动。
  // ───────────────────────────────────────────────────────────────────────
  var NATENG = null;
  var NATENG_CAPS = null;          // 最近一次 caps（变体/线程/哈希），供 UI 与诊断
  function natEng() {
    if (NATENG) return NATENG;
    NATENG = (function () {
      var seq = 0;
      var pend = {};               // id -> job（在途 ask）
      var capWait = {};            // caps 的 id -> resolve
      var cur = null, wait = null; // 单实例：同时只允许一个 ask 在途（宿主只有一个引擎）

      function post(o) {
        var cv = window.chrome && window.chrome.webview;
        if (!cv || !cv.postMessage) return false;
        try { cv.postMessage(JSON.stringify(o)); } catch (e) { return false; }
        return true;
      }
      function emptyRes(sup, err) {
        return { candidates: [], best: null, elapsed: 0, depth: 0, nodeCount: 0,
                 superseded: !!sup, error: err || '' };
      }
      // 宿主结构化结果 → 旧 doAnalyze 口径
      function mapRes(r) {
        r = r || {};
        var cands = (r.candidates || []).map(function (c) {
          var wr = (typeof c.winrateBp === 'number' && c.winrateBp >= 0 && c.winrateBp <= 10000)
                     ? c.winrateBp / 10000 : 0.5;
          // ★ 2026-10-19（用户：「节点和速度不会差十倍」）：speed 是全局口径
          //   （INFO SPEED = 全线程总节点 / 耗时），节点必须同源用全局 TOTALNODES（r.nodes）。
          //   以前这里配的是单手指子树 INFO NODES，两者差 ≈ 线程数倍（实测 ≈ 9.08x）。
          return { x: c.x, y: c.y, eval: c.eval || '', depth: c.depth || 0,
                   speed: r.speed || 0, nodes: r.nodes || 0, winrate: wr,
                   line: (c.pv || []).map(function (p) { return [p[0], p[1]]; }) };
        });
        var best = (r.bestX >= 0 && r.bestY >= 0) ? [r.bestX, r.bestY]
                                                  : (cands.length ? [cands[0].x, cands[0].y] : null);
        return { candidates: cands, best: best, elapsed: r.timeMs || 0,
                 depth: r.depth || 0, nodeCount: r.nodes || 0,
                 superseded: !!r.superseded, error: r.err || '' };
      }
      function firstTick(r) {
        var c = r && r.candidates && r.candidates[0];
        if (!c || c.x < 0) return null;
        return { x: c.x, y: c.y, depth: c.depth || r.depth || 0, eval: c.eval || '' };
      }
      function startJob(job) {
        cur = job;
        pend[job.id] = job;
        job.timer = setTimeout(function () {
          if (cur === job) cur = null;
          delete pend[job.id];
          job.resolve(job.op === 'balance'
            ? { moves: [], eval: null, swap: false, error: 'search timeout' }
            : emptyRes(false, 'search timeout'));
          pump();
        }, job.timeoutMs);
        if (!post({ engine: 1, op: job.op || 'ask', id: job.id, q: job.q })) {
          clearTimeout(job.timer); delete pend[job.id]; cur = null;
          job.resolve(job.op === 'balance'
            ? { moves: [], eval: null, swap: false, error: 'no host bridge' }
            : emptyRes(false, 'no host bridge'));
          pump();
        }
      }
      function pump() {
        if (cur || !wait) return;
        var j = wait; wait = null; startJob(j);
      }
      function makeJob(body, timeoutMs, resolve, op) {
        var b = body || {};
        var q = { board: b.board,
                  side: (b.side === 1 || b.side === 2) ? b.side : 0,
                  rule: b.rule | 0,
                  budgetMs: Math.max(200, b.turnMs | 0),
                  key: b.key || 0 };          // ★ 真实盘面指纹（此前硬编码 0，宿主无法识别同盘面连问）
        if (op === 'balance') {
          q.topN = 1; q.pondering = false;
          q.mode = (b.mode === 'two') ? 'two' : 'one';
          q.bias = b.bias | 0;
        } else {
          q.topN = Math.max(1, Math.min(8, b.topN | 0));
          q.pondering = !b.noPonder;
        }
        return { id: ++seq, timer: null, resolve: resolve, onTick: b.onTick, op: op || 'ask',
                 exclusive: !!b.exclusive, timeoutMs: timeoutMs || 8000, q: q };
      }
      // 页面侧串行（宿主只有一个引擎）：同时只跑一个 ask。新的「要落子」请求
      // （exclusive）可抢占在途的辅助请求（热度图 / 换位评估）；其余请求排队，
      // 且只保留最新的一个排队项（更旧的就地作废，避免拿旧盘面把预算跑满）。
      function submit(job) {
        if (wait) { wait.resolve(emptyRes(true)); wait = null; }
        if (!cur) { startJob(job); return; }
        if (job.exclusive && !cur.exclusive) {
          var old = cur; cur = null; delete pend[old.id];
          clearTimeout(old.timer);
          try { post({ engine: 1, op: 'stop', id: ++seq, q: {} }); } catch (e) {}
          old.resolve(emptyRes(true));
          startJob(job);
          return;
        }
        wait = job;
      }
      function ask(body, timeoutMs) {
        return new Promise(function (resolve) { submit(makeJob(body, timeoutMs, resolve, 'ask')); });
      }
      // 官方平衡点（宿主 YXBALANCEONE / YXBALANCETWO，一次性算完即停）。
      function balance(body, timeoutMs) {
        return new Promise(function (resolve) { submit(makeJob(body, timeoutMs, resolve, 'balance')); });
      }
      function caps() {
        return new Promise(function (resolve) {
          var id = ++seq;
          capWait[id] = resolve;
          if (!post({ engine: 1, op: 'caps', id: id, q: {} })) { delete capWait[id]; resolve(null); return; }
          setTimeout(function () {
            if (capWait[id]) { delete capWait[id]; resolve(NATENG_CAPS); }
          }, 3000);
        });
      }
      function call(api, body, timeoutMs) {
        if (api === 'analyze') return ask(body, timeoutMs);
        if (api === 'balance') return balance(body, timeoutMs);
        if (api === 'health') {
          return caps().then(function (c) {
            return { ok: !!(c && c.available), available: !!(c && c.available),
                     variant: (c && c.variant) || '', diag: (c && c.diag) || '' };
          });
        }
        // swap2board 依赖的 YXSWAP2BOARD 协议尚未在新宿主重建：
        // 返回空结果而不抛错，让调用方走既有的「无解 / 本地定式兜底」分支。
        bridgeDiag('LINK_ENG_API_RETIRED ' + api);
        return Promise.resolve(emptyRes(false, 'api retired: ' + api));
      }
      // 宿主 balance 结果 → 旧口径 { moves:[[x,y]…], eval:num|null, swap, error }
      function balanceRes(r) {
        r = r || {};
        var pts = (r.pts || []).map(function (p) { return [p[0], p[1]]; });
        var ev = r.bestEval, num = null;
        if (typeof ev === 'string' && ev) {
          if (/^[+-]?M/.test(ev)) num = (ev.charAt(0) === '-') ? -1000 : 1000;
          else { var n = parseInt(ev, 10); num = isNaN(n) ? null : n; }
        }
        return { moves: pts, eval: num, swap: !!r.swap, error: r.err || '' };
      }
      function onAck(msg) {
        if (!msg || !msg.id) return;
        if (msg.op === 'caps') {
          var cw = capWait[msg.id];
          if (cw) { delete capWait[msg.id]; NATENG_CAPS = msg.res || null; cw(NATENG_CAPS); }
          return;
        }
        var job = pend[msg.id];
        if (!job) return;
        delete pend[msg.id];
        clearTimeout(job.timer);
        if (cur && cur.id === msg.id) cur = null;
        var r;
        if (job.op === 'balance') {
          r = balanceRes(msg.res);
          if (msg.ok === false && msg.err) r.error = String(msg.err);
        } else {
          r = mapRes(msg.res);
          if (msg.ok === false) {
            if (msg.err === 'superseded') r.superseded = true;
            else r.error = String(msg.err || 'engine error');
          }
        }
        job.resolve(r);
        pump();
      }
      // 中间过程：宿主对同一 id 每 ≥120ms 推一次结构化 res（不重搜、不换手）。
      function onOut(msg) {
        if (!msg || msg.type !== 'engineAsk' || msg.phase !== 'tick' || !msg.id) return;
        var job = pend[msg.id];
        if (!job || typeof job.onTick !== 'function') return;
        var t = firstTick(msg.res);
        if (t) { try { job.onTick(t); } catch (e) {} }
      }
      function diag(why) {
        caps().then(function (c) {
          bridgeDiag('LINK_ENG_DIAG ' + why + ' avail=' + (c && c.available ? 1 : 0) +
                     ' variant=' + ((c && c.variant) || '-') +
                     ' threads=' + ((c && c.threads) || 0) + ' hashMB=' + ((c && c.hashMB) || 0) +
                     ' diag=' + ((c && c.diag) || '-'));
        }).catch(function () {});
      }
      // 引擎复位（空盘新局 / 规则切换）：只作废在途搜索；引擎进程由宿主常驻，不重启。
      function hardReset(reason) {
        try { post({ engine: 1, op: 'stop', id: ++seq, q: {} }); } catch (e) {}
        if (cur) { clearTimeout(cur.timer); delete pend[cur.id]; cur.resolve(emptyRes(true)); cur = null; }
        if (wait) { wait.resolve(emptyRes(true)); wait = null; }
        bridgeDiag('LINK_ENG_RESET ' + (reason || ''));
      }
      // ★★ 2026-10-19（用户：「在交换平衡点、交换评估后面再添加一个取消按键，点击取消后可以停止
      //   交换平衡的计算」）：中断在途的 ask / balance（向宿主发 op:'stop'）并让等待方立刻拿到
      //   「已作废」的结果。注意 balance 的口径必须按 balanceRes 的形状给（moves/eval/swap/error），
      //   否则 runSwapBal 侧读 .moves 会直接炸。
      function cancelAll() {
        try { post({ engine: 1, op: 'stop', id: ++seq, q: {} }); } catch (e) {}
        var jobs = [], i, j;
        if (cur) { jobs.push(cur); cur = null; }
        if (wait) { jobs.push(wait); wait = null; }
        for (i = 0; i < jobs.length; i++) {
          j = jobs[i];
          clearTimeout(j.timer); delete pend[j.id];
          j.resolve(j.op === 'balance'
            ? { moves: [], eval: null, swap: false, error: 'cancelled' }
            : emptyRes(true, 'cancelled'));
        }
        bridgeDiag('LINK_ENG_CANCEL_ALL n=' + jobs.length);
        return jobs.length;
      }
      return { call: call, status: caps, onAck: onAck, onOut: onOut, diag: diag,
               reset: hardReset, cancelAll: cancelAll };
    })();
    return NATENG;
  }
  function wsCall(api, body, timeoutMs) {
    return natEng().call(api, body, timeoutMs);
  }
  // 置换表清空 / 引擎复位（空盘新局、规则切换时调用）。
  function engineHardReset(reason) {
    try { natEng().reset(reason); } catch (e) {
      try { console.warn('[bridge] engineHardReset failed:', e); } catch (e2) {}
    }
  }
  function engineDiagDump(why) {
    try { natEng().diag(why); } catch (e) {}
  }

  // 自检钩子：用合成棋盘验证战术加固层（正常流程不用；窗口全局便于无头自检读取）
  function engineTacTest(cells, toMove) {
    var m = [], x, y;
    for (y = 0; y < N; y++) { m.push([]); for (x = 0; x < N; x++) m[y].push(0); }
    (cells || []).forEach(function (c) {
      if (m[c[1]] && m[c[1]][c[0]] !== undefined) m[c[1]][c[0]] = c[2];
    });
    var opp = 3 - toMove;
    var plan = tacPlan(m, toMove, 2000);
    var oppWin = plan.oppWin || [];
    var oppFour = plan.oppFour || [];
    var myFour = oppFour.length ? bOpenFourPoints(m, toMove) : [];
    var ans = bMustAnswer(m, toMove, [{ x: 0, y: 0 }], oppWin, oppFour, myFour);
    return {
      oppWin: oppWin.length,
      oppFour: oppFour.length,
      ms: plan.ms,
      why: plan.why,
      ans: ans,
    };
  }
  try { window.__gbTacTest = engineTacTest; } catch (e) {}

  function suspectTip(res) {
    var nb = (res && res.black && res.black.length) || 0;
    var nw = (res && res.white && res.white.length) || 0;
    return '<span style="color:#c0392b">识别结果不可信（读到黑' + nb + '子/白' + nw +
           '子，黑白差过大），已忽略本帧</span>';
  }
  function applyScan(res) {
    if (!res || !res.ok) return false;
    if (res.suspect) { ui('setStatus', suspectTip(res)); return false; }
    var geo = res.geometry;
    if (!res.found || !geo || !geo.x_lines || !geo.y_lines) {
      boardRect = null; boardGeo = null;
      return false;
    }
    boardGeo = geo;
    
    var sc = 1 / (window.devicePixelRatio || 1);
    var xs = geo.x_lines.map(function (v) { return v * sc; });
    var ys = geo.y_lines.map(function (v) { return v * sc; });
    boardRect = {
      x0: Math.min.apply(null, xs), y0: Math.min.apply(null, ys),
      x1: Math.max.apply(null, xs), y1: Math.max.apply(null, ys),
    };
    
    var newSize = geo.size || 15;
    if (typeof N === 'number' && N !== newSize) {
      lastBest = null; lastMainC0 = null; S.candCache = [];
      S.history = [];
      forceScan = true;
    }
    N = newSize;
    if (UI.setN) UI.setN(N);
    return true;
  }

  function boardMatrix(bl, wh) {
    var m = [], x, y, row;
    for (y = 0; y < N; y++) { row = []; for (x = 0; x < N; x++) row.push(0); m.push(row); }
    bl.forEach(function (p) { if (m[p.y] && m[p.y][p.x] !== undefined) m[p.y][p.x] = 1; });
    wh.forEach(function (p) { if (m[p.y] && m[p.y][p.x] !== undefined) m[p.y][p.x] = 2; });
    return m;
  }

  function boardDecided(m) {
    var b = m || lastBoardMatrix();
    var DIR = [[1, 0], [0, 1], [1, 1], [1, -1]];
    for (var y = 0; y < N; y++) for (var x = 0; x < N; x++) {
      var c = b[y][x];
      if (!c) continue;
      for (var d = 0; d < 4; d++) {
        var dx = DIR[d][0], dy = DIR[d][1];
        
        if (y - dy >= 0 && y - dy < N && x - dx >= 0 && x - dx < N && b[y - dy][x - dx] === c) continue;
        var n = 0, px = x, py = y;
        while (px >= 0 && py >= 0 && px < N && py < N && b[py][px] === c) { n++; px += dx; py += dy; }
        if (n >= 5) return true;                        
        if (n === 4) {                                  
          var ex = x - dx, ey = y - dy;
          if (ex >= 0 && ey >= 0 && ex < N && ey < N && !b[ey][ex] &&
              px >= 0 && py >= 0 && px < N && py < N && !b[py][px]) return true;
        }
      }
    }
    return false;
  }
  
  function exactFiveFor(moverColor) {
    var rule = S.rule;
    if (rule === 1) return true;
    if (rule === 2 || rule === 6 || rule === 7 || rule === 8) return moverColor === 1;
    return false;
  }

  // ★ 2026-10-13：**和主界面 calc.js 的 engineRule() 同口径**。
  //   原生引擎只认 rule = 0/1/2/5；本面板历史上用 6/7/8 表示禁手族。以前这里把
  //   S.rule **原样**传下去，engine-ai.js 的 doBalance/doAnalyze 会做
  //   `[0,1,2,5].indexOf(rule)>=0 ? rule : 0` —— 6/7/8 被静默降级成 0（无禁手），
  //   于是「有禁手」的局面拿到的是无禁手的平衡点/评估。统一在这里映射，
  //   保证「有禁手就是有禁手」（用户：「一定要分清有禁手和无禁手」）。
  function engRuleOf(r) {
    var v = +r;
    if (v === 6 || v === 7 || v === 8) return 2;
    return (v === 0 || v === 1 || v === 2 || v === 5) ? v : 0;
  }

  function hasFiveOnBoard(m) {
    var b = m || lastBoardMatrix();
    var DIR = [[1, 0], [0, 1], [1, 1], [1, -1]];
    for (var y = 0; y < N; y++) for (var x = 0; x < N; x++) {
      var c = b[y][x];
      if (!c) continue;
      for (var d = 0; d < 4; d++) {
        var dx = DIR[d][0], dy = DIR[d][1];
        if (y - dy >= 0 && y - dy < N && x - dx >= 0 && x - dx < N && b[y - dy][x - dx] === c) continue;
        var n = 0, px = x, py = y;
        while (px >= 0 && py >= 0 && px < N && py < N && b[py][px] === c) { n++; px += dx; py += dy; }
        if (exactFiveFor(c) ? (n === 5) : (n >= 5)) return true;
      }
    }
    return false;
  }

  function myColOf() { return (S.side === 0) ? 1 : 2; }

  function diag(text) { tellHost({ type: 'log', text: String(text) }); }
  
  function boardDumpText() {
    var m = lastBoardMatrix(), out = [], nb = 0, nw = 0;
    for (var y = N - 1; y >= 0; y--) {
      var row = '';
      for (var x = 0; x < N; x++) {
        var c = m[y][x];
        if (c === 1) { row += '#'; nb++; }
        else if (c === 2) { row += 'O'; nw++; }
        else row += '.';
      }
      out.push((y + 1) + ':' + row);
    }
    return 'board B=' + nb + ' W=' + nw + '\n' + out.join(' | ');
  }

  function findOpenFour(m) {
    var b = m || lastBoardMatrix();
    var DIR = [[1, 0], [0, 1], [1, 1], [1, -1]];
    for (var y = 0; y < N; y++) for (var x = 0; x < N; x++) {
      var c = b[y][x];
      if (!c) continue;
      for (var d = 0; d < 4; d++) {
        var dx = DIR[d][0], dy = DIR[d][1];
        if (y - dy >= 0 && y - dy < N && x - dx >= 0 && x - dx < N && b[y - dy][x - dx] === c) continue;
        var cells = [[x, y]], px = x + dx, py = y + dy;
        while (px >= 0 && py >= 0 && px < N && py < N && b[py][px] === c) {
          cells.push([px, py]); px += dx; py += dy;
        }
        if (cells.length !== 4) continue;
        var ex = x - dx, ey = y - dy;
        if (ex < 0 || ey < 0 || ex >= N || ey >= N || b[ey][ex]) continue;
        if (px < 0 || py < 0 || px >= N || py >= N || b[py][px]) continue;
        return { color: c, cells: cells };
      }
    }
    return null;
  }

  function lastBoardMatrix() {
    var m = [];
    for (var y = 0; y < N; y++) { m.push([]); for (var x = 0; x < N; x++) m[y].push(0); }
    (lastBl || []).forEach(function (p) { if (m[p.y] && m[p.y][p.x] !== undefined) m[p.y][p.x] = 1; });
    (lastWh || []).forEach(function (p) { if (m[p.y] && m[p.y][p.x] !== undefined) m[p.y][p.x] = 2; });
    return m;
  }

  function findWinningFive(m, side) {
    var out = [], DIR = [[1, 0], [0, 1], [1, 1], [1, -1]];
    function cnt(x, y, dx, dy, who) {
      var n = 0;
      while (x >= 0 && x < N && y >= 0 && y < N && m[y][x] === who) { n++; x += dx; y += dy; }
      return n;
    }
    for (var y = 0; y < N; y++) for (var x = 0; x < N; x++) {
      if (m[y][x] !== 0) continue;
      m[y][x] = side;
      var maxRun = 0;
      for (var d = 0; d < 4; d++) {
        var total = cnt(x, y, DIR[d][0], DIR[d][1], side) + cnt(x, y, -DIR[d][0], -DIR[d][1], side) - 1;
        if (total > maxRun) maxRun = total;
      }
      
      if (exactFiveFor(side) ? (maxRun === 5) : (maxRun >= 5)) out.push([x, y]);
      m[y][x] = 0;
    }
    return out;
  }

  // ★★ 2026-10-17 战术加固层（移植自练习器 calc.js 的 aiTurnBudget / mustAnswer /
  //    nearStoneGuard —— 用户「智力上远不如 4.39」的机制就在这里）。
  //    连线窗以前是**引擎裸输出**：固定 2 秒、不做必应、不做近点护栏。同样的引擎、
  //    同样的 12 线程，练习器会「对手成五就加时 1.5×、必堵必应、开局不飞到天涯海角」，
  //    连线窗全都没有 ⇒ 同一个局面连线显得更笨。加固只在**引擎给出候选之后**兜底，
  //    因此永远不会把一个正常的好手强行换掉（只接管「明显必应/必赢」的情形）。
  function bNearAny(m, x, y) {
    for (var dy = -3; dy <= 3; dy++) for (var dx = -3; dx <= 3; dx++) {
      var px = x + dx, py = y + dy;
      if (px < 0 || py < 0 || px >= N || py >= N) continue;
      if (m[py][px]) return true;
    }
    return false;
  }

  // 活四点 = 落子后同时形成 ≥2 个成五点（两路都成五，对手只能堵一路）
  function bOpenFourPoints(m, side) {
    var out = [];
    for (var y = 0; y < N; y++) for (var x = 0; x < N; x++) {
      if (m[y][x] || !bNearAny(m, x, y)) continue;
      m[y][x] = side;
      var w = findWinningFive(m, side);
      m[y][x] = 0;
      if (w.length >= 2) out.push([x, y]);
    }
    return out;
  }

  function bInList(list, x, y) {
    for (var i = 0; i < (list || []).length; i++)
      if (list[i][0] === x && list[i][1] === y) return true;
    return false;
  }

  // 必应：① 行棋方成五点直接赢 ② 对手唯一成五点必堵 ③ 对手活四且本方无四 → 破活四
  function bMustAnswer(m, toMove, cands, oppWin, oppFour, myFour) {
    var myWin = findWinningFive(m, toMove);
    if (myWin.length) return { x: myWin[0][0], y: myWin[0][1], why: 'win' };
    function inCands(p) {
      for (var i = 0; i < (cands || []).length; i++)
        if (cands[i].x === p[0] && cands[i].y === p[1]) return true;
      return false;
    }
    if (oppWin && oppWin.length === 1 && !inCands(oppWin[0]))
      return { x: oppWin[0][0], y: oppWin[0][1], why: 'block5' };
    if (oppFour && oppFour.length && !(myFour && myFour.length)) {
      for (var i = 0; i < (cands || []).length; i++)
        if (bInList(oppFour, cands[i].x, cands[i].y)) return null;   // 引擎已选破点
      return { x: oppFour[0][0], y: oppFour[0][1], why: 'block4' };
    }
    return null;
  }

  // 近点护栏：开局（≤10 子）不把子下到离所有棋子 >3 的地方
  function bNearGuard(m, x, y) {
    var n = 0;
    for (var j = 0; j < N; j++) for (var i = 0; i < N; i++) if (m[j][i]) n++;
    if (n === 0 || n > 10) return true;
    return bNearAny(m, x, y);
  }

  // 威胁感知预算 —— ★★ 2026-10-19 回归 4.36 的 aiTurnBudget：恢复威胁乘数
  //   （对手成五点 ×1.5、对手活四 ×1.25、行棋方自己已成五/活四 ×0.8 快出）。
  //   结果同时供必应兜底（bMustAnswer）使用。
  function tacPlan(m, toMove, baseMs) {
    var out = { ms: baseMs, why: '', oppWin: null, oppFour: null, decided: boardDecided(m) };
    if (out.decided) return out;
    out.oppWin = findWinningFive(m, 3 - toMove);
    if (out.oppWin.length) { out.ms = Math.round(baseMs * 1.5); out.why = 'opp5'; return out; }
    out.oppFour = bOpenFourPoints(m, 3 - toMove);
    if (out.oppFour.length) { out.ms = Math.round(baseMs * 1.25); out.why = 'opp4'; return out; }
    if (findWinningFive(m, toMove).length) { out.ms = Math.round(baseMs * 0.8); out.why = 'my5'; }
    return out;
  }

  function myIsToMove(bl, wh) {
    if (S.side !== 0 && S.side !== 1) return false;
    return sideToMove(bl, wh) === myColOf();
  }

  // 本手送入引擎的 side（1=黑 2=白）。★ 4.36 口径：子数相等 ⇒ 黑行棋（自含实现）。
  function sideToMove(bl, wh) { return (bl.length <= wh.length) ? 1 : 2; }

  // ★★ 2026-10-19 回归 4.36：**eval 原样透传，不做任何翻号换算**。
  //   4.36 的界面（fmtEval）就是把引擎原文直接格式化展示 —— 没有换算层，也就没有
  //   「正负颠倒」这类 bug 的容身之处。数值化（曲线/平衡判定用）才解析：mate 记 ±90000。
  function evalForSide(rawEval, rootSide, viewSide) {
    var s = (typeof rawEval === 'string') ? rawEval.trim() : '';
    return s || '-';
  }
  function evalNumOf(rawEval, rootSide, viewSide) {
    var s = (typeof rawEval === 'string') ? rawEval.trim() : '';
    var v = 0;
    var m = /^([+-]?)M(\d+)/i.exec(s);
    if (m) v = (m[1] === '-') ? -90000 : 90000;
    else {
      var n = parseFloat(s.replace('+', ''));
      if (isNaN(n)) return 0;
      v = Math.max(-90000, Math.min(90000, n));
    }
    return (rootSide === viewSide) ? v : -v;
  }

  function coordOf(x, y) { return String.fromCharCode(65 + x) + (N - y); }

  // ★ 2026-10-18：「等待超时」与「只回裸着法行」两条路径共用同一条**有界重算**
  //   （1.5s 起、最多 8 次）。两者都是「这一拍没拿到有搜索依据的结果」，
  //   按既有口径必须重算，绝不能把这手当候选直接落下。
  function scheduleAnalyzeRetry() {
    if (analyzeRetryTimer) return;
    var tryRetry = function () {
      analyzeRetryTimer = null;
      if (!engineBusy && !swBusy && !balBusy && boardGeo) {
        analyzeRetryCount = 0;
        analyze(lastBl, lastWh);
      } else if (boardGeo) {
        if (++analyzeRetryCount < 8) analyzeRetryTimer = setTimeout(tryRetry, 1500);
        else analyzeRetryCount = 0;
      }
    };
    analyzeRetryCount = 0;
    analyzeRetryTimer = setTimeout(tryRetry, 1500);
  }

  // ★ 2026-10-18：本手搜索预算 = max(用户设定, 对手上一手思考时长)，上限 15s。
  //   对手在等 = 我们的免费思考窗口。**「不超标越界」的边界**：一分不多占 ——
  //   对手想多久我们就想多久（同一盘棋的总时钟不会被我们吃掉更多），
  //   多的只是「同样的时间用在满核 + 512MB~6GB 大表 + PONDERING 的 Rapfi 上」。
  // ★★ 2026-10-18（用户：「我设置的是 2 秒，它居然算了 4 秒」）：**预算严格 = 用户设置**。
  //   旧实现是「对手上一手想了多久，我们就想多久（下限 = 用户设置、上限 15s）」——
  //   对手想 4 秒，我们就搜 4 秒，用户看到的思考时长和他自己设的 2 秒完全对不上。
  //   对手思考时长仍然采集（linkOppThinkMs / LINK_BUDGET 诊断），但**不参与预算**。
  // ★ 2026-10-19 回归 4.36：预算 = 用户设置（下限 300，与训练器 aiTurnBudget 同款）；
  //   威胁乘数由 tacPlan 负责（×1.5 / ×1.25 / ×0.8）。
  function linkTurnBudget() {
    return Math.max(300, S.turnMs || 2000);
  }

  async function analyze(bl, wh) {
    var fpNow = boardKey(bl, wh);
    if (engineBusy) { pendingEvalKey = fpNow; return; }   
    engineBusy = true;
    try {
      var ply = bl.length + wh.length;
      // ★ 一个盘面只判一次轮次：这一帧送进引擎的 side、本手「是不是我」、评分视角
      //   全部从 rootSide 推出来（以前三处各判一次，任一处分叉就是「正负颠倒」）。
      var rootSide = sideToMove(bl, wh);
      var mySide = myColOf();
      // 「是不是我这一手」现在只用于语义判断；评分视角一律用 (rootSide, mySide) 两个事实。

      var turnMsNow = linkTurnBudget();
      bridgeDiag('LINK_BUDGET base=' + (S.turnMs || 0) + ' opp=' + linkOppThinkMs +
                       ' ms=' + turnMsNow + ' ad=' + (adOn ? 1 : 0));


      // ── 威胁感知预算（练习器 aiTurnBudget 同款）：对手有成五点 → ×1.5；
      //    对手有活四 → ×1.25；行棋方自己已成五点 → ×0.8（已必胜，快出即可）。
      var tmc = rootSide;                                  // 行棋方颜色
      var bmTac = boardMatrix(bl, wh);
      var _tac = tacPlan(bmTac, tmc, turnMsNow);
      turnMsNow = _tac.ms;
      var _oppWin = _tac.oppWin, _oppFour = _tac.oppFour;
      if (_tac.why) { bridgeDiag('LINK_TACTIC budget why=' + _tac.why + ' ms=' + turnMsNow); }
      
      // ── 完全信任引擎落点（2026-10-19 用户：「不如官方整合包，应该是调用的算法层面的问题」
      //    —— 去掉 JS 战术覆盖/近点护栏/必应兜底，不再改写引擎的首选）。评估视角一律用
      //    (rootSide, mySide) 两个事实换算；预算只由 tacPlan 的威胁乘数调整，不影响落点。

      // ── 每一轮结果的展示（mid = 加深过程中，final = 收官）：
      //    分数/深度/曲线/推荐落点全部实时刷；只有 final 才写 lastBest/lastEvalFp 并（自动落子时）落子。
      var publish = function (r, final) {
        var c00 = r.candidates[0];
        S.rawEval = c00.eval || '';                                  // 引擎原文（行棋方视角）
        S.myEval = evalForSide(c00.eval, rootSide, mySide);          // 唯一换算
        S.candCache = r.candidates;
        S.history = S.history.filter(function (p) { return p.i !== ply; });
        S.history.push({ i: ply, v: evalNumOf(c00.eval, rootSide, mySide) });
        S.history.sort(function (a, b) { return a.i - b.i; });
        S.lastStat = { c0: { depth: c00.depth, speed: c00.speed, nodes: c00.nodes, line: (c00.line || []).map(function (p) { return [p[0], p[1]]; }) }, elapsed: 0 };
        ui('setStatPanel', S.lastStat.c0, 0);
        ui('drawChart');
        var bx = r.best || [c00.x, c00.y];                           // 引擎首选，零改写
        ui('setStatus', (final ? (S.lang === 'en' ? 'Best move ' : '推荐落点 ')
                              : (S.lang === 'en' ? 'Analyzing… best ' : '分析中… 当前最佳 ')) +
          '<b>' + coordOf(bx[0], bx[1]) + '</b>' +
          (S.lang === 'en' ? ' (my eval ' : '（我方评估 ') + S.myEval +
          (S.lang === 'en' ? ') · depth ' : '）· 深度 ') + (c00.depth || '-'));
        postGuide(true, bx[0], bx[1]);
        if (final) {
          lastBest = bx; lastMainC0 = c00; lastEvalFp = fpNow;
          if (adOn) adMaybeFire(bx[0], bx[1]);
        }
        return bx;
      };

      // 引擎只回一行裸着法（PV 为空 ⇒ 无搜索证据）且在 90% 预算前结案：按既有口径不落子 + 有界重算。
      var failRawOnly = function (r) {
        var c0r = r && r.candidates && r.candidates[0];
        if (c0r) { bridgeDiag('LINK_EVAL_RAWBEST ' + coordOf(c0r.x, c0r.y) + ' elapsed=' + (r.elapsed | 0)); }
        try { if (window.chrome && window.chrome.webview)
          tellHost({ type: 'status', text: 'LINK_EVAL_RAWBEST recalc' }); } catch (eRB2) {}
        ui('setStatus', '<span style="color:#e67e22">' +
          (S.lang === 'en' ? 'No search result (raw best line only) — recalculating…'
                           : '暂无搜索结果（只有裸着法行）—— 正在重算…') + '</span>');
        postGuide(false);
        scheduleAnalyzeRetry();
      };
      var failEmpty = function (r) {
        try { engineDiagDump('analyze-empty'); } catch (eGD) {}
        try { if (window.chrome && window.chrome.webview)
          tellHost({ type: 'status', text: 'LINK_EVAL_EMPTY ' + (r && r.error ? String(r.error) : 'no candidates') }); } catch (eD) {}
        ui('setStatus', '<span style="color:#c0392b">' +
          (S.lang === 'en' ? 'Engine returned no move' : '引擎未返回着法') +
          (r && r.error ? (S.lang === 'en' ? ': ' : '：') + r.error : '') + '</span>');
        postGuide(false);
      };

      var laneSlack = (swBusy || balBusy) ? (SWBAL_BUDGET_MS + 800) : 0;
      // ★★ 2026-10-18（用户：「2 秒内会重新评估多次，没有充分思考，很多都是失误点；要把内耗
      //   换成能算得更准落点的能力」）：**一手只许一次 YXNBEST**。
      //   旧实现是 while 分轮（首轮取预算 45%，后续取剩余 60%），日志实测 2 秒被切成 3 次独立搜索：
      //     同一盘面 `TIMEOUT_TURN 1719`，1.99s 后又是一次 `TIMEOUT_TURN 1096`。
      //   每一轮都要 STOP 掉上一轮留下的 ponder、等输出静默（上限 1500ms）、再重新发 YXBOARD ——
      //   Rapfi 的迭代加深只能从浅层重走一遍，而递减的 TIMEOUT_TURN 又让它自己的时间管理越来越保守。
      //   净效果：名义预算 2 秒，真正连续的搜索只有 0.9~1.7 秒 ⇒ 深度掉、优势点被算偏。
      //   现在：全额预算一次下发；中途由 waitForMove 的 onTick 把深度/推荐点实时刷出去（**不重搜**）。
      var lastTickAt = 0;
      var onTick = function (t) {
        var nowMs = Date.now();
        if (nowMs - lastTickAt < 200) return;
        lastTickAt = nowMs;
        try {
          ui('setStatus', (S.lang === 'en' ? 'Analyzing… best ' : '分析中… 当前最佳 ') +
            '<b>' + coordOf(t.x, t.y) + '</b>' +
            (S.lang === 'en' ? ' · depth ' : ' · 深度 ') + (t.depth || '-'));
          postGuide(true, t.x, t.y);
          var mv = evalForSide(t.eval, rootSide, mySide);
          S.myEval = mv;
          S.history = S.history.filter(function (p) { return p.i !== ply; });
          S.history.push({ i: ply, v: evalNumOf(t.eval, rootSide, mySide) });
          ui('drawChart');
        } catch (eTK) {}
      };
      var res = await onMainLane(function () {
        // side = 行棋方颜色。**必须与上面 rootSide 是同一个值**（同一个盘面只判一次轮次），
        // 否则引擎搜的是另一方，返回的落点是对手要走的那一手、分数也是对手视角。
        return wsCall('analyze', {
          board: boardMatrix(bl, wh), moveList: [],
          matchMs: S.matchMs || 600000, turnMs: turnMsNow,
          timeUsedMs: 0, topN: 1, rule: engRuleOf(S.rule), cid: 'desktop-' + ply,
          side: rootSide,
          exclusive: true,
          onTick: onTick,
        }, Math.max(12000, turnMsNow + 12000) + laneSlack);
      });
      if (res && res.superseded) { postGuide(false); return; }
      var c0raw = res && res.candidates && res.candidates[0];
      var _early = (typeof res.elapsed !== 'number') || (res.elapsed < turnMsNow * 0.9);
      var _rawOnly = !!c0raw && !(c0raw.line && c0raw.line.length) && _early;
      if (_rawOnly) return failRawOnly(res);
      if (!res || res.error || !res.candidates || !res.candidates.length) return failEmpty(res);
      if (fpNow !== boardKey(lastBl, lastWh)) return;    // 这一手的结论已过期，作废
      // ★ 诊断口径对齐：root / raw / my 三个值一起打，任何一次符号问题都能一眼定位。
      bridgeDiag('LINK_ANALYZE one-shot turnMs=' + turnMsNow +
                       ' elapsed=' + (res.elapsed | 0) + ' depth=' + ((c0raw && c0raw.depth) || 0) +
                       ' root=' + (rootSide === 1 ? 'B' : 'W') + ' my=' + (mySide === 1 ? 'B' : 'W') +
                       ' raw=' + (c0raw && c0raw.eval ? c0raw.eval : '-') +
                       ' myEval=' + evalForSide(c0raw && c0raw.eval, rootSide, mySide));
      publish(res, true);               // 收官：写 lastBest 并（自动落子时）落子
    } catch (e) {
      
      var em = String((e && e.message) || e);
      
      if (isWaitTimeout(e)) {
        
        if (swBusy || balBusy) {
          ui('setStatus', '<span style="color:#e67e22">' + LT('analyzeRetry') + '</span>');
        } else {
          try { console.warn('[bridge] analyze wait timeout (silent retry):', em); } catch (eC) {}
        }
        postGuide(false);
        scheduleAnalyzeRetry();
      } else {
        try { natEng().diag('analyze-fail'); } catch (eG) {}
        try { if (window.chrome && window.chrome.webview)
          tellHost({ type: 'status', text: 'LINK_EVAL_FAIL ' + String(errText(e)) });
        } catch (eD) {}
        
        ui('setStatus', '<span style="color:#c0392b">' +
          (S.lang === 'en' ? 'Analysis failed: ' : '分析失败：') + errText(e) + '</span>');
        postGuide(false);
      }
    } finally {
      engineBusy = false;
      
      if (pendingEvalKey && pendingEvalKey !== boardKey(lastBl, lastWh) && boardGeo) {
        var pk = pendingEvalKey; pendingEvalKey = null;
        analyze(lastBl, lastWh);
      } else {
        pendingEvalKey = null;
      }
    }
  }

  function boardToScreen(x, y) {
    if (!boardGeo) return null;
    var xs = boardGeo.x_lines, ys = boardGeo.y_lines;
    if (!xs || !ys || xs.length <= x || ys.length <= y) return null;
    return { x: Math.round(xs[x]), y: Math.round(ys[y]) };
  }

  function boardGap() {
    try {
      var xs = boardGeo && boardGeo.x_lines;
      if (xs && xs.length > 1) {
        var g = Math.abs(xs[1] - xs[0]);
        if (g > 4) return g;
      }
      if (boardGeo && boardGeo.spacing > 4) return boardGeo.spacing;
    } catch (e) {}
    return 0;
  }

  function postGuide(on, gx, gy) {
    if (!HOST) return;
    if (!on) { tellHost({ type: 'guide', on: false }); return; }
    var p = boardToScreen(gx, gy);
    if (!p) { tellHost({ type: 'guide', on: false }); return; }
    var gap = boardGap();
    tellHost({ type: 'guide', on: true, x: p.x, y: p.y, r: gap > 4 ? Math.round(gap * 0.46) : 0 });
  }

  var heatBusy = false;
  
  var lastBest = null;      
  var lastMainC0 = null;    

  function heatLabel(ev) {
    var s = String(ev == null ? '' : ev).trim().replace(/[^0-9A-Za-z+\-]/g, '');
    return s.length > 6 ? s.slice(0, 6) : s;
  }

  function heatTiers(list) {
    var n = list.length, out = [], i;
    if (!n) return out;
    if (n === 1) return [1];
    for (i = 0; i < n; i++) {
      var t = 1 + Math.floor(i * 4 / n);
      out.push(Math.max(1, Math.min(4, t)));
    }
    return out;
  }

  async function refreshHeatOverlay(bl, wh) {
    if (!HOST) return;
    if (!S.heatmap || !boardGeo || heatBusy) {
      if (!S.heatmap) tellHost({ type: 'heat', on: false });
      return;
    }
    heatBusy = true;
    var fp = boardKey(bl, wh);
    try {
      var refreshMs = Math.max(1200, Math.min(4000, Math.round((S.turnMs || 5000) * 0.5)));
      var ask = function (ms) {
        return wsCall('analyze', {
          board: boardMatrix(bl, wh), moveList: [],
          matchMs: S.matchMs || 600000, turnMs: ms, timeUsedMs: 0,
          topN: 8, rule: engRuleOf(S.rule), cid: 'desktop-heat', lane: 'sub',
        }, ms + 6000);
      };
      var res = await ask(refreshMs);
      if (fp !== lastBoardKey) return;                 
      if (res && res.candidates && res.candidates.length < 4) {
        try {
          var res2 = await ask(Math.min(6000, refreshMs + 1500));
          if (fp === lastBoardKey && res2 && !res2.error && res2.candidates &&
              res2.candidates.length > res.candidates.length) res = res2;
        } catch (e2) {}
      }
      if (res && res.superseded) { tellHost({ type: 'heat', on: false }); return; }
      if (!S.heatmap || !res || res.error || !res.candidates || !res.candidates.length) {
        tellHost({ type: 'heat', on: false });
        return;
      }
      var cands = res.candidates.slice(0, 8);
      if (lastBest && lastBest.length === 2) {          
        for (var i = 1; i < cands.length; i++) {
          if (cands[i].x === lastBest[0] && cands[i].y === lastBest[1]) {
            cands.unshift(cands.splice(i, 1)[0]);
            break;
          }
        }
      }
      if (lastMainC0 && cands.length &&
          cands[0].x === lastMainC0.x && cands[0].y === lastMainC0.y) {
        if (lastMainC0.eval) cands[0].eval = lastMainC0.eval;
        if (lastMainC0.depth) cands[0].depth = lastMainC0.depth;
      }
      S.candCache = cands;
      // 热力图标签的分值也必须走同一个换算：rootSide = 这一帧的行棋方。
      var heatRoot = sideToMove(bl, wh), heatMine = myColOf();

      var tiers = heatTiers(cands);
      var parts = [];
      for (var ci = 0; ci < cands.length; ci++) {
        var c = cands[ci];
        var p = boardToScreen(c.x, c.y);
        if (!p) continue;
        var ev = evalForSide(c.eval, heatRoot, heatMine);
        
        parts.push(p.x + ',' + p.y + ',' + tiers[ci] + ',' + heatLabel(ev));
      }
      if (!parts.length) { tellHost({ type: 'heat', on: false }); return; }
      tellHost({ type: 'heat', on: true, gap: Math.round(boardGap()), cells: parts.join(';') });
    } catch (e) {
      try { tellHost({ type: 'heat', on: false }); } catch (e2) {}
    } finally { heatBusy = false; }
  }

  var balMarks = [];              
  var balSticky = false;          
  var balToggleArmed = false;     
  var lastOpen4Toast = 0;         
  var balLastKey = '';
  
  var balBaseB = 0, balBaseW = 0, balNeedB = 0, balNeedW = 0;
  function balSet(cells, sticky, key, colors) {
    balMarks = cells.map(function (c) { return { x: c[0], y: c[1] }; });
    balSticky = !!sticky;
    balToggleArmed = false;
    balLastKey = key || '';
    balBaseB = (lastBl || []).length;
    balBaseW = (lastWh || []).length;
    balNeedB = 0; balNeedW = 0;
    for (var bi = 0; bi < (colors ? colors.length : 0); bi++) {
      if (colors[bi] === 'b') balNeedB++;
      else if (colors[bi] === 'w') balNeedW++;
    }
  }
  
  function balOff() {
    balMarks = []; balSticky = false; balToggleArmed = false; balLastKey = '';
    balBaseB = balBaseW = balNeedB = balNeedW = 0;
    tellHost({ type: 'balance', on: false });
    swBtnState();                                 
  }
  
  function balOnBoardChanged(newBl, newWh) {
    if (!balMarks.length) return;
    if (!balSticky) { balOff(); return; }
    var nbNow = newBl ? newBl.length : 0, nwNow = newWh ? newWh.length : 0;
    var nM = balMarks.length;
    
    if (balNeedB + balNeedW > 0 &&
        (nbNow - balBaseB) >= balNeedB && (nwNow - balBaseW) >= balNeedW) { balOff(); return; }
    if (nM >= 3) {
      var tot = nbNow + nwNow;
      if (tot >= nM) { balOff(); return; }
    }
    var occ = {};
    for (var i = 0; i < (newBl ? newBl.length : 0); i++) occ[newBl[i][0] + ',' + newBl[i][1]] = 1;
    for (var j = 0; j < (newWh ? newWh.length : 0); j++) occ[newWh[j][0] + ',' + newWh[j][1]] = 1;
    var left = balMarks.filter(function (m) { return !occ[m.x + ',' + m.y]; });
    if (left.length !== balMarks.length) balMarks = left;
    if (!balMarks.length) balOff();     
  }

  var adOn = false, adCount = 1, adWaiting = false, adFail = 0, adTimer = null;
  
  var adStage = 0;
  
  var adRound = 0;                        
  var adDone = 0;                        
  var adLastPt = null;                   
  
  var lastAdPipe = '';
  var adHadStones = false;               
  try { var _adC = parseInt(localStorage.getItem('gb_ad_count') || '1', 10); if (_adC >= 1 && _adC <= 3) adCount = _adC; } catch (e) {}
  var adFailKey = null;                 
  var adRetryUsed = false;              
  var adRetryWatch = null, adRetryWatchN = 0; 
  var newGamePending = false;              
  var plyPrev = 0;                         
  // ★ 2026-10-16：空盘「进入沿」只做一次清表/复位（空盘帧每 ~50ms 来一次，不能每次都清）。
  var emptyResetDone = false;

  function adBtnSync() {
    var b = document.getElementById('__gb_autodrop'), c = document.getElementById('__gb_adcount');
    if (b) b.setAttribute('data-ad-on', adOn ? '1' : '0');
    if (c) c.textContent = '×' + adCount;
    swBtnState();
  }

  function swBtnState() {
    var b1 = document.getElementById('__gb_swbal'), b2 = document.getElementById('__gb_autodrop');
    if (b1) b1.setAttribute('data-dis', adOn ? '0' : '1');
    if (b2) b2.setAttribute('data-dis', swQueue ? '1' : '0');
  }

  function adNewGameOff() {
    newGamePending = true;                 
    swQueue = null; swIdx = 0;
    if (!adOn) return;
    adOn = false;
    adWaiting = false; adFail = 0; adStage = 0; adRound = 0; adDone = 0; adLastPt = null; adFailKey = null; adRetryUsed = false;
    if (adRetryWatch) { clearTimeout(adRetryWatch); adRetryWatch = null; } adRetryWatchN = 0;
    if (adTimer) { clearTimeout(adTimer); adTimer = null; }
    adBtnSync();
    tellHost({ type: 'autodrop', on: false });
    ui('setStatus', '<b style="color:#555">' + LT('adOffNewGame') + '</b>');
  }

  function adArmTimeout() {
    if (adTimer) clearTimeout(adTimer);
    adTimer = setTimeout(function () {
      adTimer = null;
      if (!adWaiting) return;
      adWaiting = false;
      
      if (!adRetryUsed) {
        adRetryUsed = true;
        
        if (boardKey(lastBl, lastWh) !== lastBoardKey) { adLastPt = null; return; }
        ui('setStatus', '<span style="color:#e67e22;">' + LT('adRetry') + '</span>');
        if (swQueue) { adStage = Math.min(2, (adStage || 0) + 1); swAdvance(); }
        else if (adLastPt) adFire(adLastPt[0], adLastPt[1], false, false, adRealOnly() ? 2 : 1);
      } else {
        
        var curPt = adLastPt ? (adLastPt[0] + ',' + adLastPt[1]) : '';
        adLastPt = null;                 
        if (curPt && curPt === adFailKey) {
          ui('setStatus', '<span style="color:#c0392b">' + LT('adPtUnreachable') + '</span>');
        } else {
          adFailKey = curPt;
          adFail++;
          if (adFail >= 3) {
            adOn = false; swQueue = null; adBtnSync();
            ui('setStatus', '<span style="color:#c0392b">' + LT('adGiveUp') + '</span>');
          } else {
            ui('setStatus', '<span style="color:#c0392b">' + LT('adGiveUp') +
              ' (' + adFail + '/3)</span>');
          }
        }
        
        if (!adRetryWatch && adOn && boardGeo) {
          adRetryWatch = setTimeout(function () {
            adRetryWatch = null;
            if (!adOn || !boardGeo) return;
            if (++adRetryWatchN >= 3) { adRetryWatchN = 0; return; }
            ui('setStatus', '<span style="color:#e67e22;">' +
              (S.lang === 'en' ? 'Retrying this position…' : '重试当前局面…') + '</span>');
            analyze(lastBl, lastWh);
          }, 3000);
        }
      }
    }, 4000);
  }

  function adConfirm() {
    if (adTimer) { clearTimeout(adTimer); adTimer = null; }
    if (!adWaiting) return;
    adWaiting = false; adFail = 0; adRetryUsed = false;
    if (adRetryWatch) { clearTimeout(adRetryWatch); adRetryWatch = null; } adRetryWatchN = 0;
    
    if (lastAdPipe) tellHost({ type: 'adPipeOk', pipe: lastAdPipe });
    adStage = 0;
    adRound = 0; adFailKey = null;        
    if (swQueue) {                       
      swIdx++;
      if (swIdx >= swQueue.length) {
        var nAll = swQueue.length; swQueue = null;
        ui('setStatus', '<b style="color:#0e7490">' + LT('swPlaced').replace('%1', nAll) + '</b>');
        swBtnState();                            
      } else {
        setTimeout(swAdvance, 120);      
      }
      return;
    }
    adDone++;
    ui('setStatus', '<b style="color:#28a745">' + LT('adDropped') + ' ×' + adDone + '</b>');
  }

  function adMaybeFire(gx, gy) {
    if (!adOn || swQueue || adWaiting) return;
    if (adLastPt && adLastPt[0] === gx && adLastPt[1] === gy) return;
    adLastPt = [gx, gy];
    adFire(gx, gy, false);
  }
  
  function adRealOnly() {
    var c = document.getElementById('__gb_realclick');
    return !!(c && (c.checked === true || c.getAttribute('data-on') === '1'));
  }
  
  function adFire(gx, gy, force, once, stage) {
    var p = boardToScreen(gx, gy);
    if (!p) return;
    var cnt = once ? 1 : adCount;
    var realOnly = adRealOnly();
    
    var wantStage = (typeof stage === 'number') ? stage : (force ? 1 : 0);
    adStage = realOnly ? 2 : Math.min(1, wantStage);
    
    adRetryUsed = realOnly || !!force || (typeof stage === 'number' && stage >= 1);
    adWaiting = true;
    adArmTimeout();
    
    tellHost({ type: 'autodrop', on: true, x: p.x, y: p.y, count: cnt,
               force: adStage >= 1, real: adStage >= 2 });
    ui('setStatus', '<span style="color:#28a745">' + LT('adFired') + coordOf(gx, gy) +
      (adStage === 1 ? ' [input]' : (adStage === 2 ? ' [mouse]' : '')) + '</span>');
  }

  var swQueue = null, swIdx = 0;         
  var pendingBal = null;                 
  var balPointN = 2;
  try { var _bpn = parseInt(localStorage.getItem('gb_bal_point_n') || '2', 10); if (_bpn >= 1 && _bpn <= 6) balPointN = _bpn; } catch (e) {}
  var swBusy = false, swEvalBusy = false;
  // ★★ 2026-10-19（用户：「在交换平衡点、交换评估后面再添加一个取消按键，点击取消后，可以停止
  //   交换平衡的计算，以及清除上面的平衡标记，恢复正常的初始连线状态」）：
  //   swGen 是交换平衡/评估的**取消令牌**。runSwapBal 在 await 之后验令牌，一旦不等就直接
  //   return（连同 finally 复位 busy）—— 否则「取消」后引擎虽然被停，函数体醒来还会继续
  //   摆平衡标记、把 balPts 塞进 swQueue 排队落子，等于白取消。
  //   runSwapEval 不用改：cancelAll 给它的结果带 superseded=true，它本来就有早退分支。
  var swGen = 0;
  function swapCancel() {
    swGen++;                                     // 令在途的 runSwapBal 到期即退
    try { natEng().cancelAll(); } catch (e) {}   // 中断引擎搜索（宿主 op:'stop'）+ 让等待方立刻返回
    swQueue = null; swIdx = 0;                   // 清掉已排序的平衡落点
    adWaiting = false; pendingBal = null;
    adFail = 0; adFailKey = null;                // 取消不计入自动落子的失败计数
    if (adTimer) { clearTimeout(adTimer); adTimer = null; }
    if (adRetryWatch) { clearTimeout(adRetryWatch); adRetryWatch = null; } adRetryWatchN = 0;
    swQuietSide = false;
    balOff();                                    // 清平衡标记（含宿主 overlay 的 balance on:false）
    try { swBtnState(); } catch (e2) {}
    try { adBtnSync(); } catch (e3) {}
    bridgeDiag('LINK_SWAP_CANCEL');
    return true;
  }
  try { window.GB_LINK_SWAPCANCEL = swapCancel; } catch (eW) {}
  
  var swQuietSide = false;
  
  function sideHintMuted() { return swQuietSide; }
  
  var SWBAL_BUDGET_MS = 7500;      

  function swAdvance() {
    if (!swQueue || !adOn) return;
    if (swIdx >= swQueue.length) { swQueue = null; return; }
    var pt = swQueue[swIdx];
    var p = boardToScreen(pt[0], pt[1]);
    if (!p) { swQueue = null; return; }
    adWaiting = true;
    
    adRetryUsed = adRealOnly() || (adStage >= 1);
    adArmTimeout();
    
    adStage = stg;                        
    var stg = adRealOnly() ? 2 : Math.min(1, adStage);
    tellHost({ type: 'autodrop', on: true, x: p.x, y: p.y, count: adCount,
               force: stg >= 1, real: stg >= 2 });
    ui('setStatus', '<span style="color:#0e7490">' +
      LT('swPlacing').replace('%1', swIdx + 1).replace('%2', swQueue.length) +
      coordOf(pt[0], pt[1]) + ')</span>');
  }

  async function runSwapBal() {
    if (swBusy || balBusy) return;
    
    if (!boardGeo) { ui('setStatus', '<span style="color:#e67e22;">' + T('no_board') + '</span>'); return; }
    
    var swPly = lastBl.length + lastWh.length;
    if (swPly !== 0 && swPly !== 3) {
      ui('setStatus', swTipCol(LT('swBalStage'), '#e67e22'));
      return;
    }
    swBusy = true;
    // ★ 2026-10-19：本次运行的取消令牌基线 —— 见 swapCancel 的注释。
    var swGen0 = swGen;
    var swDead = function () { return swGen0 !== swGen; };
    
    swQuietSide = true;
    if (S.side !== -1) setSide(-1);
    balOff();
    var empty = (lastBl.length + lastWh.length === 0);
    var toBlack = lastBl.length <= lastWh.length;        
    
    var budget = Math.max(1500, Math.min(SWBAL_BUDGET_MS, Math.round((S.turnMs || 5000) * 1.5)));
    
    var turnMs = Math.max(2000, Math.min(S.turnMs || 5000, 4500));
    // ★ 2026-10-13：走 engRuleOf（禁手族 6/7/8 → 2）；原来 6/7/8 会被降级成 0。
    var rule = engRuleOf(S.rule);
    
    var t0 = Date.now();
    var leftMs = function () { return budget - (Date.now() - t0); };
    try {
      ui('setStatus', '<span style="color:#0e7490">' + LT(empty ? 'swBalEmpty' : 'swBalPair') + '</span>');
      var marks = [];
      var balFromEngine = 0;      
      // ★ 2026-10-13：连线窗也要把**平衡引擎自己的**评估分报出来（以前只有坐标，
      //   用户只能去看面板那条对弈引擎的评估 —— 禁手下那是几百上千分的「对弈分」，
      //   被误当成「平衡点评估」就是「接近 1000 分的误差」的来源）。
      var balEv = null;
      if (empty) {
        
        // ★★ 2026-10-13 回退 + 分规则（用户：「连线功能里面的交换平衡点运用之前的逻辑（用定式，
        //   和中途第二手计算）（昨天 4.39 16点30 的那一版）」＋「无禁手公式+有禁手公式，
        //   只利用里面的平衡定式，必胜定式不可添加」）：
        //   空盘开局重新走**官方定式**，不再用原生成对搜索：
        //     ① 引擎按当前规则给官方开局（SWAP2BOARD，禁手/无禁手各有一套公式库）；
        //     ② 引擎不可用 ⇒ 退到下面这两张本地公式表（已按引擎实测逐条筛过）；
        //     ③ 再不行才退回中心区逐手。
        //   两张表必须分开：禁手(rule 2) 用 RIF 三手，无禁手用 Swap2 三手 —— 混用就是
        //   「一直用的是有禁手模式的平衡点」那个毛病。
        //   筛选依据（RapfiEngine-avx512vnni，跑 YXBALANCETWO 取「白这一手＋黑应手」这一对
        //   最接近 0 的分；|评估| ≤ 50 = 官方 balanceWindow 内的「平」）：
        //     无禁手公式 9 组  → |评估| 5~19，9/9 平衡，无必胜
        //     有禁手公式 26 组 → |评估| 1~17，26/26 平衡（RIF 这一族本身就是筛掉必胜后的开局）
        //   所以两张表都只保留平衡定式，必胜定式一条都不加。
        var BAL_FREE = [
          [[6, 7], [6, 4], [4, 2]],
          [[3, 3], [5, 5], [6, 6]],
          [[3, 2], [5, 4], [4, 5]],
          [[5, 2], [1, 5], [1, 6]],
          [[8, 5], [5, 8], [6, 7]],
          [[5, 5], [8, 8], [7, 7]],
          [[13, 12], [13, 9], [10, 12]],
          [[11, 7], [10, 6], [13, 5]],
          [[3, 7], [1, 8], [0, 4]],
        ];
        // RIF 26：白 2 落在 I9(8,8) 为斜（间接）、H9(7,8) 为直（直接），黑 1 恒在天元。
        var BAL_RENJU = [
          [[7, 7], [8, 8], [9, 9]], [[7, 7], [8, 8], [9, 8]], [[7, 7], [8, 8], [9, 7]],
          [[7, 7], [8, 8], [9, 6]], [[7, 7], [8, 8], [9, 5]], [[7, 7], [8, 8], [8, 7]],
          [[7, 7], [8, 8], [8, 6]], [[7, 7], [8, 8], [8, 5]], [[7, 7], [8, 8], [7, 6]],
          [[7, 7], [8, 8], [7, 5]], [[7, 7], [8, 8], [6, 6]], [[7, 7], [8, 8], [6, 5]],
          [[7, 7], [8, 8], [5, 5]],
          [[7, 7], [7, 8], [7, 9]], [[7, 7], [7, 8], [9, 9]], [[7, 7], [7, 8], [8, 8]],
          [[7, 7], [7, 8], [9, 8]], [[7, 7], [7, 8], [9, 7]], [[7, 7], [7, 8], [7, 6]],
          [[7, 7], [7, 8], [9, 6]], [[7, 7], [7, 8], [7, 5]], [[7, 7], [7, 8], [8, 5]],
          [[7, 7], [7, 8], [8, 9]], [[7, 7], [7, 8], [8, 7]], [[7, 7], [7, 8], [8, 6]],
          [[7, 7], [7, 8], [9, 5]],
        ];
        var BAL_OPEN = (rule === 2) ? BAL_RENJU : BAL_FREE;
        
        var SWAP2_SYM = [
          function (x, y) { return [x, y]; },                      
          function (x, y) { return [y, x]; },                      
          function (x, y) { return [N - 1 - x, y]; },              
          function (x, y) { return [x, N - 1 - y]; },              
          function (x, y) { return [N - 1 - x, N - 1 - y]; },      
          function (x, y) { return [N - 1 - y, x]; },              
          function (x, y) { return [y, N - 1 - x]; },              
          function (x, y) { return [N - 1 - y, N - 1 - x]; },      
        ];
        var SYM_EXPAND = (function () {
          var out = [], seen = {};
          for (var oi = 0; oi < BAL_OPEN.length; oi++) {
            for (var tt = 0; tt < SWAP2_SYM.length; tt++) {
              var q = [], key = [], ok = true;
              for (var pi = 0; pi < 3; pi++) {
                var p = SWAP2_SYM[tt](BAL_OPEN[oi][pi][0], BAL_OPEN[oi][pi][1]);
                if (p[0] < 0 || p[0] >= N || p[1] < 0 || p[1] >= N) { ok = false; break; }
                q.push([p[0], p[1]]); key.push(p[0] + ',' + p[1]);
              }
              if (!ok) continue;
              var sig = key.slice().sort().join('|');
              if (seen[sig]) continue;
              seen[sig] = 1; out.push(q);
            }
          }
          return out;
        })();
        if (N === 15 && SYM_EXPAND.length > BAL_OPEN.length) BAL_OPEN = SYM_EXPAND;
        var LIB_FALLBACK = [[0, 0], [2, 1], [2, 4]];   
        var isLibFallback = function (pts) {
          for (var i = 0; i < 3; i++)
            if (pts[i][0] !== LIB_FALLBACK[i][0] || pts[i][1] !== LIB_FALLBACK[i][1]) return false;
          return true;
        };
        
        var takeLib = function (pts) {
          for (var i = 0; i < 3; i++)
            if (!pts[i] || pts[i][0] < 0 || pts[i][0] >= N || pts[i][1] < 0 || pts[i][1] >= N) return false;
          marks.length = 0;
          for (var j = 0; j < 3; j++)
            marks.push({ p: [pts[j][0], pts[j][1]], color: (j === 1 ? 'w' : 'b') });
          balFromEngine = 3;                    
          return true;
        };
        var libSource = 'none';
        // ★★ 2026-10-13：**去掉本轮的「原生成对搜索」**，空盘恢复成 4.39(10-12 16:30) 的
        //   「官方定式」逻辑 —— 先让引擎按规则吐官方开局，再退本地公式表，最后才中心区逐手。
        //   实测原生成对搜索虽然分很平（|评估| ≤ 44），但那不是用户要的「定式」口径，
        //   而且它挡在定式前面，等于 16:30 那版的好开局一次都用不上。
        //   同步 INFO RULE 不能省：SWAP2BOARD 的开局库是按规则分的，不同步就会把**无禁手**
        //   的公式喂给禁手局面（10-12 21:59 修过的老问题）。
        try {
          // 规则由宿主按每次 ask 的 q.rule 下发（线程计划同样由宿主 C++ 决定），
          // 页面不再手工发 INFO RULE / 线程计划。
          var s2 = await wsCall('swap2board', {}, 8000);
          if (s2 && s2.pts && s2.pts.length === 3 && !isLibFallback(s2.pts) && takeLib(s2.pts))
            libSource = 'engine';
        } catch (eS2) {  }
        if (libSource === 'none' && N === 15) {
          // 本地公式表兜底：按规则取表（禁手 RIF 26 / 无禁手 Swap2 9），两张都只含平衡定式。
          if (takeLib(BAL_OPEN[Math.floor(Math.random() * BAL_OPEN.length)]))
            libSource = 'local';
        }
        if (libSource === 'none') {
        
        var R1 = 2, R2 = 3, cx = (N - 1) >> 1, cy = (N - 1) >> 1;
        var inZone = function (p, R) {
          return Math.max(Math.abs(p[0] - cx), Math.abs(p[1] - cy)) <= R;
        };
        var m1 = [cx + Math.floor(Math.random() * (2 * R1 + 1)) - R1,
                  cy + Math.floor(Math.random() * (2 * R1 + 1)) - R1];
        var b2 = boardMatrix(lastBl, lastWh);
        b2[m1[1]][m1[0]] = toBlack ? 1 : 2;
        marks.push({ p: m1, color: toBlack ? 'b' : 'w' });
        
        var sides = [toBlack ? 2 : 1, toBlack ? 1 : 2];      
        var colors = [toBlack ? 'w' : 'b', toBlack ? 'b' : 'w'];
        for (var step = 0; step < 2; step++) {
          if (leftMs() < 400) break;                         
          var pt2 = null;
          for (var tries = 0; tries < 2; tries++) {
            if (leftMs() < 400) break;
            
            var ms = (tries === 0)
              ? Math.max(400, Math.min(turnMs, Math.floor(leftMs() * (step === 0 ? 0.55 : 0.85))))
              : Math.max(400, Math.min(turnMs, leftMs() - 500));
            if (ms < 400) break;
            
            var reqBody = {
              // ★ 2026-10-13：**一律成对 TWO**（原来这个深退路是 mode:'one' 单点，
              //   实测禁手 +165 / 无禁手 +132，正是「评估差距过大」的另一个来源）。
              //   bias 恒 0：TWO 模式下引擎内部会取反，传 15 等于让平衡搜索去追 −15，
              //   对「求最平」没有好处。
              board: b2, moveList: [], n: 1, mode: 'two', bias: 0,
              rule: rule, turnMs: ms, lane: 'main',
              cid: 'desktop-swbal1-' + (lastBl.length + lastWh.length) + '-' + step + '-' + tries,
              side: sides[step],
            };
            var jw = null;
            try {
              
              jw = await onMainLane(function () {
                return wsCall('balance', reqBody, ms + 6000);
              });
            } catch (eShot) { jw = null; }          
            // ★ 2026-10-19「取消」：引擎已停 / 结果被作废时立刻退出，别把这份空结果当失败报错。
            if (swDead()) return;
            if (!jw || jw.error || jw.swap || !jw.moves || !jw.moves.length) { pt2 = null; break; }
            if (jw.eval != null) balEv = jw.eval;
            pt2 = jw.moves[0];
            // 成对 TWO 给出的就是权威的「最平一对」；不再用「必须落在天元 5×5」去挑
            // —— 用户：「有时候这三个平衡点不仅仅只在中心那几个位置」。
            break;
          }
          if (!pt2) break;                                   
          marks.push({ p: pt2, color: colors[step] });
          b2[pt2[1]][pt2[0]] = (colors[step] === 'b') ? 1 : 2;
        }
        
        var firstFreeInZone = function (bb, R, ms) {
          var mx = (N - 1) >> 1, my = (N - 1) >> 1, used = {}, k;
          for (k = 0; k < ms.length; k++) used[ms[k].p[0] + ',' + ms[k].p[1]] = 1;
          var mir = [2 * mx - ms[0].p[0], 2 * my - ms[0].p[1]];
          if (!used[mir[0] + ',' + mir[1]] && bb[mir[1]][mir[0]] === 0 &&
              Math.max(Math.abs(mir[0] - mx), Math.abs(mir[1] - my)) <= R) return mir;
          var best = null, bd = 1e9;
          for (var y = my - R; y <= my + R; y++) {
            for (var x = mx - R; x <= mx + R; x++) {
              if (bb[y][x] !== 0 || used[x + ',' + y]) continue;
              var d = Math.max(Math.abs(x - mx), Math.abs(y - my)) * 10 +
                      (Math.abs(x - mx) + Math.abs(y - my));
              if (d < bd) { bd = d; best = [x, y]; }
            }
          }
          return best;
        };
        balFromEngine = marks.length;                        
        while (marks.length < 3) {
          var fp = firstFreeInZone(b2, R2, marks);
          if (!fp) break;
          var fc = colors[marks.length - 1];                 
          marks.push({ p: fp, color: fc });
          b2[fp[1]][fp[0]] = (fc === 'b') ? 1 : 2;
        }
        }   
      } else {
        
        var pairMs = Math.max(400, Math.min(turnMs, leftMs() - 300));
        var pairBody = {
          board: boardMatrix(lastBl, lastWh), moveList: [], n: 1, mode: 'two', bias: 0,
          rule: rule, turnMs: pairMs, lane: 'main',
          cid: 'desktop-swbal2-' + (lastBl.length + lastWh.length),
          side: toBlack ? 1 : 2,
        };
        var j1 = null;
        try {
          
          j1 = await onMainLane(function () {
            return wsCall('balance', pairBody, pairMs + 6000);
          });
        } catch (ePair) { j1 = null; }
        // ★ 2026-10-19「取消」：同上 —— 先验令牌，再谈「没结果 / 该交换」。
        if (swDead()) return;
        if (!j1 || j1.error) {
          ui('setStatus', '<span style="color:#c0392b">' + LT('balFail') + ((j1 && j1.error) || 'no response') + '</span>');
          return;
        }
        if (j1.swap) { ui('setStatus', '<b style="color:#7c3aed">' + LT('swSwapNow') + '</b>'); return; }
        if (j1.eval != null) balEv = j1.eval;
        var pts1 = (j1.moves || []).slice(0, 2);
        if (pts1.length < 2) { ui('setStatus', '<span style="color:#c0392b">' + LT('balNone') + '</span>'); return; }
        marks.push({ p: pts1[0], color: toBlack ? 'b' : 'w' });
        marks.push({ p: pts1[1], color: toBlack ? 'w' : 'b' });
        balFromEngine = 2;
      }
      
      // ★ 2026-10-13：空盘走的是「官方定式」通道（不经过平衡搜索），所以 balEv 还是空的。
      //   用户要的判据是「分差应该在 0 附近」，光给坐标看不出来 —— 这里把公式给的这 3 手
      //   摆进盘面，**让平衡引擎给这一对打分**，报出来的才是真正可核对的 |评估|
      //   （实测 RIF 26 组 1~17、Swap2 9 组 5~19，全在官方 balanceWindow(50) 内）。
      if (balEv == null && marks.length === 3 && leftMs() >= 900) {
        try {
          var b3 = boardMatrix(lastBl, lastWh);
          for (var mi = 0; mi < marks.length; mi++) {
            var mc = (marks[mi].color === 'b') ? 1 : 2;
            if (b3[marks[mi].p[1]] && b3[marks[mi].p[1]][marks[mi].p[0]] === 0)
              b3[marks[mi].p[1]][marks[mi].p[0]] = mc;
          }
          var msE = Math.max(700, Math.min(1500, leftMs() - 300));
          var jE = await onMainLane(function () {
            return wsCall('balance', {
              board: b3, moveList: [], n: 1, mode: 'two', bias: 0, rule: rule,
              turnMs: msE, lane: 'main',
              cid: 'swbal-eval-' + (lastBl.length + lastWh.length), side: 2,
            }, msE + 6000);
          });
          if (jE && !jE.swap && jE.eval != null) balEv = jE.eval;
        } catch (eE) {  }
      }
      // ★ 2026-10-19「取消」：走到这里说明引擎步骤都回来了 —— 验一次令牌再摆标记 / 排队落子。
      if (swDead()) return;

      var parts = [], names = [];
      for (var i = 0; i < marks.length; i++) {
        var p = boardToScreen(marks[i].p[0], marks[i].p[1]);
        if (p) parts.push(p.x + ',' + p.y + ',' + (i + 1) + ',' + marks[i].color);
        names.push(coordOf(marks[i].p[0], marks[i].p[1]));
      }
      
      if (parts.length !== marks.length) {
        ui('setStatus', '<span style="color:#c0392b">' + T('no_board') + '</span>');
        balOff();
        return;
      }
      if (parts.length && HOST) {
        tellHost({ type: 'balance', on: true, gap: Math.round(boardGap()), cells: parts.join(';') });
      }
      
      balSet(marks.map(function (m) { return [m.p[0], m.p[1]]; }), true,
             'swap' + (empty ? '-empty' : '-pair'),
             marks.map(function (m) { return m.color; }));
      
      var filledN = marks.length - balFromEngine;
      var evTxt = (balEv == null || isNaN(+balEv))
        ? '<span style="color:#667">' + LT('swBalEvalNone') + '</span>'
        : '<span style="color:' + (Math.abs(+balEv) <= 50 ? '#0e7490' : '#e67e22') + '">' +
          LT('swBalEval').replace('%1', String(Math.abs(Math.round(+balEv)))) + '</span>';
      ui('setStatus', '<b style="color:#0e7490">' + LT('swBalDone') + names.join('  ') + '</b>' +
        evTxt +
        (filledN > 0 ? '<span style="color:#e67e22">' + LT('swBalFilled').replace('%1', filledN) + '</span>' : '') +
        (empty ? '<span style="color:#0e7490">　※ ' + swBalOpenHintTxt() + '</span>' : ''));
      
      var balPts = marks.map(function (m) { return [m.p[0], m.p[1]]; });
      if (adOn && parts.length === marks.length) {
        swQueue = balPts; swIdx = 0;
        swBtnState();                             
        setTimeout(swAdvance, 200);
      } else {
        
        pendingBal = balPts;
      }
    } catch (e) {
      ui('setStatus', '<span style="color:#c0392b">' + LT('balFail') +
        errText(e) + '</span>');
    } finally { swBusy = false; }
  }

  // ★ 2026-10-13：连线器的开局提示 —— 从短句池随机抽**一条**（用户：「在连线器中要精简」）。
  function swBalOpenHintTxt() {
    var pool = LT('swBalOpenHints');
    if (!pool || !pool.length || typeof pool === 'string') return '';
    return pool[Math.floor(Math.random() * pool.length)];
  }

  function swTip(h) {
    return '<span style="display:inline-block;font-size:15px;font-weight:600;line-height:1.35;' +
           'color:#0e7490;border:1px solid #33d6ff;border-radius:8px;padding:4px 12px;' +
           'background:rgba(51,214,255,0.08);box-shadow:0 0 6px rgba(51,214,255,0.25);">' + h + '</span>';
  }
  function swTipCol(h, c) {
    return '<span style="display:inline-block;font-size:15px;font-weight:600;line-height:1.35;' +
           'color:' + c + ';border:1px solid #33d6ff;border-radius:8px;padding:4px 12px;' +
           'background:rgba(51,214,255,0.08);box-shadow:0 0 6px rgba(51,214,255,0.25);">' + h + '</span>';
  }
  
  function swAdvColor(which) {
    try {
      var cs = getComputedStyle(document.body || document.documentElement);
      var v = cs.getPropertyValue(which === 'b' ? '--side-b-tgl' : '--side-w-tgl');
      if (v && v.trim()) return v.trim();
    } catch (e) {}
    if (which === 'b') return S.dark ? '#a78bfa' : '#8b5cf6';
    return S.dark ? '#58a6e0' : '#3b9ae1';
  }
  
  function swAdvHl(txt, which) {
    var tok = LT(which === 'b' ? 'swAdvB' : 'swAdvW');
    if (!tok || txt.indexOf(tok) < 0) return txt;
    return txt.replace(tok, '<b style="color:' + swAdvColor(which) +
                            ';font-weight:800;">' + tok + '</b>');
  }

  async function runSwapEval() {
    if (swEvalBusy) return;
    if (!boardGeo) { ui('setStatus', '<span style="color:#e67e22;">' + T('no_board') + '</span>'); return; }
    var ply = lastBl.length + lastWh.length;
    if (!ply) { ui('setStatus', swTipCol(LT('swEvalEmpty'), '#e67e22')); return; }
    if (ply > 5) {  
      ui('setStatus', swTipCol(LT('swEvalLate'), '#667'));
      return;
    }
    swEvalBusy = true;
    
    swQuietSide = true;
    if (S.side !== -1) setSide(-1);
    try {
      ui('setStatus', swTip(LT('swEvaling')));
      var j = await wsCall('analyze', {
        board: boardMatrix(lastBl, lastWh), moveList: [],
        matchMs: 600000, turnMs: 2000, timeUsedMs: 0, topN: 1,
        rule: engRuleOf(S.rule), lane: 'sub',
        side: sideToMove(lastBl, lastWh),
        cid: 'desktop-sweval-' + ply,
      }, 2000 + 15000);
      if (j && j.superseded) return;                 
      if (!j || j.error || !j.candidates || !j.candidates.length) {
        ui('setStatus', swTipCol(LT('swEvalFail') + ((j && j.error) || 'no candidates'), '#c0392b'));
        return;
      }
      var ev = j.candidates[0].eval || '';           // 引擎原文（行棋方视角）
      // 交换评估关心的是「黑优还是白优」⇒ 换成黑方视角（rootSide 由同一判据给出）。
      var swRoot = sideToMove(lastBl, lastWh);
      var blackView = evalNumOf(ev, swRoot, 1);
      var msg;
      
      if (Math.abs(blackView) < 50) msg = LT('swEvalEven').replace('%1', ev);
      else if (blackView > 0) msg = swAdvHl(LT('swEvalBlack').replace('%1', ev), 'b');
      else msg = swAdvHl(LT('swEvalWhite').replace('%1', ev), 'w');
      ui('setStatus', swTip(LT('swEvalDone') + msg));
    } catch (e) {
      ui('setStatus', swTipCol(LT('swEvalFail') + errText(e), '#c0392b'));
    } finally { swEvalBusy = false; }
  }

  // ★ 2026-10-16（用户：「检测到棋盘空的时候，应该重新清空置换表重新评估计算」）：
  //   空盘 = 新一局 ⇒ ① 清引擎置换表（Rapfi 的 TT 跨局共用，不清就会让新局前几手
  //   直接命中上一局的条目：曲线与推荐点「沿用上一局」）② 复位本地状态（曲线/推荐点/
  //   热力/平衡标记）③ **不管执黑执白、也不管有没有选边**都要做。
  //   以前这段在 `sideUnset()`、`!myIsToMove` 之后 —— 执白或还没选边时空盘什么都不做，
  //   而且原实现只 resetForEmpty（本地缓存），从不碰引擎。
  function handleEmptyBoard(bl, wh) {
    if (!emptyResetDone) {
      emptyResetDone = true;
      newGamePending = true;
      engineHardReset('empty-board');
      resetForEmpty();
      postGuide(false);
      tellHost({ type: 'heat', on: false });
      diag('[reset] empty board -> TT cleared, caches dropped');
    }
    if (sideUnset()) {
      ui('setStatus', S.lang === 'en'
        ? 'Board located (empty) — TT cleared, pick Black or White to start'
        : '已定位棋盘（空盘）· 置换表已清空，请先选执黑或执白');
    } else if (myIsToMove(bl, wh)) {
      ui('setStatus', S.lang === 'en'
        ? 'Board located (empty) — TT cleared, analysis starts after the first move'
        : '已定位棋盘（空盘）· 置换表已清空，落子后自动分析');
      if (adOn && S.side === 0) {
        adLastPt = null;
        adFire((N - 1) >> 1, (N - 1) >> 1, true, true);
      }
    } else {
      ui('setStatus', S.lang === 'en'
        ? 'Board located (empty) — TT cleared, waiting for the opponent to open'
        : '已定位棋盘（空盘）· 置换表已清空，等待对手落子');
    }
    refreshHeatOverlay(bl, wh).catch(function () {});
  }

  async function handleScan(res) {
    var ok = applyScan(res);
    paintFrame();
    var bl = (res && res.black) || [], wh = (res && res.white) || [];
    if (!ok) {
      
      if (res && res.ok && res.suspect) {
        ui('setStatus', suspectTip(res));
        return;
      }
      
      if (lastBoardKey !== '__novis__') {
        lastBoardKey = '__novis__';
        emptyStreak = 0;
        pendingKey = ''; pendingN = 0;      
        resetForEmpty();
        postGuide(false);
        tellHost({ type: 'heat', on: false });
        balOff();
      }
      ui('setStatus', T('no_board'));
      return;
    }
    lastBl = bl; lastWh = wh;                  
    var key = boardKey(bl, wh);
    var emptyNow = (bl.length + wh.length === 0);
    if (emptyNow) emptyStreak++; else emptyStreak = 0;
    // ★ 2026-10-18：新一局（空盘）把「对手思考时长」清零 —— 上一局的节奏不能拿到这一局用。
    if (emptyNow) { linkOppThinkMs = 0; linkOppTurnAt = 0; }
    
    // ★★ 空盘要**连续 ≥3 帧 且 持续 ≥700ms** 才算真新局。
    //   原来 2 帧（约 300ms）就能打穿：识别一次抖成全空 ⇒ 清表 ⇒ 整盘从零重搜。
    if (emptyNow) {
      if (!emptySinceMs) emptySinceMs = Date.now();
      if (emptyStreak < 3 || Date.now() - emptySinceMs < 700) return;
    } else {
      emptySinceMs = 0;
    }
    
    if (key === lastBoardKey) { pendingKey = ''; pendingN = 0; forceScan = false; return; }
    
    if (forceScan) {
      forceScan = false;                    
      pendingKey = ''; pendingN = 0;
    } else {
      
      if (key !== pendingKey) {
        var addB = deltaStones(bl, lastAdoptBl), addW = deltaStones(wh, lastAdoptWh);
        var rmB = deltaStones(lastAdoptBl, bl).length, rmW = deltaStones(lastAdoptWh, wh).length;
        if (addB.length + addW.length === 1 && rmB + rmW === 0) {
          pendingKey = ''; pendingN = 0;          
        } else {
          pendingKey = key; pendingN = 1; return;
        }
      } else {
        if (++pendingN < STABLE_FRAMES) return;
        pendingKey = ''; pendingN = 0;
      }
    }
    
    lastBoardKey = key;
    
    var rmPrev = deltaStones(lastAdoptBl, bl).length + deltaStones(lastAdoptWh, wh).length;
    if (rmPrev >= 2) newGamePending = true;
    lastAdoptBl = bl; lastAdoptWh = wh;      

    // ★ 2026-10-18：在**已稳定采纳**的这一帧上给对手计时 —— 轮次交到对手手上时打点
    //   （我方落子被识别确认的那一刻），轮次回到我方时结算成 linkOppThinkMs。
    //   这就是「免费思考窗口」的实测长度，analyze() 用它定这一手的搜索预算。
    if (!myIsToMove(bl, wh)) { if (!linkOppTurnAt) linkOppTurnAt = Date.now(); }
    else if (linkOppTurnAt) {
      linkOppThinkMs = Math.max(0, Date.now() - linkOppTurnAt);
      linkOppTurnAt = 0;
    }
    
    var plyNow = bl.length + wh.length;
    if ((emptyNow && plyPrev > 0) || (plyPrev - plyNow >= 4)) newGamePending = true;
    plyPrev = plyNow;
    if (emptyNow && adHadStones) adNewGameOff();
    adHadStones = !emptyNow;
    
    if (adOn) adConfirm();
    
    balOnBoardChanged(bl, wh);
    
    if (balMarks.length && balSticky) return;
    
    // ★ 2026-10-16：空盘处理提前到所有「有边/轮次」判断之前 —— 空盘就是新一局，
    //   清置换表与复位不应该依赖「我方是否该走」「有没有选边」。
    if (emptyNow) { handleEmptyBoard(bl, wh); return; }
    emptyResetDone = false;
    
    {
      var mEndPre = lastBoardMatrix();
      if (hasFiveOnBoard(mEndPre)) {
        if (adOn) adNewGameOff();
        if (adTimer) { clearTimeout(adTimer); adTimer = null; }
        adWaiting = false;
        engineBusy = false;
        analyzeRetryTimer = null;
        postGuide(false);
        tellHost({ type: 'heat', on: false });
        balOff();
        paintFrame();
        ui('setStatus', '<b style="color:#16a34a">' +
          (S.lang === 'en' ? 'Five in a row — game over, evaluation stopped.'
                            : '已经五子连珠 —— 本局结束，已停止评估。') + '</b>');
        return;
      }
    }
    
    if (sideUnset()) {
      haltNoSide();
      return;
    }
    
    {
      var mNow = lastBoardMatrix();
      if (!emptyNow && myIsToMove(bl, wh) && !hasFiveOnBoard(mNow)) {
        var myCol = myColOf();
        var winCells = findWinningFive(mNow, myCol);
        if (winCells.length) {
          var wc = winCells[0];
          postGuide(true, wc[0], wc[1]);         
          adLastPt = null;                       
          adFire(wc[0], wc[1], true, false);     
          diag('[finish] ' + (S.side === 0 ? 'B' : 'W') + ' wins at ' + coordOf(wc[0], wc[1]) + ' cells=' + winCells.length + ' | ' + boardDumpText());
          ui('setStatus', '<b style="color:#28a745">' +
            (S.lang === 'en'
              ? ('Winning move ' + coordOf(wc[0], wc[1]) + ' — five in a row, game over')
              : ('已落制胜子 ' + coordOf(wc[0], wc[1]) + ' —— 五子连珠，本局收官')) + '</b>');
          return;                                
        }
      }
    }
    
    {
      var mEnd = lastBoardMatrix();
      
      var o4 = findOpenFour(mEnd);
      if (o4 && o4.color !== myColOf() && Date.now() - lastOpen4Toast > 8000) {
        lastOpen4Toast = Date.now();
        diag('[open4] opponent open four ' + o4.cells.map(function (p) { return coordOf(p[0], p[1]); }).join(',') + ' | ' + boardDumpText());
        ui('setStatus', '<span style="color:#c0392b">' +
          (S.lang === 'en' ? 'Opponent open four (' : '对方活四（') +
          o4.cells.map(function (p) { return coordOf(p[0], p[1]); }).join('·') +
          (S.lang === 'en' ? ') — they win next move, respond now</span>'
                           : '）—— 下一手就赢，抓紧应对</span>'));
      }
    }
    
    if (emptyNow) {
      // （空盘已在前面提前处理并 return —— 这里保留判定只为可读性提示：不该到达）
      return;
    }
    if (!myIsToMove(bl, wh)) {
      waitOpponent();
      return;
    }
    analyze(bl, wh);
    
    refreshHeatOverlay(bl, wh).catch(function () {});
  }

  function haltNoSide() {
    lastBest = null; lastMainC0 = null;
    S.candCache = [];
    S.rawEval = ''; S.myEval = '-'; S.lastStat = null;
    postGuide(false);
    tellHost({ type: 'heat', on: false });    
    balOff();                                 
    ['__gb_st_depth', '__gb_st_eval', '__gb_st_speed', '__gb_st_nodes', '__gb_st_line']
      .forEach(function (id) { var el = document.getElementById(id); if (el) el.textContent = '-'; });
    
    if (!sideHintMuted()) {
      ui('setStatus', '<span style="color:#e67e22;">' +
        (S.lang === 'en' ? 'I-play color not chosen — click Black or White to start evaluating.'
                         : '我执颜色未选定——请点「黑」或「白」开始评估（再点一次可取消）。') + '</span>');
    }
    paintFrame();
  }

  function waitOpponent() {
    ui('setStatus', '<span style="color:#555;">' +
      (S.lang === 'en' ? 'Waiting for opponent…' : '等待对手落子…') + '</span>');
    if (S.lastStat && S.lastStat.c0) ui('setStatPanel', S.lastStat.c0, S.lastStat.elapsed);
    ui('drawChart');                          
    postGuide(false);                         
    tellHost({ type: 'heat', on: false });    
    balOff();                                 
    paintFrame();
  }

  function offline() {
    boardRect = null; boardGeo = null;
    lastBest = null; lastMainC0 = null;
    paintFrame();
    postGuide(false);
    tellHost({ type: 'heat', on: false });
    balOff();
    pingEnsureDeps();                         
    
    ui('setStatus', '<span style="color:#c0392b">' +
      (S.lang === 'en'
        ? 'Recognition link lost — the host stopped pushing board frames. This app restarts the recognition pipeline automatically; if it stays down, check whether your security software is blocking it.'
        : '识别链路已断开（宿主未再推送棋盘帧）。本程序会自动重启识别管线；若长时间不恢复，请检查是否被安全软件拦截。') +
      '</span>');
  }

  if (HOST) {
    window.chrome.webview.addEventListener('message', function (ev) {
      var msg = ev.data;
      if (typeof msg === 'string') { try { msg = JSON.parse(msg); } catch (e) { return; } }
      if (!msg) return;
      if (msg.type === 'engineAck') {
        natEng().onAck(msg);                    // ★ 闭包内部分发（在途 ask 表私有）
      }
      else if (msg.type === 'engineAsk' || msg.type === 'engineOut') { natEng().onOut(msg); }
      else if (msg.type === 'scan') {
        if (busy) return;                       
        busy = true;
        Promise.resolve(handleScan(msg.data)).finally(function () { busy = false; });
      }
      // ★★ 2026-10-18（用户：「线程多于核心…要齐心协力，而不是打架」）：连线窗原来**不处理**
      //   host 握手消息（只有 boot() 的 onHostMsg 处理，而连线窗走 bootLink），
      //   于是 threadsMax / userThreads 永远读不到，线程数一直停在上次存档的中间值。
      //   这里补上分发：G.host 更新 + 一次性拓扑校正（calc.js: window.GB_onHost）。
      else if (msg.type === 'host') {
        try { if (window.GB_onHost) window.GB_onHost(msg); } catch (eHO) {}
      }
      else if (msg.type === 'offline') { offline(); }
      
      else if (msg.type === 'setSide') { setSide(msg.v); }
      else if (msg.type === 'engineCfg') {
        // 引擎配置（线程 / 哈希）由主窗口经宿主统一落盘；连线窗只需转发一个 config op。
        var ct = msg.threads | 0, ch = msg.hashMB | 0;
        bridgeDiag('LINK_ENG_CFG threads=' + ct + ' hashMB=' + ch + ' (follow main window)');
        try {
          var cvCfg = window.chrome && window.chrome.webview;
          if (cvCfg && cvCfg.postMessage)
            cvCfg.postMessage(JSON.stringify({ engine: 1, op: 'config', id: 0,
                                               q: { threads: ct, hashMB: ch } }));
        } catch (eCfg) {}
      }
      else if (msg.type === 'multiBoard') {
        
        var n = (msg && msg.n) ? msg.n : 2;
        
        ui('setStatus', '<span style="color:#e67e22;">' +
          (S.lang === 'en'
            ? (n + ' boards detected — close the others and keep only one')
            : ('检测到 ' + n + ' 个棋盘，请关闭其他棋盘，只保留一个')) +
          '</span>');
      }
      else if (msg.type === 'posState') {
        
        if (msg.open) { posConfirmed(); posBtnOn(true); }
        else { posBtnOn(false); }
      }
      else if (msg.type === 'posSave') {
        
        if (msg.ok) {
          ui('setStatus', (S.lang === 'en' ? 'Board image saved: ' : '棋盘图片已保存：') +
            '<span style="word-break:break-all">' + (msg.path || '') + '</span>');
        } else if (msg.reason === 'noboard') {
          ui('setStatus', S.lang === 'en'
            ? 'No board recognized yet — nothing to save'
            : '还没识别到棋盘，暂时没有可保存的局面');
        } else if (msg.reason === 'cancel') {
          
        } else {
          ui('setStatus', (S.lang === 'en' ? 'Save failed: ' : '保存失败：') + (msg.reason || '?'));
        }
      }
      else if (msg.type === 'autodropDone') {
        
        lastAdPipe = msg.pipeline || '';
        if (msg.pipeline === 'sendinput') {
          
        }
      }
      else if (msg.type === 'region') {
        
        if (msg.set) {
          ui('setStatus', S.lang === 'en'
            ? 'Scan region set — only this area is recognized'
            : '已设置识别区域（手动调节）：只在此区域内识别棋盘');
        } else {
          ui('setStatus', S.lang === 'en'
            ? 'Scan region cleared — full screen'
            : '已取消识别区域：恢复全屏自动找盘');
        }
      }
    });
  }

  // 识别统一由 C++ 宿主经 postMessage {"type":"scan"} 推送（零 TCP）；
  // 页面侧已无 HTTP 识别通道，非宿主环境无识别来源。
  async function tick() {
    if (busy || HOST) return;
  }

  function on(id, ev, fn) { var el = document.getElementById(id); if (el) el.addEventListener(ev, fn); }

  on('__gb_rec', 'click', function () { lastBoardKey = ''; forceScan = true; S.history = []; ui('drawChart'); tick(); });
  on('__gb_analyze', 'click', function () { var k = lastBoardKey; lastBoardKey = ''; forceScan = true; tick().then(function () { lastBoardKey = k; }); });
  
  on('__gb_autodrop', 'click', function () {
    
    if (swQueue) { ui('setStatus', swTipCol(LT('adBusyQueue'), '#0e7490')); return; }
    adOn = !adOn;
    adLastPt = null; swQueue = null; adFail = 0; adDone = 0;
    if (!adOn && adTimer) { clearTimeout(adTimer); adTimer = null; adWaiting = false; }
    adBtnSync();
    
    if (adOn && pendingBal && pendingBal.length) {
      
      var pmm = lastBoardMatrix();
      var ptodo = pendingBal.filter(function (p) { return !(pmm[p[1]] && pmm[p[1]][p[0]]); });
      pendingBal = null;
      if (ptodo.length) { swQueue = ptodo; swIdx = 0; adStage = 0; swBtnState(); setTimeout(swAdvance, 200); }
    }
    else if (adOn && S.side === 0 && lastBl.length + lastWh.length === 0) {
      adFire((N - 1) >> 1, (N - 1) >> 1, true, true);
    }
    
    else if (adOn && boardGeo && !sideUnset() &&
             lastBl.length + lastWh.length > 0 && myIsToMove(lastBl, lastWh)) {
      lastBoardKey = ''; pendingKey = ''; pendingN = 0; forceScan = true;
      if (!HOST) tick();
    }
    ui('setStatus', '<b style="color:' + (adOn ? '#28a745' : '#555') + '">' +
      LT(adOn ? 'adOn' : 'adOff') + '</b>');
  });
  on('__gb_adcount', 'click', function () {
    adCount = adCount % 3 + 1;                        
    try { localStorage.setItem('gb_ad_count', String(adCount)); } catch (e) {}
    adBtnSync();
    ui('setStatus', LT('adCount') + '×' + adCount);
  });

  function balPointSync() {
    var bp = document.getElementById('__gb_balpoint');
    if (bp && bp.tagName === 'SELECT') bp.value = String(balPointN);
    else if (bp) bp.textContent = '×' + balPointN;
    var lb = document.getElementById('lk_baln');
    if (lb && lb.tagName === 'SELECT') lb.value = String(balPointN);
    else if (lb) lb.textContent = '×' + balPointN;
    var mb = document.getElementById('btn_an_baln');
    if (mb && mb.tagName === 'SELECT') mb.value = String(balPointN);
    else if (mb) mb.textContent = '×' + balPointN;
  }
  on('__gb_balpoint', 'change', function () {
    var el = document.getElementById('__gb_balpoint');
    var v = parseInt(el ? el.value : '2', 10);
    if (!isNaN(v) && v >= 1 && v <= 6) balPointN = v;
    try { localStorage.setItem('gb_bal_point_n', String(balPointN)); } catch (e) {}
    balPointSync();
    ui('setStatus', '<b style="color:#0e7490">' + LT('balPointSet').replace('%1', balPointN) + '</b>');
  });
  balPointSync();

  on('__gb_swbal', 'click', function () {
    
    runSwapBal();
  });
  on('__gb_sweval', 'click', function () { runSwapEval(); });
  adBtnSync();                                        

  function posBtnLang() {
    var b = document.getElementById('__gb_pos');
    if (b) b.textContent = (S.lang === 'en') ? 'Position' : '局面';
    
    var sb = document.getElementById('__gb_possave');
    if (sb) sb.textContent = (S.lang === 'en') ? 'Save board' : '保存局面';
  }
  
  function balBtnLang() {  }
  
  function swBtnLang() {
    var b = document.getElementById('__gb_autodrop');
    var s1 = document.getElementById('__gb_swbal'), s2 = document.getElementById('__gb_sweval');
    if (b) b.textContent = LT('adBtn');
    if (s1) s1.textContent = LT('swBalBtn');
    if (s2) s2.textContent = LT('swEvalBtn');
  }
  
  function posBtnOn(on) {
    var b = document.getElementById('__gb_pos');
    if (!b) return;
    if (on) b.setAttribute('data-on', '1');
    else b.removeAttribute('data-on');
  }
  
  var FOOT_ACTIONS = {
    __gb_pos: function () { askPosition(); },
    
    __gb_possave: function () { tellHost({ type: 'savePosPng' }); },
    __gb_adjust: function () { tellHost({ type: 'startAdjust' }); },
  };
  
  var posRetry = null, posTries = 0;
  function askPosition() {
    posTries = 0;
    if (posRetry) { clearTimeout(posRetry); posRetry = null; }
    sendOpenPos();
  }
  function sendOpenPos() {
    tellHost({ type: 'openPos', try: posTries + 1 });
    posTries++;
    if (posRetry) clearTimeout(posRetry);
    if (posTries >= 3) { posRetry = null; return; }
    posRetry = setTimeout(function () { sendOpenPos(); }, 800);
  }
  function posConfirmed() {
    if (posRetry) { clearTimeout(posRetry); posRetry = null; }
  }
  
  function fireFoot(id, e) {
    if (!FOOT_ACTIONS[id]) return;
    if (e && e.__gbFootDone) return;
    if (e) e.__gbFootDone = true;
    
    tellHost({ type: 'footClick', id: id });
    FOOT_ACTIONS[id]();
  }
  function bindFootDirect() {
    Object.keys(FOOT_ACTIONS).forEach(function (id) {
      var el = document.getElementById(id);
      if (!el || el.__gbFootBound) return;
      el.__gbFootBound = true;
      el.addEventListener('click', function (e) { fireFoot(id, e); }, false);
    });
  }
  bindFootDirect();
  
  if (window.MutationObserver) {
    new MutationObserver(function () { bindFootDirect(); }).observe(document.documentElement,
      { childList: true, subtree: true });
  }
  document.addEventListener('click', function (e) {
    var t = e.target;
    while (t && t !== document) {
      if (t.id && FOOT_ACTIONS[t.id]) { fireFoot(t.id, e); return; }
      t = t.parentElement;
    }
  }, false);
  
  function footProbeLine() {
    try {
      var vw = window.innerWidth, vh = window.innerHeight;
      var ids = Object.keys(FOOT_ACTIONS), rep = [];
      ids.forEach(function (id) {
        var el = document.getElementById(id);
        if (!el) { rep.push(id + '=MISSING'); return; }
        var r = el.getBoundingClientRect();
        var cx = r.left + r.width / 2, cy = r.top + r.height / 2;
        var hit = null;
        try { hit = document.elementFromPoint(cx, cy); } catch (e) {}
        var hitId = hit ? (hit.id || hit.tagName) : 'null';
        var inView = (r.top >= -1 && r.bottom <= vh + 1 && r.width > 0 && r.height > 0);
        rep.push(id + '=[' + Math.round(r.left) + ',' + Math.round(r.top) + ' ' +
                 Math.round(r.width) + 'x' + Math.round(r.height) +
                 ' bottom=' + Math.round(r.bottom) +
                 (inView ? ' 在视口内' : ' 视口外!') +
                 ' hit=' + hitId + (hit === el ? '(OK)' : '(被挡!)') + ']');
      });
      var pan = document.getElementById('__gb_panel');
      var bod = document.getElementById('__gb_body');
      var pr = pan ? pan.getBoundingClientRect() : null;
      var extra = '';
      if (pr) extra += ' panel=' + Math.round(pr.width) + 'x' + Math.round(pr.height) +
                        ' bottom=' + Math.round(pr.bottom) + (pr.bottom > vh + 1 ? '(超视口!)' : '');
      if (bod) extra += ' body=' + bod.clientHeight + '/' + bod.scrollHeight +
                        '@' + Math.round(bod.scrollTop);
      tellHost({ type: 'footProbe', ids: rep.join(' | '),
                 vw: vw, vh: vh, extra: extra,
                 bar: !!document.getElementById('__gb_footbar') });
    } catch (e) {}
  }
  setTimeout(footProbeLine, 1200);
  setTimeout(footProbeLine, 3000);          

  function sideUnset() { return S.side !== 0 && S.side !== 1; }
  function applySideUI() {
    var b = document.getElementById('__gb_side_black'), w = document.getElementById('__gb_side_white');
    var txt = document.getElementById('__gb_side_txt');
    var dk = !!S.dark;
    var unset = sideUnset();
    var baseBg = dk ? '#262b3a' : '#fff';
    var baseFg = dk ? '#c8ccd6' : '#555';
    var baseBd = dk ? '#5a6072' : '#999';
    function paint(el, isBlack, on) {
      if (!el) return;
      if (on) {
        el.style.background = isBlack ? '#1a1a1a' : '#fff';
        el.style.color = isBlack ? '#fff' : '#111';
        el.style.border = '2px solid ' + (isBlack ? '#000' : '#111');
        el.style.boxShadow = isBlack ? '0 0 0 1.5px #fff inset' : 'none';
      } else {
        el.style.background = baseBg; el.style.color = baseFg;
        el.style.border = '1px solid ' + baseBd; el.style.boxShadow = 'none';
      }
      el.style.fontWeight = on ? '800' : '600';
      
      el.style.opacity = (on || unset) ? '1' : '0.45';
      el.style.padding = on ? '1px 5px' : '2px 6px';
    }
    paint(b, true, S.side === 0);
    paint(w, false, S.side === 1);
    var en = (S.lang === 'en');
    if (b) b.title = en ? 'I play Black (click again to unlock)' : '我执黑（再点一次取消）';
    if (w) w.title = en ? 'I play White (click again to unlock)' : '我执白（再点一次取消）';
    if (txt) {
      if (unset) {
        txt.textContent = en ? 'pick B/W' : '请选黑/白';
        txt.style.color = '#e67e22';
      } else {
        txt.textContent = (en ? 'locked' : '锁定') + '·' + T(S.side === 0 ? 'black' : 'white');
        txt.style.color = '#2e7d32';
      }
    }
  }
  
  function setSide(v) {
    
    var quiet = swQuietSide && v === -1;
    if (v !== -1) swQuietSide = false;
    S.side = (S.side === v) ? -1 : v;        
    S.candCache = [];
    lastBest = null; lastMainC0 = null;
    S.history = [];
    S.rawEval = ''; S.myEval = '-';
    lastBoardKey = ''; forceScan = true;
    saveNow(); applySideUI(); ui('drawChart');
    
    S.lastStat = null;
    ['__gb_st_depth', '__gb_st_eval', '__gb_st_speed', '__gb_st_nodes', '__gb_st_line']
      .forEach(function (id) { var el = document.getElementById(id); if (el) el.textContent = '-'; });
    tellHost({ type: 'heat', on: false });     
    balOff();                                  
    postGuide(false);                          
    if (!quiet) {
      ui('setStatus', sideUnset()
        ? '<span style="color:#e67e22;">' +
          (S.lang === 'en' ? 'Side unlocked — pick Black or White to start evaluating.'
                           : '已取消我执颜色——未选定前不评估，请点「黑」或「白」。') + '</span>'
        : '<span style="color:#3b7dd8;">' +
          (S.lang === 'en' ? 'Locked to ' + (S.side === 0 ? 'Black' : 'White') + ' — re-analyzing for this side only…'
                           : '已锁定我执' + (S.side === 0 ? '黑' : '白') + '——只按这一方重新评估…') + '</span>');
    }
    if (!HOST) tick();
  }
  on('__gb_side_black', 'click', function () { setSide(0); });
  on('__gb_side_white', 'click', function () { setSide(1); });

  function kickAnalyze() {
    lastBoardKey = '';
    forceScan = true;          
    S.candCache = [];
    if (!HOST) tick();
  }

  Array.prototype.forEach.call(document.querySelectorAll('.__gb_step'), function (b) {
    b.addEventListener('click', function () {
      var d = b.getAttribute('data-dir');
      if (d === 'turn+') S.customTurnSec = Math.min(6000, Math.round((S.customTurnSec + 0.25) * 100) / 100);
      else if (d === 'turn-') S.customTurnSec = Math.max(0.5, Math.round((S.customTurnSec - 0.25) * 100) / 100);
      else if (d === 'match+') S.customSec = Math.min(30000, S.customSec + 10);
      else if (d === 'match-') S.customSec = Math.max(10, S.customSec - 10);
      else if (d === 'hash+') { S.hashMB = Math.min(4096, S.hashMB + 128); ui('applyHash'); }
      else if (d === 'hash-') { S.hashMB = Math.max(0, S.hashMB - 128); ui('applyHash'); }
      ui('applyPreset');
      kickAnalyze();
    });
  });

  var turnInp = document.getElementById('__gb_turnval');
  var matchInp = document.getElementById('__gb_matchval');
  function sanitizeTurn(v) {
    v = String(v).replace(/[^\d.]/g, '');               
    var p = v.split('.');
    if (p.length > 2) v = p[0] + '.' + p.slice(1).join('');   
    if (v.indexOf('.') >= 0) { var h = v.split('.'); v = h[0] + '.' + h[1].slice(0, 2); }  
    return v;
  }
  function sanitizeMatch(v) { return String(v).replace(/[^\d]/g, ''); }
  if (turnInp) {
    turnInp.addEventListener('input', function () { this.value = sanitizeTurn(this.value); });
    turnInp.addEventListener('change', function () {
      var t = parseFloat(turnInp.value);
      if (isFinite(t)) S.customTurnSec = Math.min(6000, Math.max(0.5, Math.round(t * 100) / 100));
      ui('applyPreset'); kickAnalyze();
    });
  }
  if (matchInp) {
    matchInp.addEventListener('input', function () { this.value = sanitizeMatch(this.value); });
    matchInp.addEventListener('change', function () {
      var m = parseFloat(matchInp.value);
      if (isFinite(m)) S.customSec = Math.min(30000, Math.max(10, Math.round(m)));
      ui('applyPreset'); kickAnalyze();
    });
  }

  // ★ 2026-10-16（用户：「变规则的时候不能顺利的切换规则而评估」）：
  //   判据必须是「**引擎当前配置的规则**」而不是「上一次点击的值」—— 因为
  //   calc.js 的 lkBridgeRule() 会先把 window.GB_S.rule 改成新值、再 click 面板按钮
  //   （主界面 → 连线窗同步走的就是这条路）。若按「点击前后 S.rule 是否变化」判断，
  //   那条路永远判定为「没变」⇒ 清表不会发生。这里记引擎侧真实生效的规则。
  var lastEngineRule = (typeof engRuleOf === 'function') ? engRuleOf(S.rule) : null;
  function syncEngineRule() {
    var r = engRuleOf(S.rule);
    if (lastEngineRule !== null && lastEngineRule !== r) {
      newGamePending = true;
      engineHardReset('rule-change ' + lastEngineRule + '->' + r);
      lastEngineRule = r;
      return true;
    }
    lastEngineRule = r;
    return false;
  }

  Array.prototype.forEach.call(document.querySelectorAll('[data-rule]'), function (el) {
    el.addEventListener('click', function () {
      S.rule = +el.getAttribute('data-rule');
      saveNow(); ui('paintSel');
      // 规则真的变了才清置换表重算 —— Rapfi 的置换表条目不分规则，旧规则下的分数会直接
      // 污染新规则下的搜索（禁手/无禁手同一局面分数完全不同）。以前这里只 kickAnalyze()
      // 直接重算，等于「用旧规则的记忆算新规则」⇒ 用户看到的就是「切了规则但评估没变」。
      var ruleChanged = syncEngineRule();
      // ★★ 2026-10-17（用户：「变规则的时候不能顺利的切换规则而评估」）：
      //   换规则后**立刻**按新规则重算一次，不等下一帧视觉扫描。
      //   原因：kickAnalyze() 自身**不发起** analyze —— 它只是把 lastBoardKey 清空、
      //   forceScan 置位，真正的 analyze 由**下一帧 scan** 触发。而画面静止/未框选/
      //   帧率低时识别不一定推帧 ⇒ 表现就是「选项高亮切过去了，评估数字一动不动」。
      //   analyze() 里的 rule: engRuleOf(S.rule) 是调用时读取，所以这里发起的就是新规则。
      if (ruleChanged && lastBl && (lastBl.length + lastWh.length) > 0) {
        try { analyze(lastBl, lastWh); } catch (eAR) {}
      }
      kickAnalyze();
    });
  });

  on('__gb_heat', 'click', function () {
    S.heatmap = !S.heatmap; saveNow(); ui('paintHeat');
    kickAnalyze();
  });

  try {
    var verHdr = document.getElementById('__gb_hdr');
    if (verHdr && !document.getElementById('__gb_ver')) {
      var verEl = document.createElement('span');
      verEl.id = '__gb_ver';
      verEl.textContent = GB_VER;
      verEl.style.cssText = 'font-size:10px;color:rgba(255,255,255,.78);letter-spacing:.3px;'
        + 'font-variant-numeric:tabular-nums;user-select:none;flex:none;margin:0 2px;line-height:1;';
      verHdr.insertBefore(verEl, document.getElementById('__gb_lang'));
    }
  } catch (e) {}

  on('__gb_dark', 'click', function () { S.dark = !S.dark; saveNow(); ui('applyDark'); ui('drawChart'); applySideUI(); });
  on('__gb_lang', 'click', function () {
    S.lang = (S.lang === 'en') ? 'zh' : 'en'; saveNow(); ui('applyLang'); applySideUI(); posBtnLang();
    balBtnLang();
    swBtnLang();
    
    setTimeout(reportPanelRect, 30);
  });
  
  var opRange = document.getElementById('__gb_opacity_range');
  var opNum = document.getElementById('__gb_opacity_num');
  var blurRange = document.getElementById('__gb_blur_range');
  var blurNum = document.getElementById('__gb_blur_num');
  if (opRange) opRange.addEventListener('input', function () {
    S.opacity = +opRange.value;
    if (opNum) opNum.value = S.opacity;
    ui('applyOpacity');
  });
  if (opNum) opNum.addEventListener('change', function () {
    var v = parseInt(opNum.value, 10);
    if (!(v >= 25 && v <= 100)) { v = (S.opacity >= 25 && S.opacity <= 100) ? S.opacity : 100; opNum.value = v; }
    S.opacity = v; ui('applyOpacity');
  });
  
  if (blurRange) blurRange.addEventListener('input', function () {
    S.blur = +blurRange.value;
    if (S.blur < 0) S.blur = 0;
    if (S.blur > 50) S.blur = 50;
    if (blurNum) blurNum.value = S.blur;
    ui('applyBlur');
  });
  if (blurNum) blurNum.addEventListener('change', function () {
    var v = parseInt(blurNum.value, 10);
    if (!(v >= 0 && v <= 50)) { v = (S.blur >= 0 && S.blur <= 50) ? S.blur : 15; blurNum.value = v; }
    S.blur = v; ui('applyBlur');
  });
  on('__gb_opacity', 'click', function (ev) {
    
    if (ev && ev.stopPropagation) ev.stopPropagation();
    var pop = document.getElementById('__gb_opacity_pop');
    if (pop) pop.style.display = (pop.style.display === 'flex') ? 'none' : 'flex';
    setTimeout(reportPanelRect, 30);
  });
  
  document.addEventListener('click', function (ev) {
    var pop = document.getElementById('__gb_opacity_pop');
    if (!pop || pop.style.display !== 'flex') return;
    var t = ev.target;
    if (!(t && t.closest && t.closest('#__gb_opacity,#__gb_opacity_pop'))) {
      pop.style.display = 'none';
      setTimeout(reportPanelRect, 30);
    }
  });

  on('__gb_close', 'click', function () { tellHost({ type: 'quitApp' }); });
  
  on('__gb_min', 'click', function () { tellHost({ type: 'minToTaskbar' }); });

  function bootStep(tag) {
    try {
      var cv = window.chrome && window.chrome.webview;
      if (cv && cv.postMessage) cv.postMessage(JSON.stringify({ type: 'bootStep', at: tag }));
    } catch (e) {}
  }

  function boot() {
    bootStep('0 进入 boot');
    ui('initState');                 
    bootStep('1 initState 完成');
    
    if (S.side !== 0 && S.side !== 1) S.side = -1;
    
    bootStep('2 进入主流程（页面画布在宿主模式下交给覆盖层）');

    ui('applyDark'); bootStep('4 applyDark'); ui('applyLang'); bootStep('5 applyLang');
    ui('applyOpacity'); bootStep('6 applyOpacity'); ui('applyBlur'); bootStep('7 applyBlur');
    
    posBtnLang();
    balBtnLang();
    swBtnLang();
    ui('paintHeat'); ui('paintOpp'); ui('paintSel'); ui('paintToggle');
    bootStep('8 paint* 完成');
    applySideUI();
    bootStep('9 applySideUI 完成');
    
    ui('drawChart');
    bootStep('10 drawChart 完成');
    ui('setStatus', T('connecting') || '连接引擎…');
    bootStep('11 setStatus 完成');

    reportPanelRect();
    bootStep('12 已上报位置');
    
    var cv = window.chrome && window.chrome.webview;
    if (cv && cv.postMessage) {
      cv.postMessage(JSON.stringify({
        type: 'ready',
        uiFns: (UI.names || []).length,
        ids: document.querySelectorAll('[id^="__gb_"]').length,
      }));
    }
    bootStep('13 已发 ready');
    setInterval(reportPanelRect, 700);
    
    setTimeout(function () {
      try {
        resizeCanvas();
        bootStep('3 resizeCanvas 完成（延后）');
      } catch (e) {
        bootStep('2x resizeCanvas 异常：' + (e && e.message));
      }
    }, 0);
    if (!HOST) setInterval(tick, 900);
    tick();
  }
  
  window.GB_BAL_POINTS = async function (req) {
    // Official engine balance search for the main panel: req = {
    //   board: 0/1/2 matrix, moves: ply count, need: 1..6, rule: 0/1/2/5,
    //   turnMs, onMarks(marks), onStatus(text) }.
    // One near-zero-advantage point per side, really placed so colours
    // alternate like real moves; every point is then scored across lanes.
    if (swBusy || balBusy) { try { req.onMarks([]); } catch (e) {} return; }
    balBusy = true;
    var bmN = (req.board || []).map(function (r) { return r.slice(); });
    var plyBase = req.moves || 0;
    var needN = Math.max(1, Math.min(6, req.need || 2));
    var ruleN = (req.rule === 0 || req.rule === 1 || req.rule === 2 || req.rule === 5) ? req.rule : 0;
    var tmsN = Math.max(600, Math.min(1500, (req.turnMs || 2000) * 0.6));
    var marksN = [];
    try {
      for (var qi2 = 0; qi2 < needN; qi2++) {
        var curB2 = ((plyBase + qi2) % 2 === 0);
        var colN = curB2 ? 'b' : 'w';
        var qb = { board: bmN, moveList: [], n: 1, mode: 'one', bias: 0,
                   rule: ruleN, turnMs: tmsN, lane: 'main',
                   cid: 'gb-balpts-' + plyBase + '-' + qi2,
                   side: curB2 ? 1 : 2 };
        var jn = null;
        try { jn = await onMainLane(function () { return wsCall('balance', qb, tmsN + 6000); }); }
        catch (eN) { jn = null; }
        if (!jn || jn.error || jn.swap || !jn.moves || !jn.moves.length) break;
        var ptn2 = jn.moves[0];
        marksN.push({ p: ptn2, color: colN });
        bmN[ptn2[1]][ptn2[0]] = colN === 'b' ? 1 : 2;
      }
      var evsN = [];
      if (marksN.length) {
        var lanes3 = ['main', 'sub', 'fwd'];
        var evMs = Math.max(250, Math.min(400, Math.round(tmsN * 0.3)));
        var evTasks = marksN.map(function (m, mi) {
          var scB = (req.board || []).map(function (r) { return r.slice(); });
          for (var si = 0; si <= mi; si++) scB[marksN[si].p[1]][marksN[si].p[0]] = marksN[si].color === 'b' ? 1 : 2;
          return wsCall('analyze', {
            board: scB, moveList: [], matchMs: 600000, turnMs: evMs,
            timeUsedMs: 0, topN: 1, rule: ruleN, lane: lanes3[mi % 3],
            cid: 'gb-balpts-ev-' + plyBase + '-' + mi,
          }, evMs + 6000).then(function (jv) {
            if (jv && !jv.error && jv.candidates && jv.candidates.length) return jv.candidates[0].eval || '';
            return '';
          }).catch(function () { return ''; });
        });
        evsN = await Promise.all(evTasks);
      }
      var out = marksN.map(function (m, mi) {
        return { x: m.p[0], y: m.p[1], color: m.color, ev: evsN[mi] || '', seq: String(mi + 1) };
      });
      try { req.onMarks(out); } catch (e) {}
    } finally {
      balBusy = false;
    }
  };
  window.GB_LINK_SWAPBAL = runSwapBal;
  window.GB_LINK_SWAPEVAL = runSwapEval;
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', boot);
  else boot();
})();
