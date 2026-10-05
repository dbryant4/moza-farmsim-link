// The tuning page served by WebUi: one HTML document with its CSS and JavaScript inline, and no
// reference to anything outside itself, since the PC it runs on may be offline.
//
// It is kept as plain ASCII (HTML entities and JavaScript escapes for anything else) so the bytes
// served do not depend on how a compiler reads this file, and split into several literals because
// MSVC refuses a single string literal longer than about 16 kB.

#include "web_ui.hpp"

namespace bridge {

namespace {

const char* const kPageParts[] = {
    // ---- Document head and styles ----
    R"PAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light dark">
<title>MOZA bridge</title>
<link rel="icon" href="data:,">
<style>
:root {
  color-scheme: light dark;
  --bg: #f2f3f1; --panel: #ffffff; --ink: #1b1f23; --muted: #59626b; --line: #d8dcdf; --track: #cfd5d9;
  --accent: #1d6b58; --accent-ink: #ffffff;
  --danger: #b3261e; --danger-ink: #ffffff; --danger-soft: #fbe9e7;
  --warn: #855000; --warn-soft: #fff0cc; --warn-line: #e0b860;
  --focus: #0b57d0; --knob: #ffffff;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #121518; --panel: #1b1f23; --ink: #e7e9eb; --muted: #9da6ae; --line: #2e343b; --track: #3b434b;
    --accent: #56c2a3; --accent-ink: #06211a;
    --danger: #f28b82; --danger-ink: #3a0b07; --danger-soft: #3a1b19;
    --warn: #f2c265; --warn-soft: #33290f; --warn-line: #6b5520;
    --focus: #8ab4f8; --knob: #f4f5f6;
  }
}
* { box-sizing: border-box; }
[hidden] { display: none !important; }
html {
  -webkit-text-size-adjust: 100%;
  scroll-padding-top: calc(var(--top, 64px) + 12px);
  scroll-padding-bottom: calc(var(--bottom, 64px) + 12px);
}
body {
  margin: 0; background: var(--bg); color: var(--ink);
  font: 15px/1.45 system-ui, -apple-system, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
}
h1, h2, h3, p { margin: 0; }
button, input { font: inherit; color: inherit; }
fieldset { border: 0; margin: 0; padding: 0; min-width: 0; }
:focus-visible { outline: 2px solid var(--focus); outline-offset: 2px; }
.sr { position: absolute; width: 1px; height: 1px; overflow: hidden; clip-path: inset(50%); white-space: nowrap; }
.wrap { max-width: 1160px; margin: 0 auto; padding-left: 20px; padding-right: 20px; }

/* Top bar: connection state and the two controls that must always be in reach. */
.top { position: sticky; top: 0; z-index: 20; background: var(--panel); border-bottom: 1px solid var(--line); }
.top .wrap { display: flex; flex-wrap: wrap; align-items: center; gap: 10px 20px; padding-top: 10px; padding-bottom: 10px; }
.brand { flex: 1 1 200px; min-width: 0; }
h1 { font-size: 16px; font-weight: 650; }
.conn { display: flex; align-items: center; gap: 7px; font-size: 13px; color: var(--muted); }
.dot { flex: none; width: 9px; height: 9px; border-radius: 50%; background: var(--muted); }
.conn.live .dot { background: var(--accent); }
.conn.stale { color: var(--warn); }
.conn.stale .dot { background: var(--warn); }
.conn.down { color: var(--danger); font-weight: 650; }
.conn.down .dot { background: var(--danger); }
.master { display: flex; flex-wrap: wrap; align-items: center; gap: 10px; }

.btn {
  appearance: none; border: 1px solid var(--line); background: var(--panel); color: var(--ink);
  border-radius: 8px; padding: 7px 14px; min-height: 38px; font-weight: 550; cursor: pointer;
}
.btn:hover:not(:disabled) { border-color: var(--muted); }
.btn:disabled { opacity: .45; cursor: default; }
.btn.primary { background: var(--accent); border-color: var(--accent); color: var(--accent-ink); }
.btn.danger { background: var(--danger); border-color: var(--danger); color: var(--danger-ink); font-weight: 650; }
.btn.armed { border-color: var(--danger); color: var(--danger); }

.switch { position: relative; display: inline-flex; align-items: center; gap: 10px; min-height: 38px; cursor: pointer; }
.switch input { position: absolute; inset: 0; width: 100%; height: 100%; margin: 0; opacity: 0; cursor: pointer; }
.knob { flex: none; position: relative; width: 44px; height: 26px; border-radius: 13px; background: var(--track); transition: background .15s; }
.knob::after {
  content: ""; position: absolute; top: 3px; left: 3px; width: 20px; height: 20px; border-radius: 50%;
  background: var(--knob); box-shadow: 0 1px 2px rgba(0, 0, 0, .35); transition: transform .15s;
}
.switch input:checked + .knob { background: var(--accent); }
.switch input:checked + .knob::after { transform: translateX(18px); }
.switch input:focus-visible + .knob { outline: 2px solid var(--focus); outline-offset: 2px; }
.switch input:disabled { cursor: default; }
.switch input:disabled ~ * { opacity: .5; }
.master-switch { padding: 0 12px 0 8px; border: 1px solid var(--line); border-radius: 8px; font-weight: 650; }
.master-switch.off { background: var(--warn-soft); border-color: var(--warn-line); color: var(--warn); }

/* Page body: settings, with the live status beside them when there is room. */
main.wrap { display: grid; grid-template-columns: minmax(0, 1fr); gap: 16px; padding-top: 16px; padding-bottom: calc(var(--bottom, 64px) + 24px); }
.card { background: var(--panel); border: 1px solid var(--line); border-radius: 12px; }
.card > h2 { font-size: 15px; font-weight: 650; padding: 13px 16px 11px; border-bottom: 1px solid var(--line); }
.empty { color: var(--muted); padding: 16px; }
.settings { display: grid; gap: 16px; align-content: start; }
fieldset:disabled .settings { opacity: .55; }
@media (min-width: 900px) {
  main.wrap { grid-template-columns: minmax(0, 1fr) 340px; align-items: start; gap: 20px; padding-top: 20px; }
  #settingsBox { grid-column: 1; grid-row: 1; }
  .status {
    grid-column: 2; grid-row: 1; position: sticky; top: calc(var(--top, 64px) + 20px);
    max-height: calc(100vh - var(--top, 64px) - var(--bottom, 64px) - 40px); overflow: auto;
  }
}

/* One setting. The controls are placed on a grid so that the reset button can sit beside the name
   while coming after the slider and the number in keyboard order. */
.setting {
  display: grid; grid-template-columns: minmax(0, 1fr) auto auto; align-items: center; column-gap: 10px; row-gap: 2px;
  padding: 12px 16px 13px; border-top: 1px solid var(--line);
}
.card > h2 + .setting { border-top: 0; }
.sname { grid-row: 1; grid-column: 1; min-width: 0; min-height: 30px; display: flex; align-items: center; font-weight: 550; }
.mark { flex: none; width: 8px; height: 8px; margin-left: 8px; border-radius: 50%; background: var(--warn); }
.def {
  grid-row: 1; grid-column: 2 / 4; justify-self: end;
  appearance: none; border: 1px solid var(--line); background: transparent; color: var(--ink);
  border-radius: 7px; padding: 3px 9px; min-height: 28px; font-size: 13px; cursor: pointer; white-space: nowrap;
}
.def:hover:not(:disabled) { border-color: var(--muted); }
.def:disabled { border-color: transparent; color: var(--muted); cursor: default; padding-right: 0; }
.setting input[type=range] { grid-row: 2; grid-column: 1; width: 100%; }
.num {
  grid-row: 2; grid-column: 2; width: 6.4em; padding: 5px 6px 5px 8px; text-align: right; font-variant-numeric: tabular-nums;
  border: 1px solid var(--line); border-radius: 7px; background: var(--bg);
}
.unit { grid-row: 2; grid-column: 3; width: 3.2em; color: var(--muted); font-size: 13px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.setting > .switch { grid-row: 2; grid-column: 1 / -1; justify-self: start; }
.help { grid-row: 3; grid-column: 1 / -1; margin-top: 1px; color: var(--muted); font-size: 13px; max-width: 62ch; }
.swtext { color: var(--muted); }
@media (max-width: 560px) {
  /* Name, then a slider as wide as the screen allows, then the number with the reset beside it. */
  .setting { grid-template-columns: auto minmax(0, 1fr) auto; }
  .sname, .setting input[type=range] { grid-column: 1 / -1; }
  .num { grid-row: 3; grid-column: 1; }
  .unit { grid-row: 3; grid-column: 2; width: auto; }
  .def { grid-row: 3; grid-column: 3; }
  .help { grid-row: 4; margin-top: 3px; }
  .setting > .switch { grid-column: 1 / 3; }
  .setting.toggle .def { grid-row: 2; }
  .setting.toggle .help { grid-row: 3; }
}

input[type=range] { -webkit-appearance: none; appearance: none; min-width: 0; height: 30px; margin: 0; background: transparent; cursor: pointer; --f: 0; }
input[type=range]::-webkit-slider-runnable-track {
  height: 6px; border-radius: 3px;
  background: linear-gradient(to right, var(--accent) calc(10px + (100% - 20px) * var(--f)), var(--track) calc(10px + (100% - 20px) * var(--f)));
}
input[type=range]::-webkit-slider-thumb {
  -webkit-appearance: none; width: 20px; height: 20px; margin-top: -7px; border-radius: 50%;
  background: var(--knob); border: 2px solid var(--accent); box-shadow: 0 1px 2px rgba(0, 0, 0, .25);
}
input[type=range]::-moz-range-track { height: 6px; border-radius: 3px; background: var(--track); }
input[type=range]::-moz-range-progress { height: 6px; border-radius: 3px; background: var(--accent); }
input[type=range]::-moz-range-thumb {
  width: 16px; height: 16px; border-radius: 50%; background: var(--knob); border: 2px solid var(--accent);
  box-shadow: 0 1px 2px rgba(0, 0, 0, .25);
}
input[type=range]:focus-visible { outline: none; }
input[type=range]:focus-visible::-webkit-slider-thumb { outline: 2px solid var(--focus); outline-offset: 2px; }
input[type=range]:focus-visible::-moz-range-thumb { outline: 2px solid var(--focus); outline-offset: 2px; }
@media (pointer: coarse) {
  input[type=range] { height: 40px; }
  input[type=range]::-webkit-slider-thumb { width: 26px; height: 26px; margin-top: -10px; }
  input[type=range]::-moz-range-thumb { width: 22px; height: 22px; }
}

/* Live status. The heading folds the panel away, which matters on a narrow screen where it sits above the settings. */
.status summary {
  display: flex; align-items: center; justify-content: space-between; gap: 10px; padding: 13px 16px 11px;
  border-bottom: 1px solid var(--line); border-radius: 12px 12px 0 0; cursor: pointer; list-style: none;
}
.status summary::-webkit-details-marker { display: none; }
.status summary::after {
  content: ""; flex: none; width: 8px; height: 8px; margin: 0 4px 3px 0;
  border-right: 2px solid var(--muted); border-bottom: 2px solid var(--muted); transform: rotate(45deg);
}
.status:not([open]) summary { border-bottom: 0; border-radius: 12px; }
.status:not([open]) summary::after { margin-bottom: 0; transform: rotate(-45deg); }
.status summary h2 { font-size: 15px; font-weight: 650; }
.note { margin: 12px 16px 0; padding: 8px 10px; border-radius: 8px; font-size: 13px; background: var(--warn-soft); color: var(--warn); }
.note.down { background: var(--danger-soft); color: var(--danger); }
.sbody { padding-bottom: 10px; transition: opacity .2s; }
.status.old .sbody { opacity: .4; }
.sgroup h3 { margin: 14px 16px 2px; font-size: 12px; font-weight: 650; letter-spacing: .05em; text-transform: uppercase; color: var(--muted); }
.items { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 0 20px; padding: 0 16px; }
.item { padding: 5px 0 6px; min-width: 0; }
.item.wide { grid-column: 1 / -1; }
.ihead { display: flex; align-items: baseline; justify-content: space-between; gap: 10px; }
.ilabel { min-width: 0; color: var(--muted); font-size: 13px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.ival { flex: none; max-width: 70%; font-weight: 550; font-variant-numeric: tabular-nums; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.bar { position: relative; height: 6px; margin-top: 4px; border-radius: 3px; background: var(--track); }
.fill { position: absolute; top: 0; bottom: 0; left: 0; width: 0; border-radius: 3px; background: var(--accent); transition: left .2s linear, width .2s linear; }
.zero { position: absolute; top: -2px; bottom: -2px; width: 2px; margin-left: -1px; background: var(--muted); }
@media (min-width: 900px) { .items { grid-template-columns: minmax(0, 1fr); } }

/* Bottom bar: whether anything is unsaved, and what to do about it. */
.savebar { position: fixed; left: 0; right: 0; bottom: 0; z-index: 20; background: var(--panel); border-top: 1px solid var(--line); }
.savebar.unsaved { background: var(--warn-soft); border-top-color: var(--warn-line); }
.savebar .wrap {
  display: flex; flex-wrap: wrap; align-items: center; gap: 8px 16px;
  padding-top: 10px; padding-bottom: calc(10px + env(safe-area-inset-bottom, 0px));
}
.dirty { flex: 1 1 240px; min-width: 0; font-size: 13px; color: var(--muted); }
.dirty strong { display: block; font-size: 15px; font-weight: 650; color: var(--ink); }
.savebar.unsaved .dirty, .savebar.unsaved .dirty strong { color: var(--warn); }
.dirty span { display: block; overflow-wrap: anywhere; }
.actions { display: flex; flex-wrap: wrap; gap: 8px; }
.toast {
  position: fixed; left: 50%; bottom: calc(var(--bottom, 64px) + 12px); z-index: 30; transform: translateX(-50%);
  width: max-content; max-width: min(92vw, 560px); padding: 9px 14px; border-radius: 9px; font-size: 14px;
  background: var(--ink); color: var(--bg); box-shadow: 0 4px 14px rgba(0, 0, 0, .25);
  opacity: 0; visibility: hidden; transition: opacity .15s, visibility .15s;
}
.toast.show { opacity: 1; visibility: visible; }
.toast.bad { background: var(--danger); color: var(--danger-ink); }
@media (prefers-reduced-motion: reduce) { * { transition: none !important; } }
</style>
</head>
)PAGE",

    // ---- Document body ----
    R"PAGE(<body>
<header class="top" id="top">
  <div class="wrap">
    <div class="brand">
      <h1>MOZA bridge</h1>
      <p class="conn" id="conn">
        <span class="dot" aria-hidden="true"></span>
        <span id="connText" role="status">Connecting&hellip;</span>
        <span id="connAge" aria-hidden="true"></span>
      </p>
    </div>
    <div class="master">
      <label class="switch master-switch" id="forcesBox">
        <input type="checkbox" role="switch" id="forces" aria-label="Forces" checked disabled>
        <span class="knob" aria-hidden="true"></span>
        <span id="forcesText" aria-hidden="true">Forces on</span>
      </label>
      <button type="button" class="btn danger" id="release">Release forces</button>
    </div>
  </div>
</header>

<main class="wrap">
  <details class="card status" id="statusCard" open>
    <summary><h2>Live status</h2></summary>
    <p class="note" id="statusNote" hidden></p>
    <div class="sbody" id="status"><p class="empty">Waiting for the bridge&hellip;</p></div>
  </details>
  <fieldset id="settingsBox" disabled>
    <legend class="sr">Settings</legend>
    <div class="settings" id="settings"><p class="empty">Loading settings&hellip;</p></div>
  </fieldset>
</main>

<footer class="savebar" id="savebar">
  <div class="wrap">
    <div class="dirty">
      <strong id="dirtyText">No unsaved changes</strong>
      <span id="dirtyHint"></span>
    </div>
    <div class="actions">
      <button type="button" class="btn" id="reset" disabled>Reset to defaults</button>
      <button type="button" class="btn" id="revert" disabled>Revert</button>
      <button type="button" class="btn primary" id="save" disabled>Save</button>
    </div>
  </div>
</footer>
<div class="toast" id="toast" role="status" aria-live="polite"></div>
)PAGE",

    // ---- Script: talking to the bridge, and the settings controls ----
    R"PAGE(<script>
(() => {
'use strict';

const POLL_MS = 250;         // state is fetched four times a second
const TIMEOUT_MS = 1500;     // a request that takes longer than this has failed
const DOWN_AFTER_MS = 1500;  // no answer for this long: the bridge is not responding
const STALE_AFTER_S = 2;     // answering, but the main loop has published nothing for this long
const SEND_MS = 70;          // changes made by dragging are sent at most this often

const $ = id => document.getElementById(id);
const ui = {};
for (const id of ['top', 'conn', 'connText', 'connAge', 'forcesBox', 'forces', 'forcesText', 'release', 'statusCard',
                  'statusNote', 'status', 'settingsBox', 'settings', 'savebar', 'dirtyText', 'dirtyHint', 'reset', 'revert', 'save', 'toast']) {
  ui[id] = $(id);
}

const controls = new Map();    // setting key -> its controls and what they currently show
const statusRows = new Map();  // status key -> its elements
let settingsShape = '';
let statusShape = '';
let state = null;              // the last state document received
let lastOk = 0;                // when it was received
let graceUntil = performance.now() + DOWN_AFTER_MS;
let down = false;
let forcesTouched = 0;
let forcesBusy = 0;
let sendTimer = 0;
let queue = Promise.resolve();
let toastTimer = 0;

function make(tag, className, text) {
  const element = document.createElement(tag);
  if (className) element.className = className;
  if (text != null) element.textContent = text;
  return element;
}

function setText(element, text) {
  if (element.textContent !== text) element.textContent = text;
}

const clamp = (value, low, high) => Math.min(high, Math.max(low, value));

// A long path keeps its end, which is the part that tells settings files apart.
const shortPath = path => (path.length > 46 ? '\u2026' + path.slice(-44) : path);

function seconds(ms) {
  const s = Math.max(0, Math.round(ms / 1000));
  return s < 90 ? s + ' s' : Math.round(s / 60) + ' min';
}

function toast(text, bad) {
  ui.toast.textContent = text;
  ui.toast.className = 'toast show' + (bad ? ' bad' : '');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { ui.toast.className = 'toast' + (bad ? ' bad' : ''); }, bad ? 7000 : 3500);
}

// ---- Requests ----

function request(method, path, body) {
  const abort = new AbortController();
  const timer = setTimeout(() => abort.abort(), TIMEOUT_MS);
  const sentAt = performance.now();
  const options = {method, cache: 'no-store', signal: abort.signal};
  if (method === 'POST') {
    options.body = body || '';
    options.headers = {'Content-Type': 'text/plain'};
  }
  return fetch(path, options)
    .then(response => response.text().then(text => {
      if (!response.ok) {
        const refusal = new Error(text.trim() || 'HTTP ' + response.status);
        refusal.fromBridge = true;
        throw refusal;
      }
      return {doc: JSON.parse(text), sentAt};
    }))
    .finally(() => clearTimeout(timer));
}

const why = error => (error && error.fromBridge ? error.message : 'the bridge is not responding');

// Changes and actions go out one at a time and in order, so a save never overtakes the edit before it.
function enqueue(job) {
  queue = queue.then(job, job);
  return queue;
}

function received(doc, sentAt) {
  state = doc;
  lastOk = performance.now();
  try {
    renderSettings(doc, sentAt);
    renderStatus(doc);
    renderForces(doc, sentAt);
    renderConnection();
    renderSaveBar();
  } catch (error) {
    console.error(error);
  }
}

function poll() {
  request('GET', '/api/state')
    .then(answer => received(answer.doc, answer.sentAt))
    .catch(() => {})  // renderConnection notices the silence
    .then(() => setTimeout(poll, POLL_MS));
}

// ---- Settings ----

function decimalsOf(step) {
  const text = String(step);
  const exponent = text.indexOf('e-');
  if (exponent >= 0) return Math.min(6, parseInt(text.slice(exponent + 2), 10));
  const point = text.indexOf('.');
  return point < 0 ? 0 : Math.min(6, text.length - point - 1);
}

function formatValue(c, value) {
  const text = value.toFixed(c.decimals);
  // A hand-edited file can hold a value between steps; show that as it is rather than rounded.
  return Math.abs(Number(text) - value) > 1e-9 ? String(Number(value.toFixed(6))) : text;
}

function paint(c) {
  if (c.toggle) {
    c.toggle.checked = c.value !== 0;
    setText(c.toggleText, c.value !== 0 ? 'On' : 'Off');
    return;
  }
  c.range.value = String(c.value);
  c.range.style.setProperty('--f', String(clamp((c.value - c.meta.min) / ((c.meta.max - c.meta.min) || 1), 0, 1)));
  c.number.value = formatValue(c, c.value);
}

function decorate(c) {
  c.mark.hidden = c.value === c.saved;
  const atDefault = c.value === c.meta.default;
  setText(c.reset, atDefault ? 'Default' : 'Reset to ' + c.defaultText);
  c.reset.disabled = atDefault;
}

// The user changed a setting here: remember it, and send it shortly.
function edit(c, value) {
  c.value = value;
  c.touched = performance.now();
  c.pending = true;
  decorate(c);
  renderSaveBar();
  if (!sendTimer) {
    sendTimer = setTimeout(() => { sendTimer = 0; enqueue(sendPending); }, SEND_MS);
  }
}

function sendPending() {
  const sent = [];
  const lines = [];
  controls.forEach(c => {
    if (c.pending) {
      c.pending = false;
      c.sending = true;
      sent.push(c);
      lines.push(c.meta.key + '=' + c.value);
    }
  });
  if (!sent.length) return Promise.resolve();
  const settle = () => sent.forEach(c => { c.sending = false; });
  return request('POST', '/api/set', lines.join('\n'))
    .then(answer => { settle(); received(answer.doc, answer.sentAt); })
    .catch(error => { settle(); toast('Change not applied: ' + why(error), true); });
}

function buildSetting(s, index) {
  const id = 'setting' + index;
  const c = {meta: s, value: s.value, saved: s.saved, decimals: decimalsOf(s.step),
             touched: 0, pending: false, sending: false, dragging: false, editing: false};
  const row = make('div', s.type === 'toggle' ? 'setting toggle' : 'setting');
  const name = make('div', 'sname');
  const label = make('label', '', s.label || s.key);
  label.htmlFor = id;
  label.id = id + 'label';
  c.mark = make('span', 'mark');
  c.mark.title = 'Changed since the last save';
  c.mark.appendChild(make('span', 'sr', '(unsaved)'));
  c.mark.hidden = true;
  name.append(label, c.mark);
  row.appendChild(name);
  // Shown beside the name, but placed after the controls so that Tab reaches those first.
  c.reset = make('button', 'def', 'Default');
  c.reset.type = 'button';
  c.reset.id = id + 'reset';
  c.reset.setAttribute('aria-labelledby', c.reset.id + ' ' + label.id);

  const help = make('p', 'help', s.description);
  help.id = id + 'help';

  if (s.type === 'toggle') {
    c.defaultText = s.default ? 'on' : 'off';
    const box = make('label', 'switch');
    c.toggle = make('input');
    c.toggle.type = 'checkbox';
    c.toggle.id = id;
    c.toggle.setAttribute('role', 'switch');
    c.toggle.setAttribute('aria-describedby', help.id);
    const knob = make('span', 'knob');
    knob.setAttribute('aria-hidden', 'true');
    c.toggleText = make('span', 'swtext');
    c.toggleText.setAttribute('aria-hidden', 'true');
    box.append(c.toggle, knob, c.toggleText);
    row.appendChild(box);
    c.main = c.toggle;
    c.toggle.addEventListener('change', () => { edit(c, c.toggle.checked ? 1 : 0); paint(c); });
  } else {
    c.defaultText = formatValue(c, s.default) + (s.unit ? ' ' + s.unit : '');
    c.range = make('input');
    c.range.type = 'range';
    c.number = make('input', 'num');
    c.number.type = 'number';
    c.number.id = id;
    c.number.inputMode = 'decimal';
    for (const input of [c.range, c.number]) {
      input.min = String(s.min);
      input.max = String(s.max);
      input.step = String(s.step);
      input.setAttribute('aria-describedby', help.id);
    }
    c.range.setAttribute('aria-label', (s.label || s.key) + ', slider');
    row.append(c.range, c.number, make('span', 'unit', s.unit));
    c.main = c.number;

    // The slider applies as it moves.
    c.range.addEventListener('pointerdown', () => { c.dragging = true; });
    c.range.addEventListener('input', () => {
      edit(c, clamp(Number(c.range.value), s.min, s.max));
      c.range.style.setProperty('--f', String((c.value - s.min) / ((s.max - s.min) || 1)));
      c.number.value = formatValue(c, c.value);
    });
    for (const done of ['change', 'blur', 'pointercancel']) {
      c.range.addEventListener(done, () => { c.dragging = false; });
    }

    // The number field applies when the value is finished (Enter, leaving the field, or a step
    // with the arrows), not on every keystroke: typing 80 must not pass through 8 on the way.
    const commit = () => {
      if (!c.editing) return;
      c.editing = false;
      const typed = c.number.valueAsNumber;
      if (Number.isFinite(typed)) edit(c, clamp(typed, s.min, s.max));
      paint(c);
    };
    c.number.addEventListener('input', () => { c.editing = true; });
    c.number.addEventListener('change', commit);
    c.number.addEventListener('blur', commit);
    c.number.addEventListener('keydown', event => {
      if (event.key === 'Enter') {
        commit();
      } else if (event.key === 'Escape' && c.editing) {
        c.editing = false;
        paint(c);
      }
    });
  }

  c.reset.addEventListener('click', () => {
    c.editing = false;
    edit(c, s.default);
    paint(c);
    c.main.focus();
  });

  row.append(c.reset, help);
  controls.set(s.key, c);
  paint(c);
  decorate(c);
  return row;
}

function buildSettings(list) {
  controls.clear();
  ui.settings.textContent = '';
  if (!list.length) {
    ui.settings.appendChild(make('p', 'empty', 'The bridge has no settings.'));
    return;
  }
  const groups = new Map();
  list.forEach((s, index) => {
    let card = groups.get(s.group);
    if (!card) {
      card = make('section', 'card');
      const heading = make('h2', '', s.group || 'Settings');
      heading.id = 'group' + index;
      card.setAttribute('aria-labelledby', heading.id);
      card.appendChild(heading);
      ui.settings.appendChild(card);
      groups.set(s.group, card);
    }
    card.appendChild(buildSetting(s, index));
  });
}

function renderSettings(doc, sentAt) {
  const shape = JSON.stringify(doc.settings.map(s => [s.key, s.group, s.label, s.description, s.unit, s.type, s.min, s.max, s.step, s.default]));
  if (shape !== settingsShape) {
    settingsShape = shape;
    buildSettings(doc.settings);
  }
  for (const s of doc.settings) {
    const c = controls.get(s.key);
    if (!c) continue;
    c.saved = s.saved;
    // Never move a control under the user's hand: not while it is held or typed in, not while a
    // change of theirs is on its way, and not with an answer older than their last change.
    const busy = c.dragging || c.editing || c.pending || c.sending || sentAt < c.touched;
    if (!busy && c.value !== s.value) {
      c.value = s.value;
      paint(c);
    }
    decorate(c);
  }
}
)PAGE",

    // ---- Script: status, connection state, and the buttons ----
    R"PAGE(
// ---- Live status ----

function buildStatus(items) {
  statusRows.clear();
  ui.status.textContent = '';
  if (!items.length) {
    ui.status.appendChild(make('p', 'empty', 'The bridge publishes no status.'));
    return;
  }
  const groups = new Map();
  for (const item of items) {
    let grid = groups.get(item.group);
    if (!grid) {
      const box = make('div', 'sgroup');
      if (item.group) box.appendChild(make('h3', '', item.group));
      grid = make('div', 'items');
      box.appendChild(grid);
      ui.status.appendChild(box);
      groups.set(item.group, grid);
    }
    const ranged = typeof item.min === 'number' && typeof item.max === 'number' && item.max > item.min;
    const row = make('div', ranged ? 'item' : 'item wide');
    const head = make('div', 'ihead');
    const value = make('span', 'ival', '\u2014');
    head.append(make('span', 'ilabel', item.label || item.key), value);
    row.appendChild(head);
    let fill = null;
    if (ranged) {
      // The number beside it says the same thing, so the bar is decoration to a screen reader.
      const bar = make('div', 'bar');
      bar.setAttribute('aria-hidden', 'true');
      fill = make('div', 'fill');
      bar.appendChild(fill);
      if (item.min < 0 && item.max > 0) {
        const zero = make('div', 'zero');
        zero.style.left = (-item.min / (item.max - item.min) * 100) + '%';
        bar.appendChild(zero);
      }
      row.appendChild(bar);
    }
    grid.appendChild(row);
    statusRows.set(item.key, {value, fill});
  }
}

function renderStatus(doc) {
  const items = doc.status.items;
  const shape = JSON.stringify(items.map(i => [i.key, i.group, i.label, i.unit, i.min, i.max, i.decimals]));
  if (shape !== statusShape) {
    statusShape = shape;
    buildStatus(items);
  }
  for (const item of items) {
    const row = statusRows.get(item.key);
    if (!row) continue;
    const numeric = typeof item.value === 'number';
    let text = '\u2014';
    if (numeric) {
      text = item.value.toFixed(item.decimals);
      if (Number(text) === 0) text = text.replace('-', '');
      if (item.unit) text += ' ' + item.unit;
    } else if (typeof item.text === 'string') {
      text = item.text;
    }
    setText(row.value, text);
    if (row.fill) {
      let left = 0;
      let width = 0;
      if (numeric) {
        const span = item.max - item.min;
        const at = (clamp(item.value, item.min, item.max) - item.min) / span;
        if (item.min < 0 && item.max > 0) {
          // A signed range grows out from zero in either direction.
          const zero = -item.min / span;
          left = Math.min(zero, at);
          width = Math.abs(at - zero);
        } else {
          width = at;
        }
      }
      row.fill.style.left = (left * 100) + '%';
      row.fill.style.width = (width * 100) + '%';
    }
  }
}

// ---- Connection state ----

function renderConnection() {
  const now = performance.now();
  const silent = now - lastOk;
  const wasDown = down;
  down = now > graceUntil && (!state || silent > DOWN_AFTER_MS);

  let kind = '';
  let text = 'Connecting\u2026';
  let detail = '';
  let note = '';
  if (down) {
    kind = 'down';
    text = state ? 'Bridge not responding' : 'Cannot reach the bridge';
    if (state) {
      detail = ' \u00b7 ' + seconds(silent);
      note = 'The bridge stopped answering ' + seconds(silent) + ' ago. These are the last values it sent.';
    }
  } else if (state) {
    const age = state.status.age == null ? null : state.status.age + silent / 1000;
    if (age == null && state.status.items.length) {
      kind = 'stale';
      text = 'Connected, no status yet';
      note = 'The bridge is answering but has not published any status yet.';
    } else if (age != null && age > STALE_AFTER_S) {
      kind = 'stale';
      text = 'Status stale';
      detail = ' \u00b7 ' + seconds(age * 1000);
      note = 'The bridge is answering, but its main loop has published nothing for ' + seconds(age * 1000) + '.';
    } else {
      kind = 'live';
      text = 'Live';
    }
  }
  const className = kind ? 'conn ' + kind : 'conn';
  if (ui.conn.className !== className) ui.conn.className = className;
  setText(ui.connText, text);
  setText(ui.connAge, detail);
  ui.statusCard.classList.toggle('old', kind === 'stale' || kind === 'down');
  ui.statusNote.hidden = !note;
  ui.statusNote.className = down ? 'note down' : 'note';
  setText(ui.statusNote, note);

  // Nothing can be changed while the bridge is not there to change it. The release button stays
  // live regardless: pressing it is never wrong, and it says so if the request does not get through.
  ui.settingsBox.disabled = down || !state;
  ui.forces.disabled = down || !state;
  if (down !== wasDown) renderSaveBar();
}

// ---- Master switch and release ----

function paintForces(on) {
  ui.forces.checked = on;
  setText(ui.forcesText, on ? 'Forces on' : 'Forces off');
  ui.forcesBox.classList.toggle('off', !on);
}

function renderForces(doc, sentAt) {
  if (forcesBusy || sentAt < forcesTouched) return;
  paintForces(doc.forcesEnabled);
}

// These two skip the queue: stopping forces must not wait behind anything else.
ui.forces.addEventListener('change', () => {
  const on = ui.forces.checked;
  forcesTouched = performance.now();
  forcesBusy++;
  paintForces(on);
  request('POST', '/api/forces', on ? 'on' : 'off')
    .then(answer => { forcesBusy--; received(answer.doc, answer.sentAt); })
    .catch(error => {
      forcesBusy--;
      forcesTouched = 0;
      if (state) paintForces(state.forcesEnabled);
      toast('Forces not switched ' + (on ? 'on' : 'off') + ': ' + why(error), true);
    });
});

ui.release.addEventListener('click', () => {
  forcesTouched = performance.now();
  forcesBusy++;
  request('POST', '/api/release')
    .then(answer => {
      forcesBusy--;
      received(answer.doc, answer.sentAt);
      toast('Forces released and switched off. Switch forces on to resume.');
    })
    .catch(error => {
      forcesBusy--;
      forcesTouched = 0;
      toast('Release not confirmed: ' + why(error), true);
    });
});

// ---- Save, revert, reset ----

function renderSaveBar() {
  let unsaved = 0;
  let offDefault = 0;
  controls.forEach(c => {
    if (c.value !== c.saved) unsaved++;
    if (c.value !== c.meta.default) offDefault++;
  });
  const dirty = unsaved > 0;
  ui.savebar.classList.toggle('unsaved', dirty);
  setText(ui.dirtyText, dirty ? (unsaved === 1 ? '1 unsaved change' : unsaved + ' unsaved changes') : 'No unsaved changes');
  setText(ui.dirtyHint, dirty ? 'In effect now, but gone when the bridge restarts unless saved.'
                              : (state ? 'Settings file: ' + shortPath(state.settingsFile) : ''));
  ui.dirtyHint.title = state ? state.settingsFile : '';
  const idle = down || !state;
  ui.save.disabled = idle || !dirty;
  ui.revert.disabled = idle || !dirty;
  ui.reset.disabled = idle || offDefault === 0;
  const title = (dirty ? '\u2022 ' : '') + 'MOZA bridge';
  if (document.title !== title) document.title = title;
}

function act(path, failure) {
  clearTimeout(sendTimer);
  sendTimer = 0;
  enqueue(sendPending);
  enqueue(() => request('POST', path)
    .then(answer => {
      // These replace every value, including any the user touched a moment ago.
      controls.forEach(c => { c.touched = 0; });
      received(answer.doc, answer.sentAt);
      const result = answer.doc.result || {};
      const warnings = result.warnings || [];
      let text = result.message || 'Done';
      if (warnings.length) text += '. ' + (warnings.length === 1 ? 'Warning: ' : 'Warnings: ') + warnings.join('; ');
      toast(text, warnings.length > 0);
    })
    .catch(error => toast(failure + ': ' + why(error), true)));
}

// Reverting and resetting throw tuning away, so they take a second press.
function confirmed(button, question, run) {
  const label = button.textContent;
  let timer = 0;
  const disarm = () => {
    clearTimeout(timer);
    timer = 0;
    button.textContent = label;
    button.classList.remove('armed');
  };
  button.addEventListener('click', () => {
    if (timer) {
      disarm();
      run();
      return;
    }
    button.textContent = question;
    button.classList.add('armed');
    toast('Press again to confirm.');
    timer = setTimeout(disarm, 4000);
  });
  button.addEventListener('blur', disarm);
}

ui.save.addEventListener('click', () => act('/api/save', 'Not saved'));
confirmed(ui.revert, 'Discard changes?', () => act('/api/revert', 'Not reverted'));
confirmed(ui.reset, 'Reset everything?', () => act('/api/reset', 'Not reset'));

// ---- Start ----

// A slider released outside itself still ends the drag.
window.addEventListener('pointerup', () => controls.forEach(c => { c.dragging = false; }));

// A tab in the background gets its timers slowed down; do not call that a dead bridge on return.
document.addEventListener('visibilitychange', () => {
  if (!document.hidden) graceUntil = performance.now() + DOWN_AFTER_MS;
});

// The bars at the top and bottom change height when they wrap; the layout follows them.
const fit = () => {
  document.documentElement.style.setProperty('--top', ui.top.offsetHeight + 'px');
  document.documentElement.style.setProperty('--bottom', ui.savebar.offsetHeight + 'px');
};
if (window.ResizeObserver) {
  const observer = new ResizeObserver(fit);
  observer.observe(ui.top);
  observer.observe(ui.savebar);
}
window.addEventListener('resize', fit);
fit();

setInterval(renderConnection, 250);
poll();
})();
</script>
</body>
</html>
)PAGE",
};

std::string joinPage()
{
    std::string page;
    for (const char* part : kPageParts)
    {
        page += part;
    }
    return page;
}

} // namespace

const std::string& webUiPage()
{
    static const std::string page = joinPage();
    return page;
}

} // namespace bridge
