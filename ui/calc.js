/* Meter Gomoku Trainer - analysis and calculation bridge. Wraps the native rapfi engine through in-process hard links and exposes evaluation/analysis APIs to the overlay UI. */


'use strict';

window.__gbTop = true;   

var N = 15;

var RV_MODE = /(?:^|[?&])rv=1(?:&|$)/.test(window.location.search || '');

var VIS_MODE = /(?:^|[?&])vis=1(?:&|$)/.test(window.location.search || '');

var LINK_MODE = /(?:^|[?&])link=1(?:&|$)/.test(window.location.search || '');

var HOST = !!(window.chrome && window.chrome.webview && window.chrome.webview.postMessage);

var tellHostDefer = false;
function tellHost(o) {
  if (!HOST) return;
  if (tellHostDefer) { setTimeout(function () { window.chrome.webview.postMessage(o); }, 0); return; }
  window.chrome.webview.postMessage(o);
}

var TEST_RV_RECORD = {
  ts: 0, src: 'local', hist: true,      
  moves: [[7, 7, 1], [8, 7, 2], [7, 8, 1], [8, 8, 2], [7, 9, 1],
          [8, 9, 2], [7, 10, 1], [8, 10, 2], [7, 11, 1]],
};

var I18N = {
  zh: {
    title: 'Meter Gomoku',
    setup: '对局设置', mode: '模式', side: 'AI 执子', turn: '思考时间',
    
    secGame: '对局', secEngine: '引擎', secView: '棋盘显示', secCode: '局面',
    secFoldTip: '点击折叠 / 展开这个板块',
    
    rule: '规则', cores: '线程数', hash: '哈希表',
    weights: '引擎权重', weightsHint: 'NNUE 评估更准；传统估值更快，快棋更划算',
    wNnue: 'NNUE（准确）', wClassic: '传统估值（快）',
    variant: '引擎变体', vAuto: '自动（推荐）',
    vHintAuto: '自动：按本机指令集 + 实测吞吐选最快（推荐）',
    vHintBench: '本机实测最快：{v}',
    vHint512vnni: 'AVX-512 VNNI：宽向量 + int8 点积，现代 CPU 通常最快',
    vHint512: 'AVX-512：无 int8-VNNI 路径；Zen4 上与 VNNI 相当',
    vHintVnni: 'AVX-VNNI：256 位 int8 点积（部分 Intel CPU）',
    vHint2: 'AVX2：无 512 位路径，本系列权重下明显偏慢（约 1/3 速度）',
    vHintSse: 'SSE：最老兜底，仅应急',
    sysBench: '实测最快',
    candRange: '选点范围', maxDepth: '最大深度', maxNodes: '最大节点',
    cr2: '0 · 最窄（5×5）', cr2l3: '1 · 偏窄（5×5+3线）', cr3: '2 · 中性（7×7）',
    cr3l4: '3 · 推荐（7×7+4线）', cr4: '4 · 偏宽（9×9）', crFull: '5 · 最宽（全盘）',
    candRangeHint: '越大越不容易漏点，同时间算得更浅',
    multiPv: '分析点数', multiPvHint: '「分析计算」最多给出的点数（与分析面板同一设置）',
    multiPvUnit: ' 个点',
    mdNone: '不限', mnNone: '不限',
    phNone: '不限',
    phDepth: '4 ~ 128 的整数，留空 = 不限',
    phNodes: '0.1 ~ 256（单位 M），留空 = 不限',
    coresHint: '本机 {n} 线程 · 上限 {m}',
    coach: '指导视图', coachHint: '方形四色热力：AI 帮你算最佳落点，落子即消失',
    heatBest: '最佳', heatGood: '良好', heatFair: '一般', heatPoor: '劣势',
    
    sideAiB: 'AI 执黑', sideAiW: 'AI 执白',
    
    noNativeEngine: '本机不支持原生 AI 引擎（需要支持 AVX2 指令集的 CPU），AI 功能不可用',
    sideHintB: 'AI 执黑先走，你执白',
    sideHintW: 'AI 执白后走，你执黑（你开第一手）',
    sideHintBoth: 'AI 自打：黑白双方都由 AI 走，你只旁观 —— 棋盘正中那颗键就是开关键（❚❚ 停手 / ▶ 继续）',
    sideHintBothPlace: 'AI 自打已就绪，但「自由摆盘」是摆盘用的、不会自动走子 —— 切到「对弈」就自动下起来',
    spPause: '停手（暂停自动对弈）',
    spResume: '继续自动对弈',
    sideHintNone: '两个都不开：没有 AI，黑白双方都由你自己下',
    turnAiBoth: 'AI 自打 · 轮到{c}',
    turnYouBoth: '轮到你 · {c}（双方都你下）',
    showNum: '显示序号', showNumHint: '棋子上标出第几手',
    bgThink: '后台思考', bgThinkHint: 'AI 落子后你在思考时，引擎在后台继续深算这一局面（算你的优势点 + 它的应对），你一落子它从热置换表接着算',
    bgThinkOffAi: 'AI 对打期间已自动关闭', bgThinkOffBusy: '有计算在跑，已自动暂停',
    
    rvNum: '序号', rvNumHint: '在棋子上标出它是第几手',
    rvKeyHint: 'Ctrl+Z 上一手 · Ctrl+R 重来（回到开局）',
    preview: '预览框', previewHint: '鼠标悬停时显示的圆角方框',
    fwd: '前瞻',
    fwdIdle: '点「前瞻」，从当前残局自动推演到五子连珠',
    fwdRunning: '推演中… 已推 {k} 手',
    
    fwdEta: '计算中 · 首手预计 {d} 秒内给出（加强全局计算），全程预计 {a} 秒 ~ {b} 秒',
    anEta: '计算中… 预计 {s} 秒内给出',
    fwdDone: '已模拟推演到五子连珠（共 {n} 手）——引擎模拟线，非算杀必胜：悬浮代码预览、点击确定到那个局势',
    fwdCap: '推演停在 {n} 手：引擎这一步没给出可下的点（引擎模拟线，非算杀必胜；局面变了会自动重推）',
    fwdFull: '棋盘已铺满（共 {n} 手）仍未连五 —— 和棋（引擎模拟线，非算杀必胜）',
    fwdVcf: 'VCF 算杀成功：{n} 手连续冲四必胜（防守每手都被迫）——悬浮代码预览、点击确定',
    fwdVct: 'VCT 算杀成功：{n} 手连续威胁（冲四/活三）必胜——悬浮代码预览、点击确定',
    
    fwdVcfHit: '找到VCF杀，共{n}手；进攻方：{c}（序号：奇数=进攻方落子，偶数=防守方落子）',
    fwdVctHit: '找到VCT杀，共{n}手；进攻方：{c}（序号：奇数=进攻方落子，偶数=防守方落子）',
    fwdVcfNone: '当前局面，{c}（所选进攻方）不存在VCF强制连续冲四杀棋',
    fwdVctNone: '当前局面，{c}（所选进攻方）不存在VCT强制连续冲四杀棋',
    fwdClear: '清除计算',
    
    markCleared: '已清除棋盘上的所有分析标记（计算 / 多点分析 / 平衡点 / 扫描防守 / 前瞻 · VCF · VCT）',
    vcxPopTitle: '算杀结果',
    fwdAtkAuto: '进攻方：自动 —— 轮走方（算杀题面按惯例 = 子多的一方）。黑/白框只是**指示框，不可点**',
    fwdAtkB: '进攻方：黑棋（自动判定）',
    fwdAtkW: '进攻方：白棋（自动判定）',
    
    fwdFindVcf: 'VCF', fwdFindVct: 'VCT',
    fwdFindRun: '正在查找{k}……（多线程并行 + 迭代加深）',
    
    fwdProbing: '正在分析突破口：{r} ……（首次分析可能稍慢，随后逐手向前走；能接手就切进杀棋序列）',
    fwdVcxLater: '前瞻推进 {k} 手后，进攻方出现 {r} 杀（{n} 手）——已并入推演线，一路走到五连',
    fwdFiveNoVcx: '已五子连珠（共 {n} 手）—— 全程未出现{c}的 VCF / VCT 进攻可能，也未用到算杀（引擎推演线）',
    fwdBlack: '黑子', fwdWhite: '白子',
    fwdOk: '确定',
    fwdLegend: '进攻方指示框（不可点）：紫框 = 黑子，浅蓝框 = 白子；自动 = 轮走方（算杀题面 = 子多一方）',
    ten: '十打（叫 10 个候选点）',
    docTitle: 'Meter Gomoku',
    dragCard: '按住标题栏可拖动换位；点击小横杠折叠 / 展开卡片',
    codeTip: '局面代码：小写列字母 + 行号，逐手拼接（与 gomocalc.com 同格式）',
    nMoves: '{n} 手',
    about: '关于', aboutClose: '关闭',
    aboutTip: '版本、使用方法、功能与开源许可',
    aboutGuide: '使用方法',
    aboutGuideList: [
      { t: '一、开始一盘棋', ps: [
        '顶部「对局设置」里选模式：对弈（AI 自动应答）/ 摆盘（自由摆子，AI 完全不参与）/ 残局（自己摆一个局面来研究）。',
        '「思考时间」= AI 每手思考多少秒（内部按毫秒交给引擎）；时间越长棋力越强，热力与指导视图的色档也随它变化。',
        '「AI 执子」是两个独立开关：只开「AI 执黑」→ 你执白；只开「AI 执白」→ 你执黑并开第一手；两个都开 → AI 与 AI 自打；两个都关 → 没有 AI，黑白双方都由你自己下。',
        '棋盘上直接点击落子。中间那颗 ▶ / ❚❚ 键：空闲时点它 = 让 AI 立刻替当前行棋方算一手并落下；AI 正在思考时点它 = 暂停。',
        '「重新开始」先把当前这局自动存进历史再清盘；「保存局面」手动把当前盘面存入历史。',
      ]},
      { t: '二、残局模式（自己摆一个局面来研究）', ps: [
        '棋盘下方三个键：「确定」/「顺序摆盘」/「任意摆盘」。顺序摆盘 = 黑白轮流落子；任意摆盘 = 点开右边的圆角小框，选「黑子」或「白子」后可连续摆这一色，数量与顺序都不限（比如 30 个黑子 + 10 个白子）。',
        '选中「黑子」键变浅紫、「白子」键变浅白蓝，一眼看得出正在摆哪一色。',
        '摆好后点最左边的「确定」→ 残局定下来，键变浅绿并显示「已确定」；此后「重新开始」会回到这个局面，存进历史时它也是首帧。再点一次「已确定」即解除，可重新摆。',
        '残局模式下 AI 全程不参与（辅助一手、热力、指导视图都停摆），交换手类规则自动变灰 —— 摆盘研究不需要换先。',
      ]},
      { t: '三、计算评估', ps: [
        '「计算」= 单路深搜当前局面并把最佳点标在棋盘上（再点一次 = 停止计算）；「多点分析」按「分析点数」一次给出 2~8 个候选，圆圈里是名次与评估分。',
        '「平衡点」= 官方平衡搜索（引擎原生 **YXBALANCETWO 成对**，**不是**对弈引擎的搜索）按你选的「点数」给出黑白双方都没有明显优势的候选点，并把引擎给的评估分一并报出来（|评估| ≤ 50 才是官方平衡窗口内的「平」；评估刻度 ScalingFactor=200 ⇒ 胜率 = sigmoid(评估/200)）；「点数」可在 1~6 之间切换（空盘固定 3 个：黑 1 固定天元，白 2 / 黑 3 由一次**成对**平衡搜索联合给出 —— 成对搜索才是官方口径的「平」，单点搜索在近乎空盘上评估会到 −350~−470，那是「一眼看出谁占优」的根源）；点数为奇数时**收尾那手也走成对搜索、只取这一对的第 1 手**，所以报出的评估分同样在 0 附近；「扫描防守」= 引擎原生 **YXSEARCHDEFEND**（Rapfi 整合包同一算法）：先补一手 PASS 把行棋方让给对手，再对全盘候选点持续深算 —— 每个点的 % 就是「对手下在这里」的胜率，越高越要防；棋盘按危险度分五档：1 档最大圆环 + 半透明填充，2~4 档圆环依次减小、透明度依次升高（最小圆环 = 1 档的一半），5 档（边角 / 低危点）只铺一个数字、外围一圈更细的小环（大小与 4 档一致），像扫盘一样铺开；整套配色为天蓝色系（高危深、低危偏白）；搜索越算越深、越算越准，点「停止扫描」后**最后一帧保留在棋盘上**，落子或「清除计算」才消失。',
        '「AI 视图」= AI 思考期间在棋盘上铺它的候选点（圆形、格内是评估分），AI 一落子就撤掉；「指导视图」= 轮到你时用方形四色提示最佳落点，你一落子就消失。',
        '「分析计算」= 棋盘下方的开关型功能键：开启后在**主车道**持续深算当前局面，铺出引擎当前榜单（最佳点+候选，最多 8 个）；随搜索加深自然微变并收敛，用户落子 / 悔棋自动重开会话，仪表盘用时跨轮累加；再点一次 = 停止。摆盘 / 残局 / 双人手动对弈同样可用。',
        '「清除计算」与「计算」并排：一键擦掉**本卡里任意项目**留在棋盘上的标记 —— 计算 / 多点分析 / 平衡点的徽标与评估分、扫描防守的五档圆环与百分比，以及前瞻 · VCF · VCT 的序号、虚线与代码链；只清标记，不动局面本身。',
        '仪表盘同步给出深度、速度、节点、用时与评估分；下方评估曲线按黑 / 白两条记录整局走势（纵轴随分数自动进位）。',
      ]},
      { t: '四、前瞻 / VCF / VCT', ps: [
        '「前瞻」从当前局面逐手推演，一路走到盘面真的五子连珠才结束 —— 不凭引擎的胜负判断提前定性（最坏情况是铺满棋盘判和）。第一手会加强全局计算来找突破口，提示栏同时给出预计耗时区间。',
        '「VCF」= 连续冲四必胜搜索；「VCT」= 冲四 + 活三连续威胁搜索。两者都跑本地算杀器（多线程并行 + 迭代加深），命中即给出必胜线；没命中会明确告知「所选进攻方不存在该强制杀」。选了 VCF / VCT 时边走边回探，一能接手就切进杀棋序列。',
        '点「黑子 / 白子」框 = 指定算杀的进攻方（进攻训练选己方色，防守预判选对手色），再点一下取消。',
        '推演子是半透明的：数字 = 第几手；悬浮代码链可预览到那一手，点击代码选中、「确定」把这一段落到棋盘（之后还能接着往下确定）。',
        '推演每一步都做必应校验：能连五就连五、对手只差一手成五就必堵、对手活三而自己无四则必应 —— 防守方不会白白送掉杀棋。',
      ]},
      { t: '五、复盘与历史', ps: [
        '「历史」抽屉列出保存的局面：单击选中（↑↓ 移动、Enter 打开），右键菜单可打开复盘 / 重命名 / 导出这一局 / 存入「保存历史」/ 删除；支持 Ctrl / Shift 多选与 txt 导入导出。',
        '顶栏「复盘」= 打开一个只有棋盘的独立窗口（不连 AI，也不参与主窗的其他功能）；从历史打开的一局可以用「背诵复盘 / 回顾复盘」逐手走。VC / 残局记录打开时以整盘为首帧，不必从第一手重放。',
      ]},
      { t: '六、识图（Gomoku Vision）', ps: [
        '「上传图片 / 截图」→ 选框自动贴住棋盘（并向外多留约棋盘 1/30 的余量，边缘棋子完整进图）→「识别」；结果同步成局面代码，可复制、也可载入练习。',
        '「修改」= 人工微调：交换黑白子（点一黑一白即互换）、删除棋子、补充黑子 / 白子；Ctrl+Z 撤销。',
        '「VC 模式」= 算杀题模式：黑白子数可以不等，补充黑 / 白子不限数量、删除棋子任意删，左侧提示栏逐条说明。载入练习后自动切成残局模式，只预设进攻方（子多的一方），算杀不会自动开 —— 点「VCF」或「VCT」由你自己决定。',
        '「裁剪」可在识别前先框选图片区域；每次上传的图片自动进「图片抽屉」（超过 150 张清最老），支持多选、删除与复制。',
      ]},
      { t: '七、规则与外观', ps: [
        '规则：无禁手 / 无禁手 · 长连不赢 / 连珠（有禁手，黑方三三、四四、长连禁手）；前瞻与算杀都跟随所选规则。',
        '「EN / 中」一键切换全界面语言（含识图截图框的按键与历史日期格式）；「颜色」打开调色窗（棋盘 / 页面底色 + 深浅主题 + 预制色）；右栏卡片可拖动换位、点小横杠折叠，布局自动记忆。',
      ]},
      { t: '八、连线功能（对战平台 / 外部棋局）', ps: [
        '打开与框选：顶部「连线」键打开连线窗 → 点「框选棋盘」在屏幕上拖出目标棋盘的范围 → 点窗栏的「识别」键。框由紫色变蓝绿色、四角出现 L 形角框与定位点 = 识别成功。识别全程静默在后台做（帧间像素差分：画面静止时零消耗，棋子一变才重新识别），框色不会来回闪。',
        '分工：连线窗负责识别与 AI；主窗大棋盘只同步显示识别到的局面与执子设置，不参与评估、不落子 —— 所以不会出现两侧互相抢着算的情况。',
        'AI 执子与思考时间：连线窗的「AI 执黑」/「AI 执白」二者互斥，选中会无条件同步到主窗「对局设置」里的执子；「思考时间」同样同步（两边一个口径）。',
        '自动落子：打开后识别到对方落子即自动思考，并用三级模拟点击替你落子（先 PostMessage 直达目标窗口，无效升 SendInput，再不行用真实点击兜底），物理鼠标全程不受干扰；一局结束（五子连珠）自动关闭，下一局需重新打开。',
        '交换平衡点：点它的同时会自动取消已选的 AI 执黑 / 执白（避免相互干扰）。空盘时走**官方定式**：先让引擎按当前规则吐官方开局（禁手用禁手公式、无禁手用无禁手公式，**绝不混用**），引擎不可用时退到本地公式表（无禁手 = 官方 Swap2 三手 9 组，禁手 = RIF 26 组，两张表都经引擎实测只留平衡定式、必胜定式一条都不加）；再不行才在中心区逐手取点。所以开局这三手是「官方平衡开局」，不是对弈引擎算出来的最强手。棋盘上已有棋子时（第 2 次交换，一黑一白），走原生 YXBALANCETWO 成对搜索给出这两手并报出评估分（|评估| ≤ 50 才算平）。黑紫白天蓝，可反复点击重新评估。评估完成引擎随即释放；你把相应子数下完（不管下哪），标记自动消失，也可点「清除标记」关闭。',
        '交换平衡点 + 自动落子的顺序逻辑：先点交换平衡点、后打开自动落子 → 评估完成时若自动落子已开，就按顺序自动把这几个平衡点落下（与单机版五子棋助手完全一致）；若是评估完才打开自动落子，则打开的那一刻按序补摆。',
        '交换评估：与交换平衡点相互独立的另一项功能，用于评估当前交换局面的优劣，判断是否值得交换。',
        '盘中标记：推荐落点为天蓝圈；平衡标记为圆形 —— 上半中空显示落子序号、下半半透明填充显示评估分，黑子紫、白子天蓝；全部直接画在识别框内的对应交点，位于截图识别不到的最上层（绝不会被误读成棋子）。 —— 全部直接画在识别框内的对应交点，且不会被截图识别（绝不会被误读成棋子）。',
        '关闭：关掉连线窗后，主窗自动恢复你自己原来的模式与设置，对弈 / 计算等功能照常使用。',
      ]},
    ],
    updLine: '最新版下载：', updPwd: '密码',
    checkUpdate: '获取最新版',
    updateOpened: '已打开下载页，密码 {p} 已复制',
    aboutVer: '版本', aboutFeat: '功能', aboutOss: '开源组件', aboutLic: '许可',
    aboutUpdates: '最近更新',
    aboutUpdateList: [
      '扫描防守重写：改走 Rapfi 整合包同款引擎原生 YXSEARCHDEFEND（补一手 PASS 让对手行棋，每个点的 % = 对手下在这里的胜率）；按危险度分五档显示 —— 1 档最大圆环 + 半透明填充，2~4 档圆环依次缩小、变淡（最小 = 1 档的一半），5 档边角点铺数字、外围一圈更细的同尺寸小环扫盘；整套配色改为偏白的天蓝色系，最大圆环缩到与棋子等大、圆环更细；按钮变「停止扫描」，停止后最后一帧保留，落子或「清除计算」才消失；「清除标记」改名「清除计算」；多点分析 / 前瞻 / 文字提示统一改用虚线矩形框',
      '识图图片适应性大升级：深色 / 浅色主题、旋转截图、拍屏照片、低对比与模糊图都能读 —— 新增「局部对比读子」第三套判别器（按每个交点自己的底色定阈值，深色盘不再丢黑子、浅色盘不再把空格读成白子）、找盘阶梯（3×3 高斯 → CLAHE 均衡 → 1.5× 放大）与几何校验兜底（网格没对上时自动改用旧识别器重算）',
      '识图「识别参数」面板：打开即自动收起棋盘代码框；提示行改成固定高度的虚线文本框 —— 拖滑动条时整列不再上下抖',
      '界面资源改为单一 ui.pak（不再是一堆 .enc 加密文件）：避免被杀毒软件当成「加密文档」误删导致窗口白屏；真缺文件时会显示一张诊断页（告诉你加白名单后重装），而不是一片空白',
      '引擎调用整体回归 4.36 口径（官方 Rapfi 单发 Analyze）：评分不再正负颠倒，思考预算与 ponder 节奏回到 4.36 实测最稳的那套',
      '智力回归（对齐官方 Rapfi 单发 Analyze）：「后台预热」整块删除，落子不再预搜；车道分配恢复 09-27 早上口径（主搜留核给视图车道、哈希 60/24/16 三分）',
      '分析计算只分析 AI 未执子的一方（AI 执黑→分析白，执白→分析黑）；AI 自打时功能键停用；评估曲线白子面积填充改浅天蓝（不再发紫）',
      '界面连接升级为进程内拦截器（零 TCP）：UI 与引擎桥全部由 C++ 宿主进程内直答，不再监听任何端口',
      '智力回归：落子决策链撤除「浅模拟否决」与「历史众数覆盖」两层干扰 —— 引擎最佳点直接落子，只保留必应护栏（连五 / 堵五），原生引擎恢复满血棋力',
      '分析计算重做（gomocalc 式持续分析）：主车道满血持续深算、无时间窗口，最佳点随搜索加深自然微变并收敛；用时跨轮累加，落子 / 悔棋自动重开；摆盘 / 残局 / 双人手动同样可用',
      '摆棋评分下架：对局设置中的开关移除，盘面评估统一由「分析计算」接管（主车道深算取代 1 线程浅评分）',
      '引擎仪表盘实时化：任意计算（AI 落子 / 计算评估 / 多点分析 / 平衡 / 扫描防守 / 热力 / 前瞻 / 后台预热）期间，速度与节点**边算边跳**；标题栏状态胶囊改青 / 浅天蓝色系：青点脉冲 = 计算中（并标注正在算的项目），绿点 = 空闲',
      '推荐选点改用卡塔狗名次色：1 选蓝、2 选绿、3 选黄（4 橙、5 之后红）——不再与扫描防守的红黄青互相串色',
      '棋子数字随棋盘缩放：序号 / 评估分 / 各类徽标的字号不再有 14px 封顶，高分辨率最大化窗口下也清晰可读',
      '计算时间与思考时间对齐：计算评估放开 12 秒封顶（1.6× 思考时间）、前瞻首手加强计算放开 12 秒封顶（4× 思考时间），每一步推演本就随思考时间走',
      '后台预热（默认关闭，勾选开启）：对手思考时 AI 在后台持续预演你的应手 —— 落子后命中即秒答，未命中置换表也是热的，同样的时间能搜得更深',
      '多引擎投票已移除（算力集中一路 = 智力上限）：引擎为单实例、吃满「核心数」所选全部线程；裁决层只保留必应护栏（连五 / 堵五 / 活三）与引擎头名，选点更稳',
      '扫描防守配色升级：徽标与清单从五档名次色改为青→绿→黄→橙→红的连续渐变（%越高越红）',
      '识图强化：密集局面识别失败时按锁定网线整盘外推重建（不再 no-grid）；识别窗口向外扩约棋盘 1/30，边缘棋子读得更全',
      '「清除标记」搬家并泛化：键从「前瞻」框移到「计算评估」卡、与「计算」并排（各占半宽），一次点击可擦掉**任意项目**留下的棋盘标记（计算 / 多点分析 / 平衡点 / 扫描防守 / 前瞻 · VCF · VCT）',
      '新增「残局」模式：顺序摆盘 / 任意摆盘（数量与顺序都不限），并用「确定」键把局面定下来 —— 重新开始回到它，存进历史时它就是首帧',
      '识图「VC 模式」放开：补充黑 / 白子不限数量、删除棋子任意删，左侧提示栏逐条说明',
      'VCF / VCT 标记精简：只留蓝色虚线杀路 + 五（深蓝双环）/ 四（天蓝方框），不再满盘花',
      '前瞻首手改为加强全局计算找突破口，首次预算从 ×6 降到 ×4；提示栏新增预计耗时区间（秒 ~ 秒）',
      '攻防算法修正：行棋方显式传给引擎（子数不均衡不再站到对面去算），并加必应校验 —— 连五优先、对手成五必堵、活三必应',
      'VC / 残局记录打开即以整盘为首帧，不必再从第一手重放；残局模式下交换手规则变灰',
      '三个模式键缩短为两字（对弈 / 摆盘 / 残局）；英文模式补齐「清除标记」等文案，历史日期随语言',
      '截图自动贴盘的外扩修正：改为向外扩约棋盘 1/30（原先扩到了框内侧，边缘棋子被压住）',
    ],
    aboutApp: 'Meter Gomoku',
    aboutFeatList: [
      '三种模式：对弈（AI 执黑 / AI 执白可自由开关，两个都开即 AI 自打）/ 自由摆盘 / 残局（自摆局面 + 「确定」定盘，重新开始回到该局面）',
      '前瞻推演：一路走到盘面真的五子连珠；VCF / VCT 本地算杀（多线程并行 + 迭代加深）',
      '计算评估：计算 / 清除计算 / 多点分析 / 平衡点 / 点数 / 扫描防守，另配 AI 视图与指导视图',
      '评估曲线：纵轴随分数动态进位，最大可到 ±1200；仪表盘给出深度 / 速度 / 节点 / 用时',
      '三种规则：无禁手 / 无禁手 · 长连不赢 / 连珠（有禁手）',
      '识图：截图或上传自动贴盘识别，VC 算杀题模式，人工修改（换色 / 删子 / 补子 / 裁剪）与图片抽屉',
      '历史与复盘：txt 导入导出、重命名、右键菜单、独立复盘窗口（背诵 / 回顾，VC 记录整盘首帧）',
    ],
    aboutOssList: [
      ['Rapfi', '五子棋 / 连珠引擎内核（编译为 WebAssembly，随本软件分发）', 'GPL-3.0'],
      ['OpenCV', '棋盘与棋子识别（静态链入 GomokuVision.exe，不额外分发 DLL）', 'Apache-2.0'],
      ['Microsoft WebView2 Runtime', '桌面界面运行时（由 Windows / Edge 提供，本软件不分发）', '专有'],
    ],
    aboutOssNote: '说明：上表只列随本软件分发、或运行时必需的第三方组件。开发期用来对拍的脚本与其依赖（NumPy、Pillow 等）只在本机跑，不随本软件分发，因此不在此列。',
    aboutLicText: '本软件自身的界面与配套代码以 MIT 许可发布。棋力内核 Rapfi 以 GPL-3.0 发布，本软件以编译后的 WebAssembly（页面内 Worker）或独立进程的方式调用它，双方只通过消息与本地端口通信，不与其合并为同一作品；随本软件分发 Rapfi 时，须一并保留它的许可证与源码获取方式（可从其上游仓库 dhbloo/rapfi 取得对应源码）。识别所用的 OpenCV 以 Apache-2.0 静态链入 GomokuVision.exe。界面运行时 Microsoft WebView2 由 Windows / Edge 提供，按其自身条款使用，不随本软件分发。以上各组件的著作权与许可均归各自作者所有。',
    engine: '引擎仪表盘', depth: '深度', speed: '速度', nodes: '节点', time: '用时',
    calcOn: '计算中', calcIdle: '空闲',   
    
    tagAiMove: 'AI 落子', tagAssist: '辅助建议', tagHeat: '热力图', tagCoach: '教练',
tagDefend: '扫描防守', tagCalc: '计算评估', tagNbest: '多点分析',
    tagLiveAna: '分析计算', tagBg: '后台思考', liveAna: '分析计算', liveAnaStop: '停止分析',
    liveAnaBan: 'AI 自打：黑白双方都由 AI 走，没有用户执子方可分析，功能键停用',
    liveAnaRun: '分析计算 · 持续深算中 · 当前深度 {d}',
    anCalcRun: '计算评估 · 持续深算中 · 当前深度 {d}',
    anNbestRun: '多点分析 · 持续深算中 · 当前深度 {d}',
    liveAnaStopped: '已停止计算 · 深度 {d} · 保留停止时刻的标记与评估',
    btnAiside: 'AI 执子', btnSet: '设置', pinFix: '固定',
    pinFixTtl: '固定这张卡片：设置里开其它卡片时它不再被收起',
    pinCloseTtl: '取消固定并关闭这张卡片',
    aiSideB: 'AI 执黑', aiSideW: 'AI 执白', aiTurnLbl: '思考时间',
    tagBal1: '一手平衡', tagBal2: '二手平衡', tagFwd: '前瞻推演', tagTen: '十打选点',
    eval: '评估', best: '最佳', curve: '评估曲线', blackCurve: '黑子', whiteCurve: '白子',
    score: '分数', winRate: '胜率',
    
    history: '历史', saved: '保存历史', openDrawer: '历史',
    saveSel: '保存选中', delSel: '删除', openSel: '打开', closeDr: '关闭',
    selAll: '全部选中', selNone: '取消全选',
    drTip: '勾选若干局，点「保存选中」→ 永久存入「保存历史」',
    
    drExp: '导出', drImp: '导入',
    ctxOpen: '打开复盘', ctxRename: '重命名', ctxExport: '导出这一局',
    ctxToSaved: '存入「保存历史」', ctxDel: '删除',
    namePh: '给这局起个名字',
    expNone: '没有可导出的历史',
    expOne: '已导出 1 局为 txt（在「另存为」里选路径）',
    expDone: '已导出 {n} 局为 txt（在「另存为」里选路径）',
    impDone: '已导入 {n} 局到历史',
    impNone: '这段文本里没有能识别的局面代码（每行形如 h8h9g7…）',
    impFail: '读取 txt 失败',
    renamed: '已重命名',
    code: '局面代码', copy: '复制', paste: '粘贴', load: '载入',
    copied: '局面代码已复制', codeLoaded: '局面代码已载入棋盘', clipEmpty: '剪贴板里没有文本',
    codeBad: '局面代码读不出来：形如 h8h9g7（小写列字母 + 行号，逐手拼接）',
    reset: '重新开始', redo: '重来',
    play: '播放', pause: '暂停',
    recite: '背诵复盘', replay: '回顾复盘', exit: '退出复盘',
    review: '复盘', prev: '上一步', next: '下一步', savePos: '保存局面',
    rotTtl: '整体棋子顺时针旋转 90°（坐标轴不动）', mirrorTtl: '翻转棋子布局',
    shiftTtl: '局面平移', shiftBtnTtl: '局面步进（整体平移一格）',
    mvFv: '左右', mvFh: '上下', mvD1: '╲', mvD2: '╱',
    mvFvT: '左右翻转', mvFhT: '上下翻转',
    mvD1T: '对角翻转（左上↔右下）', mvD2T: '对角翻转（右上↔左下）',
    mvUpT: '整体向上平移一格', mvDownT: '整体向下平移一格',
    mvLeftT: '整体向左平移一格', mvRightT: '整体向右平移一格', closeTtl: '关闭',
    layoutRV: '复盘模式下不能调整布局', layoutBusy: 'AI 思考中，请稍候再调整布局',
    shiftOOB: '有棋子会被移出棋盘，无法向该方向平移',
    posSaved: '局面已保存为 PNG 图片', posSaveFail: '局面保存失败',
    pause: '暂停', resume: '继续', assistHint: '点击：AI 辅助计算一次并落子一着',
    reciteHint: '背诵复盘：凭记忆把下一手落在棋盘上，错了会用粉红圈标出。',
    miss: '背错', turnBlack: '轮到黑棋', turnWhite: '轮到白棋',
    
    turnYou: '轮到你', turnAi: '轮到 AI',
    over: '对局结束', thinking: 'AI 思考中…', paused: '已暂停', youWin: '你赢了', youLose: 'AI 获胜',
    analyzing: '分析模式：不自动落子，只看候选与热力',
    modePlace: '自由摆盘',
    
    modeEndgame: '残局',
    egSeq: '顺序摆盘', egFree: '任意摆盘', egB: '黑子', egW: '白子',
    egFreeOn: '任意摆 {c}',
    egHint: '残局模式：AI 不参与，你摆什么它就是什么。「顺序摆盘」= 黑白轮流落子；「任意摆盘」= 弹出圆角小框选黑子或白子，选中一色后可在棋盘上连续摆放（不限数量、不限顺序，比如 30 黑 + 10 白）。摆好的盘面就是首帧，可直接保存到历史或开始研究。',
    
    egOk: '确定', egOkOn: '已确定',
    egLockedHint: '残局已确定，已自动切到「摆盘」：「重新开始」会回到这个残局，存进历史时它就是首帧（历史里标 Endgame），背诵复盘也从残局开始。要重摆就回「残局」模式。',
    
    egDash: '残局：自己摆局面，AI 不参与',
    egDashLocked: '残局已确定 · 重新开始回到它',
    offline: 'AI 引擎还没就绪（首次启动要加载模型，几秒）。马上自动重试。',
    offlinePill: '引擎未连接 · 正在重试…',
    rvSpeedTtl: '单步间隔 0.5~5 秒',
    closeCard: '关闭卡片',
    rvFree: '复盘 · 自由摆盘，不连 AI',
    
    rvWindow: '复盘 · 纯棋盘',
    rvPure: '复盘 · 自己落子（没有 AI，也没有禁手/规则）',
    rvFromHist: '复盘 · 从历史打开的一局',
    
    rvNothing: '棋盘还是空的：先在棋盘上摆几手，或去历史里挑一局',
    srcLocal: '本机', srcDesktop: '桌面', srcBookmark: '书签',
    theme: '深色', theme2: '浅色', themeCustom: '自定义',
    
    color: '颜色', cpClose: '关闭', cpReset: '恢复默认',
    cpTheme: '主题', cpTarget: '调色对象', cpBoard: '棋盘', cpPage: '背景',
    cpHex: 'RGB 代码', cpPreset: '预制色',
    cpHintB: '调的是棋盘底色；网格线、星位、坐标轴会自动跟着深浅变化',
    cpHintP: '调的是页面底色；卡片与文字会自动按明暗跟随',
    cpBad: '色值读不出来：要 #rrggbb，或 rgb(r,g,b)',
    
    cards: '卡片', cardsHint: '选择显示的面板',
    cardSideL: '左', cardSideR: '右',
    
    analysis: '计算评估',
    
    anCalc: '计算', anStop: '停止计算', anDefend: '扫描防守', anStopScan: '停止扫描',
    anNbest: '多点分析', anBal: '平衡点', anBaln: '点数',
    anDefendDone: '扫 {c} 点：%越高越要防，W=距赢 L=距输',
    anDefendRefine: '持续加深…第 {r} 轮 · 每轮 {t}s',
    anDefendStable: '已收敛，结论稳定',
    anScanRun: '扫描防守 · 引擎全点深算中… 深度 {d} · 已扫 {c} 点（%越高越要防）',
    anScanFrozen: '已停止扫描：保留最后一帧（{c} 点）——落子 / 清除计算后消失',
    
    shell: '搜索',
    
    sysCpu: '处理器', sysCore: '核心', sysIsa: '指令集', sysMem: '系统内存', sysUse: '占用',
    sysOs: '系统', sysDisk: '磁盘可用', sysAvail: '可用',
    sysNative: '原生引擎', sysWasm: '网页引擎', sysUnknown: '未知',
    
    hmExcl: '独占', hmExclVal: '整档 {m}MB 给当前事件（落子 / 分析二选一）',
    hmFloor: '待机', hmFloorVal: '其余车道各 64MB 保活',
    hmUse: '占用', hmSum: '合计', hmPct: '占内存',
    
    abSys: '系统信息', abSysNone: '尚未收到宿主系统信息',
    abSysCpu: '处理器', abSysIsa: '指令集', abSysMem: '系统内存',
    abSysCores: '{p} 核 {l} 线程', abSysCache: 'L1 {a} · L2 {b} · L3 {c}',
    abSysUsed: '已用 {u}% · 可用 {a}', abSysEngine: '引擎变体',
    fwdAiOff: '前瞻已接管引擎：AI 自动落子暂停（关前瞻恢复）',
    vis: '识图',
    visTitle: '识图 · 把棋盘图片变成局面',
    visTtl: '把棋盘图片变成局面',
    visHint: '支持一次选多张（至多 150 张）；白框里可直接 Ctrl+V 粘贴图片，截屏弹出三键选择',
    visBoxHint: '在这里输入或粘贴局面代码（如 h8h9g7…，回车即载入）\n也可以 Ctrl+V 直接粘贴棋盘图片',
    visBoxHintCode: '局面代码（改完回车重新解析；Ctrl+V 可粘贴图片）',
    visUpload: '上传图片', visShot: '屏幕截图',
    
    visCrop: '裁剪', visCropTitle: '裁剪图片', visCropHint: '拖动鼠标框选要保留的区域',
    visCropReset: '重置', visCropCancel: '取消', visCropOk: '确定',
    visCropNone: '当前没有图片可裁剪 —— 先「上传图片」或「屏幕截图」',
    visCropSmall: '框选区域太小，请拖一个大一点的矩形',
    visCropDone: '已裁剪（{w} × {h}）', visCropFail: '裁剪失败，图片可能已损坏',
    visRec: '重新识别', visLoad: '加载到练习',
    visRc: '识别参数', visRcGeneral: '通用', visRcCancel: '取消', visRcOk: '确定',
    visRcOn: '识别参数：拖动滑动条立即重新识别（实时预览）并自动保存；「通用」恢复默认，「取消」回到打开时那组值',
    visRcOff: '识别参数已收起（这 5 项阈值仍按当前值生效）',
    visRcGeneralMsg: '已恢复通用识别参数（引擎默认阈值）',
    visRcOkMsg: '识别参数已确定',
    visRcCancelMsg: '已取消本次调整，识别参数回到打开时的值',
    visShotBusy: '请选择截图方式（屏幕上弹窗）…',
    visShotFail: '截屏失败：屏幕上没有可截取的内容',
    visWorking: '识别中…',
    visBad: '暂时没识别出棋盘和棋子：图片可能太模糊、棋盘占图太小；换一张更清晰、棋盘更大的图片试试',
    visNoStones: '识别到了棋盘，但没有读出棋子：图片可能模糊，或棋子与棋盘对比度太低',
    visRetry: '视觉引擎没有回话，正在自动重试…',
    visTimeout: '视觉引擎超时没有回话（已自动重试一次）：请点「重新识别」，或换一张更清晰的图片',
    visLowRes: '图片分辨率较低（{w}×{h}），识别可能不准确',
    visOk: '识别到 {b} 颗黑子、{w} 颗白子',
    visSuspect: '当前图片可能模糊，识别不准确，请人工核对；',
    visPartial: '当前棋盘可能不完整（残盘），已按棋子排布居中加载；',
    visNone: '先上传图片或截屏，再来识别',
    visLoaded: '已发送到练习窗口接着下',
    visName: '识图局面',
    visSrc: '识图',
    
    visPillTip: '点这里打开图片抽屉（选一张 / 移除）',
    visDelTitle: '移除这张',
    visDrawerTtl: '图片',
    visDrawerClose: '收起抽屉',
    visVdTip: '点缩略图选中 · ✕ 移除 · 点空白上传',
    
    visSel: '选择', visSelAll: '全选', visDelSel: '删除',
    visSelOn: '多选：开', visSelOff: '选择',
    visSelTip: '点选 = 勾选 · Ctrl+点 = 多选 · Shift+点 = 范围选 · Ctrl+A = 全选',
    visCopied: '已复制图片到剪贴板',
    visCopyFail: '复制失败：这张不是图片',
    
    visOpenApp: '用系统看图软件打开',
    visOpenTip: '双击图片 = 用系统默认看图软件打开这一张',
    visNoSel: '先勾选图片（「选择」或 Ctrl+点缩略图）',
    visDelDone: '已删除 {n} 张图片',
    visEdit: '修改',
    veSwap: '交换黑白子', veDel: '删除棋子', veAdd: '添加黑白子',
    veAddB: '黑子开始', veAddW: '白子开始',
    veUndo: '撤销', veUndoNone: '没有可撤销的操作（每步落定后才能撤）',   
    visCountBad: '★ 黑白数异常（黑 {b} / 白 {w}）—— 已禁用「保存到历史 / 加载到练习」，请用「修改」修正后再试',   
    visVc: 'VC 模式', visVcTip: '算杀题模式：黑白子数不统一也可保存/加载（VCF/VCT 题面常见黑远多于白）；加载到练习后只预设进攻方，算杀由你点「VCF / VCT」自己开',   
    visVcOn: 'VC 模式开：黑白子数不统一也可保存/加载；加载后请自己点「VCF」或「VCT」开始算杀（默认进攻方 = 子多一方）',
    visVcOff: 'VC 模式关：恢复黑白子数校验（黑=白 或 黑=白+1）',
    visVcPass: '★ VC 模式放行：黑 {b} / 白 {w}（子数不统一），可直接保存/加载',
    visVcArm: 'VC 模式已就绪：进攻方已预设为子数占优的一色 —— 点「VCF」或「VCT」开始算杀（不会自动开）',   
    veSwapHint: '交换模式：点一颗黑子，再点一颗白子 → 这两子互换颜色；可连续交换，再点本键或按 Esc 退出',
    veSwapNext: '已选中 1 颗 —— 再点一颗对面颜色的子即互换',
    veSwapDone: '已互换 · 黑 {b} / 白 {w} · 可继续点选交换',
    veDelHint: '删除棋子：黑比白多一时可单删黑子，白与黑齐平时可单删白子；其余情况点对面颜色成对删',
    
    veDelVcHint: 'VC 模式 · 删除棋子：点击任意棋子**立即删除**，不限颜色、不限数量（撤销键可回退）',
    veFillVcHint: 'VC 模式 · 补子：点空点即落该色棋子，**不限数量**（黑子白子想补多少补多少，撤销键可回退）',
    visVcEditTip: 'VC 模式已开：黑白子数**不必相等**也可保存/加载；修改模式里「补充黑子 / 补充白子」**不限数量**、想补多少补多少，「删除棋子」**点哪删哪**（不再要求黑白数配平）。摆好的残局可保存到历史，打开即整盘首帧。',
    veDelPair: '这颗不能单删 —— 再点一颗**对面颜色**的子成对删除（再点它取消）',
    veDelDone: '已删除 {x} 处（黑 {b} / 白 {w}）',
    veGhost1: '已放预览{c}子 —— 再点一个空点放对面颜色',
    veGhost2: '再点任意空点 = 落定这一对并开始下一对；点棋盘外空白 = 只落定',
    veGhostB: '黑', veGhostW: '白',
    veAddBHint: '黑子开始：点空点放半透明黑子 → 再点空点放白子 → 点第三处/空白落定',
    veAddWHint: '白子开始：点空点放半透明白子 → 再点空点放黑子 → 点第三处/空白落定',
    veEdited: '已修改（黑 {b} / 白 {w}）· 可重新识别或直接加载到练习',
    veEditOn: '修改模式：右键棋盘也能弹出功能菜单',
    veEditOff: '已退出修改',
    veRightMenu: '右键菜单：选中后点这里的功能项',
    
    veFillB: '补充黑子', veFillW: '补充白子',
    veFillBHint: '补充黑子：点击空交叉点，一点补一颗黑子（补到黑与白齐平或多一为止）',
    veFillWHint: '补充白子：点击空交叉点，一点补一颗白子（补到白与黑齐平为止）',
    veFillBFull: '黑子已不比白子少，不用再补黑子（可改用「删除棋子」或「补充白子」）',
    veFillWFull: '白子已与黑子齐平，不能再补白子（再补就比黑子多了）',
    
    veFillGuideB: '白子比黑子多 {d} 颗 → 请点「补充黑子」补齐（一点补一颗）',
    veFillGuideW: '黑子比白子多 {d} 颗 → 请点「补充白子」补齐（一点补一颗）',
    
    visCode: '棋盘代码',
    visCodeCopied: '棋盘代码已复制',
    
    nbest: '分析点数', nbestHint: '至多 8 点（与「对局设置 · 分析点数」同一设置）',
    anIdle: '选一个功能键开始计算，结果会标在棋盘上',
    anRunning: '计算中…', anStopped: '已停止计算', anEmpty: '棋盘还是空的：先落几手再算',
    anDraft: '演算中…第 {r} 轮草稿',
    anDone: '第 {n} 手 · {ev} · 深度 {d}',
    anBest: '最佳', anBalance: '平衡',
    anBalBusy: '官方平衡搜索中…',
    anBalDone: '已给出 {n} 个平衡点：{list}',
    anBalEval: '（引擎判定这一手最平：|评估| ≈ {v}，≤ 50 才是官方平衡窗口内）',
    anBalOpenHints: [
      '官方指定开局只有 3 手（黑1 天元 · 白2 在 3×3 内 · 黑3 在 5×5 内），开局评估 3 个点最合理。',
      '天元是固定的第 1 手，不必评估；真正要搜的只有白 2 与黑 3 这两手。',
      '评估 4 个以上就已经走出开局阶段，评估分会明显不可靠。',
      '判「平」的口径是 |评估| ≤ 50（官方平衡窗口）；越大越偏，接近 200 就明显一边倒了。',
      '平衡点与规则绑定：有禁手（连珠）和无禁手算出的点不一样，结果只对当前所选规则有效。',
      '按塔拉山口-10 走满 5 手开局，才可以评估到 5 个平衡点。',
    ],
    anBalNoEngine: '平衡子不可用（原生平衡引擎没应答）—— 已按「绝不用对弈引擎顶替平衡点」的口径空手返回，请稍后重试',
    anBalFail: '平衡搜索失败：',
    anBalnSet: '平衡点数已设为 {n}（空盘固定 3）',
    anBalnHint: '平衡点数（1~6，空盘固定 3）',
    anBalLabel: '点数',
    anBalColor: '（蓝色 = 白子，紫色 = 黑子）',
    
    balCall: '平衡打点',
    balCallT: '按官方平衡搜索（YXBALANCE）自动叫点',
    balCallBusy: '官方平衡搜索中…',
    balCallDone: '已叫 {n} 个官方平衡点：{list}',
    balCallFail: '平衡搜索失败：',
    balSwapNow: '引擎判定：此际应换手',
    anBal1Off: '官方平衡一手：{list}',
    anBal2Off: '官方平衡两手：{list}',
    ofRegion: '超出本手的允许区域',
    taraSwapped: '已换手：双方交换颜色',
    taraSwapAi: 'AI 行使换手：颜色已交换',
    tagTara: '塔拉换手评估',
    taraYes: '换手',
    taraNo: '继续',
    anAfter: '两手后',
    guideYama: '黑1 → 白2 → 黑3', guideYamaEn: 'B1 → W2 → B3',
    guideSwap: '等待换色决定', guideSwapEn: 'awaiting swap',
    swapTtl: '交换完成 · 请选择你的先后手',
    swapFirst: '先手 · 执黑', swapSecond: '后手 · 执白',
    ruleName: {
      0: '无禁手', 1: '无禁手 · 长连不赢', 2: '连珠（有禁手）',
    },
    ruleHint: {
      0: '无禁手：连成五子（含长连）即胜。最自由。',
      1: '标准：长连不算赢，必须正好五子。',
      2: '连珠：黑方有禁手（三三/四四/长连），白方无禁手。',
    },
    
    ruleKept: '规则已切换为「{r}」：保留当前棋盘布局，直接按新规则继续（终局判定与禁手标记已重算）',
    
    fbBlocked: '禁手：黑方不能下在这里（三三 / 四四 / 长连）—— 盘上画红叉的点就是禁手点',
  },
  en: {
    title: 'Meter Gomoku',
    setup: 'Game setup', mode: 'Mode', side: 'AI plays', turn: 'Think time',
    secGame: 'Game', secEngine: 'Engine', secView: 'Board display', secCode: 'Position',
    secFoldTip: 'Click to fold / unfold this block',
    rule: 'Rules', cores: 'Threads', hash: 'Hash',
    weights: 'Weights', weightsHint: 'NNUE is sharper; classical is faster (better in fast games)',
    wNnue: 'NNUE (accurate)', wClassic: 'Classical (fast)',
    variant: 'Engine variant', vAuto: 'Auto (recommended)',
    vHintAuto: 'Auto: picks the fastest by ISA + measured throughput (recommended)',
    vHintBench: 'Measured fastest here: {v}',
    vHint512vnni: 'AVX-512 VNNI: wide vectors + int8 dot product, usually fastest on modern CPUs',
    vHint512: 'AVX-512: no int8-VNNI path; on par with VNNI on Zen4',
    vHintVnni: 'AVX-VNNI: 256-bit int8 dot product (some Intel CPUs)',
    vHint2: 'AVX2: no 512-bit path, notably slower with these weights (~1/3 speed)',
    vHintSse: 'SSE: oldest fallback, emergencies only',
    sysBench: 'measured fastest',
    candRange: 'Candidate range', maxDepth: 'Max depth', maxNodes: 'Max nodes',
    cr2: '0 · Narrowest (5×5)', cr2l3: '1 · Narrow (5×5+3)', cr3: '2 · Neutral (7×7)',
    cr3l4: '3 · Recommended (7×7+4)', cr4: '4 · Wide (9×9)', crFull: '5 · Widest (full board)',
    candRangeHint: 'Wider never misses a point but searches shallower in the same time',
    multiPv: 'Analysis points', multiPvHint: 'Max points shown by Analyze (same setting as the panel)',
    multiPvUnit: ' pts',
    mdNone: 'No limit', mnNone: 'No limit',
    phNone: 'No limit',
    phDepth: 'Integer 4 ~ 128, empty = no limit',
    phNodes: '0.1 ~ 256 (in M), empty = no limit',
    coresHint: '{n} threads on this PC · max {m}',
    coach: 'Coach view', coachHint: 'Square 4-colour heat: AI computes your best spots; clears on move',
    heatBest: 'Best', heatGood: 'Good', heatFair: 'Fair', heatPoor: 'Poor',
    
    sideAiB: 'AI plays Black', sideAiW: 'AI plays White',
    noNativeEngine: 'This machine does not support the native AI engine (a CPU with AVX2 is required); AI is unavailable',
    sideHintB: 'AI plays Black and opens; you play White',
    sideHintW: 'AI plays White; you play Black and open',
    sideHintBoth: 'AI vs AI: the AI plays both colours, you just watch - the centre button is the switch (stop / resume)',
    sideHintBothPlace: 'AI vs AI is armed, but the board-setup mode never moves on its own - switch to "Match" and it starts',
    spPause: 'Stop (pause AI self-play)',
    spResume: 'Resume AI self-play',
    sideHintNone: 'Both off: no AI — you play both colours yourself',
    turnAiBoth: 'AI vs AI · {c} to move',
    turnYouBoth: 'Your turn · {c} (both sides)',
    showNum: 'Move numbers', showNumHint: 'Mark each stone with its move number',
    bgThink: 'Background thinking', bgThinkHint: 'After the AI moves, the engine keeps deepening this position while you think (your threats + its replies), so your move hits a hot transposition table',
    bgThinkOffAi: 'auto-off while AI plays both sides', bgThinkOffBusy: 'paused while a calculation runs',
    
    rvNum: 'Numbers', rvNumHint: 'Mark each stone with its move number',
    rvKeyHint: 'Ctrl+Z previous · Ctrl+R restart',
    preview: 'Hover box', previewHint: 'Rounded box shown when hovering the board',
    fwd: 'Look-ahead',
    fwdIdle: 'Press "Look-ahead" to auto-play from this position to five-in-a-row',
    fwdRunning: 'Deriving… {k} moves',
    
    fwdEta: 'Computing - first move within {d}s (deeper global search), whole run {a}s ~ {b}s',
    anEta: 'Computing… expect within {s}s',
    fwdDone: 'Simulated to five-in-a-row ({n} moves) - engine self-play line, not a forced mate: hover a code to preview, click to commit',
    fwdCap: 'Stopped at {n} moves: the engine gave no legal point here (engine self-play line, not a forced mate; re-derives when the position changes)',
    fwdFull: 'Board is full after {n} moves without a five - a draw (engine self-play line, not a forced mate)',
    fwdVcf: 'VCF solved: {n} forcing fours win (every defence is forced) - hover a code to preview, click to commit',
    fwdVct: 'VCT solved: {n} continuous threats (fours / open threes) win - hover a code to preview, click to commit',
    
    fwdVcfHit: 'VCF win found: {n} moves; attacker: {c} (odd numbers = attacker, even = defender)',
    fwdVctHit: 'VCT win found: {n} moves; attacker: {c} (odd numbers = attacker, even = defender)',
    fwdVcfNone: 'No VCF (forcing fours) win exists for {c} (the chosen attacker) in this position',
    fwdVctNone: 'No VCT (continuous threats) win exists for {c} (the chosen attacker) in this position',
    fwdClear: 'Clear calc',
    markCleared: 'Cleared every analysis mark on the board (Calculating / Multi-point / Balance picks / Scan def / Lookahead \u00b7 VCF \u00b7 VCT)',
    vcxPopTitle: 'Solver result',
    fwdAtkAuto: 'Attacker: auto - the side to move (mate puzzles: the majority side, as usual). The black/white frames are indicators only, not clickable',
    fwdAtkB: 'Attacker: Black (auto)',
    fwdAtkW: 'Attacker: White (auto)',
    fwdFindVcf: 'VCF', fwdFindVct: 'VCT',
    fwdFindRun: 'Searching {k}… (multi-worker + iterative deepening)',
    
    fwdProbing: 'Analysing the breakthrough: {r} ... (the first pass may take a moment, then it walks on; it switches to the kill sequence as soon as one exists)',
    fwdVcxLater: 'After {k} moves the attacker has a {r} win ({n} moves) - merged into the line, running on to five in a row',
    fwdFiveNoVcx: 'Reached five in a row in {n} moves - no VCF / VCT attacking chance for {c} ever appeared (engine line throughout)',
    fwdBlack: 'Black', fwdWhite: 'White',
    fwdOk: 'Commit',
    fwdLegend: 'Attacker indicators (not clickable): purple = black, light blue = white; auto = side to move (majority side in mate puzzles)',
    ten: 'Ten-call (10 candidates)',
    docTitle: 'Meter Gomoku',
    dragCard: 'Drag the title bar to move it; click the bar to fold / unfold the card',
    codeTip: 'Position code: lowercase column letter + row, moves concatenated (same format as gomocalc.com)',
    nMoves: '{n} moves',
    about: 'About', aboutClose: 'Close',
    aboutTip: 'Version, how to use, features and open-source licences',
    aboutGuide: 'How to Use',
    aboutGuideList: [
      { t: '1. Start a game', ps: [
        'Pick a mode in Game Setup: AI (the engine replies) / Free setup (you place stones, no AI at all) / Endgame (set up a position and study it).',
        'Thinking time = how many seconds the AI gets per move (sent to the engine in milliseconds). Longer time means a stronger move; the heat and coach views also widen with it.',
        'Two independent switches choose the AI sides: only "AI plays Black" -> you play White; only "AI plays White" -> you play Black and open the game; both on -> AI vs AI; both off -> no AI at all, you play both colours yourself.',
        'Click the board to place a stone. The centre button: when idle it asks the AI to play one move for whoever is on turn; while the AI is thinking it pauses.',
        '"Restart" auto-saves the current game into History first, then clears the board; "Save" stores the position into History by hand.',
      ]},
      { t: '2. Endgame mode (set up a position and study it)', ps: [
        'Three keys appear under the board: Confirm / Sequential / Free colour. Sequential = black and white alternate; Free colour = open the rounded box, pick Black or White and keep placing that colour - any count, any order (e.g. 30 black + 10 white).',
        'The picked colour key lights up: Black turns light purple, White turns light blue-white.',
        'Press Confirm (leftmost) to lock the position in: the key turns light green and reads "Confirmed"; from then on Restart returns to this position, and it is the first frame when saved to History. Press it again to release and re-place.',
        'The AI never joins endgame mode (assist, heat and coach views are all off), and swap-style rules are greyed out - a set-up position needs no swap.',
      ]},
      { t: '3. Evaluation', ps: [
        '"Compute" deep-searches the position and marks the best point (press again to stop); "Multi-point" returns 2-8 candidates with rank and score in each badge.',
        '"Balanced 1" picks the candidate whose evaluation is closest to zero; "Balanced 2" plays each candidate out and reads the reply before choosing; "Defense scan" runs the engine-native YXSEARCHDEFEND (the same algorithm as the Rapfi bundle): a pass is inserted so the opponent is to move, then every candidate point is searched continuously - each point\'s % is the opponent\'s win rate there, so higher = more urgent. Points are drawn in five tiers: tier 1 is the largest ring with a translucent fill, tiers 2-4 shrink and fade (the smallest ring is half the largest), and tier 5 (edges / low risk) shows just a number inside a thinner ring of the same size as tier 4, sweeping the board; the whole palette is sky blue (deeper = more urgent, paler = low risk). The search keeps deepening; "Stop scan" keeps the last frame on the board until you play or press "Clear calc".',
        '"AI view" shows the AI candidates (circles with scores) only while it thinks, and clears the moment it moves; "Coach view" hints your best move in four colours and disappears as soon as you play.',
        '"Clear calc" sits next to "Compute": one press wipes whatever any card item left on the board - badges and scores from Compute / Multi-point / Balanced 1 / Balanced 2, the five-tier rings and percentages of Defense scan, and the numbers, dashed lines and code chain of Look-ahead / VCF / VCT. It clears marks only, never the position.',
        'The dashboard shows depth, speed, nodes, time and evaluation; the curve below tracks the whole game for both colours (the axis rescales itself).',
      ]},
      { t: '4. Look-ahead / VCF / VCT', ps: [
        '"Look-ahead" derives move by move and only ends when a real five-in-a-row appears - it never stops early on the engine verdict (worst case: the board fills up as a draw). The first move gets a deeper global search to find the breakthrough, and the hint shows an expected duration range.',
        '"VCF" = continuous-four mate search; "VCT" = four + open-three threat search. Both run the local solver (multi-worker, iterative deepening): a hit gives a forced win line, a miss says plainly that no such forced mate exists. With VCF/VCT selected it re-probes at every step and switches into the kill sequence as soon as one exists.',
        'Click the Black / White badge to force the attacker (your colour for attack training, the opponent for defence drills); click again to cancel.',
        'Preview stones are translucent and numbered; hover the code chain to preview up to that move, click a code to select it, then OK commits that stretch onto the board.',
        'Every derived move passes a must-answer check: complete a five if one exists, block the opponent five, and answer an open three when you have no four of your own - the defence never hands you a free win.',
      ]},
      { t: '5. Review & history', ps: [
        'The History drawer lists saved positions: click to select (arrow keys move, Enter opens), right-click for open-review / rename / export / store into Saved / delete; Ctrl/Shift multi-select and txt import/export are supported.',
        'The "Review" button opens a separate board-only window (no AI, no other main-window features). A game opened from History can be walked through with "Recite" / "Replay". VC and endgame records open with the whole board as the first frame instead of replaying from move one.',
      ]},
      { t: '6. Vision (Gomoku Vision)', ps: [
        'Upload or screenshot - the frame snaps to the board automatically, leaving a ~1/30 board margin so edge stones stay inside - then Recognize; the result is mirrored into a board code you can copy or load into practice.',
        '"Edit" fine-tunes by hand: swap B/W (click one black + one white), remove stones, fill black/white; Ctrl+Z undoes.',
        '"VC Mode" = mate-puzzle mode: unbalanced black/white counts are allowed, filling is unlimited and removal is free, with every rule spelled out in the left hint bar. Loading into practice switches to Endgame and only presets the attacker (the majority side) - the mate search never starts by itself, you press VCF or VCT.',
        '"Crop" lets you frame the picture before recognition; every upload is auto-saved into the picture drawer (oldest dropped past 150), with multi-select, delete and copy.',
      ]},
      { t: '7. Rules & appearance', ps: [
        'Rules: free-style (no forbidden), overline does not win, Renju (double-three / double-four / overline forbidden), Swap-1, Yamaguchi and Taraguchi-10; look-ahead and mates follow the chosen rule. Swap-style rules open a small popup to pick first or second.',
        '"EN / 中" switches the whole UI (including the screenshot-box buttons and the date format in History); "Colour" opens the palette (board / page plus themes and presets); right-side cards can be dragged to reorder and folded with the dash, and the layout is remembered.',
      ]},
      { t: '8. Link mode (play in another app)', ps: [
        'Open and select: press "Link" on the top bar to open the Link window, click "Select board" and drag a rectangle over the target board, then press "Recognize" on its title bar. The box turning teal with L-shaped corner brackets and anchors means success. Recognition runs silently in the background (inter-frame pixel diff: no work at all while the board is still, re-read only when something changes), so the frame colour never flickers.',
        'Who does what: the Link window owns recognition and the AI; the main board only mirrors the recognised position and the AI-side settings - it never evaluates and never drops a stone, so the two windows can never fight over the same search.',
        'AI side and thinking time: "AI plays Black" / "AI plays White" are mutually exclusive, and the choice is mirrored unconditionally into Game Setup in the main window; Thinking time is mirrored the same way.',
        'Auto drop: with it on, the app thinks as soon as the opponent moves and plays for you through a three-stage simulated click (PostMessage straight to the target window, then SendInput, then a real click as the last resort). Your physical mouse is never touched. It switches off when the game ends (five in a row).',
        'Swap balance: pressing it automatically cancels any selected AI side first (they would interfere). On an empty board it gives three points (two Black, one White); with stones present it gives two (one Black, one White). The markers stay in the recognition frame and the balance engine is released immediately afterwards. They disappear once you have played those stones.',
        'Swap balance + Auto drop in order: if you press Swap balance first and Auto drop is on when the evaluation finishes, the points are played automatically in order (exactly like the standalone assistant). If Auto drop is switched on later, they are played in order the moment it is switched on.',
        'Swap evaluation: a separate feature from Swap balance; it scores the current swap position so you can judge whether to swap.',
        'On-board marks: the suggested move is a sky-blue ring; swap balance points are purple (Black) / blue (White) rings numbered 1, 2, 3. All of them are drawn inside the recognition frame at the matching intersections and are excluded from screen capture, so they are never misread as stones.',
        'Closing: closing the Link window restores your own mode and settings in the main window; play, computation and everything else keep working as before.',
      ]},
    ],
    updLine: 'Latest version: ', updPwd: 'password',
    checkUpdate: 'Get latest version',
    updateOpened: 'Download page opened, password {p} copied',
    aboutVer: 'Version', aboutFeat: 'Features', aboutOss: 'Open-source components', aboutLic: 'Licence',
    aboutUpdates: 'Recent updates',
    aboutUpdateList: [
      'Defense scan rewritten: it now uses the engine-native YXSEARCHDEFEND that ships with the Rapfi bundle (one PASS hands the move to the opponent, so each % is the opponent\'s win rate if they play there); danger is shown in five tiers - tier 1 is the full-size ring with a translucent fill, tiers 2-4 shrink and fade (the smallest ring is half of tier 1), tier 5 edge points show a number inside a thinner ring of the same size as tier 4, sweeping the board; the palette is now a pale sky-blue ramp, the largest ring is shrunk to stone size and all rings are thinner; the key turns into "Stop scan" and the last frame stays on the board after stopping - only a new move or "Clear calc" wipes it; "Clear marks" is renamed "Clear calc"; the multi-point list, look-ahead panels and hint lines now share the same dashed box style',
      'Vision adapts far better to real pictures: dark and light board themes, rotated screenshots, photos of a screen, low-contrast and blurry images all read now - a third "local contrast" reader judges each intersection against its own background (dark boards no longer lose black stones, light boards no longer read empty cells as white stones), board search climbs a ladder (3x3 blur, CLAHE equalisation, 1.5x upscale), and a geometry check falls back to the legacy recogniser when the grid does not line up',
      'Vision "Recognition params": opening the panel now hides the position-code box automatically, and the hint line is a fixed-height dashed box - dragging the sliders no longer makes the column jump',
      'UI assets are now one ui.pak instead of many .enc files - antivirus products no longer mistake the encrypted documents for ransomware and delete them (which used to leave a blank window); if the file really is missing, a diagnostic page explains how to whitelist and reinstall',
      'Engine calls go back to the 4.36 baseline (official Rapfi single-shot Analyze): the score no longer flips sign, and the thinking budget and pondering rhythm follow the 4.36 behaviour that measured best',
      'Strength restored: the move-decision chain drops the shallow-simulation veto and the history-mode override - the engine best move is now played directly, keeping only the must-answer guard (win / block), so the native engine plays at full power again',
      'Live analysis rebuilt (gomocalc style): continuous full-lane deep search with no time window; the best point varies naturally with search depth and converges; elapsed time accumulates across rounds and the session restarts on every move; works in setup / endgame / two-player modes too',
      'Setup scores retired: the settings switch is removed - board evaluation is now handled entirely by the Live analysis button (full-lane deep search instead of a 1-thread shallow probe)',
      'Live engine dashboard: every calculation (AI move / Evaluate / Multi-point / Balanced / Defense scan / Heat map / Look-ahead / Preheat) now streams speed and node counts WHILE it runs - parallel lanes are summed so the total throughput of the vote is visible; a status pill on the card header pulses red while computing (naming the active item) and shows a green dot when idle',
      'Candidate colours now follow KataGo: 1st choice blue, 2nd green, 3rd yellow (4th orange, 5th+ red) - no more clashing with the defence scan palette',
      'Stone numbers scale with the board: move numbers, eval digits and all badge fonts no longer cap at 14px, so they stay readable on high-resolution maximised windows',
      'Compute budgets now match the thinking time: Evaluate drops its 12s cap (1.6x thinking time), the look-ahead opening search drops its 12s cap (4x thinking time); every look-ahead step already follows the setting',
      'Background warm-up (on by default): while you think, the AI keeps rehearsing your likely replies in the background - a hit answers instantly, and even a miss leaves the hash table warm so the same time budget searches deeper',
      'Parallel engines voting has been removed (all compute on one instance = peak strength): the engine runs as a single instance using every thread chosen in Core count; the decision layer keeps only the must-answer guard (win / block-five / open-four) and the engine top choice',
      'Defense scan colours upgraded: badges and the list now use a continuous cyan -> green -> yellow -> orange -> red gradient (the higher the %, the redder)',
      'Vision hardened: dense positions rebuild the whole grid by extrapolating the locked lines (no more no-grid); the capture window expands outward by ~1/30 of the board so edge stones are read fully',
      '"Clear marks" moved and generalised: it now sits next to "Compute" inside the Evaluation card, half width each, and one press wipes the board marks left by ANY card item (Compute / Multi-point / Balanced 1 / Balanced 2 / Defense scan / Look-ahead, VCF, VCT)',
      'New Endgame mode: sequential / free-colour placement (any count, any order) plus a Confirm key that locks the position in - Restart returns to it and it becomes the first frame in History',
      'Vision "VC Mode" relaxed: unlimited stone filling, free removal, with every rule spelled out in the left hint bar',
      'VCF / VCT markers simplified to a blue dashed kill line plus a deep-blue double ring (five) and a sky-blue square (four)',
      'Look-ahead first move now uses a deeper global search to find the breakthrough (budget lowered from x6 to x4), and the hint shows an expected duration range',
      'Play corrected: the side to move is now passed to the engine explicitly (unbalanced stone counts no longer make it play for the wrong side), plus a must-answer check - win first, block a five, answer an open three',
      'VC / endgame records open with the whole board as the first frame; swap-style rules are greyed out in endgame mode',
      'Mode buttons shortened to two characters; English mode fixed for "Clear marks" and other labels, History dates follow the language',
      'Auto-snap margin corrected: it now expands outward by ~1/30 of the board instead of inward, so edge stones stay inside the frame',
    ],
    aboutApp: 'Meter Gomoku',
    aboutFeatList: [
      'Three modes: play the AI / free placement / endgame (set up a position and Confirm it; Restart returns to it)',
      'Look-ahead that runs to a real five-in-a-row; local VCF / VCT mate solver (multi-worker, iterative deepening)',
      'Evaluation: Compute / Clear calc / Multi-point / Balanced 1 / Balanced 2 / Defense scan, plus AI view and Coach view',
      'Eval curve with a self-rescaling axis up to +/-1200; dashboard with depth / speed / nodes / time',
      'Three rules: freestyle / overline-loses / Renju (forbidden moves)',
      'Vision: screenshot or upload with auto board snapping, VC mate-puzzle mode, manual editing (swap / remove / fill / crop) and a picture drawer',
      'History & review: txt import/export, rename, right-click menu, separate review window (recite / replay, VC records open on the full board)',
    ],
    aboutOssList: [
      ['Rapfi', 'Gomoku / Renju engine core (compiled to WebAssembly, distributed with this app)', 'GPL-3.0'],
      ['OpenCV', 'Board and stone recognition (statically linked into GomokuVision.exe, no extra DLL)', 'Apache-2.0'],
      ['Microsoft WebView2 Runtime', 'Desktop UI runtime (provided by Windows / Edge, not distributed with this app)', 'Proprietary'],
    ],
    aboutOssNote: 'Note: the table lists only third-party components that ship with this app or are required at runtime. Development-only scripts and their dependencies (NumPy, Pillow, ...) run on the build machine and are not distributed with this app, so they are not listed here.',
    aboutLicText: 'The interface and companion code of this app are released under the MIT licence. The playing core Rapfi is released under GPL-3.0; this app calls it as compiled WebAssembly (an in-page worker) or as a separate process, communicating only through messages and a local port, and is not merged with it into a single work. When redistributing Rapfi together with this app you must keep its licence and the way to obtain its source (available from its upstream repository, dhbloo/rapfi). OpenCV, used for recognition, is statically linked into GomokuVision.exe under Apache-2.0. The Microsoft WebView2 Runtime is provided by Windows / Edge under its own terms and is not distributed with this app. Copyright and licensing of each component above remain with its respective authors.',
    engine: 'Engine', depth: 'Depth', speed: 'Speed', nodes: 'Nodes', time: 'Time',
    calcOn: 'Thinking', calcIdle: 'Idle',   
    tagAiMove: 'AI move', tagAssist: 'Hint', tagHeat: 'Heat map', tagCoach: 'Coach',
tagDefend: 'Defence scan', tagCalc: 'Evaluate', tagNbest: 'N-best',
    tagLiveAna: 'Live analysis', tagBg: 'Background thinking', liveAna: 'Analyze', liveAnaStop: 'Stop analysis',
    liveAnaBan: 'AI self-play: both sides are played by AI - no user side to analyze',
    liveAnaRun: 'Live analysis · deep search · depth {d}',
    anCalcRun: 'Eval · continuous deep search · depth {d}',
    anNbestRun: 'Multi-PV · continuous deep search · depth {d}',
    liveAnaStopped: 'Stopped · depth {d} · markers and eval kept from the stop moment',
    btnAiside: 'AI side', btnSet: 'Settings', pinFix: 'Pin',
    pinFixTtl: 'Pin this card: it stays when other cards are opened from Settings',
    pinCloseTtl: 'Unpin and close this card',
    aiSideB: 'AI Black', aiSideW: 'AI White', aiTurnLbl: 'Think time',
    tagBal1: 'Balance 1', tagBal2: 'Balance 2', tagFwd: 'Look-ahead', tagTen: 'Ten-call',
    eval: 'Eval', best: 'Best', curve: 'Eval curve', blackCurve: 'Black', whiteCurve: 'White',
    score: 'Score', winRate: 'Win rate',
    history: 'History', saved: 'Saved', openDrawer: 'History',
    saveSel: 'Save selected', delSel: 'Delete', openSel: 'Open', closeDr: 'Close',
    selAll: 'Select all', selNone: 'Clear',
    drTip: 'Tick some games, then "Save selected" -> they live forever under "Saved"',
    drExp: 'Export', drImp: 'Import',
    ctxOpen: 'Open in review', ctxRename: 'Rename', ctxExport: 'Export this game',
    ctxToSaved: 'Move to "Saved"', ctxDel: 'Delete',
    namePh: 'Name this game',
    expNone: 'Nothing to export',
    expOne: 'Exported 1 game as txt (choose the path in "Save as")',
    expDone: 'Exported {n} games as txt (choose the path in "Save as")',
    impDone: 'Imported {n} games into history',
    impNone: 'No readable position code in that text (one game per line, like h8h9g7…)',
    impFail: 'Cannot read that txt',
    renamed: 'Renamed',
    code: 'Position code', copy: 'Copy', paste: 'Paste', load: 'Load',
    copied: 'Position code copied', codeLoaded: 'Position code loaded', clipEmpty: 'Clipboard is empty',
    codeBad: 'Cannot read the position code: it looks like h8h9g7 (lowercase column letter + row, moves concatenated)',
    reset: 'Restart', redo: 'Redo',
    play: 'Play', pause: 'Pause',
    recite: 'Recite', replay: 'Replay', exit: 'Exit review',
    review: 'Review', prev: 'Back', next: 'Forward', savePos: 'Save position',
    rotTtl: 'Rotate stones 90° clockwise (axes stay put)', mirrorTtl: 'Flip stone layout',
    shiftTtl: 'Shift position', shiftBtnTtl: 'Shift position (move all stones by one cell)',
    mvFv: 'L·R', mvFh: 'U·D', mvD1: '╲', mvD2: '╱',
    mvFvT: 'Flip left-right', mvFhT: 'Flip up-down',
    mvD1T: 'Diagonal flip (top-left↔bottom-right)', mvD2T: 'Diagonal flip (top-right↔bottom-left)',
    mvUpT: 'Shift all stones up by one cell', mvDownT: 'Shift all stones down by one cell',
    mvLeftT: 'Shift all stones left by one cell', mvRightT: 'Shift all stones right by one cell',
    closeTtl: 'Close',
    layoutRV: 'Cannot change layout in review mode', layoutBusy: 'AI is thinking — try again shortly',
    shiftOOB: 'A stone would leave the board — cannot shift that way',
    posSaved: 'Position saved as PNG', posSaveFail: 'Save failed',
    pause: 'Pause', resume: 'Resume', assistHint: 'Click: AI computes and plays one move for you',
    reciteHint: 'Recite mode: place the next move from memory; mistakes get a pink ring.',
    miss: 'Missed', turnBlack: 'Black to move', turnWhite: 'White to move',
    turnYou: 'Your turn', turnAi: 'AI to move',
    over: 'Game over', thinking: 'AI thinking…', paused: 'Paused', youWin: 'You win', youLose: 'AI wins',
    analyzing: 'Analyze mode: no auto-move, candidates + heatmap only',
    modePlace: 'Free placement',
    
    modeEndgame: 'Endgame',
    egSeq: 'Sequential', egFree: 'Free color', egB: 'Black', egW: 'White',
    egFreeOn: 'Free {c}',
    egHint: 'Endgame mode: no AI involved - the board is exactly what you place. "Sequential" = black and white alternate; "Free color" = pick black or white in the small rounded box and keep placing that colour (any count, any order - e.g. 30 black + 10 white). The placed position is the first frame; save it to History or start studying right away.',
    
    egOk: 'Confirm', egOkOn: 'Confirmed',
    egLockedHint: 'Endgame confirmed and switched to Free setup: "Restart" returns to this position, it is the first frame in History (marked Endgame), and Recite starts from it. Go back to Endgame mode to re-place.',
    egDash: 'Endgame: place the position yourself (no AI)',
    egDashLocked: 'Endgame confirmed · Restart returns to it',
    offline: 'AI engine not ready yet (first start loads the model). Retrying automatically.',
    offlinePill: 'Engine offline · retrying…',
    rvSpeedTtl: 'Step interval 0.5–5 s',
    closeCard: 'Close card',
    rvFree: 'Review · free placement (no AI)',
    rvWindow: 'Review · board only',
    rvPure: 'Review · place stones yourself (no AI, no forbidden-move rules)',
    rvFromHist: 'Review · game opened from history',
    rvNothing: 'Board is empty: place a few stones first, or pick a game in History',
    srcLocal: 'Local', srcDesktop: 'Desktop', srcBookmark: 'Bookmark',
    theme: 'Dark', theme2: 'Light', themeCustom: 'Custom',
    color: 'Colours', cpClose: 'Close', cpReset: 'Reset',
    cpTheme: 'Theme', cpTarget: 'Apply to', cpBoard: 'Board', cpPage: 'Background',
    cpHex: 'RGB code', cpPreset: 'Presets',
    cpHintB: 'Tuning the board colour; grid, star points and axes follow automatically',
    cpHintP: 'Tuning the page background; cards and text follow the brightness',
    cpBad: 'Cannot read that colour: use #rrggbb or rgb(r,g,b)',
    cards: 'Cards', cardsHint: 'Pick the panels shown right of the board',
    cardSideL: 'L', cardSideR: 'R',
    analysis: 'Analysis',
    anCalc: 'Compute', anStop: 'Stop', anDefend: 'Defend scan', anStopScan: 'Stop scan',
    anNbest: 'Multi-point', anBal: 'Balance picks', anBaln: 'Points',
    anDefendDone: 'Scanned {c} points: higher % = more urgent (W = to win, L = to lose)',
    anDefendRefine: 'Deepening… round {r} · {t}s each',
    anDefendStable: 'Converged: result stable',
    anScanRun: 'Defend scan · all-point engine search… depth {d} · {c} points (% = urgency)',
    anScanFrozen: 'Scan stopped: last frame kept ({c} points) — cleared by a move / Clear calc',
    
    shell: 'Search',
    shell: 'Search',
    sysCpu: 'CPU', sysCore: 'Cores', sysIsa: 'ISA', sysMem: 'Memory', sysUse: 'used',
    sysOs: 'OS', sysDisk: 'disk free', sysAvail: 'free',
    sysNative: 'Native', sysWasm: 'Web', sysUnknown: 'unknown',
    hmExcl: 'Active', hmExclVal: 'the whole {m}MB goes to the running event (move / analysis)',
    hmFloor: 'Idle', hmFloorVal: 'other lanes keep 64MB each',
    hmUse: 'Usage', hmSum: 'Total', hmPct: 'of RAM',
    abSys: 'System', abSysNone: 'No host system info yet',
    abSysCpu: 'CPU', abSysIsa: 'ISA', abSysMem: 'Memory',
    abSysCores: '{p}C {l}T', abSysCache: 'L1 {a} · L2 {b} · L3 {c}',
    abSysUsed: '{u}% used · {a} free', abSysEngine: 'Engine build',
    fwdAiOff: 'Look-ahead took over the engine: AI auto-play paused (restored on close)',
    vis: 'Vision',
    visTitle: 'Vision · turn board pictures into positions',
    visTtl: 'Turn board pictures into positions',
    visHint: 'Multi-select supported (up to 150); paste with Ctrl+V in the preview frame; a small picker pops up for screenshots',
    visBoxHint: 'Type or paste a position code here (e.g. h8h9g7…, Enter to load)\nor paste a board image with Ctrl+V',
    visBoxHintCode: 'Position code (edit + Enter to re-parse; Ctrl+V for an image)',
    visCodeBad: 'Code not recognized: expected "letter+number" pairs like h8h9g7i6',
    visDelTitle: 'Remove current',
    visUpload: 'Upload images', visShot: 'Screenshot',
    
    visCrop: 'Crop', visCropTitle: 'Crop image', visCropHint: 'Drag to select the region to keep',
    visCropReset: 'Reset', visCropCancel: 'Cancel', visCropOk: 'OK',
    visCropNone: 'No image to crop - upload one or take a screenshot first',
    visCropSmall: 'Selection too small - drag a larger rectangle',
    visCropDone: 'Cropped ({w} x {h})', visCropFail: 'Crop failed - the image may be broken',
    visRec: 'Re-recognize', visLoad: 'Load to practice',
    visRc: 'Recog params', visRcGeneral: 'General', visRcCancel: 'Cancel', visRcOk: 'OK',
    visRcOn: 'Recognition params: dragging a slider re-recognizes instantly (live preview) and saves automatically; "General" restores defaults, "Cancel" reverts to the values from when the panel was opened',
    visRcOff: 'Recognition params collapsed (the 5 thresholds still apply at their current values)',
    visRcGeneralMsg: 'General recognition params restored (engine defaults)',
    visRcOkMsg: 'Recognition params confirmed',
    visRcCancelMsg: 'Changes cancelled - recognition params reverted to the values from when the panel was opened',
    visShotBusy: 'Pick a capture mode (popup on screen)…',
    visShotFail: 'Capture failed: nothing to grab on screen',
    visWorking: 'Recognizing…',
    visBad: 'No board or stones found: the picture may be blurry or the board too small; try a sharper one where the board fills more of the frame',
    visNoStones: 'Board found but no stones read: the picture may be blurry, or stones lack contrast against the board',
    visRetry: 'Vision engine did not answer — retrying automatically…',
    visTimeout: 'Vision engine timed out (retried once): click 重新识别 / Recognize, or try a sharper picture',
    visLowRes: 'Low-resolution picture ({w}×{h}); recognition may be inaccurate',
    visOk: 'Recognized {b} black and {w} white stones',
    visCountBad: 'Imbalanced stones (black {b} / white {w}): Save / Load disabled - use Edit to make black=white or black=white+1',   
    visVc: 'VC Mode', visVcTip: 'Mate-puzzle mode: unbalanced stone counts may be saved/loaded (VCF/VCT diagrams often have far more black than white); loading only presets the attacker - you start the mate search yourself with VCF / VCT',   
    visVcOn: 'VC Mode ON: unbalanced counts may be saved/loaded; after loading, press VCF or VCT yourself to start the mate search (attacker defaults to the majority side)',
    visVcOff: 'VC Mode OFF: stone-count check restored (black=white or black=white+1)',
    visVcPass: 'VC Mode allows: black {b} / white {w} (unbalanced) - save/load enabled',
    visVcArm: 'VC Mode ready: attacker preset to the majority colour - press VCF or VCT to start the mate search (nothing runs automatically)',   
    visSuspect: 'The picture may be blurry — result may be inaccurate, please double-check; ',
    visPartial: 'The board may be incomplete (partial view), stones loaded centered; ',
    visNone: 'Upload a picture or take a screenshot first',
    visLoaded: 'Sent to the practice window',
    visName: 'Vision capture',
    visSrc: 'Vision',
    
    visPillTip: 'Open the picture drawer (pick / remove)',
    visDrawerTtl: 'Pictures',
    visDrawerClose: 'Close drawer',
    visVdTip: 'Click a thumbnail to select · ✕ removes · click blank to upload',
    
    visSel: 'Select', visSelAll: 'All', visDelSel: 'Delete',
    visSelOn: 'Multi: on', visSelOff: 'Select',
    visSelTip: 'Click = check · Ctrl+click = multi · Shift+click = range · Ctrl+A = all',
    visCopy: 'Copy picture',
    visCopied: 'Picture copied to clipboard',
    visCopyFail: 'Copy failed: not a picture',
    
    visOpenApp: 'Open in the system image viewer',
    visOpenTip: 'Double-click a picture to open it in the system default viewer',
    visNoSel: 'Check some pictures first ("Select" or Ctrl+click)',
    visDelDone: 'Deleted {n} picture(s)',
    visEdit: 'Edit',
    veSwap: 'Swap B/W', veDel: 'Remove stones', veAdd: 'Add stones',
    veAddB: 'Black first', veAddW: 'White first',
    veSwapHint: 'Swap mode: click a black stone, then a white stone - the two swap colors; repeat as you like; click this button again or press Esc to exit',
    veSwapNext: '1 selected - click a stone of the other color to swap',
    veSwapDone: 'Swapped - black {b} / white {w} - keep clicking to swap more',
    veUndo: 'Undo', veUndoNone: 'Nothing to undo',
    veDelHint: 'Remove stones: single black allowed when black = white+1, single white allowed when white = black; otherwise click the opposite color to remove a pair',
    
    veDelVcHint: 'VC mode · Remove: click ANY stone to delete it at once - any colour, any count (Undo to revert)',
    veFillVcHint: 'VC mode · Add: click an empty point to drop that colour - as many as you like (Undo to revert)',
    visVcEditTip: 'VC mode is on: black/white counts need NOT match to save/load; in Edit mode "Add black / Add white" are UNLIMITED and "Remove stones" deletes ANY stone (no balancing required). Save the endgame to History - it opens as a full-board first frame.',
    veDelPair: 'This stone cannot go alone - click a stone of the OPPOSITE color to remove the pair (click it again to cancel)',
    veDelDone: 'Removed {x} spot(s) (black {b} / white {w})',
    veGhost1: 'Ghost {c} placed - click an empty point for the opposite color',
    veGhost2: 'Click any empty point to commit this pair and start the next; click outside the grid to commit only',
    veGhostB: 'black', veGhostW: 'white',
    veAddBHint: 'Black first: click an empty point for a ghost black - another for white - a third point / blank commits',
    veAddWHint: 'White first: click an empty point for a ghost white - another for black - a third point / blank commits',
    veFillB: 'Add black', veFillW: 'Add white',
    veFillBHint: 'Add black: click empty intersections to add black stones one by one (until black = white or white + 1)',
    veFillWHint: 'Add white: click empty intersections to add white stones one by one (until white = black)',
    veFillBFull: 'Black already outnumbers white - no more black needed (try Remove stones or Add white)',
    veFillWFull: 'White is level with black - cannot add more white',
    veFillGuideB: 'White has {d} more than black - click "Add black" to fill in (one per click)',
    veFillGuideW: 'Black has {d} more than white - click "Add white" to fill in (one per click)',
    visCode: 'Board code',
    visCodeCopied: 'Board code copied',
    veEdited: 'Edited (black {b} / white {w}) · re-recognize or load to practice',
    veEditOn: 'Edit mode: right-click the board for the same menu',
    veEditOff: 'Edit mode off',
    veRightMenu: 'Right-click menu: pick an action',
    nbest: 'Points', nbestHint: 'Up to 8 (same setting as Analysis points in Game settings)',
    anIdle: 'Pick a button to compute; results are drawn on the board',
    anRunning: 'Computing…', anStopped: 'Stopped', anEmpty: 'Board is empty: place a few stones first',
    anDraft: 'Drafting… round {r}',
    anDone: 'Move {n} · {ev} · depth {d}',
    anBest: 'Best', anBalance: 'Balance',
    anBalBusy: 'Official balance search…',
    anBalDone: '{n} balanced points: {list}',
    anBalEval: ' (engine verdict for this pick: |eval| ≈ {v}; ≤ 50 is inside the official balance window)',
    anBalOpenHints: [
      'The official opening is only 3 moves (B1 tengen · W2 inside 3x3 · B3 inside 5x5); 3 points is the sensible number to evaluate.',
      'Tengen is the fixed first move and needs no evaluation; only White 2 and Black 3 are actually searched.',
      'Evaluating 4 or more leaves the opening stage, and the eval stops being reliable.',
      '"Even" means |eval| <= 50 (the official balance window); larger is more lopsided - near 200 is clearly one-sided.',
      'Balance points are rule-bound: Renju (forbidden moves) and freestyle give different points; the result only holds for the rule currently selected.',
      'Under Taraguchi-10 (a 5-move opening) you may evaluate up to 5 points.',
    ],
    anBalNoEngine: 'Balance sub-engine unavailable (native balance search did not answer) — returned empty rather than substituting the playing engine for balance points; retry shortly',
    anBalFail: 'Balance search failed: ',
    anBalnSet: 'Balance point count set to {n} (3 fixed on empty board)',
    anBalnHint: 'Balance point count (1-6, 3 fixed on empty board)',
    anBalLabel: 'Points',
    anBalColor: ' (Blue = White, Purple = Black)',
    
    balCall: 'Balance picks',
    balCallT: 'Auto-call picks via the official balance search (YXBALANCE)',
    balCallBusy: 'Official balance search…',
    balCallDone: '{n} official balanced picks called: {list}',
    balCallFail: 'Balance search failed: ',
    balSwapNow: 'Engine says: swap now',
    anBal1Off: 'Official balanced move: {list}',
    anBal2Off: 'Official balanced pair: {list}',
    ofRegion: 'Outside the allowed area for this move',
    taraSwapped: 'Sides swapped',
    taraSwapAi: 'AI swapped sides',
    tagTara: 'Tarannikov swap eval',
    taraYes: 'Swap',
    taraNo: 'Play on',
    anAfter: 'after 2 plies',
    guideYama: 'B1 → W2 → B3', guideYamaEn: 'B1 → W2 → B3',
    guideSwap: 'Waiting for the colour swap', guideSwapEn: 'awaiting swap',
    swapTtl: 'Swap finished · pick first or second',
    swapFirst: 'First · Black', swapSecond: 'Second · White',
    ruleName: {
      0: 'Freestyle', 1: 'Freestyle · overline loses', 2: 'Renju (forbidden)',
      5: 'Swap1', 6: 'Yamaguchi', 7: 'Taraguchi-10', 8: 'Tarannikov',
    },
    ruleHint: {
      0: 'Freestyle: five or more in a row wins.',
      1: 'Standard: overlines do not win - exactly five.',
      2: 'Renju: black is forbidden (3-3 / 4-4 / overline), white is not.',
      5: 'Swap1: after black`s first stone, white may swap colours.',
      6: 'Yamaguchi: black places 3 stones (B1 centre, W2 3x3, B3 5x5) and declares the number of 5th-move picks; white picks a colour and plays the 4th anywhere; black names the picks, white chooses one as the 5th.',
      7: 'Taraguchi-10: B1 centre, W2 within 3x3, B3 5x5, W4 7x7; black plays the 5th (9x9) or calls ten, white picks one, then white picks a colour.',
      8: 'Tarannikov: B1 centre, W2 3x3, B3 5x5, W4 7x7, B5 9x9; after every move the opponent may swap colours. No tentative fifths.',
    },
    ruleKept: 'Rule switched to "{r}": the current board is kept as-is and play continues under the new rule (result and forbidden marks recomputed)',
    fbBlocked: 'Forbidden: Black may not play here (3-3 / 4-4 / overline) - the red crosses on the board are exactly those points',
  },
};
function T(k) { var d = I18N[S.lang] || I18N.zh; return d[k] !== undefined ? d[k] : k; }

var S = {
  lang: 'zh', theme: 'light',
  mode: 'pve',                 
                               
  egSeq: true, egColor: 1,
  
  egLocked: false, egBase: null,
  
  aiB: false, aiW: false,
  side: '',                    
  
  turnMs: 2000,               
  rule: 0,
  cores: 0, hashMB: 0,
  weights: '',
  candRange: 3, maxDepth: 100, maxNodes: 0,
  variant: '',
  hashDefMig: false,          
  coresDefault: '',
  
  coach: false,
  
  bgThink: false,
  
  showNum: false,
  
  rvNum: false,
  rvSpeedSec: 1,               
  paused: false,
  
  boardColor: '', pageBgColor: '', cssVars: null,
  
  customColors: { board: '', page: '' },
  
  previewOn: true,
  customBase: 'light',         
  nbest: 3,                    
  
  cards: null,                 
};

var G = {
  moves: [],                   
  board: null,                 
  busy: false,
  engineOffline: false,        
  retryTimer: 0,               
  retryLeft: null,             
  
  sideSettle: false,           
  sideSettleT: 0,              
  sideSettleWasBusy: false,    
  reconBoost: false,           
  over: false,                 
  lastStat: null,
  curve: [],                   
  heat: [],                    
  heatGen: 0,                  
  heatColors: 2,               
  
  coach: [],                   
  coachGen: 0,                 
  heatLen: -1,                 
  nums: [],                    
  think: [],                   
  missRings: [],               
  redo: [],                    
  rvRecord: null,              
  loaded: null,                
  review: null,                
  rvPlaying: false,            
  rvTimer: 0,                  
  
  host: { cpu: 4, threadsDefault: 2, threadsMax: 3, memMB: 4096, native: false },
  hover: null,                 
  animId: 0,
  
  ana: { gen: 0, busy: false, kind: '', marks: [], rows: [],
         
         defWinUntil: 0, defRefining: false, autoDefend: false, defTimer: null,
         
         deferTmr: null },
  
  fwd: { on: false, gen: 0, line: [], idx: -1, hover: -1, busy: false, timer: 0,
         committed: -1, hold: false, sel: -1, hoverTimer: 0, base: 0, kind: '', atk: 0,   
         
         wantVcx: false, want: '', want2: '', vcxAt: -1, vcxMarks: [] },
};

function newBoard() {
  var m = [], y, x, row;
  for (y = 0; y < N; y++) { row = []; for (x = 0; x < N; x++) row.push(0); m.push(row); }
  return m;
}
function hideReviewNav() {
  els.btn_rv_prev.hidden = els.btn_rv_next.hidden = els.rvPos.hidden = els.btn_rv_exit.hidden = true;
  
  rvStopPlay();
  if (els.btn_rv_play) els.btn_rv_play.hidden = true;
  if (els.rvSpeed) els.rvSpeed.hidden = true;
}
function setMissBox() {
  var n = G.review ? G.review.miss : 0;
  els.rvMiss.textContent = T('miss') + ' ' + n;
}
function resetGame(keepLoaded) {
  engMsgClear();                               
  G.moves = []; G.board = newBoard(); G.curve = [];
  clearHeat();                                 
  clearCoach();                                
  anaReset();                                  
  G.ana.autoDefend = false;                    
  if (els.swapPop) els.swapPop.hidden = true;  
  G.missRings = []; G.redo = []; G.review = null; G.lastStat = null;
  G.over = false;                              
  
  S.egLocked = false; S.egBase = null;
  if (!keepLoaded) G.loaded = null;
  els.dockL.style.display = '';
  els.dockR.style.display = '';
  hideReviewNav();
  setMissBox();
  persistGame();
  paint();
}
function curColor() { return (G.moves.length % 2 === 0) ? 1 : 2; }   

function myColor() { return (!!S.aiB === !!S.aiW) ? 0 : (S.aiB ? 2 : 1); }

function isAiColor(c) { return (c === 1) ? !!S.aiB : !!S.aiW; }

function isUserColor(c) { return !isAiColor(c); }

function aiVsAi() { return !!S.aiB && !!S.aiW; }

function humanBoth() { return !S.aiB && !S.aiW; }

function setAiSides(b, w) {
  
  if (linkSessionOn() && LINK_CFG_LAST &&
      typeof LINK_CFG_LAST.aiB === 'boolean' && typeof LINK_CFG_LAST.aiW === 'boolean') {
    b = LINK_CFG_LAST.aiB === true; w = LINK_CFG_LAST.aiW === true;
  }
  S.aiB = !!b; S.aiW = !!w;
  S.side = (S.aiB === S.aiW) ? '' : (S.aiB ? 'w' : 'b');   
  try { dashEvalRefresh(); } catch (e) {}
  try { bgThinkYield(); } catch (e2) {}
}

var els = {};
function grab() {
  ['t_title','t_ver','btn_history','btn_lang','btn_color','btn_about','about','ab_body','ab_close','t_about',
   'board','boardWrap',
   'btn_pause','btn_prev','btn_next','btn_reset','btn_save','boardBtns',
   'btn_rot','btn_mirror','btn_shift','mirrorPop','shiftPop','t_mirrorTtl','t_shiftTtl',
   'btn_mv_fv','btn_mv_fh','btn_mv_d1','btn_mv_d2','btn_mv_close',
   'btn_mv_up','btn_mv_down','btn_mv_left','btn_mv_right','btn_shift_close',
   'btn_ten','btn_balcall','yamaDecl','taraSwap','btn_tara_yes','btn_tara_no',
   'openBar','openHint','boardFoot','swapPop','t_swapTtl','btn_swap_first','btn_swap_second',
   't_code','inp_code','btn_copy','btn_paste','btn_load',
   'btn_review','rvBar','rvGroup','btn_recite','btn_replay','rvMiss',   
   'btn_rv_prev','btn_rv_next','btn_rv_exit','rvPos','btn_redo_rv','btn_save_rv','btn_rv_hist',
   'btn_rv_play','rvSpeed',
   
   'btn_link', 'linkPane','linkStatus','lk_auto','lk_bal','lk_eval',
   'lk_select','lk_black','lk_white',
   'btn_vis','visPane','visView','visBox','visInput','visImg','visPos','btn_vis_del',
   'btn_vis_prev','btn_vis_next','btn_vis_upload','btn_vis_shot',
   'btn_vis_rec','btn_vis_rc','visRcBox','btn_vis_load','btn_vis_vc','visMsg','file_vis',
   
   'btn_vis_crop','visCrop','cropStage','cropCanvas','t_visCropTitle','t_visCropHint','t_visCropSize',
   'btn_crop_reset','btn_crop_cancel','btn_crop_ok',
   
   'btn_vis_edit','visEditBar','btn_ve_swap','btn_ve_del','btn_ve_add','btn_ve_addb',
   'btn_ve_addw','veAddKeys','btn_ve_fillb','btn_ve_fillw','btn_ve_undo',
   'visCodeBar','visCode','btn_vis_codecopy','t_visCode',
   'btn_vd_sel','btn_vd_all','btn_vd_del',
   'visDrawer','vdList','vdCount','btn_vd_close','t_visDrawer','t_vdTip',
   'stage','dockL','dockR','boardCol',
   't_setup','seg_mode','seg_side','t_mode','t_side','t_turn','sideHint',
   'egBar','btn_eg_ok','btn_eg_seq','btn_eg_free','egKeys','btn_eg_b','btn_eg_w',   
   'num_turn','t_rule','sel_rule','ruleHint',
   'sysInfo','t_cores','sel_cores','coresHint','t_hash','sel_hash','t_variant','sel_variant','variantHint','t_weights','sel_weights','weightsHint','t_candRange','sel_candRange','candRangeHint','t_multiPv','sel_multiPv','multiPvHint',
   't_maxDepth','sel_maxDepth','inp_maxDepth','t_maxNodes','sel_maxNodes','inp_maxNodes',
   
   't_secGame','t_secEngine','t_secView','t_secCode',
   
   'hashInfo',
   't_coach','chk_coach','t_coachHint',
   'heatLegend','t_heatBest','t_heatGood','t_heatFair','t_heatPoor',
   't_showNum','chk_num','t_showNumHint',
   't_bgThink','btn_bgThink','t_bgThinkHint',
   'btn_rv_num',                                   
   't_preview','chk_preview','t_previewHint',
   'fwdBox','btn_fwd','btn_fwd_vcf','btn_fwd_vct','btn_fwd_ok','btn_fwd_prev','btn_fwd_next','fwdBody','fwdMsg','fwdSeq','fwdLg','fwdLgB','fwdLgW','btn_fwd_clear',
   'vcxPop','vcxPopBody','vcxPop_close','t_vcxPop',
   't_engine','t_depth','t_speed','t_nodes','t_time','t_eval','t_best',
   'st_depth','st_speed','st_nodes','st_time','st_eval','st_best','st_line','pvBox',
   
  'turnPill',
   't_curve','t_curveTtl','curve','curveAxis','curveScroll','curveWrap','curveHover','curveInfo',
   't_blackCurve','t_whiteCurve','ci_b_score','ci_b_win','ci_w_score','ci_w_win',
   'drawer','drList','drCount','dr_save','dr_open','dr_del','dr_close','dr_all',
   'dr_exp','dr_imp','ctxMenu','file_imp',
   
   'btn_color','colorPop','t_color','cp_close','cp_theme','cp_target','cp_sv','cp_hue',
   'cp_r','cp_g','cp_b','cp_rn','cp_gn','cp_bn','cp_hex','cp_presets','cp_prev','cp_hint','cp_reset',
   'cpPresetRow',
   't_cpTheme','t_cpTarget','t_cpHex','t_cpPreset',
   'btn_set','setPop','btn_aiside','aiSidePop','btn_ai_close',   
   'ai_side_b','ai_side_w','t_aiTurn','num_turn2','btn_live',
   't_analysis','btn_an_stop','btn_an_nbest','btn_an_bal','btn_an_baln','btn_an_defend',
   'num_nbest','t_nbest','t_nbestHint','anStatus','anList','anStatusRow',
   't_history','t_saved','t_drTip'].forEach(function (id) {
    els[id] = document.getElementById(id);
  });
}

function layoutBoard() {
  var box = els.boardWrap.getBoundingClientRect();
  
  var pad = 4;
  var side = Math.max(160, Math.min(box.width, box.height) - pad * 2);
  var dpr = window.devicePixelRatio || 1;
  els.board.style.width = side + 'px';
  els.board.style.height = side + 'px';
  els.board.width = Math.round(side * dpr);
  els.board.height = Math.round(side * dpr);
  paint();
  drawCurve();
}

function css(name) {
  return getComputedStyle(document.body).getPropertyValue(name).trim();
}

function geom() {
  var cv = els.board, dpr = window.devicePixelRatio || 1;
  var size = cv.width / dpr;
  var axis = Math.max(14, size * 0.055);
  var pad = axis + Math.max(4, size * 0.012);
  var gap = (size - pad * 2) / (N - 1);
  return { size: size, dpr: dpr, axis: axis, pad: pad, gap: gap };
}

function numShown() { return RV_MODE ? !!S.rvNum : !!S.showNum; }

function pxy(p) { return p.x !== undefined ? [p.x, p.y] : [p[0], p[1]]; }
function pset(p, x, y) { if (p.x !== undefined) { p.x = x; p.y = y; } else { p[0] = x; p[1] = y; } }

function layoutTip(msg) {
  setStat(null, msg);
  var host = document.getElementById('boardCol');
  if (!host) return;
  var old = document.getElementById('layTip');
  if (old && old.parentNode) old.parentNode.removeChild(old);
  var d = document.createElement('div');
  d.id = 'layTip'; d.className = 'laytip'; d.textContent = msg;
  host.appendChild(d);
  setTimeout(function () { if (d && d.parentNode) d.parentNode.removeChild(d); }, 1800);
}
function layoutAlive() {
  if (RV_MODE || G.review) { layoutTip(T('layoutRV')); return false; }
  if (G.busy) { layoutTip(T('layoutBusy')); return false; }
  return true;
}

function layoutApply(map) {
  if (!layoutAlive()) return false;
  var lists = [G.moves], r, i, p, q;
  if (G.redo) for (r = 0; r < G.redo.length; r++) lists.push(G.redo[r].moves);
  if (S.egBase) lists.push(S.egBase);
  if (G.loaded && G.loaded.moves) lists.push(G.loaded.moves);
  for (r = 0; r < lists.length; r++)
    for (i = 0; i < lists[r].length; i++) {
      q = map.apply(null, pxy(lists[r][i]));
      if (q[0] < 0 || q[1] < 0 || q[0] >= N || q[1] >= N) { setStat(null, T('shiftOOB')); return false; }
    }
  for (r = 0; r < lists.length; r++)
    for (i = 0; i < lists[r].length; i++) {
      p = lists[r][i]; q = map.apply(null, pxy(p)); pset(p, q[0], q[1]);
    }
  G.board = newBoard();
  for (i = 0; i < G.moves.length; i++) { p = G.moves[i]; G.board[p.y][p.x] = p.c; }
  G.hover = null;
  clearHeat();                        
  G.coach = []; G.coachGen++;         
  anaReset();                         
  G.missRings = [];                   
  if (G.fwd && G.fwd.on && !G.fwd.hold) fwdQueue();
  paint();
  return true;
}

var ROT_CW  = function (x, y) { return [N - 1 - y, x]; };
var FLIP_H  = function (x, y) { return [N - 1 - x, y]; };
var FLIP_V  = function (x, y) { return [x, N - 1 - y]; };
var DIAG_1  = function (x, y) { return [y, x]; };                  
var DIAG_2  = function (x, y) { return [N - 1 - y, N - 1 - x]; };  
var SHIFT_U = function (x, y) { return [x, y - 1]; };
var SHIFT_D = function (x, y) { return [x, y + 1]; };
var SHIFT_L = function (x, y) { return [x - 1, y]; };
var SHIFT_R = function (x, y) { return [x + 1, y]; };

function cellAt(px, py) {
  var g = geom();
  var x = Math.round((px - g.pad) / g.gap), y = Math.round((py - g.pad) / g.gap);
  if (x < 0 || y < 0 || x >= N || y >= N) return null;
  return { x: x, y: y };
}

function paint() {
  var cv = els.board, ctx = cv.getContext('2d');
  var g = geom(), dpr = g.dpr, size = g.size, pad = g.pad, gap = g.gap;
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.clearRect(0, 0, cv.width, cv.height);
  ctx.scale(dpr, dpr);

  ctx.fillStyle = css('--board');
  ctx.fillRect(0, 0, size, size);

  ctx.strokeStyle = css('--line');
  ctx.lineWidth = Math.max(1, size / 700);
  ctx.beginPath();
  for (var i = 0; i < N; i++) {
    var p = pad + i * gap;
    ctx.moveTo(pad, p); ctx.lineTo(pad + gap * (N - 1), p);
    ctx.moveTo(p, pad); ctx.lineTo(p, pad + gap * (N - 1));
  }
  ctx.stroke();

  ctx.fillStyle = css('--axis');
  ctx.font = Math.max(7, g.axis * 0.5) + 'px "Segoe UI",system-ui,sans-serif';
  ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
  for (i = 0; i < N; i++) {
    var cx = pad + i * gap;
    var letter = String.fromCharCode(65 + i);
    ctx.fillText(letter, cx, g.axis * 0.5);
    ctx.fillText(letter, cx, size - g.axis * 0.5);
    var num = String(N - i);
    ctx.fillText(num, g.axis * 0.5, cx);
    ctx.fillText(num, size - g.axis * 0.5, cx);
  }
  
  ctx.fillStyle = css('--star');
  [[3, 3], [11, 3], [7, 7], [3, 11], [11, 11]].forEach(function (s) {
    ctx.beginPath();
    ctx.arc(pad + s[0] * gap, pad + s[1] * gap, Math.max(1.6, gap * 0.09), 0, Math.PI * 2);
    ctx.fill();
  });

  var HEAT4 = HEAT_PAL[2];                                 
  G.coach.forEach(function (h) {
    ctx.fillStyle = HEAT4[Math.max(0, Math.min(3, h.tier - 1))];
    var s = gap * 0.86;
    ctx.fillRect(pad + h.x * gap - s / 2, pad + h.y * gap - s / 2, s, s);
  });
  
  if (els.heatLegend) {
    var lgN = S.coach ? 4 : 0;
    if (lgN && els.heatLegend.getAttribute('data-colors') !== String(lgN))
      els.heatLegend.setAttribute('data-colors', String(lgN));
  }

  if (G.hover && S.previewOn !== false) {
    var hsz = gap * 0.92, hr = Math.max(2.5, gap * 0.16);
    var hx = pad + G.hover.x * gap - hsz / 2, hy = pad + G.hover.y * gap - hsz / 2;
    ctx.beginPath();
    ctx.moveTo(hx + hr, hy);
    ctx.lineTo(hx + hsz - hr, hy);
    ctx.quadraticCurveTo(hx + hsz, hy, hx + hsz, hy + hr);
    ctx.lineTo(hx + hsz, hy + hsz - hr);
    ctx.quadraticCurveTo(hx + hsz, hy + hsz, hx + hsz - hr, hy + hsz);
    ctx.lineTo(hx + hr, hy + hsz);
    ctx.quadraticCurveTo(hx, hy + hsz, hx, hy + hsz - hr);
    ctx.lineTo(hx, hy + hr);
    ctx.quadraticCurveTo(hx, hy, hx + hr, hy);
    ctx.closePath();
    
    var hPurple = !!(VIS && VIS.edit);
    
    var hForbid = (!VIS && !RV_MODE && renjuRule() && forbiddenAt(G.hover.x, G.hover.y));
    ctx.fillStyle = hPurple ? 'rgba(186,152,255,.28)'
      : (hForbid ? 'rgba(226,59,46,.26)' : 'rgba(150,168,196,.20)');
    ctx.fill();
    ctx.strokeStyle = hPurple ? 'rgba(158,120,246,.80)'
      : (hForbid ? 'rgba(226,59,46,.88)' : 'rgba(150,168,196,.66)');
    ctx.lineWidth = Math.max(1.4, gap * 0.055);
    ctx.stroke();
  }

  var r = gap * 0.47;
  for (var y = 0; y < N; y++) for (var x = 0; x < N; x++) {
    var c = G.board[y][x];
    if (!c) continue;
    var px = pad + x * gap, py = pad + y * gap;
    ctx.beginPath(); ctx.arc(px, py, r, 0, Math.PI * 2);
    if (c === 1) { ctx.fillStyle = css('--blk'); ctx.fill(); ctx.strokeStyle = css('--blk-edge'); }
    else { ctx.fillStyle = css('--wht'); ctx.fill(); ctx.strokeStyle = css('--wht-edge'); }
    
    ctx.lineWidth = (c === 1) ? Math.max(0.9, gap * 0.026) : Math.max(0.7, gap * 0.016);
    ctx.stroke();
  }

  if (typeof VIS !== 'undefined' && VIS.edit) {
    (VIS.ghosts || []).forEach(function (gp) {
      var px = pad + gp.x * gap, py = pad + gp.y * gap;
      ctx.save();
      ctx.globalAlpha = 0.45;
      ctx.beginPath(); ctx.arc(px, py, r, 0, Math.PI * 2);
      if (gp.c === 1) { ctx.fillStyle = css('--blk'); ctx.fill(); ctx.strokeStyle = css('--blk-edge'); }
      else { ctx.fillStyle = css('--wht'); ctx.fill(); ctx.strokeStyle = css('--wht-edge'); }
      ctx.lineWidth = Math.max(0.9, gap * 0.026);
      ctx.stroke();
      ctx.restore();
    });
    if (VIS.ghosts && VIS.ghosts.length === 2) {
      var gx = [VIS.ghosts[0].x, VIS.ghosts[1].x], gy = [VIS.ghosts[0].y, VIS.ghosts[1].y];
      var bx0 = pad + Math.min.apply(null, gx) * gap - r - gap * 0.10;
      var by0 = pad + Math.min.apply(null, gy) * gap - r - gap * 0.10;
      var bx1 = pad + Math.max.apply(null, gx) * gap + r + gap * 0.10;
      var by1 = pad + Math.max.apply(null, gy) * gap + r + gap * 0.10;
      ctx.save();
      ctx.setLineDash([Math.max(4, gap * 0.16), Math.max(3, gap * 0.10)]);
      ctx.strokeStyle = '#b58cff';
      ctx.lineWidth = Math.max(1.6, gap * 0.06);
      ctx.strokeRect(bx0, by0, bx1 - bx0, by1 - by0);
      ctx.restore();
    }
    if (VIS.swapSel) {
      ctx.save();
      ctx.strokeStyle = '#f0a63c';
      ctx.lineWidth = Math.max(2, gap * 0.08);
      ctx.beginPath();
      ctx.arc(pad + VIS.swapSel.x * gap, pad + VIS.swapSel.y * gap, r * 1.18, 0, Math.PI * 2);
      ctx.stroke();
      ctx.restore();
    }
  }

  var wl = findWinLine(G.board, ruleForRender());
  if (wl) {
    var w0 = wl.cells[0], w1 = wl.cells[wl.cells.length - 1];
    ctx.save();
    ctx.globalAlpha = 0.68;
    ctx.lineCap = 'round';
    ctx.strokeStyle = css('--winline');
    ctx.lineWidth = Math.max(2, gap * 0.07);
    ctx.beginPath();
    ctx.moveTo(pad + w0.x * gap, pad + w0.y * gap);
    ctx.lineTo(pad + w1.x * gap, pad + w1.y * gap);
    ctx.stroke();
    ctx.restore();
  }

  drawForbiddenMarks(ctx, g);

  if (numShown() && G.moves.length) {
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    
    ctx.font = '600 ' + Math.max(7, gap * 0.42) + 'px "Segoe UI",system-ui,sans-serif';
    G.moves.forEach(function (m, i) {
      
      if (i === G.moves.length - 1) return;
      var sx = pad + m.x * gap, sy = pad + m.y * gap;
      
      ctx.fillStyle = (m.c === 1) ? '#ffffff' : '#1a1a1a';
      ctx.fillText(String(i + 1), sx, sy);
    });
  }

  if ((G.heat.length || G.coach.length) && S.mode !== 'place' && S.mode !== 'endgame' && aiDecorOn()) {
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    
    ctx.font = '500 ' + Math.max(8, gap * 0.26) + 'px "Segoe UI",system-ui,sans-serif';
    ctx.lineJoin = 'round';
    var drawHeatNum = function (h) {
      if (!h.ev) return;
      var px = pad + h.x * gap, py = pad + h.y * gap;
      ctx.lineWidth = Math.max(2.4, gap * 0.1);
      ctx.strokeStyle = 'rgba(255,255,255,.9)';    
      ctx.strokeText(h.ev, px, py);
      ctx.fillStyle = '#241f1a';
      ctx.fillText(h.ev, px, py);
    };
    G.coach.forEach(drawHeatNum);                  
    G.heat.forEach(drawHeatNum);                   
  }


  if (G.busy && G.think.length && aiDecorOn()) {
    var t = (Date.now() % 1200) / 1200;
    ctx.strokeStyle = '#7a5af8';
    ctx.lineWidth = Math.max(1.6, gap * 0.07);
    G.think.forEach(function (p, i) {
      var a = 0.25 + 0.6 * (0.5 + 0.5 * Math.sin((t + i / G.think.length) * Math.PI * 2));
      ctx.globalAlpha = a;
      ctx.beginPath();
      ctx.arc(pad + p.x * gap, pad + p.y * gap, gap * 0.3, 0, Math.PI * 2);
      ctx.stroke();
    });
    ctx.globalAlpha = 1;
  }

  if (G.moves.length) {
    var lm = G.moves[G.moves.length - 1];
    var lx = pad + lm.x * gap, ly = pad + lm.y * gap;
    if (numShown()) {
      ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
      ctx.lineJoin = 'round';
      
      ctx.font = '700 ' + Math.max(9, gap * 0.42) + 'px "Segoe UI",system-ui,sans-serif';
      ctx.lineWidth = Math.max(1.6, gap * 0.05);
      ctx.strokeStyle = 'rgba(15,40,60,.35)';   
      ctx.strokeText(String(G.moves.length), lx, ly);
      
      ctx.fillStyle = css('--winline');
      ctx.fillText(String(G.moves.length), lx, ly);
    } else {
      ctx.fillStyle = css('--winline');             
      ctx.beginPath();
      ctx.arc(lx, ly, Math.max(2.2, r * 0.22), 0, Math.PI * 2);
      ctx.fill();
    }
  }

  var gpv = null, psd = curColor();
  if (PV_HOVER && PV_RENDER.length) {
    var ghi = PV_HOVER.i | 0;
    if (ghi >= 0 && ghi < PV_RENDER.length) {
      gpv = PV_RENDER.slice(0, ghi + 1);
      psd = (PV_SIDE === 1 || PV_SIDE === 2) ? PV_SIDE : curColor();
    }
  }
  if (!gpv && PV_PREVIEW && PV_PREVIEW.moves.length) {
    gpv = PV_PREVIEW.moves;
    psd = (PV_PREVIEW.side === 1 || PV_PREVIEW.side === 2) ? PV_PREVIEW.side : curColor();
  }
  if (gpv && gpv.length && aiDecorOn() &&
      S.mode !== 'place' && S.mode !== 'endgame') {
    ctx.save();
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle'; ctx.lineJoin = 'round';
    ctx.font = '600 ' + Math.max(7, gap * 0.36) + 'px "Segoe UI",system-ui,sans-serif';
    gpv.forEach(function (p, i) {
      if (p[0] < 0 || p[1] < 0 || p[0] >= N || p[1] >= N) return;
      if (G.board[p[1]] && G.board[p[1]][p[0]]) return;
      var col = (i % 2 === 0) ? psd : 3 - psd;
      var cx = pad + p[0] * gap, cy = pad + p[1] * gap;
      ctx.globalAlpha = (col === 1) ? 0.56 : 0.66;
      ctx.beginPath(); ctx.arc(cx, cy, r, 0, Math.PI * 2);
      ctx.fillStyle = (col === 1) ? css('--blk') : css('--wht'); ctx.fill();
      ctx.globalAlpha = 1;
      ctx.lineWidth = Math.max(1.2, gap * 0.045);
      ctx.strokeStyle = (col === 1) ? 'rgba(255,255,255,.42)' : 'rgba(70,58,35,.45)';
      ctx.stroke();
      ctx.globalAlpha = 0.92;
      ctx.fillStyle = (col === 1) ? '#f4f1e8' : '#2b261c';
      ctx.fillText(String(i + 1), cx, cy);
    });
    ctx.restore();
  }

  if (PV_HOVER && aiDecorOn() && S.mode !== 'place' && S.mode !== 'endgame') {
    var hx = pad + PV_HOVER.x * gap, hy = pad + PV_HOVER.y * gap;
    ctx.save();
    ctx.strokeStyle = 'rgba(122,90,248,.95)';
    ctx.lineWidth = Math.max(2, gap * 0.075);
    if (ctx.setLineDash) ctx.setLineDash([gap * 0.16, gap * 0.11]);
    ctx.beginPath(); ctx.arc(hx, hy, Math.max(9, gap * 0.44), 0, Math.PI * 2); ctx.stroke();
    if (ctx.setLineDash) ctx.setLineDash([]);
    var hb = (gpv && gpv.length) ? '' : String(PV_HOVER.seq || '');
    if (hb) {
      ctx.font = '700 ' + Math.max(8.5, gap * 0.30) + 'px "Segoe UI",system-ui,sans-serif';
      ctx.textAlign = 'center'; ctx.textBaseline = 'middle'; ctx.lineJoin = 'round';
      ctx.lineWidth = Math.max(2.4, gap * 0.10);
      ctx.strokeStyle = 'rgba(255,255,255,.95)';
      ctx.strokeText(hb, hx, hy);
      ctx.fillStyle = '#7a5af8';
      ctx.fillText(hb, hx, hy);
    }
    ctx.restore();
  }

  if (openActive() && aiDecorOn() && OPEN.kind === 'tar' && OPEN.tar && OPEN.tar.called.length) {
    ctx.strokeStyle = '#00bfa5';
    ctx.lineWidth = Math.max(1.6, gap * 0.06);
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    ctx.font = '600 ' + Math.max(8, gap * 0.3) + 'px "Segoe UI",system-ui,sans-serif';
    OPEN.tar.called.forEach(function (p, i) {
      var px = pad + p[0] * gap, py = pad + p[1] * gap;
      ctx.beginPath(); ctx.arc(px, py, r * 0.62, 0, Math.PI * 2); ctx.stroke();
      ctx.fillStyle = '#00bfa5';
      ctx.fillText(String(i + 1), px, py);
    });
  }
  
  G.missRings.forEach(function (p) {
    ctx.strokeStyle = css('--miss');
    ctx.lineWidth = Math.max(2, gap * 0.09);
    ctx.beginPath();
    ctx.arc(pad + p.x * gap, pad + p.y * gap, r * 1.25, 0, Math.PI * 2);
    ctx.stroke();
  });
  
  if (openActive() && aiDecorOn()) drawOpenGuide(ctx, g, r);

  if (G.fwd && G.fwd.on && G.fwd.line.length && aiDecorOn()) {
    var show = fwdShowIdx();
    
    if (G.fwd.kind && G.fwd.vcxAt >= 0) {
      ctx.save();
      ctx.setLineDash([Math.max(3, gap * 0.16), Math.max(2.5, gap * 0.12)]);
      ctx.lineWidth = Math.max(1.4, gap * 0.05);
      ctx.strokeStyle = 'rgba(96,146,248,.85)';        
      ctx.lineJoin = 'round'; ctx.lineCap = 'round';
      ctx.beginPath();
      var dashStarted = false;
      var dFrom = Math.max(G.fwd.committed + 1, G.fwd.vcxAt);   
      for (var di = dFrom; di <= show && di < G.fwd.line.length; di++) {
        var dm = G.fwd.line[di];
        var dxp = pad + dm.x * gap, dyp = pad + dm.y * gap;
        if (!dashStarted) { ctx.moveTo(dxp, dyp); dashStarted = true; }
        else ctx.lineTo(dxp, dyp);
      }
      if (dashStarted) ctx.stroke();
      ctx.restore();
    }
    
    if (G.fwd.vcxAt >= 0 && G.fwd.vcxMarks.length) {
      for (var mi = 0; mi < G.fwd.vcxMarks.length; mi++) {
        var mk0 = G.fwd.vcxMarks[mi];
        if (mk0.i < G.fwd.vcxAt || mk0.i > show || mk0.i <= G.fwd.committed) continue;
        if (mk0.t < 4) continue;                               
        var mpx = pad + mk0.x * gap, mpy = pad + mk0.y * gap;
        ctx.save();
        if (mk0.t >= 5) {                                      
          ctx.setLineDash([Math.max(4, gap * 0.30), Math.max(3, gap * 0.18)]);
          ctx.lineWidth = Math.max(1.8, gap * 0.06);
          ctx.strokeStyle = 'rgba(59,108,242,.68)';
          ctx.beginPath(); ctx.arc(mpx, mpy, r * 1.72, 0, Math.PI * 2); ctx.stroke();
          ctx.beginPath(); ctx.arc(mpx, mpy, r * 1.22, 0, Math.PI * 2); ctx.stroke();
        } else {                                               
          ctx.setLineDash([Math.max(3.5, gap * 0.22), Math.max(2.5, gap * 0.14)]);
          ctx.lineWidth = Math.max(1.5, gap * 0.052);
          ctx.strokeStyle = 'rgba(88,164,246,.60)';
          var hw = r * 1.62;
          ctx.beginPath(); ctx.rect(mpx - hw, mpy - hw, hw * 2, hw * 2); ctx.stroke();
        }
        ctx.restore();
      }
    }
    for (var fi = 0; fi <= show && fi < G.fwd.line.length; fi++) {
      
      if (fi <= G.fwd.committed) continue;
      var fm = G.fwd.line[fi];
      var fx = pad + fm.x * gap, fy = pad + fm.y * gap;
      
      ctx.globalAlpha = (G.fwd.vcxAt >= 0 && fi >= G.fwd.vcxAt) ? 0.8 : 0.52;   
      ctx.beginPath(); ctx.arc(fx, fy, r, 0, Math.PI * 2);
      ctx.fillStyle = css(fm.c === 1 ? '--blk' : '--wht'); ctx.fill();
      var gr = ctx.createLinearGradient(fx - r, fy - r, fx + r, fy + r);
      if (fm.c === 1) { gr.addColorStop(0, 'rgba(140,200,255,.95)'); gr.addColorStop(1, 'rgba(190,150,255,.95)'); }
      else { gr.addColorStop(0, 'rgba(120,220,235,.95)'); gr.addColorStop(1, 'rgba(170,190,255,.95)'); }
      ctx.globalAlpha = 0.9;
      ctx.strokeStyle = gr;
      ctx.lineWidth = Math.max(1.2, gap * 0.035);
      ctx.stroke();
      
      ctx.globalAlpha = 0.98;
      ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
      if (S.showNum) {
        
        ctx.font = '600 ' + Math.max(7, gap * 0.42) + 'px "Segoe UI",system-ui,sans-serif';
        var ng2 = ctx.createLinearGradient(fx - r, fy - r, fx + r, fy + r);
        if (fm.c === 1) { ng2.addColorStop(0, '#8ec9ff'); ng2.addColorStop(1, '#d0a8ff'); }
        else { ng2.addColorStop(0, '#17b3d4'); ng2.addColorStop(1, '#7d8fe2'); }
        ctx.fillStyle = ng2;
        ctx.fillText(String(G.fwd.base + fi + 1), fx, fy + 0.5);
      } else {
        ctx.font = '700 ' + Math.max(9, Math.round(r * 0.95)) + 'px "Segoe UI",system-ui,sans-serif';
        var ng = ctx.createLinearGradient(fx - r, fy - r, fx + r, fy + r);
        if (fm.c === 1) { ng.addColorStop(0, '#8ec9ff'); ng.addColorStop(1, '#d0a8ff'); }
        else { ng.addColorStop(0, '#4fd4ea'); ng.addColorStop(1, '#9fb2ff'); }
        ctx.fillStyle = ng;
        ctx.fillText(String(fi + 1), fx, fy + 0.5);
      }
      ctx.globalAlpha = 1;
    }
  }
  
  var balHide = false;
  if (G.ana.kind === 'bal' && G.ana.marks.length) {
    // Hide only after the user has played as many stones as there are marks
    // SINCE they were computed. The old rule compared the ABSOLUTE move count
    // against the mark count, so every pick on a board that already had stones
    // (e.g. 5 stones + 2 picks) was erased the moment it was drawn.
    var balBaseM = (G.ana.balBase != null) ? G.ana.balBase : 0;
    if (G.moves.length >= balBaseM + G.ana.marks.length) balHide = true;
  } else if (G.ana.kind === 'bal1' && G.ana.marks.length) {
    var isStage2 = G.ana.marks.some(function (mk) { return String(mk.badge) === '4'; });
    if (G.moves.length >= (isStage2 ? 5 : 3)) balHide = true;
  }
  if (aiDecorOn()) {
    var lm = G.ana.marks.filter(function (m) { return m && m.live; });
    if (lm.length) drawLiveMarks(ctx, lm, pad, gap);
  }
  if (aiDecorOn() && G.ana.defCells && G.ana.defCells.length)
    drawDefendMarks(ctx, G.ana.defCells, pad, gap);
  if (G.ana.marks.length && (aiDecorOn() || G.ana.kind === 'bal' || G.ana.kind === 'bal1') && !balHide) {
    
    var mr = Math.max(6, gap * 0.40);
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle'; ctx.lineJoin = 'round';
    var taken = [];
    // A balance pick that has already been played is covered by its stone, so
    // drop exactly those marks while the rest keep counting down.
    var balDraw = G.ana.marks.filter(function (m) { return !m.live; });
    if (G.ana.kind === 'bal')
      balDraw = balDraw.filter(function (m) { return !G.board[m.y][m.x]; });
    function hits(a, b) {
      return !(a.x2 < b.x1 || b.x2 < a.x1 || a.y2 < b.y1 || b.y2 < a.y1);
    }
    balDraw.forEach(function (mk) {
      if (mk.plain) return;                       
      var px = pad + mk.x * gap, py = pad + mk.y * gap;
      
      var pal = (mk.tier < 0) ? ANA_BAL : ANA_PAL[Math.max(0, Math.min(ANA_PAL.length - 1, mk.tier))];
      if (mk.col) pal = { badge: mk.col, ring: 'rgba(255,255,255,.94)' };
      
      var mt = mk.inline ? anaInlineMetrics(ctx, mk, mr) : null;
      var multi = mt ? mt.multi : (String(mk.badge).length > 1);
      var pw = mt ? mt.pw : mr * 2, ph = mt ? mt.ph : mr * 2;
      if (!mt && multi) {                          
        
        ctx.font = '700 ' + Math.max(8, mr * 0.9) + 'px "Segoe UI",system-ui,sans-serif';
        pw = ctx.measureText(mk.badge).width + mr * 0.9;
        ph = mr * 1.6;
      }
      ctx.beginPath();
      if (multi) {
        if (ctx.roundRect) ctx.roundRect(px - pw / 2, py - ph / 2, pw, ph, ph / 2);
        else ctx.rect(px - pw / 2, py - ph / 2, pw, ph);
      } else {
        ctx.arc(px, py, mr, 0, Math.PI * 2);
      }
      ctx.fillStyle = pal.badge; ctx.fill();
      ctx.lineWidth = Math.max(1.6, gap * 0.07);
      ctx.strokeStyle = pal.ring; ctx.stroke();
      taken.push({ x1: px - pw / 2 - 1, y1: py - ph / 2 - 1, x2: px + pw / 2 + 1, y2: py + ph / 2 + 1 });
    });
    balDraw.forEach(function (mk) {
      var px = pad + mk.x * gap, py = pad + mk.y * gap;
      if (mk.plain) {
        
        var pf = Math.max(7, gap * 0.24);
        ctx.font = '600 ' + pf + 'px "Segoe UI",system-ui,sans-serif';
        var palP = (mk.tier < 0) ? ANA_BAL : ANA_PAL[Math.max(0, Math.min(ANA_PAL.length - 1, mk.tier))];
        if (mk.col) palP = { badge: mk.col };   
        ctx.lineWidth = Math.max(2, gap * 0.09);
        ctx.strokeStyle = 'rgba(255,255,255,.9)';
        ctx.strokeText(mk.label, px, py);
        ctx.fillStyle = palP.badge;
        ctx.fillText(mk.label, px, py);
        return;
      }
      
      if (mk.inline) {
        var mt = anaInlineMetrics(ctx, mk, mr);
        ctx.fillStyle = '#ffffff';
        if (mk.label) {
          ctx.font = '700 ' + mt.rf + 'px "Segoe UI",system-ui,sans-serif';
          ctx.fillText(mk.badge, px, py - mt.ph * 0.19);
          ctx.font = '600 ' + mt.ef + 'px "Segoe UI",system-ui,sans-serif';
          ctx.fillText(mk.label, px, py + mt.ph * 0.24);
        } else {
          ctx.font = '700 ' + Math.max(9, mr * 0.9) + 'px "Segoe UI",system-ui,sans-serif';
          ctx.fillText(mk.badge, px, py + 0.5);
        }
        return;
      }
      
      ctx.font = '700 ' + Math.max(8, mr * 0.9) + 'px "Segoe UI",system-ui,sans-serif';
      ctx.fillStyle = '#ffffff';
      ctx.fillText(mk.badge, px, py + 0.5);
      if (!mk.label) return;
      
      var fs = Math.max(8, gap * 0.28);
      ctx.font = '600 ' + fs + 'px "Segoe UI",system-ui,sans-serif';
      var tw = ctx.measureText(mk.label).width, th = fs * 1.1;
      var off = Math.max(3, gap * 0.07);   
      var cands = [
        { x: px, y: py + mr + off + th * 0.5 },            
        { x: px, y: py - mr - off - th * 0.5 },            
        { x: px + mr + off + tw / 2, y: py },              
        { x: px - mr - off - tw / 2, y: py },              
      ];
      var pick = null;
      for (var i = 0; i < cands.length && !pick; i++) {
        var c = cands[i];
        var box = { x1: c.x - tw / 2 - 1, y1: c.y - th * 0.62, x2: c.x + tw / 2 + 1, y2: c.y + th * 0.62 };
        var clash = false;
        for (var j = 0; j < taken.length; j++) if (hits(box, taken[j])) { clash = true; break; }
        if (!clash) { pick = c; taken.push(box); }
      }
      if (!pick) pick = cands[0];
      ctx.lineWidth = Math.max(2.6, gap * 0.11);
      ctx.strokeStyle = 'rgba(255,255,255,.92)';
      ctx.strokeText(mk.label, pick.x, pick.y);
      ctx.fillStyle = '#241f1a';
      ctx.fillText(mk.label, pick.x, pick.y);
    });
  }

}

function aiDecorOn() { return !RV_MODE && !G.review; }


var LIVE_MARK_PAL = [
  { fill: 'rgba(0,194,201,.34)',  ring: 'rgba(0,210,218,.98)' },
  { fill: 'rgba(96,200,120,.30)', ring: 'rgba(110,214,134,.96)' },
  { fill: 'rgba(196,216,86,.30)', ring: 'rgba(212,228,104,.96)' },
  { fill: 'rgba(196,216,86,.20)', ring: 'rgba(212,228,104,.80)' },
  { fill: 'rgba(196,216,86,.17)', ring: 'rgba(212,228,104,.70)' },
  { fill: 'rgba(196,216,86,.15)', ring: 'rgba(212,228,104,.62)' },
  { fill: 'rgba(196,216,86,.13)', ring: 'rgba(212,228,104,.55)' },
  { fill: 'rgba(196,216,86,.11)', ring: 'rgba(212,228,104,.48)' },
];
function drawRingLabels(ctx, px, py, r, topTxt, botTxt, fsScale, fitF) {
  ctx.textAlign = 'center'; ctx.textBaseline = 'middle'; ctx.lineJoin = 'round';
  var sc = fsScale || 1;
  var fit = fitF || 1.8;
  var fsT = Math.max(9, r * 0.56 * sc);
  ctx.font = '700 ' + fsT + 'px "Segoe UI",system-ui,sans-serif';
  while (fsT > 7.5 && ctx.measureText(topTxt).width > r * fit) {
    fsT -= 0.4;
    ctx.font = '700 ' + fsT + 'px "Segoe UI",system-ui,sans-serif';
  }
  ctx.lineWidth = Math.max(2, fsT * 0.24); ctx.strokeStyle = 'rgba(0,0,0,.55)';
  var ty = botTxt ? py - r * 0.29 : py;
  ctx.strokeText(topTxt, px, ty);
  ctx.fillStyle = '#ffffff'; ctx.fillText(topTxt, px, ty);
  if (!botTxt) return;
  var fsB = Math.max(7.5, r * 0.44 * sc);
  ctx.font = '600 ' + fsB + 'px "Segoe UI",system-ui,sans-serif';
  while (fsB > 6.5 && ctx.measureText(botTxt).width > r * fit) {
    fsB -= 0.4;
    ctx.font = '600 ' + fsB + 'px "Segoe UI",system-ui,sans-serif';
  }
  ctx.lineWidth = Math.max(1.8, fsB * 0.24); ctx.strokeStyle = 'rgba(0,0,0,.55)';
  ctx.strokeText(botTxt, px, py + r * 0.32);
  ctx.fillStyle = '#ffffff'; ctx.fillText(botTxt, px, py + r * 0.32);
}

function drawLiveMarks(ctx, marks, pad, gap) {
  marks.slice(0, 8).forEach(function (mk) {
    var px = pad + mk.x * gap, py = pad + mk.y * gap;
    var r = Math.max(9, gap * 0.47);
    var best = (mk.rank === 0);
    var pal = LIVE_MARK_PAL[Math.max(0, Math.min(LIVE_MARK_PAL.length - 1, mk.rank | 0))];
    ctx.beginPath(); ctx.arc(px, py, r, 0, Math.PI * 2);
    ctx.fillStyle = pal.fill; ctx.fill();
    ctx.lineWidth = best ? Math.max(2.2, gap * 0.072) : Math.max(1.1, gap * 0.038);
    ctx.strokeStyle = pal.ring; ctx.stroke();
    drawRingLabels(ctx, px, py, r,
      Math.round((typeof mk.wr === 'number' ? mk.wr : 0.5) * 100) + '%',
      String(mk.ev || ''));
  });
}

function drawOpenGuide(ctx, g, r) {
  var pad = g.pad, gap = g.gap, n = G.moves.length;
  var gc = (css('--guide') || '#3aa0e8');
  var zh = (S.lang !== 'en');
  ctx.save();
  ctx.strokeStyle = gc; ctx.fillStyle = gc;
  ctx.setLineDash([6, 5]);
  ctx.lineWidth = Math.max(1.3, gap * 0.045);
  ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
  ctx.font = '600 ' + Math.max(9, gap * 0.3) + 'px "Segoe UI",system-ui,sans-serif';
  if (OPEN.kind === 'tar') {
    var calling = OPEN.tar && (OPEN.tar.phase === 'call10' || OPEN.tar.phase === 'pickCall');
    var reg = calling ? 4 : tarRegion(n + 1);
    var a = pad + (7 - reg) * gap, b = pad + (7 + reg) * gap;
    ctx.strokeRect(a, a, b - a, b - a);
    ctx.setLineDash([]);                       
    var tick = gap * 0.24;
    [[a, a, 1, 1], [b, a, -1, 1], [a, b, 1, -1], [b, b, -1, -1]].forEach(function (c) {
      ctx.beginPath();
      ctx.moveTo(c[0], c[1] + c[3] * tick); ctx.lineTo(c[0], c[1]);
      ctx.lineTo(c[0] + c[2] * tick, c[1]);
      ctx.stroke();
    });
    ctx.fillText((reg * 2 + 1) + '×' + (reg * 2 + 1), pad + 7 * gap, Math.max(g.axis * 0.5, a - gap * 0.55));
    if (OPEN.tar && OPEN.tar.called.length) {  
      ctx.globalAlpha = 0.55;
      ctx.setLineDash([3, 4]);
      OPEN.tar.called.forEach(function (p) {
        ctx.beginPath();
        ctx.moveTo(pad + 7 * gap, pad + 7 * gap);
        ctx.lineTo(pad + p[0] * gap, pad + p[1] * gap);
        ctx.stroke();
      });
      ctx.globalAlpha = 1;
    }
  } else if (OPEN.kind === 'yama') {
    var yc = OPEN.yama && OPEN.yama.called.length &&
             (OPEN.yama.phase === 'call' || OPEN.yama.phase === 'pick');
    if (yc) {
      
      ctx.setLineDash([]);
      ctx.globalAlpha = 0.9;
      OPEN.yama.called.forEach(function (p, i) {
        var cx = pad + p[0] * gap, cy = pad + p[1] * gap;
        ctx.beginPath();
        ctx.arc(cx, cy, gap * 0.34, 0, Math.PI * 2);
        ctx.stroke();
        ctx.fillText(String(i + 1), cx, cy);
      });
      ctx.globalAlpha = 1;
    } else {
      ctx.beginPath();
      ctx.arc(pad + 7 * gap, pad + 7 * gap, r * 1.6, 0, Math.PI * 2);
      ctx.stroke();
      ctx.fillText(zh ? T('guideYama') : T('guideYamaEn'),
                   pad + 7 * gap, Math.max(g.axis * 0.5, pad + 7 * gap - r * 1.6 - gap * 0.55));
    }
  } else if (OPEN.kind === 'tara') {
    
    var regT = tarRegion(n + 1);
    var aT = pad + (7 - regT) * gap, bT = pad + (7 + regT) * gap;
    ctx.strokeRect(aT, aT, bT - aT, bT - aT);
    ctx.setLineDash([]);
    ctx.fillText((regT * 2 + 1) + '×' + (regT * 2 + 1), pad + 7 * gap,
                 Math.max(g.axis * 0.5, aT - gap * 0.55));
  } else if (OPEN.kind === 'swap1' && n >= 1) {
    var f = G.moves[0];
    ctx.beginPath();
    ctx.arc(pad + f.x * gap, pad + f.y * gap, r * 1.55, 0, Math.PI * 2);
    ctx.stroke();
    ctx.fillText('\u21c4', pad + f.x * gap, pad + f.y * gap - r * 2.05);
    ctx.fillText(zh ? T('guideSwap') : T('guideSwapEn'), pad + 7 * gap, g.axis * 0.5);
  }
  ctx.restore();
}

function pulse() {
  if (!G.busy) { G.animId = 0; return; }
  paint();
  G.animId = requestAnimationFrame(pulse);
}
function startPulse() { if (!G.animId) G.animId = requestAnimationFrame(pulse); }

function ruleForRender() { return RV_MODE ? 0 : +S.rule; }

function exactFiveFor(rule, moverColor) {
  if (rule === 1) return true;                             
  if (rule === 2 || rule === 6 || rule === 7 || rule === 8) return moverColor === 1;   
  return false;                                            
}
function exactFiveRule(moverColor) { return exactFiveFor(ruleForRender(), moverColor); }
function winAt(b, x, y, c, exactlyFive) {
  var dirs = [[1, 0], [0, 1], [1, 1], [1, -1]], d, n, sx, sy, k;
  for (d = 0; d < 4; d++) {
    n = 1;
    for (k = 1; k < N; k++) {
      sx = x + dirs[d][0] * k; sy = y + dirs[d][1] * k;
      if (sx < 0 || sy < 0 || sx >= N || sy >= N || b[sy][sx] !== c) break;
      n++;
    }
    for (k = 1; k < N; k++) {
      sx = x - dirs[d][0] * k; sy = y - dirs[d][1] * k;
      if (sx < 0 || sy < 0 || sx >= N || sy >= N || b[sy][sx] !== c) break;
      n++;
    }
    
    if (exactlyFive ? (n === 5) : (n >= 5)) return true;
  }
  return false;
}

function winCheck(b, x, y, c) { return winAt(b, x, y, c, exactFiveFor(ruleForRender(), c)); }

function findWinLine(b, rule) {
  var dirs = [[1, 0], [0, 1], [1, 1], [1, -1]], d, c, x, y, k, sx, sy, cells, n;
  for (d = 0; d < 4; d++) {
    var dx = dirs[d][0], dy = dirs[d][1];
    for (c = 1; c <= 2; c++) {
      for (y = 0; y < N; y++) {
        for (x = 0; x < N; x++) {
          if (b[y][x] !== c) continue;
          sx = x - dx; sy = y - dy;                     
          if (sx >= 0 && sy >= 0 && sx < N && sy < N && b[sy][sx] === c) continue;
          cells = [{ x: x, y: y }];
          for (k = 1; k < N; k++) {
            sx = x + dx * k; sy = y + dy * k;
            if (sx < 0 || sy < 0 || sx >= N || sy >= N || b[sy][sx] !== c) break;
            cells.push({ x: sx, y: sy });
          }
          n = cells.length;
          if (exactFiveFor(rule, c) ? (n === 5) : (n >= 5)) return { c: c, dir: [dx, dy], cells: cells };
        }
      }
    }
  }
  return null;
}

var FB_MARKS = null, FB_KEY = '';

function renjuRule() { var r = ruleForRender(); return r === 2 || r === 6 || r === 7 || r === 8; }

function forbiddenMarks() {
  if (RV_MODE || !renjuRule() || G.over) return [];
  var key = S.rule + '#' + boardFp();
  if (FB_MARKS && FB_KEY === key) return FB_MARKS;
  var out = [], b = G.board;
  for (var y = 0; y < N; y++) for (var x = 0; x < N; x++) {
    if (b[y][x]) continue;
    if (!vcxNearStone(b, x, y)) continue;
    if (vcxForbidden(b, x, y)) out.push([x, y]);
  }
  FB_MARKS = out; FB_KEY = key;
  return out;
}

function forbiddenMarksInvalidate() { FB_MARKS = null; FB_KEY = ''; }

function drawCross(ctx, cx, cy, e) {
  ctx.beginPath();
  ctx.moveTo(cx - e, cy - e); ctx.lineTo(cx + e, cy + e);
  ctx.moveTo(cx + e, cy - e); ctx.lineTo(cx - e, cy + e);
  ctx.stroke();
}

function forbiddenAt(x, y) {
  var mk = forbiddenMarks();
  for (var i = 0; i < mk.length; i++) if (mk[i][0] === x && mk[i][1] === y) return true;
  return false;
}

function drawForbiddenMarks(ctx, g) {
  var mk = forbiddenMarks();
  if (!mk.length) return;
  var pad = g.pad, gap = g.gap, e = Math.max(4, gap * 0.25);
  ctx.save();
  ctx.lineCap = 'round';
  ctx.globalAlpha = (curColor() === 1) ? 1 : 0.45;
  for (var i = 0; i < mk.length; i++) {
    var cx = pad + mk[i][0] * gap, cy = pad + mk[i][1] * gap;
    ctx.strokeStyle = 'rgba(255,255,255,.82)';      
    ctx.lineWidth = Math.max(3.4, gap * 0.15);
    drawCross(ctx, cx, cy, e);
    ctx.strokeStyle = '#e23b2e';                    
    ctx.lineWidth = Math.max(2.2, gap * 0.095);
    drawCross(ctx, cx, cy, e);
  }
  ctx.restore();
}

function coordName(x, y) { return String.fromCharCode(97 + x) + (N - y); }

function winPoints(b, c, rule) {
  var out = [], x, y, ex = exactFiveFor(rule, c);
  for (y = 0; y < N; y++) for (x = 0; x < N; x++) {
    if (b[y][x]) continue;
    if (winAt(b, x, y, c, ex)) out.push({ x: x, y: y });
  }
  return out;
}

function fourPoints(b, c, rule) {
  var out = [], x, y;
  for (y = 0; y < N; y++) for (x = 0; x < N; x++) {
    if (b[y][x] || !nearAnyStone(b, x, y)) continue;
    b[y][x] = c;
    var w = winPoints(b, c, rule);
    b[y][x] = 0;
    if (w.length) out.push({ x: x, y: y });
  }
  return out;
}

function openFourPoints(b, c, rule) {
  var out = [], x, y;
  for (y = 0; y < N; y++) for (x = 0; x < N; x++) {
    if (b[y][x] || !nearAnyStone(b, x, y)) continue;
    b[y][x] = c;
    var w = winPoints(b, c, rule);
    b[y][x] = 0;
    if (w.length >= 2) out.push({ x: x, y: y });
  }
  return out;
}

function nearAnyStone(b, x, y) {
  for (var dy = -4; dy <= 4; dy++) {
    var sy = y + dy;
    if (sy < 0 || sy >= N) continue;
    for (var dx = -4; dx <= 4; dx++) {
      var sx = x + dx;
      if (sx < 0 || sx >= N) continue;
      if (b[sy][sx]) return true;
    }
  }
  return false;
}

function mustAnswer(b, cc, cands, op, of, myFour) {
  
  if (findWinLine(b, engineRule())) return null;
  var i, j, w = winPoints(b, cc, engineRule());
  if (w.length) return { x: w[0].x, y: w[0].y, why: 'win' };            
  
  if (op && op.length === 1) {
    for (i = 0; i < cands.length; i++) {
      if (cands[i].x === op[0].x && cands[i].y === op[0].y) return null;
    }
    return { x: op[0].x, y: op[0].y, why: 'block5' };
  }
  if (of && of.length && !myFour) {                                      
    for (i = 0; i < cands.length; i++) {
      for (j = 0; j < of.length; j++) if (cands[i].x === of[j].x && cands[i].y === of[j].y) return null;
    }
    return { x: of[0].x, y: of[0].y, why: 'block4' };
  }
  return null;                                                           
}


var ANA_PAL = [
  { badge: 'rgba(0,170,178,.95)',  ring: 'rgba(255,255,255,.95)' },
  { badge: 'rgba(70,168,96,.95)',   ring: 'rgba(255,255,255,.95)' },
  { badge: 'rgba(150,172,40,.95)',  ring: 'rgba(255,255,255,.95)' },
  { badge: 'rgba(150,172,40,.95)',  ring: 'rgba(255,255,255,.95)' },
  { badge: 'rgba(150,172,40,.95)',  ring: 'rgba(255,255,255,.95)' },
];
var ANA_BAL = { badge: 'rgba(124,77,255,.95)', ring: 'rgba(255,255,255,.95)' };

function anaInlineMetrics(ctx, mk, mr) {
  var multi = String(mk.badge).length > 1;
  
  var rf = Math.max(7, mr * 0.5);
  var ef = Math.max(7, mr * 0.58);
  var efMin = Math.max(7, mr * 0.32);
  
  var lim = multi ? mr * 2.2 : mr * 1.7;
  ctx.font = '600 ' + ef + 'px "Segoe UI",system-ui,sans-serif';
  while (mk.label && ef > efMin && ctx.measureText(mk.label).width > lim) {
    ef -= 0.5;
    ctx.font = '600 ' + ef + 'px "Segoe UI",system-ui,sans-serif';
  }
  var bw = 0, lw = 0;
  ctx.font = '700 ' + rf + 'px "Segoe UI",system-ui,sans-serif';
  bw = ctx.measureText(mk.badge).width;
  if (mk.label) {
    ctx.font = '600 ' + ef + 'px "Segoe UI",system-ui,sans-serif';
    lw = ctx.measureText(mk.label).width;
  }
  var pw, ph;
  if (multi) { pw = Math.min(Math.max(bw, lw) + mr * 0.9, mr * 2.35); ph = mr * 1.9; }
  else { pw = mr * 2; ph = mr * 2; }
  return { multi: multi, rf: rf, ef: ef, pw: pw, ph: ph };
}

var HEAT_FAST_MS = 260;

function clearHeat() {
  G.heatGen++;
  G.heat = [];
  G.nums = [];
  G.think = [];
}

function clearCoach() {
  G.coachGen++;
  G.coach = [];
}

async function coachPass(gen, fp, ms) {
  try {
    
    var r = await analyze(ms, 8, 'main', curColor(), T('tagCoach'), { exclusive: true, noPonder: true });
    var cs = (r && r.candidates) || [];
    if (gen !== G.coachGen || fp !== boardFp()) return false;   
    if (!cs.length) return false;
    G.coach = cs.map(function (c, i) {
      return { x: c.x, y: c.y, tier: 1 + Math.floor(i * 4 / cs.length), ev: fmtEval(c.eval) };
    });
    return true;
  } catch (e) { return false; }
}

var COACH_MAX_MS = 8000;
async function refreshCoach() {
  
  if (linkSessionOn()) { clearCoach(); return; }
  
  var want = S.coach && !aiVsAi() && S.mode !== 'place' && S.mode !== 'endgame' &&
             !G.review && !RV_MODE && !G.over;
  if (!want) { clearCoach(); return; }
  if (isAiColor(curColor())) { clearCoach(); return; }        
  if (G.busy) return;                                       
  var gen = ++G.coachGen;
  var fp = boardFp();
  var got = await coachPass(gen, fp, HEAT_FAST_MS);
  if (gen !== G.coachGen) return;
  if (got) paint();
  if (gen !== G.coachGen || fp !== boardFp()) return;
  if (S.coach && !G.busy && !G.over && !isAiColor(curColor())) {
    var got2 = await coachPass(gen, fp, heatBudget());
    if (gen !== G.coachGen) return;
    if (got2) paint();
  }
}

var gAiGen = 0;
async function aiMove() {
  if (G.busy) return;
  if (G.moves.length === 0 && curColor() === 1) {
    var cx = Math.floor(G.size / 2), cy = Math.floor(G.size / 2);
    place(cx, cy, false, true);
    return;
  }
  var myGen = ++gAiGen;          
  G.busy = true;
  G.think = G.nums.length ? G.nums.map(function (n) { return { x: n.x, y: n.y }; }) : [];
  startPulse();
  setTurnPill(T('thinking'));
  
  try {
    var t0 = Date.now();                        
    
    var budget = aiTurnBudget();
    var wasRecon = !!G.reconBoost;                 
    if (wasRecon) { G.reconBoost = false; }
    var r = await analyze(budget, 1, null, curColor(), T('tagAiMove'),
                          { exclusive: true, urgent: true, noPonder: !S.bgThink || aiVsAi() });
    
    if (myGen !== gAiGen) { G.think = []; G.busy = false; refreshUI(); return; }
    
    if (S.paused) { G.think = []; G.busy = false; setTurnPill(T('paused')); refreshUI(); return; }
    var cs = (r && r.candidates) || [];
    if (!cs.length) { setStat(null, T('offline')); G.busy = false; refreshUI(); return; }
    var c0 = cs[0];
    c0.timeMs = Date.now() - t0;                
    var best = r.best || [c0.x, c0.y];
    G.lastStat = c0;
    var moverIsBlack = (G.moves.length % 2 === 0);
    pushCurve(G.moves.length, evalNum(c0.eval), moverIsBlack);
    G.redo = [];                            
    place(best[0], best[1], false, true);
    
    if (linkSessionOn() && S.linkAuto && !G.over) {
      adFireAt(best[0], best[1], linkBoardFp(G.board), false, false);
    }
    setStat(c0, null);
    
    clearHeat();
    clearCoach();
    if (settleMove()) { G.busy = false; paint(); bgThinkKick(180); return; }   
    paint();
  } catch (e) {
    setStat(null, T('offline'));
  }
  G.busy = false;
  refreshUI();
  
  if (S.coach && !G.over) refreshCoach().then(paint);
  chainAiIfNeeded();                          
}

function chainAiIfNeeded() {
  
  if (linkSessionOn()) return;
  
  if (G.sideSettle) return;
  if (RV_MODE || S.mode !== 'pve' || S.paused || G.review || G.over || G.busy) return;
  if (openActive()) return;                   
  if (!isAiColor(curColor())) return;
  setTimeout(function () {
    if (G.sideSettle) return;                 
    if (RV_MODE || S.mode !== 'pve' || S.paused || G.review || G.over || G.busy) return;
    if (!isAiColor(curColor())) return;
    aiMove();
  }, 220);
}

async function aiAssistOnce() {
  
  if (G.busy || G.review || G.over || openActive() ||
      S.mode === 'place' || S.mode === 'endgame') return;
  
  if (S.mode === 'pve' && isAiColor(curColor())) { aiMove(); return; }
  var myGen = ++gAiGen;          
  G.busy = true;
  G.think = G.nums.length ? G.nums.map(function (n) { return { x: n.x, y: n.y }; }) : [];
  startPulse();
  setTurnPill(T('thinking'));
  try {
    var t0 = Date.now();                        
    
    var ab = aiTurnBudget();                                  
    var r = await analyze(ab, 1, null, curColor(), T('tagAssist'), { exclusive: true, urgent: true });
    if (myGen !== gAiGen) { G.think = []; G.busy = false; refreshUI(); return; }   
    if (S.paused) { G.think = []; G.busy = false; setTurnPill(T('paused')); refreshUI(); return; }   
    var cs = (r && r.candidates) || [];
    if (!cs.length) { setStat(null, T('offline')); G.busy = false; refreshUI(); return; }
    var c0 = cs[0];
    c0.timeMs = Date.now() - t0;
    var best = r.best || [c0.x, c0.y];
    G.lastStat = c0;
    var moverIsBlack = (G.moves.length % 2 === 0);
    pushCurve(G.moves.length, evalNum(c0.eval), moverIsBlack);
    G.redo = [];
    place(best[0], best[1]);
    setStat(c0, null);
    clearHeat();                              
    clearCoach();
    if (settleMove()) { G.busy = false; paint(); bgThinkKick(180); return; }   
    paint();
  } catch (e) {
    setStat(null, T('offline'));
  }
  G.busy = false;
  refreshUI();
  if (S.coach && !G.over) refreshCoach().then(paint);   
}

function place(x, y, fromRedo, byAI) {
  if (x < 0 || y < 0 || x >= N || y >= N) return false;
  if (G.board[y][x]) return false;
  
  if (!fromRedo) G.redo = [];
  
  var c = (S.mode === 'endgame' && !S.egSeq) ? (S.egColor || 1) : curColor();
  G.board[y][x] = c;
  G.moves.push({ x: x, y: y, c: c });
  PV_PREVIEW = null;
  LIVE_AN.t0 = 0;
  
  G.heat = G.heat.filter(function (h) { return !G.board[h.y][h.x]; });
  
  G.ana.gen++;
  if (G.ana.defTimer) { clearTimeout(G.ana.defTimer); G.ana.defTimer = null; }
  G.ana.defWinUntil = 0; G.ana.defRefining = false;
  if (G.ana.kind === 'bal' && G.ana.marks.length) {
    if (!byAI) {
      G.ana.balLeft = (G.ana.balLeft || G.ana.marks.length) - 1;
      if (G.ana.balLeft <= 0) {
        G.ana.gen++; G.ana.busy = false;
        G.ana.marks = []; G.ana.rows = []; G.ana.defCells = null;
        G.ana.kind = '';
        anaMark(null); renderAna(T('anIdle'), true);
      }
    }
    G.ana.busy = false;
  } else {
    G.ana.busy = false; G.ana.kind = '';
    anaReset();
  }
  liveAnaBoardChanged();
  bgThinkKick(140);
  paint();
  
  if (G.fwd && G.fwd.on && !G.fwd.hold) fwdQueue();
  return true;
}

function hoverFromEvent(ev) {
  var rect = els.board.getBoundingClientRect();
  
  return cellAt(ev.clientX - rect.left, ev.clientY - rect.top);
}
function bindBoardHover() {
  els.board.addEventListener('mousemove', function (ev) {
    var h = hoverFromEvent(ev), c = G.hover;
    if (!h && !c) return;
    if (h && c && h.x === c.x && h.y === c.y) return;      
    G.hover = h; paint();
  });
  els.board.addEventListener('mouseleave', function () {
    if (!G.hover) return;
    G.hover = null; paint();
  });
}

function onBoardClick(ev) {
  var rect = els.board.getBoundingClientRect();
  var g = geom();
  var px = ev.clientX - rect.left, py = ev.clientY - rect.top;
  var hit = cellAt(px, py);
  if (!hit) return;
  var x = hit.x, y = hit.y;
  liveAnaTouch();

  if (G.review && G.review.kind === 'recite') { reciteStep(x, y); return; }
  if (G.review) return;                       
  if (G.busy) return;
  if (G.over) return;                         
  
  if (S.mode === 'pve' && aiVsAi()) { setStat(null, T('sideHintBoth')); return; }
  
  if (openActive()) {
    var cp = openCallPhase();
    if (cp) {
      if (cp.mode === 'call') openToggleCall(cp.set, cp.cap, x, y);
      else pickCallAt(cp.set, x, y);
      return;
    }
    if (openForbid(x, y)) { setStat(null, T('ofRegion')); paint(); return; }
  }
  
  if (S.mode === 'pve' && curColor() === 1 && renjuRule() && forbiddenAt(x, y)) {
    setStat(null, T('fbBlocked')); paint(); return;
  }
  if (!place(x, y)) return;
  G.redo = [];                                
  afterMove();
}

function settleMove() {
  var last = G.moves[G.moves.length - 1];
  if (last && winCheck(G.board, last.x, last.y, last.c)) {
    G.over = true;
    G.think = [];
    clearHeat();      
    setStat(G.lastStat, T('over') + ' · ' + winTextFor(last.c));
    persistGame();
    refreshUI();
    return true;
  }
  persistGame();
  return false;
}

function winTextFor(winColor) {
  var my = myColor();
  if (my === 1 || my === 2) return (winColor === my) ? T('youWin') : T('youLose');
  return (winColor === 1 ? T('fwdBlack') : T('fwdWhite')) + ' ' + T('youWin');
}

function afterMove() {
  clearCoach();   
  if (settleMove()) { paint(); return; }
  refreshUI();
  G.retryLeft = null;        
  if (openActive() && openStep()) { paint(); return; }   
  if (S.paused) return;
  
  if (boardDecided() && (G.ana.marks.length || G.ana.defCells)) {
    if (G.ana.kind === 'bal' && G.ana.marks.length) {
      // Balance picks persist like the Link overlay. Only the user's own
      // stones count them down (place() with byAI=false); AI auto-moves
      // never clear the marks.
      G.ana.busy = false;
      anaMark(null);
    } else {
      G.ana.gen++; G.ana.busy = false;
      if (G.ana.defTimer) { clearTimeout(G.ana.defTimer); G.ana.defTimer = null; }
      G.ana.autoDefend = false;
      G.ana.marks = []; G.ana.rows = []; G.ana.defCells = null;
      anaMark(null); renderAna(T('anIdle'), true);
    }
  }
  if (S.mode === 'place' || S.mode === 'endgame') {
    paint(); return;
  }   
  
  if (G.ana.autoDefend && !G.ana.busy && !G.over && !boardDecided() &&
      !isAiColor(curColor())) { try { anaDefend(true); } catch (e) {} }
  if (isAiColor(curColor())) aiMove();        
}

function persistGame() {
  try {
    var mv = G.moves.map(function (m) { return [m.x, m.y, m.c]; });
    localStorage.setItem('gbcalc.game.v1', JSON.stringify({ moves: mv, rule: S.rule,
      side: S.side, aiB: S.aiB, aiW: S.aiW, mode: S.mode, ts: Date.now() }));
  } catch (e) {}
}

function restoreGame() {
  var o = null;
  try { o = JSON.parse(localStorage.getItem('gbcalc.game.v1') || 'null'); } catch (e) { o = null; }
  if (!o || !Array.isArray(o.moves) || !o.moves.length) return false;
  G.board = newBoard(); G.moves = []; G.curve = []; G.heat = []; G.nums = []; G.coach = [];
  o.moves.forEach(function (a) {
    var c = a[2] || ((G.moves.length % 2 === 0) ? 1 : 2);
    if (a[0] < 0 || a[1] < 0 || a[0] >= N || a[1] >= N) return;
    if (G.board[a[1]][a[0]]) return;
    G.board[a[1]][a[0]] = c;
    G.moves.push({ x: a[0], y: a[1], c: c });
  });
  recomputeOver();
  paint();
  refreshCoach();                             
  return true;
}

function recomputeOver() {
  var over = false;
  for (var i = G.moves.length - 1; i >= 0 && !over; i--) {
    var m = G.moves[i];
    if (winCheck(G.board, m.x, m.y, m.c)) over = true;
  }
  G.over = over;
  return over;
}

var OPEN = null;

function cheb(x, y) { return Math.max(Math.abs(x - 7), Math.abs(y - 7)); }

function tarRegion(n) { return [0, 1, 2, 3, 4][n - 1]; }
function symTransforms(x, y) {
  var cx = x - 7, cy = y - 7;
  return [[cx, cy], [-cx, cy], [cx, -cy], [-cx, -cy],
          [cy, cx], [-cy, cx], [cy, -cx], [-cy, -cx]].map(function (p) {
    return [p[0] + 7, p[1] + 7];
  });
}
function symEquivInSet(p, set) {
  var tr = symTransforms(p[0], p[1]);
  for (var i = 0; i < set.length; i++) {
    for (var j = 0; j < tr.length; j++) {
      if (tr[j][0] === set[i][0] && tr[j][1] === set[i][1]) return true;
    }
  }
  return false;
}
function openActive() { return !!OPEN && !OPEN.finished; }

function openStart() {
  OPEN = null;
  els.btn_ten.hidden = true;
  if (els.btn_balcall) els.btn_balcall.hidden = true;
  els.openBar.hidden = true;
  if (els.swapPop) els.swapPop.hidden = true;   
  
  if (S.mode === 'endgame') return;
  if (S.egLocked) return;              
  
  if (S.rule === 5) OPEN = { kind: 'swap1' };
  else if (S.rule === 6) OPEN = { kind: 'yama', yama: null };
  else if (S.rule === 7) OPEN = { kind: 'tar', tar: null };
  else if (S.rule === 8) OPEN = { kind: 'tara' };
  if (OPEN && OPEN.kind === 'tar' && !G.moves.length) {
    place(7, 7);                       
    afterMove();
  } else {
    openHint();
  }
}
function openHint() {
  if (!openActive() || RV_MODE) {
    els.openBar.hidden = true;
    if (els.yamaDecl) els.yamaDecl.hidden = true;
    if (els.taraSwap) els.taraSwap.hidden = true;
    return;
  }
  
  if (OPEN.choice) {
    els.openBar.hidden = true; els.btn_ten.hidden = true;
    if (els.yamaDecl) els.yamaDecl.hidden = true;
    if (els.taraSwap) els.taraSwap.hidden = true;
    return;
  }
  var n = G.moves.length, t = '', en = (S.lang === 'en');
  if (OPEN.kind === 'swap1') {
    t = en ? 'Swap1: waiting for the reply side to decide' : '一手交换：等待对方决定是否换色';
  } else if (OPEN.kind === 'yama') {
    var y = OPEN.yama;
    if (!y || y.phase === 'declare') {
      t = en ? 'Yamaguchi: black declares the number of 5th-move picks (1–10)'
             : '山口：黑方声明第5手打点数（点右侧数字）';
    } else if (y.phase === 'swap') {
      t = en ? 'Yamaguchi: waiting for the swap decision' : '山口：等待换色决定';
    } else if (n === 3) {
      t = en ? 'Yamaguchi: white plays the 4th move anywhere'
             : '山口：白第4手（任意位置，官方无区域限制）';
    } else if (y.phase === 'call' && n >= 4) {
      t = en ? ('Yamaguchi: black names ' + (y.n - y.called.length) + ' more 5th-move pick(s) (' + y.called.length + '/' + y.n + ')')
             : ('山口：黑方叫第5手候选（已叫 ' + y.called.length + '/' + y.n + '，对称点不可重复）');
    } else if (y.phase === 'pick') {
      t = en ? 'Yamaguchi: white picks one of the offered 5th moves'
             : '山口：白方从候选中挑一个作为第5手';
    } else {
      t = en ? 'Yamaguchi: play on' : '山口：正常对局';
    }
  } else if (OPEN.kind === 'tar') {
    if (OPEN.tar && OPEN.tar.phase === 'call10') {
      t = en ? ('Ten-call: pick ' + (10 - OPEN.tar.called.length) + ' more candidate(s)')
             : ('十打：还需叫 ' + (10 - OPEN.tar.called.length) + ' 个候选点（对称点不可重复）');
    } else if (OPEN.tar && OPEN.tar.phase === 'pickCall') {
      t = en ? 'Ten-call: white picks one of the offered 5th moves' : '十打：白方正在挑点';
    } else if (n < 4) {
      var r = tarRegion(n + 1);
      t = en ? ('Taraguchi: move ' + (n + 1) + ' within ' + (r * 2 + 1) + '×' + (r * 2 + 1) + ' of centre')
             : ('塔拉山口：第 ' + (n + 1) + ' 手限天元周围 ' + (r * 2 + 1) + '×' + (r * 2 + 1));
    } else if (n === 4) {
      t = en ? 'Taraguchi: play the 5th stone, or use Ten-call'
             : '塔拉山口：直接下第 5 手（9×9 内），或点「十打」';
    }
  } else if (OPEN.kind === 'tara') {
    if (OPEN.phase === 'swapask') {
      t = en ? 'Tarannikov: swap sides now?' : '塔拉尼科夫：是否换手？（每手后均有此权）';
    } else if (n < 5) {
      var r2 = tarRegion(n + 1);
      t = en ? ('Tarannikov: move ' + (n + 1) + ' within ' + (r2 * 2 + 1) + '×' + (r2 * 2 + 1) + ' of centre')
             : ('塔拉尼科夫：第 ' + (n + 1) + ' 手限天元周围 ' + (r2 * 2 + 1) + '×' + (r2 * 2 + 1));
    } else if (n === 5) {
      t = en ? 'Tarannikov: white plays the 5th move, last swap option follows'
             : '塔拉尼科夫：白下第 5 手（9×9 内），之后仍有最后一次换手机会';
    }
  }
  
  var declOn = !!(OPEN.kind === 'yama' && OPEN.yama && OPEN.yama.phase === 'declare');
  
  var swapOn = !!(OPEN.kind === 'tara' && OPEN.phase === 'swapask' && S.mode === 'pve' &&
                  ((curColor() === 1) ? !S.aiB : !S.aiW));
  if (els.yamaDecl) els.yamaDecl.hidden = !declOn;
  if (els.taraSwap) els.taraSwap.hidden = !swapOn;
  
  var cp = openCallPhase();
  var balOk = !!(cp && cp.mode === 'call' && cp.cap - cp.set.called.length > 0);
  if (els.btn_balcall) els.btn_balcall.hidden = !balOk;
  if (cp && cp.set.balBusy) t = T('balCallBusy');
  els.openHint.textContent = t;
  els.openBar.hidden = !t;
  els.btn_ten.hidden = !(OPEN.kind === 'tar' && n === 4 && !(OPEN.tar && OPEN.tar.phase));
}

function openStep() {
  if (!openActive()) return false;
  var n = G.moves.length;
  if (OPEN.kind === 'swap1') {
    if (n === 1 && !OPEN.done) { showSwapChoice(); return true; }
    OPEN.finished = true; openHint(); return false;
  }
  
  if (OPEN.kind === 'yama') {
    var y = OPEN.yama;
    if (n < 3) { openHint(); return true; }                    
    if (!y) { OPEN.yama = { phase: 'declare', n: 0, called: [] }; openHint(); return true; }
    if (y.phase === 'declare') { openHint(); return true; }    
    if (y.phase === 'swap') { openHint(); return true; }       
    if (!OPEN.done) { showSwapChoice(); return true; }         
    if (n === 3) { y.phase = 'call'; openHint(); return true; }  
    if (n === 4) {                                             
      if (y.phase === 'pick' && !OPEN.picked && aiShouldPickCall()) { aiPickCall(); return true; }
      openHint(); return true;
    }
    if (n >= 5) { y.phase = 'done'; OPEN.finished = true; openHint(); return false; }
    openHint(); return true;
  }
  if (OPEN.kind === 'tar') {
    if (OPEN.tar && OPEN.tar.phase === 'call10') { openHint(); return true; }
    if (n >= 6) { OPEN.finished = true; openHint(); return false; }
    
    if (n === 4 && OPEN.tar && OPEN.tar.phase === 'pickCall') {
      if (aiShouldPickCall()) { aiPickCall(); return true; }   
      openHint(); return true;                                 
    }
    if (n === 5 && !OPEN.done) { showSwapChoice(); return true; }
    if (n === 5 && OPEN.done) { OPEN.finished = true; openHint(); return false; }
    openHint(); return true;
  }
  
  if (OPEN.kind === 'tara') {
    if (n >= 6) { OPEN.finished = true; openHint(); return false; }
    if (n <= 5 && OPEN.askedN !== n) {         
      OPEN.askedN = n; OPEN.phase = 'swapask';
      askTaraSwap(); return true;              
    }
    openHint(); return true;
  }
  return false;
}

function openForbid(x, y) {
  if (!openActive()) return false;
  var n = G.moves.length + 1;                 
  if (OPEN.kind === 'tar' || OPEN.kind === 'tara') {
    if (n > 5) return false;
    return cheb(x, y) > tarRegion(n);
  }
  if (OPEN.kind === 'yama') {
    if (n > 3) return false;
    return cheb(x, y) > tarRegion(n);
  }
  return false;
}

function showSwapChoice() {
  OPEN.choice = true;
  if (RV_MODE || S.mode !== 'pve') {          
    chooseSwapSide((curColor() === 1) ? 'b' : 'w', true);
    return;
  }
  els.swapPop.hidden = false;
  openHint(); paint();
}

function chooseSwapSide(side, silent) {
  
  setAiSides(side !== 'b', side !== 'w');
  syncSideUI(); save();
  if (els.swapPop) els.swapPop.hidden = true;
  OPEN.choice = false;
  OPEN.done = true;
  openStep();                                 
  refreshUI();
  if (!silent) paint();
  refreshCoach();
  maybeAi();                                  
}

function aiShouldPickCall() {                 
  return S.mode === 'pve' && !!S.aiW;
}
async function aiPickCall() {
  var set = (OPEN.kind === 'tar') ? OPEN.tar : OPEN.yama;
  var pts = set.called.slice();
  if (!pts.length) { OPEN.finished = true; return; }
  var pick = pts[0], bestV = null;
  for (var i = 0; i < pts.length; i++) {
    
    var b2 = G.board.map(function (row) { return row.slice(); });
    b2[pts[i][1]][pts[i][0]] = curColor();
    var r = null;
    
    try { r = await analyze(Math.max(400, Math.min(900, S.turnMs)), 1, 'sub', 3 - curColor(), T('tagTen'), { board: b2 }); } catch (e) {}
    var cs = (r && r.candidates) || [];
    var v = cs.length ? evalNum(cs[0].eval) : 0;
    
    if (bestV === null || v > bestV) { bestV = v; pick = pts[i]; }
  }
  OPEN.picked = true;
  set.phase = 'done';
  place(pick[0], pick[1]);
  afterMove();
}
function startTenCall() {
  OPEN.tar = { phase: 'call10', called: [] };
  openHint(); paint();
}

function openToggleCall(set, cap, x, y) {
  var i, found = -1;
  for (i = 0; i < set.called.length; i++) {
    if (set.called[i][0] === x && set.called[i][1] === y) { found = i; break; }
  }
  if (found >= 0) { set.called.splice(found, 1); }
  else if (set.called.length >= cap) { return; }
  else if (symEquivInSet([x, y], set.called)) { return; }                    
  else if (renjuRule() && curColor() === 1 && forbiddenAt(x, y)) { return; } 
  else { set.called.push([x, y]); }
  if (set.called.length >= cap) { set.phase = 'pick'; openStep(); }
  openHint(); paint();
}

function pickCallAt(set, x, y) {
  for (var i = 0; i < set.called.length; i++) {
    if (set.called[i][0] === x && set.called[i][1] === y) {
      set.phase = 'done'; OPEN.picked = true;
      place(x, y);
      afterMove();
      return;
    }
  }
}

function openCallPhase() {
  if (!openActive()) return null;
  if (OPEN.kind === 'tar' && OPEN.tar) {
    if (OPEN.tar.phase === 'call10') return { set: OPEN.tar, cap: 10, mode: 'call' };
    if (OPEN.tar.phase === 'pickCall') return { set: OPEN.tar, mode: 'pick' };
  }
  if (OPEN.kind === 'yama' && OPEN.yama) {
    if (OPEN.yama.phase === 'call' && G.moves.length >= 4)
      return { set: OPEN.yama, cap: OPEN.yama.n, mode: 'call' };
    if (OPEN.yama.phase === 'pick') return { set: OPEN.yama, mode: 'pick' };
  }
  return null;
}

function declareYamaN(k) {
  if (!openActive() || OPEN.kind !== 'yama' || !OPEN.yama || OPEN.yama.phase !== 'declare') return;
  OPEN.yama.n = Math.max(1, Math.min(10, k));
  OPEN.yama.phase = 'swap';
  openHint();
  showSwapChoice();                            
}

function askTaraSwap() {
  if (S.mode !== 'pve' || (!S.aiB && !S.aiW)) { OPEN.phase = null; openHint(); return; }
  var turn = curColor();                       
  var aiTurn = (turn === 1) ? !!S.aiB : !!S.aiW;
  openHint(); paint();
  if (!aiTurn) return;                         
  analyze(800, 1, 'sub', turn, T('tagTara')).then(function (r) {
    if (!openActive() || OPEN.phase !== 'swapask') return;
    var cs = (r && r.candidates) || [];
    var v = cs.length ? evalNum(cs[0].eval) : 0;
    if (v < -200) { doTaraSwap(true); return; }
    OPEN.phase = null; openHint(); refreshUI(); maybeAi();     
  }).catch(function () {
    if (!openActive() || OPEN.phase !== 'swapask') return;
    OPEN.phase = null; openHint(); refreshUI(); maybeAi();
  });
}

function doTaraSwap(byAi) {
  if (S.mode === 'pve') { setAiSides(!S.aiB, !S.aiW); syncSideUI(); save(); }
  OPEN.phase = null;
  toast(byAi ? T('taraSwapAi') : T('taraSwapped'));
  refreshUI(); openHint(); paint();
  maybeAi();                                   
}

async function balCallTen() {
  if (!openActive()) return;     
  var cp = openCallPhase();
  if (!cp || cp.mode !== 'call' || cp.set.balBusy) return;
  var need = cp.cap - cp.set.called.length;
  if (need <= 0) return;
  var t = cp.set, turnMs = Math.max(2000, S.turnMs || 2000);
  t.balBusy = true;
  openHint(); paint();
  try {
    var balBoard = G.board.map(function (r) { return r.slice(); });
    var balSide = curColor();
    if (!G.moves.length) {
      var c0 = (N - 1) >> 1;
      if (!balBoard[c0][c0]) { balBoard[c0][c0] = 1; balSide = 2; }
    }
    var j = await apiBalance({
      board: balBoard, moveList: [], n: Math.max(1, Math.ceil(need / 2)),
      mode: 'two', rule: engineRule(),
      turnMs: turnMs, lane: 'main', cid: 'calc-baltar-' + G.moves.length, side: balSide,
    }, need * turnMs + 30000);
    if (!openActive() || openCallPhase() === null || openCallPhase().set !== t) return;
    if (j && j.swap) { toast(T('balSwapNow')); return; }
    var pts = (j && j.moves) || [];
    var added = 0, names = [];
    for (var i = 0; i < pts.length && t.called.length < cp.cap; i++) {
      var p = pts[i];
      if (!G.board[p[1]] || G.board[p[1]][p[0]]) continue;    
      if (symEquivInSet(p, t.called)) continue;               
      if (renjuRule() && curColor() === 1 && forbiddenAt(p[0], p[1])) continue;
      t.called.push([p[0], p[1]]);
      names.push(coordName(p[0], p[1]));
      added++;
    }
    if (t.called.length >= cp.cap) { t.phase = 'pick'; openStep(); }
    toast(T('balCallDone').replace('{n}', String(added))
          .replace('{list}', names.join(', ') || '—'));
  } catch (e) {
    if (openActive() && openCallPhase() && openCallPhase().set === t)
      toast(T('balCallFail') + (e && e.message ? e.message : 'unknown'));
  } finally {
    if (OPEN && t) { t.balBusy = false; openHint(); paint(); }
  }
}

function syncSideUI() {
  var map = { b: !!S.aiB, w: !!S.aiW };
  [].forEach.call(els.seg_side.querySelectorAll('button'), function (b) {
    var key = b.getAttribute('data-ai');
    b.classList.toggle('on', !!map[key]);
    b.setAttribute('aria-pressed', map[key] ? 'true' : 'false');
  });
}

var LIVE_LANES = {};
function onEngineLive(d) {
  if (!d || !d.lane) return;
  if (d.searching === false) {
    
    if (!(LIVE_AN.on && d.lane === 'main')) delete LIVE_LANES[d.lane];
    paintLiveDash(); return;
  }
  LIVE_LANES[d.lane] = {
    t0: d.t0 || Date.now(), speed: +d.speed || 0, nodes: +d.nodes || 0,
    depth: +d.depth || 0, eval: d.eval || '', tag: d.tag || '', best: d.best || null,
  };
  
  if (!RV_MODE && G.ana.busy && G.ana.kind === 'calc' && d.lane === 'main' &&
      d.pvs && d.pvs.length && d.pvs[0] && d.pvs[0].x != null) {
    var ap0 = d.pvs[0];
    G.ana.marks = [{ x: ap0.x, y: ap0.y, badge: '1', label: fmtEval(ap0.eval), tier: 0 }];
    G.ana.rows = [{ i: 0, badge: '1', coord: coordName(ap0.x, ap0.y), eval: fmtEval(ap0.eval), pv: pvText(ap0.line) }];
    paint();
  }
  paintLiveDash();
}
function liveAgg() {
  var keys = Object.keys(LIVE_LANES);
  if (!keys.length) return null;
  var a = { speed: 0, nodes: 0, depth: 0, t0: Infinity, tag: '', n: 0 };
  for (var i = 0; i < keys.length; i++) {
    var L = LIVE_LANES[keys[i]];
    a.speed += L.speed || 0; a.nodes += L.nodes || 0; a.n++;
    if ((L.depth || 0) > a.depth) a.depth = L.depth || 0;
    if ((L.t0 || Infinity) < a.t0) a.t0 = L.t0;
    if (!a.tag && L.tag) a.tag = L.tag;
  }
  return a;
}

function fmtSpeed(nps) {
  if (!nps || nps < 0) return '-';
  if (nps >= 1e7) return (nps / 1e6).toFixed(3) + ' M';
  return String(Math.round(nps));      
}
function fmtNodes(n) { return fmtSpeed(n); }   

function paintLiveDash() {
  if (RV_MODE) return;
  var a = liveAgg();
  var busyUI = !!(a || G.busy || G.ana.busy || (G.fwd && G.fwd.busy) || (LIVE_AN.on && LIVE_AN.t0));
  
  if (busyUI && a && a.tag) engMsgAdd(T('calcOn') + ' · ' + a.tag);
  if (!a) return;
  if (a.depth) els.st_depth.textContent = String(a.depth);
  els.st_speed.textContent = fmtSpeed(a.speed);
  els.st_nodes.textContent = fmtNodes(a.nodes);
  
  var tShow = a.t0 || (LIVE_AN.on ? LIVE_AN.t0 : 0) || Date.now();
  els.st_time.textContent = ((Date.now() - tShow) / 1000).toFixed(1) + ' s';
  if (a.tag) engMsgAdd(T('calcOn') + ' · ' + a.tag);
  
  if (linkSessionOn()) {
    tellHost({ type: 'linkStats',
      depth: els.st_depth.textContent, speed: els.st_speed.textContent,
      nodes: els.st_nodes.textContent, time: els.st_time.textContent,
      eval: els.st_eval.textContent, best: els.st_best.textContent });
  }
}



var ENG_MSG_MAX = 200;
function engMsgAdd(msg) {
  var box = els.st_line;
  if (!box || msg == null) return;
  msg = String(msg);
  if (!msg) return;
  var last = box.lastElementChild;
  if (last && last.textContent === msg) return;
  var wrap = box.parentNode;
  var atBottom = true;
  if (wrap) atBottom = (wrap.scrollHeight - wrap.scrollTop - wrap.clientHeight < 28);
  var d = document.createElement('div');
  d.className = 'em-line';
  d.textContent = msg;
  box.appendChild(d);
  while (box.children.length > ENG_MSG_MAX) box.removeChild(box.firstElementChild);
  if (wrap && wrap.classList && wrap.classList.contains('eng-msg') && atBottom)
    wrap.scrollTop = wrap.scrollHeight;
}
function engMsgClear() { if (els.st_line) els.st_line.innerHTML = ''; }

var PV_HOVER = null, PV_SIDE = 0, PV_PREVIEW = null;
var PV_RENDER = [], PV_KEY = '', PV_TMR = 0, PV_LAST = 0, PV_PENDING = null;
var PV_MIN_MS = 260;

function pvPreviewClear() { PV_PREVIEW = null; }

function pvMarkSel(box) {
  var kids = box.querySelectorAll('.pv-mv');
  for (var i = 0; i < kids.length; i++) kids[i].classList.remove('sel');
  if (!PV_PREVIEW || !PV_PREVIEW.moves.length) return;
  var k = PV_PREVIEW.moves.length - 1;
  var last = PV_PREVIEW.moves[k], el = kids[k];
  if (el && +el.getAttribute('data-x') === last[0] && +el.getAttribute('data-y') === last[1])
    el.classList.add('sel');
}

function pvDrawNow() {
  var box = els.pvBox;
  if (!box) return;
  PV_RENDER = PV_PENDING || [];
  PV_KEY = PV_RENDER.map(function (p) { return p[0] + ',' + p[1]; }).join(' ');
  if (!PV_RENDER.length) {
    box.hidden = true; box.innerHTML = ''; PV_RENDER = [];
    return;
  }
  box.hidden = false;
  box.innerHTML = PV_RENDER.map(function (p, i) {
    return '<span class="pv-mv' + (i === 0 ? ' best' : '') + '" data-x="' + p[0] + '" data-y="' + p[1] +
           '" data-i="' + i + '" data-seq="' + (i + 1) + '">' + escHtml(coordName(p[0], p[1])) + '</span>';
  }).join('');
  pvMarkSel(box);
  bindPvBox(box);
  if (PV_HOVER) {
    var hj = -1;
    for (var hk = 0; hk < PV_RENDER.length; hk++)
      if (PV_RENDER[hk][0] === PV_HOVER.x && PV_RENDER[hk][1] === PV_HOVER.y) { hj = hk; break; }
    if (hj < 0) { PV_HOVER = null; paint(); }
    else if (hj !== PV_HOVER.i) { PV_HOVER.i = hj; PV_HOVER.seq = hj + 1; paint(); }
  }
  var wrap = box.parentNode;
  if (wrap && wrap.classList && wrap.classList.contains('eng-msg') &&
      wrap.scrollHeight - wrap.scrollTop - wrap.clientHeight < 28)
    wrap.scrollTop = wrap.scrollHeight;
}

function bindPvBox(box) {
  if (box.__pvBound) return;
  box.__pvBound = 1;
  box.addEventListener('mouseover', function (e) {
    var t = e.target && e.target.closest ? e.target.closest('.pv-mv') : null;
    if (!t) return;
    PV_HOVER = { i: +t.getAttribute('data-i'), x: +t.getAttribute('data-x'), y: +t.getAttribute('data-y'), seq: +t.getAttribute('data-seq') };
    paint();
  });
  box.addEventListener('mouseout', function (e) {
    var t = e.target && e.target.closest ? e.target.closest('.pv-mv') : null;
    if (!t) return;
    PV_HOVER = null;
    paint();
  });
  box.addEventListener('click', function (e) {
    var t = e.target && e.target.closest ? e.target.closest('.pv-mv') : null;
    if (!t) return;
    var i = +t.getAttribute('data-i');
    var p = PV_RENDER[i];
    if (!p) return;
    var same = PV_PREVIEW && PV_PREVIEW.moves.length === i + 1 &&
               PV_PREVIEW.moves[i][0] === p[0] && PV_PREVIEW.moves[i][1] === p[1];
    if (same) {
      PV_PREVIEW = null;
    } else {
      PV_PREVIEW = {
        moves: PV_RENDER.slice(0, i + 1).map(function (q) { return [q[0], q[1]]; }),
        side: (PV_SIDE === 1 || PV_SIDE === 2) ? PV_SIDE : curColor(),
      };
    }
    pvMarkSel(box);
    paint();
  });
}

function setPvLine(line, side) {
  if (side === 1 || side === 2) PV_SIDE = side;
  var arr = (line || []).slice(0, 30).map(function (p) { return [p[0] | 0, p[1] | 0]; });
  PV_PENDING = arr;
  var box = els.pvBox;
  if (!box) return;
  if (!arr.length) {
    if (PV_TMR) { clearTimeout(PV_TMR); PV_TMR = 0; }
    PV_LAST = Date.now();
    var hadPv = !!PV_PREVIEW, hadHover = !!PV_HOVER;
    PV_HOVER = null;
    pvPreviewClear();
    pvDrawNow();
    if (hadPv || hadHover) paint();
    return;
  }
  var key = arr.map(function (p) { return p[0] + ',' + p[1]; }).join(' ');
  if (key === PV_KEY && !box.hidden) return;
  var wait = PV_MIN_MS - (Date.now() - PV_LAST);
  if (wait <= 0) {
    PV_LAST = Date.now();
    pvDrawNow();
  } else if (!PV_TMR) {
    PV_TMR = setTimeout(function () { PV_TMR = 0; PV_LAST = Date.now(); pvDrawNow(); }, wait);
  }
}

function evalBlack(c0) {
  var v = evalNum(c0 && c0.eval);
  if (!isFinite(v)) return null;
  return (c0.side === 2) ? -v : v;
}
function scoreTxt(v) {
  if (v == null || !isFinite(v)) return '-';
  if (Math.abs(v) > RAPFI_EVAL_MAX) {
    var mv = Math.round(RAPFI_MATE - Math.abs(v));
    return (v > 0 ? '+M' : '-M') + mv;
  }
  return (v > 0 ? '+' : '') + Math.round(v);
}
function scoreWin(v) {
  if (v == null || !isFinite(v)) return null;
  if (Math.abs(v) > RAPFI_EVAL_MAX) return v > 0 ? 1 : 0;
  return 1 / (1 + Math.exp(-v / 200));
}


function evalViewColor(sideMove) {
  if (aiVsAi()) return (sideMove === 1 || sideMove === 2) ? sideMove : curColor();
  if (!!S.aiB === !!S.aiW) return 0;
  return S.aiB ? 1 : 2;
}
function evalToView(ev, sideMove, c) {
  var b = evalBlack({ eval: ev, side: sideMove });
  if (b == null || !isFinite(b)) return null;
  return (c === 2) ? -b : b;
}
function dashEvalSet(ev, sideMove) {
  if (!els.st_eval) return;
  var c = evalViewColor(sideMove);
  var v = evalToView(ev, sideMove, c);
  els.st_eval.textContent = (v == null) ? '-' : scoreTxt(v);
  if (els.t_eval) {
    var suf = (c === 1) ? '·黑' : (c === 2 ? '·白' : '');
    if (S.lang === 'en') suf = (c === 1) ? ' ·B' : (c === 2 ? ' ·W' : '');
    els.t_eval.textContent = T('eval') + suf;
    var who = aiVsAi() ? '行棋方' : 'AI';
    els.t_eval.title = (c === 0) ? '' : (who + '执' + (c === 1 ? '黑' : '白') + '（正值 = 该方有利）');
  }
}
function dashEvalRefresh() {
  var st = G.lastStat;
  if (st && st.eval != null) dashEvalSet(st.eval, st.side);
  else dashEvalSet(null, 0);
}

function setStat(c0, msg) {
  if (c0) {
    els.st_depth.textContent = c0.depth != null ? c0.depth : '-';
    
    els.st_speed.textContent = c0.speed != null ? fmtSpeed(+c0.speed) : '-';
    els.st_nodes.textContent = c0.nodes != null ? fmtNodes(+c0.nodes) : '-';
    
    els.st_time.textContent = (c0.timeMs != null)
      ? (c0.timeMs / 1000).toFixed(3) + ' s' : '-';
    dashEvalSet(c0.eval, c0.side);
    els.st_best.textContent = (c0.x != null) ? coordName(c0.x, c0.y) : '-';
    setPvLine(c0.line || [], (c0.side === 1 || c0.side === 2) ? c0.side : 0);
    
    if (linkSessionOn()) {
      tellHost({ type: 'linkStats',
        depth: els.st_depth.textContent, speed: els.st_speed.textContent,
        nodes: els.st_nodes.textContent, time: els.st_time.textContent,
        eval: els.st_eval.textContent, best: els.st_best.textContent });
    }
  } else {
    engMsgAdd(msg);
  }
}

function setTurnPill(txt) { if (els.turnPill) els.turnPill.textContent = txt || ''; }

function refreshUI() {
  
  if (els.btn_link) els.btn_link.textContent = (S.lang === 'en') ? 'Link' : '连线';
  if (els.btn_vis) els.btn_vis.textContent = (S.lang === 'en') ? 'Image' : '识图';
  setLangBtn();                        
  try { bgThinkHintSync(); } catch (eBH) {}
  
  if (RV_MODE) { refreshRvUI(); return; }
  var c = curColor();
  if (G.review) {
    setTurnPill(G.review.kind === 'recite' ? T('recite') : T('replay'));
  } else if (S.paused) {
    setTurnPill(T('paused'));
  } else if (G.over) {
    setTurnPill(T('over'));
  } else if (S.mode === 'place') {
    setTurnPill((c === 1 ? T('turnBlack') : T('turnWhite')) + ' · ' + T('modePlace'));
  } else if (S.mode === 'endgame') {
    
    var egTxt = S.egSeq ? T('egSeq')
      : T('egFreeOn').replace('{c}', S.egColor === 1 ? T('egB') : T('egW'));
    setTurnPill((c === 1 ? T('turnBlack') : T('turnWhite')) + ' · ' + T('modeEndgame') +
                ' · ' + egTxt);
  } else {
    
    var colorName = (c === 1 ? T('fwdBlack') : T('fwdWhite'));
    if (aiVsAi()) setTurnPill(T('turnAiBoth').replace('{c}', colorName));
    else if (humanBoth()) setTurnPill(T('turnYouBoth').replace('{c}', colorName));
    else setTurnPill(isAiColor(c) ? (G.engineOffline ? T('offlinePill') : T('thinking')) : T('turnYou'));
  }
  
  var aiRunning = G.busy || (!S.paused && !G.over && !G.review &&
                             S.mode === 'pve' && isAiColor(curColor()));
  els.btn_pause.textContent = (S.paused || !aiRunning) ? '▶' : '❚❚';
  
  els.btn_pause.title = S.paused
    ? (aiVsAi() ? T('spResume') : T('resume'))
    : (aiVsAi() ? T('spPause') : (aiRunning ? T('pause') : T('assistHint')));
  
  els.btn_pause.disabled = (S.mode === 'place' || S.mode === 'endgame');
  
  if (els.btn_live) liveAnaUI();
  
  if (els.sideHint) {
    els.sideHint.textContent = aiVsAi()
      ? (S.mode === 'pve' ? T('sideHintBoth') : T('sideHintBothPlace'))
      : humanBoth() ? T('sideHintNone')
      : (S.aiB ? T('sideHintB') : T('sideHintW'));
  }
  
  if (els.egBar) {
    var egOn = (S.mode === 'endgame');
    els.egBar.hidden = !egOn;
    if (egOn) {
      
      els.btn_eg_ok.textContent = S.egLocked ? T('egOkOn') : T('egOk');
      els.btn_eg_ok.classList.toggle('eg-ok', !!S.egLocked);
      els.btn_eg_seq.classList.toggle('eg-on', !!S.egSeq);
      els.btn_eg_free.classList.toggle('eg-on', !S.egSeq);
      if (els.egKeys) els.egKeys.hidden = !!S.egSeq;      
      els.btn_eg_b.classList.toggle('sel-b', !S.egSeq && S.egColor === 1);
      els.btn_eg_w.classList.toggle('sel-w', !S.egSeq && S.egColor === 2);
    }
  }
  
  els.btn_reset.textContent = T('reset');
  
  if (els.chk_coach) els.chk_coach.disabled = aiVsAi();
  if (els.heatLegend) els.heatLegend.hidden = !(S.coach && !aiVsAi() && S.mode !== 'place' && S.mode !== 'endgame');
  
  if (els.sel_rule) {
    [].forEach.call(els.sel_rule.querySelectorAll('option'), function (o) {
      if (o.value === '5' || o.value === '6' || o.value === '7') o.disabled = (S.mode === 'endgame');
    });
  }
  els.btn_prev.disabled = openActive() || G.moves.length === 0;
  els.btn_next.disabled = openActive() || !G.redo.length;
  
  if (document.activeElement !== els.inp_code) {
    var codeTxt = buildCode();
    if (els.inp_code.value !== codeTxt) els.inp_code.value = codeTxt;
  }
  drawCurve();
  bgThinkKick(200);
}

var CURVE_TIERS = [
  [10, 2], [20, 5], [50, 10], [100, 25], [200, 50], [500, 100],
  [1000, 250], [2000, 500], [4000, 1000], [6000, 1500],
];
var curveTier = 0;                 

function cvLabel(v) {
  var s = (Math.abs(v) >= 10000) ? (v / 1000).toFixed(0) + 'k' : String(v);
  return (v > 0 ? '+' : '') + s;
}
function drawCurve() {
  var cv = els.curve, scroll = els.curveScroll, ax = els.curveAxis;
  if (!cv || !scroll || !ax) return;
  var dpr = window.devicePixelRatio || 1;
  
  var rootFs = parseFloat(getComputedStyle(document.documentElement).fontSize) || 16;
  var H = Math.round(16 * rootFs);
  var padT = 10, padB = 18;
  var y0 = padT, y1 = H - padB;
  
  var peak = 0;
  G.curve.forEach(function (p) {
    var vb = Math.abs(p.b || 0), vw = Math.abs(p.w || 0);
    if (vb > RAPFI_EVAL_MAX || vw > RAPFI_EVAL_MAX) return;   
    peak = Math.max(peak, vb, vw);
  });
  if (!G.curve.length) curveTier = 0;                 
  var ti = 0;
  while (ti < CURVE_TIERS.length - 1 && peak > CURVE_TIERS[ti][0]) ti++;
  if (ti > curveTier) curveTier = ti;                                        
  else if (ti < curveTier && peak <= CURVE_TIERS[ti][0] * 0.8) curveTier = ti;  
  var RANGE = CURVE_TIERS[curveTier][0], STEP = CURVE_TIERS[curveTier][1];
  function yOf(v) {
    
    if (Math.abs(v) > RAPFI_EVAL_MAX) {
      var d = Math.max(0, Math.min(1,
        (Math.abs(v) - RAPFI_EVAL_MAX) / (RAPFI_MATE - RAPFI_EVAL_MAX)));
      var band = 0.08 * (1 - d);
      return y0 + (y1 - y0) * ((v > 0) ? band : (1 - band));
    }
    var t = (RANGE - v) / (2 * RANGE);
    return y0 + (y1 - y0) * Math.max(0, Math.min(1, t));
  }
  
  var AXW = (RANGE >= 1000) ? 36 : 30;

  ax.style.width = AXW + 'px';
  ax.style.height = H + 'px';
  ax.width = Math.round(AXW * dpr);
  ax.height = Math.round(H * dpr);
  var ac = ax.getContext('2d');
  ac.setTransform(1, 0, 0, 1, 0, 0);
  ac.clearRect(0, 0, ax.width, ax.height);
  ac.scale(dpr, dpr);
  ac.font = '9px sans-serif';
  ac.textBaseline = 'middle';
  ac.textAlign = 'right';
  for (var g = -RANGE; g <= RANGE; g += STEP) {
    var gy = yOf(g);
    ac.strokeStyle = (g === 0) ? css('--sub') : css('--border');
    ac.lineWidth = (g === 0) ? 1.4 : 1;
    ac.beginPath(); ac.moveTo(AXW - 6, gy); ac.lineTo(AXW, gy); ac.stroke();
    ac.fillStyle = css('--sub');
    ac.fillText((g > 0 ? '+' : '') + g, AXW - 8, gy);
  }

  var availW = Math.max(160, scroll.clientWidth - 2);
  var step = 18;
  
  var maxPly = 0;
  G.curve.forEach(function (p) { maxPly = Math.max(maxPly, p.i || 0); });
  var need = 16 + Math.max(0, maxPly) * step + 16;
  var w = Math.max(availW, need);
  var padL = 6, padR = 14;
  var x0 = padL, x1 = w - padR;
  cv.style.width = w + 'px';
  cv.style.height = H + 'px';
  cv.width = Math.round(w * dpr);
  cv.height = Math.round(H * dpr);
  var ctx = cv.getContext('2d');
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.clearRect(0, 0, cv.width, cv.height);
  ctx.scale(dpr, dpr);

  ctx.lineWidth = 1;                          
  for (var g2 = -RANGE; g2 <= RANGE; g2 += STEP) {
    var gy2 = yOf(g2);
    ctx.strokeStyle = (g2 === 0) ? css('--sub') : css('--border');
    ctx.lineWidth = (g2 === 0) ? 1.4 : 1;
    ctx.beginPath(); ctx.moveTo(x0, gy2); ctx.lineTo(x1, gy2); ctx.stroke();
  }

  CURVE_GEO = { x0: x0, x1: x1, maxPly: maxPly, H: H, w: w };
  if (G.curve.length < 1) {
    ctx.fillStyle = css('--sub');
    ctx.font = '9px sans-serif';
    ctx.textAlign = 'left';
    ctx.textBaseline = 'middle';
    ctx.fillText(T('curve'), x0 + 8, (y0 + y1) / 2);
    drawCurveInfo();
    return;
  }

  (function drawPlyAxis() {
    if (maxPly <= 0 || x1 <= x0) return;
    var pxPerPly = (x1 - x0) / maxPly;
    var plyStep = 1;
    [1, 2, 5, 10, 20, 25, 50].some(function (s) { plyStep = s; return pxPerPly * s >= 22; });
    ctx.font = '9px sans-serif';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'top';
    function plyTick(p) {
      var tx = x0 + (x1 - x0) * (p / maxPly);
      ctx.strokeStyle = css('--border');
      ctx.lineWidth = 1;
      ctx.beginPath(); ctx.moveTo(tx, y1); ctx.lineTo(tx, y1 + 3); ctx.stroke();
      var s = String(p);
      var tw = ctx.measureText(s).width;
      var cx = Math.max(tw / 2 + 1, Math.min(w - tw / 2 - 1, tx));
      ctx.fillStyle = css('--sub');
      ctx.fillText(s, cx, y1 + 4);
    }
    var lastP = 0;
    for (var pp = 1; pp <= maxPly; pp += plyStep) { plyTick(pp); lastP = pp; }
    if (maxPly !== lastP && (maxPly - lastP) * pxPerPly >= 12) plyTick(maxPly);
  })();

  function plot(key, color) {
    var pts = [];
    G.curve.forEach(function (p) {
      var v = p[key];
      if (typeof v !== 'number' || !isFinite(v)) return;   
      pts.push({
        x: x0 + (x1 - x0) * (maxPly <= 1 ? 0 : (p.i / maxPly)),
        y: yOf(Math.max(-RANGE, Math.min(RANGE, v))),
      });
    });
    if (!pts.length) return;
    var yz = yOf(0);
    function trace() {
      ctx.beginPath();
      ctx.moveTo(pts[0].x, pts[0].y);
      if (pts.length === 1) { ctx.lineTo(pts[0].x + 0.1, pts[0].y); return; }
      for (var i = 0; i < pts.length - 1; i++) {
        var p0 = pts[i - 1] || pts[i], p1 = pts[i], p2 = pts[i + 1], p3 = pts[i + 2] || p2;
        ctx.bezierCurveTo(p1.x + (p2.x - p0.x) / 6, p1.y + (p2.y - p0.y) / 6,
                          p2.x - (p3.x - p1.x) / 6, p2.y - (p3.y - p1.y) / 6,
                          p2.x, p2.y);
      }
    }
    
    function rgba(hex, a) {
      return 'rgba(' + parseInt(hex.slice(1, 3), 16) + ',' + parseInt(hex.slice(3, 5), 16) + ',' +
             parseInt(hex.slice(5, 7), 16) + ',' + a + ')';
    }
    var flat = [];
    for (var s = 0; s < pts.length - 1; s++) {
      var p0 = pts[s - 1] || pts[s], p1 = pts[s], p2 = pts[s + 1], p3 = pts[s + 2] || p2;
      var c1x = p1.x + (p2.x - p0.x) / 6, c1y = p1.y + (p2.y - p0.y) / 6;
      var c2x = p2.x - (p3.x - p1.x) / 6, c2y = p2.y - (p3.y - p1.y) / 6;
      for (var t2 = 0; t2 < 28; t2++) {
        var u = t2 / 28, v = 1 - u;
        
        var w1 = v * v * v, w2 = 3 * v * v * u, w3 = 3 * v * u * u, w4 = u * u * u;
        flat.push({ x: w1 * p1.x + w2 * c1x + w3 * c2x + w4 * p2.x,
                    y: w1 * p1.y + w2 * c1y + w3 * c2y + w4 * p2.y });
      }
    }
    flat.push(pts[pts.length - 1]);

    function scaledY(q, s) { return yz + (q.y - yz) * s; }
    function areaPath() {
      ctx.beginPath();
      ctx.moveTo(flat[0].x, yz);                 
      for (var f2 = 0; f2 < flat.length; f2++) ctx.lineTo(flat[f2].x, flat[f2].y);
      ctx.lineTo(flat[flat.length - 1].x, yz);   
      ctx.closePath();                           
    }
    
    areaPath();
    ctx.fillStyle = rgba(color, 0.05);
    ctx.fill();
    
    var AREA_LAYERS = 24, AREA_STEP = 0.010;
    for (var li = 0; li < AREA_LAYERS; li++) {
      var sIn = li / AREA_LAYERS;
      ctx.beginPath();
      ctx.moveTo(flat[0].x, flat[0].y);
      for (var f4 = 1; f4 < flat.length; f4++) ctx.lineTo(flat[f4].x, flat[f4].y);
      for (var f5 = flat.length - 1; f5 >= 0; f5--) ctx.lineTo(flat[f5].x, scaledY(flat[f5], sIn));
      ctx.closePath();
      ctx.fillStyle = rgba(color, AREA_STEP);
      ctx.fill();
    }
    ctx.strokeStyle = color;                 
    ctx.lineWidth = 2;
    ctx.lineJoin = 'round'; ctx.lineCap = 'round';
    trace();
    ctx.stroke();
    ctx.fillStyle = color;
    pts.forEach(function (p) {
      ctx.beginPath(); ctx.arc(p.x, p.y, 1.8, 0, Math.PI * 2); ctx.fill();
    });
  }
  plot('b', '#7a5af8');
  
  plot('w', '#a4d7f5');
  
  if (w > availW) scroll.scrollLeft = w;
  drawCurveInfo();
}


var CURVE_GEO = null, CURVE_HOVER = null;

function curveLastPoint() {
  var out = null;
  G.curve.forEach(function (p) {
    if (typeof p.b !== 'number' || !isFinite(p.b)) return;
    if (!out || (p.i || 0) >= (out.i || 0)) out = p;
  });
  return out;
}
function curvePointAt(ply) {
  var out = null, best = Infinity;
  G.curve.forEach(function (p) {
    if (typeof p.b !== 'number' || !isFinite(p.b)) return;
    var d = Math.abs((p.i || 0) - ply);
    if (d < best) { best = d; out = p; }
  });
  return out;
}
function curvePlyX(ply) {
  if (!CURVE_GEO) return 0;
  var g = CURVE_GEO, n = g.maxPly || 0;
  return g.x0 + ((n <= 1) ? 0 : (g.x1 - g.x0) * (ply / n));
}
function curveHoverLine(p) {
  var ln = els.curveHover;
  if (!ln) return;
  if (!p || !CURVE_GEO) { ln.style.display = 'none'; return; }
  ln.style.left = curvePlyX(p.i || 0) + 'px';
  ln.style.height = CURVE_GEO.H + 'px';
  ln.style.display = 'block';
}
function drawCurveInfo() {
  var hover = (CURVE_HOVER != null);
  var p = hover ? curvePointAt(CURVE_HOVER) : curveLastPoint();
  var b = p ? p.b : null;
  curveHoverLine(hover ? p : null);
  if (b == null) {
    if (els.ci_b_score) els.ci_b_score.textContent = '-';
    if (els.ci_b_win) els.ci_b_win.textContent = '-';
    if (els.ci_w_score) els.ci_w_score.textContent = '-';
    if (els.ci_w_win) els.ci_w_win.textContent = '-';
    paintCiBar(els.ci_b_win, 0); paintCiBar(els.ci_w_win, 0);
    return;
  }
  var wb = scoreWin(b);
  if (els.ci_b_score) els.ci_b_score.textContent = scoreTxt(b);
  if (els.ci_b_win) els.ci_b_win.textContent = Math.round(wb * 100) + '%';
  if (els.ci_w_score) els.ci_w_score.textContent = scoreTxt(-b);
  if (els.ci_w_win) els.ci_w_win.textContent = Math.round((1 - wb) * 100) + '%';
  paintCiBar(els.ci_b_win, wb); paintCiBar(els.ci_w_win, 1 - wb);
}
function paintCiBar(node, ratio) {
  if (!node || !node.parentNode || !node.parentNode.parentNode) return;
  var row = node.parentNode.parentNode;      // .v → .cir → .ci
  if (!row || !row.classList || !row.classList.contains('ci')) return;
  var r = (typeof ratio === 'number' && isFinite(ratio)) ? ratio : 0;
  if (r < 0) r = 0; if (r > 1) r = 1;
  row.style.setProperty('--wr', (r * 100).toFixed(2) + '%');
}

function wireCurveHover() {
  var cv = els.curve;
  if (!cv || cv.__gbHover) return;
  cv.__gbHover = true;
  cv.addEventListener('mousemove', function (e) {
    if (!CURVE_GEO || !G.curve.length) return;
    var r = cv.getBoundingClientRect();
    var g = CURVE_GEO, n = g.maxPly || 0;
    if (n <= 0) return;
    var mx = Math.max(g.x0, Math.min(g.x1, e.clientX - r.left));
    var ply = Math.round((mx - g.x0) / (g.x1 - g.x0) * n);
    var p = curvePointAt(ply);
    if (!p) return;
    CURVE_HOVER = p.i || 0;
    drawCurveInfo();
  });
  cv.addEventListener('mouseleave', function () {
    CURVE_HOVER = null;
    drawCurveInfo();
  });
}

var HIST_KEY = 'gbcalc.history.v1';
var SAVED_KEY = 'gbcalc.saved.v1';
var HIST_MAX = 150;

function loadHist() {
  try { return JSON.parse(localStorage.getItem(HIST_KEY) || '[]'); } catch (e) { return []; }
}
function saveHist(list) {
  try { localStorage.setItem(HIST_KEY, JSON.stringify(list.slice(0, HIST_MAX))); } catch (e) {}
}
function loadSaved() {
  try { return JSON.parse(localStorage.getItem(SAVED_KEY) || '[]'); } catch (e) { return []; }
}
function saveSaved(list) {
  try { localStorage.setItem(SAVED_KEY, JSON.stringify(list)); } catch (e) {}
}
function addRecord(rec) {
  var list = loadHist();
  list.push(rec);
  while (list.length > HIST_MAX) list.shift();
  saveHist(list);
}
function saveCurrent() {
  if (!G.moves.length) return;
  var rec = { ts: Date.now(), src: 'local', rule: S.rule, first: S.side,
              moves: G.moves.map(function (m) { return [m.x, m.y, m.c]; }) };
  
  if (S.egLocked && S.egBase && S.egBase.length) { rec.eg = true; rec.egLen = S.egBase.length; }
  else if (S.mode === 'endgame') { rec.eg = true; rec.egLen = G.moves.length; }
  addRecord(rec);
}

var DR = { tab: 'hist', sel: {}, kb: -1 };   
function openDrawer(tab) {
  DR.tab = tab || DR.tab;
  DR.sel = {};
  DR.kb = -1;                          
  if (els.dr_all) els.dr_all.textContent = T('selAll');   
  els.drawer.hidden = false;
  [].forEach.call(els.drawer.querySelectorAll('.tab'), function (b) {
    b.classList.toggle('on', b.getAttribute('data-tab') === DR.tab);
  });
  
  els.dr_save.classList.toggle('ghost-slot', DR.tab !== 'hist');
  renderDrawer();
}
function closeDrawer() { els.drawer.hidden = true; closeCtx(); }

function listOfTab() { return (DR.tab === 'hist') ? loadHist() : loadSaved(); }
function renderDrawer() {
  if (els.t_drTip) els.t_drTip.textContent = T('drTip');   
  var list = listOfTab();
  els.drCount.textContent = list.length + (DR.tab === 'hist' ? ' / ' + HIST_MAX : '');
  els.drList.innerHTML = '';
  
  if (els.dr_all) els.dr_all.textContent = drAllSelected() ? T('selNone') : T('selAll');
  if (!list.length) {
    els.drList.innerHTML = '<div class="empty">—</div>';
    return;
  }
  list.slice().reverse().forEach(function (h, ri) {
    var idx = list.length - 1 - ri;          
    var d = document.createElement('div');
    d.className = 'item' + (DR.sel[idx] ? ' sel' : '');
    d.setAttribute('data-idx', String(idx)); 
    var cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.checked = !!DR.sel[idx];
    cb.onclick = function (e) {
      e.stopPropagation();
      DR.sel[idx] = cb.checked;
      d.classList.toggle('sel', cb.checked);
    };
    var srcTxt = h.src === 'desktop' ? T('srcDesktop')
               : (h.src === 'bookmark' ? T('srcBookmark')
               : (h.src === 'vis' ? T('visSrc') : T('srcLocal')));
    var no = document.createElement('span');
    no.className = 'no'; no.textContent = '#' + (idx + 1);
    
    var nm = document.createElement('span');
    nm.className = 'nm'; nm.textContent = h.name || '';
    var src = document.createElement('span');
    src.className = 'src'; src.textContent = srcTxt;
    
    var egTag = null;
    if (h.eg) {
      egTag = document.createElement('span');
      egTag.className = 'tag'; egTag.textContent = 'Endgame';
    }
    var ts = document.createElement('span');
    
    ts.textContent = (h.ts ? new Date(h.ts).toLocaleString(S.lang === 'en' ? 'en-US' : 'zh-CN') : '');
    var m = document.createElement('span');
    var mn = (h.moves && h.moves.length) || 0;
    m.className = 'm'; m.textContent = T('nMoves').replace('{n}', String(mn));
    d.appendChild(cb); d.appendChild(no); d.appendChild(nm); d.appendChild(src);
    if (egTag) d.appendChild(egTag);           
    d.appendChild(ts); d.appendChild(m);
    
    d.onclick = function () { openFromHistoryIndex(idx); };
    
    d.oncontextmenu = function (e) {
      e.preventDefault();
      
      DR.sel = {};
      DR.sel[idx] = true;
      [].forEach.call(els.drList.querySelectorAll('.item'), function (n) {
        var on = (n === d);
        n.classList.toggle('sel', on);
        var c = n.querySelector('input[type=checkbox]');
        if (c) c.checked = on;
      });
      showCtx(e, idx);
    };
    els.drList.appendChild(d);
  });
}
function selectedIdx() {
  return Object.keys(DR.sel).filter(function (k) { return DR.sel[k]; })
    .map(function (k) { return +k; }).sort(function (a, b) { return a - b; });
}

function drKbPaint() {
  [].forEach.call(els.drList.querySelectorAll('.item'), function (n) {
    var idx = +(n.getAttribute('data-idx') || -1);
    var on = idx === DR.kb;
    n.classList.toggle('kb', on);
    if (on && n.scrollIntoView) { try { n.scrollIntoView({ block: 'nearest' }); } catch (e) {} }
  });
}
function drKeyNav(d) {
  var list = listOfTab();
  if (!els.drList || !list.length) return;
  if (DR.kb < 0 || DR.kb >= list.length) DR.kb = list.length - 1;   
  DR.kb = Math.max(0, Math.min(list.length - 1, DR.kb + d));
  drKbPaint();
}
function drKbEnter() {
  if (DR.kb >= 0 && DR.kb < listOfTab().length) openFromHistoryIndex(DR.kb);
}
function visKbPaint() {
  if (!els.vdList) return;
  [].forEach.call(els.vdList.querySelectorAll('.vd-item'), function (n) {
    var idx = +(n.getAttribute('data-idx') || -1);
    var on = idx === VIS.kb;
    n.classList.toggle('kb', on);
    if (on && n.scrollIntoView) { try { n.scrollIntoView({ block: 'nearest' }); } catch (e) {} }
  });
}
function visKeyNav(d) {
  if (!els.vdList || !VIS.list.length) return;
  if (VIS.kb < 0 || VIS.kb >= VIS.list.length) VIS.kb = VIS.idx;    
  VIS.kb = Math.max(0, Math.min(VIS.list.length - 1, VIS.kb + d));
  visKbPaint();
}
function visKbEnter() {
  if (VIS.kb >= 0 && VIS.kb < VIS.list.length) { VIS.idx = VIS.kb; VIS.kb = -1; visRefreshView(); }
}
document.addEventListener('keydown', function (e) {
  var t = e.target;
  if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA')) return;
  if (G.fwd && G.fwd.on && G.fwd.line.length) return;               
  var visOpen = !!(els.visDrawer && !els.visDrawer.hidden);
  var drOpen = !!(els.drawer && !els.drawer.hidden);
  if (visOpen && !drOpen) {                                         
    if (e.key === 'ArrowRight' || e.key === 'ArrowDown') { visKeyNav(1); e.preventDefault(); }
    else if (e.key === 'ArrowLeft' || e.key === 'ArrowUp') { visKeyNav(-1); e.preventDefault(); }
    else if (e.key === 'Enter') { visKbEnter(); e.preventDefault(); }
    return;
  }
  if (drOpen) {                                                     
    if (e.key === 'ArrowUp' || e.key === 'ArrowRight') { drKeyNav(1); e.preventDefault(); }
    else if (e.key === 'ArrowDown' || e.key === 'ArrowLeft') { drKeyNav(-1); e.preventDefault(); }
    else if (e.key === 'Enter') { drKbEnter(); e.preventDefault(); }
    return;
  }
  
  if (G.review && !RV_MODE) {
    if (e.key === 'ArrowLeft') { rvGoTo(G.review.k - 1); e.preventDefault(); }
    else if (e.key === 'ArrowRight') { rvGoTo(G.review.k + 1); e.preventDefault(); }
    else if ((e.key === ' ' || e.code === 'Space') && G.review.kind === 'replay') {
      rvSetPlaying(!G.rvPlaying); e.preventDefault();
    }
    return;
  }
  
  if (!RV_MODE && !e.ctrlKey && !e.altKey && !e.metaKey) {
    if (e.key === 'ArrowLeft') { stepBack(); e.preventDefault(); }
    else if (e.key === 'ArrowRight') { stepForward(); e.preventDefault(); }
  }
});

function renameRecord(idx, val) {
  var list = listOfTab();
  if (!list[idx]) return false;
  var nm = String(val == null ? '' : val).replace(/[\r\n\t]+/g, ' ').trim().slice(0, 40);
  if (nm) list[idx].name = nm; else delete list[idx].name;
  if (DR.tab === 'hist') saveHist(list); else saveSaved(list);
  renderDrawer();
  toast(T('renamed'));
  return true;
}

function beginRename(idx) {
  var item = els.drList.querySelector('.item[data-idx="' + idx + '"]');
  var span = item ? item.querySelector('.nm') : null;
  if (!span) return false;
  var rec = listOfTab()[idx] || {};
  var inp = document.createElement('input');
  inp.type = 'text'; inp.className = 'rn'; inp.maxLength = 40;
  inp.value = rec.name || '';
  inp.placeholder = T('namePh');
  var settled = false;
  function settle(keep) {
    if (settled) return;                    
    settled = true;
    if (keep) renameRecord(idx, inp.value); else renderDrawer();
  }
  inp.onkeydown = function (e) {
    e.stopPropagation();                    
    if (e.key === 'Enter') { e.preventDefault(); settle(true); }
    else if (e.key === 'Escape') { e.preventDefault(); settle(false); }
  };
  inp.onblur = function () { settle(true); };
  inp.onclick = function (e) { e.stopPropagation(); };
  span.textContent = '';                    
  span.appendChild(inp);
  inp.focus();
  try { inp.select(); } catch (e) {}
  return true;
}

var CTX = { idx: -1 };
function closeCtx() { if (els.ctxMenu) els.ctxMenu.hidden = true; CTX.idx = -1; }

function showCtx(ev, idx) {
  var m = els.ctxMenu;
  if (!m) return;
  CTX.idx = idx;
  m.innerHTML = '';
  var items = [
    [T('ctxOpen'), false, function () { openFromHistoryIndex(idx); }],
    [T('ctxRename'), false, function () { beginRename(idx); }],
    [T('ctxExport'), false, function () { exportRecords([listOfTab()[idx]], exportNameOf(idx)); }],
  ];
  if (DR.tab === 'hist') items.push([T('ctxToSaved'), false, function () { moveToSaved([idx]); }]);
  items.push([T('ctxDel'), true, function () { deleteIndexes([idx]); }]);
  items.forEach(function (it) {
    var b = document.createElement('button');
    b.type = 'button';
    b.textContent = it[0];
    if (it[1]) b.className = 'danger';
    b.onclick = function (e) { e.stopPropagation(); closeCtx(); it[2](); };
    m.appendChild(b);
  });
  m.hidden = false;
  var w = m.offsetWidth, h = m.offsetHeight;               
  m.style.left = Math.max(6, Math.min(ev.clientX, window.innerWidth - w - 6)) + 'px';
  m.style.top = Math.max(6, Math.min(ev.clientY, window.innerHeight - h - 6)) + 'px';
  return true;
}
function exportNameOf(idx) {
  var h = listOfTab()[idx] || {};
  return (h.name ? h.name : 'gomoku-game-' + (idx + 1)) + '.txt';
}

function deleteIndexes(idxs) {
  idxs = (idxs || []).slice().sort(function (a, b) { return a - b; });
  if (!idxs.length) return 0;
  var list = listOfTab().filter(function (_, i) { return idxs.indexOf(i) < 0; });
  if (DR.tab === 'hist') saveHist(list); else saveSaved(list);
  DR.sel = {};
  renderDrawer();
  return idxs.length;
}

function moveToSaved(idxs) {
  var hist = loadHist(), picked = [];
  (idxs || []).forEach(function (i) { if (hist[i]) picked.push(hist[i]); });
  if (!picked.length) return 0;
  saveSaved(loadSaved().concat(picked));
  saveHist(hist.filter(function (_, i) { return idxs.indexOf(i) < 0; }));
  DR.sel = {};
  openDrawer('saved');
  return picked.length;
}

function openFromHistoryIndex(idx) {
  var h = listOfTab()[idx];
  if (!h) return false;
  closeCtx();
  if (RV_MODE) { reviewLoad(recordForReview(h, true)); closeDrawer(); return true; }
  openReviewWindow(h, true);
  closeDrawer();
  return true;
}

var HIST_TXT_HEAD = {
  zh: '# Meter Gomoku · 历史导出 v1\n' +
      '# 每行一局：<名字>\\t<局面代码>（名字可空；代码 = 小写列字母 + 行号，逐手拼接，与 gomocalc.com 同格式）\n' +
      '# 「#」开头的行是注释，导入时跳过。\n',
  en: '# Gomoku Trainer · history export v1\n' +
      '# One game per line: <name>\\t<position code> (name optional; code = lowercase column letter + row, moves concatenated, same format as gomocalc.com)\n' +
      '# Lines starting with "#" are comments and are skipped on import.\n',
};
function histTxtOf(list) {
  var head = HIST_TXT_HEAD[S.lang] || HIST_TXT_HEAD.zh;
  return head + (list || []).map(function (h) {
    var nm = String((h && h.name) || '').replace(/[\r\n\t]+/g, ' ');
    return nm + '\t' + movesToCode(h && h.moves);
  }).join('\n') + '\n';
}

function exportRecords(list, fname) {
  list = (list || []).filter(Boolean);
  if (!list.length) { toast(T('expNone')); return 0; }
  var txt = histTxtOf(list);
  var name = fname || ('gomoku-history-' + list.length + '.txt');
  if (HOST) {
    tellHost({ type: 'saveTxt', name: name, data: txt, n: list.length });
    return list.length;
  }
  try {
    var a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([txt], { type: 'text/plain;charset=utf-8' }));
    a.download = name;
    document.body.appendChild(a); a.click();
    setTimeout(function () { a.remove(); }, 4000);
  } catch (e) { return 0; }
  toast(T('expDone').replace('{n}', String(list.length)));
  return list.length;
}

function exportSelected() {
  var list = listOfTab(), idx = selectedIdx();
  var picked = idx.length ? idx.map(function (i) { return list[i]; }) : list;
  var nm = (picked.length === 1 && picked[0] && picked[0].name) ? picked[0].name : '';
  return exportRecords(picked, nm ? nm + '.txt' : '');
}

function parseHistTxt(text) {
  var out = [];
  var lines = String(text == null ? '' : text).replace(/^\uFEFF/, '').split(/\r?\n/);
  for (var i = 0; i < lines.length; i++) {
    var line = lines[i].replace(/\s+$/, '');
    if (!line || /^\s*#/.test(line)) continue;
    var name = '', code = line;
    var tab = line.indexOf('\t');
    if (tab >= 0) { name = line.slice(0, tab).trim(); code = line.slice(tab + 1); }
    var mv = parseCode(code);
    if (!mv.length) continue;
    var rec = { ts: Date.now() + i, src: 'local', rule: S.rule,
                moves: mv.map(function (m) { return [m[0], m[1], m[2]]; }) };
    name = name.replace(/[\r\n\t]+/g, ' ').trim().slice(0, 40);
    if (name) rec.name = name;
    out.push(rec);
  }
  return out;
}

function importHist(text) {
  if (text == null) { toast(T('impFail')); return 0; }
  var recs = parseHistTxt(text);
  if (!recs.length) { toast(T('impNone')); return 0; }
  var list = loadHist().concat(recs);
  while (list.length > HIST_MAX) list.shift();
  saveHist(list);
  if (DR.tab === 'hist') renderDrawer();
  toast(T('impDone').replace('{n}', String(recs.length)));
  
  tellHost({ type: 'histImported', n: recs.length,
             fp: recs.map(function (r) {
               return (r.name || '') + '=' + movesToCode(r.moves);
             }).join(' | ') });
  return recs.length;
}

function testHistExport() {
  var seed = [
    { ts: 1700000000000, src: 'local', name: '导出样本A', rule: 0,
      moves: [[7, 7, 1], [8, 7, 2], [7, 8, 1], [8, 8, 2], [7, 9, 1],
              [8, 9, 2], [7, 10, 1], [8, 10, 2], [7, 11, 1]] },
    { ts: 1700000001000, src: 'local', rule: 0, moves: [[0, 0, 1], [1, 0, 2], [0, 1, 1]] },
  ];
  saveHist(seed);
  renderDrawer();
  return exportRecords(seed, 'gb-hist-roundtrip.txt');
}

function pickImport() {
  if (HOST) { tellHost({ type: 'openTxt' }); return true; }
  if (els.file_imp) { els.file_imp.value = ''; els.file_imp.click(); return true; }
  return false;
}

function toast(msg) {
  if (!msg) return;
  if (RV_MODE) { if (els.t_drTip) els.t_drTip.textContent = msg; return; }
  setStat(null, msg);
}

var APP_VERSION = '4.46';

var UPDATE_URL = 'https://wwbbj.lanzout.com/b016l49aqj';
var UPDATE_PWD = '15';

function checkUpdate() {
  if (HOST) { tellHost({ type: 'checkUpdate' }); return; }
  openUpdatePage();   
}

function openBrowserShell() {
  if (HOST) { tellHost({ type: 'openShell' }); return; }
  window.open(UPDATE_URL, '_blank');
}

function openUpdatePage() {
  try { if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(UPDATE_PWD); }
  catch (e) {  }
  if (HOST) { tellHost({ type: 'copy', text: UPDATE_PWD }); tellHost({ type: 'openUrl', url: UPDATE_URL }); return; }
  window.open(UPDATE_URL, '_blank');
  toast(T('updateOpened').replace('{p}', UPDATE_PWD));
}

function escHtml(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}
function abEsc(t) { return escHtml(String(t == null ? '' : t).replace(/\*\*/g, '')); }
function renderAbout() {
  if (!els.about || !els.ab_body) return;
  var h = '';
  h += '<div class="ab-app">' + escHtml(T('aboutApp')) + '</div>';
  h += '<div class="ab-ver">' + escHtml(T('aboutVer')) + ' <b>v' + escHtml(APP_VERSION) + '</b></div>';
  
  h += renderAboutSys();

  h += '<div class="ab-h2">' + escHtml(T('aboutGuide')) + '</div>';
  (T('aboutGuideList') || []).forEach(function (sec) {
    h += '<div class="ab-g-sec"><div class="ab-g-t">' + escHtml(sec.t) + '</div>';
    (sec.ps || []).forEach(function (p) { h += '<p class="ab-g-p">' + abEsc(p) + '</p>'; });
    h += '</div>';
  });

  h += '<div class="ab-h2">' + escHtml(T('aboutFeat')) + '</div><ul class="ab-ul">';
  (T('aboutFeatList') || []).forEach(function (x) { h += '<li>' + abEsc(x) + '</li>'; });
  h += '</ul>';

  h += '<div class="ab-h2">' + escHtml(T('aboutOss')) + '</div>';
  h += '<table class="ab-tb"><tbody>';
  (T('aboutOssList') || []).forEach(function (r) {
    h += '<tr><td class="ab-n">' + abEsc(r[0]) + '</td><td class="ab-d">' + abEsc(r[1]) +
         '</td><td class="ab-l">' + abEsc(r[2]) + '</td></tr>';
  });
  h += '</tbody></table>';
  h += '<div class="ab-lic">' + escHtml(T('aboutOssNote')) + '</div>';

  h += '<div class="ab-h2">' + escHtml(T('aboutLic')) + '</div>';
  h += '<div class="ab-lic">' + abEsc(T('aboutLicText')) + '</div>';

  h += '<div class="ab-h2">' + escHtml(T('aboutUpdates')) + '</div><ul class="ab-ul ab-news">';
  (T('aboutUpdateList') || []).forEach(function (x) { h += '<li>' + abEsc(x) + '</li>'; });
  h += '</ul>';
  els.ab_body.innerHTML = h;
  
}
function openAbout() { renderAbout(); if (els.about) els.about.hidden = false; }
function closeAbout() { if (els.about) els.about.hidden = true; }
function wireAbout() {
  if (els.btn_about) els.btn_about.onclick = openAbout;
  if (els.ab_close) els.ab_close.onclick = closeAbout;
  if (els.about) els.about.addEventListener('click', function (e) {
    if (e.target === els.about) closeAbout();          
  });
}

function drAllSelected() {
  var list = listOfTab();
  if (!list.length) return false;
  for (var i = 0; i < list.length; i++) if (!DR.sel[i]) return false;
  return true;
}
function wireDrawer() {
  [].forEach.call(els.drawer.querySelectorAll('.tab'), function (b) {
    b.onclick = function () { openDrawer(b.getAttribute('data-tab')); };
  });
  
  if (els.dr_all) els.dr_all.onclick = function () {
    var list = listOfTab();
    if (!list.length) return;
    if (drAllSelected()) { DR.sel = {}; }
    else { DR.sel = {}; for (var j = 0; j < list.length; j++) DR.sel[j] = true; }
    els.dr_all.textContent = drAllSelected() ? T('selNone') : T('selAll');
    renderDrawer();
  };
  els.dr_close.onclick = closeDrawer;
  
  document.addEventListener('mousedown', function (e) {
    if (e.button !== 0 || els.drawer.hidden) return;
    if (els.drawer.contains(e.target)) return;
    if (e.target && e.target.closest && e.target.closest('#btn_history')) return;
    if (e.target && e.target.closest && e.target.closest('#ctxMenu')) return;
    closeDrawer();
  });
  els.dr_exp.onclick = exportSelected;
  els.dr_imp.onclick = pickImport;
  els.dr_save.onclick = function () { moveToSaved(selectedIdx()); };
  els.dr_del.onclick = function () { deleteIndexes(selectedIdx()); };
  els.dr_open.onclick = function () {
    var idx = selectedIdx();
    if (!idx.length) return;
    openFromHistoryIndex(idx[0]);
  };
  if (els.file_imp) {
    els.file_imp.onchange = function () {
      var f = els.file_imp.files && els.file_imp.files[0];
      if (!f) return;
      var fr = new FileReader();
      fr.onload = function () { importHist(String(fr.result || '')); };
      fr.onerror = function () { toast(T('impFail')); };
      fr.readAsText(f, 'utf-8');
    };
  }
  
  document.addEventListener('click', function (e) {
    if (els.ctxMenu && !els.ctxMenu.hidden && !els.ctxMenu.contains(e.target)) closeCtx();
  }, true);
  document.addEventListener('contextmenu', function (e) {
    
    var t = e.target;
    var onItem = !!(t && t.closest && t.closest('.item, #ctxMenu'));
    if (els.ctxMenu && !els.ctxMenu.hidden && !onItem) closeCtx();
  }, true);
  
  els.drList.addEventListener('contextmenu', function (e) {
    var it = e.target && e.target.closest ? e.target.closest('.item') : null;
    if (!it) return;
    var idx = +it.getAttribute('data-idx');
    if (isNaN(idx)) return;
    e.preventDefault();
    e.stopPropagation();
    DR.sel = {};
    DR.sel[idx] = true;
    [].forEach.call(els.drList.querySelectorAll('.item'), function (n) {
      var on = (n === it);
      n.classList.toggle('sel', on);
      var c = n.querySelector('input[type=checkbox]');
      if (c) c.checked = on;
    });
    showCtx(e, idx);
  });
  window.addEventListener('keydown', function (e) { if (e.key === 'Escape') closeCtx(); });
  els.drList.addEventListener('scroll', closeCtx);
}

function recordForReview(h, hist) {
  
  var fromHist = (hist === undefined) ? !!(h && h.hist) : !!hist;
  return {
    ts: (h && h.ts) || Date.now(),
    src: (h && h.src) || 'local',
    hist: fromHist,
    
    eg: !!(h && h.eg), egLen: (h && h.egLen) || 0, vc: !!(h && h.vc),
    moves: ((h && h.moves) || []).map(function (a) { return [a[0], a[1], a[2] || 0]; })
      .filter(function (a) { return a[0] != null && a[1] != null; }),
  };
}

function openReviewWindow(rec, hist) {
  var payload = rec ? recordForReview(rec, hist)
                    : { ts: Date.now(), src: 'local', hist: !!hist, moves: [] };
  if (HOST) {
    tellHost({ type: 'openReview', record: payload });
    return true;
  }
  
  try { localStorage.setItem('gbcalc.review.pending', JSON.stringify(payload)); } catch (e) {}
  var w = window.open('calc.html?rv=1', 'gb_review');
  return !!w;
}

function openReviewFromBoard() {
  openReviewWindow(G.moves.length ? {
    ts: Date.now(), src: 'local',
    moves: G.moves.map(function (m) { return [m.x, m.y, m.c]; }),
  } : null, false);
}

function openRecord(h) {
  var mv = (h.moves || []).map(function (a) {
    return { x: a[0], y: a[1], c: a[2] || 0 };
  }).filter(function (m) { return m.x != null && m.y != null; });
  G.loaded = { moves: mv, src: h.src || 'local' };
  G.board = newBoard();
  G.moves = [];
  mv.forEach(function (m) {
    var c = m.c || ((G.moves.length % 2 === 0) ? 1 : 2);
    m.c = c;
    G.board[m.y][m.x] = c; G.moves.push({ x: m.x, y: m.y, c: c });
  });
  G.curve = []; G.heat = []; G.nums = []; G.missRings = []; G.redo = []; G.review = null;
  recomputeOver();                            
  
  if (h.vc && !RV_MODE && S.mode !== 'endgame') {
    
    S.egLocked = false; S.egBase = null;
    S.mode = 'endgame';
    [].forEach.call(els.seg_mode.querySelectorAll('button'), function (x) {
      x.classList.toggle('on', x.getAttribute('data-mode') === 'endgame');
    });
    save();
  }
  
  if (h.eg && !RV_MODE) {
    var baseN = Math.max(0, Math.min(h.egLen || mv.length, mv.length));
    S.egBase = mv.slice(0, baseN).map(function (m) { return [m.x, m.y, m.c]; });
    S.egLocked = S.egBase.length > 0;
    if (S.mode !== 'place') {
      S.mode = 'place';
      [].forEach.call(els.seg_mode.querySelectorAll('button'), function (x) {
        x.classList.toggle('on', x.getAttribute('data-mode') === 'place');
      });
    }
    save();
  }
  hideReviewNav();
  setMissBox();
  closeDrawer();
  paint();
  refreshUI();
  refreshCoach();                             
}

var VIS_LIST_MAX = 150;

var VIS = { list: [], idx: 0, busy: false, result: null, seq: 0, queued: false, tmr: 0,
            sel: null, anchor: -1, edit: false, pick: null, delPts: [], swapSel: null,
            ghosts: [], saveTmr: 0, addOpen: false, undoStack: [], kb: -1,
            vc: false, gateBad: false,
            wd: 0, retry: 0 };

function visSelSet() { if (!VIS.sel) VIS.sel = new Set(); return VIS.sel; }

function visMsg(t) {
  if (els.visMsg) els.visMsg.textContent = t || '';
  visLayout();
}

function visSynthMoves(bl, wh) {
  var bs = [], ws = [];
  (bl || []).forEach(function (p) { bs.push({ x: p.x, y: p.y, c: 1 }); });
  (wh || []).forEach(function (p) { ws.push({ x: p.x, y: p.y, c: 2 }); });
  var mv = [], bi = 0, wi = 0;
  for (var i = 0; i < bs.length + ws.length; i++) {
    var takeB = (i % 2 === 0);
    var p = takeB ? bs[bi++] : ws[wi++];
    if (!p) p = takeB ? ws[wi++] : bs[bi++];
    if (!p) break;
    mv.push([p.x, p.y, p.c]);
  }
  return mv;
}

function visShowMoves(mv) {
  var b = newBoard();
  (mv || []).forEach(function (m) { b[m[1]][m[0]] = m[2]; });
  G.board = b;
  G.moves = (mv || []).map(function (a) { return { x: a[0], y: a[1], c: a[2] }; });
  G.over = false; G.loaded = null; G.review = null;
  G.heat = []; G.coach = []; G.curve = []; G.missRings = []; G.redo = [];
  VIS.ghosts = []; VIS.swapSel = null; VIS.delPts = [];   
  paint();
  visUpdateCode();                       
}

function visUpdateCode() {
  if (!els.visCodeBar || !els.visCode) return;
  var has = !!(G.moves && G.moves.length);
  els.visCodeBar.hidden = !has || visRc.on;
  if (has) els.visCode.value = movesToCode(G.moves);
  visLayout();
}

function visShowResult(r) {
  visShowMoves(visSynthMoves((r && r.black) || [], (r && r.white) || []));
}

function visRefreshView() {
  var has = VIS.list.length > 0;
  VIS.retry = 0;
  var it = VIS.list[VIS.idx];
  var isImg = !!it && it.t === 'img';
  els.visImg.hidden = !isImg;
  els.visInput.hidden = isImg;                 
  els.btn_vis_del.hidden = !has;
  els.btn_vis_prev.disabled = !has || VIS.idx <= 0;
  els.btn_vis_next.disabled = !has || VIS.idx >= VIS.list.length - 1;
  if (isImg) {
    els.visImg.src = it.d;
    
    if (!it.w) {
      var probe = new Image();
      probe.onload = function () { it.w = probe.naturalWidth; it.h = probe.naturalHeight; };
      probe.src = it.d;
    }
  } else {
    els.visImg.removeAttribute('src');
    els.visInput.value = it ? it.code : '';
    els.visInput.placeholder = has ? T('visBoxHintCode') : T('visBoxHint');
    if (has && document.activeElement !== els.visInput) els.visInput.focus();
  }
  els.visPos.textContent = has ? (VIS.idx + 1) + ' / ' + VIS.list.length : '0 / 0';
  els.visPos.title = T('visPillTip');
  if (els.vdCount) els.vdCount.textContent = String(VIS.list.length);
  if (els.t_vdTip) els.t_vdTip.textContent = T('visVdTip');
  visRenderDrawer();                         
  visUpdateCode();                           
  visAutoRecognize();                        
}

function visAutoRecognize() {
  var it = VIS.list[VIS.idx];
  if (!it) return;
  if (it.t === 'code') { visShowMoves(it.moves); return; }
  if (VIS.tmr) clearTimeout(VIS.tmr);
  VIS.tmr = setTimeout(function () {
    VIS.tmr = 0;
    if (VIS.busy) { VIS.queued = true; return; }
    visRecognize();
  }, 200);
}

var VIS_RC_DEF = { gray_offset: 50, black_offset: 96, white_offset: 96,
                   black_percent: 33, white_percent: 33 };
var VIS_RC_FIELDS = [
  { k: 'gray_offset',  lo: 0, hi: 255, zh: '灰度偏差', en: 'Gray offset',
    dh: '识别时灰度容差，越大越容易判成棋子',              de: 'Gray tolerance; larger catches more stones' },
  { k: 'black_offset', lo: 0, hi: 255, zh: '黑色偏差', en: 'Black offset',
    dh: '黑子判定的颜色容差，偏大偏小都可能误判',           de: 'Colour tolerance for black stones' },
  { k: 'black_percent', lo: 0, hi: 100, zh: '黑色占比', en: 'Black ratio',
    dh: '黑子判定阈值（0-100），偏严可去噪、偏松可救暗子',   de: 'Black-stone threshold (0-100)' },
  { k: 'white_offset', lo: 0, hi: 255, zh: '白色偏差', en: 'White offset',
    dh: '白子判定的颜色容差，偏大偏小都可能误判',           de: 'Colour tolerance for white stones' },
  { k: 'white_percent', lo: 0, hi: 100, zh: '白色占比', en: 'White ratio',
    dh: '白子判定阈值（0-100），偏严可去噪、偏松可救暗子',   de: 'White-stone threshold (0-100)' }];
var visRc = { on: false, val: {}, base: {}, tmr: 0 };

function visRcLabel(f) { return (S.lang === 'en') ? f.en : f.zh; }
function visRcDesc(f) { return (S.lang === 'en') ? f.de : f.dh; }
function visRcMax(k) {
  for (var i = 0; i < VIS_RC_FIELDS.length; i++) if (VIS_RC_FIELDS[i].k === k) return VIS_RC_FIELDS[i].hi;
  return 255;
}

function visRcNum(k) {
  var v = parseInt(visRc.val[k], 10);
  if (isNaN(v)) v = VIS_RC_DEF[k];
  return v;
}

function visRcBuild() {
  if (!els.visRcBox || els.visRcBox.childElementCount) return;
  for (var k in VIS_RC_DEF) if (visRc.val[k] === undefined) visRc.val[k] = VIS_RC_DEF[k];
  var html = '';
  for (var i = 0; i < VIS_RC_FIELDS.length; i++) {
    var f = VIS_RC_FIELDS[i];
    html += '<div class="vis-rcitem" data-k="' + f.k + '">' +
      '<div class="vis-rcnameline">' +
        '<span class="vis-rclab" title="' + visRcDesc(f) + '">' + visRcLabel(f) + '</span>' +
        '<input class="vis-rcinp" type="text" inputmode="numeric" maxlength="3" value="' +
          visRcNum(f.k) + '">' +
      '</div>' +
      '<input class="vis-rcsld" type="range" min="' + f.lo + '" max="' + f.hi + '" step="1" value="' +
        visRcNum(f.k) + '">' +
    '</div>';
  }
  html += '<div class="vis-rcbtns">' +
            '<button class="btn vis-rcbtn-gen"></button>' +
            '<button class="btn vis-rcbtn-cancel"></button>' +
            '<button class="btn vis-rcbtn-ok"></button>' +
          '</div>';
  els.visRcBox.innerHTML = html;
  var gen = els.visRcBox.querySelector('.vis-rcbtn-gen');
  if (gen) { gen.textContent = T('visRcGeneral'); gen.onclick = visRcGeneral; }
  var cnl = els.visRcBox.querySelector('.vis-rcbtn-cancel');
  if (cnl) { cnl.textContent = T('visRcCancel'); cnl.onclick = visRcCancel; }
  var okb = els.visRcBox.querySelector('.vis-rcbtn-ok');
  if (okb) { okb.textContent = T('visRcOk'); okb.onclick = visRcOk; }
  var items = els.visRcBox.querySelectorAll('.vis-rcitem');
  for (var j = 0; j < items.length; j++) {
    (function (item) {
      var k = item.getAttribute('data-k');
      var sld = item.querySelector('.vis-rcsld'), inp = item.querySelector('.vis-rcinp');
      sld.addEventListener('input', function () {
        var v = parseInt(sld.value, 10);
        if (isNaN(v)) return;
        visRc.val[k] = v;
        if (inp) inp.value = String(v);
        visRcLive();
      });
      inp.addEventListener('input', function () {
        var raw = String(inp.value).replace(/[^0-9]/g, '');
        var v = parseInt(raw, 10);
        if (raw !== '' && !isNaN(v) && v >= 0 && v <= visRcMax(k)) {
          visRc.val[k] = v;
          if (sld) sld.value = String(v);
          visRcLive();
        }
        inp.value = raw;
      });
      inp.addEventListener('blur', function () {
        var v = visRcNum(k);
        visRc.val[k] = v;
        inp.value = String(v);
        if (sld) sld.value = String(v);
        visRcLive();
      });
    })(items[j]);
  }
}

function visRcFill(o) {
  if (!o) return;
  for (var i = 0; i < VIS_RC_FIELDS.length; i++) {
    var k = VIS_RC_FIELDS[i].k;
    var v = parseInt(o[k], 10);
    if (!isNaN(v)) visRc.val[k] = v;
  }
  visRcPaint();
}

function visRcPaint() {
  if (!els.visRcBox) return;
  var items = els.visRcBox.querySelectorAll('.vis-rcitem');
  for (var j = 0; j < items.length; j++) {
    var k = items[j].getAttribute('data-k');
    var v = visRcNum(k);
    var sld = items[j].querySelector('.vis-rcsld'), inp = items[j].querySelector('.vis-rcinp');
    if (sld) sld.value = String(v);
    if (inp && document.activeElement !== inp) inp.value = String(v);
  }
}

function visRcSnapshot() {
  var o = {};
  for (var i = 0; i < VIS_RC_FIELDS.length; i++) { var k = VIS_RC_FIELDS[i].k; o[k] = visRcNum(k); }
  return o;
}

function visRcLive() {
  if (visRc.tmr) clearTimeout(visRc.tmr);
  visRc.tmr = setTimeout(function () {
    visRc.tmr = 0;
    visRcPersist();
    if (visRc.on) visAutoRecognize();
  }, 180);
}

function visRcPersist() {
  if (!HOST) return;
  tellHost({ type: 'visRecogPreview', recog: visRcSnapshot() });
}

function visRcGeneral() {
  for (var i = 0; i < VIS_RC_FIELDS.length; i++) {
    var k = VIS_RC_FIELDS[i].k;
    visRc.val[k] = VIS_RC_DEF[k];
  }
  visRcPaint();
  visRcLive();
  visMsg(T('visRcGeneralMsg'));
}

function visRcOk() {
  visRcPersist();
  visRcClose(false);
  visMsg(T('visRcOkMsg'));
}

function visRcCancel() {
  for (var i = 0; i < VIS_RC_FIELDS.length; i++) {
    var k = VIS_RC_FIELDS[i].k;
    visRc.val[k] = (visRc.base[k] === undefined) ? VIS_RC_DEF[k] : visRc.base[k];
  }
  visRcPaint();
  visRcClose(true);
  visMsg(T('visRcCancelMsg'));
}

function visRcClose(revert) {
  visRc.on = false;
  if (visRc.tmr) { clearTimeout(visRc.tmr); visRc.tmr = 0; }
  visRcPersist();
  if (els.visRcBox) els.visRcBox.hidden = true;
  visUpdateCode();
  visLayout();
  if (revert) visAutoRecognize();
}

function visRcToggle() {
  if (!els.visRcBox) return;
  if (visRc.on) { visRcClose(false); visMsg(T('visRcOff')); return; }
  visRc.on = true;
  visRcBuild();
  for (var i = 0; i < VIS_RC_FIELDS.length; i++) {
    var k = VIS_RC_FIELDS[i].k;
    visRc.base[k] = visRcNum(k);
  }
  els.visRcBox.hidden = false;
  visRcPaint();
  visUpdateCode();
  visLayout();
  visMsg(T('visRcOn'));
}

function visLayout() {
  if (!els.visBox || !els.visView) return;
  var gapPx = 8;                                  // .vis-view gap:.5rem ≈ 8px
  var used = 0, n = 0, kids = els.visView.children;
  for (var i = 0; i < kids.length; i++) {
    var c = kids[i];
    if (c === els.visBox) continue;
    if (c.hidden || c.offsetParent === null || !c.offsetHeight) continue;
    used += c.offsetHeight; n++;
  }
  var avail = els.visView.clientHeight || 0;
  var h;
  if (avail) h = avail - used - gapPx * (n + 1);
  else h = Math.round(((els.board ? els.board.clientHeight : 480) || 480) * 0.56);
  if (visRc.on) { if (h < 88) h = 88; }
  else { if (h < 170) h = 170; }
  if (h > 460) h = 460;
  els.visBox.style.height = h + 'px';
}

function visMakeRoom() {
  var dropped = false;
  while (VIS.list.length >= VIS_LIST_MAX) {
    VIS.list.shift();
    if (VIS.idx > 0) VIS.idx--;
    VIS.anchor = -1;
    if (VIS.sel) VIS.sel.clear();
    dropped = true;
  }
  return dropped;
}

var VIS_DB = null;
function visDb() {
  if (VIS_DB) return VIS_DB;
  VIS_DB = new Promise(function (res) {
    try {
      var rq = indexedDB.open('gbcalc-vis', 1);
      rq.onupgradeneeded = function () { rq.result.createObjectStore('kv'); };
      rq.onsuccess = function () { res(rq.result); };
      rq.onerror = function () { res(null); };
    } catch (e) { res(null); }
  });
  return VIS_DB;
}
function visPersist() {
  if (VIS.saveTmr) clearTimeout(VIS.saveTmr);
  VIS.saveTmr = setTimeout(function () {
    VIS.saveTmr = 0;
    visDb().then(function (db) {
      if (!db) return;
      try { db.transaction('kv', 'readwrite').objectStore('kv').put(VIS.list.slice(), 'items'); }
      catch (e) {}
    });
  }, 400);
}
function visRestore(cb) {
  visDb().then(function (db) {
    if (!db) { cb(); return; }
    try {
      var rq = db.transaction('kv').objectStore('kv').get('items');
      rq.onsuccess = function () {
        var items = rq.result;
        if (Array.isArray(items)) {
          VIS.list = items.filter(function (it) {
            return it && (it.t === 'img' || it.t === 'code');
          }).slice(-VIS_LIST_MAX);
        }
        cb();
      };
      rq.onerror = function () { cb(); };
    } catch (e) { cb(); }
  });
}

function visAddImage(dataUrl, select) {
  if (!dataUrl || String(dataUrl).indexOf('data:image') !== 0) return false;
  visMakeRoom();
  VIS.list.push({ t: 'img', d: String(dataUrl) });
  if (select) VIS.idx = VIS.list.length - 1;
  visPersist();
  visRefreshView();
  return true;
}

function visAddCode(txt, select) {
  var code = String(txt || '').trim();
  var mv = parseCode(code);
  if (!mv.length) { visMsg(T('visCodeBad')); return false; }
  visMakeRoom();
  
  var cur = VIS.list[VIS.idx];
  if (cur && cur.t === 'code' && select === 'replace') {
    cur.code = code; cur.moves = mv;
  } else {
    VIS.list.push({ t: 'code', code: code, moves: mv });
    VIS.idx = VIS.list.length - 1;
  }
  visRefreshView();
  visShowMoves(mv);
  visMsg('');
  return true;
}

var CROP = { open: false, img: null, sel: null, drag: null, scale: 1, cssW: 1, cssH: 1, dpr: 1 };

function cropOpen() {
  var it = VIS.list[VIS.idx];
  if (!it || it.t !== 'img') { visMsg(T('visCropNone')); return; }
  var im = new Image();
  im.onload = function () {
    if (!im.naturalWidth || !im.naturalHeight) { visMsg(T('visCropFail')); return; }
    CROP.img = im; CROP.sel = null; CROP.drag = null;
    
    var maxW = Math.max(240, window.innerWidth * 0.9);
    var maxH = Math.max(200, window.innerHeight * 0.84);
    CROP.scale = Math.min(maxW / im.naturalWidth, maxH / im.naturalHeight, 1);
    CROP.dpr = Math.max(1, window.devicePixelRatio || 1);
    CROP.cssW = Math.max(1, Math.round(im.naturalWidth * CROP.scale));
    CROP.cssH = Math.max(1, Math.round(im.naturalHeight * CROP.scale));
    var cv = els.cropCanvas;
    cv.width = Math.max(1, Math.round(CROP.cssW * CROP.dpr));    
    cv.height = Math.max(1, Math.round(CROP.cssH * CROP.dpr));
    cv.style.width = CROP.cssW + 'px';                           
    cv.style.height = CROP.cssH + 'px';
    CROP.open = true;
    els.visCrop.hidden = false;
    cropDraw(); cropSizeText();
  };
  im.onerror = function () { visMsg(T('visCropFail')); };
  im.src = it.d;
}

function cropDraw() {
  var cv = els.cropCanvas, ctx = cv.getContext('2d');
  var W = CROP.cssW, H = CROP.cssH;
  ctx.setTransform(CROP.dpr, 0, 0, CROP.dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);
  if (CROP.img) ctx.drawImage(CROP.img, 0, 0, W, H);
  var s = CROP.sel;
  if (!s || s.w < 1 || s.h < 1) return;
  ctx.save();
  ctx.fillStyle = 'rgba(0,0,0,.45)';          
  ctx.beginPath();
  ctx.rect(0, 0, W, H);
  ctx.rect(s.x, s.y, s.w, s.h);
  ctx.fill('evenodd');
  ctx.strokeStyle = 'rgba(90,190,255,.98)';
  ctx.lineWidth = 1.5;
  ctx.strokeRect(s.x + .5, s.y + .5, s.w - 1, s.h - 1);
  ctx.strokeStyle = 'rgba(90,190,255,.42)';   
  ctx.lineWidth = 1;
  ctx.beginPath();
  for (var k = 1; k <= 2; k++) {
    ctx.moveTo(s.x + s.w * k / 3, s.y); ctx.lineTo(s.x + s.w * k / 3, s.y + s.h);
    ctx.moveTo(s.x, s.y + s.h * k / 3); ctx.lineTo(s.x + s.w, s.y + s.h * k / 3);
  }
  ctx.stroke();
  ctx.fillStyle = 'rgba(90,190,255,.98)';     
  var hs = 3.5;
  [[s.x, s.y], [s.x + s.w, s.y], [s.x, s.y + s.h], [s.x + s.w, s.y + s.h]].forEach(function (p) {
    ctx.fillRect(p[0] - hs, p[1] - hs, hs * 2, hs * 2);
  });
  ctx.restore();
}

function cropSizeText() {
  if (!els.t_visCropSize) return;
  var s = CROP.sel;
  if (!s || s.w < 1 || s.h < 1 || !CROP.img) { els.t_visCropSize.textContent = ''; return; }
  els.t_visCropSize.textContent = Math.round(s.w / CROP.scale) + ' × ' + Math.round(s.h / CROP.scale);
}

function cropClose() {
  CROP.open = false; CROP.drag = null;
  if (els.visCrop) els.visCrop.hidden = true;
}
function cropReset() { CROP.sel = null; cropDraw(); cropSizeText(); }

function cropNorm(a, b) {
  var x1 = Math.max(0, Math.min(CROP.cssW, Math.min(a.x, b.x)));
  var y1 = Math.max(0, Math.min(CROP.cssH, Math.min(a.y, b.y)));
  var x2 = Math.max(0, Math.min(CROP.cssW, Math.max(a.x, b.x)));
  var y2 = Math.max(0, Math.min(CROP.cssH, Math.max(a.y, b.y)));
  return { x: x1, y: y1, w: x2 - x1, h: y2 - y1 };
}
function cropPos(ev) {
  var r = els.cropCanvas.getBoundingClientRect();
  return { x: ev.clientX - r.left, y: ev.clientY - r.top };
}

function cropApply() {
  var s = CROP.sel, im = CROP.img;
  if (!s || !im) { visMsg(T('visCropSmall')); return; }
  var sw = Math.round(s.w / CROP.scale), sh = Math.round(s.h / CROP.scale);
  if (sw < 8 || sh < 8) { visMsg(T('visCropSmall')); return; }
  var sx = Math.max(0, Math.round(s.x / CROP.scale)), sy = Math.max(0, Math.round(s.y / CROP.scale));
  sw = Math.max(1, Math.min(sw, im.naturalWidth - sx));
  sh = Math.max(1, Math.min(sh, im.naturalHeight - sy));
  var cv = document.createElement('canvas');
  cv.width = sw; cv.height = sh;
  cv.getContext('2d').drawImage(im, sx, sy, sw, sh, 0, 0, sw, sh);
  var url = '';
  try { url = cv.toDataURL('image/png'); } catch (e) { url = ''; }
  if (!url || url.indexOf('data:image') !== 0) { visMsg(T('visCropFail')); return; }
  var it = VIS.list[VIS.idx];
  if (!it || it.t !== 'img') { visMsg(T('visCropNone')); return; }
  it.d = url;
  VIS.result = null;
  visPersist();
  visRefreshView();
  cropClose();
  visMsg(T('visCropDone').replace('{w}', String(sw)).replace('{h}', String(sh)));
}

function wireCrop() {
  if (!els.cropCanvas) return;
  els.cropCanvas.addEventListener('mousedown', function (ev) {
    if (ev.button !== 0) return;
    ev.preventDefault();
    CROP.drag = cropPos(ev);
    CROP.sel = { x: CROP.drag.x, y: CROP.drag.y, w: 0, h: 0 };
    cropDraw(); cropSizeText();
  });
  window.addEventListener('mousemove', function (ev) {
    if (!CROP.open || !CROP.drag) return;
    CROP.sel = cropNorm(CROP.drag, cropPos(ev));
    cropDraw(); cropSizeText();
  });
  window.addEventListener('mouseup', function () {
    if (!CROP.drag) return;
    CROP.drag = null;
    if (CROP.sel && (CROP.sel.w < 3 || CROP.sel.h < 3)) { CROP.sel = null; cropDraw(); cropSizeText(); }
  });
  document.addEventListener('keydown', function (ev) {
    if (CROP.open && ev.key === 'Escape') { ev.preventDefault(); cropClose(); }
  });
  if (els.btn_crop_reset) els.btn_crop_reset.onclick = cropReset;
  if (els.btn_crop_cancel) els.btn_crop_cancel.onclick = cropClose;
  if (els.btn_crop_ok) els.btn_crop_ok.onclick = cropApply;
  if (els.visCrop) els.visCrop.addEventListener('mousedown', function (ev) {
    if (ev.target === els.visCrop) cropClose();          
  });
}

function visRemoveCurrent() { visRemoveAt(VIS.idx); }

function visRemoveAt(i) {
  if (i < 0 || i >= VIS.list.length) return;
  VIS.list.splice(i, 1);
  if (VIS.idx >= VIS.list.length) VIS.idx = Math.max(0, VIS.list.length - 1);
  VIS.anchor = -1;
  if (VIS.sel) VIS.sel.clear();
  VIS.result = null;
  visMsg('');
  visPersist();
  if (!VIS.list.length) {
    visRefreshView();
    G.board = newBoard(); G.moves = []; paint();
    if (els.visInput) els.visInput.focus();
    return;
  }
  visRefreshView();
  var it = VIS.list[VIS.idx];
  if (it.t === 'code') visShowMoves(it.moves);
}

function visRemoveSelected() {
  var sel = VIS.sel && VIS.sel.size ? Array.from(VIS.sel) : [];
  if (!sel.length) { visMsg(T('visNoSel')); return; }
  var n = sel.length;
  sel.sort(function (a, b) { return b - a; }).forEach(function (i) {
    if (i >= 0 && i < VIS.list.length) VIS.list.splice(i, 1);
  });
  VIS.sel = null; VIS.anchor = -1;
  if (VIS.idx >= VIS.list.length) VIS.idx = Math.max(0, VIS.list.length - 1);
  VIS.result = null;
  visMsg(T('visDelDone').replace('{n}', String(n)));
  visPersist();
  if (!VIS.list.length) {
    visRefreshView();
    G.board = newBoard(); G.moves = []; paint();
    return;
  }
  visRefreshView();
  var it = VIS.list[VIS.idx];
  if (it && it.t === 'code') visShowMoves(it.moves);
}

function visImageExt(d) {
  var m = /^data:image\/([a-z0-9.+-]+)/i.exec(String(d || ''));
  var e = m ? m[1].toLowerCase() : 'png';
  if (e === 'jpeg' || e === 'jpg') e = 'jpg';
  else if (e === 'svg+xml') e = 'svg';
  else if (e.indexOf('+') >= 0) e = 'png';        
  return e;
}

function visOpenImage(i) {
  var it = VIS.list[i];
  if (!it || it.t !== 'img' || !it.d) return false;
  var ext = visImageExt(it.d);
  if (HOST) {
    tellHost({ type: 'openImage', idx: i + 1, ext: ext, data: it.d });
    return true;
  }
  try {
    var a = document.createElement('a');
    a.href = it.d;
    a.download = 'gomoku-vision-' + (i + 1) + '.' + ext;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
  } catch (e) { return false; }
  return true;
}

function visCopyImage(idx) {
  var it = (typeof idx === 'number' && VIS.list[idx]) ? VIS.list[idx] : null;
  if (!it && VIS.sel && VIS.sel.size) {
    var first = Array.from(VIS.sel).sort(function (a, b) { return a - b; })[0];
    it = VIS.list[first] || null;
  }
  if (!it) it = VIS.list[VIS.idx];
  if (!it || it.t !== 'img') { visMsg(T('visCopyFail')); return; }
  var im = new Image();
  im.onload = function () {
    try {
      var cv = document.createElement('canvas');
      cv.width = im.naturalWidth; cv.height = im.naturalHeight;
      cv.getContext('2d').drawImage(im, 0, 0);
      cv.toBlob(function (blob) {
        if (!blob) { visMsg(T('visCopyFail')); return; }
        try {
          navigator.clipboard.write([new ClipboardItem({ 'image/png': blob })])
            .then(function () { visMsg(T('visCopied')); })
            .catch(function () { visMsg(T('visCopyFail')); });
        } catch (e) { visMsg(T('visCopyFail')); }
      }, 'image/png');
    } catch (e) { visMsg(T('visCopyFail')); }
  };
  im.src = it.d;
}

function visRenderDrawer() {
  if (!els.vdList || !els.visDrawer || els.visDrawer.hidden) return;
  els.vdList.textContent = '';
  var sel = VIS.sel || new Set();
  var selMode = !!(els.btn_vd_sel && els.btn_vd_sel.classList.contains('accent'));
  VIS.list.forEach(function (it, i) {
    var d = document.createElement('button');
    d.type = 'button';
    d.setAttribute('data-idx', String(i));
    d.className = 'vd-item' + (i === VIS.idx ? ' sel' : '') + (sel.has(i) ? ' chk' : '');
    if (it.t === 'img') {
      var im = document.createElement('img');
      im.src = it.d; im.alt = '';
      d.appendChild(im);
    } else {
      var c = document.createElement('span');
      c.className = 'vd-code';
      c.textContent = it.code;
      d.appendChild(c);
    }
    var n = document.createElement('span');
    n.className = 'idx';
    n.textContent = String(i + 1);
    d.appendChild(n);
    var chk = document.createElement('span');
    chk.className = 'vd-chk';
    chk.textContent = '✓';
    d.appendChild(chk);
    var del = document.createElement('span');
    del.className = 'vd-del';
    del.textContent = '✕';
    del.title = T('visDelTitle');
    del.onclick = function (ev) { ev.stopPropagation(); visRemoveAt(i); };
    d.appendChild(del);
    d.onclick = function (ev) { visItemClick(ev, i); };
    
    d.ondblclick = function (ev) { ev.preventDefault(); ev.stopPropagation(); visOpenImage(i); };
    if (it.t === 'img') d.title = T('visOpenTip');
    d.oncontextmenu = function (ev) {
      ev.preventDefault();
      ev.stopPropagation();                    
      visShowMenu(ev, [
        [T('visSel') + (sel.has(i) ? ' ✓' : ''), false, function () {
          var s = visSelSet();
          if (s.has(i)) s.delete(i); else { s.add(i); VIS.anchor = i; }
          visRenderDrawer();
        }],
        [T('visSelAll'), false, function () {
          var s = visSelSet();
          VIS.list.forEach(function (_, k) { s.add(k); });
          visRenderDrawer();
        }],
        
        [T('visOpenApp'), false, function () { visOpenImage(i); }],
        [T('visCopy'), false, function () { visCopyImage(i); }],
        [T('visDelSel'), true, function () {
          var s = visSelSet();
          s.clear(); s.add(i);
          visRemoveSelected();
        }],
      ]);
    };
    d._selMode = selMode;
    els.vdList.appendChild(d);
  });
  if (els.vdCount) els.vdCount.textContent = String(VIS.list.length);
}

function visItemClick(ev, i) {
  var sel = visSelSet();
  var selMode = !!(els.btn_vd_sel && els.btn_vd_sel.classList.contains('accent'));
  if (ev.shiftKey && VIS.anchor >= 0) {
    var lo = Math.min(VIS.anchor, i), hi = Math.max(VIS.anchor, i);
    for (var k = lo; k <= hi; k++) sel.add(k);
  } else if (ev.ctrlKey || ev.metaKey || selMode) {
    if (sel.has(i)) sel.delete(i);
    else { sel.add(i); VIS.anchor = i; }
  } else {
    if (sel.size) sel.clear();                 
    VIS.anchor = i;
    if (VIS.idx !== i) VIS.idx = i;
  }
  visRefreshView();
}

function visShowMenu(ev, items) {
  var m = els.ctxMenu;
  if (!m) return;
  m.innerHTML = '';
  items.forEach(function (it) {
    var b = document.createElement('button');
    b.type = 'button';
    b.textContent = it[0];
    if (it[1]) b.className = 'danger';
    b.onclick = function (e) { e.stopPropagation(); closeCtx(); it[2](); };
    m.appendChild(b);
  });
  m.hidden = false;
  var w = m.offsetWidth, h = m.offsetHeight;               
  m.style.left = Math.max(6, Math.min(ev.clientX, window.innerWidth - w - 6)) + 'px';
  m.style.top = Math.max(6, Math.min(ev.clientY, window.innerHeight - h - 6)) + 'px';
}

function visAddFiles(files) {
  var arr = [].slice.call(files || []);
  if (!arr.length) return;
  visMakeRoom();
  var pending = Math.min(arr.length, VIS_LIST_MAX), done = 0, firstAt = VIS.list.length;
  arr.slice(0, pending).forEach(function (f) {
    var rd = new FileReader();
    rd.onload = function () {
      visMakeRoom();                          
      VIS.list.push({ t: 'img', d: String(rd.result || '') });
      if (++done === pending) {
        VIS.idx = firstAt;                       
        if (VIS.idx >= VIS.list.length) VIS.idx = VIS.list.length - 1;
        visPersist();
        visRefreshView();
        visMsg('');
      }
    };
    rd.readAsDataURL(f);
  });
}

var VIS_REC_TO = 12000;
function visClearWatchdog() { if (VIS.wd) { clearTimeout(VIS.wd); VIS.wd = 0; } }
function visArmWatchdog() {
  visClearWatchdog();
  VIS.wd = setTimeout(function () {
    VIS.wd = 0;
    if (!VIS.busy) return;
    VIS.busy = false; VIS.queued = false;
    if (els.btn_vis_rec) els.btn_vis_rec.disabled = false;
    if (HOST) tellHost({ type: 'status', text: 'VIS timeout ' + VIS_REC_TO + 'ms retry=' + VIS.retry });
    var it = VIS.list[VIS.idx];
    if (it && it.t === 'img' && VIS.retry < 1) {
      VIS.retry++;
      visMsg(T('visRetry'));
      visRecognize();
    } else {
      visMsg(T('visTimeout'));
    }
  }, VIS_REC_TO);
}

function visRecognize() {
  var it = VIS.list[VIS.idx];
  if (!it) { visMsg(T('visNone')); return; }
  if (it.t === 'code') {
    visShowMoves(it.moves);
    var b = 0, w = 0;
    it.moves.forEach(function (m) { if (m[2] === 1) b++; else w++; });
    visMsg(T('visOk').replace('{b}', String(b)).replace('{w}', String(w)));
    return;
  }
  if (VIS.busy) { VIS.queued = true; return; }
  VIS.busy = true;
  VIS.seq++;
  VIS.reqSeq = VIS.seq;
  visArmWatchdog();
  if (els.btn_vis_rec) els.btn_vis_rec.disabled = true;
  visMsg(T('visWorking'));

  tellHost({ type: 'visRecognize', mode: 'image', data: it.d, seq: VIS.reqSeq,
             recog: visRcSnapshot() });
}

function visRecognizeDone() {
  var was = VIS.busy;
  VIS.busy = false;
  VIS.retry = 0;
  visClearWatchdog();
  if (els.btn_vis_rec) els.btn_vis_rec.disabled = false;
  if (was && VIS.queued) { VIS.queued = false; visAutoRecognize(); }
}

function visHandleResult(m) {
  visRecognizeDone();
  if (typeof m.seq === 'number' && m.seq !== VIS.seq) return;   
  var r = m.result || {};
  if (!r.ok) { visMsg(T('visBad') + (r.err ? ' (' + r.err + ')' : '')); return; }
  var bl = r.black || [], wh = r.white || [];
  if (!bl.length && !wh.length) {
    if (VIS.result) { VIS.result = null; visShowMoves([]); }
    visMsg(T('visNoStones'));
    return;
  }
  VIS.result = { black: bl, white: wh };
  visShowResult(r);
  var msg = (r.suspect ? T('visSuspect') : '') + (r.partial ? T('visPartial') : '') +
            T('visOk').replace('{b}', String(bl.length)).replace('{w}', String(wh.length));
  var it = VIS.list[VIS.idx];
  if (it && it.t === 'img' && it.w && Math.min(it.w, it.h) < 320) {
    msg += ' ' + T('visLowRes').replace('{w}', String(it.w)).replace('{h}', String(it.h));
  }
  visMsg(msg);
  visFillGuide();                          
  
  if (visGate()) visMsg(T('visCountBad')
    .replace('{b}', String(bl.length)).replace('{w}', String(wh.length)));
  else if (VIS.vc && VIS.gateBad) visMsg(T('visVcPass')
    .replace('{b}', String(bl.length)).replace('{w}', String(wh.length)));   
}

function visLoadPractice() {
  if (!VIS.result) { visMsg(T('visNone')); return; }
  if (visGate()) { visMsg(T('visCountBad')
    .replace('{b}', String(visCounts().b)).replace('{w}', String(visCounts().w))); return; }  
  if (HOST) {
    tellHost({ type: 'visToTrainer', payload: { src: 'vis', black: VIS.result.black, white: VIS.result.white, vc: !!VIS.vc } });   
    visMsg(T('visLoaded'));
  } else {
    try { localStorage.setItem('gbcalc.vis.pending', JSON.stringify({ black: VIS.result.black, white: VIS.result.white, vc: !!VIS.vc })); } catch (e) {}   
    window.open('calc.html');
  }
}

function visCounts() {
  var b = 0, w = 0;
  for (var y = 0; y < N; y++)
    for (var x = 0; x < N; x++) {
      if (G.board[y][x] === 1) b++;
      else if (G.board[y][x] === 2) w++;
    }
  return { b: b, w: w };
}

function visSyncResult() {
  var mv = [], bl = [], wh = [];
  for (var y = 0; y < N; y++)
    for (var x = 0; x < N; x++) {
      var v = G.board[y][x];
      if (!v) continue;
      mv.push({ x: x, y: y, c: v });
      if (v === 1) bl.push({ x: x, y: y }); else wh.push({ x: x, y: y });
    }
  G.moves = mv;
  VIS.result = { black: bl, white: wh };
  visFillGuide();                          
  visUpdateCode();                         
  visGate();                               
}

function visEditHint(t) { visMsg(t); }

function visFillGuide() {
  if (!els.btn_ve_fillb || !els.btn_ve_fillw) return;
  var c = visCounts();
  
  var canB = VIS.vc ? true : (c.b <= c.w);   
  var canW = VIS.vc ? true : (c.w < c.b);    
  els.btn_ve_fillb.disabled = !canB;
  els.btn_ve_fillw.disabled = !canW;
  els.btn_ve_fillb.classList.toggle('accent', canB);   
  els.btn_ve_fillw.classList.toggle('accent', canW);
  if (VIS.edit && !VIS.vc && !VIS.pick) {
    if (c.w < c.b) visEditHint(T('veFillGuideW').replace('{d}', String(c.b - c.w)));
    else if (c.b < c.w) visEditHint(T('veFillGuideB').replace('{d}', String(c.w - c.b)));
  }
}

function visEditExitPick() {
  VIS.pick = null; VIS.delPts = []; VIS.swapSel = null; VIS.ghosts = []; G.missRings = [];
  
  if (els.btn_ve_addb) els.btn_ve_addb.classList.remove('sel-b');
  if (els.btn_ve_addw) els.btn_ve_addw.classList.remove('sel-w');
  paint();
}

function visEditToggle() {
  VIS.edit = !VIS.edit;
  if (!VIS.edit) VIS.undoStack = [];   
  if (!VIS.edit) { VIS.addOpen = false; if (els.btn_ve_add) els.btn_ve_add.classList.remove('add-on'); }
  if (els.visEditBar) els.visEditBar.hidden = !VIS.edit;
  if (els.btn_vis_edit) els.btn_vis_edit.classList.toggle('accent', VIS.edit);
  if (els.veAddKeys) els.veAddKeys.hidden = true;
  if (VIS.edit) {
    var c = visCounts();
    
    visEditHint(VIS.vc ? T('visVcEditTip')
      : T('veEditOn') + ' · ' + T('visOk').replace('{b}', String(c.b)).replace('{w}', String(c.w)));
  } else {
    visEditHint('');
    visEditExitPick();
  }
  paint();
}

function visEditSwap() {
  if (VIS.pick === 'swap') { visEditExitPick(); visEditHint(T('veEditOn')); return; }
  VIS.pick = 'swap'; VIS.swapSel = null; VIS.delPts = []; VIS.ghosts = []; G.missRings = [];
  visEditHint(T('veSwapHint'));
  paint();
}

function visEditPick(kind) {
  
  var c0 = visCounts();
  if (!VIS.vc) {
    if (kind === 'fillb' && c0.b > c0.w) { visEditHint(T('veFillBFull')); visFillGuide(); return; }
    if (kind === 'fillw' && c0.w >= c0.b) { visEditHint(T('veFillWFull')); visFillGuide(); return; }
  }
  VIS.pick = kind; VIS.delPts = []; VIS.swapSel = null; VIS.ghosts = []; G.missRings = [];
  
  if (els.btn_ve_addb) els.btn_ve_addb.classList.toggle('sel-b', kind === 'addb');
  if (els.btn_ve_addw) els.btn_ve_addw.classList.toggle('sel-w', kind === 'addw');
  visEditHint(kind === 'del' ? T(VIS.vc ? 'veDelVcHint' : 'veDelHint')
    : kind === 'fillb' ? T(VIS.vc ? 'veFillVcHint' : 'veFillBHint')
    : kind === 'fillw' ? T(VIS.vc ? 'veFillVcHint' : 'veFillWHint')
    : kind === 'addb' ? T('veAddBHint') : T('veAddWHint'));
  paint();
}

function visAddToggle() {
  VIS.addOpen = !VIS.addOpen;
  if (els.btn_ve_add) els.btn_ve_add.classList.toggle('add-on', VIS.addOpen);
  if (els.veAddKeys) els.veAddKeys.hidden = !VIS.addOpen;
  if (VIS.addOpen) visEditPick('addb');
  else if (VIS.pick === 'addb' || VIS.pick === 'addw') visEditExitPick();
}

function visSnapPush() {
  if (!VIS.edit) return;
  VIS.undoStack.push(G.board.map(function (row) { return row.slice(); }));
  if (VIS.undoStack.length > 100) VIS.undoStack.shift();
}

function visUndo() {
  if (!VIS.edit || !VIS.undoStack.length) {
    if (VIS.edit) visEditHint(T('veUndoNone'));
    return;
  }
  G.board = VIS.undoStack.pop();
  VIS.ghosts = []; VIS.delPts = []; VIS.swapSel = null; G.missRings = [];
  visSyncResult();
  var c = visCounts();
  visEditHint(T('veEdited').replace('{b}', String(c.b)).replace('{w}', String(c.w)));
  paint();
}

function visGate() {
  var c = visCounts();
  var bad = (c.w > c.b) || (c.b - c.w > 1);
  VIS.gateBad = bad;                     
  if (VIS.vc) bad = false;               
  if (els.btn_vis_load) els.btn_vis_load.disabled = bad;
  return bad;
}

function visVcToggle() {
  VIS.vc = !VIS.vc;
  if (els.btn_vis_vc) els.btn_vis_vc.classList.toggle('accent', VIS.vc);
  visGate();
  visFillGuide();                        
  
  visMsg(T(VIS.vc ? 'visVcEditTip' : 'visVcOff'));
}

function visGhostsCommit() {
  var gs = VIS.ghosts || [];
  VIS.ghosts = [];
  if (gs.length < 2) { paint(); return; }      
  visSnapPush();                               
  gs.forEach(function (p) { G.board[p.y][p.x] = p.c; });
  visSyncResult();
  var c = visCounts();
  visEditHint(T('veEdited').replace('{b}', String(c.b)).replace('{w}', String(c.w)));
  paint();
}

function visBoardClick(ev) {
  if (!VIS.edit || !VIS.pick) return;
  var rect = els.board.getBoundingClientRect();
  var g = geom();
  var hit = cellAt(ev.clientX - rect.left, ev.clientY - rect.top);   
  var x = hit ? hit.x : -1, y = hit ? hit.y : -1;
  var inside = x >= 0 && y >= 0 && x < N && y < N;

  if (VIS.pick === 'addb' || VIS.pick === 'addw') {
    var startC = (VIS.pick === 'addb') ? 1 : 2;
    if (!inside) { visGhostsCommit(); return; }              
    if (G.board[y][x]) return;                               
    var gs = VIS.ghosts;
    if (gs.length >= 2) {                                    
      visGhostsCommit();
      if (!G.board[y][x]) { VIS.ghosts = [{ x: x, y: y, c: startC }]; visEditHint(T('veGhost2')); }
      paint();
      return;
    }
    if (gs.length === 1 && gs[0].x === x && gs[0].y === y) { VIS.ghosts = []; paint(); return; } 
    gs.push({ x: x, y: y, c: gs.length ? (3 - startC) : startC });
    visEditHint(gs.length === 1
      ? T('veGhost1').replace('{c}', startC === 1 ? T('veGhostB') : T('veGhostW'))
      : T('veGhost2'));
    paint();
    return;
  }

  if (!inside) return;

  if (VIS.pick === 'fillb' || VIS.pick === 'fillw') {
    if (G.board[y][x]) return;                               
    var fc = (VIS.pick === 'fillb') ? 1 : 2;
    var fc0 = visCounts();
    
    if (!VIS.vc) {
      if (fc === 1 && fc0.b > fc0.w) { visEditHint(T('veFillBFull')); visFillGuide(); return; }
      if (fc === 2 && fc0.w >= fc0.b) { visEditHint(T('veFillWFull')); visFillGuide(); return; }
    }
    visSnapPush();                             
    G.board[y][x] = fc;
    visSyncResult();
    var fc1 = visCounts();
    visEditHint(T('veEdited').replace('{b}', String(fc1.b)).replace('{w}', String(fc1.w)));
    paint();
    return;
  }

  if (VIS.pick === 'del') {
    if (!G.board[y][x]) return;
    var col = G.board[y][x];
    
    if (VIS.vc) {
      visSnapPush();                             
      G.board[y][x] = 0;
      visSyncResult(); VIS.delPts = []; G.missRings = [];
      var cvc = visCounts();
      visEditHint(T('veDelDone').replace('{x}', '1').replace('{b}', String(cvc.b)).replace('{w}', String(cvc.w)));
      paint();
      return;
    }
    var cnt0 = visCounts();
    
    var singleOk = (col === 1) ? (cnt0.b === cnt0.w + 1) : (cnt0.b === cnt0.w);
    var dup = -1;
    for (var i = 0; i < VIS.delPts.length; i++)
      if (VIS.delPts[i].x === x && VIS.delPts[i].y === y) dup = i;
    if (dup >= 0) { VIS.delPts.splice(dup, 1); G.missRings = VIS.delPts.slice(); paint(); return; }
    if (VIS.delPts.length && G.board[VIS.delPts[0].y][VIS.delPts[0].x] !== col) {
      
      visSnapPush();                           
      G.board[VIS.delPts[0].y][VIS.delPts[0].x] = 0;
      G.board[y][x] = 0;
      visSyncResult(); VIS.delPts = []; G.missRings = [];
      var c = visCounts();
      visEditHint(T('veDelDone').replace('{x}', '2').replace('{b}', String(c.b)).replace('{w}', String(c.w)));
      paint();
      return;
    }
    if (singleOk) {
      visSnapPush();                             
      G.board[y][x] = 0;
      visSyncResult(); VIS.delPts = []; G.missRings = [];
      var c1 = visCounts();
      visEditHint(T('veDelDone').replace('{x}', '1').replace('{b}', String(c1.b)).replace('{w}', String(c1.w)));
      paint();
      return;
    }
    VIS.delPts = [{ x: x, y: y }];             
    G.missRings = VIS.delPts.slice();
    visEditHint(T('veDelPair'));
    paint();
    return;
  }

  if (VIS.pick === 'swap') {
    if (!G.board[y][x]) return;
    var col2 = G.board[y][x];
    if (!VIS.swapSel) {
      VIS.swapSel = { x: x, y: y };
      visEditHint(T('veSwapNext'));
      paint();
      return;
    }
    if (VIS.swapSel.x === x && VIS.swapSel.y === y) { VIS.swapSel = null; paint(); return; } 
    var s = VIS.swapSel, sc = G.board[s.y][s.x];
    if (sc === col2) { VIS.swapSel = { x: x, y: y }; paint(); return; }  
    visSnapPush();                               
    G.board[s.y][s.x] = col2;                    
    G.board[y][x] = sc;
    visSyncResult();
    VIS.swapSel = null;
    var c3 = visCounts();
    visEditHint(T('veSwapDone').replace('{b}', String(c3.b)).replace('{w}', String(c3.w)));
    paint();
    return;
  }
}

function visBoardCtx(ev) {
  if (!VIS.edit) return;
  ev.preventDefault();
  ev.stopPropagation();                        
  visShowMenu(ev, [
    [T('veSwap'), false, visEditSwap],
    [T('veDel'), false, function () { visEditPick('del'); }],
    [T('veFillB'), false, function () { visEditPick('fillb'); }],
    [T('veFillW'), false, function () { visEditPick('fillw'); }],
    [T('veAdd'), false, function () {
      VIS.addOpen = true;                        
      if (els.btn_ve_add) els.btn_ve_add.classList.add('add-on');
      if (els.veAddKeys) els.veAddKeys.hidden = false;
      visEditHint(T('veAddBHint') + ' / ' + T('veAddWHint'));
    }],
    [T('veUndo'), false, visUndo],               
  ]);
}

function bootVis() {
  try {
    var st = JSON.parse(localStorage.getItem('gbcalc.settings.v1') || '{}');
    if (st && st.theme) S.theme = (st.theme === 'light') ? 'light' : 'dark';
    if (st && st.lang) S.lang = (st.lang === 'en') ? 'en' : 'zh';
    if (st && st.boardColor) S.boardColor = st.boardColor;
    if (st && st.pageBgColor) S.pageBgColor = st.pageBgColor;
    if (st && st.cssVars) { S.cssVars = st.cssVars; applyColorVars(st.cssVars); }
  } catch (e) {}
  document.body.setAttribute('data-theme', S.theme);
  document.body.classList.add('vis');
  document.title = T('visTitle');
  grab();
  if (!G.board) G.board = newBoard();
  els.visPane.hidden = false;
  els.btn_vis_upload.onclick = function () { els.file_vis.click(); };
  els.file_vis.onchange = function () {
    visAddFiles(els.file_vis.files);
    els.file_vis.value = '';                    
  };
  els.btn_vis_shot.onclick = visTakeShot;
  
  if (els.btn_vis_crop) els.btn_vis_crop.onclick = cropOpen;
  wireCrop();
  els.btn_vis_prev.onclick = function () { if (VIS.idx > 0) { VIS.idx--; visRefreshView(); } };
  els.btn_vis_next.onclick = function () { if (VIS.idx < VIS.list.length - 1) { VIS.idx++; visRefreshView(); } };
  
  if (els.visImg) {
    els.visImg.ondblclick = function () { visOpenImage(VIS.idx); };
    els.visImg.title = T('visOpenTip');
  }
  els.btn_vis_rec.onclick = visRecognize;
  if (els.btn_vis_rc) els.btn_vis_rc.onclick = visRcToggle;
  els.btn_vis_load.onclick = visLoadPractice;
  if (els.btn_vis_vc) els.btn_vis_vc.onclick = visVcToggle;   
  
  els.btn_vis_del.onclick = visRemoveCurrent;
  
  els.visPos.onclick = function () {
    if (!els.visDrawer) return;
    els.visDrawer.hidden = !els.visDrawer.hidden;
    visRenderDrawer();
  };
  if (els.btn_vd_close) els.btn_vd_close.onclick = function () {
    if (els.visDrawer) els.visDrawer.hidden = true;
  };
  
  if (els.btn_vd_sel) els.btn_vd_sel.onclick = function () {
    var on = els.btn_vd_sel.classList.toggle('accent');
    els.btn_vd_sel.textContent = on ? T('visSelOn') : T('visSelOff');
    visMsg(on ? T('visSelTip') : '');
  };
  if (els.btn_vd_all) els.btn_vd_all.onclick = function () {
    var sel = visSelSet();
    VIS.list.forEach(function (_, i) { sel.add(i); });
    visRenderDrawer();
    visMsg(T('visSelTip'));
  };
  if (els.btn_vd_del) els.btn_vd_del.onclick = visRemoveSelected;
  
  if (els.btn_vis_edit) els.btn_vis_edit.onclick = visEditToggle;
  if (els.btn_ve_swap) els.btn_ve_swap.onclick = visEditSwap;
  if (els.btn_ve_del) els.btn_ve_del.onclick = function () { visEditPick('del'); };
  if (els.btn_ve_add) els.btn_ve_add.onclick = function () { visAddToggle(); };   
  if (els.btn_ve_addb) els.btn_ve_addb.onclick = function () { visEditPick('addb'); };
  if (els.btn_ve_addw) els.btn_ve_addw.onclick = function () { visEditPick('addw'); };
  if (els.btn_ve_undo) els.btn_ve_undo.onclick = function () { visUndo(); };      
  
  document.addEventListener('keydown', function (e) {
    if (!(e.ctrlKey || e.metaKey) || String(e.key).toLowerCase() !== 'z') return;
    if (!VIS.edit) return;
    var t = e.target;
    if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA')) return;
    e.preventDefault();
    visUndo();
  });
  
  if (els.btn_ve_fillb) els.btn_ve_fillb.onclick = function () { visEditPick('fillb'); };
  if (els.btn_ve_fillw) els.btn_ve_fillw.onclick = function () { visEditPick('fillw'); };
  
  if (els.btn_vis_codecopy) {
    els.btn_vis_codecopy.onclick = function () {
      var txt = els.visCode ? els.visCode.value : '';
      if (!txt) return;
      var done = function () { visMsg(T('visCodeCopied')); };
      var legacy = function () {
        try {
          els.visCode.removeAttribute('readonly');
          els.visCode.select();
          document.execCommand('copy');
          els.visCode.setAttribute('readonly', '');
          window.getSelection && window.getSelection().removeAllRanges();
        } catch (e) {}
        done();
      };
      if (navigator.clipboard && navigator.clipboard.writeText)
        navigator.clipboard.writeText(txt).then(done, legacy);
      else legacy();
    };
  }
  els.board.addEventListener('click', visBoardClick);
  els.board.addEventListener('contextmenu', visBoardCtx);
  
  bindBoardHover();
  
  document.addEventListener('click', function (e) {
    if (els.ctxMenu && !els.ctxMenu.hidden && !els.ctxMenu.contains(e.target)) closeCtx();
  });
  document.addEventListener('contextmenu', function (e) {
    if (e.defaultPrevented) return;            
    var onItem = !!(e.target && e.target.closest && e.target.closest('#ctxMenu'));
    if (els.ctxMenu && !els.ctxMenu.hidden && !onItem) closeCtx();
  });
  
  if (els.visDrawer) els.visDrawer.addEventListener('click', function (ev) {
    var t = ev.target;
    if (t === els.visDrawer || t === els.vdList || (t && t.id === 't_vdTip')) {
      els.file_vis.click();
    }
  });
  document.addEventListener('keydown', function (ev) {
    if (ev.key === 'Escape') {
      if (els.ctxMenu && !els.ctxMenu.hidden) closeCtx();
      if (VIS.edit && VIS.pick) { visEditExitPick(); visEditHint(T('veEditOn')); return; }
      if (els.visDrawer && !els.visDrawer.hidden) { els.visDrawer.hidden = true; }
      return;
    }
    
    if ((ev.ctrlKey || ev.metaKey) && (ev.key === 'a' || ev.key === 'A') &&
        els.visDrawer && !els.visDrawer.hidden &&
        document.activeElement !== els.visInput) {
      ev.preventDefault();
      var sel = visSelSet();
      VIS.list.forEach(function (_, i) { sel.add(i); });
      visRenderDrawer();
    }
  });
  
  window.addEventListener('resize', function () { fitFont(); layoutBoard(); });
  if (window.ResizeObserver) {
    new ResizeObserver(function () { fitFont(); layoutBoard(); }).observe(els.boardWrap);
  }
  
  els.visInput.addEventListener('paste', function (ev) {
    var items = (ev.clipboardData && ev.clipboardData.items) || [];
    var hit = null;
    for (var i = 0; i < items.length; i++) {
      if (items[i] && items[i].type && items[i].type.indexOf('image/') === 0) { hit = items[i]; break; }
    }
    if (hit) {
      ev.preventDefault();
      var f = hit.getAsFile();
      if (!f) return;
      var rd = new FileReader();
      rd.onload = function () {
        if (visAddImage(String(rd.result || ''), true)) visMsg('');
        else visMsg(T('visNone'));
      };
      rd.readAsDataURL(f);
      return;
    }
    var txt = ev.clipboardData ? ev.clipboardData.getData('text') : '';
    if (txt && txt.trim()) {
      ev.preventDefault();
      var cur = VIS.list[VIS.idx];
      visAddCode(txt, cur && cur.t === 'code' ? 'replace' : undefined);
    }
  });
  els.visInput.addEventListener('keydown', function (ev) {
    if (ev.key !== 'Enter') return;
    ev.preventDefault();
    var txt = els.visInput.value;
    if (!txt.trim()) return;
    var cur = VIS.list[VIS.idx];
    visAddCode(txt, cur && cur.t === 'code' ? 'replace' : undefined);
  });
  
  document.addEventListener('paste', function (ev) {
    if (document.activeElement === els.visInput) return;   
    var items = (ev.clipboardData && ev.clipboardData.items) || [];
    var hit = null;
    for (var i = 0; i < items.length; i++) {
      if (items[i] && items[i].type && items[i].type.indexOf('image/') === 0) { hit = items[i]; break; }
    }
    if (!hit) return;
    ev.preventDefault();
    var f = hit.getAsFile();
    if (!f) return;
    var rd = new FileReader();
    rd.onload = function () {
      if (visAddImage(String(rd.result || ''), true)) visMsg('');
      else visMsg(T('visNone'));
    };
    rd.readAsDataURL(f);
  });
  
  visRestore(function () {
    visRefreshView();
    layoutBoard();
    visLayout();
    refreshUI();
  });
  visRefreshView();
  layoutBoard();
  visLayout();
  refreshUI();

  function visTakeShot() {
    visMsg(T('visShotBusy'));
    tellHost({ type: 'visShot' });
  }

  function onVisHostMsg(ev) {
    var m = ev.data; if (!m) return;
    if (m.type === 'visShotData') { visAddImage(m.data, true); visMsg(''); }
    else if (m.type === 'visShotFail') visMsg(T('visShotFail'));
    else if (m.type === 'visShotCancel') visMsg('');
    else if (m.type === 'visResult') visHandleResult(m);
    else if (m.type === 'visRecog') visRcFill(m.recog);
    else if (m.type === 'uiTheme') {
      S.theme = (m.theme === 'light') ? 'light' : 'dark';
      document.body.setAttribute('data-theme', S.theme);
      if (m.cssVars !== undefined) { S.cssVars = m.cssVars || null; applyColorVars(S.cssVars); }
      paint();
    }
  }

  if (HOST) {
    window.chrome.webview.addEventListener('message', function (ev) {
      tellHostDefer = true;
      try { onVisHostMsg(ev); } finally { tellHostDefer = false; }
    });
    window.chrome.webview.postMessage({ type: 'ready', theme: themeKey(), bg: pageBgHex(), lang: S.lang });
  }
  applyLang();
  bootDone();
}

function openVisWindow() {
  if (HOST) { tellHost({ type: 'openVis' }); return true; }
  var w = window.open('calc.html?vis=1', 'gb_vis');
  return !!w;
}

function pageBgHex() {
  var c = getComputedStyle(document.body).backgroundColor;
  if (!c || c === 'transparent' || /rgba\(\s*0,\s*0,\s*0,\s*0\s*\)/.test(c)) {
    c = getComputedStyle(document.documentElement).backgroundColor;
  }
  var m = /rgba?\((\d+),\s*(\d+),\s*(\d+)/.exec(c || '');
  if (!m) return '';
  function h(n) { return ('0' + (+n).toString(16)).slice(-2); }
  return '#' + h(m[1]) + h(m[2]) + h(m[3]);
}

function bootReview() {
  
  try {
    var rvst = JSON.parse(localStorage.getItem('gbcalc.settings.v1') || '{}');
    if (rvst && rvst.theme) S.theme = (rvst.theme === 'light') ? 'light' : 'dark';
    
    if (rvst && rvst.boardColor) S.boardColor = rvst.boardColor;
    if (rvst && rvst.pageBgColor) S.pageBgColor = rvst.pageBgColor;
    if (rvst && rvst.cssVars) { S.cssVars = rvst.cssVars; applyColorVars(rvst.cssVars); }
    
    if (rvst && typeof rvst.previewOn === 'boolean') S.previewOn = rvst.previewOn;
    
    if (rvst && typeof rvst.rvNum === 'boolean') S.rvNum = rvst.rvNum;
    
    if (rvst && (rvst.lang === 'zh' || rvst.lang === 'en')) S.lang = rvst.lang;
  } catch (e) {}
  
  window.addEventListener('storage', function (ev) {
    if (ev.key !== 'gbcalc.settings.v1' || !ev.newValue) return;
    try {
      var st = JSON.parse(ev.newValue);
      if (typeof st.previewOn === 'boolean' && st.previewOn !== S.previewOn) {
        S.previewOn = st.previewOn;
        paint();
      }
    } catch (e) {}
  });
  document.body.setAttribute('data-theme', S.theme);
  document.body.classList.add('rv');
  document.title = T('rvWindow');
  grab();
  if (!G.board) G.board = newBoard();
  
  els.rvBar.hidden = false;
  els.board.addEventListener('click', onRvBoardClick);
  
  els.board.addEventListener('contextmenu', function (ev) {
    ev.preventDefault();
    if (G.review) { rvGoTo(G.review.k - 1); return; }
    undoOne();
  });
  bindBoardHover();          
  window.addEventListener('resize', function () { fitFont(); layoutBoard(); visLayout(); });
  if (window.ResizeObserver) {
    new ResizeObserver(function () { fitFont(); layoutBoard(); }).observe(els.boardWrap);
  }

  els.btn_recite.onclick = startRecite;
  els.btn_replay.onclick = startReplay;
  els.btn_rv_prev.onclick = function () { rvGoTo(G.review ? G.review.k - 1 : 0); };
  els.btn_rv_next.onclick = function () {
    rvGoTo(G.review ? G.review.k + 1 : 0);
  };
  
  els.btn_rv_num.onclick = function () { S.rvNum = !S.rvNum; save(); syncRvNumBtn(); paint(); };
  
  els.btn_rv_play.onclick = function () { rvSetPlaying(!G.rvPlaying); };
  els.rvSpeed.onchange = function () {
    S.rvSpeedSec = +els.rvSpeed.value || 1;
    save();
    if (G.rvPlaying) rvSetPlaying(true);           
  };
  els.btn_rv_exit.onclick = exitReview;
  
  window.addEventListener('keydown', function (e) {
    if (!RV_MODE) return;                    
    var t = e.target;
    if (t && t.tagName && /^(INPUT|SELECT|TEXTAREA)$/.test(t.tagName)) return;
    var kc = String(e.key || '').toLowerCase();
    var mod = e.ctrlKey || e.metaKey;
    
    if (!G.review) {
      if (e.key === 'ArrowLeft' || (mod && kc === 'z')) { undoOne(); e.preventDefault(); }
      else if (e.key === 'ArrowRight') { stepForward(); e.preventDefault(); }
      return;
    }
    var handled = true;
    
    if (mod && kc === 'z') {
      rvGoTo(G.review.k - 1);
    } else if (mod && kc === 'r') {
      rvSetPlaying(false);                           
      rvGoTo(0);
    } else if (e.key === 'ArrowLeft') {
      rvGoTo(G.review.k - 1);                         
    } else if (e.key === 'ArrowRight') {
      rvGoTo(G.review.k + 1);
    } else if (e.key === ' ' || e.code === 'Space') {
      if (G.review.kind === 'replay') rvSetPlaying(!G.rvPlaying);   
    } else {
      handled = false;
    }
    if (handled) e.preventDefault();
  });
  els.btn_redo_rv.onclick = redoReview;             
  els.btn_save_rv.onclick = savePos;
  
  els.btn_rv_hist.onclick = function () {
    if (els.drawer.hidden) openDrawer('hist'); else closeDrawer();
  };
  
  wireDrawer();                                     
  wireAbout();

  hideReviewNav();
  setMissBox();
  els.rvMiss.title = T('reciteHint');               
  layoutBoard();
  applyLang();                                          
  refreshUI();

  function onRvHostMsg(ev) {
    var m = ev.data; if (!m) return;
    if (m.type === 'reviewData') reviewLoad(m.record || null);
    
    else if (m.type === 'histTxt') importHist(m.text == null ? '' : String(m.text));
    else if (m.type === 'histExported') toast(T('expDone').replace('{n}', String(m.n || 0)));
    
    else if (m.type === 'uiTheme') {
      S.theme = (m.theme === 'light') ? 'light' : 'dark';
      document.body.setAttribute('data-theme', S.theme);
      
      if (m.cssVars !== undefined) { S.cssVars = m.cssVars || null; applyColorVars(S.cssVars); }
      
      if (m.lang === 'zh' || m.lang === 'en') { S.lang = m.lang; applyLang(); refreshRvUI(); }
      paint();
    }
  }

  if (HOST) {
    window.chrome.webview.addEventListener('message', function (ev) {
      tellHostDefer = true;                    
      try { onRvHostMsg(ev); } finally { tellHostDefer = false; }
    });
    
    window.chrome.webview.postMessage({ type: 'ready', theme: themeKey(), bg: pageBgHex(), lang: S.lang });
  } else {
    
    var txt = null;
    try { txt = localStorage.getItem('gbcalc.review.pending'); } catch (e) {}
    try { localStorage.removeItem('gbcalc.review.pending'); } catch (e) {}
    if (txt) { try { reviewLoad(JSON.parse(txt)); } catch (e) {} }
  }
  bootDone();
}

function reviewLoad(rec) {
  var mv = ((rec && rec.moves) || []).map(function (a) {
    return { x: a[0], y: a[1], c: a[2] || 0 };
  }).filter(function (m) { return m.x != null && m.y != null; });
  var fromHist = !!(rec && rec.hist) && mv.length > 0;
  
  var egFirst = !!(rec && rec.vc) && mv.length > 0;
  
  var egLen = (rec && rec.eg && rec.egLen > 0) ? Math.min(rec.egLen, mv.length) : 0;
  G.rvRecord = rec || null;
  
  G.loaded = fromHist ? { moves: mv, src: (rec && rec.src) || 'local', egLen: egLen } : null;
  G.board = newBoard(); G.moves = []; G.heat = []; G.nums = []; G.curve = [];
  G.missRings = []; G.review = null; G.redo = []; G.over = false;
  if (egFirst || egLen > 0) {
    mv.slice(0, egFirst ? mv.length : egLen).forEach(function (m) {
      var c = m.c || 1;
      G.board[m.y][m.x] = c;
      G.moves.push({ x: m.x, y: m.y, c: c });
    });
  }
  hideReviewNav();
  setMissBox();
  if (els.rvGroup) els.rvGroup.hidden = !(fromHist || egFirst || egLen > 0);
  if (els.btn_recite) els.btn_recite.hidden = egFirst;   
  
  if (HOST) tellHost({ type: 'reviewAck', moves: mv.length });
  paint();
  refreshUI();
}

function refreshRvUI() {
  var rvKind = G.review ? G.review.kind : '';
  if (els.btn_recite) els.btn_recite.classList.toggle('on', rvKind === 'recite');
  if (els.btn_replay) els.btn_replay.classList.toggle('on', rvKind === 'replay');
  if (els.rvBar) els.rvBar.classList.toggle('stepping', rvKind === 'replay');
  els.btn_redo_rv.textContent = T('redo');
  els.btn_save_rv.textContent = T('savePos');
  els.btn_rv_hist.textContent = T('history');     
  syncRvNumBtn();                                 
  
  els.t_history.textContent = T('history');
  els.t_saved.textContent = T('saved');
  els.dr_save.textContent = T('saveSel');
  els.dr_open.textContent = T('openSel');
  els.dr_exp.textContent = T('drExp');
  els.dr_imp.textContent = T('drImp');
  els.dr_del.textContent = T('delSel');
  els.dr_close.textContent = T('closeDr');
  if (els.dr_all) els.dr_all.textContent = drAllSelected() ? T('selNone') : T('selAll');   
  
  if (els.btn_rv_play && !els.btn_rv_play.hidden)
    els.btn_rv_play.textContent = G.rvPlaying ? T('pause') : T('play');
  setMissBox();
}

function onRvBoardClick(ev) {
  var rect = els.board.getBoundingClientRect();
  var g = geom();
  var hit = cellAt(ev.clientX - rect.left, ev.clientY - rect.top);   
  if (!hit) return;
  var x = hit.x, y = hit.y;
  if (G.review && G.review.kind === 'recite') { reciteStep(x, y); return; }
  if (G.review) return;                             
  if (G.board[y][x]) return;                        
  place(x, y);
  refreshUI();
}

function ensureLoadedForReview() {
  return !!(G.loaded && G.loaded.moves && G.loaded.moves.length);
}

function startReplay() {
  if (!ensureLoadedForReview()) return;
  var k0 = (G.loaded.egLen > 0) ? G.loaded.egLen : 0;
  G.review = { kind: 'replay', k: k0, miss: 0, missAt: {} };
  G.board = newBoard(); G.moves = []; G.missRings = [];
  els.btn_rv_prev.hidden = els.btn_rv_next.hidden = els.rvPos.hidden = els.btn_rv_exit.hidden = false;
  
  rvStopPlay();
  if (els.rvSpeed) els.rvSpeed.value = String(rvStepSec());
  if (els.btn_rv_play) els.btn_rv_play.hidden = false;
  if (els.rvSpeed) els.rvSpeed.hidden = false;
  applyReviewStep();
}

function startRecite() {
  if (!ensureLoadedForReview()) return;
  var k0 = (G.loaded.egLen > 0) ? G.loaded.egLen : 0;
  G.review = { kind: 'recite', k: k0, miss: 0, missAt: {} };
  G.board = newBoard(); G.moves = []; G.missRings = [];
  els.btn_rv_prev.hidden = els.btn_rv_next.hidden = els.rvPos.hidden = false;
  
  rvStopPlay();
  if (els.btn_rv_play) els.btn_rv_play.hidden = true;
  if (els.rvSpeed) els.rvSpeed.hidden = true;
  els.btn_rv_exit.hidden = false;
  setMissBox();
  applyReviewStep();
}

function rvStepSec() {
  var v = +S.rvSpeedSec;
  if (!(v >= 0.5) || !(v <= 5)) v = 1;
  return v;
}
function rvStepMs() { return Math.round(rvStepSec() * 1000); }

function rvGoTo(k) {
  if (!G.review) return;
  var total = (G.loaded && G.loaded.moves) ? G.loaded.moves.length : 0;
  var nk = Math.max(0, Math.min(total, k | 0));
  if (nk === G.review.k) return;
  G.review.k = nk;
  applyReviewStep();
  if (G.rvPlaying) rvSetPlaying(true);
}

function syncRvNumBtn() {
  if (!els.btn_rv_num) return;
  els.btn_rv_num.textContent = T('rvNum');
  els.btn_rv_num.classList.toggle('on', !!S.rvNum);
  els.btn_rv_num.title = T('rvNumHint') + ' · ' + T('rvKeyHint');
}
function rvSetPlaying(on) {
  if (!G.review || G.review.kind !== 'replay') { rvStopPlay(); return; }
  G.rvPlaying = !!on;
  if (els.btn_rv_play) {
    els.btn_rv_play.textContent = G.rvPlaying ? T('pause') : T('play');
    els.btn_rv_play.classList.toggle('on', G.rvPlaying);
  }
  clearTimeout(G.rvTimer);
  if (!G.rvPlaying) { G.rvTimer = 0; return; }
  var step = function () {
    if (!G.review || G.review.kind !== 'replay' || !G.rvPlaying) return;
    if (G.review.k >= G.loaded.moves.length) { rvStopPlay(); return; }   
    G.review.k++;
    applyReviewStep();
    G.rvTimer = setTimeout(step, rvStepMs());
  };
  G.rvTimer = setTimeout(step, rvStepMs());
}

function rvStopPlay() {
  G.rvPlaying = false;
  clearTimeout(G.rvTimer);
  G.rvTimer = 0;
  if (els.btn_rv_play) {
    els.btn_rv_play.textContent = T('play');
    els.btn_rv_play.classList.remove('on');
  }
}

function applyReviewStep() {
  if (!G.review) return;
  var mv = G.loaded.moves, k = G.review.k;
  G.board = newBoard(); G.moves = []; G.missRings = [];
  for (var i = 0; i < k && i < mv.length; i++) {
    var m = mv[i];
    G.board[m.y][m.x] = m.c;
    G.moves.push({ x: m.x, y: m.y, c: m.c });
  }
  var n = 0;
  for (var key in G.review.missAt) {
    var idx = +key;
    if (idx < k) { n++; G.missRings.push(G.review.missAt[key]); }
  }
  G.review.miss = n;
  els.rvPos.textContent = k + ' / ' + mv.length;
  setMissBox();
  paint();
  refreshUI();
}
function reciteStep(x, y) {
  var mv = G.loaded.moves, k = G.review.k;
  if (k >= mv.length) return;
  var want = mv[k];
  if (want.x === x && want.y === y) {
    G.review.k++;
    applyReviewStep();
    return;
  }
  
  G.review.missAt[k] = { x: x, y: y };
  G.review.k++;
  applyReviewStep();
}

function exitReview() {
  if (!G.review) return;
  G.review = null;
  G.missRings = [];
  hideReviewNav();
  setMissBox();
  G.board = newBoard(); G.moves = [];
  paint();
  refreshUI();
}

function redoReview() {
  G.review = null; G.missRings = [];
  G.board = newBoard(); G.moves = [];
  G.curve = []; G.heat = []; G.nums = [];
  G.over = false;
  hideReviewNav();
  setMissBox();
  paint();
  refreshUI();
}

function buildCode() {
  return movesToCode(G.moves);
}

function movesToCode(mv) {
  return (mv || []).map(function (m) {
    if (m == null) return '';
    var x = (m.x != null) ? m.x : m[0];
    var y = (m.y != null) ? m.y : m[1];
    if (x == null || y == null) return '';
    return coordName(x, y);
  }).join('');
}

function parseCode(txt) {
  var low = String(txt || '').trim().toLowerCase();
  var toks = low.match(/([a-z])(\d+)/g);
  if (!toks) return [];
  var mv = [], board = newBoard();
  for (var i = 0; i < toks.length; i++) {
    var tm = /([a-z])(\d+)/.exec(toks[i]);
    if (!tm) continue;
    var x = tm[1].charCodeAt(0) - 97;
    var y = N - parseInt(tm[2], 10);
    if (x < 0 || x >= N || y < 0 || y >= N) continue;
    if (board[y][x]) continue;                         
    var c = (mv.length % 2 === 0) ? 1 : 2;
    board[y][x] = c;
    mv.push([x, y, c]);
    if (winAt(board, x, y, c, exactFiveRule(c))) break;  
  }
  return mv;
}

function loadFromCode(txt) {
  var mv = parseCode(txt);
  if (!mv.length) { setStat(null, T('codeBad')); return false; }
  openRecord({ ts: Date.now(), src: 'local', rule: S.rule, first: 'b', moves: mv });
  setStat(null, T('codeLoaded'));
  return true;
}

function applyCodeText(t) {
  var txt = String(t == null ? '' : t).trim();
  if (!txt) { setStat(null, T('clipEmpty')); return; }
  els.inp_code.value = txt;
  loadFromCode(txt);
}

function savePos() {
  var url;
  try {
    var code = buildCode();
    if (els.inp_code) els.inp_code.value = code;
    url = els.board.toDataURL('image/png');
  } catch (e) {
    setStat(null, T('posSaveFail') + (e && e.message ? '：' + e.message : ''));
    return;
  }
  var name = 'gomoku-' + G.moves.length + '-' + Date.now() + '.png';
  if (HOST) {
    tellHost({ type: 'savePng', name: name, data: url });    
    return;
  }
  try {
    var a = document.createElement('a');
    a.href = url;
    a.download = name;
    document.body.appendChild(a); a.click();
    setTimeout(function () { a.remove(); }, 4000);
    setStat(null, T('posSaved'));
  } catch (e2) {
    setStat(null, T('posSaveFail') + (e2 && e2.message ? '：' + e2.message : ''));
  }
}

function undoOne() {
  if (G.review || openActive() || !G.moves.length) return;
  
  if (G.busy) {
    gAiGen++; G.busy = false; G.think = [];
  }
  var m = G.moves.pop();
  if (!m) return;
  G.board[m.y][m.x] = 0;
  G.redo.push([m]);
  G.over = false;                             
  G.curve = G.curve.filter(function (p) { return p.i < G.moves.length; });
  G.heat = []; G.nums = [];
  recomputeOver(); persistGame();
  anaReset();
  liveAnaBoardChanged();
  paint(); refreshUI();
  refreshCoach();
}
function stepBack() {
  if (G.review || openActive() || !G.moves.length) return;
  
  var aiThinking = (S.mode === 'pve' && G.busy && isAiColor(curColor()));
  if (G.busy) {                 
    gAiGen++;                   
    G.busy = false;             
    G.think = [];
  }
  var n = (S.mode === 'pve' && G.moves.length >= 2) ? 2 : 1;
  if (aiThinking) n = 1;        
  var chunk = [];
  for (var i = 0; i < n && G.moves.length; i++) {
    var m = G.moves.pop();
    G.board[m.y][m.x] = 0;
    chunk.unshift(m);
  }
  G.redo.push(chunk);
  G.over = false;                             
  G.curve = G.curve.filter(function (p) { return p.i < G.moves.length; });
  G.heat = []; G.nums = [];
  recomputeOver(); persistGame();
  anaReset();
  liveAnaBoardChanged();
  paint(); refreshUI();
  refreshCoach();
}
function stepForward() {
  if (G.review || openActive() || !G.redo.length) return;
  var chunk = G.redo.pop();
  chunk.forEach(function (m) { place(m.x, m.y, true); });   
  recomputeOver(); persistGame();             
  paint(); refreshUI();
  refreshCoach();
}

function ingestExternal(text) {
  var o;
  try { o = JSON.parse(text); } catch (e) { return false; }
  if (o && Array.isArray(o.records)) { o.records.forEach(function (r) { r.src = r.src || 'desktop'; addRecord(r); }); renderDrawer(); return true; }
  var mv = null;
  if (o && Array.isArray(o.moves)) {
    mv = o.moves.map(function (a) { return [a[0], a[1], a[2] || 0]; });
  } else if (o && (o.black || o.white)) {
    mv = [];
    var tmp = [];
    (o.black || []).forEach(function (p) { tmp.push({ x: p.x, y: p.y, c: 1 }); });
    (o.white || []).forEach(function (p) { tmp.push({ x: p.x, y: p.y, c: 2 }); });
    var bi = 0, wi = 0;
    var blacks = tmp.filter(function (p) { return p.c === 1; });
    var whites = tmp.filter(function (p) { return p.c === 2; });
    for (var i = 0; i < tmp.length; i++) {
      var takeBlack = (i % 2 === 0);
      var p = takeBlack ? blacks[bi++] : whites[wi++];
      if (!p) p = takeBlack ? whites[wi++] : blacks[bi++];
      if (!p) break;
      mv.push([p.x, p.y, p.c]);
    }
  }
  if (!mv || !mv.length) return false;
  
  if (o.vc) {
    
    S.egLocked = false; S.egBase = null;
    if (S.mode !== 'endgame') {
      S.mode = 'endgame';
      [].forEach.call(els.seg_mode.querySelectorAll('button'), function (x) {
        x.classList.toggle('on', x.getAttribute('data-mode') === 'endgame');
      });
      G.heat = []; G.nums = [];
      save();
    }
  } else if (S.mode !== 'place') {
    S.mode = 'place';
    [].forEach.call(els.seg_mode.querySelectorAll('button'), function (x) {
      x.classList.toggle('on', x.getAttribute('data-mode') === 'place');
    });
    G.heat = []; G.nums = [];
    save();
    refreshUI();
  }
  var rec = { ts: Date.now(), src: o.src || 'desktop', rule: (o.rule != null ? o.rule : S.rule),
              first: 'b', moves: mv, vc: !!o.vc };   
  addRecord(rec);
  openRecord(rec);
  renderDrawer();
  
  if (o.vc) setTimeout(visVcArm, 450);
  return true;
}

var LAY_KEY = 'gbcalc.layout.v7';   
function layKey() { return 'gbcalc.layout.v8'; }   
function loadLayout() {
  try {
    var raw = localStorage.getItem(layKey());
    if (!raw) {
      var cands = ['gbcalc.layout.v7', 'gbcalc.layout.link.v1'];
      for (var i = 0; i < cands.length; i++) {
        var sv = localStorage.getItem(cands[i]);
        if (sv && sv.length > 2) { localStorage.setItem(layKey(), sv); raw = sv; break; }
      }
    }
    return raw ? JSON.parse(raw) : {};
  } catch (e) { return {}; }
}
function saveLayout(l) { try { localStorage.setItem(layKey(), JSON.stringify(l)); } catch (e) {} }

function syncDocks() {
  [els.dockL, els.dockR].forEach(function (dock) {
    if (!dock) return;
    
    var live = [].filter.call(dock.querySelectorAll('.card'), function (c) { return !c.hidden; });
    dock.classList.toggle('has', live.length > 0 || !!dock.querySelector('.cardbar'));
  });
}

function defaultLayout() {
  if (!els.dockR) return;
  
  ['engine'].forEach(function (id) {
    var c = document.querySelector('.card[data-id="' + id + '"]');
    if (c) els.dockL.appendChild(c);
  });
  CARD_IDS.forEach(function (id) {
    var c = document.querySelector('.card[data-id="' + id + '"]');
    if (c && c.parentNode !== els.dockR && c.parentNode !== els.dockL) els.dockR.appendChild(c);
  });
}

function defaultLayoutLink() {
  if (!els.dockL) return;
  ['engine', 'setup', 'analysis'].forEach(function (id) {
    var c = document.querySelector('.card[data-id="' + id + '"]');
    if (c) els.dockL.appendChild(c);
  });
}
// Link mode: collect every card into the left dock, so the empty right dock collapses to
// zero width and the board gets a wider viewport. Mirrors cardOrderBySelect()'s link test,
// so cards opened later from Settings land on the left too.
function cardsToLeftDock() {
  if (!els.dockL) return;
  [].forEach.call(document.querySelectorAll('.card'), function (c) {
    if (c.parentNode !== els.dockL) els.dockL.appendChild(c);
  });
}
function applyLayout() {
  var l = loadLayout();
  var order = l.order || {};
  var fresh = !order.L && !order.R;
  
  ['dockL', 'dockR'].forEach(function (dn) {
    var list = order[dn === 'dockL' ? 'L' : 'R'];
    if (!list || !els[dn]) return;
    list.forEach(function (id) {
      var card = document.querySelector('.card[data-id="' + id + '"]');
      if (card) els[dn].appendChild(card);
    });
  });
  
  if (fresh) { if (linkSessionOn()) defaultLayoutLink(); else defaultLayout(); }
  
  var col = l.collapsed || {};
  [].forEach.call(document.querySelectorAll('.card'), function (card) {
    setCardFolded(card, !!col[card.getAttribute('data-id')], true);
  });
  
  applyCardVis();
}
function persistLayout() {
  var l = { order: {} };
  ['dockL', 'dockR'].forEach(function (dn) {
    l.order[dn === 'dockL' ? 'L' : 'R'] = [].map.call(els[dn].querySelectorAll('.card'),
      function (c) { return c.getAttribute('data-id'); });
  });
  
  l.collapsed = {};
  [].forEach.call(document.querySelectorAll('.card'), function (c) {
    if (c.classList.contains('collapsed')) l.collapsed[c.getAttribute('data-id')] = 1;
  });
  saveLayout(l);
  syncDocks();
  
  buildCardMenu();
}

function setCardFolded(card, on, silent) {
  if (!card) return;
  card.classList.toggle('collapsed', !!on);
  if (!silent) layoutBoard();
}
function toggleCardFold(card) {
  if (!card) return;
  setCardFolded(card, !card.classList.contains('collapsed'));
  persistLayout();
}

var drag = null;
function initPanels() {
  [].forEach.call(document.querySelectorAll('.card'), function (card) {
    var h = card.querySelector('.card-h');
    if (h) h.addEventListener('mousedown', onCardDown);
    
    var dash = card.querySelector('.card-h .dash');
    if (dash) dash.addEventListener('click', function (e) {
      e.stopPropagation();
      toggleCardFold(card);
    });
    
    var pk = card.querySelector('.card-h .pinkey');
    if (pk) pk.addEventListener('click', function (e) {
      e.stopPropagation();
      var id = card.getAttribute('data-id');
      if (CARD_PIN[id]) {                     
        delete CARD_PIN[id];
        savePins();
        var v = cardVis(); v[id] = 0; save();
        applyCardVis(); layoutBoard();
      } else {                                
        CARD_PIN[id] = true;
        savePins();
        pinKeySync(pk, id);
      }
    });
  });
}
function onCardDown(e) {
  var card = e.target.closest('.card');
  if (!card) return;
  
  if (e.target.closest('button, input, select, a, .dash')) return;
  var r = card.getBoundingClientRect();
  var ph = document.createElement('div');
  ph.className = 'ph';
  ph.style.height = r.height + 'px';
  card.parentNode.insertBefore(ph, card);
  card.classList.add('floating');
  card.style.position = 'fixed';
  card.style.left = r.left + 'px';
  card.style.top = r.top + 'px';
  card.style.width = r.width + 'px';
  card.style.zIndex = 60;
  card.style.pointerEvents = 'none';
  document.body.classList.add('dragging');
  drag = { card: card, ph: ph, dx: e.clientX - r.left, dy: e.clientY - r.top };
  document.addEventListener('mousemove', onCardMove);
  document.addEventListener('mouseup', onCardUp);
  e.preventDefault();
}
function onCardMove(e) {
  if (!drag) return;
  drag.card.style.left = (e.clientX - drag.dx) + 'px';
  drag.card.style.top = (e.clientY - drag.dy) + 'px';
  var docks = [els.dockL, els.dockR], tgt = els.dockR, best = 1e9;
  docks.forEach(function (d) {
    var r = d.getBoundingClientRect(), cx = (r.left + r.right) / 2;
    var dist = Math.abs(e.clientX - cx);
    if (dist < best) { best = dist; tgt = d; }
  });
  var kids = [].filter.call(tgt.children, function (c) {
    return c.classList.contains('card') || c.classList.contains('ph');
  });
  var ref = null;
  for (var i = 0; i < kids.length; i++) {
    var r = kids[i].getBoundingClientRect();
    if (e.clientY < (r.top + r.bottom) / 2) { ref = kids[i]; break; }
  }
  if (drag.ph.parentNode !== tgt || drag.ph.nextSibling !== ref) tgt.insertBefore(drag.ph, ref);
}
function onCardUp() {
  if (!drag) return;
  var card = drag.card, ph = drag.ph;
  card.classList.remove('floating');
  card.style.position = ''; card.style.left = ''; card.style.top = '';
  card.style.width = ''; card.style.zIndex = ''; card.style.pointerEvents = '';
  document.body.classList.remove('dragging');
  ph.parentNode.insertBefore(card, ph);
  if (ph.parentNode) ph.parentNode.removeChild(ph);
  document.removeEventListener('mousemove', onCardMove);
  document.removeEventListener('mouseup', onCardUp);
  drag = null;
  layoutBoard(); persistLayout();
}

function applyLang() {
  var d = I18N[S.lang];
  ['title','setup','mode','side','turn','rule','cores','hash','variant','weights','candRange','multiPv','maxDepth','maxNodes','heat','heatHint',
   'showNum','showNumHint','bgThink','preview','previewHint','engine','depth','speed','nodes','time','eval','best','curve',
   'blackCurve','whiteCurve','curveHint','reciteHint','code','drTip',
   'heatBest','heatGood','heatFair','heatPoor','coach','coachHint',
   'swapTtl','swapFirst','swapSecond',
   'secGame','secEngine','secView','secCode'].forEach(function (k) {
    var el = els['t_' + k];
    if (el) el.textContent = d[k];
  });
  try { dashEvalRefresh(); } catch (eEV) {}
  try { if (els.t_bgThinkHint) els.t_bgThinkHint.removeAttribute('data-base'); } catch (eBH1) {}
  try { bgThinkHintSync(); } catch (eBH2) {}
  els.btn_reset.textContent = d.reset;
  els.btn_pause.textContent = S.paused ? '▶' : '❚❚';
  els.btn_pause.title = S.paused ? (aiVsAi() ? d.spResume : d.resume)
                                 : (aiVsAi() ? d.spPause : d.assistHint);
  
  els.btn_prev.title = d.prev;
  els.btn_next.title = d.next;
  els.btn_rot.title = d.rotTtl;
  els.btn_mirror.title = d.mirrorTtl;
  els.btn_shift.title = d.shiftBtnTtl;
  els.t_mirrorTtl.textContent = d.mirrorTtl;
  els.t_shiftTtl.textContent = d.shiftTtl;
  els.btn_mv_fv.textContent = d.mvFv; els.btn_mv_fv.title = d.mvFvT;
  els.btn_mv_fh.textContent = d.mvFh; els.btn_mv_fh.title = d.mvFhT;
  els.btn_mv_d1.textContent = d.mvD1; els.btn_mv_d1.title = d.mvD1T;
  els.btn_mv_d2.textContent = d.mvD2; els.btn_mv_d2.title = d.mvD2T;
  els.btn_mv_close.title = d.closeTtl;
  els.btn_shift_close.title = d.closeTtl;
  els.btn_mv_up.title = d.mvUpT; els.btn_mv_down.title = d.mvDownT;
  els.btn_mv_left.title = d.mvLeftT; els.btn_mv_right.title = d.mvRightT;
  els.btn_save.textContent = d.savePos;
  els.btn_review.textContent = d.review;
  els.btn_copy.textContent = d.copy;
  els.btn_paste.textContent = d.paste;
  els.btn_load.textContent = d.load;
  if (els.btn_vis_codecopy) els.btn_vis_codecopy.textContent = d.copy;   
  els.btn_recite.textContent = d.recite;
  els.btn_replay.textContent = d.replay;
  els.btn_rv_exit.textContent = d.exit;
  els.btn_redo_rv.textContent = d.redo;      
  els.btn_save_rv.textContent = d.savePos;
  
  syncRvNumBtn();
  els.btn_history.textContent = d.openDrawer;
  
  if (!RV_MODE) {
    els.btn_color.textContent = d.color;
    els.t_color.textContent = d.color;
    els.cp_close.textContent = d.cpClose;
    els.cp_reset.textContent = d.cpReset;
    els.t_cpTheme.textContent = d.cpTheme;
    els.t_cpTarget.textContent = d.cpTarget;
    els.t_cpHex.textContent = d.cpHex;
    els.t_cpPreset.textContent = d.cpPreset;
    var ctp = els.cp_theme.querySelectorAll('button');
    if (ctp[0]) ctp[0].textContent = d.theme2;      
    if (ctp[1]) ctp[1].textContent = d.theme;       
    if (ctp[2]) ctp[2].textContent = d.themeCustom; 
    var ctt = els.cp_target.querySelectorAll('button');
    if (ctt[0]) ctt[0].textContent = d.cpBoard;
    if (ctt[1]) ctt[1].textContent = d.cpPage;
    
    if (els.btn_set) els.btn_set.textContent = d.btnSet;
    if (els.btn_aiside) els.btn_aiside.textContent = d.btnAiside;
    if (els.ai_side_b && els.ai_side_b.querySelector('.tgl-txt')) els.ai_side_b.querySelector('.tgl-txt').textContent = d.aiSideB;
    if (els.ai_side_w && els.ai_side_w.querySelector('.tgl-txt')) els.ai_side_w.querySelector('.tgl-txt').textContent = d.aiSideW;
    if (els.t_aiTurn) els.t_aiTurn.textContent = d.aiTurnLbl;
    [].forEach.call(document.querySelectorAll('.pinkey'), function (pk) {
      var cd = pk.closest('.card');
      pinKeySync(pk, cd ? cd.getAttribute('data-id') : '');
    });
    if (els.btn_live) liveAnaUI();          
    els.t_analysis.textContent = d.analysis;
    if (els.t_curveTtl) els.t_curveTtl.textContent = d.curve;   
    [].forEach.call(document.querySelectorAll('.ci-score'), function (el) { el.textContent = d.score; });
    [].forEach.call(document.querySelectorAll('.ci-win'), function (el) { el.textContent = d.winRate; });
    drawCurveInfo();
    els.btn_an_nbest.textContent = (G.ana.busy && G.ana.kind === 'nbest') ? d.anStop : d.anNbest;   
    els.btn_an_defend.textContent = (G.ana.busy && G.ana.kind === 'defend') ? d.anStopScan : d.anDefend;
    els.btn_an_bal.textContent = d.anBal;
    if (els.btn_an_baln) { els.btn_an_baln.value = String(balPN()); els.btn_an_baln.title = d.anBalnHint; }
    
    if (els.btn_fwd) els.btn_fwd.textContent = d.fwd;
    if (els.btn_fwd_ok) els.btn_fwd_ok.textContent = d.fwdOk;
    if (els.fwdLg) {
      els.fwdLg.title = d.fwdLegend;
      
      var lgEms = els.fwdLg.querySelectorAll('em');
      if (lgEms[0]) lgEms[0].textContent = d.fwdBlack;
      if (lgEms[1]) lgEms[1].textContent = d.fwdWhite;
    }
    document.body.setAttribute('data-lang', S.lang || 'zh');
    if (els.fwdMsg && !G.fwd.busy && !(G.fwd.line && G.fwd.line.length)) els.fwdMsg.textContent = d.fwdIdle;
    anaCalcUI();                      
    els.t_nbest.textContent = d.nbest;
    els.t_nbestHint.textContent = d.nbestHint;
    if (els.t_multiPv) els.t_multiPv.textContent = d.multiPv;
    if (els.multiPvHint) els.multiPvHint.textContent = d.multiPvHint;
    if (els.candRangeHint) els.candRangeHint.textContent = d.candRangeHint || '';
    if (els.sel_candRange) els.sel_candRange.title = d.candRangeHint || '';
    if (els.inp_maxDepth) { els.inp_maxDepth.placeholder = d.phNone || ''; els.inp_maxDepth.title = d.phDepth || ''; }
    if (els.inp_maxNodes) { els.inp_maxNodes.placeholder = d.phNone || ''; els.inp_maxNodes.title = d.phNodes || ''; }
    if (els.sel_maxDepth) els.sel_maxDepth.title = d.phDepth || '';
    if (els.sel_maxNodes) els.sel_maxNodes.title = d.phNodes || '';
    if (els.sel_multiPv) {
      var mpSel = Math.max(1, Math.min(8, Math.round(+S.nbest || 3)));
      var mpHtml = [];
      for (var mi = 1; mi <= 8; mi++) {
        mpHtml.push('<option value="' + mi + '"' + (mi === mpSel ? ' selected' : '') + '>' + mi + (d.multiPvUnit || '') + '</option>');
      }
      els.sel_multiPv.innerHTML = mpHtml.join('');
    }
    cpSyncSegs();
    applyCardVis();                                   
    if (!G.ana.busy && !G.ana.rows.length) renderAna(d.anIdle, true);
    
    renderSysInfo();
    renderHashInfo();
    var hostNow = G.host || {};
    if (els.coresHint) els.coresHint.textContent = T('coresHint')
      .replace('{n}', String(hostNow.cpu | 0))
      .replace('{m}', String(Math.max(1, hostNow.threadsMax | 0)));
    if (els.weightsHint) els.weightsHint.textContent = T('weightsHint');
  }
  els.t_history.textContent = d.history;
  els.t_saved.textContent = d.saved;
  els.dr_save.textContent = d.saveSel;
  els.dr_open.textContent = d.openSel;
  els.dr_exp.textContent = d.drExp;
  els.dr_imp.textContent = d.drImp;
  els.dr_del.textContent = d.delSel;
  els.dr_close.textContent = d.closeDr;
  
  if (els.dr_all) els.dr_all.textContent = drAllSelected() ? d.selNone : d.selAll;
  els.btn_rv_hist.textContent = d.history;
  document.documentElement.lang = S.lang;
  var mm = els.seg_mode.querySelectorAll('button');
  
  if (mm[0]) mm[0].textContent = S.lang === 'en' ? 'Play' : '对弈';
  if (mm[1]) mm[1].textContent = S.lang === 'en' ? 'Free' : '摆盘';
  if (mm[2]) mm[2].textContent = S.lang === 'en' ? 'Endgame' : '残局';
  
  if (els.btn_eg_ok) els.btn_eg_ok.textContent = S.egLocked ? d.egOkOn : d.egOk;
  if (els.btn_eg_seq) els.btn_eg_seq.textContent = d.egSeq;
  if (els.btn_eg_free) els.btn_eg_free.textContent = d.egFree;
  if (els.btn_eg_b) els.btn_eg_b.textContent = d.egB;
  if (els.btn_eg_w) els.btn_eg_w.textContent = d.egW;
  var ss = els.seg_side.querySelectorAll('button');
  
  if (ss[0]) { var t0 = ss[0].querySelector('.tgl-txt'); if (t0) t0.textContent = d.sideAiB; else ss[0].textContent = d.sideAiB; }
  if (ss[1]) { var t1 = ss[1].querySelector('.tgl-txt'); if (t1) t1.textContent = d.sideAiW; else ss[1].textContent = d.sideAiW; }
  
  if (els.sideHint) els.sideHint.textContent = aiVsAi()
    ? (S.mode === 'pve' ? d.sideHintBoth : d.sideHintBothPlace)
    : humanBoth() ? d.sideHintNone : (S.aiB ? d.sideHintB : d.sideHintW);
  
  var rn = I18N[S.lang].ruleName || {};
  [].forEach.call(els.sel_rule.querySelectorAll('option'), function (o) {
    if (rn[o.value]) o.textContent = rn[o.value];
  });
  els.btn_ten.textContent = d.ten;
  
  if (els.btn_balcall) {
    els.btn_balcall.textContent = d.balCall;
    els.btn_balcall.title = d.balCallT;
  }
  
  if (els.btn_tara_yes) els.btn_tara_yes.textContent = d.taraYes;
  if (els.btn_tara_no) els.btn_tara_no.textContent = d.taraNo;
  if (els.btn_about) els.btn_about.textContent = T('about');
  if (els.t_about) els.t_about.textContent = T('about');
  if (els.ab_close) els.ab_close.textContent = T('aboutClose');
  renderAbout();                                   
  
  [].forEach.call(document.querySelectorAll('[data-tip]'), function (e) {
    var k = e.getAttribute('data-tip');
    if (k && d[k]) e.title = d[k];
  });
  
  if (!RV_MODE && !VIS_MODE) document.title = T('docTitle');
  
  if (els.btn_vis) els.btn_vis.textContent = d.vis;
  if (els.t_visTtl) els.t_visTtl.textContent = d.visTtl;
  if (els.t_visHint) els.t_visHint.textContent = d.visHint;
  if (els.btn_vis_upload) els.btn_vis_upload.textContent = d.visUpload;
  if (els.btn_vis_shot) els.btn_vis_shot.textContent = d.visShot;
  
  if (els.btn_vis_crop) els.btn_vis_crop.textContent = d.visCrop;
  if (els.t_visCropTitle) els.t_visCropTitle.textContent = d.visCropTitle;
  if (els.t_visCropHint) els.t_visCropHint.textContent = d.visCropHint;
  if (els.btn_crop_reset) els.btn_crop_reset.textContent = d.visCropReset;
  if (els.btn_crop_cancel) els.btn_crop_cancel.textContent = d.visCropCancel;
  if (els.btn_crop_ok) els.btn_crop_ok.textContent = d.visCropOk;
  if (els.btn_vis_del) els.btn_vis_del.title = d.visDelTitle;
  
  if (els.t_visDrawer) els.t_visDrawer.textContent = d.visDrawerTtl;
  if (els.t_vdTip) els.t_vdTip.textContent = d.visVdTip;
  if (els.visPos) els.visPos.title = d.visPillTip;
  if (VIS_MODE && els.visInput && document.activeElement !== els.visInput) els.visInput.placeholder = T('visBoxHintCode');
  if (els.btn_vis_rec) els.btn_vis_rec.textContent = d.visRec;
  if (els.btn_vis_rc) els.btn_vis_rc.textContent = d.visRc;
  if (els.btn_vis_rc) els.btn_vis_rc.title = d.visRc;
  if (els.btn_vis_load) els.btn_vis_load.textContent = d.visLoad;
  
  if (els.btn_vis_edit) els.btn_vis_edit.textContent = d.visEdit;
  if (els.btn_vis_vc) { els.btn_vis_vc.textContent = d.visVc; els.btn_vis_vc.title = d.visVcTip; }   
  if (els.btn_ve_swap) els.btn_ve_swap.textContent = d.veSwap;
  if (els.btn_ve_del) els.btn_ve_del.textContent = d.veDel;
  if (els.btn_ve_add) els.btn_ve_add.textContent = d.veAdd;
  if (els.btn_ve_addb) els.btn_ve_addb.textContent = d.veAddB;
  if (els.btn_ve_addw) els.btn_ve_addw.textContent = d.veAddW;
  if (els.btn_ve_undo) els.btn_ve_undo.textContent = d.veUndo;    
  if (els.t_vcxPop) els.t_vcxPop.textContent = d.vcxPopTitle;     
  if (els.vcxPop_close) els.vcxPop_close.textContent = d.aboutClose;
  
  if (els.btn_ve_fillb) els.btn_ve_fillb.textContent = d.veFillB;
  if (els.btn_ve_fillw) els.btn_ve_fillw.textContent = d.veFillW;
  if (els.t_visCode) els.t_visCode.textContent = d.visCode;
  if (els.btn_fwd_vcf) els.btn_fwd_vcf.textContent = d.fwdFindVcf;
  if (els.btn_fwd_vct) els.btn_fwd_vct.textContent = d.fwdFindVct;
  
  if (els.btn_fwd_clear) els.btn_fwd_clear.textContent = d.fwdClear;
  if (els.btn_vd_sel) els.btn_vd_sel.textContent =
    els.btn_vd_sel.classList.contains('accent') ? d.visSelOn : d.visSelOff;
  if (els.btn_vd_all) els.btn_vd_all.textContent = d.visSelAll;
  if (els.btn_vd_del) els.btn_vd_del.textContent = d.visDelSel;
  if (VIS_MODE) document.title = T('visTitle');
  
  if (HOST) tellHost({ type: 'uiTheme', theme: themeKey(), bg: pageBgHex(), lang: S.lang, cssVars: S.cssVars });
  setLangBtn();
  applyRuleHint();
  renderDrawer();
  drawCurve();
}

function setLangBtn() {
  if (els && els.btn_lang) els.btn_lang.textContent = (S.lang === 'en') ? '中' : 'EN';
}
function applyRuleHint() {
  var h = (I18N[S.lang].ruleHint || {})[+S.rule];
  els.ruleHint.textContent = h || '';
}

function themeKey() { return (S.theme === 'custom') ? (S.customBase === 'dark' ? 'dark' : 'light')
                                                    : (S.theme || 'light'); }
function applyTheme() {
  document.body.setAttribute('data-theme', themeKey());
  
  applyColors();
  paint();
  drawCurve();
  if (els.btn_color) els.btn_color.title = T('color');
  cpSyncSegs();
  
  if (HOST) tellHost({ type: 'uiTheme', theme: themeKey(), bg: pageBgHex(), lang: S.lang, cssVars: S.cssVars });
}
window.GB_onHost = function (m) {
  if (!m) return;
  try {
    G.host = G.host || {};
    G.host.threadsMax = m.threadsMax | 0;
    G.host.threadsDefault = m.threadsDefault | 0;
    G.host.cpu = m.cpu | 0;
    G.host.physCores = m.physCores | 0;
    G.host.userThreads = m.userThreads | 0;
    G.host.userHashMB = m.userHashMB | 0;
    if (typeof m.avx2 === 'boolean') {
      G.host.sse = !!m.sse; G.host.avx2 = !!m.avx2; G.host.avx512 = !!m.avx512;
      G.host.avx512vnni = !!m.avx512vnni; G.host.avxvnni = !!m.avxvnni;
      G.host.variant = m.variant || '';
      G.host.variantBench = m.variantBench || '';
      try { fillVariantSelect(); } catch (eV) {}
    }
  } catch (e0) {}
  var maxT = m.threadsMax | 0;
  if (!(maxT >= 1)) return;
  if (m.userThreads >= MIN_THREADS) S.cores = m.userThreads;
  if (!(S.cores >= 10 && S.cores <= 12 && maxT > S.cores)) return;
  if (migFlag('threadsTopo3')) return;
  var _old = S.cores;
  S.cores = maxT;
  try { enginePrefSet(S.cores, S.hashMB); } catch (e1) {}
  try { save(); } catch (e2) {}
  try { applyEngineConfig(); } catch (e3) {}
  try { console.log('[threads] topo migrate ' + _old + ' -> ' + maxT); } catch (e4) {}
};
function variantCapsNorm(x) {
  x = x || {};
  return {
    hasSSE:        !!(x.hasSSE        !== undefined ? x.hasSSE        : x.sse),
    hasAVX2:       !!(x.hasAVX2       !== undefined ? x.hasAVX2       : x.avx2),
    hasAVX512:     !!(x.hasAVX512     !== undefined ? x.hasAVX512     : x.avx512),
    hasAVX512VNNI: !!(x.hasAVX512VNNI !== undefined ? x.hasAVX512VNNI : x.avx512vnni),
    hasAVXVNNI:    !!(x.hasAVXVNNI    !== undefined ? x.hasAVXVNNI    : x.avxvnni)
  };
}
function fillVariantSelect() {
  var el = els.sel_variant;
  if (!el) return;
  var cur = variantPrefGet();
  S.variant = cur;
  var vHint = function (val) {
    var h = els.variantHint;
    if (!h) return;
    var txt;
    if (!val) {
      txt = T('vHintAuto');
      var b = (G.host || {}).variantBench || (G.sysInfo || {}).variantBench;
      if (b) txt += ' · ' + T('vHintBench').replace('{v}', b);
    } else {
      txt = T('vHint' + (val === 'avx512vnni' ? '512vnni' :
                         val === 'avx512' ? '512' :
                         val === 'avxvnni' ? 'Vnni' :
                         val === 'avx2' ? '2' : 'Sse'));
    }
    if (h.textContent !== txt) h.textContent = txt;
  };
  vHint(cur);
  var mk = function (c) {
    c = variantCapsNorm(c);
    var list = [];
    if (c.hasAVX512VNNI) list.push(['avx512vnni', 'AVX-512 VNNI', 'vHint512vnni']);
    if (c.hasAVX512)     list.push(['avx512', 'AVX-512', 'vHint512']);
    if (c.hasAVXVNNI)    list.push(['avxvnni', 'AVX-VNNI', 'vHintVnni']);
    if (c.hasAVX2)       list.push(['avx2', 'AVX2', 'vHint2']);
    if (c.hasSSE)        list.push(['sse', 'SSE', 'vHintSse']);
    var html = '<option value=""' + (cur ? '' : ' selected') + ' title="' + escHtml(T('vHintAuto')) + '">' + T('vAuto') + '</option>' +
      list.map(function (o) {
        return '<option value="' + o[0] + '"' + (o[0] === cur ? ' selected' : '') +
               ' title="' + escHtml(T(o[2])) + '">' + o[1] + '</option>';
      }).join('');
    if (el.innerHTML !== html) el.innerHTML = html;
  };
  var html0 = '<option value=""' + (cur ? '' : ' selected') + '>' + T('vAuto') + '</option>';
  if (el.innerHTML !== html0) el.innerHTML = html0;

  var h = G.host || {};
  if (typeof h.avx2 === 'boolean' || typeof h.hasAVX2 === 'boolean') { mk(h); return; }
  if (G.capsCache2) { mk(G.capsCache2); return; }
  try {
    LocalAI.caps().then(function (c) {
      c = c || {};
      if (!c.hasSSE) return;
      G.capsCache2 = { hasSSE: c.hasSSE, hasAVX2: c.hasAVX2, hasAVX512: c.hasAVX512,
                       hasAVX512VNNI: c.hasAVX512VNNI, hasAVXVNNI: c.hasAVXVNNI };
      mk(G.capsCache2);
    });
  } catch (e) {}
}

function selSyncCustom(el, v, fmt) {
  if (!el) return;
  var has = false;
  [].forEach.call(el.options, function (o) { if (+o.value === v) has = true; });
  if (!has) {
    var o = document.createElement('option');
    o.value = String(v);
    o.textContent = fmt(v);
    el.appendChild(o);
  }
  el.value = String(v);
}

function applyEngineConfig() {
  // Send threads/hash from the dedicated engine key (S may have been polluted by another
  // window's save). This is the single gate that keeps the link window and the main UI in
  // sync with the engine: whatever window triggers it, the host gets the user's real choice.
  var eo = enginePrefGet();
  if (eo.cores >= MIN_THREADS) S.cores = eo.cores;
  if (eo.hashMB >= 1) S.hashMB = eo.hashMB;
  if (!S.weights) S.weights = weightsPrefGet();
  var wMode = (S.weights === 'classic' || S.weights === 'nnue') ? S.weights : defaultWeightsMode();
  if (!S.variant) S.variant = variantPrefGet();
  var extra = { caution: (S.candRange >= 0 && S.candRange <= 5) ? S.candRange : 3,
                maxDepth: S.maxDepth || 0, maxNodes: S.maxNodes || 0,
                variant: S.variant || '' };
  LocalAI.config(S.cores, S.hashMB, wMode, extra);

  tellHost({ type: 'engineConfig', threads: S.cores || 0, hashMB: S.hashMB || 0, weights: wMode,
             caution: extra.caution, maxDepth: extra.maxDepth, maxNodes: extra.maxNodes,
             variant: extra.variant });
}

function defaultWeightsMode() {
  var h = G.host || {};
  if (h.hasAVX2 === false || h.avx2 === false) return 'classic';
  return 'nnue';
}

function hexToRgb255(hex) {
  var s = String(hex == null ? '' : hex).trim();
  var m = /^#?([0-9a-f]{6})$/i.exec(s);
  if (m) { var v = parseInt(m[1], 16); return [(v >> 16) & 255, (v >> 8) & 255, v & 255]; }
  
  var m3 = /rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)/i.exec(s);
  return m3 ? [+m3[1], +m3[2], +m3[3]] : null;
}
function rgbToHex(r, g, b) {
  function h(n) { n = Math.max(0, Math.min(255, Math.round(n))); return ('0' + n.toString(16)).slice(-2); }
  return '#' + h(r) + h(g) + h(b);
}
function mixHex(hex, other, t) {
  var a = hexToRgb255(hex), b = hexToRgb255(other);
  if (!a || !b) return hex;
  return rgbToHex(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t);
}

function rgbToHsv(r, g, b) {
  r /= 255; g /= 255; b /= 255;
  var mx = Math.max(r, g, b), mn = Math.min(r, g, b), d = mx - mn, h = -1;
  if (d > 1e-6) {
    if (mx === r) h = ((g - b) / d) % 6;
    else if (mx === g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h *= 60; if (h < 0) h += 360;
  }
  return { h: h, s: mx ? d / mx : 0, v: mx };
}
function hsvToRgb(h, s, v) {
  h = ((h % 360) + 360) % 360;
  var c = v * s, x = c * (1 - Math.abs(((h / 60) % 2) - 1)), m = v - c;
  var t = (h < 60) ? [c, x, 0] : (h < 120) ? [x, c, 0] : (h < 180) ? [0, c, x]
        : (h < 240) ? [0, x, c] : (h < 300) ? [x, 0, c] : [c, 0, x];
  return [(t[0] + m) * 255, (t[1] + m) * 255, (t[2] + m) * 255];
}
function lum255(rgb) { return (0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]) / 255; }

function boardVarsFrom(hex) {
  var rgb = hexToRgb255(hex);
  if (!rgb) return null;
  var hsv = rgbToHsv(rgb[0], rgb[1], rgb[2]);
  var f = (hsv.v < 0.55) ? 1 : -1;             
  function shade(dv, ds) {
    return rgbToHex.apply(null, hsvToRgb(hsv.h, Math.max(0.08, Math.min(0.92, hsv.s + ds)),
                                        Math.max(0.08, Math.min(0.98, hsv.v + f * dv))));
  }
  return {
    '--board': rgbToHex(rgb[0], rgb[1], rgb[2]),
    '--line': shade(0.20, 0.06),     
    '--star': shade(0.34, 0.08),     
    '--axis': shade(0.28, 0.06),     
  };
}

function pageVarsFrom(hex) {
  var rgb = hexToRgb255(hex);
  if (!rgb) return null;
  var base = rgbToHex(rgb[0], rgb[1], rgb[2]);
  if (lum255(rgb) < 0.5) {
    return {
      '--bg': base, '--card': mixHex(base, '#ffffff', 0.07), '--chip': mixHex(base, '#ffffff', 0.13),
      '--border': mixHex(base, '#ffffff', 0.22), '--ink': '#e7e9ec', '--sub': '#9aa0a6',
      '--chip-on': '#e7e9ec', '--chip-on-ink': mixHex(base, '#000000', 0.3),
    };
  }
  return {
    '--bg': base, '--card': mixHex(base, '#ffffff', 0.55), '--chip': mixHex(base, '#ffffff', 0.3),
    '--border': mixHex(base, '#000000', 0.12), '--ink': '#191919', '--sub': '#8a8a86',
    '--chip-on': '#191919', '--chip-on-ink': '#ffffff',
  };
}

var COLOR_VARS = ['--board', '--line', '--star', '--axis',
                  '--bg', '--card', '--chip', '--border', '--ink', '--sub',
                  '--chip-on', '--chip-on-ink'];

function computeColorVars() {
  var vars = {}, k;
  var b = boardVarsFrom(S.boardColor); if (b) for (k in b) vars[k] = b[k];
  var p = pageVarsFrom(S.pageBgColor); if (p) for (k in p) vars[k] = p[k];
  S.cssVars = Object.keys(vars).length ? vars : null;
  return S.cssVars;
}

function applyColorVars(vars) {
  COLOR_VARS.forEach(function (n) { document.body.style.removeProperty(n); });
  if (vars) for (var k in vars) document.body.style.setProperty(k, vars[k]);
}
function applyColors() { applyColorVars(computeColorVars()); }

var CP = { h: 40, s: 0.45, v: 0.85, target: 'board', drag: 0 };

var CP_PRESETS = ['#d9b878', '#e3be6f', '#ecdcbb', '#c3cbb4', '#a5804e'];

function cpRGB() { var c = hsvToRgb(CP.h, CP.s, CP.v); return [Math.round(c[0]), Math.round(c[1]), Math.round(c[2])]; }
function cpCurrentHex() {
  var fallback = (CP.target === 'page') ? css('--bg') : css('--board');
  var pick = (CP.target === 'page') ? S.pageBgColor : S.boardColor;
  return hexToRgb255(pick) ? pick : (hexToRgb255(fallback) ? fallback : '#d9b878');
}

function cpRender() {
  var sv = els.cp_sv, hue = els.cp_hue;
  if (!sv || !hue) return;
  var dpr = Math.max(1, Math.min(3, window.devicePixelRatio || 1));
  var W = Math.round(sv.clientWidth || 288), H = Math.round(sv.clientHeight || 150);
  var HW = Math.round(hue.clientWidth || W), HH = Math.round(hue.clientHeight || 16);
  if (W < 16) W = 288; if (H < 16) H = 150;          
  if (HW < 16) HW = W; if (HH < 6) HH = 16;
  [[sv, W, H], [hue, HW, HH]].forEach(function (t) {
    var bw = Math.round(t[1] * dpr), bh = Math.round(t[2] * dpr);
    if (t[0].width !== bw || t[0].height !== bh) { t[0].width = bw; t[0].height = bh; }
  });
  var sc = sv.getContext('2d'), hc = hue.getContext('2d');
  sc.setTransform(dpr, 0, 0, dpr, 0, 0); hc.setTransform(dpr, 0, 0, dpr, 0, 0);
  
  var base = hsvToRgb(CP.h, 1, 1);
  var g = sc.createLinearGradient(0, 0, W, 0);
  g.addColorStop(0, '#ffffff'); g.addColorStop(1, rgbToHex(base[0], base[1], base[2]));
  sc.fillStyle = g; sc.fillRect(0, 0, W, H);
  var g2 = sc.createLinearGradient(0, 0, 0, H);
  g2.addColorStop(0, 'rgba(0,0,0,0)'); g2.addColorStop(1, 'rgba(0,0,0,1)');
  sc.fillStyle = g2; sc.fillRect(0, 0, W, H);
  
  var cx = CP.s * W, cy = (1 - CP.v) * H;
  sc.lineJoin = 'round';
  sc.lineWidth = 1.6; sc.strokeStyle = 'rgba(0,0,0,.55)';
  sc.beginPath(); sc.arc(cx, cy, 6.7, 0, Math.PI * 2); sc.stroke();
  sc.lineWidth = 2.2; sc.strokeStyle = 'rgba(255,255,255,.98)';
  sc.beginPath(); sc.arc(cx, cy, 5.3, 0, Math.PI * 2); sc.stroke();
  
  var hg = hc.createLinearGradient(0, 0, HW, 0);
  var steps = Math.max(24, Math.min(360, Math.round(HW * dpr)));
  for (var i = 0; i <= steps; i++) {
    var c = hsvToRgb((i / steps) * 360, 1, 1);
    hg.addColorStop(i / steps, rgbToHex(c[0], c[1], c[2]));
  }
  hc.fillStyle = hg; hc.fillRect(0, 0, HW, HH);
  
  var hx = Math.max(1.5, Math.min(HW - 1.5, (CP.h / 360) * HW));
  hc.fillStyle = 'rgba(255,255,255,.98)'; hc.fillRect(hx - 1.5, 0, 3, HH);
  hc.lineWidth = 1; hc.strokeStyle = 'rgba(0,0,0,.45)'; hc.strokeRect(hx - 1.5, 0.5, 3, HH - 1);
  cpSyncFields();
}
function cpSyncFields() {
  var rgb = cpRGB(), hex = rgbToHex(rgb[0], rgb[1], rgb[2]);
  if (els.cp_r) els.cp_r.value = rgb[0];
  if (els.cp_g) els.cp_g.value = rgb[1];
  if (els.cp_b) els.cp_b.value = rgb[2];
  els.cp_rn.value = rgb[0]; els.cp_gn.value = rgb[1]; els.cp_bn.value = rgb[2];
  if (document.activeElement !== els.cp_hex) els.cp_hex.value = hex;
  els.cp_prev.style.background = hex;
  [].forEach.call(els.cp_presets.querySelectorAll('button'), function (b) {
    b.classList.toggle('on', b.getAttribute('data-c') === hex);
  });
}

function cpLive() {
  
  if (S.theme !== 'custom') { S.customBase = (S.theme === 'dark') ? 'dark' : 'light'; S.theme = 'custom'; cpSyncSegs(); }
  
  cpRender();
  var hex = rgbToHex.apply(null, cpRGB());
  if (CP.target === 'page') { S.pageBgColor = hex; S.customColors.page = hex; }
  else { S.boardColor = hex; S.customColors.board = hex; }
  applyColors(); paint();
}

function cpCommit(keepTheme) {
  if (!keepTheme && S.theme !== 'custom') { S.customBase = (S.theme === 'dark') ? 'dark' : 'light'; S.theme = 'custom'; cpSyncSegs(); }
  var hex = rgbToHex.apply(null, cpRGB());
  if (CP.target === 'page') { S.pageBgColor = hex; S.customColors.page = hex; }
  else { S.boardColor = hex; S.customColors.board = hex; }
  
  applyColors();
  save();
  applyTheme();
}
function cpSyncSegs() {
  if (els.cp_theme) [].forEach.call(els.cp_theme.querySelectorAll('button'), function (b) {
    b.classList.toggle('on', b.getAttribute('data-th') === (S.theme || 'light'));
  });
  
  if (els.cpPresetRow) els.cpPresetRow.hidden = (CP.target === 'page');
  if (els.cp_target) [].forEach.call(els.cp_target.querySelectorAll('button'), function (b) {
    b.classList.toggle('on', b.getAttribute('data-tg') === CP.target);
  });
  if (els.cp_hint) els.cp_hint.textContent = (CP.target === 'page') ? T('cpHintP') : T('cpHintB');
}
function cpFromColor(hex) {
  var rgb = hexToRgb255(hex);
  if (!rgb) return false;
  var hsv = rgbToHsv(rgb[0], rgb[1], rgb[2]);
  if (hsv.h >= 0) CP.h = hsv.h;      
  CP.s = hsv.s; CP.v = hsv.v;
  cpRender();
  return true;
}
function cpFromRGB(r, g, b) {
  var hsv = rgbToHsv(Math.max(0, Math.min(255, +r || 0)),
                     Math.max(0, Math.min(255, +g || 0)),
                     Math.max(0, Math.min(255, +b || 0)));
  if (hsv.h >= 0) CP.h = hsv.h;
  CP.s = hsv.s; CP.v = hsv.v;
  cpRender();
}

function cpBindDrag(cv, name, onPos) {
  if (!cv) return;
  function at(e) {
    var r = cv.getBoundingClientRect();
    onPos(Math.max(0, Math.min(1, (e.clientX - r.left) / Math.max(1, r.width))),
          Math.max(0, Math.min(1, (e.clientY - r.top) / Math.max(1, r.height))));
  }
  cv.addEventListener('mousedown', function (e) { CP.drag = name; at(e); e.preventDefault(); });
  window.addEventListener('mousemove', function (e) { if (CP.drag === name) at(e); });
  window.addEventListener('mouseup', function () { if (CP.drag === name) { CP.drag = 0; cpCommit(); } });
}
function openColorPop() {
  if (els.colorPop) els.colorPop.hidden = false;
  cpSyncSegs();
  cpFromColor(cpCurrentHex());
}
function closeColorPop() { if (els.colorPop) els.colorPop.hidden = true; }
function wireColors() {
  if (!els.btn_color) return;
  els.btn_color.onclick = function () { openColorPop(); };
  els.cp_close.onclick = closeColorPop;
  els.colorPop.addEventListener('click', function (e) { if (e.target === els.colorPop) closeColorPop(); });
  els.cp_theme.onclick = function (e) {
    var b = e.target.closest('button'); if (!b) return;
    var th = b.getAttribute('data-th');
    S.theme = (th === 'dark' || th === 'custom') ? th : 'light';
    if (th !== 'custom') S.customBase = th;   
    
    if (th !== 'custom') {
      if (S.boardColor) S.customColors.board = S.boardColor;
      if (S.pageBgColor) S.customColors.page = S.pageBgColor;
      S.boardColor = ''; S.pageBgColor = '';
    }
    
    if (th === 'custom' && !S.boardColor && !S.pageBgColor) {
      if (S.customColors.board || S.customColors.page) {
        S.boardColor = S.customColors.board;
        S.pageBgColor = S.customColors.page;
      } else {
        var hex0 = rgbToHex.apply(null, cpRGB());
        if (CP.target === 'page') { S.pageBgColor = hex0; S.customColors.page = hex0; }
        else { S.boardColor = hex0; S.customColors.board = hex0; }
      }
    }
    applyTheme(); cpSyncSegs();
    if (th !== 'custom') cpFromColor(cpCurrentHex());   
    save();
  };
  els.cp_target.onclick = function (e) {
    var b = e.target.closest('button'); if (!b) return;
    CP.target = (b.getAttribute('data-tg') === 'page') ? 'page' : 'board';
    cpSyncSegs();
    cpFromColor(cpCurrentHex());
  };
  
  els.cp_presets.innerHTML = CP_PRESETS.map(function (c) {
    return '<button type="button" data-c="' + c + '" style="background:' + c + '" title="' + c + '"></button>';
  }).join('');
  [].forEach.call(els.cp_presets.querySelectorAll('button'), function (b) {
    
    b.onclick = function () { cpFromColor(b.getAttribute('data-c')); cpCommit(true); };
  });
  cpBindDrag(els.cp_sv, 'sv', function (x, y) {
    CP.s = x; CP.v = 1 - y; cpLive();
  });
  cpBindDrag(els.cp_hue, 'hue', function (x) {
    CP.h = x * 360; cpLive();
  });
  
  [['cp_r', 'cp_rn', 0], ['cp_g', 'cp_gn', 1], ['cp_b', 'cp_bn', 2]].forEach(function (t) {
    var s = els[t[0]], n = els[t[1]], idx = t[2];
    function set(v, commit) {
      var rgb = cpRGB(); rgb[idx] = Math.max(0, Math.min(255, Math.round(+v || 0)));
      cpFromRGB(rgb[0], rgb[1], rgb[2]);
      if (commit) cpCommit(); else cpLive();
    }
    if (s) {
      s.oninput = function () { set(s.value, false); };
      s.onchange = function () { set(s.value, true); };
    }
    if (n) n.onchange = function () { set(n.value, true); };
  });
  els.cp_hex.onchange = function () {
    var rgb = hexToRgb255(els.cp_hex.value);
    if (!rgb) { toast(T('cpBad')); cpSyncFields(); return; }
    cpFromRGB(rgb[0], rgb[1], rgb[2]);
    cpCommit();
  };
  els.cp_reset.onclick = function () {
    S.boardColor = ''; S.pageBgColor = '';
    
    S.customColors.board = ''; S.customColors.page = '';
    
    if (S.theme === 'custom') { S.theme = 'light'; S.customBase = 'light'; }
    
    applyColors();
    save();
    applyTheme();
    cpFromColor(cpCurrentHex());
    toast(T('cpReset'));
  };
}

var CARD_IDS = ['setup', 'analysis', 'engine'];   
var DEFAULT_CARDS = { setup: 0, analysis: 0, engine: 1 };   

var MIN_THREADS = 2;
var PREFS_KEY = 'gbcalc.prefs.v1';
// cores / hashMB were removed from this generic pref table: the main / link / replay / vision
// windows each have their own S, and any window's save() re-wrote them to the shared key, so a
// stale window overwrote the value the user had just changed. They now live in ENGINE_PREF_KEY
// and are only written on an explicit change.
var PREF_FIELDS = ['cores', 'hashMB', 'turnMs', 'rule', 'lang', 'theme',
                   'cards', 'customBase', 'aiB', 'aiW', 'linkAuto', 'linkClicks',
                   'candRange', 'maxDepth', 'maxNodes'];
function prefsAll() {
  var o = null;
  try { o = JSON.parse(localStorage.getItem(PREFS_KEY) || 'null'); } catch (e) { o = null; }
  return (o && typeof o === 'object') ? o : {};
}
function prefGet(k, def) {
  var o = prefsAll();
  return (o[k] === undefined || o[k] === null) ? def : o[k];
}
function prefSet(k, v) {
  try {
    var o = prefsAll();
    if (v === undefined) delete o[k]; else o[k] = v;
    localStorage.setItem(PREFS_KEY, JSON.stringify(o));
  } catch (e) {}
}
function prefsSyncFromS() {
  try {
    var o = prefsAll(), ch = false;
    PREF_FIELDS.forEach(function (k) {
      if (S[k] === undefined) return;
      if (k === 'cores' && !(S[k] >= MIN_THREADS)) return;
      if (k === 'hashMB' && !(S[k] >= 1)) return;
      var a = S[k], b = o[k];
      if (typeof a === 'object') { if (JSON.stringify(a) !== JSON.stringify(b)) { o[k] = a; ch = true; } }
      else if (a !== b) { o[k] = a; ch = true; }
    });
    if (ch) localStorage.setItem(PREFS_KEY, JSON.stringify(o));
  } catch (e) {}
}
function prefsApplyToS() {
  try {
    var o = prefsAll();
    PREF_FIELDS.forEach(function (k) {
      if (o[k] === undefined || !S.hasOwnProperty(k)) return;
      var v = S[k];
      var bad = (v === undefined || v === null || v === '');
      if (k === 'cores') bad = !(v >= MIN_THREADS);
      if (k === 'hashMB') bad = !(v >= 1);
      if (k === 'cards') bad = !v || typeof v !== 'object';
      if (bad) S[k] = o[k];
    });
  } catch (e) {}
}
// Root cause of "changing the thread count while the link feature is on does not persist":
// the main / link (?link=1) / replay (?rv=1) / vision (?vis=1) windows are four separate pages
// with four separate S objects sharing one localStorage. After the user changed the thread
// count in the main window, any other window's save() wrote its own stale cores back to
// gbcalc.settings.v1, so the main window read the old value on the next start. Threads/hash
// are engine parameters, not UI state: give them a dedicated key as the single source of truth,
// written only by an explicit user change (dropdown onchange) or the one-time factory default,
// and read by every startup / host-dispatch path.
var ENGINE_PREF_KEY = 'gbcalc.engine.v1';
function enginePrefAll() {
  var o = null;
  try { o = JSON.parse(localStorage.getItem(ENGINE_PREF_KEY) || 'null'); } catch (e) { o = null; }
  return (o && typeof o === 'object') ? o : null;
}
// Read: accept only already-valid values from the dedicated key (no merging); 0 = fall back.
function enginePrefGet() {
  var o = enginePrefAll() || {};
  return { cores: (o.cores >= MIN_THREADS) ? o.cores : 0,
           hashMB: (o.hashMB >= 1) ? o.hashMB : 0 };
}
// Write: the only entry point. Keeps the old value when cores/hashMB are invalid.
function enginePrefSet(cores, hashMB) {
  var o = enginePrefAll() || {};
  if (cores >= MIN_THREADS) o.cores = cores;
  if (hashMB >= 1) o.hashMB = hashMB;
  o.ts = Date.now();
  try { localStorage.setItem(ENGINE_PREF_KEY, JSON.stringify(o)); } catch (e) {}
}


var WEIGHTS_PREF_KEY = 'gbcalc.engine.weights';
var VARIANT_PREF_KEY = 'gbcalc.engine.variant';
function variantPrefGet() {
  try { var v = localStorage.getItem(VARIANT_PREF_KEY); return v || ''; } catch (e) { return ''; }
}
function variantPrefSet(v) { try { localStorage.setItem(VARIANT_PREF_KEY, v || ''); } catch (e) {} }
function weightsPrefGet() {
  try {
    var v = localStorage.getItem(WEIGHTS_PREF_KEY);
    return (v === 'nnue' || v === 'classic') ? v : '';
  } catch (e) { return ''; }
}
function weightsPrefSet(v) {
  try {
    if (v === 'nnue' || v === 'classic') localStorage.setItem(WEIGHTS_PREF_KEY, v);
  } catch (e) {}
}
// One-time migration: when the key does not exist yet, seed it with the valid values from the
// legacy prefs / settings.v1 (leave it empty when neither has a valid value, so an explicit
// user change writes it later).
function enginePrefEnsure() {
  if (enginePrefAll()) return;
  var pc = prefGet('cores', 0), ph = prefGet('hashMB', 0);
  if (!(pc >= MIN_THREADS) || !(ph >= 1)) {
    try {
      var s = JSON.parse(localStorage.getItem('gbcalc.settings.v1') || '{}');
      if (!(pc >= MIN_THREADS) && s.cores >= MIN_THREADS) pc = s.cores;
      if (!(ph >= 1) && s.hashMB >= 1) ph = s.hashMB;
    } catch (e) {}
  }
  if (pc >= MIN_THREADS || ph >= 1) enginePrefSet(pc, ph);
}
// Align this page's S engine fields with the source of truth (only when the key holds a valid
// value). Called on boot / link-mode switch / before every save().
function enginePrefApplyToS() {
  if (S.hasOwnProperty('cores')) { var c = enginePrefGet().cores; if (c >= MIN_THREADS) S.cores = c; }
  if (S.hasOwnProperty('hashMB')) { var h = enginePrefGet().hashMB; if (h >= 1) S.hashMB = h; }
}

function savePins() { prefSet('pins', CARD_PIN); }

var CARD_PIN = (function () {
  var p = prefGet('pins', null);
  if (p && typeof p === 'object') {
    var o = {};
    for (var k in p) o[k] = !!p[k];
    return o;
  }
  return { engine: true };
})();

function cardVis() {
  
  if (S.cards && S.cards.curve !== undefined) S.cards = null;
  if (!S.cards || typeof S.cards !== 'object') S.cards = {};
  CARD_IDS.forEach(function (id) {
    if (S.cards[id] === undefined) S.cards[id] = DEFAULT_CARDS[id];
  });
  return S.cards;
}
function applyCardVis() {
  var v = cardVis();
  CARD_IDS.forEach(function (id) {
    var el = document.querySelector('.card[data-id="' + id + '"]');
    if (el) el.hidden = !v[id];
  });
  buildCardMenu();
  syncDocks();
  
  syncPinKeys();
}

function buildCardMenu() {
  if (!els.setPop) return;
  [].forEach.call(els.setPop.querySelectorAll('button[data-card]'), function (b) {
    var id = b.getAttribute('data-card');
    b.textContent = T(id);
    b.classList.toggle('on', !!cardVis()[id]);
  });
}
function toggleSetPop() {
  if (!els.setPop) return;
  
  if (els.aiSidePop && !els.aiSidePop.hidden) {
    els.aiSidePop.hidden = true;
    if (els.btn_aiside) els.btn_aiside.classList.remove('on');
  }
  els.setPop.hidden = !els.setPop.hidden;
  buildCardMenu();
  if (els.btn_set) els.btn_set.classList.toggle('on', !els.setPop.hidden);
}

function cardOrderBySelect(id) {
  var card = document.querySelector('.card[data-id="' + id + '"]');
  if (!card || card.hidden || !card.parentNode) return;
  // In link mode cards opened from Settings land in the left dock as well.
  var target = (linkSessionOn() && els.dockL) ? els.dockL : card.parentNode;
  target.appendChild(card);     
}

function syncPinKeys() {
  [].forEach.call(document.querySelectorAll('.card'), function (c) {
    var pk = c.querySelector('.card-h .pinkey');
    if (pk) pinKeySync(pk, c.getAttribute('data-id'));
  });
}

function toggleCardFromMenu(id) {
  var v = cardVis();
  var opening = !v[id];
  v[id] = opening ? 1 : 0;
  if (!opening) { delete CARD_PIN[id]; savePins(); }   
  save();
  if (opening && (id === 'setup' || id === 'analysis')) {
    var other = (id === 'setup') ? 'analysis' : 'setup';
    if (v[other] && !CARD_PIN[other]) v[other] = 0;
  }
  applyCardVis();
  if (opening) cardOrderBySelect(id);   
  syncPinKeys();                        
  persistLayout();
  layoutBoard();
}

function pinKeySync(pk, id) {
  if (!pk) return;
  if (CARD_PIN[id]) {                       
    pk.textContent = '\u2715';
    pk.title = T('pinCloseTtl');
  } else {                                  
    pk.textContent = T('pinFix');
    pk.title = T('pinFixTtl');
  }
}
function wireCards() {
  
  if (els.btn_set) {
    els.btn_set.onclick = function (e) { e.stopPropagation(); toggleSetPop(); };
  }
  if (els.setPop) {
    [].forEach.call(els.setPop.querySelectorAll('button[data-card]'), function (b) {
      b.onclick = function () { toggleCardFromMenu(b.getAttribute('data-card')); };
    });
  }
  function outside(e) {
    if (!els.setPop || els.setPop.hidden) return;
    if (els.setPop.contains(e.target)) return;
    if (e.target && e.target.closest && e.target.closest('#btn_set')) return;
    els.setPop.hidden = true;
    if (els.btn_set) els.btn_set.classList.remove('on');
  }
  document.addEventListener('mousedown', outside);
  document.addEventListener('click', outside);
}

function anaTierOf(i) { return Math.min(ANA_PAL.length - 1, i); }
function pvText(line) {
  if (!line || !line.length) return '';
  return line.slice(1, 9).map(function (p) { return coordName(p[0], p[1]); }).join(' ');
}

function anaMark(btn) {
  [els.btn_an_nbest, els.btn_an_bal, els.btn_an_baln, els.btn_an_defend].forEach(function (b) {
    if (b) b.classList.toggle('on', !!btn && b === btn);
  });
  anaCalcUI();
}

function anaCalcUI() {
  if (!els.btn_an_stop) return;
  var busy = !!G.ana.busy;
  els.btn_an_stop.disabled = false;                    
  els.btn_an_stop.classList.toggle('on', busy);
  els.btn_an_stop.textContent = busy ? T('anStop') : T('anCalc');
  if (els.btn_an_nbest) {   
    els.btn_an_nbest.textContent = (busy && G.ana.kind === 'nbest') ? T('anStop') : T('anNbest');
  }
  if (els.btn_an_defend) {
    els.btn_an_defend.textContent = (busy && G.ana.kind === 'defend') ? T('anStopScan') : T('anDefend');
  }
}

var ANA_DIRS = [[1, 0], [0, 1], [1, 1], [1, -1]];
function anaLineScore(cnt, open) {
  if (cnt >= 5) return 100000;
  if (cnt === 4) return open >= 2 ? 10000 : (open === 1 ? 1200 : 0);
  if (cnt === 3) return open >= 2 ? 1500 : (open === 1 ? 150 : 0);
  if (cnt === 2) return open >= 2 ? 120 : (open === 1 ? 20 : 0);
  return open >= 2 ? 15 : (open === 1 ? 3 : 0);
}

function cellLines(board, x, y, c) {
  var SZ = board.length, out = [];
  for (var d = 0; d < 4; d++) {
    var dx = ANA_DIRS[d][0], dy = ANA_DIRS[d][1], cnt = 1, open = 0;
    for (var s = -1; s <= 1; s += 2) {
      var i = 1;
      for (;;) {
        var px = x + dx * i * s, py = y + dy * i * s;
        if (px < 0 || py < 0 || px >= SZ || py >= SZ) break;
        var v = board[py][px];
        if (v === c) { cnt++; i++; continue; }
        if (v === 0) open++;
        break;
      }
    }
    out.push({ cnt: cnt, open: open });
  }
  return out;
}
function cellScore(board, x, y, c) {
  var total = 0;
  cellLines(board, x, y, c).forEach(function (l) { total += anaLineScore(l.cnt, l.open); });
  return total;
}
function renderAna(msg, quietLog) {
  if (els.anStatus) {
    els.anStatus.textContent = msg || '';
    els.anStatus.style.whiteSpace = (typeof msg === 'string' && msg.indexOf('\n') >= 0) ? 'pre-line' : '';
  }
  if (!els.anList) return;
  els.anList.innerHTML = G.ana.rows.map(function (r) {
    var pal = (r.tier < 0) ? ANA_BAL : ANA_PAL[anaTierOf(r.i)];
    if (r.col) pal = { badge: r.col };      
    return '<div class="it"><span class="rk" style="background:' + pal.badge + '">' + escHtml(r.badge) + '</span>' +
           '<span class="co">' + escHtml(r.coord) + '</span>' +
           '<span class="ev">' + escHtml(r.eval) + '</span>' +
           (r.pv ? '<span class="pv">' + escHtml(r.pv) + '</span>' : '') + '</div>';
  }).join('');
  if (!quietLog) engMsgAdd(msg);
}

function anaReset() {
  G.ana.gen++;
  anaLiveStop();
  defLiveStop();
  G.ana.defCells = null;
  PV_HOVER = null;
  PV_PREVIEW = null;
  setPvLine([]);
  if (G.ana.defTimer) { clearTimeout(G.ana.defTimer); G.ana.defTimer = null; }  
  G.ana.defWinUntil = 0; G.ana.defRefining = false;                             
  if (G.ana.deferTmr) { clearTimeout(G.ana.deferTmr); G.ana.deferTmr = null; }  
  G.ana.busy = false; G.ana.kind = '';
  G.ana.marks = []; G.ana.rows = [];
  G.ana.autoDefend = false;
  anaMark(null);
  renderAna(T('anIdle'), true);
}


var ANA_LIVE = { handle: null };
function anaLiveStop() {
  if (ANA_LIVE.handle) { try { ANA_LIVE.handle.stop(); } catch (e) {} ANA_LIVE.handle = null; }
}
function anaLiveRun(kind, topN, tagKey) {
  var gen = G.ana.gen, mvN = G.moves.length;
  var side = curColor(), t0 = Date.now(), lastUi = 0, lastPaint = 0;
  var ended = false;
  var h = LocalAI.live({
    board: G.board, moveList: [], matchMs: 600000, turnMs: 3600000, timeUsedMs: 0,
    topN: topN, rule: engineRule(), cid: 'calc-' + kind + '-' + mvN, lane: 'main', side: side,
    liveId: LocalAI.nextLiveId(), tag: T(tagKey),
  }, function (res) {
    if (gen !== G.ana.gen || G.moves.length !== mvN) return;
    var cs = ((res && res.candidates) || []).filter(function (c) {
      return c && c.x >= 0 && c.y >= 0 && !(G.board[c.y] && G.board[c.y][c.x]);
    });
    if (!cs.length) return;
    var depth = (res && res.depth) || cs[0].depth || 0;
    if (kind === 'calc') {
      var c0 = cs[0];
      G.lastStat = { x: c0.x, y: c0.y, depth: depth, eval: c0.eval, speed: res.speed || 0,
                     nodes: res.nodes || 0, timeMs: null, line: c0.line || [], side: side };
      G.ana.marks = [{ live: true, x: c0.x, y: c0.y, wr: c0.winrate, ev: fmtEval(c0.eval),
                       rank: 0, depth: depth }];
      G.ana.rows = [{ i: 0, badge: '1', coord: coordName(c0.x, c0.y),
                      eval: fmtEval(c0.eval), pv: pvText(c0.line) }];
      setPvLine(c0.line || [], side);
    } else {
      G.ana.marks = []; G.ana.rows = [];
      cs.slice(0, topN).forEach(function (c, i) {
        G.ana.marks.push({ x: c.x, y: c.y, badge: String(i + 1), label: fmtEval(c.eval),
                           tier: anaTierOf(i), inline: true });
        G.ana.rows.push({ i: i, badge: String(i + 1), coord: coordName(c.x, c.y),
                          eval: fmtEval(c.eval), pv: pvText(c.line) });
      });
      setPvLine((cs[0] && cs[0].line) || [], side);
    }
    var now = Date.now();
    if (now - lastUi >= 280) {
      lastUi = now;
      if (depth) els.st_depth.textContent = String(depth);
      if (cs[0].eval != null && String(cs[0].eval) !== '') dashEvalSet(cs[0].eval, side);
      els.st_speed.textContent = fmtSpeed(res.speed || 0);
      els.st_nodes.textContent = fmtNodes(res.nodes || 0);
      els.st_time.textContent = ((now - t0) / 1000).toFixed(1) + ' s';
      renderAna(T(kind === 'calc' ? 'anCalcRun' : 'anNbestRun').replace('{d}', String(depth || 0)));
    }
    if (now - lastPaint >= 200) { lastPaint = now; paint(); }
  }, function (ev) {
    ended = true;
    if (gen !== G.ana.gen) return;
    ANA_LIVE.handle = null;
    if (ev && ev.busy) {
      setTimeout(function () {
        if (gen === G.ana.gen && G.ana.busy) anaLiveRun(kind, topN, tagKey);
      }, 420);
      return;
    }
    G.ana.busy = false;
    anaMark(null);
    if (ev && ev.error) renderAna(T('offline'));
    paint();
  });
  if (!ended && gen === G.ana.gen) ANA_LIVE.handle = h;
}

function anaDeferClear() {
  if (G.ana.deferTmr) clearTimeout(G.ana.deferTmr);
  var gen = G.ana.gen;
  G.ana.deferTmr = setTimeout(function () {
    G.ana.deferTmr = null;
    if (G.ana.busy || gen !== G.ana.gen) return;     
    G.ana.marks = []; G.ana.rows = [];
    if (!G.ana.kind) anaMark(null);
    if (!G.ana.busy) renderAna(T('anIdle'), true);
    paint();
  }, 1000);
}
function anaGuard(minStones) {
  if (RV_MODE || G.review) return false;
  bgThinkYield();
  if (LIVE_AN.on) liveAnaToggle();          
  if (G.ana.deferTmr) { clearTimeout(G.ana.deferTmr); G.ana.deferTmr = null; }  
  if (G.ana.busy) { toast(T('anRunning')); return false; }
  if (G.moves.length < (minStones || 0)) { renderAna(T('anEmpty')); return false; }
  return true;
}
function anaIp() { return Math.max(1, Math.min(8, Math.round(+S.nbest || 3))); }

async function anaCalc() {
  if (!anaGuard(0)) return;
  aiStandDown();                              
  ++G.ana.gen;
  G.ana.busy = true; G.ana.kind = 'calc';
  G.ana.marks = []; G.ana.rows = [];
  setPvLine([]);
  anaMark(els.btn_an_stop);
  renderAna(T('anCalcRun').replace('{d}', '0'));
  anaLiveRun('calc', 1, 'tagCalc');
}


var DEF_LIVE = { handle: null, gen: 0 };
function defLiveStop() {
  DEF_LIVE.gen++;
  if (DEF_LIVE.handle) { try { DEF_LIVE.handle.stop(); } catch (e) {} DEF_LIVE.handle = null; }
}
var DEF_TIERS = [
  { rf: 1.00, ff: 1.00, ring: 0.98, fill: 0.30, lw: 1.00 },
  { rf: 0.84, ff: 0.93, ring: 0.82, fill: 0.23, lw: 0.82 },
  { rf: 0.68, ff: 0.86, ring: 0.64, fill: 0.17, lw: 0.68 },
  { rf: 0.50, ff: 0.78, ring: 0.46, fill: 0.11, lw: 0.56 }
];
function defendTierOf(pct) {
  if (pct >= 95) return 1;
  if (pct >= 90) return 2;
  if (pct >= 85) return 3;
  if (pct >= 70) return 4;
  return 5;
}
function anaDefendStop() {
  if (G.ana.kind !== 'defend') return;
  defLiveStop();
  G.ana.busy = false; G.ana.kind = '';
  G.ana.autoDefend = false;
  anaMark(null);
  var n = (G.ana.defCells || []).length;
  renderAna(n ? T('anScanFrozen').replace('{c}', String(n)) : T('anIdle'));
  paint();
}
function anaDefend(renew) {
  if (!anaGuard(1)) return;
  aiStandDown();
  defLiveStop();
  var gen = ++G.ana.gen;
  var mvN = G.moves.length, t0 = Date.now(), lastUi = 0, lastPaint = 0, lastAna = 0;

  G.ana.busy = true; G.ana.kind = 'defend';
  G.ana.autoDefend = true;
  G.ana.marks = []; G.ana.rows = []; G.ana.defCells = null;
  setPvLine([]);
  anaMark(els.btn_an_defend);
  renderAna(T('anScanRun').replace('{d}', '0').replace('{c}', '0'));
  paint();

  var ended = false;
  var h = LocalAI.live({
    board: G.board, moveList: [], matchMs: 600000, turnMs: 3600000, timeUsedMs: 0,
    topN: 1, rule: engineRule(), cid: 'scan-' + mvN, lane: 'main', side: curColor(),
    liveId: LocalAI.nextLiveId(), tag: T('tagDefend'), scan: true
  }, function (res) {
    if (gen !== G.ana.gen || G.moves.length !== mvN) return;
    var cs = ((res && res.candidates) || []).filter(function (c) {
      return c && c.x >= 0 && c.y >= 0 && !(G.board[c.y] && G.board[c.y][c.x]);
    });
    if (!cs.length) return;
    var seen = {}, cells = [];
    cs.forEach(function (c) {
      var pct = Math.round((typeof c.winrate === 'number' ? c.winrate : 0.5) * 100);
      pct = Math.max(0, Math.min(100, pct));
      var k = c.x + ',' + c.y;
      if (seen[k] != null) {
        var old = cells[seen[k]];
        if (pct > old.pct) { old.pct = pct; old.evalTxt = c.eval || old.evalTxt; }
        return;
      }
      seen[k] = cells.length;
      cells.push({ x: c.x, y: c.y, pct: pct, evalTxt: c.eval || '', depth: c.depth || 0 });
    });
    cells.sort(function (a, b) { return b.pct - a.pct; });
    cells.forEach(function (c, i) { c.rank = i + 1; c.tier = defendTierOf(c.pct); });
    renderDefendMarks(cells);

    var depth = (res && res.depth) || (cells[0] && cells[0].depth) || 0;
    var now = Date.now();
    if (now - lastUi >= 280) {
      lastUi = now;
      if (depth) els.st_depth.textContent = String(depth);
      els.st_speed.textContent = fmtSpeed(res.speed || 0);
      els.st_nodes.textContent = fmtNodes(res.nodes || 0);
      els.st_time.textContent = ((now - t0) / 1000).toFixed(1) + ' s';
      var scSide = 3 - curColor();
      if (cells[0] && cells[0].evalTxt) dashEvalSet(cells[0].evalTxt, scSide);
      els.st_best.textContent = cells[0] ? coordName(cells[0].x, cells[0].y) : '-';
    }
    if (now - lastAna >= 1000) {
      lastAna = now;
      renderAna(T('anScanRun').replace('{d}', String(depth || 0)).replace('{c}', String(cells.length)));
    }
    if (now - lastPaint >= 320) { lastPaint = now; paint(); }
  }, function (ev) {
    ended = true;
    if (gen !== G.ana.gen) return;
    DEF_LIVE.handle = null;
    if (ev && ev.busy) {
      setTimeout(function () {
        if (gen === G.ana.gen && G.ana.busy && G.ana.kind === 'defend') {
          G.ana.busy = false; G.ana.kind = '';
          anaDefend();
        }
      }, 500);
      return;
    }
    if (!G.ana.busy || G.ana.kind !== 'defend') return;
    G.ana.busy = false; G.ana.kind = ''; G.ana.autoDefend = false;
    anaMark(null);
    renderAna(ev && ev.error ? T('offline')
      : T('anScanFrozen').replace('{c}', String((G.ana.defCells || []).length)));
    paint();
  });
  if (!ended && gen === G.ana.gen) DEF_LIVE.handle = h;
}

function defendColor(pct) {
  var B = [[90, 'hsl(214,72%,52%)'], [80, 'hsl(211,72%,54%)'], [70, 'hsl(208,71%,56%)'],
           [60, 'hsl(205,70%,58%)'], [50, 'hsl(202,68%,60%)'], [40, 'hsl(199,66%,62%)'],
           [30, 'hsl(196,64%,64%)'], [20, 'hsl(198,70%,68%)'], [10, 'hsl(201,76%,72%)'],
           [5, 'hsl(203,82%,76%)'], [0, 'hsl(205,88%,80%)']];
  for (var i = 0; i < B.length; i++) if (pct >= B[i][0]) return B[i][1];
  return B[B.length - 1][1];
}

function renderDefendMarks(cells) {
  G.ana.defCells = cells;
  G.ana.marks = [];
  G.ana.rows = cells.slice(0, 8).map(function (c, i) {
    return { i: i, badge: String(i + 1),
             coord: coordName(c.x, c.y), eval: c.pct + '%',
             pv: c.evalTxt ? ('评分 ' + c.evalTxt) : '',
             col: defendColor(c.pct || 0) };
  });
}

function defendAlpha(col, a) {
  return String(col).replace(/^hsl\((.*)\)$/, 'hsla($1,' + a.toFixed(2) + ')');
}
function drawDefendMarks(ctx, cells, pad, gap) {
  var r0 = Math.max(8, gap * 0.43);
  var r3 = Math.max(8, r0 * DEF_TIERS[2].rf);
  var fs5 = Math.max(9, r3 * 0.56 * (DEF_TIERS[2].ff / DEF_TIERS[2].rf));
  var by = [[], [], [], [], []];
  cells.forEach(function (c) { by[Math.max(0, Math.min(4, (c.tier || 5) - 1))].push(c); });
  for (var t = 4; t >= 0; t--) {
    by[t].forEach(function (c) {
      var px = pad + c.x * gap, py = pad + c.y * gap;
      var pct = Math.max(0, Math.min(100, Math.round(c.pct || 0)));
      var col = defendColor(pct);
      if (t >= 3) {
        ctx.save();
        ctx.beginPath();
        ctx.arc(px, py, r0 * DEF_TIERS[3].rf, 0, Math.PI * 2);
        ctx.lineWidth = (t === 3 ? Math.max(1.0, gap * 0.030) : Math.max(0.9, gap * 0.020));
        ctx.strokeStyle = defendAlpha(col, t === 3 ? 0.88 : 0.72);
        ctx.stroke();
        ctx.textAlign = 'center'; ctx.textBaseline = 'middle'; ctx.lineJoin = 'round';
        ctx.font = '700 ' + fs5 + 'px "Segoe UI",system-ui,sans-serif';
        ctx.lineWidth = Math.max(1.4, fs5 * 0.28);
        ctx.strokeStyle = 'rgba(0,0,0,.58)';
        ctx.strokeText(String(pct), px, py);
        ctx.fillStyle = '#fff';
        ctx.fillText(String(pct), px, py);
        ctx.restore();
        return;
      }
      var ti = DEF_TIERS[t];
      var r = Math.max(8, r0 * ti.rf);
      ctx.beginPath(); ctx.arc(px, py, r, 0, Math.PI * 2);
      ctx.fillStyle = defendAlpha(col, ti.fill); ctx.fill();
      ctx.lineWidth = (t === 0 ? Math.max(1.6, gap * 0.044) : Math.max(1, gap * 0.038 * ti.lw));
      ctx.strokeStyle = defendAlpha(col, ti.ring); ctx.stroke();
      drawRingLabels(ctx, px, py, r, pct + '%', '', ti.ff / ti.rf, 2.0 + t * 0.1);
    });
  }
}

var LIVE_AN = {
  on: false, gen: 0, handle: null, items: [], picked: [], depth: 0,
  timer: 0, key: '', t0: 0, frozen: false, lastUi: 0,
};

function liveAnaWanted() {
  if (linkSessionOn()) return false;
  if (S.mode === 'pve' && isAiColor(curColor())) return false;
  return LIVE_AN.on && !RV_MODE && !G.review && !G.over && aiDecorOn() && !aiVsAi();
}

function liveAnaSide() {
  if (S.aiB !== S.aiW) return S.aiB ? 2 : 1;
  return sideByCount();
}
function liveAnaKeyNow() { return G.moves.length + ':' + liveAnaSide() + ':' + boardFp(); }

function liveAnaStopJob() {
  if (LIVE_AN.handle) { try { LIVE_AN.handle.stop(); } catch (e) {} LIVE_AN.handle = null; }
  if (LIVE_AN.timer) { clearTimeout(LIVE_AN.timer); LIVE_AN.timer = 0; }
  G.ana.busy = false;
}
function liveAnaWipeMarks() {
  G.ana.gen++;
  G.ana.kind = ''; G.ana.busy = false;
  G.ana.marks = []; G.ana.rows = [];
  renderAna(T('anIdle'), true);
  paint();
}
function liveAnaClear() {
  LIVE_AN.gen++;
  liveAnaStopJob();
  LIVE_AN.items = []; LIVE_AN.picked = [];
  LIVE_AN.key = ''; LIVE_AN.t0 = 0; LIVE_AN.depth = 0; LIVE_AN.frozen = false; LIVE_AN.lastUi = 0;
  delete LIVE_LANES.main;
  paintLiveDash();
  paint();
}
function liveAnaUI() {
  if (!els.btn_live) return;
  if (aiVsAi() && LIVE_AN.on) { LIVE_AN.on = false; liveAnaClear(); }
  els.btn_live.disabled = aiVsAi();
  els.btn_live.title = aiVsAi() ? T('liveAnaBan') : '';
  els.btn_live.classList.toggle('on', LIVE_AN.on);
  els.btn_live.textContent = LIVE_AN.on ? T('liveAnaStop') : T('liveAna');
}
function liveAnaToggle() {
  if (aiVsAi()) { liveAnaUI(); return; }
  if (linkSessionOn()) {
    setStat(null, '连线会话中不使用「分析计算」—— 评估只针对 AI 执色方进行');
    return;
  }
  LIVE_AN.on = !LIVE_AN.on;
  liveAnaUI();
  if (LIVE_AN.on) {
    aiStandDown();
    bgThinkStop();
    anaLiveStop();
    liveAnaWipeMarks();
    LIVE_AN.frozen = false;
    LIVE_AN.t0 = 0; LIVE_AN.depth = 0;
    liveAnaStart();
  } else {
    liveAnaFreeze();
  }
}

var LIVE_PICK_LIM = [0.08, 0.05, 0.03, 0.05, 0.07, 0.09, 0.11];


function liveAnaPick(cs, n) {
  if (!cs || !cs.length) return [];
  var cap = Math.max(1, Math.min(8, Math.round(n || anaIp())));
  var out = [cs[0]], bw = cs[0].wr;
  for (var i = 1; i < cs.length && out.length < cap; i++) {
    var k = Math.min(out.length - 1, LIVE_PICK_LIM.length - 1);
    if (bw - cs[i].wr > LIVE_PICK_LIM[k] + 1e-9) break;
    out.push(cs[i]);
  }
  return out;
}
function liveAnaApply(res) {
  var cs = ((res && res.candidates) || []).filter(function (c) {
    return c && c.x >= 0 && c.y >= 0 && !(G.board[c.y] && G.board[c.y][c.x]);
  }).map(function (c, i) {
    return { x: c.x, y: c.y, ev: fmtEval(c.eval), wr: (typeof c.winrate === 'number') ? c.winrate : 0.5,
             depth: c.depth || 0, rank: i, line: c.line || [] };
  });
  LIVE_AN.items = cs;
  LIVE_AN.picked = liveAnaPick(cs, anaIp());
  setPvLine((cs[0] && cs[0].line) || [], liveAnaSide());
  if (LIVE_AN.picked.some(function (c) { return /^[+-]?M\d+$/i.test(String(c.ev)); }))
    LIVE_AN.mate = true;
  var d = (res && res.depth) || 0;
  if (d > LIVE_AN.depth) LIVE_AN.depth = d;
  G.ana.marks = LIVE_AN.picked.map(function (c, i) {
    return { live: true, x: c.x, y: c.y, wr: c.wr, ev: c.ev, rank: i, depth: LIVE_AN.depth };
  });
  var now = Date.now();
  if (now - LIVE_AN.lastUi >= 300) {
    LIVE_AN.lastUi = now;
    liveAnaRows();
    renderAna(T('liveAnaRun').replace('{d}', String(LIVE_AN.depth || 0)));
  }
  if (now - (LIVE_AN.lastPaint || 0) >= 120) { LIVE_AN.lastPaint = now; paint(); }
}
function liveAnaRows() {
  G.ana.rows = (LIVE_AN.picked || []).map(function (c, i) {
    return { i: i, badge: String(i + 1), coord: coordName(c.x, c.y),
             eval: Math.round((c.wr || 0) * 100) + '%  ' + c.ev, pv: '' };
  });
  renderAna(T('liveAnaRun').replace('{d}', String(LIVE_AN.depth || 0)));
}

function liveAnaStart() {
  if (!LIVE_AN.on) return;
  if (!liveAnaWanted()) return;
  var gen = ++LIVE_AN.gen;
  liveAnaStopJob();
  var mvN = G.moves.length;
  var side = liveAnaSide();
  LIVE_AN.key = liveAnaKeyNow();
  LIVE_AN.mate = false;
  if (!LIVE_AN.t0) LIVE_AN.t0 = Date.now();
  G.ana.busy = true; G.ana.kind = 'live';
  renderAna(T('liveAnaRun').replace('{d}', String(LIVE_AN.depth || 0)));
  var ended = false;
  var h = LocalAI.live({
    board: G.board, moveList: [], matchMs: 600000, turnMs: 3600000, timeUsedMs: 0,
    topN: anaIp(), rule: engineRule(), cid: 'calc-live-' + mvN, lane: 'main', side: side,
    liveId: LocalAI.nextLiveId(), tag: T('tagLiveAna'),
  }, function (res) {
    if (gen !== LIVE_AN.gen) return;
    if (G.moves.length !== mvN || LIVE_AN.key !== liveAnaKeyNow()) { liveAnaBoardChanged(); return; }
    liveAnaApply(res);
  }, function () {
    ended = true;
    if (gen !== LIVE_AN.gen) return;
    G.ana.busy = false;
    LIVE_AN.handle = null;
    if (LIVE_AN.on && !LIVE_AN.mate && liveAnaWanted()) liveAnaScheduleRetry(350);
  });
  if (!ended && gen === LIVE_AN.gen) LIVE_AN.handle = h;
}
function liveAnaScheduleRetry(ms) {
  if (!LIVE_AN.on || !liveAnaWanted()) return;
  if (LIVE_AN.timer) clearTimeout(LIVE_AN.timer);
  LIVE_AN.timer = setTimeout(function () { LIVE_AN.timer = 0; liveAnaStart(); }, ms || 350);
}

function liveAnaFreeze() {
  LIVE_AN.on = false;
  LIVE_AN.gen++;
  liveAnaStopJob();
  LIVE_AN.frozen = true;
  liveAnaUI();
  G.ana.busy = false; G.ana.kind = '';
  G.ana.marks = (LIVE_AN.picked || []).map(function (c, i) {
    return { live: true, x: c.x, y: c.y, wr: c.wr, ev: c.ev, rank: i, depth: LIVE_AN.depth };
  });
  G.ana.rows = (LIVE_AN.picked || []).map(function (c, i) {
    return { i: i, badge: String(i + 1), coord: coordName(c.x, c.y),
             eval: Math.round((c.wr || 0) * 100) + '%  ' + c.ev, pv: '' };
  });
  renderAna(T('liveAnaStopped').replace('{d}', String(LIVE_AN.depth || 0)));
  paint();
  bgThinkSchedule(300);
}

function liveAnaTouch(restartMs) {
  if (!LIVE_AN.on && !LIVE_AN.frozen) return false;
  LIVE_AN.gen++;
  liveAnaStopJob();
  LIVE_AN.items = []; LIVE_AN.picked = []; LIVE_AN.depth = 0;
  LIVE_AN.lastUi = 0; LIVE_AN.frozen = false; LIVE_AN.mate = false;
  PV_HOVER = null; PV_PREVIEW = null; setPvLine([]);
  liveAnaWipeMarks();
  if (LIVE_AN.on) {
    LIVE_AN.timer = setTimeout(function () { LIVE_AN.timer = 0; liveAnaStart(); },
                               restartMs == null ? 160 : restartMs);
  }
  return true;
}

function liveAnaBoardChanged() {
  liveAnaTouch(120);
}


var BG_AN = { gen: 0, handle: null, key: '', timer: 0 };

function bgThinkBusyBy() {
  if (aiVsAi()) return 'ai';
  if (LIVE_AN.on) return 'an';
  if (G.ana && G.ana.busy) return 'an';
  if (G.fwd && G.fwd.busy) return 'fwd';
  return '';
}
function bgThinkYield() {
  if (bgThinkBusyBy() || !bgThinkWanted()) {
    if (BG_AN.handle || BG_AN.timer) bgThinkStop();
  }
  bgThinkHintSync();
}
function bgThinkHintSync() {
  var h = els.t_bgThinkHint;
  if (!h) return;
  if (!h.getAttribute('data-base')) h.setAttribute('data-base', h.textContent || '');
  var base = h.getAttribute('data-base') || '';
  var why = bgThinkBusyBy();
  var tag = '';
  if (why === 'ai') tag = T('bgThinkOffAi');
  else if (why) tag = T('bgThinkOffBusy');
  var want = tag ? (base ? (base + ' · ' + tag) : tag) : base;
  if (h.textContent !== want) h.textContent = want;
  h.classList.toggle('bg-off', !!tag);
}

function bgThinkIntroOnce() {
  try {
    if (sessionStorage.getItem('gb.bgThinkIntro')) return;
    sessionStorage.setItem('gb.bgThinkIntro', '1');
  } catch (e) { return; }
  try { engMsgAdd(T('bgThinkHint')); } catch (e2) {}
}

function bgThinkWanted() {
  if (!S.bgThink || G.engineOffline || bgThinkBusyBy()) return false;
  if (!S.aiB && !S.aiW) return false;
  if (S.mode !== 'pve' || S.paused) return false;
  if (linkSessionOn() || LINK_MODE || VIS_MODE || RV_MODE || G.review || G.over || G.busy) return false;
  if (openActive()) return false;
  if (isAiColor(curColor())) return false;
  return true;
}
function bgThinkKeyNow() { return G.moves.length + ':' + curColor() + ':' + boardFp(); }
function bgThinkStop() {
  BG_AN.gen++;
  BG_AN.key = '';
  if (BG_AN.timer) { clearTimeout(BG_AN.timer); BG_AN.timer = 0; }
  if (BG_AN.handle) { try { BG_AN.handle.stop(); } catch (e) {} BG_AN.handle = null; }
}
function bgThinkSchedule(ms) {
  if (BG_AN.timer || !bgThinkWanted()) return;
  BG_AN.timer = setTimeout(function () { BG_AN.timer = 0; bgThinkStart(); }, ms == null ? 400 : ms);
}
function bgThinkKick(ms) {
  if (!bgThinkWanted()) {
    if (BG_AN.handle || BG_AN.timer) bgThinkStop();
    return;
  }
  var key = bgThinkKeyNow();
  if ((BG_AN.handle && BG_AN.key === key) || (BG_AN.timer && BG_AN.key === key)) return;
  bgThinkStop();
  BG_AN.key = key;
  bgThinkSchedule(ms == null ? 200 : ms);
}
//
//
function bgThinkStart() {  }

function bgThinkHaltEngine() {
  if (G.busy) return;
  if (G.ana && G.ana.busy) return;
  if (G.fwd && G.fwd.busy) return;
  try { LocalAI.stopLane('main'); } catch (e) {}
}

function boardDecided() {
  var b = G.board, N2 = b.length;
  var DIR = [[1, 0], [0, 1], [1, 1], [1, -1]];
  for (var y = 0; y < N2; y++) for (var x = 0; x < N2; x++) {
    var c = b[y][x];
    if (!c) continue;
    for (var d = 0; d < 4; d++) {
      var dx = DIR[d][0], dy = DIR[d][1];
      if (y - dy >= 0 && y - dy < N2 && x - dx >= 0 && x - dx < N2 && b[y - dy][x - dx] === c) continue;
      var n = 0, px = x, py = y;
      while (px >= 0 && py >= 0 && px < N2 && py < N2 && b[py][px] === c) { n++; px += dx; py += dy; }
      if (n >= 5) return true;                        
      if (n === 4) {                                  
        var ex = x - dx, ey = y - dy;
        if (ex >= 0 && ey >= 0 && ex < N2 && ey < N2 && !b[ey][ex] &&
            px >= 0 && py >= 0 && px < N2 && py < N2 && !b[py][px]) return true;
      }
    }
  }
  return false;
}

function sideByCount() {
  var bn = 0, wn = 0;
  for (var y = 0; y < G.board.length; y++) for (var x = 0; x < G.board.length; x++) {
    if (G.board[y][x] === 1) bn++; else if (G.board[y][x] === 2) wn++;
  }
  return bn > wn ? 2 : 1;
}

function streamWindowMs() { return Math.round(Math.max(4000, (S.turnMs || 2000) * 4.5)); }
function streamGapMs() { return 120; }

var VCX_BUDGET = 600000;              

var VCX_VCF_DEPTH = 64;               
var VCX_VCT_DEPTH = 32;               
var VCX_VCT_ATK_MAX = 16;             
var VCX_WORKER_TIMEOUT = 8000;        
var VCX_WORKER_DEEP = 30000;          
var VCX_STEP_TIMEOUT = 2500;          
                                      
var VCX_FORBID = false;               

var VCX_EXACT_1 = false;              
var VCX_EXACT_2 = false;              
var vcxUsed = 0;                      

function vcxAnalyze(b, x, y, c) {
  vcxUsed++;
  var threats = [], seen = {}, win = false;
  var DIR = [[1, 0], [0, 1], [1, 1], [1, -1]];
  for (var d = 0; d < 4; d++) {
    var dx = DIR[d][0], dy = DIR[d][1];
    for (var k = -4; k <= 0; k++) {
      var x0 = x + dx * k, y0 = y + dy * k;
      var xe = x0 + dx * 4, ye = y0 + dy * 4;                    
      if (x0 < 0 || y0 < 0 || xe < 0 || ye < 0 || x0 >= N || y0 >= N || xe >= N || ye >= N) continue;
      var nC = 1, dead = false;                                  
      var qs = [];
      for (var m = 0; m < 5; m++) {
        if (m === -k) continue;                                  
        var cx = x0 + dx * m, cy = y0 + dy * m;
        var v = b[cy][cx];
        if (v === c) nC++;
        else if (v === 0) qs.push(cy * N + cx);
        else { dead = true; break; }
      }
      if (dead) continue;
      if (nC === 5) {
        
        if (c === 1 ? VCX_EXACT_1 : VCX_EXACT_2) {
          var rl = 1, s1, cxx, cyy;
          for (s1 = 1; s1 <= 5; s1++) { cxx = x + dx * s1; cyy = y + dy * s1; if (cxx < 0 || cyy < 0 || cxx >= N || cyy >= N || b[cyy][cxx] !== c) break; rl++; }
          for (s1 = 1; s1 <= 5; s1++) { cxx = x - dx * s1; cyy = y - dy * s1; if (cxx < 0 || cyy < 0 || cxx >= N || cyy >= N || b[cyy][cxx] !== c) break; rl++; }
          if (rl !== 5) continue;                                  
        }
        win = true; return { win: true, threats: threats };
      }
      if (nC === 4 && qs.length === 1 && !seen[qs[0]]) { seen[qs[0]] = 1; threats.push(qs[0]); }
    }
  }
  return { win: win, threats: threats };
}

function vcxDefCanFive(b, def) {
  var DIR = [[1, 0], [0, 1], [1, 1], [1, -1]];
  for (var y = 0; y < N; y++) for (var x = 0; x < N; x++) {
    if (b[y][x]) continue;
    vcxUsed++;
    for (var d = 0; d < 4; d++) {
      var dx = DIR[d][0], dy = DIR[d][1];
      for (var k = -4; k <= 0; k++) {
        var x0 = x + dx * k, y0 = y + dy * k;
        var xe2 = x0 + dx * 4, ye2 = y0 + dy * 4;
        if (x0 < 0 || y0 < 0 || xe2 < 0 || ye2 < 0 || x0 >= N || y0 >= N || xe2 >= N || ye2 >= N) continue;
        var nC = 1, dead = false;
        for (var m = 0; m < 5; m++) {
          if (m === -k) continue;
          var v = b[y0 + dy * m][x0 + dx * m];
          if (v === def) nC++;
          else if (v !== 0) { dead = true; break; }
        }
        if (!dead && nC === 5) {
          
          if (def === 1 ? VCX_EXACT_1 : VCX_EXACT_2) {
            var rl2 = 1, s2, cx2b, cy2b;
            for (s2 = 1; s2 <= 5; s2++) { cx2b = x + dx * s2; cy2b = y + dy * s2; if (cx2b < 0 || cy2b < 0 || cx2b >= N || cy2b >= N || b[cy2b][cx2b] !== def) break; rl2++; }
            for (s2 = 1; s2 <= 5; s2++) { cx2b = x - dx * s2; cy2b = y - dy * s2; if (cx2b < 0 || cy2b < 0 || cx2b >= N || cy2b >= N || b[cy2b][cx2b] !== def) break; rl2++; }
            if (rl2 !== 5) continue;
          }
          return true;
        }
      }
    }
  }
  return false;
}

function vcxNearStone(b, x, y) {
  for (var dy = -2; dy <= 2; dy++) for (var dx = -2; dx <= 2; dx++) {
    var nx = x + dx, ny = y + dy;
    if (nx >= 0 && ny >= 0 && nx < N && ny < N && b[ny][nx]) return true;
  }
  return false;
}

function vcxRunLenAt(b, q, c) {
  c = c || 1;
  var x = q % N, y = (q / N) | 0, DIR = [[1, 0], [0, 1], [1, 1], [1, -1]];
  b[y][x] = c;
  var best = 1;
  for (var d = 0; d < 4; d++) {
    var dx = DIR[d][0], dy = DIR[d][1], run = 1, s;
    for (s = 1; s <= 5; s++) { var cx = x + dx * s, cy = y + dy * s; if (cx < 0 || cy < 0 || cx >= N || cy >= N || b[cy][cx] !== c) break; run++; }
    for (s = 1; s <= 5; s++) { var cx2 = x - dx * s, cy2 = y - dy * s; if (cx2 < 0 || cy2 < 0 || cx2 >= N || cy2 >= N || b[cy2][cx2] !== c) break; run++; }
    if (run > best) best = run;
  }
  b[y][x] = 0;
  return best;
}

function vcxOverlineAt(b, q) { return vcxRunLenAt(b, q, 1) >= 6; }

function vcxFiveReal(b, q, c) {
  var x = q % N, y = (q / N) | 0;
  if (x < 0 || y < 0 || x >= N || y >= N || b[y][x]) return false;
  var need = (c === 1) ? VCX_EXACT_1 : VCX_EXACT_2;
  var DIR = [[1, 0], [0, 1], [1, 1], [1, -1]];
  b[y][x] = c;
  var hit = false;
  for (var d = 0; d < 4 && !hit; d++) {
    var dx = DIR[d][0], dy = DIR[d][1];
    for (var k = -4; k <= 0; k++) {
      var x0 = x + dx * k, y0 = y + dy * k, xe = x0 + dx * 4, ye = y0 + dy * 4;
      if (x0 < 0 || y0 < 0 || xe < 0 || ye < 0 || x0 >= N || y0 >= N || xe >= N || ye >= N) continue;
      var full = true;
      for (var m = 0; m < 5; m++) if (b[y0 + dy * m][x0 + dx * m] !== c) { full = false; break; }
      if (full) { hit = true; break; }
    }
  }
  b[y][x] = 0;
  if (!hit) return false;
  if (!need) return true;                                  
  return vcxRunLenAt(b, q, c) === 5;                       
}

function vcxRealThreats(b, arr, c, ax, ay) {
  c = c || 1;
  var need = (c === 1) ? VCX_EXACT_1 : VCX_EXACT_2;
  var forbid = VCX_FORBID && c === 1;
  if (!need && !forbid) return arr;                        
  var anchored = (ax != null && ax >= 0);
  if (anchored) b[ay][ax] = c;
  var out = [];
  for (var i = 0; i < arr.length; i++) {
    var q = arr[i];
    if (forbid && vcxForbidden(b, q % N, (q / N) | 0)) continue;   
    if (vcxFiveReal(b, q, c)) out.push(q);
  }
  if (anchored) b[ay][ax] = 0;
  return out;
}

var VCX_PAT_DEAD = 0, VCX_PAT_OL = 1, VCX_PAT_B1 = 2, VCX_PAT_F1 = 3, VCX_PAT_B2 = 4,
    VCX_PAT_F2 = 5, VCX_PAT_F2A = 6, VCX_PAT_F2B = 7, VCX_PAT_B3 = 8, VCX_PAT_B3S = 9,
    VCX_PAT_F3 = 10, VCX_PAT_F3S = 11, VCX_PAT_B4 = 12, VCX_PAT_B4S = 13, VCX_PAT_F4 = 14,
    VCX_PAT_F5 = 15;

var VCX_P4_NONE = 0, VCX_P4_FORBID = 4, VCX_P4_FIVE = 5, VCX_P4_BFLEX4 = 13;
var VCX_PAT_MEMO = new Map();                        

function vcxLineOf(b, x, y, d) {
  var a = [], k;
  if (d === 0) { for (k = 0; k < N; k++) a.push(b[y][k]); return { a: a, m: x }; }
  if (d === 1) { for (k = 0; k < N; k++) a.push(b[k][x]); return { a: a, m: y }; }
  if (d === 2) {                                     
    var sx = x, sy = y;
    while (sx > 0 && sy > 0) { sx--; sy--; }
    while (sx < N && sy < N) { a.push(b[sy][sx]); sx++; sy++; }
    return { a: a, m: x - (sx - a.length) };
  }
  var tx = x, ty = y;                                
  while (tx > 0 && ty < N - 1) { tx--; ty++; }
  while (tx < N && ty >= 0) { a.push(b[ty][tx]); tx++; ty--; }
  return { a: a, m: x - (tx - a.length) };
}

var VCX_LINE_HALF = 5;                               

function vcxLinePat(line, m) {                       
  var win = [];
  for (var j = -VCX_LINE_HALF; j <= VCX_LINE_HALF; j++) {
    var idx = m + j;
    win.push(idx >= 0 && idx < line.length ? line[idx] : 2);
  }
  return vcxLinePatWin(win);
}

function vcxLinePatWin(win) {                        
  var key = win.join('');
  var hit = VCX_PAT_MEMO.get(key);
  if (hit !== undefined) return hit;
  var L = win.length, realLen = 1, fullLen = 1, inc = 1, start = 5, end = 5, i, j, p;
  for (i = 4; i >= 0; i--) {                         
    if (win[i] === 1) { if (inc) realLen++; }
    else if (win[i] === 2) break;
    else inc = 0;
    fullLen++; start = i;
  }
  inc = 1;
  for (i = 6; i < L; i++) {                          
    if (win[i] === 1) { if (inc) realLen++; }
    else if (win[i] === 2) break;
    else inc = 0;
    fullLen++; end = i;
  }
  if (realLen >= 6) p = VCX_PAT_OL;                  
  else if (realLen >= 5) p = VCX_PAT_F5;             
  else if (fullLen < 5) p = VCX_PAT_DEAD;            
  else {
    var cnt = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    var f5a = -1, f5b = -1;
    for (i = start; i <= end; i++) {                 
      if (win[i] !== 0) continue;
      win[i] = 1;
      var w2 = [];
      for (j = -VCX_LINE_HALF; j <= VCX_LINE_HALF; j++) {
        var t = i + j;
        w2.push(t >= 0 && t < L ? win[t] : 2);       
      }
      var sp = vcxLinePatWin(w2);
      win[i] = 0;
      if (sp === VCX_PAT_F5) { if (f5a < 0) f5a = i; else if (f5b < 0) f5b = i; }
      cnt[sp]++;
    }
    if (cnt[VCX_PAT_F5] >= 2) {
      p = VCX_PAT_F4;                                
      if (f5b >= 0 && f5b - f5a < 5) p = VCX_PAT_OL; 
    } else if (cnt[VCX_PAT_F5] === 1) {              
      win[f5a] = 2;
      p = vcxLinePatWin(win) >= VCX_PAT_B3 ? VCX_PAT_B4S : VCX_PAT_B4;
      win[f5a] = 0;
    }
    else if (cnt[VCX_PAT_F4] >= 2) p = VCX_PAT_F3S;  
    else if (cnt[VCX_PAT_F4]) p = VCX_PAT_F3;
    else if (cnt[VCX_PAT_B4S]) p = VCX_PAT_B3S;
    else if (cnt[VCX_PAT_B4]) p = VCX_PAT_B3;
    else if (cnt[VCX_PAT_F3S] + cnt[VCX_PAT_F3] >= 4) p = VCX_PAT_F2B;
    else if (cnt[VCX_PAT_F3S] + cnt[VCX_PAT_F3] >= 3) p = VCX_PAT_F2A;
    else if (cnt[VCX_PAT_F3S] + cnt[VCX_PAT_F3]) p = VCX_PAT_F2;
    else if (cnt[VCX_PAT_B3S] + cnt[VCX_PAT_B3]) p = VCX_PAT_B2;
    else if (cnt[VCX_PAT_F2B] + cnt[VCX_PAT_F2A] + cnt[VCX_PAT_F2]) p = VCX_PAT_F1;
    else if (cnt[VCX_PAT_B2]) p = VCX_PAT_B1;
    else p = VCX_PAT_DEAD;
  }
  VCX_PAT_MEMO.set(key, p);
  return p;
}

function vcxFused4(pats) {
  var nF5 = 0, nOL = 0, nB4 = 0, nF4 = 0, nF3 = 0;
  for (var d = 0; d < 4; d++) {
    var p = pats[d];
    if (p === VCX_PAT_F5) nF5++;
    else if (p === VCX_PAT_OL) nOL++;
    else if (p === VCX_PAT_B4 || p === VCX_PAT_B4S) nB4++;
    else if (p === VCX_PAT_F4) nF4++;
    else if (p === VCX_PAT_F3 || p === VCX_PAT_F3S) nF3++;
  }
  if (nF5) return VCX_P4_FIVE;                       
  if (nOL) return VCX_P4_FORBID;
  if (nB4 + nF4 >= 2) return VCX_P4_FORBID;          
  if (nF3 >= 2) return VCX_P4_FORBID;                
  if (nF4) return VCX_P4_BFLEX4;                     
  return VCX_P4_NONE;
}

function vcxForbidden(b, x, y) {
  var VCXDIR = [[1, 0], [0, 1], [1, 1], [1, -1]];
  b[y][x] = 1;
  var pats = [];
  for (var d = 0; d < 4; d++) {
    var ln = vcxLineOf(b, x, y, d);
    pats.push(vcxLinePat(ln.a, ln.m));
  }
  b[y][x] = 0;
  var nF5 = 0, nOL = 0, fours = 0, threes = [];
  for (d = 0; d < 4; d++) {
    var p = pats[d];
    if (p === VCX_PAT_F5) nF5++;
    else if (p === VCX_PAT_OL) nOL++;
    else if (p >= VCX_PAT_B4) fours++;               
    else if (p === VCX_PAT_F3 || p === VCX_PAT_F3S) threes.push(d);
  }
  if (nF5) return false;                             
  if (nOL) return true;                              
  if (fours >= 2) return true;                       
  if (threes.length < 2) return false;               
  
  var win3 = 0;
  b[y][x] = 1;
  for (var t = 0; t < threes.length && win3 < 2; t++) {
    var dd = threes[t], dx = VCXDIR[dd][0], dy = VCXDIR[dd][1];
    for (var sg = -1; sg <= 1; sg += 2) {            
      var hit = false;
      for (var k = 1; k <= 4; k++) {
        var px = x + dx * k * sg, py = y + dy * k * sg;
        if (px < 0 || py < 0 || px >= N || py >= N) break;
        var v = b[py][px];
        if (v === 2) break;                          
        if (v === 0) {                               
          b[py][px] = 1;
          var ln2 = vcxLineOf(b, px, py, dd);
          var pd = vcxLinePat(ln2.a, ln2.m);
          var patsE = [];
          for (var q = 0; q < 4; q++) {
            var le = vcxLineOf(b, px, py, q);
            patsE.push(vcxLinePat(le.a, le.m));
          }
          var f4 = vcxFused4(patsE);
          var ok = f4 === VCX_P4_BFLEX4 || pd === VCX_PAT_F5;
          if (!ok && f4 === VCX_P4_FORBID && pd === VCX_PAT_F4) {
            b[py][px] = 0;                           
            ok = !vcxForbidden(b, px, py);
            b[py][px] = 1;
          }
          b[py][px] = 0;
          hit = ok;
          break;                                     
        }
        
      }
      if (hit) { win3++; break; }                    
    }
  }
  b[y][x] = 0;
  return win3 >= 2;
}

function vcxVcfTry(b, mv, atk, dLeft) {
  var def = 3 - atk, line = null;
  b[mv.y][mv.x] = atk;
  if (!vcxDefCanFive(b, def)) {                                
    if (mv.live) {                                             
      var q0 = mv.threats[0], q1 = mv.threats[1];
      
      if (VCX_FORBID && def === 1) {
        var f0 = vcxForbidden(b, q0 % N, (q0 / N) | 0), f1 = vcxForbidden(b, q1 % N, (q1 / N) | 0);
        if (f0 && f1) { b[mv.y][mv.x] = 0; return null; }
        if (f0) { var tq = q0; q0 = q1; q1 = tq; }
      }
      line = [{ x: mv.x, y: mv.y, c: atk },
              { x: q0 % N, y: (q0 / N) | 0, c: def },
              { x: q1 % N, y: (q1 / N) | 0, c: atk }];
    } else {
      
      var qx = mv.q % N, qy = (mv.q / N) | 0;
      
      if (VCX_FORBID && def === 1 && vcxForbidden(b, qx, qy)) { b[mv.y][mv.x] = 0; return null; }
      b[qy][qx] = def;
      var sub = vcxVcfLoop(b, atk, dLeft - 1, 0, 1);
      b[qy][qx] = 0;
      if (sub) line = [{ x: mv.x, y: mv.y, c: atk },
                       { x: qx, y: qy, c: def }].concat(sub);
    }
  }
  b[mv.y][mv.x] = 0;
  return line;
}

function vcxVcfLoop(b, atk, dLeft, skip, mod) {
  if (dLeft <= 0 || vcxUsed > VCX_BUDGET) return null;
  var cands = [], liveFour = null;
  for (var y = 0; y < N; y++) for (var x = 0; x < N; x++) {
    if (b[y][x] || !vcxNearStone(b, x, y)) continue;
    if (VCX_FORBID && atk === 1 && vcxForbidden(b, x, y)) continue;   
    var a = vcxAnalyze(b, x, y, atk);
    if (a.win) return [{ x: x, y: y, c: atk }];
    var th = vcxRealThreats(b, a.threats, atk, x, y);            
    if (th.length >= 2) { if (!liveFour) liveFour = { live: true, x: x, y: y, threats: th }; }
    else if (th.length === 1) cands.push({ live: false, x: x, y: y, q: th[0] });
  }
  var tries = [];
  if (liveFour) tries.push(liveFour);
  for (var i = 0; i < cands.length; i++) tries.push(cands[i]);
  for (var t = (skip || 0); t < tries.length; t += (mod || 1)) {
    var line = vcxVcfTry(b, tries[t], atk, dLeft);
    if (line) return line;
    if (vcxUsed > VCX_BUDGET) return null;
  }
  return null;
}

function vcxVctCands(b, atk, atkMax) {
  var fours = [], threes = [];
  var DIR = [[1, 0], [0, 1], [1, 1], [1, -1]];
  for (var y = 0; y < N; y++) for (var x = 0; x < N; x++) {
    if (b[y][x] || !vcxNearStone(b, x, y)) continue;
    if (VCX_FORBID && atk === 1 && vcxForbidden(b, x, y)) continue;   
    var a = vcxAnalyze(b, x, y, atk);
    if (a.win) return [{ x: x, y: y, w: 3, threats: [] }];
    var th = vcxRealThreats(b, a.threats, atk, x, y);            
    if (th.length >= 2) fours.unshift({ x: x, y: y, w: 2, threats: th });   
    else if (th.length === 1) fours.push({ x: x, y: y, w: 1, threats: th });
    else {                                                       
      var live = false;                                          
      b[y][x] = atk;
      for (var d = 0; d < 4 && !live; d++)
        for (var k = -4; k <= 4 && !live; k++) {
          if (k === 0) continue;
          var qx = x + DIR[d][0] * k, qy = y + DIR[d][1] * k;
          if (qx < 0 || qy < 0 || qx >= N || qy >= N || b[qy][qx]) continue;
          var a3 = vcxAnalyze(b, qx, qy, atk);
          if (vcxRealThreats(b, a3.threats, atk, qx, qy).length >= 2) live = true;
        }
      b[y][x] = 0;
      if (live) threes.push({ x: x, y: y, w: 0, threats: [] });
    }
  }
  
  return fours.concat(threes.slice(0, atkMax));
}

function vcxVctTry(b, mv, atk, dLeft) {
  var def = 3 - atk, line = null;
  if (mv.w === 3) return [{ x: mv.x, y: mv.y, c: atk }];         
  b[mv.y][mv.x] = atk;
  if (mv.w === 2) {                                              
    if (!vcxDefCanFive(b, def)) {
      var q0 = mv.threats[0], q1 = mv.threats[1];
      
      if (VCX_FORBID && def === 1) {
        var g0 = vcxForbidden(b, q0 % N, (q0 / N) | 0), g1 = vcxForbidden(b, q1 % N, (q1 / N) | 0);
        if (g0 && g1) { b[mv.y][mv.x] = 0; return null; }
        if (g0) { var tq2 = q0; q0 = q1; q1 = tq2; }
      }
      line = [{ x: mv.x, y: mv.y, c: atk },
              { x: q0 % N, y: (q0 / N) | 0, c: def },
              { x: q1 % N, y: (q1 / N) | 0, c: atk }];
    }
  } else if (mv.w === 1) {                                       
    if (!vcxDefCanFive(b, def)) {
      var qx = mv.threats[0] % N, qy = (mv.threats[0] / N) | 0;
      
      if (VCX_FORBID && def === 1 && vcxForbidden(b, qx, qy)) { b[mv.y][mv.x] = 0; return null; }
      b[qy][qx] = def;
      var sub = vcxVctLoop(b, atk, dLeft - 1, 0, 1);
      b[qy][qx] = 0;
      if (sub) line = [{ x: mv.x, y: mv.y, c: atk }, { x: qx, y: qy, c: def }].concat(sub);
    }
  } else {                                                       
    if (!vcxDefCanFive(b, def)) {
      var replies = vcxDefReplies(b, mv.x, mv.y, atk);
      var allLose = replies.length > 0, best = null;
      for (var r = 0; r < replies.length; r++) {
        var rp = replies[r];
        b[rp.y][rp.x] = def;
        var sub2 = vcxVctLoop(b, atk, dLeft - 1, 0, 1);
        b[rp.y][rp.x] = 0;
        if (!sub2) { allLose = false; break; }                   
        if (!best) best = { rp: rp, sub: sub2 };
        if (vcxUsed > VCX_BUDGET) break;
      }
      if (allLose && best)
        line = [{ x: mv.x, y: mv.y, c: atk },
                { x: best.rp.x, y: best.rp.y, c: def }].concat(best.sub);
    }
  }
  b[mv.y][mv.x] = 0;
  return line;
}

function vcxVctLoop(b, atk, dLeft, skip, mod) {
  if (dLeft <= 0 || vcxUsed > VCX_BUDGET) return null;
  var cands = vcxVctCands(b, atk, VCX_VCT_ATK_MAX);
  for (var i = (skip || 0); i < cands.length; i += (mod || 1)) {
    if (vcxUsed > VCX_BUDGET) return null;
    var line = vcxVctTry(b, cands[i], atk, dLeft);
    if (line) return line;
  }
  return null;
}

function vcxDefReplies(b, px, py, atk) {
  var def = 3 - atk, set = {}, list = [];
  var DIR = [[1, 0], [0, 1], [1, 1], [1, -1]];
  for (var d = 0; d < 4; d++)
    for (var k = -4; k <= 4; k++) {
      if (k === 0) continue;
      var nx = px + DIR[d][0] * k, ny = py + DIR[d][1] * k;
      if (nx < 0 || ny < 0 || nx >= N || ny >= N || b[ny][nx]) continue;
      var key = ny * N + nx;
      if (set[key]) continue;
      set[key] = 1;
      var aA = vcxAnalyze(b, nx, ny, atk);                                  
      var mustBlock = aA.win || vcxRealThreats(b, aA.threats, atk, nx, ny).length >= 2;  
      var counter = false;
      if (!mustBlock) {                                                     
        var aD = vcxAnalyze(b, nx, ny, def);
        counter = aD.win || vcxRealThreats(b, aD.threats, def, nx, ny).length >= 1;
      }
      if (!mustBlock && !counter) continue;                                 
      
      if (VCX_FORBID && def === 1 && vcxForbidden(b, nx, ny)) continue;
      list.push({ x: nx, y: ny, w: counter ? 0 : 1, d: Math.max(Math.abs(nx - px), Math.abs(ny - py)) });
    }
  list.sort(function (a, c) { return (a.w - c.w) || (a.d - c.d); });
  return list;                                                              
}

function vcxId(loopFn, b, atk, d0, dMax) {
  for (var d = d0; d <= dMax; d++) {
    var line = loopFn(b, atk, d, 0, 1);
    if (line) return line;
    if (vcxUsed > VCX_BUDGET) return null;
  }
  return null;
}

function vcxAudit(b0, line, renju, ex1, ex2, atk) {
  if (!line || !line.length) return false;
  var tb = b0.map(function (row) { return row.slice(); });
  var expect = atk;
  for (var i = 0; i < line.length; i++) {
    var m = line[i];
    if (!m || m.x < 0 || m.y < 0 || m.x >= N || m.y >= N) return false;
    if (m.c !== expect) return false;                     
    if (tb[m.y][m.x] !== 0) return false;                 
    if (renju && m.c === 1 && vcxForbidden(tb, m.x, m.y)) return false;   
    tb[m.y][m.x] = m.c;
    expect = 3 - expect;
  }
  var last = line[line.length - 1];
  if (last.c !== atk) return false;
  
  var need = (last.c === 1) ? ex1 : ex2;
  var DIR = [[1, 0], [0, 1], [1, 1], [1, -1]], hit = false;
  for (var d = 0; d < 4 && !hit; d++) {
    var dx = DIR[d][0], dy = DIR[d][1];
    for (var k = -4; k <= 0; k++) {
      var x0 = last.x + dx * k, y0 = last.y + dy * k, xe = x0 + dx * 4, ye = y0 + dy * 4;
      if (x0 < 0 || y0 < 0 || xe < 0 || ye < 0 || x0 >= N || y0 >= N || xe >= N || ye >= N) continue;
      var full = true;
      for (var m2 = 0; m2 < 5; m2++) if (tb[y0 + dy * m2][x0 + dx * m2] !== last.c) { full = false; break; }
      if (full) { hit = true; break; }
    }
  }
  if (!hit) return false;
  if (need) {                                             
    var best = 1;
    for (var d2 = 0; d2 < 4; d2++) {
      var rx = DIR[d2][0], ry = DIR[d2][1], r0 = 1, s2;
      for (s2 = 1; s2 < N; s2++) { var ax = last.x + rx * s2, ay = last.y + ry * s2; if (ax < 0 || ay < 0 || ax >= N || ay >= N || tb[ay][ax] !== last.c) break; r0++; }
      for (s2 = 1; s2 < N; s2++) { var bx = last.x - rx * s2, by = last.y - ry * s2; if (bx < 0 || by < 0 || bx >= N || by >= N || tb[by][bx] !== last.c) break; r0++; }
      if (r0 > best) best = r0;
    }
    if (best !== 5) return false;
  }
  return true;
}

function vcxSolve(b, rule, mode, atkSel) {
  mode = mode || 'auto';
  var total = 0;
  for (var i = 0; i < N; i++) for (var j = 0; j < N; j++) if (b[i][j]) total++;
  if (total === 0) return null;
  var toMove = (total % 2 === 0) ? 1 : 2;                        
  if (atkSel && atkSel !== toMove) return null;                  
  var side = atkSel || toMove;
  var renju = (rule === 2);
  VCX_FORBID = (renju && side === 1);                            
  VCX_EXACT_1 = exactFiveFor(rule, 1);                           
  VCX_EXACT_2 = exactFiveFor(rule, 2);
  vcxUsed = 0;
  if (mode !== 'vct') {                                          
    var tmp = b.map(function (row) { return row.slice(); });
    var line = vcxId(vcxVcfLoop, tmp, side, 2, VCX_VCF_DEPTH);
    
    if (line && line.length && vcxAudit(b, line, renju, VCX_EXACT_1, VCX_EXACT_2, side))
      return { kind: 'VCF', line: line };
  }
  if (mode !== 'vcf' && vcxUsed <= VCX_BUDGET) {                 
    var tmp2 = b.map(function (row) { return row.slice(); });
    var line2 = vcxId(vcxVctLoop, tmp2, side, 2, VCX_VCT_DEPTH);
    if (line2 && line2.length && vcxAudit(b, line2, renju, VCX_EXACT_1, VCX_EXACT_2, side))
      return { kind: 'VCT', line: line2 };
  }
  return null;
}

function vcxSolveAsync(b, rule, mode) {
  var atkSel = arguments.length > 3 ? arguments[3] : 0;   
  var tOutArg = arguments.length > 4 ? arguments[4] : 0;  
  mode = mode || 'auto';
  return new Promise(function (resolve) {
    var total = 0;
    for (var i = 0; i < N; i++) for (var j = 0; j < N; j++) if (b[i][j]) total++;
    if (total === 0) { resolve(null); return; }
    var toMove = (total % 2 === 0) ? 1 : 2;                      
    
    if (atkSel && atkSel !== toMove) { resolve(null); return; }
    var side = atkSel || toMove;                                 
    var rj = (rule === 2 && side === 1);                         
    VCX_FORBID = rj;
    var ex1 = exactFiveFor(rule, 1), ex2 = exactFiveFor(rule, 2);   
    VCX_EXACT_1 = ex1; VCX_EXACT_2 = ex2;
    var tOut = tOutArg || ((mode === 'auto') ? VCX_WORKER_TIMEOUT : VCX_WORKER_DEEP);   
    var nCore = Math.min(4, Math.max(2, (typeof navigator !== 'undefined' && navigator.hardwareConcurrency) || 2));
    var src = 'var N=' + N + ',VCX_BUDGET=' + VCX_BUDGET + ',VCX_VCF_DEPTH=' + VCX_VCF_DEPTH +
              ',VCX_VCT_DEPTH=' + VCX_VCT_DEPTH + ',VCX_VCT_ATK_MAX=' + VCX_VCT_ATK_MAX +
              ',VCX_FORBID=' + (rj ? 'true' : 'false') +
              ',VCX_EXACT_1=' + (ex1 ? 'true' : 'false') +
              ',VCX_EXACT_2=' + (ex2 ? 'true' : 'false') +
              ',vcxUsed=0;\n' +
              
              'var VCX_PAT_DEAD=0,VCX_PAT_OL=1,VCX_PAT_B1=2,VCX_PAT_F1=3,VCX_PAT_B2=4,' +
              'VCX_PAT_F2=5,VCX_PAT_F2A=6,VCX_PAT_F2B=7,VCX_PAT_B3=8,VCX_PAT_B3S=9,' +
              'VCX_PAT_F3=10,VCX_PAT_F3S=11,VCX_PAT_B4=12,VCX_PAT_B4S=13,VCX_PAT_F4=14,' +
              'VCX_PAT_F5=15,VCX_P4_NONE=0,VCX_P4_FORBID=4,VCX_P4_FIVE=5,VCX_P4_BFLEX4=13,' +
              'VCX_PAT_MEMO=new Map();\n' +
              
              [vcxAnalyze, vcxDefCanFive, vcxNearStone, vcxOverlineAt, vcxRealThreats,
               vcxRunLenAt, vcxFiveReal, vcxAudit,
               vcxLineOf, vcxLinePat, vcxFused4, vcxForbidden, vcxVcfTry, vcxVcfLoop,
               vcxVctCands, vcxVctTry, vcxVctLoop, vcxDefReplies,
               exactFiveFor].map(function (f) { return f.toString(); }).join('\n') +
              '\n;onmessage=function(e){var d=e.data,atk=d.atk,vcxUsed=0;' +
              
              'var b0=d.board.map(function(r){return r.slice();});' +
              'var b=b0.map(function(r){return r.slice();});' +
              'VCX_FORBID=!!d.forbid;VCX_EXACT_1=!!d.ex1;VCX_EXACT_2=!!d.ex2;' +
              'var line=null;try{' +
              'if(d.task==="vcf"){for(var dd=2;dd<=' + VCX_VCF_DEPTH + ';dd++){line=vcxVcfLoop(b,atk,dd,d.skip,d.mod);if(line)break;}}' +
              'else{for(var dd2=2;dd2<=' + VCX_VCT_DEPTH + ';dd2++){line=vcxVctLoop(b,atk,dd2,d.skip,d.mod);if(line)break;}}' +
              '}catch(err){line=null;}' +
              
              'if(line&&line.length&&!vcxAudit(b0,line,!!d.forbid,!!d.ex1,!!d.ex2,atk))line=null;' +
              'postMessage({task:d.task,line:line||null,used:vcxUsed});};';
    var workers = [], done = false, got = 0, expect = 0, fallenBack = false;
    function finish(res) {
      if (done) return;
      done = true;
      workers.forEach(function (w) { try { w.terminate(); } catch (e) {} });
      resolve(res);
    }
    var timer = setTimeout(function () { finish(null); }, tOut);   
    function settleOne() {
      got++;
      if (got >= expect && !done) { clearTimeout(timer); finish(null); }
    }
    try {
      var blob = new Blob([src], { type: 'application/javascript' });
      var url = URL.createObjectURL(blob);
      var tasks = [];
      if (mode !== 'vct') {
        if (mode === 'vcf') {                      
          for (var v = 0; v < nCore; v++) tasks.push({ task: 'vcf', skip: v, mod: nCore });
        } else {
          tasks.push({ task: 'vcf', skip: 0, mod: 1 });
        }
      }
      if (mode !== 'vcf') {                        
        var nw = (mode === 'vct') ? nCore : nCore - 1;
        for (var w = 0; w < Math.max(1, nw); w++) tasks.push({ task: 'vct', skip: w, mod: Math.max(1, nw) });
      }
      expect = tasks.length;
      tasks.forEach(function (t) {
        var wk = new Worker(url);
        wk.onmessage = function (ev) {
          if (done) return;
          var r = ev.data;
          if (r && r.line && r.line.length) {
            clearTimeout(timer);
            finish({ kind: r.task.toUpperCase(), line: r.line });
            return;
          }
          settleOne();
        };
        wk.onerror = function () { settleOne(); };
        wk.postMessage({ task: t.task, skip: t.skip, mod: t.mod, atk: side,
                         forbid: rj,                                 
                         ex1: ex1, ex2: ex2,                         
                         board: b.map(function (row) { return row.slice(); }) });
        workers.push(wk);
      });
    } catch (e) {
      if (fallenBack) return;
      fallenBack = true;
      clearTimeout(timer);
      finish(vcxSolve(b, rule, mode, atkSel));                     
    }
  });
}

function fwdFindUi() {
  var k = G.fwd.kind || '';
  if (els.btn_fwd_vcf) els.btn_fwd_vcf.classList.toggle('find-on', k === 'VCF');
  if (els.btn_fwd_vct) els.btn_fwd_vct.classList.toggle('find-on', k === 'VCT');
}

async function fwdTryVcx(b, kind, budgetMs) {
  var bb = b.map(function (row) { return row.slice(); });
  var k = (kind === 'auto') ? 'auto' : String(kind).toLowerCase();
  var r = await vcxSolveAsync(bb, engineRule(), k, G.fwd.atk, budgetMs);
  return (r && r.line && r.line.length) ? r : null;
}

function fwdVcxMarks(b, line, startIdx) {
  var marks = [];
  if (!line || !line.length) return marks;
  var t = b.map(function (row) { return row.slice(); });
  var DIRS = [[1, 0], [0, 1], [1, 1], [1, -1]];
  
  var atk = (line[0] && line[0].c) || G.fwd.atk || 0;
  for (var i = 0; i < line.length; i++) {
    var m = line[i];
    if (m.y < 0 || m.y >= N || m.x < 0 || m.x >= N) continue;
    t[m.y][m.x] = m.c;
    if (atk && m.c !== atk) { marks.push({ i: startIdx + i, x: m.x, y: m.y, c: m.c, t: 0 }); continue; }
    var best = 0;
    for (var d = 0; d < 4; d++) {
      var dx = DIRS[d][0], dy = DIRS[d][1], run = 1;
      for (var s = 1; s < 6; s++) { var y1 = m.y + dy * s, x1 = m.x + dx * s; if (y1 < 0 || y1 >= N || x1 < 0 || x1 >= N || t[y1][x1] !== m.c) break; run++; }
      for (var s2 = 1; s2 < 6; s2++) { var y2 = m.y - dy * s2, x2 = m.x - dx * s2; if (y2 < 0 || y2 >= N || x2 < 0 || x2 >= N || t[y2][x2] !== m.c) break; run++; }
      if (run > best) best = run;
    }
    if (best < 3) continue;                        
    marks.push({ i: startIdx + i, x: m.x, y: m.y, c: m.c, t: Math.min(5, best) });
  }
  return marks;
}

function fwdAdoptVcx(b, vcx, from) {
  G.fwd.kind = vcx.kind;
  G.fwd.vcxAt = from;
  var bb = b.map(function (row) { return row.slice(); });
  var cut = vcx.line.length;
  for (var i = 0; i < vcx.line.length; i++) {
    var m = vcx.line[i];
    if (bb[m.y] && !bb[m.y][m.x]) bb[m.y][m.x] = m.c;
    if (winCheck(bb, m.x, m.y, m.c)) { cut = i + 1; break; }   
  }
  var line = vcx.line.slice(0, cut);
  G.fwd.vcxMarks = fwdVcxMarks(b, line, from);
  for (var j = 0; j < line.length; j++) G.fwd.line.push(line[j]);
}

async function fwdFind(kind) {
  if (RV_MODE || G.review || G.over) return false;
  if (!G.fwd.on) {                     
    G.fwd.on = true;
    if (els.btn_fwd) els.btn_fwd.classList.add('on');
    fwdQuietAi();                      
  }
  G.fwd.wantVcx = true;
  G.fwd.want = kind;
  G.fwd.want2 = (kind === 'VCF') ? 'VCT' : 'VCF';
  G.fwd.kind = kind;                   
  fwdFindUi();
  await fwdRun();
  return !!(G.fwd.line && G.fwd.line.length);
}

function visVcArm() {
  if (RV_MODE || G.review || G.over || G.fwd.busy) return;
  var nb = 0, nw = 0;
  for (var ai = 0; ai < N; ai++) for (var aj = 0; aj < N; aj++) {
    if (G.board[ai][aj] === 1) nb++; else if (G.board[ai][aj] === 2) nw++;
  }
  G.fwd.atk = (nb > nw) ? 1 : (nw > nb ? 2 : 0);
  fwdAtkUi();
  if (els.fwdMsg) els.fwdMsg.textContent = T('visVcArm');
}

function vcxPopShow(text) {
  if (!els.vcxPop) return;
  if (els.vcxPopBody) els.vcxPopBody.textContent = text || '';
  els.vcxPop.hidden = false;
}

function fwdClearMarks() {
  G.fwd.gen++;                             
  if (G.fwd.timer) { clearTimeout(G.fwd.timer); G.fwd.timer = 0; }
  fwdHoverCancel();
  G.fwd.busy = false; G.fwd.line = []; G.fwd.idx = -1; G.fwd.hover = -1;
  G.fwd.committed = -1; G.fwd.hold = false; G.fwd.sel = -1; G.fwd.kind = '';
  G.fwd.vcxAt = -1; G.fwd.vcxMarks = [];                     
  fwdFindUi(); fwdOkUI();
  renderFwdSeq(); paint();
}

function clearAllMarks() {
  fwdClearMarks();                          
  anaReset();                               
  G.ana.defCells = null;                    
  G.ana.autoDefend = false;                 
  try { tellHost({ type: 'balance', on: false }); } catch (e) {}
  paint();
  toast(T('markCleared'));
}

function fwdAtkUi() {
  if (els.fwdLgB) els.fwdLgB.classList.toggle('sel', G.fwd.atk === 1);
  if (els.fwdLgW) els.fwdLgW.classList.toggle('sel', G.fwd.atk === 2);
}

function fwdToggle() {
  if (G.fwd.on) {
    if (G.fwd.hold) { G.fwd.hold = false; G.fwd.committed = -1; G.fwd.sel = -1; fwdRun(); return; }
    fwdOff(); return;
  }
  if (G.over) { toast(T('over')); return; }
  if (RV_MODE || G.review) return;
  
  G.fwd.wantVcx = false; G.fwd.want = ''; G.fwd.want2 = '';
  G.fwd.on = true;
  if (els.btn_fwd) els.btn_fwd.classList.add('on');   
  fwdQuietAi();                          
  fwdRun();
}

function fwdQuietAi() {
  
  if (linkSessionOn()) return;
  if (!S.aiB && !S.aiW) { if (G.busy) { G.fwd.hadBusy = true; S.paused = true; save(); refreshUI(); } return; }
  G.fwd.prevAi = { b: !!S.aiB, w: !!S.aiW };
  setAiSides(false, false);
  if (els.ai_side_b) els.ai_side_b.classList.toggle('on', false);
  if (els.ai_side_w) els.ai_side_w.classList.toggle('on', false);
  if (G.busy) { G.fwd.hadBusy = true; S.paused = true; }
  save(); refreshUI();
  toast(T('fwdAiOff'));
}

function evalQuietAi() {
  
  if (!linkSessionOn() && (S.aiB || S.aiW)) {
    setAiSides(false, false);
    if (els.ai_side_b) els.ai_side_b.classList.toggle('on', false);
    if (els.ai_side_w) els.ai_side_w.classList.toggle('on', false);
    save(); refreshUI();
    toast(T('fwdAiOff'));
  }
  if (G.busy) gAiGen++;                       
}

function aiStandDown() {
  evalQuietAi();
  try { LocalAI.stopLane('main'); } catch (e) {}
  try { LocalAI.stopLane('sub'); } catch (e) {}
  try { LocalAI.stopLane('fwd'); } catch (e) {}
  if (G.busy) { gAiGen++; G.busy = false; G.think = []; }
  try { refreshUI(); } catch (e) {}
}

function coachPauseByAiVsAi() {
  if (!aiVsAi()) return false;
  clearCoach();                              
  return true;
}

var SIDE_FLIP_N = 3, SIDE_FLIP_WIN_MS = 1600, SIDE_SETTLE_MS = 550;
var sideFlips = [];   
function noteDisrupt() {
  var now = Date.now();
  sideFlips.push(now);
  while (sideFlips.length && now - sideFlips[0] > SIDE_FLIP_WIN_MS) sideFlips.shift();
  if (G.sideSettle) { sideSettleArm(); return true; }   
  if (sideFlips.length < SIDE_FLIP_N) return false;     
  sideFlips.length = 0;                                 
  engineHardReconnect();
  return true;
}
function noteSideSwitch() { return noteDisrupt(); }

function noteRestartSpam() { return noteDisrupt(); }

function engineHardReconnect() {
  G.sideSettleWasBusy = !!G.busy;
  G.sideSettle = true;             
  gAiGen++;                        
  G.think = [];
  clearHeat(); clearCoach();
  LocalAI.restart();               
  setStat(null, T('offline'));     
  refreshUI();
  sideSettleArm();
}
function sideSettleArm() {
  if (G.sideSettleT) clearTimeout(G.sideSettleT);
  G.sideSettleT = setTimeout(function () {
    G.sideSettleT = 0;
    G.sideSettle = false;
    if (G.sideSettleWasBusy) G.busy = false;   
    LocalAI.boot();                      
    refreshUI();
    if (RV_MODE || G.review || G.over || openActive()) return;
    
    if (linkSessionOn()) return;
    if (S.mode === 'pve' && !S.paused && isAiColor(curColor())) {
      G.reconBoost = true;               
      aiMove();
    } else if (S.coach && S.mode !== 'place' && S.mode !== 'endgame') {
      refreshCoach().then(paint);
    }
  }, SIDE_SETTLE_MS);
}

function fwdRestoreAi() {
  if (G.fwd.hadBusy) { G.fwd.hadBusy = false; S.paused = false; }
  var p = G.fwd.prevAi; G.fwd.prevAi = null;
  if ((p && (p.b || p.w)) && !S.aiB && !S.aiW) {    
    setAiSides(p.b, p.w);
    if (els.ai_side_b) els.ai_side_b.classList.toggle('on', !!p.b);
    if (els.ai_side_w) els.ai_side_w.classList.toggle('on', !!p.w);
    refreshUI();
  }
}
function fwdOff() {
  G.fwd.gen++;
  if (G.fwd.timer) { clearTimeout(G.fwd.timer); G.fwd.timer = 0; }
  fwdHoverCancel();
  G.fwd.on = false; G.fwd.line = []; G.fwd.idx = -1; G.fwd.hover = -1; G.fwd.busy = false;
  G.fwd.committed = -1; G.fwd.hold = false; G.fwd.sel = -1; G.fwd.kind = '';
  
  G.fwd.wantVcx = false; G.fwd.want = ''; G.fwd.want2 = '';
  G.fwd.vcxAt = -1; G.fwd.vcxMarks = [];
  fwdFindUi();                           
  if (els.btn_fwd) els.btn_fwd.classList.remove('on');
  fwdRestoreAi();                        
  fwdOkUI();
  renderFwdSeq(); paint();
}

function fwdQueue() {
  if (!G.fwd.on || G.fwd.hold) return;
  if (G.fwd.timer) clearTimeout(G.fwd.timer);
  G.fwd.timer = setTimeout(function () {
    G.fwd.timer = 0;
    if (G.fwd.on && !G.fwd.hold && !G.over) fwdRun();
  }, 300);
}

async function fwdRun() {
  if (RV_MODE || G.review || G.over) return;
  var gen = ++G.fwd.gen;
  if (G.fwd.timer) { clearTimeout(G.fwd.timer); G.fwd.timer = 0; }
  fwdHoverCancel();
  
  var wantVcx = !!G.fwd.wantVcx;
  var wantA = wantVcx ? (G.fwd.want || 'VCF') : '';
  var wantB = wantVcx ? (G.fwd.want2 || (wantA === 'VCF' ? 'VCT' : 'VCF')) : '';
  G.fwd.line = []; G.fwd.idx = -1; G.fwd.hover = -1; G.fwd.busy = true;
  G.fwd.committed = -1; G.fwd.hold = false; G.fwd.sel = -1;
  G.fwd.vcxAt = -1; G.fwd.vcxMarks = [];
  G.fwd.kind = wantVcx ? wantA : '';       
  fwdFindUi();
  fwdOkUI();
  var baseMoves = G.moves.length;
  G.fwd.base = baseMoves;                    
  var b = G.board.map(function (row) { return row.slice(); });   
  function fwdCount() {
    var t = 0;
    for (var ti = 0; ti < N; ti++) for (var tj = 0; tj < N; tj++) if (b[ti][tj]) t++;
    return t;
  }
  function fwdSide() { return (fwdCount() % 2 === 0) ? 1 : 2; }   
  
  var baseStones = fwdCount();          
  var maxSteps = N * N - baseStones;    
  var atkFix = G.fwd.atk || fwdSide();
  
  var thinkMs = Math.max(300, S.turnMs || 2000);
  var msg = els.fwdMsg;
  function vcxHitText(kind, n, atkC) {
    return T(kind === 'VCF' ? 'fwdVcfHit' : 'fwdVctHit')
      .replace('{n}', String(n)).replace('{c}', T(atkC === 1 ? 'fwdBlack' : 'fwdWhite'));
  }
  
  if (!wantVcx || fwdSide() === atkFix) {
    if (msg) msg.textContent = wantVcx
      ? T('fwdProbing').replace('{r}', wantA)
      : T('fwdFindRun').replace('{k}', 'VCF/VCT');
    var r1 = await fwdTryVcx(b, wantVcx ? wantA : 'auto', wantVcx ? VCX_WORKER_DEEP : VCX_WORKER_TIMEOUT);
    if (gen !== G.fwd.gen) return;
    if (!r1 && wantVcx) { r1 = await fwdTryVcx(b, wantB, VCX_WORKER_DEEP); if (gen !== G.fwd.gen) return; }
    if (r1) {
      fwdAdoptVcx(b, r1, 0);                 
      G.fwd.busy = false;
      if (msg) msg.textContent = vcxHitText(r1.kind, r1.line.length, G.fwd.atk || atkFix);
      fwdFindUi(); renderFwdSeq(); paint();
      return;
    }
  }
  G.fwd.kind = wantVcx ? wantA : '';
  fwdFindUi();
  
  function fwdReportFive() {
    G.fwd.busy = false;
    
    if (msg) msg.textContent = wantVcx
      ? T('fwdFiveNoVcx').replace('{n}', String(G.fwd.line.length))
        .replace('{c}', T(atkFix === 1 ? 'fwdBlack' : 'fwdWhite'))
      : T('fwdDone').replace('{n}', String(G.fwd.line.length));
    renderFwdSeq(); paint();
  }
  for (var step = 0; step < maxSteps; step++) {   
    if (gen !== G.fwd.gen) return;
    if (G.moves.length !== baseMoves) {                          
      
      if (!G.fwd.hold) {
        G.fwd.line = []; G.fwd.idx = -1; G.fwd.hover = -1;
        renderFwdSeq();
      }
      G.fwd.busy = false;
      return;
    }
    
    if (findWinLine(b, engineRule())) { fwdReportFive(); return; }   
    
    var total = 0;
    for (var i = 0; i < N; i++) for (var j = 0; j < N; j++) if (b[i][j]) total++;
    var cc = (total % 2 === 0) ? 1 : 2;
    
    var op = [], of = [], myFour = 0, quick = null;
    quick = winPoints(b, cc, engineRule());
    if (!quick.length) {
      op = winPoints(b, 3 - cc, engineRule());
      if (!op.length) {
        of = openFourPoints(b, 3 - cc, engineRule());
        if (of.length) myFour = fourPoints(b, cc, engineRule()).length;
      }
      
    }
    
    if (wantVcx && step > 0 && cc === atkFix) {
      var rp = await fwdTryVcx(b, wantA, VCX_STEP_TIMEOUT);
      if (gen !== G.fwd.gen) return;
      if (!rp) { rp = await fwdTryVcx(b, wantB, VCX_STEP_TIMEOUT); if (gen !== G.fwd.gen) return; }
      if (rp) {
        var from = G.fwd.line.length;
        fwdAdoptVcx(b, rp, from);            
        G.fwd.busy = false;
        if (msg) msg.textContent = T('fwdVcxLater')
          .replace('{k}', String(from)).replace('{r}', rp.kind).replace('{n}', String(rp.line.length));
        fwdFindUi(); renderFwdSeq(); paint();
        return;
      }
    }
    if (step === 0 && msg) {
      
      var deepMs = Math.max(4000, Math.min(60000, (S.turnMs || 400) * 4));
      var loS = Math.round((deepMs + thinkMs * 8) / 1000);
      var hiS = Math.round((deepMs + thinkMs * 40) / 1000);
      msg.textContent = T('fwdEta').replace('{d}', String(Math.ceil(deepMs / 1000)))
                                  .replace('{a}', String(loS)).replace('{b}', String(hiS));
    } else if (msg) {
      msg.textContent = T('fwdRunning').replace('{k}', String(step + 1));
    }
    var r = null, c = null;
    if (!quick.length) {                                          
      
      var firstDeep = (step === 0);
      
      var tms = firstDeep ? Math.max(4000, Math.min(60000, (S.turnMs || 400) * 4)) : thinkMs;
      try {
        r = await LocalAI.analyze({ board: b, moveList: [], matchMs: 600000,
          turnMs: tms, timeUsedMs: 0,
          topN: firstDeep ? Math.max(2, S.nbest || 3) : (of.length ? 4 : 1),
          rule: engineRule(), cid: 'fwd-' + gen + '-' + step, lane: 'fwd', side: cc,
          
          exclusive: true,
          liveId: LocalAI.nextLiveId(), tag: T('tagFwd') }, tms + 9000);
      } catch (e) { r = null; }
      if (gen !== G.fwd.gen) return;
      var c0 = r && r.candidates && r.candidates[0];
      if (c0 && !(c0.y < 0 || c0.y >= N || c0.x < 0 || c0.x >= N || b[c0.y][c0.x])) c = c0;
    }
    
    var cands = (r && r.candidates) ? r.candidates.filter(function (q) {
      return q && !(q.y < 0 || q.y >= N || q.x < 0 || q.x >= N || b[q.y][q.x]);
    }) : [];
    var pick = quick.length ? { x: quick[0].x, y: quick[0].y, why: 'win' }
              : mustAnswer(b, cc, cands, op, of, myFour);
    if (!pick && !c) break;                                       
    if (!pick) pick = { x: c.x, y: c.y, why: 'engine' };
    b[pick.y][pick.x] = cc;
    G.fwd.line.push({ x: pick.x, y: pick.y, c: cc });
    renderFwdSeq(); paint();                                     
    
    if (findWinLine(b, engineRule())) { fwdReportFive(); return; }   
    
  }
  G.fwd.busy = false;
  
  var boardFull = (G.fwd.line.length >= maxSteps);
  if (msg) msg.textContent = T(boardFull ? 'fwdFull' : 'fwdCap').replace('{n}', String(G.fwd.line.length));
  
  if (wantVcx) {
    if (!RV_MODE && !G.review && !G.ana.busy && G.moves.length > 0) anaCalc();
    vcxPopShow(T(wantA === 'VCF' ? 'fwdVcfNone' : 'fwdVctNone')
      .replace('{c}', T(atkFix === 1 ? 'fwdBlack' : 'fwdWhite')));
  }
  renderFwdSeq(); paint();
}

function fwdShowIdx() {
  if (G.fwd.hover >= 0) return G.fwd.hover;
  if (G.fwd.idx >= 0) return G.fwd.idx;
  if (G.fwd.committed >= 0) return G.fwd.committed;
  return G.fwd.line.length - 1;
}

var FWD_HOLD_MS = 140;
var fwdPaintPend = false;
function fwdPaintSoon() {
  if (fwdPaintPend) return;
  fwdPaintPend = true;
  (window.requestAnimationFrame || function (f) { setTimeout(f, 16); })(function () {
    fwdPaintPend = false; paint();
  });
}
function fwdHover(i) {
  if (G.fwd.hoverTimer) { clearTimeout(G.fwd.hoverTimer); G.fwd.hoverTimer = 0; }
  if (G.fwd.hover === i) return;
  G.fwd.hover = i;
  fwdPaintSoon();
}
function fwdHoverRelease() {
  if (G.fwd.hoverTimer) clearTimeout(G.fwd.hoverTimer);
  G.fwd.hoverTimer = setTimeout(function () {
    G.fwd.hoverTimer = 0;
    if (G.fwd.hover >= 0) { G.fwd.hover = -1; fwdPaintSoon(); }
  }, FWD_HOLD_MS);
}
function fwdHoverCancel() {
  if (G.fwd.hoverTimer) { clearTimeout(G.fwd.hoverTimer); G.fwd.hoverTimer = 0; }
  G.fwd.hover = -1;
}

function fwdOkUI() {
  if (!els.btn_fwd_ok) return;
  els.btn_fwd_ok.classList.toggle('on', G.fwd.sel >= 0 && G.fwd.sel < (G.fwd.line || []).length);
}

function renderFwdSeq() {
  var seq = els.fwdSeq;
  if (!seq) return;
  var line = G.fwd.line || [];
  seq.innerHTML = '';
  var show = fwdShowIdx();
  line.forEach(function (m, i) {
    var s = document.createElement('button');
    s.type = 'button';
    s.className = 'fwc ' + (m.c === 1 ? 'b' : 'w')
      + (i === show ? ' cur' : '')
      + (i === G.fwd.sel ? ' sel' : '')
      + (G.fwd.committed >= 0 && i > G.fwd.committed ? ' dim' : '');
    s.textContent = coordName(m.x, m.y);
    s.title = (m.c === 1 ? T('fwdBlack') : T('fwdWhite')) + ' · ' + (i + 1);
    s.onmouseenter = function () { fwdHover(i); };
    s.onmouseleave = function () { fwdHoverRelease(); };
    s.onclick = function () { fwdSelect(i); };
    seq.appendChild(s);
  });
  if (!line.length && !G.fwd.busy && els.fwdMsg) els.fwdMsg.textContent = T('fwdIdle');
}

function fwdSelect(i) {
  if (!G.fwd.on || !G.fwd.line.length) return;
  if (i < 0 || i >= G.fwd.line.length) return;
  if (G.fwd.committed >= 0 && i <= G.fwd.committed) {
    
    G.fwd.sel = -1; G.fwd.idx = i; fwdOkUI(); renderFwdSeq(); paint(); return;
  }
  G.fwd.sel = i; G.fwd.idx = i;
  fwdOkUI(); renderFwdSeq(); paint();
}
function fwdStep(d) {
  if (!G.fwd.on || !G.fwd.line.length) return;
  var len = G.fwd.line.length;
  var cur = (G.fwd.idx >= 0) ? G.fwd.idx : len - 1;
  G.fwd.idx = Math.max(-1, Math.min(len - 1, cur + d));
  fwdHoverCancel();
  renderFwdSeq(); paint();
}

function fwdCommit() {
  var i = G.fwd.sel;
  if (i < 0 || i >= (G.fwd.line || []).length) return;
  var line = G.fwd.line.slice(0, i + 1);
  var from = G.fwd.committed + 1;                 
  G.fwd.committed = i; G.fwd.sel = -1; G.fwd.idx = i;
  G.fwd.hold = true;                              
  fwdHoverCancel();
  fwdOkUI(); renderFwdSeq();
  for (var k = from; k < line.length; k++) {
    if (!place(line[k].x, line[k].y)) break;      
  }
  if (!G.fwd.on) { paint(); return; }
  afterMove();                                    
  paint();
  
}

function anaStop() {
  G.ana.gen++;
  anaLiveStop();
  if (G.ana.defTimer) { clearTimeout(G.ana.defTimer); G.ana.defTimer = null; }
  G.ana.defWinUntil = 0; G.ana.defRefining = false;
  G.ana.busy = false; G.ana.kind = '';
  
  if (G.ana.deferTmr) { clearTimeout(G.ana.deferTmr); G.ana.deferTmr = null; }
  anaMark(null);
  renderAna(T('anStopped'));
  paint();                              
}

async function anaNbest() {
  if (!anaGuard(0)) return;
  aiStandDown();                              
  var N = anaIp();
  ++G.ana.gen;
  G.ana.busy = true; G.ana.kind = 'nbest';
  G.ana.marks = []; G.ana.rows = [];
  setPvLine([]);
  anaMark(els.btn_an_nbest);
  renderAna(T('anNbestRun').replace('{d}', '0'));
  anaLiveRun('nbest', N, 'tagNbest');
}

var BAL_OPEN_TJ = [
  [[7, 7], [8, 8], [9, 9]],
  [[7, 7], [8, 8], [9, 8]],
  [[7, 7], [8, 8], [9, 7]],
  [[7, 7], [8, 8], [9, 6]],
  [[7, 7], [8, 8], [9, 5]],
  [[7, 7], [8, 8], [8, 7]],
  [[7, 7], [8, 8], [8, 6]],
  [[7, 7], [8, 8], [8, 5]],
  [[7, 7], [8, 8], [7, 6]],
  [[7, 7], [8, 8], [7, 5]],
  [[7, 7], [8, 8], [6, 6]],
  [[7, 7], [8, 8], [6, 5]],
  [[7, 7], [8, 8], [5, 5]],
  [[7, 7], [7, 8], [7, 9]],
  [[7, 7], [7, 8], [9, 9]],
  [[7, 7], [7, 8], [8, 8]],
  [[7, 7], [7, 8], [9, 8]],
  [[7, 7], [7, 8], [9, 7]],
  [[7, 7], [7, 8], [7, 6]],
  [[7, 7], [7, 8], [9, 6]],
  [[7, 7], [7, 8], [7, 5]],
  [[7, 7], [7, 8], [8, 5]],
  [[7, 7], [7, 8], [8, 9]],
  [[7, 7], [7, 8], [8, 7]],
  [[7, 7], [7, 8], [8, 6]],
  [[7, 7], [7, 8], [9, 5]],
];
function balClosestToZero(cands, nearC, nearR) {
  if (!cands || !cands.length) return null;
  var scope = cands;
  if (nearR > 0 && nearC >= 0) {
    var near = cands.filter(function (c) {
      return Math.max(Math.abs(c.x - nearC), Math.abs(c.y - nearC)) <= nearR;
    });
    if (near.length) scope = near;
  }
  var best = null, bv = Infinity;
  scope.forEach(function (c) {
    var v = evalNum(c.eval);
    if (!isNaN(v) && Math.abs(v) < bv) { bv = Math.abs(v); best = c; }
  });
  return best || scope[0];
}

// ─────────────────────────────────────────────────────────────────────────────
function balLegalAt(board, x, y, side) {
  if (!board[y] || board[y][x]) return false;
  if (side === 1 && renjuRule() && vcxForbidden(board, x, y)) return false;
  return true;
}

var _balCidSeq = 0;
async function balBalancedPick(board, side, rule, turnMs, gen, lane) {
  var qMs = Math.max(600, Math.min(1500, Math.round((turnMs || 2000) * 0.6)));
  var cid = 'calc-openbal-' + side + '-' + (++_balCidSeq);
  var scanNative = async function (n) {
    var j = await apiBalance({
      board: board, moveList: [], n: n, mode: 'two', rule: rule,
      turnMs: qMs, lane: lane || 'sub', cid: cid, side: side,
    }, n * qMs + 25000);
    if (gen !== G.ana.gen) return null;
    if (!j || j.swap) return null;
    return j.moves || [];
  };
  var rounds = [1, 2];
  for (var ri = 0; ri < rounds.length; ri++) {
    var mv = null;
    try { mv = await scanNative(rounds[ri]); } catch (e) { mv = null; }
    if (gen !== G.ana.gen) return null;
    if (mv && mv.length) {
      for (var k = 0; k < mv.length; k++) {
        if (balLegalAt(board, mv[k][0], mv[k][1], side)) return { x: mv[k][0], y: mv[k][1] };
      }
    }
  }
  if (gen !== G.ana.gen) return null;
  return null;
}

async function openBalChain(rule, turnMs, gen) {
  var c = (N - 1) >> 1;
  var board = G.board.map(function (r) { return r.slice(); });
  var out = [], stones = 0;
  for (var y = 0; y < N; y++) for (var x = 0; x < N; x++) if (board[y][x]) stones++;
  if (!stones) { board[c][c] = 1; stones = 1; out.push({ x: c, y: c, color: 'b', seq: '1' }); }
  if (out.length !== 1) return out;
  var qMs = Math.max(600, Math.min(2000, Math.round((turnMs || 2000) * 0.8)));
  G.ana.balEval = null; G.ana.balMode = null;
  var j2 = null;
  try {
    j2 = await apiBalance({
      board: board, moveList: [], n: 1, mode: 'two', rule: rule,
      turnMs: qMs, lane: 'main', cid: 'calc-openbal2-' + (++_balCidSeq),
      side: 2,
    }, qMs + 25000);
  } catch (e2) { j2 = null; }
  if (gen !== G.ana.gen) return out;
  var pair = (j2 && !j2.swap && j2.moves) ? j2.moves.slice(0, 2) : [];
  if (pair.length === 2 && balLegalAt(board, pair[0][0], pair[0][1], 2)) {
    board[pair[0][1]][pair[0][0]] = 2;
    if (balLegalAt(board, pair[1][0], pair[1][1], 1)) {
      out.push({ x: pair[0][0], y: pair[0][1], color: 'w', seq: '2' });
      out.push({ x: pair[1][0], y: pair[1][1], color: 'b', seq: '3' });
      G.ana.balEval = (j2 && j2.eval != null) ? j2.eval : null;
      G.ana.balMode = 'two';
      return out;
    }
    board[pair[0][1]][pair[0][0]] = 0;
  }
  var sides = [2, 1], lanes = ['sub', 'sub'];
  for (var i = 0; i < sides.length && out.length < 3; i++) {
    var p = null;
    try { p = await balBalancedPick(board, sides[i], rule, turnMs, gen, lanes[i]); }
    catch (e) { p = null; }
    if (gen !== G.ana.gen) return out;
    if (!p) break;
    board[p.y][p.x] = sides[i];
    out.push({ x: p.x, y: p.y, color: (sides[i] === 1) ? 'b' : 'w', seq: String(out.length + 1) });
  }
  G.ana.balMode = 'one';
  return out;
}

function balDoneMsg(n, names, ev) {
  var base = T('anBalDone').replace('{n}', String(n)).replace('{list}', names.join(', '));
  if (ev == null || isNaN(+ev)) return base;
  return base + T('anBalEval').replace('{v}', String(Math.abs(Math.round(+ev))));
}

function balOpenHintTxt() {
  if (G.moves.length > 3) return '';
  var pool = T('anBalOpenHints');
  if (!pool || !pool.length || typeof pool === 'string') return '';
  var left = pool.slice();
  var n = Math.min(left.length, 1 + Math.floor(Math.random() * 2));
  var out = [];
  for (var i = 0; i < n && left.length; i++) {
    out.push(left.splice(Math.floor(Math.random() * left.length), 1)[0]);
  }
  return '\n　※ ' + out.join('\n　※ ');
}

async function anaBalPoints() {
  if (!anaGuard(0)) return;
  aiStandDown();
  var gen = ++G.ana.gen;
  G.ana.busy = true; G.ana.kind = 'bal';
  anaMark(els.btn_an_bal);
  renderAna(T('anRunning'));
  var turnMs = Math.max(2000, S.turnMs || 2000);
  var empty = !G.moves.length;
  var need = empty ? 3 : Math.max(1, Math.min(6, balPN()));
  try {
    if (!empty) {
      var bmB = G.board.map(function (r) { return r.slice(); });
      var plyB = G.moves.length;
      var marksArr = [], namesB = [], evB = null;
      var qMs = Math.max(600, Math.min(1800, turnMs));
      while (marksArr.length < need) {
        var modeB = 'two';
        var qi = marksArr.length;
        var curB = ((plyB + qi) % 2 === 0);
        var jQ = null;
        try {
          jQ = await apiBalance({
            board: bmB, moveList: [], n: 1, mode: modeB, rule: engineRule(),
            turnMs: qMs, lane: 'main', cid: 'calc-balpts-' + G.moves.length + '-' + qi,
            side: curB ? 1 : 2,
          }, qMs + 20000);
        } catch (eQ) { jQ = null; }
        if (gen !== G.ana.gen) return;
        if (jQ && jQ.swap) { renderAna(T('balSwapNow')); paint(); return; }
        if (jQ && jQ.eval != null) evB = jQ.eval;
        var ptsQ = (jQ && jQ.moves) || [];
        if (!ptsQ.length) break;
        var addedQ = 0;
        for (var pk = 0; pk < ptsQ.length && marksArr.length < need; pk++) {
          if (bmB[ptsQ[pk][1]][ptsQ[pk][0]]) continue;
          var sideQ = ((plyB + marksArr.length) % 2 === 0) ? 1 : 2;
          bmB[ptsQ[pk][1]][ptsQ[pk][0]] = sideQ;
          marksArr.push({ x: ptsQ[pk][0], y: ptsQ[pk][1],
                          color: sideQ === 1 ? 'b' : 'w', seq: String(marksArr.length + 1) });
          namesB.push(coordName(ptsQ[pk][0], ptsQ[pk][1]));
          addedQ++;
        }
        if (!addedQ) break;
      }
      if (marksArr.length) {
        renderBalMarks(marksArr, balDoneMsg(marksArr.length, namesB, evB) + balOpenHintTxt());
        return;
      }
      if (typeof window.GB_BAL_POINTS === 'function') {
        marksArr = []; namesB = [];
        try {
          await window.GB_BAL_POINTS({
            board: G.board, moves: G.moves.length, need: need, rule: engineRule(),
            turnMs: Math.max(2000, S.turnMs || 2000),
            onMarks: function (ms) { marksArr = ms || []; },
          });
        } catch (eB) { marksArr = []; }
        if (gen !== G.ana.gen) return;
        if (marksArr && marksArr.length) {
          marksArr.forEach(function (m) { namesB.push(coordName(m.x, m.y)); });
          renderBalMarks(marksArr, T('anBalDone').replace('{n}', String(marksArr.length)).replace('{list}', namesB.join(', ')));
          return;
        }
      }
      var retry = null;
      try {
        retry = await apiBalance({
          board: bmB, moveList: [], n: 1, mode: 'two', rule: engineRule(),
          turnMs: Math.max(1200, qMs), lane: 'sub',
          cid: 'calc-balpts-retry-' + G.moves.length, side: ((plyB % 2 === 0) ? 1 : 2),
        }, Math.max(1200, qMs) + 20000);
      } catch (eR) { retry = null; }
      if (gen !== G.ana.gen) return;
      if (retry && retry.swap) { renderAna(T('balSwapNow')); paint(); return; }
      if (retry && retry.moves && retry.moves.length) {
        var addR = 0;
        for (var rk = 0; rk < retry.moves.length && marksArr.length < need; rk++) {
          if (bmB[retry.moves[rk][1]][retry.moves[rk][0]]) continue;
          var sideR = ((plyB + marksArr.length) % 2 === 0) ? 1 : 2;
          bmB[retry.moves[rk][1]][retry.moves[rk][0]] = sideR;
          marksArr.push({ x: retry.moves[rk][0], y: retry.moves[rk][1],
                          color: sideR === 1 ? 'b' : 'w', seq: String(marksArr.length + 1) });
          namesB.push(coordName(retry.moves[rk][0], retry.moves[rk][1]));
          addR++;
        }
        if (retry.eval != null) evB = retry.eval;
        if (addR && marksArr.length) {
          renderBalMarks(marksArr, balDoneMsg(marksArr.length, namesB, evB));
          return;
        }
      }
      renderAna(T('anBalNoEngine'));
      return;
    }
    if (empty) {
      var ruleE = engineRule();
      var marksE = await openBalChain(ruleE, Math.max(2000, S.turnMs || 2000), gen);
      if (gen !== G.ana.gen) return;
      if (marksE.length < 3) {
        var cyB = (N - 1) >> 1;
        marksE = [{ x: cyB, y: cyB, color: 'b', seq: '1' }];
        var fbk = BAL_OPEN_TJ[Math.floor(Math.random() * BAL_OPEN_TJ.length)];
        marksE.push({ x: fbk[1][0], y: fbk[1][1], color: 'w', seq: '2' });
        marksE.push({ x: fbk[2][0], y: fbk[2][1], color: 'b', seq: '3' });
        G.ana.balEval = null;
      }
      var namesE = marksE.map(function (m) { return coordName(m.x, m.y); });
      renderBalMarks(marksE, balDoneMsg(marksE.length, namesE, G.ana.balEval) + balOpenHintTxt());
      return;
    }
    renderAna(T('anBalNoEngine'));
  } catch (e) {
    if (gen === G.ana.gen) renderAna(T('offline'));
  } finally {
    if (gen === G.ana.gen) { G.ana.busy = false; anaMark(null); }
  }
}

function renderBalMarks(arr, msg) {
  G.ana.balLeft = arr.length;
  G.ana.balBase = G.moves.length;   // these marks belong to THIS board state
  var WHITE_DISC = 'rgba(70,190,255,.95)';
  G.ana.marks = arr.map(function (m, i) {
    // Sequence number only - no eval score on the balance marks
    return { x: m.x, y: m.y, badge: m.seq, label: '', tier: -1, inline: true,
             col: m.color === 'b' ? null : WHITE_DISC };
  });
  G.ana.rows = arr.map(function (m, i) {
    return { i: i, badge: m.seq, coord: coordName(m.x, m.y), eval: '', pv: '', tier: -1,
             col: m.color === 'b' ? null : WHITE_DISC };
  });
  renderAna(msg + T('anBalColor'));
  paint();
  if (linkSessionOn() && arr.length) {
    var gm2 = null;
    try { var bb = els.board.getBoundingClientRect(); gm2 = { gap: Math.round(bb.width / 14) }; } catch (e) { gm2 = { gap: 40 }; }
    var cellsP = arr.map(function (m) { return m.x + ',' + m.y + ',' + m.seq + ',' + m.color + ',' + (m.ev || ''); });
    tellHost({ type: 'balance', on: true, gap: gm2.gap, cells: cellsP.join(';') });
  }
}

function balPN() {
  if (els.btn_an_baln && els.btn_an_baln.value) {
    var v = parseInt(els.btn_an_baln.value, 10);
    if (v >= 1 && v <= 6) return v;
  }
  var n = 2;
  try { var sv = parseInt(localStorage.getItem('gb_bal_point_n') || '2', 10); if (sv >= 1 && sv <= 6) n = sv; } catch (e) {}
  return n;
}

function wireAnalysis() {
  wireCurveHover();
  if (!els.btn_an_nbest) return;
  
  els.btn_an_stop.onclick = function () { if (G.ana.busy) anaStop(); else anaCalc(); };
  
  els.btn_an_nbest.onclick = function () { if (G.ana.busy && G.ana.kind === 'nbest') { anaStop(); return; } anaNbest(); };
  
  els.btn_fwd.onclick = function () { fwdToggle(); };
  
  if (els.btn_fwd_vcf) els.btn_fwd_vcf.onclick = function () { fwdFind('VCF'); };
  if (els.btn_fwd_vct) els.btn_fwd_vct.onclick = function () { fwdFind('VCT'); };
  els.btn_fwd_ok.onclick = function () { fwdCommit(); };
  els.btn_fwd_prev.onclick = function () { fwdStep(-1); };
  els.btn_fwd_next.onclick = function () { fwdStep(1); };
  
  if (els.btn_fwd_clear) els.btn_fwd_clear.onclick = function () { clearAllMarks(); };
  if (els.vcxPop_close) els.vcxPop_close.onclick = function () { if (els.vcxPop) els.vcxPop.hidden = true; };
  
  window.addEventListener('keydown', function (e) {
    if (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight') return;
    if (!G.fwd || !G.fwd.on || !G.fwd.line.length) return;
    if (RV_MODE || G.review) return;
    var t = e.target;
    if (t && t.tagName && /^(INPUT|SELECT|TEXTAREA)$/.test(t.tagName)) return;
    fwdStep(e.key === 'ArrowLeft' ? -1 : 1);
    e.preventDefault();
  });
  els.btn_an_defend.onclick = function () {
    if (G.ana.busy && G.ana.kind === 'defend') { anaDefendStop(); return; }
    aiStandDown(); anaDefend(true);
  };
  els.btn_an_bal.onclick = function () { anaBalPoints(); };
  if (els.btn_an_baln) els.btn_an_baln.onchange = function () {
    var n = parseInt(els.btn_an_baln.value, 10);
    if (isNaN(n) || n < 1 || n > 6) n = 2;
    try { localStorage.setItem('gb_bal_point_n', String(n)); } catch (e) {}
    toast(T('anBalnSet').replace('{n}', String(n)));
  };
  els.num_nbest.onchange = function () {
    S.nbest = Math.max(1, Math.min(8, Math.round(+els.num_nbest.value || 3)));
    els.num_nbest.value = String(S.nbest);
    if (els.sel_multiPv) els.sel_multiPv.value = String(S.nbest);
    save();
  };
  els.num_nbest.value = String(anaIp());
  renderAna(T('anIdle'), true);
}

function gbOf(mb) { mb = mb | 0; return mb >= 1024 ? (mb / 1024).toFixed(1) + ' GB' : mb + ' MB'; }

function kbStr(kb) {
  kb = kb | 0;
  if (kb <= 0) return '—';
  return kb >= 1024 ? (kb / 1024).toFixed(0) + 'MB' : kb + 'KB';
}

function sysInfoCards() {
  var s = G.sysInfo;
  if (!s) return null;
  var ca = s.cache || {};
  var isa = s.isa || {};
  
  var cpuSub = T('abSysCores').replace('{p}', s.physCores | 0).replace('{l}', s.logiCores | 0);
  if (s.mhz) cpuSub += ' · ' + (s.mhz / 1000).toFixed(2) + 'GHz';
  if (ca.l1 || ca.l2 || ca.l3) {
    cpuSub += ' · ' + T('abSysCache')
      .replace('{a}', kbStr(ca.l1)).replace('{b}', kbStr(ca.l2)).replace('{c}', kbStr(ca.l3));
  }
  
  var isaMain = ['AVX2 ' + (isa.avx2 ? '✓' : '✗'),
                 'AVX-512 ' + (isa.avx512 ? '✓' : '✗'),
                 'VNNI ' + ((isa.avxvnni || isa.avx512vnni) ? '✓' : '✗')].join(' · ');
  var isaSub = T('abSysEngine') + ' ' +
    (s.native ? (s.variant || T('sysNative')) + '（' + T('sysNative') +
                (s.variantBench && s.variantBench === s.variant ? ' · ' + T('sysBench') : '') + '）'
              : T('sysWasm'));
  
  var memSub = T('abSysUsed').replace('{u}', s.memLoad | 0).replace('{a}', gbOf(s.memAvailMB));
  
  var osLine = (s.osName || '') + (s.osBuild ? ' (build ' + s.osBuild + ')' : '');
  if (s.diskFreeGB) osLine += ' · ' + T('sysDisk') + ' ' + s.diskFreeGB + 'GB';
  return {
    cpu: s.cpuName || T('sysUnknown'), cpuSub: cpuSub,
    isa: isaMain, isaSub: isaSub,
    mem: gbOf(s.memTotalMB), memSub: memSub,
    os: osLine || T('sysUnknown'),
  };
}

function sysCardsHtml() {
  var c = sysInfoCards();
  if (!c) return '';
  function card(title, main, sub) {
    return '<div class="ab-sys-c"><div class="ab-sys-t">' + escHtml(title) +
           '</div><div class="ab-sys-v">' + escHtml(main) +
           '</div><div class="ab-sys-s">' + escHtml(sub) + '</div></div>';
  }
  return '<div class="ab-sys">' +
           card(T('abSysCpu'), c.cpu, c.cpuSub) +
           card(T('abSysIsa'), c.isa, c.isaSub) +
           card(T('abSysMem'), c.mem, c.memSub) +
         '</div>' +
         '<div class="ab-lic">' + escHtml(c.os) + '</div>';
}

function renderSysInfo() {
  var el = els.sysInfo;
  if (!el) return;
  el.innerHTML = sysCardsHtml();          
}

function renderAboutSys() {
  var cards = sysCardsHtml();
  if (!cards) return '<div class="ab-h2">' + escHtml(T('abSys')) + '</div>' +
                 '<div class="ab-lic">' + escHtml(T('abSysNone')) + '</div>';
  return '<div class="ab-h2">' + escHtml(T('abSys')) + '</div>' + cards;
}

var HASH_FLOOR_MB = 64;

function renderHashInfo() {
  var el = els.hashInfo;
  if (!el) return;
  var fullMB = (G.hashEffMB || S.hashMB) | 0;
  var h = '<div class="si-r"><span class="si-k">' + escHtml(T('hmExcl')) +
          '</span><span class="si-v">' + escHtml(T('hmExclVal').replace('{m}', fullMB)) +
          '</span></div>';
  h += '<div class="si-r"><span class="si-k">' + escHtml(T('hmFloor')) +
       '</span><span class="si-v">' + escHtml(T('hmFloorVal').replace('{m}', HASH_FLOOR_MB)) +
       '</span></div>';
  var si = G.sysInfo || {};
  var tot = si.memTotalMB | 0, av = si.memAvailMB | 0;
  
  var sumMB = fullMB + HASH_FLOOR_MB * 2;
  var line = T('hmSum') + ' ' + sumMB + 'MB';
  
  if (tot > 0) line += ' · ' + T('sysMem') + ' ' + (tot >= 1024 ? (tot / 1024).toFixed(1) + 'GB' : tot + 'MB') +
                       ' · ' + T('sysAvail') + ' ' + (av >= 1024 ? (av / 1024).toFixed(1) + 'GB' : av + 'MB') +
                       ' · ' + T('hmPct') + ' ' + (sumMB / tot * 100).toFixed(1) + '%';
  h += '<div class="si-r"><span class="si-k">' + escHtml(T('hmUse')) +
       '</span><span class="si-v">' + escHtml(line) + '</span></div>';
  el.innerHTML = h;
}

function buildSelects() {
  var host = G.host || {};
  // Threads/hash: the host-persisted value is the single source of truth. Page localStorage is
  // unreliable - Chromium commits DOMStorage lazily (~5s) while the app terminates the WebView2
  // child on exit, so an uncommitted write is lost (observed: set 5 -> restart read back 3).
  // The host writes it synchronously on engineConfig (host.cpp SaveEnginePrefs) and sends it
  // back with the handshake; prefer it here, and keep the local dedicated key as a fallback.
  var _e = enginePrefGet();
  if (_e.cores >= MIN_THREADS) S.cores = _e.cores;
  if (_e.hashMB >= 1) S.hashMB = _e.hashMB;
  if (host.userThreads >= MIN_THREADS) S.cores = host.userThreads;
  if (host.userHashMB >= 1) S.hashMB = host.userHashMB;
  if (host.userThreads >= MIN_THREADS || host.userHashMB >= 1) enginePrefSet(S.cores, S.hashMB);
  if (!(host.threadsMax >= 1)) return;
  renderSysInfo();
  var maxT = Math.max(1, host.threadsMax || 1);
  //     t=10 2879K nps / d22  ·  t=12 3567K / d29  ·  t=14~16 4.0M+ / d30
  //     10s：T4 depth 23 / 0.73M      vs   T14 depth 22 / 2.36M
  var physC = Math.max(0, host.physCores | 0);
  var lowCores = (physC > 1) ? (S.cores < physC) : (S.cores >= 10 && S.cores <= 12);
  if (lowCores && maxT > S.cores && !migFlag('threadsTopo4')) {
    var _oldCores = S.cores;
    S.cores = maxT;
    try { enginePrefSet(S.cores, S.hashMB); } catch (eTT0) {}
    try { save(); } catch (eTT) {}
    try { console.log('[threads] topo migrate ' + _oldCores + ' -> ' + maxT); } catch (eTT2) {}
  }
  
  var factoryT = (host.threadsDefault >= 1 && host.threadsDefault <= maxT) ? host.threadsDefault : maxT;
  if (factoryT < MIN_THREADS) factoryT = Math.min(maxT, MIN_THREADS);
  if (!(S.cores >= MIN_THREADS)) { S.cores = factoryT; enginePrefSet(S.cores, S.hashMB); save(); }
  var effT = Math.max(Math.min(MIN_THREADS, maxT), Math.min(S.cores, maxT));
  var html = '';
  for (var t = Math.min(MIN_THREADS, maxT); t <= maxT; t++) {
    html += '<option value="' + t + '"' + (t === effT ? ' selected' : '') + '>' + t + '</option>';
  }
  els.sel_cores.innerHTML = html;
  
  if (els.coresHint)
    els.coresHint.textContent = T('coresHint')
      .replace('{n}', String(host.cpu || 0)).replace('{m}', String(maxT));

  var native = !!host.native;
  var memMB = host.memMB | 0;
  var maxH = native ? 6144 : 2048;
  var hs = [256, 512, 768, 1024, 1536, 2048];
  if (maxH > 2048) hs = hs.concat([3072, 4096, 6144]);
  
  // RAM-aware hash cap: grey out any option the machine cannot feed.
  // cap = min(6GB hard limit, 80% of RAM, RAM minus 2GB system reserve).
  var capMB = maxH;
  if (memMB > 0) {
    var byPct = Math.floor(memMB * 0.8);
    var byRes = Math.max(0, memMB - 2048);
    capMB = Math.min(maxH, byPct, byRes);
  }
  var effH = (S.hashMB && hs.indexOf(S.hashMB) >= 0 && S.hashMB <= capMB)
    ? S.hashMB
    : (hs.filter(function (h) { return h <= capMB; }).pop() || 512);
  if (!migFlag('hash1024')) {
    if (S.hashMB === 1024) { S.hashMB = 512; enginePrefSet(S.cores, S.hashMB); save(); effH = 512; }
  }
  if (!S.hashMB) { S.hashMB = 2048; enginePrefSet(S.cores, S.hashMB); save(); effH = 2048; }
  if (!migFlag('hash2048') && S.hashMB >= 256 && S.hashMB < 2048) {
    S.hashMB = 2048;
    enginePrefSet(S.cores, S.hashMB);
    save();
    effH = Math.min(2048, capMB);
  }
  els.sel_hash.innerHTML = hs.map(function (h) {
    var off = (h > capMB);
    return '<option value="' + h + '"' + (h === effH ? ' selected' : '') +
           (off ? ' disabled' : '') + '>' + h + ' MB</option>';
  }).join('');
  G.hashEffMB = effH;

  if (S.hashMul !== undefined) { delete S.hashMul; save(); }
  renderHashInfo();

  fillVariantSelect();
  var wOpt = weightsPrefGet() || defaultWeightsMode();
  if (els.sel_weights) {
    els.sel_weights.innerHTML =
      '<option value="nnue"' + (wOpt === 'nnue' ? ' selected' : '') + '>' + T('wNnue') + '</option>' +
      '<option value="classic"' + (wOpt === 'classic' ? ' selected' : '') + '>' + T('wClassic') + '</option>';
  }

  var CR_LABELS = ['cr2', 'cr2l3', 'cr3', 'cr3l4', 'cr4', 'crFull'];
  var crNow = (S.candRange >= 0 && S.candRange <= 5) ? S.candRange : 3;
  S.candRange = crNow;
  if (els.sel_candRange) {
    els.sel_candRange.innerHTML = CR_LABELS.map(function (k, i) {
      return '<option value="' + i + '"' + (i === crNow ? ' selected' : '') + '>' + T(k) + '</option>';
    }).join('');
    els.sel_candRange.title = T('candRangeHint');
  }
  if (els.multiPvHint) els.multiPvHint.textContent = T('multiPvHint');
  if (!migFlag('multiPv3') && (S.nbest === undefined || S.nbest === 4)) { S.nbest = 3; save(); }
  var mpNow = Math.max(1, Math.min(8, Math.round(+S.nbest || 3)));
  S.nbest = mpNow;
  if (els.sel_multiPv) {
    var mpOpts = [];
    for (var mi = 1; mi <= 8; mi++) {
      mpOpts.push('<option value="' + mi + '"' + (mi === mpNow ? ' selected' : '') + '>' + mi + T('multiPvUnit') + '</option>');
    }
    els.sel_multiPv.innerHTML = mpOpts.join('');
  }
  var MD_OPTS = [0, 24, 32, 40, 48, 56, 64, 72, 88, 100];
  var mdNow = S.maxDepth;
  if (!(mdNow === 0 || (mdNow >= 4 && mdNow <= 128))) mdNow = 100;
  S.maxDepth = mdNow;
  if (els.sel_maxDepth) {
    els.sel_maxDepth.innerHTML = MD_OPTS.map(function (v) {
      return '<option value="' + v + '"' + (v === mdNow ? ' selected' : '') + '>' +
             (v ? String(v) : T('mdNone')) + '</option>';
    }).join('');
    selSyncCustom(els.sel_maxDepth, mdNow, function (x) { return String(x); });
  }
  if (els.inp_maxDepth) els.inp_maxDepth.value = mdNow ? String(mdNow) : '';
  var MN_OPTS = [0, 1000000, 4000000, 16000000, 64000000, 256000000, 1000000000];
  var mnNow = S.maxNodes;
  if (!(mnNow === 0 || (mnNow >= 100000 && mnNow <= 1000000000))) mnNow = 0;
  S.maxNodes = mnNow;
  if (els.sel_maxNodes) {
    els.sel_maxNodes.innerHTML = MN_OPTS.map(function (v) {
      return '<option value="' + v + '"' + (v === mnNow ? ' selected' : '') + '>' +
             (v ? (v / 1000000) + 'M' : T('mnNone')) + '</option>';
    }).join('');
    selSyncCustom(els.sel_maxNodes, mnNow, function (x) { return (x / 1000000) + 'M'; });
  }
  if (els.inp_maxNodes) els.inp_maxNodes.value = mnNow ? String(+(mnNow / 1000000).toFixed(1)) : '';

  applyEngineConfig();
}

function boot() {
  grab();
  
  if (els.t_ver) els.t_ver.textContent = 'v' + APP_VERSION;
  LocalAI.boot();              
                               
  try {
    var st = JSON.parse(localStorage.getItem(settingsKey()) || '{}');
    for (var k in st) if (S.hasOwnProperty(k)) S[k] = st[k];
    var rgBoot = loadRuleGlobal();
    if (rgBoot != null) S.rule = rgBoot;
    
    if (S.mode !== 'pve' && S.mode !== 'place' && S.mode !== 'endgame') S.mode = 'place';   
    
    if (st.aiB === undefined && st.aiW === undefined && st.side !== undefined) {
      setAiSides(st.side !== 'b', st.side !== 'w');
    } else {
      setAiSides(S.aiB, S.aiW);
    }
    
    if (aiVsAi()) S.paused = false;
  } catch (e) {}

  if (!G.board) G.board = newBoard();
  enginePrefEnsure();    // seed the engine key from legacy saves (one-time)
  enginePrefApplyToS();  // threads/hash: the engine key is the source of truth
  prefsApplyToS();       // remaining global prefs as fallback
  initPanels();
  applyLayout();
  try { syncPinKeys(); } catch (e) {}
  
  wireColors();
  wireCards();
  wireAnalysis();

  els.board.addEventListener('click', onBoardClick);
  
  els.board.addEventListener('contextmenu', function (ev) {
    ev.preventDefault();
    if (G.review) { rvGoTo(G.review.k - 1); return; }   
    undoOne();          
  });
  bindBoardHover();          
  window.addEventListener('resize', function () { fitFont(); layoutBoard(); visLayout(); });
  if (window.ResizeObserver) new ResizeObserver(function () { fitFont(); layoutBoard(); }).observe(els.boardWrap);

  els.btn_lang.onclick = function () { S.lang = (S.lang === 'zh') ? 'en' : 'zh'; save(); applyLang(); refreshUI(); };
  
  els.btn_pause.onclick = function () {
    
    if (aiVsAi()) {
      S.paused = !S.paused;
      save();
      refreshUI();
      if (!S.paused) maybeAi();          
      return;
    }
    if (G.busy) { S.paused = true; save(); refreshUI(); return; }
    if (S.paused) { S.paused = false; save(); refreshUI(); maybeAi(); return; }
    aiAssistOnce();
  };
  els.btn_prev.onclick = stepBack;
  els.btn_next.onclick = stepForward;
  
  els.btn_reset.onclick = function () {
    if (G.moves.length >= 2) {
      var rec = { ts: Date.now(), src: 'local', rule: S.rule, side: S.side,
                  moves: G.moves.map(function (m) { return [m.x, m.y, m.c]; }) };
      
      if (S.egLocked && S.egBase && S.egBase.length) {
        rec.eg = true; rec.egLen = S.egBase.length;
      }
      addRecord(rec);
      renderDrawer();
    }
    
    var restartSpam = noteRestartSpam();
    resetGame(false);
    openStart();
    refreshUI();
    
    if (!restartSpam) maybeAi();
  };
  els.btn_save.onclick = savePos;
  els.btn_save_rv.onclick = savePos;          
  
  els.btn_rot.onclick = function () { layoutApply(ROT_CW); };
  
  els.btn_shift.onclick = function (e) {
    e.stopPropagation();
    els.mirrorPop.hidden = true;
    els.shiftPop.hidden = !els.shiftPop.hidden;
  };
  els.btn_mirror.onclick = function (e) {
    e.stopPropagation();
    els.shiftPop.hidden = true;
    els.mirrorPop.hidden = !els.mirrorPop.hidden;
  };
  els.btn_mv_fv.onclick = function () { layoutApply(FLIP_H); };   
  els.btn_mv_fh.onclick = function () { layoutApply(FLIP_V); };   
  els.btn_mv_d1.onclick = function () { layoutApply(DIAG_1); };   
  els.btn_mv_d2.onclick = function () { layoutApply(DIAG_2); };   
  els.btn_mv_up.onclick = function () { layoutApply(SHIFT_U); };
  els.btn_mv_down.onclick = function () { layoutApply(SHIFT_D); };
  els.btn_mv_left.onclick = function () { layoutApply(SHIFT_L); };
  els.btn_mv_right.onclick = function () { layoutApply(SHIFT_R); };
  els.btn_mv_close.onclick = function () { els.mirrorPop.hidden = true; };      
  els.btn_shift_close.onclick = function () { els.shiftPop.hidden = true; };    
  document.addEventListener('click', function (e) {
    if (els.mirrorPop && !els.mirrorPop.hidden &&
        !els.mirrorPop.contains(e.target) && e.target !== els.btn_mirror) {
      els.mirrorPop.hidden = true;
    }
    if (els.shiftPop && !els.shiftPop.hidden &&
        !els.shiftPop.contains(e.target) && e.target !== els.btn_shift) {
      els.shiftPop.hidden = true;
    }
  });
  
  els.btn_review.onclick = openReviewFromBoard;
  
  function linkVisMutex() {  }
  if (els.btn_link) els.btn_link.onclick = function () { tellHost({ type: 'openLink' }); };
  
  if (els.btn_vis) els.btn_vis.onclick = function () { tellHost({ type: 'openVis' }); };
  els.btn_ten.onclick = function () { if (openActive() && OPEN.kind === 'tar') startTenCall(); };
  
  if (els.btn_balcall) els.btn_balcall.onclick = function () { balCallTen(); };
  
  if (els.yamaDecl && !els.yamaDecl.childElementCount) {
    for (var dk = 1; dk <= 10; dk++) {
      var db = document.createElement('button');
      db.className = 'btn';
      db.textContent = String(dk);
      (function (k) { db.onclick = function () { declareYamaN(k); }; })(dk);
      els.yamaDecl.appendChild(db);
    }
  }
  
  if (els.btn_tara_yes) els.btn_tara_yes.onclick = function () { doTaraSwap(false); };
  if (els.btn_tara_no) els.btn_tara_no.onclick = function () {
    if (openActive() && OPEN.phase === 'swapask') {
      OPEN.phase = null; openHint(); refreshUI(); maybeAi();
    }
  };
  
  els.btn_swap_first.onclick = function () { if (OPEN && OPEN.choice) chooseSwapSide('b'); };
  els.btn_swap_second.onclick = function () { if (OPEN && OPEN.choice) chooseSwapSide('w'); };

  els.btn_history.onclick = function () { openDrawer('hist'); };
  
  wireDrawer();
  wireAbout();

  els.btn_copy.onclick = function () {
    els.inp_code.value = buildCode();
    els.inp_code.select();
    try { document.execCommand('copy'); } catch (e) {}
    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(els.inp_code.value).catch(function () {});
    }
    tellHost({ type: 'copy', text: els.inp_code.value });   
    setStat(null, T('copied'));
  };
  els.btn_paste.onclick = function () {
    
    if (HOST) tellHost({ type: 'paste' });
    if (navigator.clipboard && navigator.clipboard.readText) {
      navigator.clipboard.readText().then(applyCodeText)
        .catch(function () { if (!HOST) els.inp_code.focus(); });
    } else if (!HOST) {
      els.inp_code.focus();                                 
    }
  };
  els.btn_load.onclick = function () { loadFromCode(els.inp_code.value); };

  els.seg_mode.onclick = function (e) {
    var b = e.target.closest('button'); if (!b) return;
    S.mode = b.getAttribute('data-mode');
    [].forEach.call(els.seg_mode.querySelectorAll('button'), function (x) { x.classList.toggle('on', x === b); });
    
    if (S.mode === 'endgame') { S.egLocked = false; S.egBase = null; }
    save();
    
    if (S.mode === 'place' || S.mode === 'endgame') { G.heat = []; G.nums = []; }
    refreshUI();
    paint();
    
    if (S.mode === 'endgame') setStat(null, S.egLocked ? T('egDashLocked') : T('egDash'));
    if (S.mode === 'pve') { refreshCoach(); maybeAi(); }
  };
  
  els.btn_eg_ok.onclick = function () {
    if (S.egLocked) {
      S.egLocked = false; S.egBase = null;
      setStat(null, T('egDash'));
    } else {
      S.egBase = G.moves.map(function (m) { return [m.x, m.y, m.c]; });
      S.egLocked = true;
      S.mode = 'place';
      [].forEach.call(els.seg_mode.querySelectorAll('button'), function (x) {
        x.classList.toggle('on', x.getAttribute('data-mode') === 'place');
      });
      setStat(null, T('egDashLocked'));
    }
    save(); refreshUI();
  };
  els.btn_eg_seq.onclick = function () { S.egSeq = true; save(); refreshUI(); };
  els.btn_eg_free.onclick = function () { S.egSeq = false; save(); refreshUI(); };
  els.btn_eg_b.onclick = function () { S.egColor = 1; save(); refreshUI(); };
  els.btn_eg_w.onclick = function () { S.egColor = 2; save(); refreshUI(); };
  
  function aiSideToggle(key) {
    
    if (linkSessionOn() && LINK_CFG_LAST) {
      setAiSides(LINK_CFG_LAST.aiB === true, LINK_CFG_LAST.aiW === true);
      syncSideUI(); syncAiSidePop(); refreshUI();
      setStat(null, T('statLinkSideOwn') ||
        '连线会话中：AI 执子由连线窗控制，已同步回连线档');
      return;
    }
    var bOn = (key === 'b') ? !S.aiB : S.aiB;
    var wOn = (key === 'w') ? !S.aiW : S.aiW;
    setAiSides(bOn, wOn);
    
    if (S.mode !== 'pve') S.mode = 'pve';
    save();
    S.paused = false;
    try { window.chrome && window.chrome.webview &&
      window.chrome.webview.postMessage({ type: 'aiSide', b: bOn, w: wOn, mode: S.mode }); } catch (e) {}
    
    if (aiVsAi()) S.paused = false;
    
    coachPauseByAiVsAi();
    syncSideUI();
    syncAiSidePop();
    save();
    refreshUI();
    refreshCoach();
    
    noteSideSwitch();
    
    if (!openActive()) maybeAi();
  }
  els.seg_side.onclick = function (e) {
    var b = e.target.closest('button'); if (!b) return;
    var key = b.getAttribute('data-ai');
    if (!key) return;
    aiSideToggle(key);
  };
  
  function syncAiSidePop() {
    if (els.ai_side_b) els.ai_side_b.classList.toggle('on', !!S.aiB);
    if (els.ai_side_w) els.ai_side_w.classList.toggle('on', !!S.aiW);
    if (els.num_turn2) els.num_turn2.value = turnSecs();
  }
  if (els.ai_side_b) els.ai_side_b.onclick = function () { aiSideToggle('b'); };
  if (els.ai_side_w) els.ai_side_w.onclick = function () { aiSideToggle('w'); };
  if (els.num_turn2) {
    els.num_turn2.onchange = function () { setTurn(els.num_turn2.value); };
  }
  if (els.btn_aiside) {
    els.btn_aiside.onclick = function (e) {
      e.stopPropagation();
      if (els.aiSidePop) {
        var willOpen = els.aiSidePop.hidden;          
        els.aiSidePop.hidden = !els.aiSidePop.hidden;
        
        if (willOpen && !S.aiB && !S.aiW && !linkSessionOn()) {
          setAiSides(true, false);
          if (aiVsAi()) S.paused = false;
          syncSideUI(); save(); refreshUI(); refreshCoach();
          if (!openActive()) maybeAi();
        }
        syncAiSidePop();
        els.btn_aiside.classList.toggle('on', !els.aiSidePop.hidden);
      }
    };
  }
  if (els.btn_ai_close) {
    els.btn_ai_close.onclick = function () {
      if (els.aiSidePop) els.aiSidePop.hidden = true;
      if (els.btn_aiside) els.btn_aiside.classList.remove('on');
    };
  }
  
  function turnSecs() { return +(S.turnMs / 1000).toFixed(3); }
  function setTurn(v) {
    
    var secs = Math.round((+v || 2) * 1000) / 1000;      
    secs = Math.max(0.2, Math.min(300, secs));           
    S.turnMs = Math.round(secs * 1000);
    els.num_turn.value = String(secs);
    if (els.num_turn2) els.num_turn2.value = String(secs);   
    save();
  }
  els.num_turn.onchange = function () { setTurn(els.num_turn.value); };
  els.sel_rule.onchange = function () {
    var prev = +S.rule;
    S.rule = +els.sel_rule.value; save(); applyRuleHint();
    saveRuleGlobal(S.rule);
    if (linkSessionOn()) tellHost({ type: 'linkCfgSync', rule: +S.rule });
    if (+S.rule === prev) { refreshUI(); return; }
    
    if (G.moves.length) {
      if (openActive()) OPEN = null;
      G.heat = []; G.nums = []; G.coach = [];
      forbiddenMarksInvalidate();
      recomputeOver();
      refreshUI();
      paint();
      setStat(null, T('ruleKept').replace('{r}', ((I18N[S.lang].ruleName || {})[+S.rule]) || ''));
      refreshCoach();
      if (!G.over) maybeAi();           
      return;
    }
    resetGame(false); openStart(); refreshUI();   
  };
  
  if (linkSessionOn()) tellHost({ type: 'linkCfgSync', rule: +S.rule });
  els.sel_cores.onchange = function () {
    S.cores = Math.max(MIN_THREADS, +els.sel_cores.value | 0);
    enginePrefSet(S.cores, S.hashMB);   // explicit user change -> write the engine key
    save(); applyEngineConfig();
  };
  els.sel_hash.onchange = function () {
    S.hashMB = +els.sel_hash.value;
    enginePrefSet(S.cores, S.hashMB);   // explicit user change -> write the engine key
    save(); applyEngineConfig();
    
    renderHashInfo();
  };
  if (els.sel_variant) els.sel_variant.onchange = function () {
    S.variant = els.sel_variant.value || '';
    variantPrefSet(S.variant);
    try { fillVariantSelect(); } catch (eV) {}
    applyEngineConfig();
  };
  if (els.sel_candRange) els.sel_candRange.onchange = function () {
    S.candRange = +els.sel_candRange.value; save(); applyEngineConfig();
  };
  if (els.sel_multiPv) els.sel_multiPv.onchange = function () {
    S.nbest = Math.max(1, Math.min(8, Math.round(+els.sel_multiPv.value || 3)));
    els.sel_multiPv.value = String(S.nbest);
    if (els.num_nbest) els.num_nbest.value = String(S.nbest);
    save();
  };
  if (els.inp_maxDepth) {
    els.inp_maxDepth.value = S.maxDepth ? String(S.maxDepth) : '';
    els.inp_maxDepth.onchange = els.inp_maxDepth.onblur = function () {
      var raw = String(els.inp_maxDepth.value || '').trim();
      var v;
      if (raw === '' || raw === '0') v = 0;
      else {
        v = parseInt(raw, 10);
        if (!isFinite(v)) v = 72;
        if (v > 0 && v < 4) v = 4;
        if (v > 128) v = 128;
      }
      els.inp_maxDepth.value = v ? String(v) : '';
      S.maxDepth = v;
      selSyncCustom(els.sel_maxDepth, v, function (x) { return String(x); });
      save(); applyEngineConfig();
    };
  }
  if (els.inp_maxNodes) {
    els.inp_maxNodes.value = S.maxNodes ? String(+(S.maxNodes / 1000000).toFixed(1)) : '';
    els.inp_maxNodes.onchange = els.inp_maxNodes.onblur = function () {
      var raw = String(els.inp_maxNodes.value || '').trim();
      var m;
      if (raw === '' || raw === '0') m = 0;
      else {
        m = parseFloat(raw);
        if (!isFinite(m) || m <= 0) m = 0.1;
        if (m > 256) m = 256;
        m = Math.round(m * 10) / 10;
      }
      els.inp_maxNodes.value = m ? String(m) : '';
      S.maxNodes = Math.round(m * 1000000);
      selSyncCustom(els.sel_maxNodes, S.maxNodes, function (x) { return (x / 1000000) + 'M'; });
      save(); applyEngineConfig();
    };
  }
  if (els.sel_maxDepth) els.sel_maxDepth.onchange = function () {
    var v = +els.sel_maxDepth.value;
    S.maxDepth = v;
    if (els.inp_maxDepth) els.inp_maxDepth.value = v ? String(v) : '';
    save(); applyEngineConfig();
  };
  if (els.sel_maxNodes) els.sel_maxNodes.onchange = function () {
    var v = +els.sel_maxNodes.value;
    S.maxNodes = v;
    if (els.inp_maxNodes) els.inp_maxNodes.value = v ? String(+(v / 1000000).toFixed(1)) : '';
    save(); applyEngineConfig();
  };
  if (els.sel_weights) els.sel_weights.onchange = function () {
    var v = els.sel_weights.value;
    if (v !== 'nnue' && v !== 'classic') return;
    S.weights = v;
    weightsPrefSet(v);
    save(); applyEngineConfig();
  };
  
  els.chk_coach.onchange = function () {
    S.coach = !!els.chk_coach.checked; save();
    if (S.coach) { evalQuietAi(); refreshCoach().then(paint); }
    else { clearCoach(); paint(); }
  };
  els.chk_num.onchange = function () { S.showNum = !!els.chk_num.checked; save(); paint(); };
  
  els.btn_bgThink.onclick = function () {
    S.bgThink = !S.bgThink; save();
    if (els.btn_bgThink) {
      els.btn_bgThink.classList.toggle('on', !!S.bgThink);
      els.btn_bgThink.setAttribute('aria-pressed', S.bgThink ? 'true' : 'false');
    }
    bgThinkHintSync();
    if (S.bgThink) { bgThinkIntroOnce(); }
    else { bgThinkStop(); bgThinkHaltEngine(); }
  };
  
  els.chk_preview.onchange = function () { S.previewOn = !!els.chk_preview.checked; save(); paint(); };
  
  if (els.btn_live) els.btn_live.onclick = liveAnaToggle;

  els.btn_save_rv.onclick = savePos;          

  els.num_turn.value = turnSecs();
  els.sel_rule.value = String(S.rule);
  els.chk_coach.checked = !!S.coach;
  els.chk_num.checked = !!S.showNum;
  els.chk_preview.checked = S.previewOn !== false;
  if (els.btn_bgThink) {
    els.btn_bgThink.classList.toggle('on', !!S.bgThink);
    els.btn_bgThink.setAttribute('aria-pressed', S.bgThink ? 'true' : 'false');
  }
  try { bgThinkHintSync(); } catch (eBH) {}
  els.chk_preview.checked = S.previewOn !== false;   
  [].forEach.call(els.seg_mode.querySelectorAll('button'), function (b) { b.classList.toggle('on', b.getAttribute('data-mode') === S.mode); });
  syncSideUI();                              
  restoreGame();                              

  if (HOST) {
    window.chrome.webview.addEventListener('message', function (ev) {
      tellHostDefer = true;                    
      try { onHostMsg(ev); } finally { tellHostDefer = false; }
    });
    window.__gbReadySent = true;   
    window.chrome.webview.postMessage({ type: 'ready', theme: themeKey(), bg: pageBgHex(), lang: S.lang });
  } else {
    buildSelects();
  }

  function onHostMsg(ev) {
    var m = ev.data; if (!m) return;
    
    if (m.type === 'engineOut' || m.type === 'engineAck' || m.type === 'engineAsk') {
      try { LocalAI.relay(m); } catch (e) {}
      return;
    }
    if (m.type === 'host') {
      G.host = {
        cpu: m.cpu || 4, threadsDefault: m.threadsDefault || 2, threadsMax: m.threadsMax || 3,
        memMB: m.memMB || 0, native: !!m.native,
        // Host-persisted user engine prefs (0 = never set); buildSelects treats them as truth.
        userThreads: m.userThreads | 0, userHashMB: m.userHashMB | 0,

        physCores: m.physCores || 0, memAvailMB: m.memAvailMB || 0,
        cpuName: m.cpuName || '', variant: m.variant || ''
      };
      if (typeof m.avx2 === 'boolean') {
        G.host.sse = !!m.sse; G.host.avx2 = !!m.avx2; G.host.avx512 = !!m.avx512;
        G.host.avx512vnni = !!m.avx512vnni; G.host.avxvnni = !!m.avxvnni;
      }
      G.host.variantBench = m.variantBench || '';
      buildSelects();
    } else if (m.type === 'sysInfo') {
      
      G.sysInfo = m;
      renderSysInfo();
      if (els.about && !els.about.hidden) renderAbout();
    } else if (m.type === 'sysMem') {
      
      if (G.sysInfo) {
        G.sysInfo.memTotalMB = m.memTotalMB | 0;
        G.sysInfo.memAvailMB = m.memAvailMB | 0;
        G.sysInfo.memLoad = m.memLoad | 0;
      }
      renderSysInfo();
      renderHashInfo();                                   
      if (els.about && !els.about.hidden) renderAbout();
    } else if (m.type === 'external') {
      ingestExternal(m.payload || '');
    } else if (m.type === 'linkPos') {
      
      try { linkIngest(JSON.parse(m.payload || '{}')); } catch (e) {}
    } else if (m.type === 'linkOn') {
      setLinkMode(!!m.on);
    } else if (m.type === 'linkClosed' || m.type === 'visClosed') {
      
      if (typeof linkVisMutex === 'function') linkVisMutex('none');
    } else if (m.type === 'linkCfg') {
      
      LINK_CFG_LAST = m;
      var cfgChanged = false;
      if (typeof m.aiB === 'boolean' && typeof m.aiW === 'boolean') {
        var sidesChanged = (S.aiB !== m.aiB || S.aiW !== m.aiW);
        
        setAiSides(m.aiB, m.aiW);
        if (typeof syncAiSidePop === 'function') syncAiSidePop();
        if (typeof syncSideUI === 'function') syncSideUI();
        if (sidesChanged) {
          cfgChanged = true;
          
        }
      }
      
      if (m.turnMs >= 500 && m.turnMs <= 300000 && S.turnMs !== m.turnMs) {
        S.turnMs = m.turnMs; if (els.num_turn) els.num_turn.value = turnSecs(); cfgChanged = true;
      }
      
      if (m.rule >= 0 && m.rule <= 2 && +S.rule !== +m.rule && els.sel_rule) {
        els.sel_rule.value = String(+m.rule);
        els.sel_rule.onchange();
      }
      S.linkAuto = !!m.auto; if (m.clicks) S.linkClicks = m.clicks;
      save(); if (cfgChanged) refreshUI();
    } else if (m.type === 'linkTip') {
      
      var lkst = document.getElementById('lkStatus');
      if (lkst) lkst.textContent = m.txt || '';
    } else if (m.type === 'linkBalance') {
      
      if (m.kind === 'bal' || m.kind === 'bal1' || m.kind === 'bal2') { if (els.btn_an_bal) els.btn_an_bal.click(); }
      setStat(null, T('statLinkBal') || '连线：平衡计算已在主窗进行');
    } else if (m.type === 'historyInbox') {
      var t = m.text || '';
      if (t.charAt(0) === '"') { try { t = JSON.parse(t); } catch (e) {} }
      ingestExternal(t);
    } else if (m.type === 'clip') {
      
      applyCodeText(m.text || '');
    } else if (m.type === 'histTxt') {
      
      importHist(m.text == null ? '' : String(m.text));
    } else if (m.type === 'histExported') {
      
      setStat(null, T('expDone').replace('{n}', String(m.n || 0)));
    } else if (m.type === 'testHistRoundtrip') {
      
      try { testHistExport(); }
      catch (e) { tellHost({ type: 'dbg', where: 'hook-threw', err: String((e && e.stack) || e) }); }
    } else if (m.type === 'testHistNowImport') {
      
      pickImport();
    } else if (m.type === 'testOpenReview') {
      
      openReviewWindow(TEST_RV_RECORD, true);   
    } else if (m.type === 'testSavePos') {
      
      savePos();
    } else if (m.type === 'testAiBoth') {
      
      testAiBoth();
    }
  }

  function testAiBoth() {
    var rep = function (tag) {
      tellHost({
        type: 'testSelfPlay', tag: tag, moves: G.moves.length,
        paused: !!S.paused, mode: S.mode, busy: !!G.busy,
        icon: els.btn_pause ? els.btn_pause.textContent : '',
        disabled: !!(els.btn_pause && els.btn_pause.disabled),
        ai: (S.aiB ? 'B' : '-') + (S.aiW ? 'W' : '-'),
      });
    };
    var bs = els.seg_side.querySelectorAll('button');
    var mb = els.seg_mode.querySelector('button[data-mode="pve"]');
    if (mb && S.mode !== 'pve') mb.click();          
    
    els.num_turn.value = '1';
    els.num_turn.onchange();
    els.btn_reset.click();                           
    
    if (S.aiB && S.aiW) bs[0].click();               
    if (!S.aiW) bs[1].click();
    if (!S.aiB) bs[0].click();                       
    rep('start');
    var iv = setInterval(function () { rep('tick'); }, 1000);
    setTimeout(function () { els.btn_pause.click(); rep('click-stop'); }, 9000);
    setTimeout(function () { els.btn_pause.click(); rep('click-resume'); }, 16000);
    setTimeout(function () { clearInterval(iv); rep('end'); }, 24000);
  }

  applyColors();
  applyTheme();
  applyLang();
  layoutBoard();
  refreshUI();
  
  bootDone();
  setTimeout(function () { renderDrawer(); }, 0);

  if ([5, 6, 7].indexOf(+S.rule) >= 0 && !G.moves.length) openStart();
  else openHint();

  maybeAi();
}

function bootDone() {
  var b = document.getElementById('boot');
  if (!b) return;
  b.classList.add('off');                    
  setTimeout(function () { if (b.parentNode) b.parentNode.removeChild(b); }, 260);
}

function maybeAi() {
  
  if (linkSessionOn()) return;
  
  if (G.sideSettle) return;
  if (RV_MODE || S.mode !== 'pve' || S.paused || G.review || G.over) return;
  
  if (isAiColor(curColor())) aiMove();        
}

function maybeAiForce() {
  if (RV_MODE || S.mode !== 'pve' || S.paused || G.review || G.over) return;
  if (G.sideSettle) return;
  var nb = 0, nw = 0;
  (G.moves || []).forEach(function (mv) {
    if (!mv || mv.length < 3) return;
    if (mv[2] === 1) ++nb; else if (mv[2] === 2) ++nw;
  });
  var turn = (nb <= nw) ? 1 : 2;      
  var bakB = S.aiB, bakW = S.aiW;
  if (turn === 1) S.aiB = true; else S.aiW = true;
  aiMove();
  S.aiB = bakB; S.aiW = bakW;
}

function linkBoardFp(bd) {
  var s = '';
  for (var y = 0; y < N; y++) for (var x = 0; x < N; x++) s += bd[y][x];
  return s;
}

function linkSideToMove(bd) {
  var bn = 0, wn = 0;
  for (var y = 0; y < N; y++) for (var x = 0; x < N; x++) {
    if (bd[y][x] === 1) bn++; else if (bd[y][x] === 2) wn++;
  }
  return (bn <= wn) ? 1 : 2;
}

function linkSay(stZh, stEn, bestCoord) {
  try { tellHost({ type: 'linkStats', st: lkL(stZh, stEn), best: bestCoord || '' }); } catch (e) {}
}

var LINK_LAST_DROP = null;
function linkDropCell(bd, x, y, force) {
  var fp = linkBoardFp(bd);
  if (!force && LINK_LAST_DROP && LINK_LAST_DROP.fp === fp && LINK_LAST_DROP.x === x &&
      LINK_LAST_DROP.y === y && (Date.now() - LINK_LAST_DROP.t) < 10000) return false;
  LINK_LAST_DROP = { fp: fp, x: x, y: y, t: Date.now() };
  adFireAt(x, y, fp, !!force, false);
  return true;
}

var AD_WAITING = false, AD_RETRY_USED = false, AD_REAL_USED = false, AD_FAIL = 0, AD_TIMER = null;
function adConfirmDrop() {
  if (AD_TIMER) { clearTimeout(AD_TIMER); AD_TIMER = null; }
  if (!AD_WAITING) return;
  AD_WAITING = false; AD_RETRY_USED = false; AD_REAL_USED = false; AD_FAIL = 0;
}
function adFireAt(x, y, fp, force, real) {
  AD_WAITING = true;
  AD_RETRY_USED = !!force;              
  AD_REAL_USED = !!real;                
  if (AD_TIMER) clearTimeout(AD_TIMER);
  AD_TIMER = setTimeout(function () {
    AD_TIMER = null;
    if (!AD_WAITING) return;
    AD_WAITING = false;
    if (!S.linkAuto) return;            
    if (!AD_RETRY_USED) {               
      AD_RETRY_USED = true;
      linkSay('落子未确认，升级真实输入重试…', 'Drop not confirmed - escalating to real input...', '');
      tellHost({ type: 'linkDrop', x: x, y: y, force: true, real: false });
    } else if (!AD_REAL_USED) {
      
      AD_REAL_USED = true;
      linkSay('仍未落上，用与真人相同的点击路径重试…', 'Still not dropped - retrying via human-path click...', '');
      tellHost({ type: 'linkDrop', x: x, y: y, force: true, real: true });
    } else {                            
      AD_FAIL++;
      if (AD_FAIL >= 3) {
        S.linkAuto = false; save();
        linkSay('自动落子连续 3 次未落上 —— 已停机，请检查目标窗口后再打开',
                'Auto-drop failed 3 times - stopped, check the target window', '');
        
        try { tellHost({ type: 'linkStats', st: '', best: '', auto: false }); } catch (e) {}
      }
    }
  }, 4000);
  tellHost({ type: 'linkDrop', x: x, y: y, force: !!force, real: !!real });
}

var LINK_EVAL_LAST = '';   

var LINK_EVAL_BUSY = false, LINK_EVAL_PENDING = '', LINK_EVAL_LASTFP = '', LINK_HAS_FRAME = false;

async function linkEvalSide(bd, side, canDrop) {
  
  var aiSideHard = (S.aiB || S.aiW) ? (S.aiB ? 1 : 2) : 0;
  if (aiSideHard && side !== aiSideHard) return;
  var rule = engineRule();
  var budget = Math.max(300, S.turnMs || 2000);
  
  if (canDrop && S.linkAuto && /^[+-]M/.test(LINK_EVAL_LAST)) budget = Math.min(budget, 200);
  
  if (winPoints(bd, 3 - side, rule).length) budget = Math.round(budget * 1.5);
  else if (openFourPoints(bd, 3 - side, rule).length) budget = Math.round(budget * 1.25);
  linkSay('正在评估我方（' + (side === 1 ? '黑' : '白') + '）…',
          'Evaluating our side (' + (side === 1 ? 'B' : 'W') + ')...', '');
  var r = null;
  try { r = await analyze(budget, 1, null, side, 'tagLinkEval', { board: bd, noPonder: !canDrop }); }
  catch (e) {
    
    linkSay('评估出错：引擎未连接，正在重试…', 'Eval error: engine offline, retrying...', '');
    return;
  }
  var cs = (r && r.candidates) || [];
  if (!cs.length) {
    linkSay('评估出错：引擎未返回结果，正在重试…', 'Eval error: no result from engine...', '');
    return;
  }
  
  if (linkBoardFp(bd) !== linkBoardFp(G.board)) {
    LINK_EVAL_PENDING = linkBoardFp(G.board);
    return;
  }
  var c0 = cs[0];
  var best = r.best || [c0.x, c0.y];
  if (!(best && best.length === 2 && best[0] >= 0 && best[1] >= 0 &&
        best[0] < N && best[1] < N && !bd[best[1]][best[0]])) {
    best = null;
    for (var i = 0; i < cs.length; i++) {
      if (cs[i] && cs[i].x >= 0 && cs[i].y >= 0 && cs[i].x < N && cs[i].y < N &&
          !bd[cs[i].y][cs[i].x]) { best = [cs[i].x, cs[i].y]; break; }
    }
    if (!best) return;
  }
  var ev = evalNum(c0.eval);
  LINK_EVAL_LAST = c0.eval || '';
  
  pushCurve(G.moves.length, isNaN(ev) ? 0 : ev, side === 1);
  var bc = coordName(best[0], best[1]);
  if (canDrop && isAiColor(side) && S.linkAuto && !G.over) {
    linkDropCell(bd, best[0], best[1], false);
    linkSay('评估完成，自动落子（' + (side === 1 ? '黑' : '白') + '）→ ' + bc.toUpperCase(),
            'Evaluated - auto dropping (' + (side === 1 ? 'B' : 'W') + ') at ' + bc.toUpperCase(), bc);
  } else {
    linkSay('评估完成，等待对方落子…（推荐 ' + bc.toUpperCase() + '）',
            'Evaluated - waiting for opponent (best ' + bc.toUpperCase() + ')', bc);
  }
}


function noteEngineDown() {
  G.engineOffline = true;
  LocalAI.boot();                          
  scheduleEngineRetry();
  refreshUI();
}

function scheduleEngineRetry() {
  if (G.retryTimer || !G.engineOffline) return;
  if ((G.retryLeft = (G.retryLeft == null ? 60 : G.retryLeft - 1)) <= 0) return;
  G.retryTimer = setTimeout(function () {
    G.retryTimer = 0;
    if (!G.engineOffline || RV_MODE || G.review) return;
    if (G.sideSettle) return;                 
    if (S.mode !== 'pve' || S.paused || G.over) { G.retryLeft = null; return; }
    if (isAiColor(curColor())) aiMove();           
    else if (S.coach) refreshCoach();              
    else scheduleEngineRetry();                    
  }, 2500);
}

function fitFont() {
  var w = window.innerWidth, h = window.innerHeight;
  var px = Math.max(12, Math.min(20, Math.min(w / 95, h / 48)));
  document.documentElement.style.fontSize = px.toFixed(2) + 'px';
}

function linkSessionOn() { return !!(G && G.linkOn); }
function settingsKey() { return 'gbcalc.settings.v1'; }

function saveRuleGlobal(r) {
  try { localStorage.setItem('gbcalc.rule.v1', String(+r)); } catch (e) {}
}
function loadRuleGlobal() {
  try {
    var v = parseInt(localStorage.getItem('gbcalc.rule.v1'), 10);
    return (v === 0 || v === 1 || v === 2) ? v : null;
  } catch (e) { return null; }
}

// cores/hashMB no longer take part in the two-save cross copy: they are owned by the dedicated
// engine key, and copying them between the two S objects was the propagation path.
function GLOBAL_LOOK_KEYS() { return ['lang', 'theme', 'cssVars', 'customBase', 'rule']; }

var MIG_KEY = 'gbcalc.mig.v1';
function migFlag(name) {
  var o = null;
  try { o = JSON.parse(localStorage.getItem(MIG_KEY) || '{}'); } catch (e) { o = null; }
  if (!o || typeof o !== 'object') o = {};
  if (o[name]) return true;
  o[name] = 1;
  try { localStorage.setItem(MIG_KEY, JSON.stringify(o)); } catch (e) {}
  return false;
}

function save() {
  if (LINK_MODE) {
    try {
      var _bs = window.GB_S;
      if (_bs) {
        if (typeof _bs.bgThink === 'boolean') S.bgThink = _bs.bgThink;
        if (_bs.turnMs >= 200 && _bs.turnMs <= 300000) S.turnMs = _bs.turnMs;
      }
    } catch (eS) {}
  }
  // Align the engine fields with the dedicated key before any window writes a save, so a stale
  // window can never write an old cores/hashMB again.
  enginePrefApplyToS();
  prefsSyncFromS();
  try {
    var k = settingsKey();
    localStorage.setItem(k, JSON.stringify(S));
    var other = (k === 'gbcalc.settings.v1') ? 'gbcalc.settings.link.v1' : 'gbcalc.settings.v1';
    var ost = null;
    try { ost = JSON.parse(localStorage.getItem(other) || 'null'); } catch (e2) { ost = null; }
    if (!ost || typeof ost !== 'object') return;      
    var changed = false;
    GLOBAL_LOOK_KEYS().forEach(function (kk) {
      if (S[kk] !== undefined && ost[kk] !== S[kk]) { ost[kk] = S[kk]; changed = true; }
    });
    if (changed) localStorage.setItem(other, JSON.stringify(ost));
  } catch (e) {}
}

var LINK_CFG_LAST = null;
function setLinkMode(on) {
  if (!!G.linkOn === !!on) return;
  
  var keepTheme = S.theme, keepCssVars = S.cssVars, keepCustomBase = S.customBase;
  var keepLang = (S.lang === 'en') ? 'en' : 'zh';
  
  try { localStorage.setItem('gbcalc.settings.v1', JSON.stringify(S)); } catch (e) {}
  G.linkOn = !!on;
  document.body.classList.toggle('linkOn', !!on);
  try { localStorage.setItem('gbcalc.link.active', on ? '1' : '0'); } catch (e) {}
  
  LINK_EVAL_BUSY = false; LINK_EVAL_PENDING = ''; LINK_EVAL_LASTFP = ''; LINK_HAS_FRAME = false;
  
  var nk = 'gbcalc.settings.v1';
  var st = null;
  try { st = JSON.parse(localStorage.getItem(nk) || 'null'); } catch (e) {}
  if (!st) {
    st = JSON.parse(JSON.stringify(S));
    try { localStorage.setItem(nk, JSON.stringify(st)); } catch (e) {}
  }
  for (var k in st) if (S.hasOwnProperty(k)) S[k] = st[k];
  enginePrefApplyToS();  // threads/hash: the engine key is the source of truth
  prefsApplyToS();   // link save must not override the other global prefs
  
  S.theme = keepTheme; S.cssVars = keepCssVars; S.customBase = keepCustomBase;
  S.lang = keepLang;
  
  if (on && LINK_CFG_LAST) {
    var lc = LINK_CFG_LAST;
    if (typeof lc.aiB === 'boolean' && typeof lc.aiW === 'boolean') setAiSides(lc.aiB, lc.aiW);
    if (lc.turnMs >= 500 && lc.turnMs <= 300000) S.turnMs = lc.turnMs;
    if (lc.rule >= 0 && lc.rule <= 2) S.rule = +lc.rule;
    S.linkAuto = !!lc.auto; if (lc.clicks) S.linkClicks = lc.clicks;
    save();
  }
  
  if (on) {
    S.mode = 'pve';
    G.heat = []; G.nums = [];
    ++gAiGen; G.busy = false; G.think = [];
    tellHost({ type: 'linkCfgSync', rule: +S.rule });
  } else {
    ++gAiGen; G.busy = false; G.think = [];     
  }
  
  document.body.setAttribute('data-theme', S.theme);
  applyColorVars(S.cssVars);
  setAiSides(S.aiB, S.aiW);
  if (els.seg_mode) [].forEach.call(els.seg_mode.querySelectorAll('button'), function (x) {
    x.classList.toggle('on', x.getAttribute('data-mode') === S.mode);
  });
  if (els.num_turn) els.num_turn.value = ((+S.turnMs || 2000) / 1000).toFixed(3);
  if (els.sel_rule) els.sel_rule.value = String(S.rule);
  applyLayout();                       
  // Link session opened: collect all cards into the left dock and persist it.
  if (on) { cardsToLeftDock(); persistLayout(); }
  
  applyLang();
  buildSelects(); refreshUI(); paint();
}

function linkIngest(o) {
  var tmp = [];
  (o.black || []).forEach(function (p) { if (p) tmp.push({ x: p.x, y: p.y, c: 1 }); });
  (o.white || []).forEach(function (p) { if (p) tmp.push({ x: p.x, y: p.y, c: 2 }); });
  if (!tmp.length) return false;
  var blacks = tmp.filter(function (p) { return p.c === 1; });
  var whites = tmp.filter(function (p) { return p.c === 2; });
  var mv = [], bi = 0, wi = 0;
  for (var i = 0; i < tmp.length; i++) {
    var takeB = (i % 2 === 0);
    var p = takeB ? blacks[bi++] : whites[wi++];
    if (!p) p = takeB ? whites[wi++] : blacks[bi++];
    if (!p) break;
    mv.push([p.x, p.y, p.c]);
  }
  
  if (S.mode !== 'pve') {
    S.mode = 'pve';
    [].forEach.call(els.seg_mode.querySelectorAll('button'), function (x) {
      x.classList.toggle('on', x.getAttribute('data-mode') === 'pve');
    });
    G.heat = []; G.nums = [];
    ++gAiGen;                    
    save();
  }
  openRecord({ moves: mv, src: 'link', rule: S.rule, first: 'b' });
  refreshUI(); paint();
  return true;
}

window.addEventListener('wheel', function (e) { if (e.ctrlKey) e.preventDefault(); }, { passive: false });
window.addEventListener('keydown', function (e) {
  if (e.ctrlKey && (e.key === '+' || e.key === '-' || e.key === '=' || e.key === '0')) e.preventDefault();
}, false);

fitFont();

try {
  window.__gbProbe = {
    ver: function () { try { return APP_VERSION; } catch (e) { return "?"; } },
    snap: function () {
      return { cores: S.cores, hashMB: S.hashMB, turnMs: S.turnMs,
               aiB: !!S.aiB, aiW: !!S.aiW, mode: S.mode, paused: !!S.paused,
               busy: !!G.busy, moves: (G.moves || []).length, over: !!G.over };
    },
    setAi: function (b, w) {
      setAiSides(!!b, !!w); S.mode = "pve"; S.paused = false;
      save(); try { refreshUI(); } catch (e) {} return this.snap();
    },
    drop: function (x, y) { return place(x, y, false); },
    think: function () { try { aiMove(); } catch (e) {} return true; }
  };
} catch (e) {}

var GB_BOOT = LINK_MODE ? bootLink : (VIS_MODE ? bootVis : (RV_MODE ? bootReview : boot));
if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', GB_BOOT);
} else {
  GB_BOOT();
}

var LK = { frame: 'none', b: 0, w: 0, board: null, curve: [], canvas: null, curveCv: null,
            balMarks: null, balKind: 'bal1', best: null,
            detected: false };   
var lkStOverride = 0;            

function lkCfg() {
  try { return JSON.parse(localStorage.getItem('gbcalc.link.cfg') || '{}'); } catch (e) { return {}; }
}
function lkSaveCfg(c) { try { localStorage.setItem('gbcalc.link.cfg', JSON.stringify(c)); } catch (e) {} }
function lkSendCfg() {
  var c = lkCfg();
  tellHost({
    type: 'linkCfg',
    auto: !!c.auto, clicks: 1,
    aiB: c.aiB === true, aiW: c.aiW === true,    
    rule: (c.rule >= 0 && c.rule <= 2) ? c.rule : 0,
    turnMs: (c.turnMs >= 200 && c.turnMs <= 300000) ? c.turnMs : 2000,  
  });
}
function lkL(zh, en) { return (S.lang !== 'en') ? zh : en; }   

function lkStatus() {
  var el = document.getElementById('lkStatus');
  if (!el) return;
  
  if (Date.now() - (lkStOverride || 0) < 3500) return;
  var t;
  if (LK.frame === 'locked')
    t = lkL('（黑：' + LK.b + '　白：' + LK.w + '）等待对方落子中……',
            '(Black: ' + LK.b + '  White: ' + LK.w + ') Waiting for opponent...');
  else if (LK.frame === 'pending')
    t = lkL('已框选 —— 点框上「识别」确认（或保持「自动 开」自动识别），成功后框变浅蓝绿',
            'Framed - click "Scan" (or keep "Auto" on); it turns teal on success');
  else
    
    t = lkL('请先点「框选棋盘」手动圈定对方棋盘 —— 框住后点框上的「识别」（或开「自动」自动找盘），成功框变浅蓝绿',
            'Click "Frame Board" first to select the opponent board manually - then click Scan on the frame (or turn Auto on); it turns teal once detected');
  el.textContent = t;
}

function lkFramedNow() { return LK.frame === 'locked'; }

function lkRemindFrame() {
  var el = document.getElementById('lkStatus');
  if (!el) return;
  el.innerHTML = '<b style="color:#e67e22">' + lkL(
    '还没锁定对方棋盘哦 —— 请先点「框选棋盘」手动圈定，再点框上的「识别」（或开「自动」自动找盘）；框变浅蓝绿后这些功能键才生效。',
    'Board not locked yet - click "Frame Board" to select it, then click "Scan" on the frame (or turn Auto on). These keys work once the frame turns teal.') + '</b>';
}

function linkBalanceSwap() {
  var st = document.getElementById('lkStatus');
  var bd = G.board || newBoard();
  var nn = 0;
  if (bd) for (var i = 0; i < 15; i++) for (var j = 0; j < 15; j++) if (bd[i][j]) nn++;
  if (nn > 5) {
    if (st) st.textContent = lkL('棋子已超过 5 个，不再评估交换平衡点',
                                 'Over 5 stones - swap-balance disabled');
    return;
  }
  
  try { if (window.GB_LINK_SWAPBAL) { window.GB_LINK_SWAPBAL(); return; } } catch (e) {}
  if (st) st.textContent = lkL(LK_BAL_TXT.none[0], LK_BAL_TXT.none[1]);
}

function linkBalanceEval() {
  var st = document.getElementById('lkStatus');
  
  try { if (window.GB_LINK_SWAPEVAL) { window.GB_LINK_SWAPEVAL(); return; } } catch (e) {}
  if (st) st.textContent = lkL(LK_BAL_TXT.none[0], LK_BAL_TXT.none[1]);
}

var LK_BAL_TXT = {
  swap:  ['引擎判定：此际应当换手（SWAP）——当前没有比换手更优的平衡点',
          'Engine says SWAP - no better balanced move than passing'],
  none:  ['引擎未返回平衡点（盘面可能已无可选点）', 'No balanced point returned by engine'],
  won:   ['盘面已有成五点 —— 必胜局面没有平衡可言', 'Board already has a five - no balance in a won position'],
  done1: ['平衡一手（最接近 0 分）：', 'Balanced move (closest to 0): '],
  done2: ['平衡两手：', 'Balanced pair: '],
  riffy: ['RIF 开局库评估完成 —— 以下为最平衡的 3 个点（黑≈0 / 白≈0）',
          'RIF openings evaluated - 3 most balanced points (B~0 / W~0)'],
  run:   ['平衡计算中（引擎官方平衡迭代）…', 'Balancing (engine official)...']
};

function lkSyncCount() {
  var el = document.getElementById('lkCount');
  if (!el) return;
  var n = LK.b + LK.w;
  el.textContent = lkL('共 ' + n + ' 子 · 黑 ' + LK.b + ' · 白 ' + LK.w + '',
                       'Total ' + n + ' / Black ' + LK.b + ' / White ' + LK.w + '');
}

function lkPaintBoard() {
  var cv = LK.canvas;
  if (!cv) return;
  var ctx = cv.getContext('2d');
  var cs = getComputedStyle(document.body);
  var css = function (k, dft) { var v = cs.getPropertyValue(k); return (v && v.trim()) || dft; };
  var W = cv.width, H = cv.height, N = 15;
  
  var R = { inner: 4 / 15, coord: 13 / 15, gap: 6 / 15, top: 8 / 15, bot: 22 / 15,
            font: 8 / 15, stone: 11 / 30, star: 2 / 15, line: 1 / 15 };
  var uw = 14 + 2 * R.coord + 2 * R.inner, uh = R.top + 14 + R.bot;
  var cell = Math.min(W / uw, H / uh);              
  var ox = (W - cell * uw) / 2 + cell * (R.inner + R.coord);   
  var oy = (H - cell * uh) / 2 + cell * R.top;                 
  var span = cell * 14, gapPx = cell * R.gap, r = cell * R.stone;
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = css('--board', '#d9b878');
  ctx.fillRect(0, 0, W, H);
  
  ctx.strokeStyle = css('--line', '#8a6f42');
  ctx.lineWidth = Math.max(1, cell * R.line);
  ctx.beginPath();
  for (var i = 0; i < N; i++) {
    var o = cell * i;
    ctx.moveTo(ox, oy + o); ctx.lineTo(ox + span, oy + o);
    ctx.moveTo(ox + o, oy); ctx.lineTo(ox + o, oy + span);
  }
  ctx.stroke();
  
  ctx.fillStyle = css('--axis', '#7a6338');
  ctx.font = Math.max(6, cell * R.font) + 'px "Segoe UI",system-ui,sans-serif';
  ctx.textBaseline = 'middle';
  for (i = 0; i < N; i++) {
    var cy = oy + cell * i, cx = ox + cell * i, row = String(N - i);
    ctx.textAlign = 'right';  ctx.fillText(row, ox - gapPx, cy);
    ctx.textAlign = 'left';   ctx.fillText(row, ox + span + gapPx, cy);
    ctx.textAlign = 'center';
    ctx.fillText(String.fromCharCode(65 + i), cx, oy + span + gapPx + cell * (R.coord / 2));
  }
  
  ctx.fillStyle = css('--star', '#4a3a1e');
  [3, 7, 11].forEach(function (sa) {
    [3, 7, 11].forEach(function (sb) {
      ctx.beginPath();
      ctx.arc(ox + sa * cell, oy + sb * cell, Math.max(1.2, cell * R.star), 0, Math.PI * 2);
      ctx.fill();
    });
  });
  
  var bd = LK.board;
  
  if (LK.best && LK.best.x >= 0 && LK.best.y >= 0) {
    var bpx = ox + LK.best.x * cell, bpy = oy + LK.best.y * cell, br2 = cell * 0.46;
    ctx.beginPath(); ctx.arc(bpx, bpy, br2, 0, Math.PI * 2);
    ctx.strokeStyle = 'rgba(47,216,255,.95)'; ctx.lineWidth = Math.max(2, cell * 0.09); ctx.stroke();
    ctx.beginPath(); ctx.arc(bpx, bpy, br2 * 0.8, 0, Math.PI * 2);
    ctx.strokeStyle = 'rgba(255,255,255,.55)'; ctx.lineWidth = 1; ctx.stroke();
  }
  
  var stoneN = 0;                                    
  for (var y = 0; y < N; ++y) for (var x = 0; x < N; ++x) {
    var st = bd && bd[y] && bd[y][x];
    if (!st) continue;
    stoneN++;
    ctx.beginPath(); ctx.arc(ox + x * cell, oy + y * cell, r, 0, Math.PI * 2);
    if (st === 1) { ctx.fillStyle = css('--blk', '#111111'); ctx.fill(); ctx.strokeStyle = css('--blk-edge', '#e8ecf5'); }
    else { ctx.fillStyle = css('--wht', '#fcfcfc'); ctx.fill(); ctx.strokeStyle = css('--wht-edge', '#969aa3'); }
    ctx.lineWidth = Math.max(1, cell * R.line);
    ctx.stroke();
  }
  
  if (S.showNum && bd) {
    var bn = 0, wn = 0;
    ctx.font = (cell * 0.42).toFixed(0) + 'px sans-serif';
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    for (var y2 = 0; y2 < N; ++y2) for (var x2 = 0; x2 < N; ++x2) {
      var st3 = bd[y2] && bd[y2][x2];
      if (!st3) continue;
      if (st3 === 1) { bn += 2; ctx.fillStyle = '#fff'; ctx.fillText(String(bn - 1), ox + x2 * cell, oy + y2 * cell); }
      else { wn += 2; ctx.fillStyle = '#555'; ctx.fillText(String(wn), ox + x2 * cell, oy + y2 * cell); }
    }
  }
  
  var detected = !!LK.detected || LK.frame === 'locked';
  var hint = !detected ? lkL('尚未识别到棋盘', 'No board detected yet')
           : (stoneN === 0 ? lkL('准备中…', 'Preparing…') : '');
  if (hint) {
    ctx.fillStyle = css('--sub', '#8a8a86');
    ctx.font = Math.max(10, cell * 0.85) + 'px "Segoe UI",system-ui,sans-serif';
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    ctx.fillText(hint, ox + span / 2, oy + span / 2);
  }
}

function lkPaintCurve() {
  var cv = LK.curveCv;
  if (!cv) return;      
  var ax = document.getElementById('lkCurveAxis'),
      scroll = document.getElementById('lkCurveScroll');
  if (!cv || !ax || !scroll) return;
  var dpr = window.devicePixelRatio || 1;
  var cs = getComputedStyle(document.body);
  var sub = (cs.getPropertyValue('--sub') || '#8a8a86').trim();
  var blk = (cs.getPropertyValue('--blk') || '#1b1916').trim();
  var bdc = (cs.getPropertyValue('--border') || '#c8c4b8').trim();
  var H = Math.round(scroll.clientHeight || 100);
  if (H < 80) H = 100;
  var padT = 6, padB = 16, y0 = padT, y1 = H - padB;
  var pts = LK.curve || [];
  var mx = 10;
  pts.forEach(function (p) { var v = Math.abs(p.b || 0); if (v > mx) mx = v; });
  var tier = 10;
  [10, 50, 150, 500, 1500].some(function (t) { tier = t; return mx <= t; });
  function yOf(v) {
    var t = (tier - v) / (2 * tier);
    return y0 + (y1 - y0) * Math.max(0, Math.min(1, t));
  }
  var steps = (tier <= 50) ? (tier / 5) : (tier / 4);
  var AXW = 34;
  
  ax.style.width = AXW + 'px'; ax.style.height = H + 'px';
  ax.width = Math.round(AXW * dpr); ax.height = Math.round(H * dpr);
  var ac = ax.getContext('2d');
  ac.setTransform(1, 0, 0, 1, 0, 0); ac.clearRect(0, 0, ax.width, ax.height);
  ac.scale(dpr, dpr);
  ac.font = '8px sans-serif'; ac.textBaseline = 'middle'; ac.textAlign = 'right';
  for (var g = -tier; g <= tier; g += steps) {
    var gy = yOf(g);
    ac.strokeStyle = (Math.abs(g) < 1e-9) ? sub : bdc;
    ac.lineWidth = (Math.abs(g) < 1e-9) ? 1.2 : 1;
    ac.beginPath(); ac.moveTo(AXW - 5, gy); ac.lineTo(AXW, gy); ac.stroke();
    ac.fillStyle = sub;
    ac.fillText((g > 0 ? '+' : '') + Math.round(g), AXW - 7, gy);
  }
  
  var availW = Math.max(140, scroll.clientWidth - 2);
  var stepX = 14, maxPly = 0;
  pts.forEach(function (p) { maxPly = Math.max(maxPly, p.i || 0); });
  var need = 12 + Math.max(0, maxPly) * stepX + 12;
  var w = Math.max(availW, need);
  cv.style.width = w + 'px'; cv.style.height = H + 'px';
  cv.width = Math.round(w * dpr); cv.height = Math.round(H * dpr);
  var ctx = cv.getContext('2d');
  ctx.setTransform(1, 0, 0, 1, 0, 0); ctx.clearRect(0, 0, cv.width, cv.height);
  ctx.scale(dpr, dpr);
  ctx.lineWidth = 1;
  for (var g2 = -tier; g2 <= tier; g2 += steps) {
    var gy2 = yOf(g2);
    ctx.strokeStyle = (Math.abs(g2) < 1e-9) ? sub : bdc;
    ctx.lineWidth = (Math.abs(g2) < 1e-9) ? 1.2 : 1;
    ctx.beginPath(); ctx.moveTo(0, gy2); ctx.lineTo(w, gy2); ctx.stroke();
  }
  if (pts.length) {
    ctx.strokeStyle = blk;
    ctx.lineWidth = Math.max(1.3, H * 0.03);
    ctx.beginPath();
    pts.forEach(function (p, idx) {
      var px = 4 + (w - 8) * (pts.length === 1 ? 0.5 : idx / (pts.length - 1));
      var py = yOf(Math.max(-tier, Math.min(tier, p.b || 0)));
      if (idx === 0) ctx.moveTo(px, py); else ctx.lineTo(px, py);
    });
    ctx.stroke();
  }
  ctx.fillStyle = sub;
  ctx.font = '8px sans-serif';
  ctx.textAlign = 'left'; ctx.textBaseline = 'bottom';
  ctx.fillText((S.lang !== 'en') ? ('第 ' + (pts.length ? pts[pts.length - 1].i : 0) + ' 手') : ('Ply ' + (pts.length ? pts[pts.length - 1].i : 0)), 4, H - 3);
}

function bootLink() {
  try {
    
    var st1 = JSON.parse(localStorage.getItem('gbcalc.settings.link.v1') || '{}');
    var st = JSON.parse(localStorage.getItem('gbcalc.settings.v1') || '{}');
    if (st && st.theme) S.theme = (st.theme === 'light') ? 'light' : 'dark';
    else if (st1 && st1.theme) S.theme = (st1.theme === 'light') ? 'light' : 'dark';
    if (st && st.lang) S.lang = (st.lang === 'en') ? 'en' : 'zh';
    else if (st1 && st1.lang) S.lang = (st1.lang === 'en') ? 'en' : 'zh';
    if (st && st.cssVars) { S.cssVars = st.cssVars; applyColorVars(st.cssVars); }
    else if (st1 && st1.cssVars) { S.cssVars = st1.cssVars; applyColorVars(st1.cssVars); }
    var rgLk = loadRuleGlobal();
    if (rgLk != null) S.rule = rgLk;
  } catch (e) {}
  document.body.setAttribute('data-theme', S.theme);
  document.body.classList.add('link');
  document.title = lkL('Link', 'Link');   
  
  function lkBrandHTML(){
    
    return '<i>Meter</i> Gomoku Link';
  }
  var lkBootBrand = document.querySelector('#boot .brand');
  if (lkBootBrand) lkBootBrand.innerHTML = lkBrandHTML();
  
  document.documentElement.style.fontSize = '13px';
  grab();
  var pane = document.createElement('div');
  pane.id = 'linkPane';
  pane.innerHTML =
    '<div class="lk-body">' +
      '<div class="lk-panel">' +
        '<div id="lkFnBox">' +
        '<div class="lk-sec lk-fn">' +
          '<div class="seg s3" id="lk_rule">' +
            '<button data-rule="0">' + lkL('无禁手', 'Freestyle') + '</button>' +
            '<button data-rule="1">' + lkL('长连不赢', 'Overline') + '</button>' +
            '<button data-rule="2">' + lkL('有禁手', 'Renju') + '</button>' +
          '</div>' +
        '</div>' +
        '<div class="lk-sec lk-fn lk-btns lk-twoline">' +
          '<div class="lk-btns-row">' +
            '<button id="lk_select" class="btn">' + lkL('框选棋盘', 'Frame Board') + '</button>' +
            '<button id="lk_rcparam" class="btn" title="' +
            lkL('识别参数：把棋盘上方的按键区换成棋盘/棋子识别阈值（灰度/黑白偏差与占比、同步时间间隔），调节即时生效',
                'Recog params: swap the button area above the board for the board/stone recognition thresholds '
               + '(gray / black / white offsets and percents, interval); changes take effect live') +
            '">' + lkL('识别参数', 'Recog Params') + '</button>' +
          '</div>' +
          '<div class="lk-btns-row">' +
            '<button id="lk_real" class="btn lk-tgl" title="' +
            lkL('真实点击：鼠标会真的移到落点去按左键 —— 应对只认真实鼠标输入的程序（如 QQ 五子棋 等）。'
               + '打开期间会占用你的鼠标。',
                'Real click: the mouse really moves to the point and presses — for programs that only '
               + 'accept real mouse input (e.g. QQ Gomoku). It takes over your cursor while enabled.') +
            '">' + lkL('真实点击', 'Real Click') + '</button>' +
            '<button id="lk_auto" class="btn lk-tgl">' + lkL('自动落子', 'Auto Drop') + '</button>' +
            '<button id="lk_adcount" class="btn" title="' + lkL('落子次数：每个落点连点几下（1~3 循环）', 'Clicks per move (cycles 1-3)') + '">×1</button>' +
          '</div>' +
        '</div>' +
        '<div class="lk-sec lk-fn lk-side">' +
          '<div class="lk-ai-row">' +
            '<span class="ai-grp">' +
              '<button id="lk_black" data-ai="b" class="tglbtn"><span class="tgl-txt">' + lkL('AI执黑', 'AI Black') + '</span></button>' +
              '<button id="lk_white" data-ai="w" class="tglbtn"><span class="tgl-txt">' + lkL('AI执白', 'AI White') + '</span></button>' +
            '</span>' +
            '<span class="ai-grp">' +
              '<span class="ai-turn">' + lkL('思考时间', 'Think Time') + '</span>' +
              '<span class="numbox ainum"><input id="lk_turn" type="number" min="0.2" max="300" step="0.1" value="2"><span class="unit">s</span></span>' +
            '</span>' +
          '</div>' +
        '</div>' +
        '<div class="lk-sec lk-fn lk-btns">' +
          '<span class="dashed-group" id="lkSwGrp">' +
          '<button id="lk_bal" class="btn">' + lkL('交换平衡点', 'Swap Balance') + '</button>' +
          '<button id="lk_eval" class="btn">' + lkL('交换评估', 'Swap Eval') + '</button>' +
          '<button id="lk_swxcancel" class="btn">' + lkL('取消', 'Cancel') + '</button>' +
          '</span>' +
        '</div>' +
        '</div>' +
        '<div class="lk-sec lk-fn lk-rcbox" id="lkRcBox" hidden></div>' +
        '<div class="lk-stats">' +
          '<div class="lk-stat"><span class="k">' + lkL('深度', 'Depth') + '</span><span class="v" id="lk_s_depth">-</span></div>' +
          '<div class="lk-stat"><span class="k">' + lkL('速度', 'Speed') + '</span><span class="v" id="lk_s_speed">-</span></div>' +
          '<div class="lk-stat"><span class="k">' + lkL('节点', 'Nodes') + '</span><span class="v" id="lk_s_nodes">-</span></div>' +
          '<div class="lk-stat"><span class="k">' + lkL('评估', 'Eval') + '</span><span class="v" id="lk_s_eval">-</span></div>' +
        '</div>' +
        '<div id="lkStatus" class="lk-status"></div>' +
      '</div>' +
      '<div class="lk-boardcol">' +
        '<div class="lk-count" id="lkCount"></div>' +
        '<canvas id="lkBoard" width="640" height="640"></canvas>' +
      '</div>' +
    '</div>' +
    '<div class="lk-drmask" id="lkSlotMask" hidden></div>' +
    '<div class="lk-drawer" id="lkSlotDrawer" hidden>' +
      '<div class="lk-dr-hd">' +
        '<span class="lk-dr-ttl" id="lk_slotInfo"></span>' +
        '<button id="lk_slotX" class="lk-dr-x" aria-label="close" title="' + lkL('关闭', 'Close') + '">&#215;</button>' +
      '</div>' +
      '<div class="lk-dr-frame">' +
        '<div class="lk-dr-list" id="lkSlotList"></div>' +
      '</div>' +
      '<div class="lk-dr-keys">' +
        '<button id="lk_slotPick" class="btn">' + lkL('加载', 'Load') + '</button>' +
        '<button id="lk_slotAll" class="btn">' + lkL('全选', 'All') + '</button>' +
        '<button id="lk_slotDel" class="btn">' + lkL('删除', 'Delete') + '</button>' +
        '<button id="lk_slotUndo" class="btn">' + lkL('撤销', 'Undo') + '</button>' +
      '</div>' +
    '</div>' +
    '<div class="lk-namemask" id="lkNameMask" hidden>' +
      '<div class="lk-namebox">' +
        '<div class="lk-nm-hd">' +
          '<span class="lk-nm-ttl" id="lk_nameTtl"></span>' +
          '<span class="lk-nm-keys">' +
            '<button id="lk_nameOk" class="btn"></button>' +
            '<button id="lk_nameNo" class="btn"></button>' +
          '</span>' +
        '</div>' +
        '<input id="lk_nameInp" type="text" maxlength="45" autocomplete="off">' +
      '</div>' +
    '</div>' +
    '<div class="lk-brand" id="lkBrandEl"></div>';
  document.body.appendChild(pane);
  var lkBrandEl = document.getElementById('lkBrandEl');
  if (lkBrandEl) lkBrandEl.innerHTML = lkBrandHTML();

  var c = lkCfg();
  c.auto = false;                      
  lkSaveCfg(c);
  LK.canvas = document.getElementById('lkBoard');
  LK.curveCv = document.getElementById('lkCurve');
  var q = function (id) { return document.getElementById(id); };
  var segBtns = document.querySelectorAll('#lk_rule button');
  function ruleSync() {
    [].forEach.call(segBtns, function (b) {
      b.classList.toggle('on', +b.getAttribute('data-rule') === (c.rule >= 0 && c.rule <= 2 ? c.rule : 0));
    });
  }
  c.rule = (+S.rule === 0 || +S.rule === 1 || +S.rule === 2) ? +S.rule
         : ((c.rule >= 0 && c.rule <= 2) ? +c.rule : 0);
  lkSaveCfg(c);
  ruleSync();
  q('lk_auto').classList.toggle('on', !!c.auto);
  
  if (c.aiB !== true && c.aiB !== false) { c.aiB = true; c.aiW = false; }
  if (c.aiB === true && c.aiW === true) c.aiW = false;
  q('lk_auto').onclick = function () {
    c.auto = !(c.auto === true); lkSaveCfg(c);
    q('lk_auto').classList.toggle('on', c.auto === true); lkSendCfg();
    if (c.auto === true) lkBridgeSide(c.aiB ? 0 : (c.aiW ? 1 : -1));  
    lkBridgeAuto(c.auto === true);           
    
    if (!lkFramedNow()) lkRemindFrame();
  };
  
  function lkSyncTgl() {
    q('lk_black').classList.toggle('on', c.aiB === true);
    q('lk_white').classList.toggle('on', c.aiW === true);
  }
  q('lk_black').onclick = function () {
    c.aiB = !(c.aiB === true);
    if (c.aiB) c.aiW = false;                    
    lkSaveCfg(c); lkSyncTgl(); lkSendCfg();
    lkBridgeSide(c.aiB ? 0 : (c.aiW ? 1 : -1));  
    if (!lkFramedNow()) lkRemindFrame();         
  };
  q('lk_white').onclick = function () {
    c.aiW = !(c.aiW === true);
    if (c.aiW) c.aiB = false;                    
    lkSaveCfg(c); lkSyncTgl(); lkSendCfg();
    lkBridgeSide(c.aiW ? 1 : (c.aiB ? 0 : -1));  
    if (!lkFramedNow()) lkRemindFrame();         
  };
  q('lk_select').onclick = function () { tellHost({ type: 'linkSelect' }); };
  
  if (!migFlag('realClickOff')) c.realClick = false;
  if (c.realClick !== true && c.realClick !== false) c.realClick = false;
  lkSaveCfg(c);
  function lkSyncReal() {
    var rc = document.getElementById('__gb_realclick');
    if (!rc) return;
    if (c.realClick === true) rc.setAttribute('data-on', '1');
    else rc.removeAttribute('data-on');
  }
  window.__lkSyncReal = lkSyncReal;
  if (q('lk_real')) {
    q('lk_real').classList.toggle('on', c.realClick === true);
    q('lk_real').onclick = function () {
      c.realClick = !(c.realClick === true);
      lkSaveCfg(c);
      q('lk_real').classList.toggle('on', c.realClick === true);
      lkSyncReal();
    };
  }
  lkSyncTgl();                                   

  var rcState = { interval: 200, gray_offset: 50, black_offset: 96,
                  white_offset: 96, black_percent: 33, white_percent: 33 };
  if (c.linkRecog && typeof c.linkRecog === 'object') {
    for (var rk in rcState) if (typeof c.linkRecog[rk] === 'number') rcState[rk] = c.linkRecog[rk] | 0;
  }
  var RC_FIELDS = [
    { key: 'interval',      id: 'lk_rc_interval', lo: 40, hi: 2000, step: 10,
      label: lkL('同步时间间隔(ms)', 'Interval (ms)'),
      hint: lkL('调小更灵敏、更跟手，但更占 CPU', 'Smaller = faster but more CPU') },
    { key: 'gray_offset',   id: 'lk_rc_gray',     lo: 0,  hi: 255,  step: 1,
      label: lkL('灰度偏差(0-255)', 'Gray offset (0-255)'),
      hint: lkL('调大更易判成棋子，调小更严格（可能漏子）', 'Larger = easier to detect, smaller = stricter') },
    { key: 'black_offset',  id: 'lk_rc_blackoff', lo: 0,  hi: 255,  step: 1,
      label: lkL('黑色偏差(0-255)', 'Black offset (0-255)'),
      hint: lkL('黑子识别过多 → 调小；黑子漏识 → 调大', 'Too many black stones -> lower; missing -> raise') },
    { key: 'black_percent', id: 'lk_rc_blackpct', lo: 0,  hi: 100,  step: 1,
      label: lkL('黑色占比(0-100)', 'Black percent (0-100)'),
      hint: lkL('黑子识别过多 → 调小；黑子漏识 → 调大', 'Too many black stones -> lower; missing -> raise') },
    { key: 'white_offset',  id: 'lk_rc_whiteoff', lo: 0,  hi: 255,  step: 1,
      label: lkL('白色偏差(0-255)', 'White offset (0-255)'),
      hint: lkL('白子识别过多 → 调小；白子漏识 → 调大', 'Too many white stones -> lower; missing -> raise') },
    { key: 'white_percent', id: 'lk_rc_whitepct', lo: 0,  hi: 100,  step: 1,
      label: lkL('白色占比(0-100)', 'White percent (0-100)'),
      hint: lkL('白子识别过多 → 调小；白子漏识 → 调大', 'Too many white stones -> lower; missing -> raise') }
  ];
  var rcBox = q('lkRcBox'), lkFnBox = q('lkFnBox');
  if (rcBox) {
    var rcHTML = '';
    for (var ri = 0; ri < RC_FIELDS.length; ri++) {
      var rf = RC_FIELDS[ri];
      rcHTML +=
        '<div class="lk-rcitem">' +
          '<div class="lk-rcnameline">' +
            '<span class="lk-rclab">' + rf.label + '</span>' +
            '<input class="lk-rcinp" type="text" inputmode="numeric" autocomplete="off" id="' + rf.id +
            '" data-min="' + rf.lo + '" data-max="' + rf.hi + '" title="' + rf.hint + '">' +
          '</div>' +
          '<input class="lk-rcsld" type="range" id="' + rf.id + '_s" min="' + rf.lo +
          '" max="' + rf.hi + '" step="' + rf.step + '" tabindex="-1">' +
        '</div>';
    }
    rcHTML +=
      '<div class="lk-rcbtns">' +
        '<button id="lk_rcGeneral" class="btn" title="' +
        lkL('通用：载入识别默认值（interval 200 / 灰度 50 / 黑偏差 96 / 黑占比 33 / 白偏差 96 / 白占比 33），可识别绝大部分棋盘',
            'General: load the recognition defaults (interval 200 / gray 50 / blackOff 96 / blackPct 33 / whiteOff 96 / whitePct 33)') +
        '">' + lkL('通用', 'General') + '</button>' +
        '<span class="lk-slotbox">' +
          '<button id="lk_rcSlotSave" class="btn" title="' +
          lkL('存档：把当前这 6 项参数按主题名称存起来（最多 50 个）',
              'Save: store the current 6 params under a theme name (max 50)') + '">' +
          lkL('存档', 'Save') + '</button>' +
          '<button id="lk_rcSlotLoad" class="btn" title="' +
          lkL('读档：展开抽屉，载入之前存档的参数', 'Load: open the drawer and load a saved preset') + '">' +
          lkL('读档', 'Load') + '</button>' +
        '</span>' +
        '<span class="lk-rcbtns-r">' +
          '<button id="lk_rcApply" class="btn">' + lkL('应用', 'Apply') + '</button>' +
          '<button id="lk_rcCancel" class="btn">' + lkL('取消', 'Cancel') + '</button>' +
        '</span>' +
      '</div>';
    rcBox.innerHTML = rcHTML;
  }
  function rcSnapshot() {
    return { interval: rcState.interval, gray_offset: rcState.gray_offset,
             black_offset: rcState.black_offset, white_offset: rcState.white_offset,
             black_percent: rcState.black_percent, white_percent: rcState.white_percent };
  }
  function rcPaint() {
    for (var pi = 0; pi < RC_FIELDS.length; pi++) {
      var pf = RC_FIELDS[pi], pv = String(rcState[pf.key]);
      var ps = q(pf.id + '_s'), pt = q(pf.id);
      if (ps) ps.value = pv;
      if (pt) pt.value = pv;
    }
  }
  var rcTimer = 0, rcOrig = null, rcOrigApplied = null;
  function rcLive() {
    clearTimeout(rcTimer);
    rcTimer = setTimeout(function () {
      tellHost({ type: 'linkRecogPreview', recog: rcSnapshot() });
    }, 120);
  }
  var rcStats = document.querySelector('#linkPane .lk-stats');
  var rcStatusSaved = null;
  var rcStatusH = 0;
  function rcPanelExtra(hide) {
    if (rcStats) rcStats.hidden = !!hide;
    var paneEl = document.getElementById('linkPane');
    if (paneEl) paneEl.classList.toggle('rcmode', !!hide);
    var st = q('lkStatus');
    if (!st) return;
    if (hide) {
      if (rcStatusSaved === null) {
        rcStatusSaved = st.innerHTML;
        if (rcStatusH > 0) { st.style.height = rcStatusH + 'px'; st.style.overflowY = 'auto'; }
      }
      var html = '';
      for (var hi = 0; hi < RC_FIELDS.length; hi++) {
        html += '<div>' + RC_FIELDS[hi].label + '：' + RC_FIELDS[hi].hint + '</div>';
      }
      st.innerHTML = html;
      st.scrollTop = 0;
    } else {
      st.style.height = '';
      st.style.overflowY = '';
      rcStatusH = 0;
      if (rcStatusSaved !== null) {
        st.innerHTML = rcStatusSaved;
        rcStatusSaved = null;
      }
    }
  }
  function rcOpen() {
    rcOrig = rcSnapshot();
    rcOrigApplied = c.linkRecog
      ? { interval: c.linkRecog.interval, gray_offset: c.linkRecog.gray_offset,
          black_offset: c.linkRecog.black_offset, white_offset: c.linkRecog.white_offset,
          black_percent: c.linkRecog.black_percent, white_percent: c.linkRecog.white_percent }
      : rcSnapshot();
    var st0 = q('lkStatus');
    rcStatusH = (st0 && st0.offsetHeight) ? Math.round(st0.offsetHeight * 0.75) : 0;
    rcPaint();
    if (lkFnBox) lkFnBox.hidden = true;
    if (rcBox) rcBox.hidden = false;
    rcPanelExtra(true);
    rcLive();
  }
  function rcClose() {
    if (rcBox) rcBox.hidden = true;
    if (lkFnBox) lkFnBox.hidden = false;
    lkSlotClose();
    lkNameClose();
    rcPanelExtra(false);
  }
  for (var fi = 0; fi < RC_FIELDS.length; fi++) {
    (function (f) {
      var s = q(f.id + '_s'), t = q(f.id);
      if (s && t) s.addEventListener('input', function () {
        var v = parseInt(s.value, 10);
        if (isNaN(v)) return;
        rcState[f.key] = v; t.value = String(v); t.__rcPrev = v; rcLive();
      });
      if (!t) return;
      t.addEventListener('focus', function () { t.__rcPrev = rcState[f.key]; });
      t.addEventListener('input', function () {
        var v = t.value.replace(/[^0-9]/g, '');
        if (v !== t.value) t.value = v;
      });
      function rcCommit() {
        var v = parseInt(t.value, 10);
        var lo = +t.getAttribute('data-min'), hi = +t.getAttribute('data-max');
        if (isNaN(v) || v < lo || v > hi) {
          t.value = String(t.__rcPrev != null ? t.__rcPrev : rcState[f.key]);
          return;
        }
        rcState[f.key] = v; t.value = String(v); t.__rcPrev = v;
        if (s) s.value = String(v);
        rcLive();
      }
      t.addEventListener('change', rcCommit);
      t.addEventListener('blur', rcCommit);
    })(RC_FIELDS[fi]);
  }
  if (q('lk_rcparam')) q('lk_rcparam').onclick = rcOpen;
  var RC_GENERAL = { interval: 200, gray_offset: 50, black_offset: 96,
                     white_offset: 96, black_percent: 33, white_percent: 33 };
  if (q('lk_rcGeneral')) q('lk_rcGeneral').onclick = function () {
    for (var gk in RC_GENERAL) rcState[gk] = RC_GENERAL[gk];
    rcPaint();
    rcLive();
  };
  if (q('lk_rcApply')) q('lk_rcApply').onclick = function () {
    c.linkRecog = rcSnapshot();
    lkSaveCfg(c);
    tellHost({ type: 'linkRecogCfg', recog: c.linkRecog });
    rcClose();
    var stRc = document.getElementById('lkStatus');
    if (stRc) stRc.textContent = lkL('识别参数已应用', 'Recognition params applied');
  };
  if (q('lk_rcCancel')) q('lk_rcCancel').onclick = function () {
    rcState = { interval: rcOrig.interval, gray_offset: rcOrig.gray_offset,
                black_offset: rcOrig.black_offset, white_offset: rcOrig.white_offset,
                black_percent: rcOrig.black_percent, white_percent: rcOrig.white_percent };
    rcPaint();
    tellHost({ type: 'linkRecogCancel', recog: rcOrigApplied });
    rcClose();
  };

  // ═══════════════════════════════════════════════════════════════════════════════════════
  // ═══════════════════════════════════════════════════════════════════════════════════════
  var LK_SLOTS_KEY = 'gbcalc.linkrecog.slots';
  var LK_SLOTS_MAX = 50;
  var LK_RC_KEYS = ['interval', 'gray_offset', 'black_offset',
                    'white_offset', 'black_percent', 'white_percent'];
  var lkSlots = [], lkSlotSel = {}, lkSlotUndo = [];
  var slotDrawer = q('lkSlotDrawer'), slotMask = q('lkSlotMask');
  function lkSlotsFromLocal() {
    try {
      var a = JSON.parse(localStorage.getItem(LK_SLOTS_KEY) || '[]');
      if (a && typeof a.length === 'number') return a;
    } catch (e) {}
    return [];
  }
  function lkSlotRecog(sl) {
    var r = (sl && sl.recog) || {}, o = {};
    for (var i = 0; i < LK_RC_KEYS.length; i++) {
      var k = LK_RC_KEYS[i];
      o[k] = (typeof r[k] === 'number') ? (r[k] | 0) : RC_GENERAL[k];
    }
    return o;
  }
  function lkSlotsStore() {
    if (lkSlots.length > LK_SLOTS_MAX) lkSlots = lkSlots.slice(0, LK_SLOTS_MAX);
    try { localStorage.setItem(LK_SLOTS_KEY, JSON.stringify(lkSlots)); } catch (e) {}
    tellHost({ type: 'linkRecogSlots', json: JSON.stringify(lkSlots) });
  }
  function lkEsc(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
                    .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  }
  function lkSlotTime(ts) {
    var d = new Date(ts), p = function (v) { return (v < 10 ? '0' : '') + v; };
    return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' ' +
           p(d.getHours()) + ':' + p(d.getMinutes());
  }
  function lkSlotSelNames() {
    var out = [];
    for (var i = 0; i < lkSlots.length; i++) if (lkSlotSel[lkSlots[i].name]) out.push(lkSlots[i].name);
    return out;
  }
  function lkSlotHas(name) {
    for (var i = 0; i < lkSlots.length; i++) if (lkSlots[i].name === name) return true;
    return false;
  }
  function lkSlotPaint() {
    var list = q('lkSlotList');
    if (!list) return;
    var sel = lkSlotSelNames();
    var html = '';
    if (!lkSlots.length) {
      html = '<div class="lk-dr-empty">' + lkL(
        '还没有存档 —— 先把参数调好，点「存档」命名保存（最多 50 个）',
        'No presets yet - tune the params, then press Save (max 50)') + '</div>';
    } else {
      for (var i = 0; i < lkSlots.length; i++) {
        var sl = lkSlots[i];
        html += '<div class="lk-dr-item' + (lkSlotSel[sl.name] ? ' on' : '') +
                '" data-name="' + lkEsc(sl.name) + '" title="' + lkEsc(sl.name) + '">' +
                  '<span class="lk-dr-ck">✓</span>' +
                  '<span class="lk-dr-nm">' + lkEsc(sl.name) + '</span>' +
                  '<span class="lk-dr-tm">' + lkEsc(sl.ts ? lkSlotTime(sl.ts) : '') + '</span>' +
                '</div>';
      }
    }
    list.innerHTML = html;
    [].forEach.call(list.querySelectorAll('.lk-dr-item'), function (it) {
      var nm = it.getAttribute('data-name'), hit = 0;
      it.onclick = function () {
        if (hit) { clearTimeout(hit); hit = 0; return; }
        hit = setTimeout(function () { hit = 0; lkSlotToggle(nm); }, 190);
      };
      it.ondblclick = function (ev) {
        if (ev) ev.preventDefault();
        if (hit) { clearTimeout(hit); hit = 0; }
        lkSlotApply(nm);
      };
    });
    var ai = q('lk_slotAll');
    if (ai) ai.textContent = (lkSlots.length && sel.length === lkSlots.length)
      ? lkL('取消全选', 'Clear') : lkL('全选', 'All');
    var info = q('lk_slotInfo');
    if (info) info.textContent = lkL('已存档 ' + lkSlots.length + '/' + LK_SLOTS_MAX +
                                     ' 个 · 已选 ' + sel.length + ' 个',
                                     lkSlots.length + '/' + LK_SLOTS_MAX + ' saved · ' + sel.length + ' selected');
  }
  function lkSlotToggle(name) {
    if (lkSlotSel[name]) delete lkSlotSel[name];
    else lkSlotSel[name] = 1;
    lkSlotPaint();
  }
  function lkSlotOpen() { lkSlotSel = {}; lkSlotUndo = []; lkSlotPaint(); if (slotMask) slotMask.hidden = false; if (slotDrawer) slotDrawer.hidden = false; }
  function lkSlotClose() { if (slotDrawer) slotDrawer.hidden = true; if (slotMask) slotMask.hidden = true; }
  function lkSlotAll() {
    var all = lkSlots.length > 0 && lkSlotSelNames().length === lkSlots.length;
    lkSlotSel = {};
    if (!all) for (var i = 0; i < lkSlots.length; i++) lkSlotSel[lkSlots[i].name] = 1;
    lkSlotPaint();
  }
  function lkSlotApply(name) {
    var st = q('lkStatus'), nm = name || '';
    if (!nm) {
      var names = lkSlotSelNames();
      if (names.length !== 1) {
        if (st) st.textContent = lkL('请先选中 1 个存档，再点「加载」',
                                     'Select exactly one preset, then press Load');
        return;
      }
      nm = names[0];
    }
    var r = null;
    for (var i = 0; i < lkSlots.length; i++) if (lkSlots[i].name === nm) r = lkSlotRecog(lkSlots[i]);
    if (!r) { if (st) st.textContent = lkL('找不到这个存档', 'Preset not found'); return; }
    rcState = { interval: r.interval, gray_offset: r.gray_offset, black_offset: r.black_offset,
                white_offset: r.white_offset, black_percent: r.black_percent,
                white_percent: r.white_percent };
    rcPaint(); rcLive();
    lkSlotClose();
    if (st) st.textContent = lkL('已加载「' + nm + '」并即时生效', 'Loaded preset "' + nm + '" (live)');
  }
  function lkSlotDel() {
    var st = q('lkStatus'), names = lkSlotSelNames();
    if (!names.length) {
      if (st) st.textContent = lkL('请先选中要删除的存档', 'Select the presets to delete first');
      return;
    }
    var batch = [];
    lkSlots = lkSlots.filter(function (sl) {
      if (lkSlotSel[sl.name]) { batch.push(sl); return false; }
      return true;
    });
    lkSlotUndo.push(batch);
    lkSlotSel = {};
    lkSlotsStore(); lkSlotPaint();
    if (st) st.textContent = lkL('已删除 ' + batch.length + ' 个存档（点「撤销」可找回）',
                                 'Deleted ' + batch.length + ' preset(s) - Undo restores them');
  }
  function lkSlotUndoFn() {
    var st = q('lkStatus');
    if (!lkSlotUndo.length) { if (st) st.textContent = lkL('没有可撤销的删除', 'Nothing to undo'); return; }
    var batch = lkSlotUndo.pop(), back = 0;
    for (var i = 0; i < batch.length; i++) {
      if (!lkSlotHas(batch[i].name) && lkSlots.length < LK_SLOTS_MAX) { lkSlots.push(batch[i]); back++; }
    }
    lkSlotsStore(); lkSlotPaint();
    if (st) st.textContent = lkL('已撤销删除（恢复 ' + back + ' 个存档）', 'Undo done (' + back + ' restored)');
  }
  var LK_NAME_CJK = /[\u2e80-\u9fff\uf900-\ufaff\uff00-\uffef\u3000-\u303f]/;
  function lkNameClip(s) {
    s = String(s == null ? '' : s);
    var lim = LK_NAME_CJK.test(s) ? 20 : 45;
    return s.length > lim ? s.slice(0, lim) : s;
  }
  function lkDefaultName() {
    for (var n = 1; n <= LK_SLOTS_MAX + 1; n++) {
      var nm = 'Parameter ' + n;
      if (!lkSlotHas(nm)) return nm;
    }
    return 'Parameter ' + (lkSlots.length + 1);
  }
  function lkNameOpen() {
    var box = q('lkNameMask'), inp = q('lk_nameInp');
    var tt = q('lk_nameTtl'), ok = q('lk_nameOk'), no = q('lk_nameNo');
    if (tt) tt.textContent = lkL('存档命名', 'Preset Name');
    if (ok) ok.textContent = lkL('确定', 'OK');
    if (no) no.textContent = lkL('取消', 'Cancel');
    if (inp) { inp.maxLength = 45; inp.value = lkDefaultName(); }
    if (box) box.hidden = false;
    setTimeout(function () { try { if (inp) { inp.focus(); inp.select(); } } catch (e) {} }, 30);
  }
  function lkNameClose() { var b = q('lkNameMask'); if (b) b.hidden = true; }
  function lkNameOk() {
    var st = q('lkStatus'), inp = q('lk_nameInp');
    var nm = lkNameClip(((inp && inp.value) || '').replace(/^\s+|\s+$/g, ''));
    if (!nm) nm = lkDefaultName();
    var rec = rcSnapshot(), hit = false;
    for (var i = 0; i < lkSlots.length; i++) {
      if (lkSlots[i].name === nm) { lkSlots[i].recog = rec; lkSlots[i].ts = Date.now(); hit = true; break; }
    }
    if (!hit && lkSlots.length >= LK_SLOTS_MAX) {
      if (st) st.textContent = lkL('存档已满（最多 50 个）—— 请先删掉一些，或换个名字覆盖同名存档',
                                   'Preset list full (max 50) - delete some or overwrite a name');
      lkNameClose(); return;
    }
    if (!hit) lkSlots.unshift({ name: nm, recog: rec, ts: Date.now() });
    lkNameClose();
    lkSlotsStore();
    if (slotDrawer && !slotDrawer.hidden) lkSlotPaint();
    if (st) st.textContent = lkL('已存档「' + nm + '」（共 ' + lkSlots.length + ' 个）',
                                 'Saved preset "' + nm + '" (' + lkSlots.length + ' total)');
  }
  if (q('lk_rcSlotSave')) q('lk_rcSlotSave').onclick = function () { lkNameOpen(); };
  if (q('lk_rcSlotLoad')) q('lk_rcSlotLoad').onclick = function () {
    if (slotDrawer && !slotDrawer.hidden) lkSlotClose();
    else lkSlotOpen();
  };
  if (slotMask) slotMask.onclick = lkSlotClose;
  if (q('lk_slotPick')) q('lk_slotPick').onclick = function () { lkSlotApply(); };
  if (q('lk_slotAll')) q('lk_slotAll').onclick = lkSlotAll;
  if (q('lk_slotDel')) q('lk_slotDel').onclick = lkSlotDel;
  if (q('lk_slotUndo')) q('lk_slotUndo').onclick = lkSlotUndoFn;
  if (q('lk_slotX')) q('lk_slotX').onclick = lkSlotClose;
  if (q('lk_nameOk')) q('lk_nameOk').onclick = lkNameOk;
  if (q('lk_nameNo')) q('lk_nameNo').onclick = lkNameClose;
  if (q('lkNameMask')) q('lkNameMask').onclick = function (ev) { if (ev && ev.target === this) lkNameClose(); };
  if (q('lk_nameInp')) {
    q('lk_nameInp').addEventListener('input', function () {
      var v = lkNameClip(this.value);
      if (v !== this.value) this.value = v;
    });
    q('lk_nameInp').addEventListener('keydown', function (ev) {
      if (ev.key === 'Enter') { ev.preventDefault(); lkNameOk(); }
      else if (ev.key === 'Escape') { ev.preventDefault(); lkNameClose(); }
    });
  }
  lkSlots = lkSlotsFromLocal();

  window.__lkRcOpen = rcOpen;
  window.__lkRcPaint = rcPaint;

  [].forEach.call(segBtns, function (b) {
    b.onclick = function () {
      c.rule = +b.getAttribute('data-rule'); lkSaveCfg(c); ruleSync(); lkSendCfg();
      lkBridgeRule(c.rule);                    
      saveRuleGlobal(c.rule);
      if (!lkFramedNow()) lkRemindFrame();     
    };
  });
  
  q('lk_bal').onclick = function () {
    
    if (!lkFramedNow()) { lkRemindFrame(); return; }
    c.aiB = false; c.aiW = false;
    lkSaveCfg(c); lkSyncTgl(); lkSendCfg();      
    var sb = document.getElementById('__gb_swbal');
    if (sb) {
      
      sb.click();
    } else {
      lkBridgeSide(-1);                          
      if (typeof linkBalanceSwap === 'function') linkBalanceSwap();
    }
  };
  
  q('lk_eval').onclick = function () {
    if (!lkFramedNow()) { lkRemindFrame(); return; }   
    c.aiB = false; c.aiW = false;
    lkSaveCfg(c); lkSyncTgl(); lkSendCfg();
    var se = document.getElementById('__gb_sweval');
    if (se) se.click();
    else linkBalanceEval();
  };

  if (q('lk_swxcancel')) q('lk_swxcancel').onclick = function () {
    var ok = false;
    if (typeof window.GB_LINK_SWAPCANCEL === 'function') {
      try { window.GB_LINK_SWAPCANCEL(); ok = true; } catch (eSc) {
        try { console.warn('[link] swap cancel failed:', eSc); } catch (eSc2) {}
      }
    }
    var sc = document.getElementById('lkStatus');
    if (sc) {
      sc.textContent = ok
        ? lkL('已取消交换平衡计算，平衡标记与待落点已清除',
              'Swap-balance cancelled; marks and pending moves cleared')
        : lkL('当前没有进行中的交换平衡计算', 'No swap-balance calculation is running');
    }
  };
  
  (function lkBridgeSetup() {
    window.GB_STANDALONE = true;               
    var BS = window.GB_S = (window.GB_S || {});   
    BS.lang = S.lang; BS.dark = (S.theme !== 'light');
    BS.rule = (c.rule >= 0 && c.rule <= 2) ? c.rule : 0;
    BS.customTurnSec = Math.round((c.turnMs >= 200 ? c.turnMs : 2000) / 100) / 10;
    
    BS.turnMs = (c.turnMs >= 200 ? c.turnMs : 2000); BS.customSec = 600; BS.hashMB = 0;
    try {
      var _bst = JSON.parse(localStorage.getItem('gbcalc.settings.v1') || '{}');
      if (_bst && typeof _bst.bgThink === 'boolean') BS.bgThink = _bst.bgThink;
    } catch (eBT) {}
    if (typeof BS.bgThink !== 'boolean') BS.bgThink = false;
    BS.side = -1; BS.heatmap = false;          
    BS.history = []; BS.candCache = []; BS.myEval = '-'; BS.rawEval = '';
    
    var bh = document.createElement('div');
    bh.id = '__gb_panel';
    bh.style.cssText = 'position:fixed;left:-9999px;top:0;width:1px;height:1px;overflow:hidden;';
    bh.innerHTML =
      '<canvas id="__gb_frame" width="8" height="8"></canvas>' +
      '<div id="__gb_hdr"></div><div id="__gb_body"></div><div id="__gb_bottombar"></div>' +
      '<span id="__gb_side_black"></span><span id="__gb_side_white"></span><span id="__gb_side_txt"></span>' +
      '<button id="__gb_autodrop"></button><button id="__gb_adcount"></button>' +
      
      '<button id="__gb_realclick"></button>' +
      '<button id="__gb_swbal"></button><select id="__gb_balpoint" class="sel"><option value="1">1</option><option value="2" selected>2</option><option value="3">3</option><option value="4">4</option><option value="5">5</option><option value="6">6</option></select><button id="__gb_sweval"></button>' +
      '<span id="__gb_st_depth"></span><span id="__gb_st_eval"></span><span id="__gb_st_speed"></span>' +
      '<span id="__gb_st_nodes"></span><span id="__gb_st_line"></span>' +
      '<button data-rule="0"></button><button data-rule="1"></button><button data-rule="2"></button>';
    document.body.appendChild(bh);
    
    var lastBestKey = '';
    window.GBUI = {
      T: function (k) {
        var m = { connecting: lkL('连接引擎…', 'Connecting engine…'),
                  black: lkL('黑', 'Black'), white: lkL('白', 'White'),
                  no_board: lkL('未检测到棋盘', 'No board detected') };
        return m[k] || k;
      },
      initState: function () {                 
        try {
          var sv = JSON.parse(localStorage.getItem('gbcalc.assist.s') || '{}');
          for (var k in sv) {
            if (!BS.hasOwnProperty(k)) continue;
            
            if (k === 'lang' || k === 'dark') continue;
            if (k === 'rule') continue;
            BS[k] = sv[k];
          }
        } catch (e) {}
      },
      save: function () {
        try { localStorage.setItem('gbcalc.assist.s', JSON.stringify({
          side: BS.side, rule: BS.rule, customTurnSec: BS.customTurnSec,
          customSec: BS.customSec, hashMB: BS.hashMB, dark: BS.dark, lang: BS.lang })); }
        catch (e) {}
      },
      applyPreset: function () { BS.turnMs = Math.round((BS.customTurnSec || 2) * 1000);
                                 BS.matchMs = Math.round((BS.customSec || 600) * 1000); },
      setStatPanel: function (c0) {            
        function put(id, v) { var el = document.getElementById(id); if (el) el.textContent = v; }
        put('lk_s_depth', c0 && c0.depth != null ? c0.depth : '-');
        put('lk_s_speed', c0 && c0.speed != null ? c0.speed : '-');
        put('lk_s_nodes', c0 && c0.nodes != null ? c0.nodes : '-');
        put('lk_s_eval', BS.myEval || '-');
      },
      drawChart: function () {                 
        try {
          var c0 = (BS.candCache && BS.candCache[0]) || null;
          var key = c0 ? (c0.x + ',' + c0.y) : '';
          if (key !== lastBestKey) {
            lastBestKey = key;
            LK.best = c0 ? { x: c0.x, y: c0.y } : null;
            if (typeof lkPaintBoard === 'function') lkPaintBoard();
          }
        } catch (e) {}
      },
      setN: function () {}, applyDark: function () {}, applyLang: function () {},
      applyOpacity: function () {}, applyBlur: function () {}, applyHash: function () {},
      paintHeat: function () {}, paintOpp: function () {}, paintSel: function () {},
      paintToggle: function () {},
      setStatus: function (html) {             
        var el = document.getElementById('lkStatus');
        if (el) el.innerHTML = String(html == null ? '' : html);
      },
    };
    
    var sc = document.createElement('script');
    sc.src = 'bridge.js';
    sc.onload = function () {                  
      try {
        lkBridgeSide(c.aiB === true ? 0 : (c.aiW === true ? 1 : -1));
        lkBridgeAuto(c.auto === true);
        lkBridgeRule((c.rule >= 0 && c.rule <= 2) ? c.rule : 0);
        var srcAC = document.getElementById('__gb_adcount'), dstAC = document.getElementById('lk_adcount');
        if (srcAC && dstAC) {
          dstAC.textContent = srcAC.textContent;
          new MutationObserver(function () { dstAC.textContent = srcAC.textContent; })
            .observe(srcAC, { childList: true, characterData: true, subtree: true });
          dstAC.onclick = function () { srcAC.click(); };
        }
        
        var srcAD = document.getElementById('__gb_autodrop'), dstAD = document.getElementById('lk_auto');
        if (srcAD && dstAD) {
          var syncAD = function () {
            var raw = srcAD.getAttribute('data-ad-on');
            if (raw !== '0' && raw !== '1') return;      
            var on = (raw === '1');
            dstAD.classList.toggle('on', on);
            
            if ((c.auto === true) !== on) { c.auto = on; lkSaveCfg(c); }
          };
          new MutationObserver(syncAD)
            .observe(srcAD, { attributes: true, attributeFilter: ['data-ad-on'] });
          syncAD();
        }
      } catch (e) {}
    };
    sc.onerror = function () {
      var el = document.getElementById('lkStatus');
      if (el) el.textContent = lkL('助手算法核心（bridge.js）加载失败', 'Failed to load bridge.js');
    };
    document.head.appendChild(sc);
  })();
  
  if (window.__lkSyncReal) window.__lkSyncReal();
  
  function lkBridgeSide(side) {
    var BS = window.GB_S;
    if (!BS) return;
    if (side === -1) {
      if (BS.side === 0 || BS.side === 1) {
        var cur = document.getElementById(BS.side === 0 ? '__gb_side_black' : '__gb_side_white');
        if (cur) cur.click();
      }
      return;
    }
    var el = document.getElementById(side === 0 ? '__gb_side_black' : '__gb_side_white');
    if (el && BS.side !== side) el.click();
  }
  function lkBridgeAuto(on) {
    var b = document.getElementById('__gb_autodrop');
    if (b && (b.getAttribute('data-ad-on') === '1') !== !!on) b.click();
  }
  function lkBridgeRule(rule) {
    var BS = window.GB_S;
    if (!BS || BS.rule === rule) return;
    BS.rule = rule;
    var b = document.querySelector('#__gb_panel [data-rule="' + rule + '"]');
    if (b) b.click();                          
  }
  var turnIn = q('lk_turn');
  if (turnIn) {
    turnIn.value = String((c.turnMs >= 200 && c.turnMs <= 300000) ? (c.turnMs / 1000) : 2);
    turnIn.onchange = function () {
      var v = parseFloat(turnIn.value);
      if (isNaN(v) || v < 0.2) v = 2;
      if (v > 300) v = 300;
      c.turnMs = Math.round(v * 1000); lkSaveCfg(c); lkSendCfg();
      
      if (window.GB_S) {
        window.GB_S.customTurnSec = Math.round(v * 100) / 100;
        window.GB_S.turnMs = Math.round(v * 1000);
      }
      var st2 = document.getElementById('lkStatus');
      if (st2) st2.textContent = lkL('思考时间已设为 ' + v + ' 秒', 'Think time set to ' + v + ' s');
    };
  }

  try {
    prefsApplyToS();
    initPanels();
    applyLayout();
    wireColors();
    wireCards();
    wireAnalysis();
    try { syncPinKeys(); } catch (e1) {}
  } catch (ePanel) {}

  lkStatus(); lkSyncCount(); lkPaintBoard(); lkPaintCurve();
  bootDone();                        
  if (HOST) {
    window.chrome.webview.addEventListener('message', function (ev) {
      tellHostDefer = true;
      try { onLinkHostMsg(ev); } finally { tellHostDefer = false; }
    });
    window.chrome.webview.postMessage({ type: 'ready', theme: themeKey(), bg: pageBgHex(), lang: S.lang });
    lkSendCfg();                     
  }

  function onLinkHostMsg(ev) {
    var m = ev.data; if (!m) return;
    if (m.type === 'uiTheme') {
      
      if (m.lang === 'zh' || m.lang === 'en') { S.lang = m.lang; }      if (m.theme === 'light' || m.theme === 'dark') { S.theme = m.theme; document.body.setAttribute('data-theme', S.theme); }
      var BSS = window.GB_S;
      if (BSS) { BSS.lang = S.lang; BSS.dark = (S.theme !== 'light'); }
      lkStatus(); lkSyncCount(); lkPaintBoard(); lkPaintCurve();
      var lkBe = document.getElementById('lkBrandEl'); if (lkBe) lkBe.innerHTML = lkBrandHTML();
      return;
    }
    if (m.type === 'linkState') {
      LK.frame = m.frame || LK.frame;
      
      if (LK.frame === 'locked') LK.detected = true;
      lkStatus();
      lkPaintBoard();
      return;
    }
    if (m.type === 'linkBoard') {
      var bd = newBoard();
      (m.black || []).forEach(function (p) { if (p && bd[p[1]]) bd[p[1]][p[0]] = 1; });
      (m.white || []).forEach(function (p) { if (p && bd[p[1]]) bd[p[1]][p[0]] = 2; });
      LK.board = bd;
      LK.b = m.b | 0; LK.w = m.w | 0;
      LK.frame = 'locked';
      LK.detected = true;                 
      lkPaintBoard(); lkSyncCount(); lkStatus();
      return;
    }
    if (m.type === 'linkEval' && m.curve) {
      LK.curve = m.curve;
      lkPaintCurve();
      return;
    }
    if (m.type === 'linkCfgSync') {
      if (m.rule >= 0 && m.rule <= 2) {
        c.rule = m.rule; ruleSync(); lkBridgeRule(m.rule);
        saveRuleGlobal(m.rule);
      }
      return;
    }
    if (m.type === 'linkRecog' && m.recog) {
      var rr = m.recog;
      if (typeof rr.interval === 'number')      rcState.interval = rr.interval | 0;
      if (typeof rr.gray_offset === 'number')   rcState.gray_offset = rr.gray_offset | 0;
      if (typeof rr.black_offset === 'number')  rcState.black_offset = rr.black_offset | 0;
      if (typeof rr.white_offset === 'number')  rcState.white_offset = rr.white_offset | 0;
      if (typeof rr.black_percent === 'number') rcState.black_percent = rr.black_percent | 0;
      if (typeof rr.white_percent === 'number') rcState.white_percent = rr.white_percent | 0;
      c.linkRecog = { interval: rcState.interval, gray_offset: rcState.gray_offset,
                      black_offset: rcState.black_offset, white_offset: rcState.white_offset,
                      black_percent: rcState.black_percent, white_percent: rcState.white_percent };
      lkSaveCfg(c);
      rcPaint();
      return;
    }
    if (m.type === 'linkRecogSlots' && typeof m.json === 'string') {
      var arr = [];
      try { arr = JSON.parse(m.json) || []; } catch (eS) { arr = []; }
      if (!(arr && typeof arr.length === 'number')) arr = [];
      if (arr.length) lkSlots = arr.slice(0, LK_SLOTS_MAX);
      else {
        var loc = lkSlotsFromLocal();
        lkSlots = loc.slice(0, LK_SLOTS_MAX);
        if (lkSlots.length) lkSlotsStore();
      }
      if (slotDrawer && !slotDrawer.hidden) lkSlotPaint();
      return;
    }
    if (m.type === 'linkBalanceDone') {
      var st3 = document.getElementById('lkStatus');
      if (st3 && m.kind === 'bal1')
        st3.textContent = lkL(LK_BAL_TXT.riffy[0] + '  ' + (m.move || '') + '  ' + (m.ev || ''),
                              LK_BAL_TXT.riffy[1] + '  ' + (m.move || '') + '  ' + (m.ev || ''));
      else if (st3 && m.kind === 'bal2')
        st3.textContent = lkL(LK_BAL_TXT.done2[0] + ' ' + (m.move || '') + '  ' + (m.ev || ''),
                              LK_BAL_TXT.done2[1] + ' ' + (m.move || '') + '  ' + (m.ev || ''));
      
      lkPaintBoard();
      return;
    }
    if (m.type === 'linkBalanceRun') {
      var st4 = document.getElementById('lkStatus');
      if (st4) st4.textContent = lkL(LK_BAL_TXT.run[0], LK_BAL_TXT.run[1]);
      return;
    }
    if (m.type === 'linkStats') {
      var sMap = { depth: 'lk_s_depth', speed: 'lk_s_speed', nodes: 'lk_s_nodes' };
      for (var sk in sMap) {
        var elv = document.getElementById(sMap[sk]);
        if (elv && m[sk] != null && m[sk] !== '') elv.textContent = m[sk];
      }
      
      if (m.st != null && m.st !== '') {
        var stEl = document.getElementById('lkStatus');
        if (stEl) stEl.textContent = m.st;
        lkStOverride = Date.now();   
        
      }
      
      if (m.auto === false && c.auto === true) {
        c.auto = false; lkSaveCfg(c);
        var laEl = document.getElementById('lk_auto');
        if (laEl) laEl.classList.remove('on');
      }
      
      if (typeof m.best === 'string' && /^[a-o](?:[1-9]|1[0-5])$/.test(m.best.trim())) {
        var bt = m.best.trim();
        LK.best = { x: bt.charCodeAt(0) - 97, y: 15 - parseInt(bt.slice(1), 10) };
        lkPaintBoard();
      } else if (m.best === '') {
        LK.best = null;
        lkPaintBoard();
      }
      return;
    }
    if (m.type === 'autodropDone') {
      var el2 = document.getElementById('lkStatus');
      if (el2) el2.textContent = lkL('自动落子已点击 (' + m.x + ',' + m.y + ') × ' + m.count,
                                     'Dropped (' + m.x + ',' + m.y + ') x ' + m.count);
      return;
    }
  }
}
