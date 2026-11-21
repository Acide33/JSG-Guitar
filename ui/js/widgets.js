// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Jérôme Soler — JSG Guitar
/*
    JSG Guitar — contrôles de l'interface (copie de Source/Widgets.h, en HTML).
    Les éléments fixes viennent de l'image de fond ; ces contrôles ne dessinent que ce qui bouge.
    Toutes les coordonnées sont celles de l'image (1536 x 1024).
*/

export const stage = () => document.getElementById('stage');
export let stageScale = 1;
export const setStageScale = (s) => { stageScale = s; };

export function el(tag, cls, parent, style) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (style) Object.assign(e.style, style);
  if (parent) parent.appendChild(e);
  return e;
}

export function place(e, x, y, w, h) {
  e.style.left = x + 'px'; e.style.top = y + 'px';
  if (w !== undefined) e.style.width = w + 'px';
  if (h !== undefined) e.style.height = h + 'px';
  return e;
}

/** Position d'un événement dans le repère de l'image. */
export function stagePoint(ev) {
  const r = stage().getBoundingClientRect();
  return { x: (ev.clientX - r.left) / stageScale, y: (ev.clientY - r.top) / stageScale };
}

//------------------------------------------------------------------------------
// Bulle de valeur pendant le réglage d'un potard
let bubble = null;
export function showBubble(x, y, text) {
  if (!bubble) bubble = el('div', '', stage()), bubble.id = 'bubble';
  bubble.textContent = text;
  place(bubble, x, y);
  bubble.classList.remove('hidden');
}
export function hideBubble() { bubble?.classList.add('hidden'); }

/**
    Glisser vertical / horizontal pour régler une position 0..1 (comme les potards de la version PC
    « RotaryHorizontalVerticalDrag »). Molette : petits pas. Double-clic : valeur par défaut.
    opts : get(), set(pos), reset(), sensitivity (pixels pour toute la course), steps (sélecteur cranté),
           onStart(), onEnd(), bubble() -> {x, y, text}, click() (clic sans glisser)
*/
export function makeDraggable(target, opts) {
  const sens = opts.sensitivity || 220;
  let start = null;

  target.addEventListener('pointerdown', (ev) => {
    if (ev.button !== 0) return;
    ev.preventDefault();
    ev.stopPropagation();
    target.setPointerCapture(ev.pointerId);
    start = { x: ev.clientX, y: ev.clientY, pos: opts.get(), moved: false };
    opts.onStart?.();
    const b = opts.bubble?.(); if (b) showBubble(b.x, b.y, b.text);
  });

  target.addEventListener('pointermove', (ev) => {
    if (!start) return;
    const dx = (ev.clientX - start.x) / stageScale, dy = (ev.clientY - start.y) / stageScale;
    if (Math.abs(dx) + Math.abs(dy) > 3) start.moved = true;
    if (!start.moved) return;
    const fine = ev.shiftKey ? 0.2 : 1;
    let pos = Math.min(1, Math.max(0, start.pos + (dx - dy) / sens * fine));
    if (opts.steps > 1) pos = Math.round(pos * (opts.steps - 1)) / (opts.steps - 1);
    opts.set(pos);
    const b = opts.bubble?.(); if (b) showBubble(b.x, b.y, b.text);
  });

  const end = () => {
    if (!start) return;
    const clicked = !start.moved;
    start = null;
    hideBubble();
    opts.onEnd?.();
    if (clicked) opts.click?.();
  };
  target.addEventListener('pointerup', end);
  target.addEventListener('pointercancel', end);

  target.addEventListener('wheel', (ev) => {
    ev.preventDefault();
    const step = opts.steps > 1 ? 1 / (opts.steps - 1) : (opts.wheelStep || 0.01);
    opts.set(Math.min(1, Math.max(0, opts.get() + (ev.deltaY < 0 ? step : -step))));
    const b = opts.bubble?.();
    if (b) { showBubble(b.x, b.y, b.text); clearTimeout(target._bubbleTimer); target._bubbleTimer = setTimeout(hideBubble, 700); }
  }, { passive: false });

  if (opts.reset) target.addEventListener('dblclick', (ev) => { ev.preventDefault(); opts.reset(); });
}

const startAngle = -135, sweep = 270;
export const angleFor = (pos) => startAngle + pos * sweep;

/**
    Potard de l'image : seul le repère est dessiné, par-dessus le bouton peint dans le fond.
    style : 'large' | 'small' | 'chrome'
*/
export function pointerKnob(parent, { x, y, r, style = 'large', get, set, reset, format, tooltip }) {
  const half = r + 6;
  const e = place(el('div', 'knob ' + style, parent), x - half, y - half, half * 2, half * 2);
  if (tooltip) e.title = tooltip;

  let inner, outer, width;
  if (style === 'large') { inner = r * 0.10; outer = r * 0.96; width = 2.8; }
  else if (style === 'small') { inner = r * 0.10; outer = r * 0.94; width = 2.4; }
  else { inner = 0; outer = r * 1.02; width = 2.2; }

  const ptr = place(el('div', 'ptr', e), half - width / 2, half - outer, width, outer - inner);
  ptr.style.transformOrigin = `50% ${outer}px`;
  if (style === 'chrome') place(el('div', 'dot', e), half - 2.5, half - 2.5);

  let shown = -1;
  const update = () => {
    const pos = get();
    if (pos !== shown) { shown = pos; ptr.style.transform = `rotate(${angleFor(pos)}deg)`; }
  };

  makeDraggable(e, { get, set: (p) => { set(p); update(); }, reset: reset && (() => { reset(); update(); }),
    bubble: format && (() => ({ x, y: y - half - 4, text: format() })) });
  update();
  return { e, update };
}

/** Potard entièrement dessiné (panneaux des onglets), avec son nom et sa valeur. */
export function drawnKnob(parent, { x, y, w, h, title, get, set, reset, format, tooltip }) {
  const e = place(el('div', 'dknob', parent), x, y, w, h);
  if (tooltip) e.title = tooltip;
  el('div', 't', e).textContent = title;
  const value = el('div', 'v', e);

  const areaH = h - 40;
  const radius = Math.min(w, areaH) / 2 - 7;
  const cx = w / 2, cy = areaH / 2;
  const ns = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(ns, 'svg');
  svg.setAttribute('width', w); svg.setAttribute('height', areaH);
  const id = 'g' + Math.random().toString(36).slice(2);
  svg.innerHTML = `
    <defs>
      <linearGradient id="${id}r" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#dfe5ee"/><stop offset="1" stop-color="#4a515c"/></linearGradient>
      <linearGradient id="${id}c" x1="0.2" y1="0" x2="0.65" y2="1"><stop offset="0" stop-color="#3a3f47"/><stop offset="1" stop-color="#07080a"/></linearGradient>
    </defs>
    <path class="track" fill="none" stroke="#1b2029" stroke-width="3" stroke-linecap="round"/>
    <path class="val" fill="none" stroke="#2f9bff" stroke-width="3" stroke-linecap="round"/>
    <circle cx="${cx}" cy="${cy}" r="${radius}" fill="url(#${id}r)"/>
    <circle cx="${cx}" cy="${cy}" r="${radius - 2.5}" fill="url(#${id}c)"/>
    <rect class="p" x="${cx - 1.2}" y="${cy - radius + 4}" width="2.4" height="${radius * 0.62}" rx="1.2" fill="rgba(255,255,255,0.95)"/>`;
  e.appendChild(svg);

  const R = radius + 4;
  const arc = (a0, a1) => {
    const p = (a) => [cx + R * Math.sin(a * Math.PI / 180), cy - R * Math.cos(a * Math.PI / 180)];
    const [x0, y0] = p(a0), [x1, y1] = p(a1);
    return `M ${x0} ${y0} A ${R} ${R} 0 ${a1 - a0 > 180 ? 1 : 0} 1 ${x1} ${y1}`;
  };
  svg.querySelector('.track').setAttribute('d', arc(startAngle, startAngle + sweep));
  const valPath = svg.querySelector('.val'), p = svg.querySelector('.p');

  let shown = -1;
  const update = () => {
    const pos = get();
    value.textContent = format();
    if (pos === shown) return;
    shown = pos;
    const a = angleFor(pos);
    valPath.setAttribute('d', pos > 0.001 ? arc(startAngle, a) : '');
    p.setAttribute('transform', `rotate(${a} ${cx} ${cy})`);
  };

  makeDraggable(e, { get, set: (v) => { set(v); update(); }, reset: reset && (() => { reset(); update(); }), sensitivity: 200 });
  update();
  return { e, update };
}

/** Sélecteur « ‹ nom › ». framed : dessine son cadre et ses flèches (hors de l'image de fond). */
export function selector(parent, { x, y, w, h, arrow = 30, fontSize = 15.5, framed = false, getText, onStep, onClick, tooltip }) {
  const e = place(el('div', 'selector' + (framed ? ' framed' : ''), parent), x, y, w, h);
  e.style.fontSize = fontSize + 'px';
  if (tooltip) e.title = tooltip;
  const left = el('div', 'arrow', e, { width: arrow - 6 + 'px' });
  const name = el('div', 'name', e);
  const span = el('span', '', name);
  const right = el('div', 'arrow', e, { width: arrow - 6 + 'px' });
  if (framed) { left.textContent = '‹'; right.textContent = '›'; }

  left.addEventListener('click', (ev) => { ev.stopPropagation(); onStep(-1); update(); });
  right.addEventListener('click', (ev) => { ev.stopPropagation(); onStep(1); update(); });
  name.addEventListener('click', (ev) => { ev.stopPropagation(); onClick?.(e); });
  e.addEventListener('wheel', (ev) => { ev.preventDefault(); onStep(ev.deltaY > 0 ? 1 : -1); update(); }, { passive: false });

  let shown = null, fitted = false;
  const update = () => {
    const t = getText();
    if (t !== shown) { shown = t; span.textContent = t; fitted = false; }
    if (!fitted) fitted = fitText(span, name);
  };
  const setDimmed = (d) => e.classList.toggle('dimmed', d);
  update();
  return { e, update, setDimmed };
}

/** Comme dans la version PC : le texte trop long est d'abord resserré (jusqu'à 75 %), puis coupé.
    Renvoie faux si l'élément n'est pas affiché (à refaire plus tard). */
export function fitText(span, box) {
  const avail = box.clientWidth - 4;
  if (avail <= 0) return false;
  span.style.display = 'inline-block';
  span.style.transform = '';
  span.style.maxWidth = '';
  const need = span.scrollWidth;
  if (need > avail) {
    const k = Math.max(0.75, avail / need);
    span.style.transform = `scaleX(${k})`;
    span.style.maxWidth = avail / k + 'px';
  }
  return true;
}

/** Icône marche/arrêt d'un module (bleue et lumineuse quand il est allumé). */
export function powerIcon(parent, { x, y, get, set, tooltip, radius = 8.5 }) {
  const e = place(el('div', 'power', parent), x - 17, y - 17);
  if (tooltip) e.title = tooltip;
  const gap = 0.62, c = 17, r = radius;
  const a0 = -Math.PI / 2 + gap, a1 = -Math.PI / 2 - gap + 2 * Math.PI;
  const pt = (a) => `${c + r * Math.cos(a)} ${c + r * Math.sin(a)}`;
  e.innerHTML = `<svg viewBox="0 0 34 34"><path d="M ${pt(a0)} A ${r} ${r} 0 1 1 ${pt(a1)} M ${c} ${c - r * 1.25} L ${c} ${c - r * 0.15}"/></svg>`;
  e.addEventListener('click', (ev) => { ev.stopPropagation(); set(!get()); update(); });
  const update = () => e.classList.toggle('on', !!get());
  update();
  return { e, update };
}

/** LED cliquable ; la zone cliquable couvre aussi le libellé de l'image. */
export function ledButton(parent, { area, ledX, ledY, radius, get, onClick, tooltip }) {
  const [x, y, w, h] = area;
  const e = place(el('div', 'led', parent), x, y, w, h);
  if (tooltip) e.title = tooltip;
  place(el('i', '', e), ledX - x - radius, ledY - y - radius, radius * 2, radius * 2);
  e.addEventListener('click', (ev) => { ev.stopPropagation(); onClick(); });
  const update = () => e.classList.toggle('on', !!get());
  update();
  return { e, update };
}

/** Bouton texte (style des boutons de JSG Guitar). */
export function button(parent, { x, y, w, h, text, onClick, tooltip, toggle }) {
  const e = place(el('button', 'btn', parent), x, y, w, h);
  e.textContent = text;
  if (tooltip) e.title = tooltip;
  e.addEventListener('click', (ev) => { ev.stopPropagation(); onClick?.(ev); update(); });
  const update = () => { if (toggle) e.classList.toggle('on', !!toggle()); };
  update();
  return { e, update };
}

//------------------------------------------------------------------------------
// Menus déroulants
let openMenu = null;

export function closeMenu() { openMenu?.remove(); openMenu = null; }

/**
    items : [{ text, onClick, checked, disabled } | { header } | { separator: true } | { text, submenu: items }]
    (un sous-menu remplace le contenu du menu, avec « Retour »)
    anchor : élément sous lequel le menu s'ouvre, ou { x, y } dans le repère de l'image.
*/
export function showMenu(items, anchor) {
  closeMenu();
  const m = el('div', 'menu', stage());
  openMenu = m;

  const fill = (list, back) => {
    m.innerHTML = '';
    if (back) {
      const row = el('div', 'item', m);
      row.textContent = '‹ Retour';
      row.addEventListener('click', (ev) => { ev.stopPropagation(); fill(back, null); });
      el('div', 'sep', m);
    }
    for (const it of list) {
      if (it.header) { el('div', 'header', m).textContent = it.header; continue; }
      if (it.separator) { el('div', 'sep', m); continue; }
      const row = el('div', 'item' + (it.checked ? ' checked' : '') + (it.disabled ? ' disabled' : ''), m);
      row.textContent = it.text + (it.submenu ? '  ›' : '');
      if (it.disabled) continue;
      row.addEventListener('click', (ev) => {
        ev.stopPropagation();
        if (it.submenu) { fill(it.submenu, list); return; }
        closeMenu();
        it.onClick?.();
      });
    }
  };
  fill(items, null);

  let x, y;
  if (anchor instanceof Element) {
    const r = anchor.getBoundingClientRect(), s = stage().getBoundingClientRect();
    x = (r.left - s.left) / stageScale; y = (r.bottom - s.top) / stageScale + 2;
  } else { x = anchor.x; y = anchor.y; }

  place(m, x, y);
  const mw = m.offsetWidth, mh = m.offsetHeight;
  if (x + mw > 1530) m.style.left = Math.max(4, 1530 - mw) + 'px';
  if (y + mh > 1018) m.style.top = Math.max(4, 1018 - mh) + 'px';
  return m;
}

document.addEventListener('pointerdown', (ev) => {
  if (openMenu && !ev.target.closest('.menu')) closeMenu();
});
document.addEventListener('keydown', (ev) => { if (ev.key === 'Escape') closeMenu(); });

//------------------------------------------------------------------------------
// Messages
let toastTimer = null;
export function toast(text, ms = 3500) {
  let t = document.getElementById('toast');
  if (!t) { t = el('div', '', stage()); t.id = 'toast'; }
  t.textContent = text;
  t.classList.remove('hidden');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.add('hidden'), ms);
}

/** Boîte de dialogue. content : élément ou texte. buttons : [{ text, primary, onClick -> faux pour garder ouvert }] */
export function modal(title, content, buttons = [{ text: 'OK', primary: true }]) {
  const back = el('div', 'modal-back', stage());
  const box = el('div', 'modal', back);
  el('h2', '', box).textContent = title;
  if (typeof content === 'string') content.split('\n\n').forEach((p) => (el('p', '', box).textContent = p));
  else box.appendChild(content);
  const row = el('div', 'buttons', box);
  const close = () => back.remove();
  for (const b of buttons) {
    const btn = el('button', 'btn' + (b.primary ? ' on' : ''), row);
    btn.textContent = b.text;
    btn.addEventListener('click', () => { if (b.onClick?.() !== false) close(); });
  }
  back.addEventListener('pointerdown', (ev) => { if (ev.target === back) close(); });
  back.addEventListener('keydown', (ev) => {
    if (ev.key === 'Escape') close();
    if (ev.key === 'Enter') { const p = buttons.find((b) => b.primary); if (p && p.onClick?.() !== false) close(); }
  });
  setTimeout(() => (box.querySelector('input, select, button.on') || box).focus(), 0);
  return { close, box };
}
