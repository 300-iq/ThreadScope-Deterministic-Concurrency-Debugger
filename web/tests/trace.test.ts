import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { parseTrace } from '../shared/trace.ts';
const fixture=(name='lost-update')=>JSON.parse(readFileSync(new URL(`./fixtures/${name}.json`,import.meta.url),'utf8'));
for(const name of ['lost-update','corrected-counter','deadlock','ordered-locks']) test(`parse actual C++ fixture: ${name}`,()=>{
  const raw=fixture(name);const trace=parseTrace(JSON.stringify(raw));assert.deepEqual(trace,raw);
});
test('legacy v1 snapshots, dependencies and categories are not fabricated',()=>{
  const raw=fixture();delete raw.outcome.shared_state;delete raw.outcome.wait_for;
  for(const e of raw.events){delete e.shared_state_before;delete e.shared_state_after;delete e.wait_for;delete e.type;}
  const trace=parseTrace(raw);assert.equal(trace.events[0].type,'shared_state');assert.equal(trace.events[0].wait_for,undefined);assert.equal(trace.outcome.shared_state,undefined);
});
for(const [name,change] of [
  ['unsupported schema',(t:any)=>{t.schema_version=2;}],
  ['mismatched schedule',(t:any)=>{t.schedule[0]=1;}],
  ['noncontiguous step',(t:any)=>{t.events[0].step=1;}],
  ['invalid runnable set',(t:any)=>{t.events[0].runnable=[0];}],
  ['running scheduling point',(t:any)=>{t.events[0].threads[0].state='running';}],
  ['changed resource layout',(t:any)=>{t.outcome.shared_state[0].name='another';}],
  ['partial snapshots',(t:any)=>{delete t.events[0].shared_state_after;}],
  ['partial graph',(t:any)=>{delete t.events[0].wait_for;}],
  ['wrong event category',(t:any)=>{t.events[0].type='lock_release';}],
  ['integer overflow',(t:any)=>{t.events[0].observation.before=2147483648;}],
  ['numeric assertion result',(t:any)=>{t.events[0].observation.assertion_passed=0;}],
  ['unknown outcome',(t:any)=>{t.outcome.status='success';}],
] as const) test(`reject ${name}`,()=>{const t=fixture();change(t);assert.throws(()=>parseTrace(t));});
test('reject graph inconsistent with recorded ownership',()=>{const t=fixture('deadlock');t.outcome.wait_for[0].waits_for=0;assert.throws(()=>parseTrace(t),/ownership/);});
test('reject malformed JSON and oversized string',()=>{assert.throws(()=>parseTrace('{'));assert.throws(()=>parseTrace(' '.repeat(8*1024*1024+1)),/8 MiB/);});
test('enforce documented UI event limit',()=>{const t=fixture();t.events=Array(1001).fill(t.events[0]);t.schedule=Array(1001).fill(0);t.limits.max_steps=2000;assert.throws(()=>parseTrace(t),/1000 events/);});
test('accept actual C++ UTF-8 resource ordering, which differs from UTF-16 ordering',()=>{
  const raw=fixture('unicode-resources');const trace=parseTrace(raw);
  assert.deepEqual(trace.outcome.shared_state?.map(r=>r.name),['\uE000','\u{10000}']);
});
for(const [name,change] of [
  ['completed with failed assertion',(t:any)=>{t.outcome.status='completed';}],
  ['deadlock with only terminated workers',(t:any)=>{t.outcome.status='deadlock';}],
  ['assertion outcome without failed event',(t:any)=>{t.events.at(-1).observation.assertion_passed=true;}],
  ['completed with running worker',(t:any)=>{t.outcome.status='completed';t.events.at(-1).observation.assertion_passed=true;t.outcome.threads[0].state='running';}],
  ['step bound before configured limit',(t:any)=>{t.outcome.status='step_limit';}],
] as const) test(`reject contradictory outcome: ${name}`,()=>{const raw=fixture();change(raw);assert.throws(()=>parseTrace(raw));});
test('reject assertion result inconsistent with observed/expected values',()=>{const raw=fixture();raw.events.at(-1).observation.expected=1;assert.throws(()=>parseTrace(raw),/assertion result/);});
