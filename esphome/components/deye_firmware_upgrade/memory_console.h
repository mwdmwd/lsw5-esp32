// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace esphome::deye_firmware_upgrade
{
static constexpr char MEMORY_CONSOLE_HTML[] = R"HTML(<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <title>Inverter memory console</title>
    <style>
      body { margin: 1em; max-width: 80em; }
      pre { overflow: auto; }
      #terminal { height: 16em; min-height: 6em; resize: vertical; white-space: pre; padding: .75em; margin: 0 0 .5em; background: #171717; color: #eee; border: 1px solid #666; box-sizing: border-box; }
      #commandForm { display: flex; align-items: center; gap: .4em; font-family: monospace; }
      #command { flex: 1; min-width: 0; font: inherit; }
      #goto { width: 9em; font-family: monospace; }
      fieldset { margin: 1em 0; }
      button { margin: .15em; }
      #error { color: #a00; white-space: pre-wrap; }
      #error:empty { display: none; }
    </style>
  </head>
  <body>
    <h1>Inverter memory console</h1>
    <p>
      <a href="/">ESPHome</a> | <a href="/firmware">Firmware</a> | <a href="/modbus">Modbus console</a>
      | <span id="unit">Connecting…</span>
    </p>
    <p id="error" role="alert"></p>
    <fieldset>
      <legend>Capture...</legend>
      <button class="start" data-region="flash">
        Full flash (<span id="flashSize">512 KiB</span>)
      </button>
      <button class="start" data-region="app">Application</button>
      <button class="start" data-region="ram">SRAM (192 KiB)</button>
      <button class="start" data-region="tcm">TCM RAM (64 KiB)</button>
      <p>
        Reads run in the background alongside normal polling. A new read replaces the previous
        capture. RAM is read live.
      </p>
    </fieldset>
    <p><span id="message" role="status">Connecting…</span><br><span id="metrics">—</span></p>
    <p>
      <progress id="progress" max="1" value="0" aria-label="Capture progress"></progress>
      <button id="cancel" disabled>Cancel</button>
      <button id="download" disabled>Download .bin</button>
      <button id="clear" disabled>Clear capture</button>
    </p>
    <h2>Hexdump</h2>
    <form id="gotoForm">
      <label for="goto">Address: </label>
      <input id="goto" placeholder="0x0800c000" aria-label="Go to captured address" spellcheck="false">
      <button>Go</button>
      <button id="copy" type="button" disabled>Copy text</button>
    </form>
    <pre id="hex" tabindex="0" aria-label="Memory hexdump">No capture yet.</pre>
    <p>
      <button id="previous" disabled>← Previous</button>
      <button id="next" disabled>Next →</button>
      <span id="pageInfo"></span>
    </p>
    <details>
      <summary>Command reference</summary>
      <pre>mdb ADDRESS [COUNT]       Display bytes (default: 1)
mdh ADDRESS [COUNT]       Display 16-bit halfwords (default: 1)
mdw ADDRESS [COUNT]       Display 32-bit words (default: 1)
mdd ADDRESS [COUNT]       Display 64-bit doublewords (default: 1)
hd ADDRESS [BYTES]        hexdump (default: 256 bytes)
dump ADDRESS BYTES        Capture a custom range into PSRAM
capture flash|app|ram|tcm  Capture a complete region
save / cancel / clear    Download, stop or free the capture
help                     Show this reference</pre>
      <p>
        Numbers are decimal or <code>0x</code>-prefixed hex. The <code>md*</code> count is in units
        of the selected width, as in OpenOCD. Display commands read up to 4096 fresh bytes; the
        pager uses the saved capture. HMI addresses count bytes. Lengths and download offsets count
        bytes. On HMI, <code>mdw 0x1fff7a20</code> reads the GD32 density word.
      </p>
    </details>
    <p><a href="/memory/status">Capture status JSON</a></p>
    <h2>Console</h2>
    <pre id="terminal" role="log" aria-label="Command output" tabindex="0">Type help for commands. ↑ / ↓ recalls command history.</pre>
    <form id="commandForm">
      <label for="command">hmi &gt; </label>
      <input id="command" aria-label="Command" autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="mdw 0x0800c000">
      <button type="submit">Run</button>
    </form>
    <script>
'use strict';
const $ = id => document.getElementById(id);
const hex = (n, digits = 8) => n.toString(16).padStart(digits, '0');
const size = n => n >= 1024 ? `${(n / 1024).toLocaleString(undefined, {maximumFractionDigits: 1})} KiB` : `${n} B`;
const duration = seconds => { const n = Math.max(0, Math.ceil(seconds)); return n >= 3600 ? `${Math.floor(n/3600)}h ${Math.floor(n%3600/60)}m` : n >= 60 ? `${Math.floor(n/60)}m ${n%60}s` : `${n}s`; };
let state = null, busy = false, pageOffset = 0, pageData = new Uint8Array(), pageKey = '', pending = null;
let history = [], historyIndex = 0, draft = '', revision = 0, pageRevision = 0;
function number(text) {
  if (!/^(0x[0-9a-f]+|[0-9]+)$/i.test(text || '')) throw Error('Use decimal or 0x-prefixed hex numbers.');
  const n = Number(text);
  if (!Number.isSafeInteger(n) || n < 0 || n > 0xffffffff) throw Error('Number is outside the 32-bit range.');
  return n;
}
function output(text) {
  const el = $('terminal');
  el.textContent = (el.textContent + '\n' + text).slice(-24000);
  el.scrollTop = el.scrollHeight;
}
function report(error) { $('error').textContent = error.message || String(error); }
async function api(action, args) {
  const response = await fetch('/memory/' + action, args === undefined ? {cache:'no-store'} : {method:'POST', body:new URLSearchParams(args)});
  const result = await response.json();
  if (!response.ok || result.error) throw Error(result.error || `HTTP ${response.status}`);
  return result;
}
async function bytes(job, offset, length) {
  const response = await fetch(`/memory/data?job=${job}&offset=${offset}&length=${length}`, {cache:'no-store'});
  if (!response.ok) { const result = await response.json(); throw Error(result.error || `HTTP ${response.status}`); }
  return new Uint8Array(await response.arrayBuffer());
}
function hexDump(data, address) {
  if (!data.length) return '';
  const lines = [];
  let previous = '', repeated = false;
  for (let row = 0; row < data.length; row += 16) {
    const part = data.subarray(row, row + 16);
    const bytes = Array.from(part, b => hex(b, 2)), key = bytes.join('');
    if (part.length === 16 && key === previous) {
      if (!repeated) lines.push('*');
      repeated = true; continue;
    }
    previous = part.length === 16 ? key : ''; repeated = false;
    const left = bytes.slice(0, 8).join(' ').padEnd(23);
    const right = bytes.slice(8).join(' ').padEnd(23);
    const ascii = Array.from(part, b => b >= 32 && b < 127 ? String.fromCharCode(b) : '.').join('');
    lines.push(`${hex(address + row)}  ${left}  ${right}  |${ascii}|`);
  }
  lines.push(hex(address + data.length));
  return lines.join('\n');
}
function format(data, address, width) {
  if (!width) return hexDump(data, address);
  const lines = [];
  for (let row = 0; row < data.length; row += 32) {
    const part = data.subarray(row, row + 32), values = [];
    for (let i = 0; i < part.length; i += width) {
      let value = 0n;
      const count = Math.min(width, part.length - i);
      for (let j = count - 1; j >= 0; --j) value = (value << 8n) | BigInt(part[i+j]);
      values.push(hex(value, count * 2));
    }
    lines.push(`0x${hex(address + row)}: ${values.join(' ')}`);
  }
  return lines.join('\n');
}
function renderPage() {
  if (!state || !pageData.length) return;
  $('hex').textContent = hexDump(pageData, state.address + pageOffset);
  $('goto').value = '0x' + hex(state.address + pageOffset);
  $('pageInfo').textContent = `0x${hex(state.address + pageOffset)}–0x${hex(state.address + pageOffset + pageData.length - 1)} · ${pageData.length} bytes`;
  $('copy').disabled = false;
}
async function refreshPage() {
  if (!state) return;
  const s = state;
  const length = Math.min(256, Math.max(0, s.received - pageOffset));
  const key = `${s.job_id}:${pageOffset}:${length}`;
  $('previous').disabled = pageOffset === 0;
  $('next').disabled = pageOffset + 256 >= s.received;
  if (!length || key === pageKey) return;
  const requestRevision = ++pageRevision, offset = pageOffset;
  const data = await bytes(s.job_id, offset, length);
  if (requestRevision !== pageRevision || state.job_id !== s.job_id || pageOffset !== offset) return;
  pageKey = key; pageData = data; renderPage();
}
async function update(s) {
  if (!state || state.job_id !== s.job_id) {
    pageOffset = 0; pageKey = ''; pageData = new Uint8Array();
    $('hex').textContent = s.active ? 'Waiting for the first bytes…' : 'No captured bytes.';
    $('pageInfo').textContent = ''; $('copy').disabled = true;
  }
  state = s;
  $('unit').textContent = `HMI unit ${s.unit}`;
  $('flashSize').textContent = size(s.flash_size);
  $('message').textContent = s.message;

  $('progress').max = s.length || 1; $('progress').value = s.received;
  const rate = s.elapsed_ms > 0 ? s.received * 1000 / s.elapsed_ms : 0;
  $('metrics').textContent = s.length ? `${s.received.toLocaleString()} / ${s.length.toLocaleString()} B · ${(100*s.received/s.length).toFixed(1)}% · ${duration(s.elapsed_ms/1000)}${s.active && rate ? ` · ${rate.toFixed(0)} B/s · ~${duration((s.length-s.received)/rate)} left` : ''}` : 'No capture in PSRAM';
  $('cancel').disabled = !s.active; $('clear').disabled = s.active || !s.length;
  $('download').disabled = !s.received; $('download').textContent = s.complete ? 'Download .bin' : 'Download partial';
  document.querySelectorAll('.start').forEach(button => button.disabled = s.active || busy);
  await refreshPage();
  if (pending && pending.job === s.job_id && !s.active) {
    const done = pending; pending = null;
    output(`${s.message} ${s.received} / ${s.length} bytes.`);
    if (s.complete && done.display) {
      const data = await bytes(s.job_id, done.address - s.address, done.length);
      output(format(data, done.address, done.width));
    }
  } else if (pending && pending.job !== s.job_id) { pending = null; output('Capture replaced by another request.'); }
}
async function start(address, length, display = false, width = 0) {
  if (busy || (state && state.active)) throw Error('A capture is already running. Cancel it first.');
  if (!length || address + length > 0x100000000) throw Error('Invalid capture range.');
  if (display && length > 4096) throw Error('Display up to 4096 bytes, or use dump for a larger capture.');
  busy = true; ++revision;
  try {
    const s = await api('read', {address:'0x' + hex(address), length});
    pending = {job:s.job_id, address, length, display, width};

    output(`Reading ${size(length)} at 0x${hex(address)}…`);
    await update(s);
  } finally { busy = false; }
}
async function capture(region) {
  if (!state) throw Error('Still connecting.');
  const ranges = {flash:[0x08000000, state.flash_size], app:[0x0800c000, state.flash_size-0xc000], ram:[0x20000000, 192*1024], tcm:[0x10000000, 64*1024]};
  if (!ranges[region]) throw Error('Choose flash, app, ram or tcm.');
  await start(...ranges[region]);
}
function download() {
  if (!state || !state.received) throw Error('No captured bytes to download.');
  const a = document.createElement('a');
  a.href = '/memory/download?job=' + state.job_id;
  a.download = ''; document.body.append(a); a.click(); a.remove();
}
async function control(action) {
  if (!state) throw Error('Still connecting.');
  ++revision;
  await update(await api(action, {job:state.job_id}));
}
async function command(text) {
  const [cmd, ...args] = text.trim().toLowerCase().split(/\s+/);
  const widths = {mdb:1, mdh:2, mdw:4, mdd:8};
  if (cmd === 'help') { output(document.querySelector('details pre').textContent); return; }
  if (cmd === 'save') { download(); return; }
  if (cmd === 'cancel' || cmd === 'clear') { await control(cmd); return; }
  if (cmd === 'capture' && args.length === 1) { await capture(args[0]); return; }
  if (Object.hasOwn(widths, cmd)) {
    if (args.length < 1 || args.length > 2) throw Error(`Usage: ${cmd} ADDRESS [COUNT]`);
    const address = number(args[0]), width = widths[cmd], count = args.length === 2 ? number(args[1]) : 1;
    if (count) await start(address, count * width, true, width);
    return;
  }
  if ((cmd === 'hd' || cmd === 'hexdump' || cmd === 'dump') && args.length >= 1 && args.length <= 2) {
    if (cmd === 'dump' && args.length !== 2) throw Error('Usage: dump ADDRESS BYTES');
    await start(number(args[0]), args.length === 2 ? number(args[1]) : 256, cmd !== 'dump'); return;
  }
  throw Error('Unknown command or arguments. Type help.');
}
function action(fn) { return async event => { if (event) event.preventDefault(); $('error').textContent = ''; try { await fn(); } catch (error) { report(error); } }; }
$('commandForm').addEventListener('submit', action(async () => {
  const text = $('command').value.trim(); if (!text) return;
  history.push(text); history = history.slice(-50); historyIndex = history.length; draft = '';
  $('command').value = ''; output('hmi > ' + text); await command(text);
}));
$('command').addEventListener('keydown', event => {
  if (event.key !== 'ArrowUp' && event.key !== 'ArrowDown') return;
  event.preventDefault(); if (historyIndex === history.length) draft = $('command').value;
  historyIndex = Math.max(0, Math.min(history.length, historyIndex + (event.key === 'ArrowUp' ? -1 : 1)));
  $('command').value = historyIndex === history.length ? draft : history[historyIndex];
});
document.querySelectorAll('[data-region]').forEach(button => button.addEventListener('click', action(() => capture(button.dataset.region))));
$('download').addEventListener('click', action(download));
$('cancel').addEventListener('click', action(() => control('cancel')));
$('clear').addEventListener('click', action(() => control('clear')));

$('previous').addEventListener('click', action(async () => { pageOffset = Math.max(0, pageOffset-256); await refreshPage(); }));
$('next').addEventListener('click', action(async () => { pageOffset += 256; await refreshPage(); }));
$('gotoForm').addEventListener('submit', action(async () => {
  const address = number($('goto').value);
  if (!state || address < state.address || address >= state.address + state.received) throw Error('Choose an address within the captured bytes.');
  pageOffset = Math.floor((address-state.address)/16)*16; await refreshPage();
}));
$('copy').addEventListener('click', action(async () => {
  const text = $('hex').textContent;
  if (navigator.clipboard && window.isSecureContext) await navigator.clipboard.writeText(text);
  else { const area = document.createElement('textarea'); area.value = text; area.style.position = 'fixed'; area.style.opacity = '0'; document.body.append(area); area.select(); const ok = document.execCommand('copy'); area.remove(); if (!ok) throw Error('Select the hexdump text to copy it.'); }
  output('Hexdump copied.');
}));
async function poll() {
  try {
    if (!busy) {
      const requestRevision = revision, s = await api('status');
      if (requestRevision === revision && !busy) await update(s);
    }
  } catch (error) { report(error); }
  setTimeout(poll, state && state.active ? 1000 : 3000);
}
poll();
    </script>
  </body>
</html>)HTML";
} // namespace esphome::deye_firmware_upgrade
