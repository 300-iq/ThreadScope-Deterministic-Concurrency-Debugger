import test from 'node:test';
import assert from 'node:assert/strict';
import { api } from '../src/api.ts';
test('frontend reports malformed/non-JSON backend replies explicitly',async context=>{
  context.mock.method(globalThis,'fetch',async()=>new Response('<html>proxy unavailable</html>',{status:502}));
  await assert.rejects(api('explore',{}),/invalid JSON \(HTTP 502\)/);
});
test('frontend keeps the actual API error rather than accepting a success result',async context=>{
  context.mock.method(globalThis,'fetch',async()=>new Response('{"error":"Trace diverged"}',{status:409}));
  await assert.rejects(api('replay',{}),/Trace diverged/);
});
test('frontend rejects ambiguous JSON replies',async context=>{
  context.mock.method(globalThis,'fetch',async()=>new Response('{"verified":false,"verified":true}'));
  await assert.rejects(api('replay',{}),/invalid JSON/);
});
