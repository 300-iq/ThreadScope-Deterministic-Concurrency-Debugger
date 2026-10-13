import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { parseTrace } from '../shared/trace.ts';
import { comparisonCheckpoint, firstSchedulingDivergence, recordedDifference, resourceDifferences, threadChoices } from '../src/comparison.ts';
const trace=parseTrace(readFileSync(new URL('./fixtures/lost-update.json',import.meta.url),'utf8'));
test('choices expose only recorded runnable alternatives and remember engine rejections',()=>{
  const choices=threadChoices(trace,1);assert.equal(choices.find(t=>t.id===0)?.reason,null);
  assert.equal(choices.find(t=>t.id===1)?.reason,'original choice');assert.equal(choices.find(t=>t.id===2)?.reason,'blocked');
  assert.equal(threadChoices(trace,1,new Set(['1:0'])).find(t=>t.id===0)?.reason,'rejected by engine');
  assert.equal(threadChoices(trace,0,new Set(['1:0'])).find(t=>t.id===1)?.reason,null);
  assert.deepEqual(threadChoices(trace,trace.events.length),[]);
});
test('divergence includes changed choice, different endpoint and identical schedules',()=>{
  assert.equal(firstSchedulingDivergence(trace,trace),null);
  const modified=structuredClone(trace);modified.schedule[1]=0;
  assert.equal(firstSchedulingDivergence(trace,modified),1);
  modified.schedule=trace.schedule.slice(0,2);assert.equal(firstSchedulingDivergence(trace,modified),2);
});
test('shorter execution has no invented checkpoints and final comparison uses each own end',()=>{
  const deadlock=parseTrace(readFileSync(new URL('./fixtures/deadlock.json',import.meta.url),'utf8'));
  assert.equal(comparisonCheckpoint(deadlock,2)?.terminal,true);
  assert.equal(comparisonCheckpoint(deadlock,3),null);
  assert.equal(comparisonCheckpoint(deadlock,-1),null);
  assert.equal(comparisonCheckpoint(deadlock,NaN),null);
  assert.equal(comparisonCheckpoint(deadlock,'final')?.waits?.length,2);
  assert.equal(comparisonCheckpoint(trace,'final')?.shared?.[0].value,1);
});
test('resource comparison highlights values, owners and absent resources without inventing missing legacy data',()=>{
  const a=[{name:'counter',kind:'atomic_int' as const,value:1},{name:'lock',kind:'mutex' as const,value:0}];
  const b=[{name:'counter',kind:'atomic_int' as const,value:2},{name:'lock',kind:'mutex' as const,value:null}];
  assert.ok(resourceDifferences(a,b).every(r=>r.changed===true));
  assert.ok(resourceDifferences(a,a).every(r=>r.changed===false));
  assert.equal(resourceDifferences(a,[])[0].changed,true);
  assert.equal(resourceDifferences(a,undefined)[0].changed,null);
});
test('thread and wait comparisons distinguish absent metadata from empty recorded state',()=>{
  assert.equal(recordedDifference([],[]),false);assert.equal(recordedDifference([],undefined),null);
  assert.equal(recordedDifference([{thread:0,waits_for:1,resource:'first',kind:'mutex'}],[]),true);
});
