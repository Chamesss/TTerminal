'use strict';

const post = (msg) => window.chrome.webview.postMessage(msg);
const $ = (id) => document.getElementById(id);

const store = {
  get: (k, d) => { try { return JSON.parse(localStorage.getItem(k)) ?? d; } catch { return d; } },
  set: (k, v) => { try { localStorage.setItem(k, JSON.stringify(v)); } catch {} },
};

const term = new Terminal({
  fontFamily: '"JetBrainsMono Nerd Font", "Cascadia Code", "Cascadia Mono", Consolas, monospace',
  fontSize: store.get('fontSize', 14),
  lineHeight: 1.15,
  cursorBlink: true,
  cursorStyle: 'bar',
  cursorWidth: 2,
  allowTransparency: true,
  allowProposedApi: true,
  scrollback: 10000,
  overviewRuler: { width: 6 },
  windowsPty: { backend: 'conpty' },
  theme: {
    background: 'rgba(0,0,0,0)',
    foreground: '#c8d3f5',
    cursor: '#33ccff',
    cursorAccent: '#0c0e16',
    selectionBackground: 'rgba(51,204,255,.28)',
    overviewRulerBorder: 'rgba(0,0,0,0)',
    black: '#222436', brightBlack: '#636da6',
    red: '#ff757f', brightRed: '#ff8d94',
    green: '#4fd6be', brightGreen: '#00ff99',
    yellow: '#ffc777', brightYellow: '#ffd8ab',
    blue: '#82aaff', brightBlue: '#9ab8ff',
    magenta: '#c099ff', brightMagenta: '#d7b4ff',
    cyan: '#33ccff', brightCyan: '#b2ebff',
    white: '#c8d3f5', brightWhite: '#ffffff',
  },
});

const fit = new FitAddon.FitAddon();
term.loadAddon(fit);
term.loadAddon(new WebLinksAddon.WebLinksAddon((_, uri) => post('u' + uri)));
term.open($('term'));

// Swaps in the GPU renderer after first paint
const loadWebgl = () => {
  const script = document.createElement('script');
  script.src = 'addon-webgl.js';
  script.onload = () => {
    try {
      const gl = new WebglAddon.WebglAddon();
      gl.onContextLoss(() => gl.dispose());
      term.loadAddon(gl);
      fit.fit();
    } catch { /* DOM renderer fallback */ }
  };
  document.head.appendChild(script);
};

// Shell I/O
term.onData((d) => post('i' + d));
term.onBinary((d) => post('b' + d));
term.onResize(({ cols, rows }) => post('r' + cols + ',' + rows));
term.onTitleChange((t) => {
  const title = t.replace(/^MINGW(32|64):|^MSYS:/, '') || 'bash';
  $('title').textContent = title;
  post('t' + title);
});

window.chrome.webview.addEventListener('message', ({ data }) => {
  switch (data[0]) {
    case 'o': term.write(data.slice(1)); break;
    case 'p': if (data.length > 1) term.paste(data.slice(1)); break;
    case 'f':
      document.body.classList.toggle('blurred', data[1] === '0');
      if (data[1] === '1') term.focus();
      break;
    case 'm': document.body.classList.toggle('max', data[1] === '1'); break;
  }
});

// Fit to window, debounced to one per frame
let fitQueued = false;
new ResizeObserver(() => {
  if (fitQueued) return;
  fitQueued = true;
  requestAnimationFrame(() => { fitQueued = false; fit.fit(); });
}).observe($('wrap'));

// Clipboard and keybinds
const copy = () => {
  post('c' + term.getSelection());
  term.clearSelection();
};

const zoom = (delta) => {
  const size = delta === 0 ? 14 : Math.min(32, Math.max(8, term.options.fontSize + delta));
  term.options.fontSize = size;
  store.set('fontSize', size);
  fit.fit();
};

term.attachCustomKeyEventHandler((e) => {
  if (e.type !== 'keydown' || !e.ctrlKey || e.altKey) return true;
  const k = e.key.toLowerCase();
  const act = (fn) => { e.preventDefault(); fn(); return false; };

  if (k === 'c' && (e.shiftKey || term.hasSelection())) return act(copy);
  if (k === 'v') return act(() => post('p'));
  if (k === 'n' && e.shiftKey) return act(() => post('n'));
  if (k === '=' || k === '+') return act(() => zoom(1));
  if (k === '-' || k === '_') return act(() => zoom(-1));
  if (k === '0') return act(() => zoom(0));
  return true;
});

$('wrap').addEventListener('contextmenu', (e) => {
  e.preventDefault();
  term.hasSelection() ? copy() : post('p');
});

$('wrap').addEventListener('wheel', (e) => {
  if (!e.ctrlKey) return;
  e.preventDefault();
  zoom(e.deltaY < 0 ? 1 : -1);
}, { passive: false });

// Window chrome
document.querySelectorAll('.ctl button').forEach((b) =>
  b.addEventListener('click', () => post(b.dataset.cmd)));

document.querySelectorAll('.grip').forEach((g) =>
  g.addEventListener('mousedown', (e) => { if (e.button === 0) post('s' + g.dataset.edge); }));

document.fonts.ready.then(() => {
  fit.fit();
  post('k' + term.cols + ',' + term.rows);
  term.focus();
  setTimeout(loadWebgl, 250);
});
