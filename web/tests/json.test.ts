import test from 'node:test';
import assert from 'node:assert/strict';
import { parseJson } from '../shared/json.ts';
test('reject duplicate keys including escaped spellings at all scopes',()=>{
  for(const text of ['{"program":1,"program":2}','{"program":1,"\\u0070rogram":2}','{"trace":{"status":1,"status":2}}']) assert.throws(()=>parseJson(text),/Duplicate/);
});
test('accept separate object keys and braces/escaped quotes inside strings',()=>{
  const value={objects:[{name:'a'},{name:'b'}],label:'escaped " quotes \\ {} [,]'};
  assert.deepEqual(parseJson(JSON.stringify(value)),value);
});
test('bound containers while permitting flat arrays and reject malformed syntax',()=>{
  assert.throws(()=>parseJson('['.repeat(33)+'0'+']'.repeat(33)),/nesting/);
  assert.deepEqual(parseJson('['.repeat(32)+'0'+']'.repeat(32)),JSON.parse('['.repeat(32)+'0'+']'.repeat(32)));
  for(const text of ['{"x":','[}','{"x",2}','"unterminated','{"x":"bad\\x"}']) assert.throws(()=>parseJson(text));
});
