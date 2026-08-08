'use strict';

// 调试辅助：?noanim 时禁用 CSS 动画（仅用于自动化截图验证，正常启动器不受影响）
if (location.search.indexOf('noanim') >= 0) {
  const style = document.createElement('style');
  style.textContent = '*,*::before,*::after{animation:none!important;transition:none!important}';
  document.head.appendChild(style);
}

/* ---------- 与 C++ 宿主的桥接 ---------- */

const bridge = (window.chrome && window.chrome.webview) ? window.chrome.webview : null;
const DEMO = !bridge;

function post(cmd, payload) {
  if (!bridge) return;
  try {
    const msg = Object.assign({ type: cmd }, payload || {});
    bridge.postMessage(msg);
  } catch (err) { /* 忽略桥接异常 */ }
}

const $ = (id) => document.getElementById(id);

/* ---------- 全局 UI 状态 ---------- */

const ui = {
  status: '就绪',
  localVersion: '未知',
  gamePath: '—',
  launcherVersion: 'v1.0.0',
  busy: false,
  progressShow: false,
  progress: 0,
  progressTotal: 0,
  progressFile: '',
  downloading: false,
  pending: [],
  pendingText: '',
  checked: false,
  gameInstalled: false,
  gameRunning: false,
  opMode: 0,
  frames: [],
  fps: 12,
  settings: {
    gamePath: '',
    serverUrl: '',
    speed: 0,
    frameDir: 'Background',
    fps: 12,
    autoCheck: true,
    autoRepair: false,
  },
};

/* ---------- 背景序列帧（Canvas 播放：帧未就绪保持上一帧，绝不黑屏） ---------- */

let frameIdx = 0;
let frameTimer = null;
let bgRaf = 0;

function setupFrames(urls, fps) {
  const canvas = $('bgSeq');
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  const frames = Array.isArray(urls) ? urls : [];
  const frameMs = 1000 / Math.max(1, Number(fps) || 24);
  const dpr = Math.min(window.devicePixelRatio || 1, 2);

  if (frameTimer) { clearInterval(frameTimer); frameTimer = null; }
  if (bgRaf) { cancelAnimationFrame(bgRaf); bgRaf = 0; }
  ui.frames = frames;
  ui.fps = Math.max(1, Number(fps) || 24);

  const resize = () => {
    canvas.width = Math.floor(innerWidth * dpr);
    canvas.height = Math.floor(innerHeight * dpr);
  };
  resize();
  window.addEventListener('resize', resize);

  const imgs = [];
  frames.forEach((src, i) => {
    const img = new Image();
    img.onload = () => {};
    img.onerror = () => {};
    img.src = src;
    imgs[i] = img;
  });

  const draw = (i) => {
    const img = imgs[i];
    if (!img || !img.complete || img.naturalWidth === 0 || img.naturalHeight === 0) return false;
    const cw = canvas.width;
    const ch = canvas.height;
    ctx.clearRect(0, 0, cw, ch);
    const scale = Math.max(cw / img.naturalWidth, ch / img.naturalHeight);
    const dw = img.naturalWidth * scale;
    const dh = img.naturalHeight * scale;
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = 'high';
    ctx.drawImage(img, (cw - dw) / 2, (ch - dh) / 2, dw, dh);
    return true;
  };

  let current = -1;
  let lastSwitch = 0;
  const showFirstReady = () => {
    for (let i = 0; i < imgs.length; i++) {
      if (draw(i)) {
        current = i;
        frameIdx = i;
        return true;
      }
    }
    return false;
  };

  const tick = (now) => {
    if (current < 0) {
      if (showFirstReady()) lastSwitch = now;
    } else if (imgs.length > 1 && now - lastSwitch >= frameMs) {
      // 到点切下一帧；下一帧未就绪则保持当前帧
      lastSwitch += frameMs;
      const next = (current + 1) % imgs.length;
      if (draw(next)) {
        current = next;
        frameIdx = next;
      }
    }
    bgRaf = requestAnimationFrame(tick);
  };

  if (frames.length) {
    showFirstReady();
    bgRaf = requestAnimationFrame(tick);
  }
}

/* ---------- 渲染 ---------- */

function renderStatus(text) {
  ui.status = text || '就绪';
}

function renderBusy(busy, mode) {
  ui.busy = !!busy;
  ui.opMode = Number(mode) || 0;
  $('btnLaunch').disabled = ui.busy;
  document.querySelectorAll('.toolBtn[data-action]').forEach((b) => {
    b.disabled = ui.busy;
  });
  $('btnCancel').classList.toggle('hidden', !ui.busy);
  updateLaunchButton();
}

let dlSpeed = 0, dlLastBytes = 0, dlLastTime = 0, lastDlRender = 0;

function trackDownloadSpeed(doneBytes) {
  const now = performance.now();
  if (doneBytes <= 0) {
    dlSpeed = 0; dlLastBytes = 0; dlLastTime = 0;
    lastDlRender = 0;
    return;
  }
  if (dlLastTime) {
    const dt = (now - dlLastTime) / 1000;
    const db = doneBytes - dlLastBytes;
    if (dt > 0 && db >= 0) {
      const inst = db / dt;
      dlSpeed = dlSpeed ? dlSpeed * 0.7 + inst * 0.3 : inst;
    }
  }
  dlLastBytes = doneBytes;
  dlLastTime = now;
}

function fmtSpeed(bps) {
  if (!bps || bps <= 0) return '';
  if (bps >= 1024 * 1024 * 1024) return (bps / 1024 / 1024 / 1024).toFixed(2) + ' GB/s';
  if (bps >= 1024 * 1024) return (bps / 1024 / 1024).toFixed(1) + ' MB/s';
  if (bps >= 1024) return (bps / 1024).toFixed(1) + ' KB/s';
  return bps.toFixed(0) + ' B/s';
}

function renderProgress(show, p, file, total, download, mode) {
  ui.progressShow = !!show;
  ui.progress = Math.max(0, Math.min(1, Number(p) || 0));
  ui.progressFile = file || '';
  ui.progressTotal = Number(total) || 0;
  ui.downloading = !!download && ui.progressShow;
  ui.opMode = Number(mode) || ui.opMode;

  if (ui.downloading) {
    trackDownloadSpeed(ui.progress * ui.progressTotal);
    const now = performance.now();
    if (now - lastDlRender < 100) return;  // 节流，避免高频刷新
    lastDlRender = now;
  }
  updateLaunchButton();
}

function fmtSize(bytes) {
  const n = Number(bytes) || 0;
  if (n <= 0) return '';
  if (n >= 1024 * 1024 * 1024) return (n / 1024 / 1024 / 1024).toFixed(2) + ' GB';
  if (n >= 1024 * 1024) return (n / 1024 / 1024).toFixed(1) + ' MB';
  if (n >= 1024) return (n / 1024).toFixed(1) + ' KB';
  return n + ' B';
}

function renderPending(items, text) {
  ui.pending = Array.isArray(items) ? items : [];
  ui.pendingText = text || '';
  updateLaunchButton();
}

function renderResult(ok, message) {
  // 结果通过 Toast 提示，无独立文本区
  void ok; void message;
}

function toast(message, kind) {
  const box = $('toasts');
  const t = document.createElement('div');
  t.className = 'toast ' + (kind || 'info');
  const icon = document.createElement('span');
  icon.className = 'toastIcon';
  icon.textContent = kind === 'ok' ? '✓' : kind === 'err' ? '!' : 'i';
  const text = document.createElement('span');
  text.textContent = message || '';
  t.append(icon, text);
  box.appendChild(t);
  setTimeout(() => {
    t.classList.add('out');
    setTimeout(() => t.remove(), 320);
  }, 4600);
}

/* ---------- 初始化 ---------- */

function fillSettingsForm() {
  const s = ui.settings;
  $('setGamePath').value = s.gamePath || '';
  $('setServerUrl').value = s.serverUrl || '';
  $('setSpeed').value = s.speed || 0;
  $('setFrameDir').value = s.frameDir || 'Background';
  $('setFps').value = s.fps || 12;
  $('setAutoCheck').checked = !!s.autoCheck;
  $('setAutoRepair').checked = !!s.autoRepair;
}

function applyInit(d) {
  ui.launcherVersion = 'v' + (d.launcherVersion || '1.0.0');
  ui.localVersion = d.localVersion || '未知';
  ui.gamePath = d.gamePath || '—';
  ui.checked = !!d.checked;
  ui.gameInstalled = !!d.installed;
  ui.gameRunning = !!d.gameRunning;
  $('localVersion').textContent = ui.localVersion;

  setupFrames(d.frames, d.frameFps);
  renderStatus(d.status || '就绪');
  renderBusy(d.busy);
  renderProgress(d.showProgress, d.progress, d.currentFile);
  renderPending(d.pending, d.pendingText);
  if (d.lastResult) renderResult(true, d.lastResult);
  updateLaunchButton();

  ui.settings = {
    gamePath: d.gamePath || '',
    serverUrl: d.serverUrl || '',
    speed: d.speedLimitKBps || 0,
    frameDir: d.frameDir || 'Background',
    fps: d.frameFps || 12,
    autoCheck: !!d.autoCheckOnStart,
    autoRepair: !!d.autoRepairOnStart,
  };
  fillSettingsForm();
}

function handleHost(d) {
  if (!d || typeof d.type !== 'string') return;
  switch (d.type) {
    case 'init':
      applyInit(d);
      break;
    case 'status':
      renderStatus(d.text);
      break;
    case 'busy':
      renderBusy(d.busy, d.mode);
      break;
    case 'progress':
      renderProgress(d.show, d.progress, d.file, d.total, d.download, d.mode);
      break;
    case 'pending':
      renderPending(d.items, d.text);
      break;
    case 'game_state':
      ui.gameRunning = !!d.running;
      updateLaunchButton();
      break;
    case 'result':
      ui.checked = true;
      ui.downloading = false;
      renderResult(d.ok, d.message);
      updateLaunchButton();
      toast(d.message, d.ok ? 'ok' : 'err');
      break;
    case 'folder_selected':
      if (d.path) {
        $('setGamePath').value = d.path;
        post('set_game_path', { path: d.path });
      }
      break;
    case 'settings_saved':
      toast('设置已保存', 'ok');
      break;
    default:
      break;
  }
}

if (bridge) {
  bridge.addEventListener('message', (e) => {
    let d = e.data;
    if (typeof d === 'string') {
      try { d = JSON.parse(d); } catch (err) { return; }
    }
    handleHost(d);
  });
  post('init');
}

/* ---------- 设置面板 ---------- */

function openSettings() {
  fillSettingsForm();
  $('settingsPanel').classList.add('open');
  $('settingsPanel').setAttribute('aria-hidden', 'false');
}

function closeSettings() {
  $('settingsPanel').classList.remove('open');
  $('settingsPanel').setAttribute('aria-hidden', 'true');
}

$('btnSettings').addEventListener('click', openSettings);
$('btnSettingsClose').addEventListener('click', closeSettings);
$('btnSettingsCancel').addEventListener('click', closeSettings);

$('settingsForm').addEventListener('submit', (e) => {
  e.preventDefault();
  post('settings_save', {
    gamePath: $('setGamePath').value.trim(),
    serverUrl: $('setServerUrl').value.trim(),
    speedLimitKBps: Math.max(0, Number($('setSpeed').value) || 0),
    frameDir: $('setFrameDir').value.trim() || 'Background',
    frameFps: Math.max(1, Number($('setFps').value) || 12),
    autoCheckOnStart: $('setAutoCheck').checked,
    autoRepairOnStart: $('setAutoRepair').checked,
  });
  closeSettings();
});

$('btnBrowse').addEventListener('click', () => post('browse'));

/* ---------- 窗口控制 ---------- */

$('btnWinMin').addEventListener('click', () => post('window_minimize'));
$('btnWinMax').addEventListener('click', () => post('window_maximize'));
$('btnWinClose').addEventListener('click', () => post('window_close'));

const titlebar = $('titlebar');
titlebar.addEventListener('mousedown', (e) => {
  if (e.button !== 0) return;
  if (e.target.closest('button')) return;
  e.preventDefault();
  post('window_drag');
});
titlebar.addEventListener('dblclick', (e) => {
  if (e.target.closest('button')) return;
  post('window_maximize');
});

/* ---------- 主操作 ---------- */

function updateLaunchButton() {
  const btn = $('btnLaunch');
  const label = $('launchLabel');
  const badge = $('launchBadge');
  const opLabels = { 1: '检查中', 2: '下载中', 3: '修复中', 4: '自检中', 5: '背景更新中', 6: '资源更新中' };
  const setMode = (mode) => {
    btn.classList.remove('updateMode', 'downloadMode', 'runningMode', 'progressMode');
    if (mode) btn.classList.add(mode);
    if (mode !== 'progressMode') btn.style.removeProperty('--dl-pct');
  };
  if (ui.busy) {
    btn.disabled = true;
    badge.classList.add('hidden');
    const prefix = opLabels[ui.opMode] || '处理中';
    if (ui.progressShow) {
      setMode('progressMode');
      const pct = Math.round(ui.progress * 100);
      const speedText = ui.opMode === 2 ? fmtSpeed(dlSpeed) : '';
      label.textContent = prefix + ' ' + pct + '%' + (speedText ? ' · ' + speedText : '');
      btn.style.setProperty('--dl-pct', pct + '%');
    } else {
      setMode(null);
      label.textContent = prefix + '…';
    }
    return;
  }
  btn.disabled = false;
  dlSpeed = 0; dlLastBytes = 0; dlLastTime = 0;
  if (ui.gameRunning) {
    setMode('runningMode');
    label.textContent = '关闭游戏';
    badge.classList.add('hidden');
  } else if (!ui.gameInstalled) {
    setMode('downloadMode');
    label.textContent = '下载游戏';
    badge.classList.add('hidden');
  } else if (ui.pending.length > 0) {
    setMode('updateMode');
    label.textContent = '开始更新';
    badge.textContent = ui.pending.length;
    badge.classList.remove('hidden');
  } else {
    setMode(null);
    label.textContent = '启动游戏';
    badge.classList.add('hidden');
  }
}

$('btnLaunch').addEventListener('click', () => {
  if (ui.busy) return;
  if (ui.gameRunning) post('close_game');
  else if (!ui.gameInstalled) post('download_game');
  else if (ui.pending.length > 0) post('apply');
  else post('launch');
});
$('btnCancel').addEventListener('click', () => post('cancel'));
$('btnRefresh').addEventListener('click', () => post('refresh'));

/* 修复 / 目录 下拉菜单 */
const repairMenu = $('repairMenu');
$('btnRepairMenu').addEventListener('click', (e) => {
  e.stopPropagation();
  repairMenu.classList.toggle('open');
});
repairMenu.querySelectorAll('.menuItem').forEach((item) => {
  item.addEventListener('click', (e) => {
    e.stopPropagation();
    repairMenu.classList.remove('open');
    post(item.dataset.action);
  });
});
document.addEventListener('click', () => repairMenu.classList.remove('open'));

/* ---------- 常规交互 ---------- */

document.addEventListener('contextmenu', (e) => e.preventDefault());
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape') closeSettings();
});

/* ---------- 启动 ---------- */

if (DEMO) {
  // 浏览器预览模式：模拟宿主数据，便于纯前端调试
  applyInit({
    launcherVersion: '1.0.0',
    localVersion: '1.2.3',
    gamePath: 'D:\\Games\\CodeBuild',
    installed: false,
    status: '已是最新版本',
    busy: false,
    showProgress: true,
    progress: 0.42,
    currentFile: 'CodeBuild\\Content\\Paks\\demo.pak',
    pending: [
      { versionId: '1.3.0', type: 'patch', totalSize: 345678901, date: '2026-08-06' },
      { versionId: '1.4.0', type: 'full', totalSize: 1200000000, date: '2026-08-06' },
    ],
    pendingText: '发现 2 个更新版本',
    frameFps: 12,
    serverUrl: 'http://127.0.0.1:8710',
    frameDir: 'Background',
    speedLimitKBps: 0,
    autoCheckOnStart: true,
    autoRepairOnStart: false,
  });

  // 演示下载进度与速度（仅浏览器预览）
  const demoTotal = 612993710;
  let demoDone = 0;
  const demoStart = performance.now();
  const demoInt = setInterval(() => {
    demoDone += 6 * 1024 * 1024;  // 模拟约 6MB/300ms
    if (demoDone >= demoTotal) {
      clearInterval(demoInt);
      handleHost({ type: 'busy', busy: false });
      handleHost({ type: 'progress', show: false, progress: 1, file: '', total: demoTotal, download: true });
      handleHost({ type: 'result', ok: true, message: '下载完成' });
      // 演示游戏运行状态：按钮变为“关闭游戏”，2 秒后自动退出
      handleHost({ type: 'game_state', running: true });
      setTimeout(() => {
        handleHost({ type: 'game_state', running: false });
        // 演示修复进度显示在主按钮上（无全屏弹窗）
        handleHost({ type: 'busy', busy: true, mode: 3 });
        let rep = 0;
        const repInt = setInterval(() => {
          rep += 0.08;
          if (rep >= 1) {
            clearInterval(repInt);
            handleHost({ type: 'busy', busy: false, mode: 0 });
            handleHost({ type: 'progress', show: false, progress: 1, file: '', total: 0, download: false, mode: 0 });
            handleHost({ type: 'result', ok: true, message: '修复完成' });
            return;
          }
          handleHost({
            type: 'progress',
            show: true,
            progress: rep,
            file: 'Engine\\Binaries\\x.pak',
            total: 0,
            download: false,
            mode: 3,
          });
        }, 400);
      }, 2000);
      return;
    }
    handleHost({ type: 'busy', busy: true, mode: 2 });
    handleHost({
      type: 'progress',
      show: true,
      progress: demoDone / demoTotal,
      file: 'CodeBuild-Win64-Shipping.exe',
      total: demoTotal,
      download: true,
      mode: 2,
    });
  }, 300);
}
