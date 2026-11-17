import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import {layout,patchImage,extractPayload,checksum,hash} from '../tools/patch-client-autoload.mjs';
const input=fs.readFileSync(new URL('../00_reference/rohanclient_unpacked.exe',import.meta.url));
const object=fs.readFileSync(new URL('../obj/Autoload/bootstrap.obj',import.meta.url));
const payload=extractPayload(object);
test('only known inputs admitted and source stays intact',()=>{
  const before=hash(input),other=Buffer.from(input);other[0x1000]^=1;
  assert.throws(()=>layout(other),/SHA-256/);
  assert.throws(()=>layout(patchImage(input,payload)),/SHA-256/);
  assert.equal(hash(input),before);
});
test('new read/execute section and headers remain aligned',()=>{
  const l=layout(input),out=patchImage(input,payload);
  assert.equal(out.readUInt16LE(l.pe+6),l.count+1);
  assert.equal(out.toString('ascii',l.header,l.header+8),'.rchboot');
  assert.equal(out.readUInt32LE(l.header+36),0x60000020);
  assert.equal(out.readUInt32LE(l.opt+16),l.rva);
  assert.equal(out.readUInt32LE(l.opt+56),l.rva+l.sectionAlign);
  assert.equal(out.readUInt32LE(l.opt+60),l.fileAlign);
  assert.ok(l.header+40<=out.readUInt32LE(l.opt+60));
  assert.equal(l.raw%l.fileAlign,0);assert.equal(l.rva%l.sectionAlign,0);
  assert.deepEqual(out.subarray(l.raw,l.raw+payload.length),payload);
  assert.equal(out.readUInt32LE(l.opt+64),checksum(out,l.opt+64));
});
test('all pre-existing game code and imports preserved',()=>{
  const l=layout(input),out=patchImage(input,payload);
  const allowed=[[l.pe+6,2],[l.opt+4,4],[l.opt+16,4],[l.opt+56,12],[l.header,40]];
  for(let i=0;i<input.length;i++) if(input[i]!==out[i])
    assert.ok(allowed.some(([start,n])=>i>=start&&i<start+n),`Unexpected edit at${i.toString(16)}`);
});
