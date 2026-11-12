// SPDX-License-Identifier: GPL-3.0-or-later
const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const {join} = require('node:path');
const {test} = require('node:test');
const {runInNewContext} = require('node:vm');
const source = readFileSync(join(__dirname, '../../components/deye_firmware_upgrade/memory_console.h'), 'utf8').match(/<script>([\s\S]*?)<\/script>/)[1];

function rig() {
  const elements = new Map(), requests = [];
  const regions = ['flash', 'app', 'ram', 'tcm', 'boot'].map(region => ({dataset:{region}, addEventListener() {}}));
  const document = {
    getElementById(id) {
      if (!elements.has(id)) elements.set(id, {value:id === 'target' ? 'hmi' : '', textContent:'', addEventListener() {}});
      return elements.get(id);
    },
    querySelectorAll() { return regions; },
  };
  const snapshot = {job_id:0, active:false, length:0, received:0, address:0, unit:1, target:'hmi', flash_size:524288};
  const api = runInNewContext(source + '\n({hexDump, format, start, capture, update, updateTarget});', {
    document, URLSearchParams, setTimeout() {},
    fetch:async (url, options) => {
      if (options && options.method === 'POST') {
        const p = Object.fromEntries(options.body);
        requests.push({url, ...p});
        Object.assign(snapshot, {job_id:1, address:Number(p.address), length:Number(p.length), target:p.target, address_unit_bytes:p.target === 'dsp1175' ? 2 : 1});
      }
      return {ok:true, json:async () => ({...snapshot})};
    },
  });
  return {api, elements, requests, regions, snapshot};
}

test('memory hexdump uses C28 word addresses while downloads retain byte order', () => {
  const {api} = rig(), data = Uint8Array.from({length:32}, (_, i) => i);
  assert.match(api.hexDump(data, 0x84000, 2), /^00084008  /m);
  assert.match(api.hexDump(data, 0x84000, 2), /\n00084010$/);
  assert.match(api.hexDump(data, 0x0800c000), /^0800c010  /m);
  assert.match(api.format(data.subarray(0, 4), 0x84000, 2, 2), /0100 0302/);
});

test('DSP UI sends word address and byte count, validates extent, and offers only supported regions', async () => {
  const r = rig();
  await r.api.update({...r.snapshot});
  r.elements.get('target').value = 'dsp1175';
  r.api.updateTarget();
  assert.deepEqual(r.regions.filter(b => !b.hidden).map(b => b.dataset.region), ['app', 'boot']);
  await assert.rejects(r.api.start(0x84000, 3), /even byte length/);
  await assert.rejects(r.api.start(0xa95ff, 4), /within that range/);
  await assert.rejects(r.api.start(0xdf9c, 2), /within that range/);
  await assert.rejects(r.api.start(0x9000, 2), /within that range/);
  await assert.rejects(r.api.start(0x5d000, 2), /within that range/);
  await r.api.start(0x84001, 2);
  assert.deepEqual(r.requests[0], {url:'/memory/read', address:'0x00084001', length:'2', target:'dsp1175'});
  await r.api.capture('app');
  assert.equal(r.requests[1].address, '0x00084000');
  assert.equal(r.requests[1].length, '306176');
  await assert.rejects(r.api.capture('flash'), /selected processor/);
});
