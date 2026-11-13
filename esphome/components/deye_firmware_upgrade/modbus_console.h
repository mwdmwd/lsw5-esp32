// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace esphome::deye_firmware_upgrade
{
static constexpr char MODBUS_CONSOLE_HTML[] = R"HTML(<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <title>Inverter Modbus console</title>
    <style>
      body { margin: 1em; max-width: 80em; }
      fieldset { margin: 1em 0; }
      button { margin: .15em; }
      input, select, textarea { font: inherit; }
      .fields { display: flex; flex-wrap: wrap; gap: .75em; align-items: end; }
      .fields label { display: flex; flex-direction: column; gap: .2em; }
      .number { width: 8em; }
      #values, #request { font-family: monospace; box-sizing: border-box; width: 100%; }
      #request { margin: .5em 0; }
      #valuesLabel { display: block; margin: .75em 0; }
      #valuesLabel[hidden] { display: none; }
      pre { overflow: auto; }
      #tx, #rx { white-space: pre-wrap; overflow-wrap: anywhere; }
      #terminal { height: 12em; min-height: 5em; resize: vertical; white-space: pre-wrap; overflow-wrap: anywhere; padding: .75em; background: #171717; color: #eee; border: 1px solid #666; }
      .table { overflow-x: auto; max-height: 32em; }
      table { border-collapse: collapse; font-variant-numeric: tabular-nums; }
      th, td { padding: .35em .8em; border: 1px solid #aaa; text-align: right; }
      thead { position: sticky; top: 0; background: #eee; }
      #error { color: #a00; white-space: pre-wrap; }
      #error:empty { display: none; }
    </style>
  </head>
  <body>
    <h1>Inverter Modbus console</h1>
    <p>
      <a href="/">ESPHome</a> | <a href="/memory">Memory console</a> | <a href="/firmware">Firmware</a>
    </p>
    <p id="error" role="alert"></p>
    <fieldset>
      <legend>Registers</legend>
      <form id="registerForm">
        <div class="fields">
          <label>Unit <input id="unit" class="number" value="1" required spellcheck="false"></label>
          <label>
            Function
            <select id="function">
              <option value="3">03 · Read holding registers</option>
              <option value="4">04 · Read input registers</option>
              <option value="6">06 · Write single register</option>
              <option value="16">16 / 0x10 · Write multiple registers</option>
            </select>
          </label>
          <label>
            Start register
            <input id="address" class="number" value="500" required spellcheck="false">
          </label>
          <label>
            Count <input id="count" class="number" value="1" required spellcheck="false">
          </label>
        </div>
        <label id="valuesLabel" hidden>
          Values (decimal, signed decimal, or 0x-prefixed hex; separated by spaces or commas)<input id="values" placeholder="123 0x0042 -1" spellcheck="false" autocomplete="off">
        </label>
        <p>
          Use register numbers directly (r500 = 500), in decimal or <code>0x</code>-prefixed hex.
          Values are unscaled 16-bit words. Writes take effect on the inverter.
        </p>
        <button class="send" id="registerSend" type="submit" disabled>Read registers</button>
      </form>
    </fieldset>
    <fieldset>
      <legend>Raw request</legend>
      <form id="rawForm">
        <label for="request">Hex bytes: unit, function, and data</label>
        <textarea id="request" rows="3" maxlength="1024" required spellcheck="false" autocapitalize="off" autocomplete="off">01 03 01 f4 00 01</textarea>
        <label><input id="appendCrc" type="checkbox" checked> Append Modbus CRC16</label>
        <p>
          Use compact or space-separated hex. If Append CRC is unchecked, include the CRC bytes.
          Maximum 256 bytes, including CRC.
        </p>
        <button class="send" type="submit" disabled>Send raw request</button>
      </form>
    </fieldset>
    <p>
      <label>Response timeout
      <input id="timeout" type="number" min="100" max="10000" step="1" value="1000" class="number">
      ms</label> <button id="cancel" disabled>Cancel request</button>
    </p>
    <p>
      Requests wait for an idle bus and are sent once. Normal polling resumes after the reply or
      timeout. Finish any memory capture or firmware transfer first.
    </p>
    <p><strong id="message" role="status">Connecting…</strong><br><span id="metrics">—</span></p>
    <h2>Register view</h2>
    <p id="decoded">No response yet.</p>
    <div class="table">
      <table id="registers" hidden>
        <thead>
          <tr>
            <th scope="col">Register</th>
            <th scope="col">Hex</th>
            <th scope="col">Unsigned</th>
            <th scope="col">Signed</th>
          </tr>
        </thead>
        <tbody id="rows">
        </tbody>
      </table>
    </div>
    <h2>Raw frames</h2>
    <p id="txLabel">TX (including CRC)</p>
    <pre id="tx" tabindex="0">—</pre>
    <p>RX (including CRC) · <span id="crc">—</span></p>
    <pre id="rx" tabindex="0">—</pre>
    <h2>Session log</h2>
    <pre id="terminal" role="log" tabindex="0" aria-label="Modbus transaction log">The latest transaction is retained on the ESP32 until the next request or restart.</pre>
    <button id="clearLog">Clear log</button>
    <p><a href="/modbus/status">Request status JSON</a></p>
    <noscript>This console requires JavaScript.</noscript>
    <script>
'use strict';
const $ = id => document.getElementById(id);
const hex = (n, digits = 2) => n.toString(16).padStart(digits, '0');
const bytes = text => text ? text.trim().split(/\s+/).map(b => parseInt(b, 16)) : [];
const word = (data, i) => data[i] * 256 + data[i + 1];
const pair = n => [n >> 8, n & 255];
let state = null, busy = false, revision = 0, logged = null;
function number(text, min, max, name) {
  text = text.trim();
  if (!/^(0x[0-9a-f]+|-?[0-9]+)$/i.test(text)) throw Error(`${name}: use decimal or 0x-prefixed hex.`);
  const n = Number(text);
  if (!Number.isInteger(n) || n < min || n > max) throw Error(`${name}: use ${min} through ${max}.`);
  return n;
}
function registerRequest(unitText, functionText, addressText, countText, valuesText) {
  const unit = number(unitText, 1, 247, 'Unit'), fn = Number(functionText);
  const address = number(addressText, 0, 65535, 'Start register');
  if (fn === 3 || fn === 4) {
    const count = number(countText, 1, 125, 'Count');
    if (address + count > 65536) throw Error('Register range exceeds 65535.');
    return [unit, fn, ...pair(address), ...pair(count)];
  }
  if (fn !== 6 && fn !== 16) throw Error('Unsupported register function.');
  const values = valuesText.trim().split(/[\s,]+/).filter(Boolean).map(v => number(v, -32768, 65535, 'Value') & 65535);
  if (!values.length || values.length > (fn === 6 ? 1 : 123)) throw Error(fn === 6 ? 'Enter exactly one value.' : 'Enter 1 through 123 values.');
  if (address + values.length > 65536) throw Error('Register range exceeds 65535.');
  return fn === 6 ? [unit, fn, ...pair(address), ...pair(values[0])] :
    [unit, fn, ...pair(address), ...pair(values.length), values.length * 2, ...values.flatMap(pair)];
}
function decodeRegisters(s) {
  const tx = bytes(s.request), rx = bytes(s.response);
  const result = {message:'No response yet.', rows:[]};
  if (s.active) { result.message = 'Waiting for the complete reply…'; return result; }
  if (!rx.length) return result;
  if (!s.crc_valid || s.overflow || s.timed_out || s.cancelled) {
    result.message = 'Capture is incomplete or invalid; inspect the raw bytes below.'; return result;
  }
  if (rx[0] !== tx[0] || (rx[1] !== tx[1] && rx[1] !== (tx[1] | 128))) {
    result.message = 'Reply unit or function differs from the request; inspect the raw bytes below.'; return result;
  }
  if (rx[1] & 128) {
    const errors = {1:'Illegal function', 2:'Illegal data address', 3:'Illegal data value', 4:'Device failure', 5:'Acknowledge', 6:'Device busy', 8:'Memory parity error', 10:'Gateway path unavailable', 11:'Gateway target did not respond'};
    result.message = rx.length === 5 ? `Modbus exception 0x${hex(rx[2])}: ${errors[rx[2]] || 'Unknown exception'}.` : 'Malformed Modbus exception; inspect the raw bytes below.';
    return result;
  }
  const fn = tx[1], start = word(tx, 2), count = word(tx, 4);
  let values;
  if ((fn === 3 || fn === 4) && tx.length === 8 && count >= 1 && count <= 125 &&
      rx[2] === count * 2 && rx.length === 5 + count * 2) {
    values = Array.from({length:count}, (_, i) => word(rx, 3 + i * 2));
    result.message = `${count} ${fn === 3 ? 'holding' : 'input'} register${count === 1 ? '' : 's'} from unit ${rx[0]}. Values are unscaled.`;
  } else if (fn === 6 && tx.length === 8 && rx.length === 8 && tx.every((b, i) => b === rx[i])) {
    values = [count];
    result.message = 'Single-register write acknowledged. Requested value shown; read back to verify.';
  } else if (fn === 16 && count >= 1 && count <= 123 && tx.length === 9 + count * 2 &&
             tx[6] === count * 2 && rx.length === 8 && rx.slice(2, 6).every((b, i) => b === tx[i + 2])) {
    values = Array.from({length:count}, (_, i) => word(tx, 7 + i * 2));
    result.message = `${count}-register write acknowledged. Requested values shown; read back to verify.`;
  } else {
    result.message = 'No matching register response to decode. Raw bytes are available below.'; return result;
  }
  if (start + values.length > 65536) { result.message = 'Register range exceeds 65535.'; return result; }
  result.rows = values.map((value, i) => ({address:start + i, value, signed:value >= 32768 ? value - 65536 : value}));
  return result;
}
function controls() {
  document.querySelectorAll('.send').forEach(button => button.disabled = busy || !state || state.active);
  $('cancel').disabled = busy || !state || !state.active;
}
function update(s) {
  state = s; controls();
  $('message').textContent = s.message;
  $('metrics').textContent = s.request_id ? `Request #${s.request_id} · ${s.elapsed_ms} ms · ${bytes(s.request).length} TX bytes · ${bytes(s.response).length} RX bytes` : 'No request sent.';
  $('txLabel').textContent = s.sent ? 'TX (including CRC)' : 'Queued request (including CRC; not sent)';
  $('tx').textContent = s.request || '—'; $('rx').textContent = s.response || '—';
  $('crc').textContent = s.crc_valid === null ? 'CRC not available' : s.crc_valid ? 'CRC valid' : 'CRC invalid';
  const decoded = decodeRegisters(s);
  $('decoded').textContent = decoded.message; $('rows').replaceChildren();
  for (const row of decoded.rows) {
    const tr = document.createElement('tr');
    [ `${row.address} (0x${hex(row.address, 4)})`, '0x' + hex(row.value, 4), row.value, row.signed ].forEach((value, i) => {
      const cell = document.createElement(i ? 'td' : 'th');
      if (!i) cell.scope = 'row';
      cell.textContent = value; tr.append(cell);
    });
    $('rows').append(tr);
  }
  $('registers').hidden = !decoded.rows.length;
  if (s.request_id && !s.active && logged !== s.request_id) {
    logged = s.request_id;
    const el = $('terminal');
    el.textContent = (el.textContent + `\n\n#${s.request_id} · ${s.elapsed_ms} ms · ${s.message}\n${s.sent ? 'TX' : 'Not sent'}: ${s.request}\nRX: ${s.response || '(none)'}\n${decoded.message}`).slice(-24000);
    el.scrollTop = el.scrollHeight;
  }
}
async function api(action, args) {
  const response = await fetch('/modbus/' + action, args === undefined ? {cache:'no-store'} : {method:'POST', body:new URLSearchParams(args)});
  const result = await response.json();
  if (!response.ok || result.error) throw Error(result.error || `HTTP ${response.status}`);
  return result;
}
async function send(frame, appendCrc) {
  if (busy || (state && state.active)) throw Error('A request is already running.');
  const timeout = number($('timeout').value, 100, 10000, 'Response timeout');
  busy = true; ++revision; controls();
  try { update(await api('send', {hex:frame, append_crc:appendCrc ? '1' : '0', timeout_ms:timeout})); }
  finally { busy = false; controls(); }
}
function action(fn) { return async event => { event.preventDefault(); $('error').textContent = ''; try { await fn(); } catch (error) { $('error').textContent = error.message || String(error); } }; }
$('rawForm').addEventListener('submit', action(() => send($('request').value, $('appendCrc').checked)));
$('registerForm').addEventListener('submit', action(async () => {
  const frame = registerRequest($('unit').value, $('function').value, $('address').value, $('count').value, $('values').value).map(b => hex(b)).join(' ');
  $('request').value = frame; $('appendCrc').checked = true;
  await send(frame, true);
}));
$('function').addEventListener('change', () => {
  const writing = Number($('function').value) > 4;
  $('valuesLabel').hidden = !writing; $('values').required = writing;
  $('count').disabled = writing; $('registerSend').textContent = writing ? 'Write registers' : 'Read registers';
});
$('cancel').addEventListener('click', action(async () => {
  if (!state || !state.active || busy) return;
  busy = true; ++revision; controls();
  try { update(await api('cancel', {request_id:state.request_id})); }
  finally { busy = false; controls(); }
}));
$('clearLog').addEventListener('click', () => { $('terminal').textContent = ''; });
async function poll() {
  try {
    if (!busy) {
      const requestRevision = revision, s = await api('status');
      if (requestRevision === revision && !busy) update(s);
    }
  } catch (error) { $('error').textContent = error.message || String(error); }
  setTimeout(poll, state && state.active ? 250 : 2000);
}
poll();
    </script>
  </body>
</html>)HTML";
} // namespace esphome::deye_firmware_upgrade
