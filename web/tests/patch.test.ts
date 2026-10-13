import test from 'node:test';
import assert from 'node:assert/strict';
import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { EngineService } from '../server/engine.ts';
import { demoSettings, implementationStats, parsePatchVerification } from '../shared/patch.ts';
import PatchVerify from '../src/PatchVerify.tsx';

test('comparison parser rejects mixed settings, program identities and fabricated counts',async()=>{
  const result=await new EngineService().patchVerify({scenario:'counter',...demoSettings});
  assert.deepEqual(parsePatchVerification(result),result);
  for(const mutate of [
    (r:typeof result)=>{r.corrected.options.maxSteps=7;},
    (r:typeof result)=>{r.corrected.options.maxSchedules=1;},
    (r:typeof result)=>{r.corrected.options.timeoutMs=999;},
    (r:typeof result)=>{r.corrected.traces[0].program='lost-update/v1';},
    (r:typeof result)=>{r.broken.summary.failures=0;},
    (r:typeof result)=>{r.corrected.traces=[];},
    (r:typeof result)=>{r.broken.exitCode=0;},
    (r:typeof result)=>{r.corrected.summary.exploredSchedules=6;},
    (r:typeof result)=>{r.broken.traces[0].limits.max_steps=7;},
  ]) {const changed=structuredClone(result);mutate(changed);assert.throws(()=>parsePatchVerification(changed));}
});
test('statistics distinguish assertion failures, deadlocks and bounded outcomes from actual runs',async()=>{
  const engine=new EngineService();
  const locks=await engine.patchVerify({scenario:'lock-order',...demoSettings});
  assert.deepEqual(implementationStats(locks.broken),{explored:6,assertions:0,deadlocks:2,completed:4,interrupted:0});
  assert.deepEqual(implementationStats(locks.corrected),{explored:2,assertions:0,deadlocks:0,completed:2,interrupted:0});
  const bounded=await engine.patchVerify({scenario:'counter',...demoSettings,maxSteps:1,maxSchedules:1});
  assert.deepEqual(implementationStats(bounded.corrected),{explored:1,assertions:0,deadlocks:0,completed:0,interrupted:1});
});
test('initial UI prominently states limits and shows no fabricated exploration statistics',()=>{
  const unavailable=async()=>{throw new Error('No execution requested during rendering.');};
  const html=renderToStaticMarkup(createElement(PatchVerify,{busy:false,connected:true,compare:unavailable,replay:unavailable,inspect:()=>{}}));
  assert.match(html,/No observed failure in a bounded search does not prove general correctness/);
  assert.match(html,/Replay stays with the implementation that recorded the trace/);
  assert.match(html,/Select broken implementation/);assert.match(html,/Select corrected implementation/);
  assert.match(html,/load\/store|read-modify-write/);assert.match(html,/fetch_add/);
  assert.equal((html.match(/Awaiting actual C\+\+ exploration/g) ?? []).length,2);
  assert.equal((html.match(/>—<\/b>/g) ?? []).length,6);
  assert.doesNotMatch(html,/Replay verified|No observed failures · modeled tree exhausted/);
});
