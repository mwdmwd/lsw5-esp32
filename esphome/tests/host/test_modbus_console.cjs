// SPDX-License-Identifier: GPL-3.0-or-later
const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const {join} = require('node:path');
const {test} = require('node:test');
const {runInNewContext} = require('node:vm');

const source = readFileSync(join(__dirname, '../../components/deye_firmware_upgrade/modbus_console.h'), 'utf8').match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = new Map();
function element() {
  return {value:'', children:[], hidden:false, textContent:'', addEventListener() {},
    append(child) { this.children.push(child); }, replaceChildren() { this.children = []; }};
}
const document = {
  getElementById(id) { if (!elements.has(id)) elements.set(id, element()); return elements.get(id); },
  querySelectorAll() { return []; },
  createElement:element,
};
const api = runInNewContext(source + '\n({registerRequest, decodeRegisters, update});', {
  document, fetch:async () => { throw Error('Offline test'); }, setTimeout() {},
});
const build = (...args) => Array.from(api.registerRequest(...args));
const decode = s => JSON.parse(JSON.stringify(api.decodeRegisters(s)));
const snapshot = (request, response, extra = {}) => ({request_id:1, active:false, sent:true,
  crc_valid:true, elapsed_ms:50, message:'Reply received.', request, response, ...extra});

test('register builder uses zero-based addresses, big-endian words, and decimal or hex inputs', () => {
  assert.deepEqual(build('1', '3', '500', '2', ''), [1, 3, 1, 244, 0, 2]);
  assert.deepEqual(build('0xaa', '4', '0xffff', '1', ''), [170, 4, 255, 255, 0, 1]);
  assert.deepEqual(build('1', '6', '500', '', '-1'), [1, 6, 1, 244, 255, 255]);
  assert.deepEqual(build('1', '16', '500', '', '123, 0x8000 -32768'),
    [1, 16, 1, 244, 0, 3, 6, 0, 123, 128, 0, 128, 0]);
});

test('invalid register requests cannot silently truncate addresses, counts, or values', () => {
  for (const args of [
    ['0', '3', '500', '1', ''], ['248', '3', '500', '1', ''],
    ['1', '3', '65536', '1', ''], ['1', '3', '65535', '2', ''],
    ['1', '3', '1e2', '1', ''], ['1', '3', '500', '0', ''],
    ['1', '3', '500', '126', ''], ['1', '6', '500', '', ''],
    ['1', '6', '500', '', '1 2'], ['1', '6', '500', '', '65536'],
    ['1', '6', '500', '', '-32769'], ['1', '6', '500', '', '1.5'],
    ['1', '16', '65535', '', '1 2'], ['1', '16', '500', '', '1 '.repeat(124)],
  ]) assert.throws(() => build(...args), undefined, args.join(' / '));
});

test('register view decodes signed and unsigned reads sent through either form', () => {
  const s = snapshot('01 03 01 f4 00 02 85 c5', '01 03 04 00 02 ff ff 00 00');
  assert.deepEqual(decode(s).rows, [
    {address:500, value:2, signed:2}, {address:501, value:65535, signed:-1},
  ]);
  const input = snapshot('01 04 00 00 00 01 00 00', '01 04 02 80 00 00 00');
  assert.deepEqual(decode(input).rows, [{address:0, value:32768, signed:-32768}]);
  assert.match(decode(input).message, /input register/);
});

test('writes show acknowledged requested values and require a matching echo', () => {
  const single = snapshot('01 06 01 f4 ff ff 00 00', '01 06 01 f4 ff ff 00 00');
  assert.deepEqual(decode(single).rows, [{address:500, value:65535, signed:-1}]);
  assert.match(decode(single).message, /read back to verify/);
  const multi = snapshot('01 10 01 f4 00 02 04 00 01 80 00 00 00', '01 10 01 f4 00 02 00 00');
  assert.deepEqual(decode(multi).rows, [
    {address:500, value:1, signed:1}, {address:501, value:32768, signed:-32768},
  ]);
  assert.equal(decode({...single, response:'01 06 01 f4 00 00 00 00'}).rows.length, 0);
  assert.equal(decode({...multi, response:'01 10 01 f4 00 01 00 00'}).rows.length, 0);
});

test('exceptions, bad CRCs, partial captures, and mismatched frames never become register values', () => {
  const s = snapshot('01 03 01 f4 00 01 00 00', '01 03 02 00 02 00 00');
  assert.match(decode({...s, response:'01 83 02 c0 f1'}).message, /Illegal data address/);
  for (const extra of [
    {active:true}, {crc_valid:false}, {timed_out:true}, {cancelled:true}, {overflow:true},
    {response:'02 03 02 00 02 00 00'}, {response:'01 04 02 00 02 00 00'},
    {response:'01 03 04 00 02 00 00'}, {response:'01 03 02 00 00'},
    {response:'01 83 02 00 00 00'}, {request:'01 41 00 00'},
  ]) assert.equal(decode({...s, ...extra}).rows.length, 0, JSON.stringify(extra));
});

test('UI renders register values as text and removes stale rows on the next request', () => {
  api.update(snapshot('01 03 01 f4 00 01 00 00', '01 03 02 ff ff 00 00'));
  const rows = elements.get('rows');
  assert.equal(elements.get('registers').hidden, false);
  assert.deepEqual(rows.children[0].children.map(c => c.textContent), ['500 (0x01f4)', '0xffff', 65535, -1]);
  api.update(snapshot('01 03 01 f4 00 01 00 00', '', {request_id:2, active:true, sent:false}));
  assert.equal(rows.children.length, 0);
  assert.equal(elements.get('registers').hidden, true);
  assert.match(elements.get('txLabel').textContent, /not sent/);
});
