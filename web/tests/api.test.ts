import test,{before,after} from 'node:test';
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { request as httpRequest } from 'node:http';
import { createApiServer } from '../server/app.ts';
import { EngineService, invokeEngine, parseSummary, validateExplore } from '../server/engine.ts';
import type { Exploration,ReplayResult } from '../shared/api.ts';
const server=createApiServer();let base='';
before(async()=>{server.listen(0,'127.0.0.1');await once(server,'listening');const address=server.address();assert.ok(address && typeof address !== 'string');base=`http://127.0.0.1:${address.port}`;});
after(async()=>{server.closeAllConnections();await new Promise<void>(resolve=>server.close(()=>resolve()));});
const options={scenario:'lost-update',maxSchedules:6,maxSteps:6,timeoutMs:1000};
async function post(path:string,body:unknown,headers:Record<string,string>={}) {return fetch(`${base}/api/${path}`,{method:'POST',headers:{'Content-Type':'application/json',...headers},body:JSON.stringify(body)});}
test('catalog comes from real engine help',async()=>{const response=await fetch(`${base}/api/scenarios`);assert.equal(response.status,200);const data=await response.json();assert.equal(data.scenarios.length,4);assert.ok(data.scenarios.find((s:any)=>s.name==='mutex-ordered'));});
test('explore actual lost-update runs and verify a failure with exit code 1',async()=>{
  const response=await post('explore',options);assert.equal(response.status,200);const data=await response.json() as Exploration;
  assert.equal(data.summary.exploredSchedules,6);assert.equal(data.summary.failures,4);assert.equal(data.summary.incomplete,false);assert.equal(data.traces.length,6);assert.equal(data.traces[0].outcome.status,'completed');
  const failed=data.traces.find(t=>t.outcome.status==='assertion_failed')!;assert.equal(failed.outcome.shared_state?.[0].value,1);
  const replay=await post('replay',{trace:failed});assert.equal(replay.status,200);const result=await replay.json() as ReplayResult;assert.equal(result.verified,true);assert.equal(result.exitCode,1);
  failed.events[0].observation.before=99;const mismatch=await(await post('replay',{trace:failed})).json() as ReplayResult;assert.equal(mismatch.verified,false);assert.equal(mismatch.status,'diverged');assert.equal(mismatch.exitCode,2);
});
test('corrected scenario returns actual passing timelines',async()=>{const data=await(await post('explore',{...options,scenario:'mutex-ordered',maxSchedules:2,maxSteps:8})).json() as Exploration;assert.equal(data.summary.failures,0);assert.equal(data.traces.length,2);assert.ok(data.traces.every(t=>t.outcome.status==='completed' && t.events.length===8));const replay=await(await post('replay',{trace:data.traces[0]})).json() as ReplayResult;assert.equal(replay.verified,true);});
test('bounded exploration is not reported as complete and step bound replays',async()=>{const data=await(await post('explore',{...options,maxSchedules:1,maxSteps:1})).json() as Exploration;assert.equal(data.exitCode,2);assert.equal(data.summary.incomplete,true);assert.equal(data.summary.stepBoundReached,true);assert.equal(data.summary.scheduleBoundReached,true);const replay=await(await post('replay',{trace:data.traces[0]})).json() as ReplayResult;assert.equal(replay.verified,true);assert.equal(replay.exitCode,2);});
test('reject arbitrary paths, commands, scenarios and invalid bounds before execution',async()=>{for(const bad of [{...options,executable:'/bin/sh'},{...options,command:'rm -rf /'},{...options,scenario:'lost-update; echo BAD'},{...options,maxSteps:257},{...options,maxSchedules:0},{...options,timeoutMs:'1000'},{...options,maxSchedules:10000}]) assert.equal((await post('explore',bad)).status,400);assert.equal((await post('replay',{path:'../../etc/passwd'})).status,400);});
test('reject foreign origin, host, non-JSON and unknown routes',async()=>{
  assert.equal((await post('explore',options,{Origin:'https://example.com'})).status,403);
  // Fetch may discard a manually supplied Host header. Use HTTP directly to
  // exercise the header the server actually receives, rather than a false test.
  const status=await new Promise<number|undefined>((resolve,reject)=>{const req=httpRequest(`${base}/api/scenarios`,{headers:{Host:'attacker.example'}},res=>{res.resume();resolve(res.statusCode);});req.on('error',reject);req.end();});
  assert.equal(status,403);
  assert.equal((await post('explore',options,{'Content-Type':'text/plain'})).status,415);
  assert.equal((await fetch(`${base}/api/unknown`)).status,404);
});
test('pre-cancelled engine request executes nothing',async()=>{const abort=new AbortController();abort.abort();await assert.rejects(invokeEngine(['--help'],abort.signal),/cancelled/);});
test('missing exploration summary fails closed',()=>{assert.throws(()=>parseSummary('some interrupted engine output'),/No success/);assert.throws(()=>validateExplore({...options,maxSteps:NaN}));});
test('one job at a time, and cancellation releases the job guard',async()=>{
  const engine=new EngineService();const first=engine.explore(options);
  await assert.rejects(engine.explore(options),/already in progress/);await first;
  const abort=new AbortController();abort.abort();await assert.rejects(engine.explore(options,abort.signal),/cancelled/);
  assert.equal((await engine.explore({...options,maxSchedules:1})).traces.length,1);
});

test('what-if runs failure to pass and pass to failure with verified real replays and immutable original',async()=>{
  const data=await(await post('explore',options)).json() as Exploration;
  const original=data.traces[1], saved=JSON.stringify(original);
  assert.deepEqual(original.schedule,[0,1,0,1,2,2]);
  const {firstSchedulingDivergence,resourceDifferences}=await import('../src/comparison.ts');
  const {parseTrace}=await import('../shared/trace.ts');
  for(let i=0;i<3;i++) {
    const response=await post('branch',{trace:original,step:1,thread:0});assert.equal(response.status,200);
    const passing=await response.json() as import('../shared/api.ts').BranchResult;
    assert.equal(passing.status,'executed');assert.equal(passing.originalVerified,true);assert.equal(passing.replay?.verified,true);
    assert.equal(passing.policy,'lowest_runnable_id');assert.deepEqual(passing.prefix,[0,0]);
    assert.deepEqual(passing.trace.schedule,[0,0,1,1,2,2]);assert.equal(passing.trace.outcome.status,'completed');assert.equal(passing.trace.outcome.shared_state?.[0].value,2);
    assert.deepEqual(parseTrace(passing.trace),passing.trace);assert.deepEqual(passing.trace.events[0],original.events[0]);
    assert.equal(firstSchedulingDivergence(original,passing.trace),1);
    assert.equal(resourceDifferences(original.outcome.shared_state,passing.trace.outcome.shared_state)[0].changed,true);
    const failing=await(await post('branch',{trace:passing.trace,step:1,thread:1})).json() as import('../shared/api.ts').BranchResult;
    assert.equal(failing.trace.outcome.status,'assertion_failed');assert.equal(failing.replay?.verified,true);assert.equal(failing.exitCode,1);
    assert.deepEqual(failing.trace,original);
  }
  assert.equal(JSON.stringify(original),saved);
});
test('engine rejects a nonrunnable prefix, and edited originals cannot produce a branch',async()=>{
  const data=await(await post('explore',options)).json() as Exploration;const trace=data.traces[1];
  const invalid=await(await post('branch',{trace,step:1,thread:2})).json() as import('../shared/api.ts').BranchResult;
  assert.equal(invalid.status,'invalid_prefix');assert.equal(invalid.trace.outcome.status,'replay_mismatch');assert.equal(invalid.trace.events.length,1);
  assert.match(invalid.trace.outcome.message,/T2 is not runnable at step 1; runnable=\[0,1\]/);assert.equal(invalid.replay,null);
  const changed=structuredClone(trace);changed.events[0].observation.before=99;
  const response=await post('branch',{trace:changed,step:1,thread:0});assert.equal(response.status,409);
  assert.match((await response.json()).error,/no branch was run/);
  changed.events[0].observation.before=trace.events[0].observation.before;changed.outcome.message='edited stop decision';
  assert.equal((await post('branch',{trace:changed,step:1,thread:0})).status,409);
  for(const extra of [{step:6,thread:0},{step:-1,thread:0},{step:1,thread:64},{step:1,thread:'0'},{step:1,thread:0,command:'sh'},{step:1,thread:0,prefix:[0]}])
    assert.equal((await post('branch',{trace,...extra})).status,400);
});
test('what-if preserves step bounds and turns a modeled deadlock into an actual passing run',async()=>{
  const bounded=await(await post('explore',{...options,maxSteps:2,maxSchedules:2})).json() as Exploration;
  const branch=await(await post('branch',{trace:bounded.traces[0],step:1,thread:1})).json() as import('../shared/api.ts').BranchResult;
  assert.equal(branch.trace.outcome.status,'step_limit');assert.equal(branch.trace.events.length,2);assert.equal(branch.replay?.verified,true);assert.equal(branch.exitCode,2);
  const deadlocks=await(await post('explore',{...options,scenario:'mutex-deadlock',maxSteps:8})).json() as Exploration;
  const original=deadlocks.traces.find(t=>t.outcome.status==='deadlock')!;
  const passing=await(await post('branch',{trace:original,step:1,thread:original.schedule[0]})).json() as import('../shared/api.ts').BranchResult;
  assert.equal(passing.trace.outcome.status,'completed');assert.equal(passing.replay?.verified,true);assert.equal(passing.trace.events.length,8);
  assert.equal(original.outcome.wait_for?.length,2);assert.deepEqual(passing.trace.outcome.wait_for,[]);
  assert.ok(passing.trace.outcome.shared_state?.every(r=>r.kind==='mutex' && r.value===null));
});

test('Patch & Verify executes both actual counter implementations with identical settings and fresh state',async()=>{
  const {implementationStats,parsePatchVerification}=await import('../shared/patch.ts');
  const input={scenario:'counter',maxSchedules:6,maxSteps:8,timeoutMs:1000};const saved=JSON.stringify(input);
  let previous: import('../shared/api.ts').PatchVerification | undefined;
  for(let repetition=0;repetition<3;repetition++) {
    const response=await post('patch-verify',input);assert.equal(response.status,200);
    const result=parsePatchVerification(await response.json());
    assert.deepEqual(result.settings,{maxSchedules:6,maxSteps:8,timeoutMs:1000});
    assert.equal(result.broken.scenario,'lost-update');assert.equal(result.corrected.scenario,'lost-update-rmw');
    assert.deepEqual(implementationStats(result.broken),{explored:6,assertions:4,deadlocks:0,completed:2,interrupted:0});
    assert.deepEqual(implementationStats(result.corrected),{explored:2,assertions:0,deadlocks:0,completed:2,interrupted:0});
    for(const kind of ['broken','corrected'] as const) {
      const implementation=result[kind];
      assert.equal(implementation.options.maxSchedules,input.maxSchedules);assert.equal(implementation.options.maxSteps,input.maxSteps);assert.equal(implementation.options.timeoutMs,input.timeoutMs);
      for(const trace of implementation.traces) assert.equal(trace.events[0].shared_state_before?.[0].value,0);
      if(previous) assert.deepEqual(implementation.traces,previous[kind].traces);
    }
    assert.ok(result.corrected.traces.every(t=>t.outcome.shared_state?.[0].value===2));
    previous=result;
  }
  assert.equal(JSON.stringify(input),saved);
  const failed=previous!.broken.traces.find(t=>t.outcome.status==='assertion_failed')!;
  assert.equal((await(await post('replay',{trace:failed})).json() as ReplayResult).verified,true);
  const incompatible=structuredClone(failed);incompatible.program='lost-update-rmw/v1';
  const rejected=await(await post('replay',{trace:incompatible})).json() as ReplayResult;
  assert.equal(rejected.verified,false);assert.equal(rejected.status,'diverged');assert.match(rejected.output,/operation/);
});
test('Patch & Verify reports actual lock deadlocks separately and replays the broken trace only in its own implementation',async()=>{
  const {implementationStats,parsePatchVerification}=await import('../shared/patch.ts');
  const result=parsePatchVerification(await(await post('patch-verify',{scenario:'lock-order',maxSchedules:6,maxSteps:8,timeoutMs:1000})).json());
  assert.equal(implementationStats(result.broken).deadlocks,2);assert.equal(implementationStats(result.broken).assertions,0);
  assert.equal(implementationStats(result.corrected).deadlocks,0);assert.equal(result.corrected.summary.incomplete,false);
  const failed=result.broken.traces.find(t=>t.outcome.status==='deadlock')!;
  const verified=await(await post('replay',{trace:failed})).json() as ReplayResult;
  assert.equal(verified.verified,true);assert.equal(verified.exitCode,1);
  assert.equal((await post('replay',{trace:failed,scenario:'mutex-ordered'})).status,400);
  const incompatible=structuredClone(failed);incompatible.program=result.corrected.traces[0].program;
  const rejected=await(await post('replay',{trace:incompatible})).json() as ReplayResult;
  assert.equal(rejected.verified,false);assert.equal(rejected.status,'diverged');assert.match(rejected.output,/replay mismatch:/);
  assert.equal(failed.program,'mutex-deadlock/v1');
  const ownReplay=await(await post('replay',{trace:result.corrected.traces[0]})).json() as ReplayResult;
  assert.equal(ownReplay.verified,true);
});
test('Patch & Verify handles low schedule and step bounds without declaring programs correct',async()=>{
  const {parsePatchVerification}=await import('../shared/patch.ts');
  for(const limits of [{maxSchedules:1,maxSteps:8},{maxSchedules:1,maxSteps:1}]) {
    const result=parsePatchVerification(await(await post('patch-verify',{scenario:'counter',timeoutMs:1000,...limits})).json());
    for(const run of [result.broken,result.corrected]) {
      assert.equal(run.traces.length,1);assert.equal(run.summary.incomplete,true);assert.equal(run.exitCode,2);assert.equal(run.summary.failures,0);
      if(limits.maxSteps===1) assert.equal(run.traces[0].outcome.status,'step_limit');
      else {assert.equal(run.traces[0].outcome.status,'completed');assert.equal(run.summary.scheduleBoundReached,true);}
    }
  }
});
test('Patch & Verify refuses arbitrary implementations and holds one job guard across the paired searches',async()=>{
  const input={scenario:'counter',maxSchedules:6,maxSteps:8,timeoutMs:1000};
  for(const value of [{...input,scenario:'lost-update'},{...input,scenario:'counter; echo BAD'},{...input,broken:'/bin/sh'},{...input,corrected:'mutex-ordered'},{...input,command:'sh'},{...input,maxSchedules:0},{...input,maxSteps:257},{...input,timeoutMs:'1000'}])
    assert.equal((await post('patch-verify',value)).status,400);
  const engine=new EngineService();const running=engine.patchVerify(input);
  await assert.rejects(engine.explore(options),/already in progress/);
  await assert.rejects(engine.patchVerify(input),/already in progress/);await running;
  const controller=new AbortController();controller.abort();await assert.rejects(engine.patchVerify(input,controller.signal),/cancelled/);
  assert.equal((await engine.patchVerify(input)).corrected.traces.length,2);
});
test('API rejects contradictory traces and duplicate JSON options before execution',async()=>{
  const result=await(await post('explore',options)).json() as Exploration;
  const failed=result.traces.find(t=>t.outcome.status==='assertion_failed')!;failed.outcome.status='completed';
  assert.equal((await post('replay',{trace:failed})).status,400);
  const response=await fetch(`${base}/api/explore`,{method:'POST',headers:{'Content-Type':'application/json'},body:'{"scenario":"lost-update","maxSchedules":1,"maxSchedules":6,"maxSteps":8,"timeoutMs":1000}'});
  assert.equal(response.status,400);assert.match((await response.json()).error,/Duplicate/);
});
