import type { Exploration, PatchScenario, PatchVerification } from './api.ts';
import { integer, isFailure, parseTrace, record } from './trace.ts';

// Scenario pairs map to the compiled C++ registry.
export const patchScenarios = [
  {
    id: 'counter' as const, name: 'Lost-update counter',
    broken: 'lost-update', corrected: 'lost-update-rmw',
    brokenCode: 'const int local = thread.load(counter);\nthread.store(counter, local + 1);',
    correctedCode: 'thread.fetch_add(counter, 1);',
    difference: 'Each split load/store is atomic, but the pair is not one read-modify-write. Both workers can read 0 and write 1. fetch_add performs each increment as one controlled atomic operation.',
    replayDifference: 'The corrected worker has one fetch_add event instead of two load/store events. Event counts, operations, observations and runnable sets can change.',
  },
  {
    id: 'lock-order' as const, name: 'Lock-order deadlock',
    broken: 'mutex-deadlock', corrected: 'mutex-ordered',
    brokenCode: '// T0\nthread.lock(first); thread.lock(second);\n// T1\nthread.lock(second); thread.lock(first);',
    correctedCode: '// Both T0 and T1\nthread.lock(first); thread.lock(second);',
    difference: 'Opposite acquisition orders can leave each worker holding the lock the other needs. Consistent ordering makes the second worker wait on first before it owns second. Both versions release their locks in reverse acquisition order.',
    replayDifference: 'Changing acquisition order changes the pending resource and when each thread is runnable. A choice that was enabled in the broken program can be blocked in the corrected program.',
  },
] as const;
export function patchScenario(id: unknown) {
  const pair=patchScenarios.find(pair=>pair.id===id);
  if(!pair) throw new Error('Choose a supported comparison: counter or lock-order.');
  return pair;
}
export const demoSettings = {maxSchedules:6,maxSteps:8,timeoutMs:1000} as const;
export function implementationStats(exploration: Exploration) {
  return {
    explored:exploration.traces.length,
    assertions:exploration.traces.filter(t=>t.outcome.status==='assertion_failed').length,
    deadlocks:exploration.traces.filter(t=>t.outcome.status==='deadlock').length,
    completed:exploration.traces.filter(t=>t.outcome.status==='completed').length,
    interrupted:exploration.traces.filter(t=>!['completed','assertion_failed','deadlock'].includes(t.outcome.status)).length,
  };
}
// Fail closed if an API response mixes settings or program identities. Counts
// displayed by the UI are always derived from the actual parsed run traces.
export function parsePatchVerification(input: unknown): PatchVerification {
  const body=record(input,'comparison'), pair=patchScenario(body.scenario);
  const limits=record(body.settings,'settings');
  const settings={maxSchedules:integer(limits.maxSchedules,'maxSchedules',1,100),maxSteps:integer(limits.maxSteps,'maxSteps',1,256),timeoutMs:integer(limits.timeoutMs,'timeoutMs',100,5000)};
  function implementation(input: unknown,scenario: string): Exploration {
    const value=record(input,scenario), options=record(value.options,'options'), summary=record(value.summary,'summary');
    if(value.scenario!==scenario || options.scenario!==scenario || options.maxSchedules!==settings.maxSchedules || options.maxSteps!==settings.maxSteps || options.timeoutMs!==settings.timeoutMs)
      throw new Error('Comparison implementations must use identical recorded exploration settings.');
    if(!Array.isArray(value.traces) || !value.traces.length || value.traces.length>settings.maxSchedules)
      throw new Error('Comparison requires the actual traces from each bounded search.');
    const traces=value.traces.map(parseTrace);
    if(traces.some(t=>t.program!==`${scenario}/v1` || t.limits.max_steps!==settings.maxSteps || t.limits.timeout_ms!==settings.timeoutMs))
      throw new Error('Comparison trace program or limits do not match its implementation.');
    for(const key of ['frontierExhausted','incomplete','scheduleBoundReached','stepBoundReached'])
      if(typeof summary[key]!=='boolean') throw new Error('Comparison is missing its bounded-search status.');
    const failures=traces.filter(t=>isFailure(t.outcome.status)).length;
    const incomplete=!summary.frontierExhausted || traces.some(t=>!['completed','assertion_failed','deadlock'].includes(t.outcome.status));
    if(summary.exploredSchedules!==traces.length || summary.failures!==failures || summary.incomplete!==incomplete ||
      summary.stepBoundReached!==traces.some(t=>t.outcome.status==='step_limit') || value.exitCode!==(incomplete ? 2 : failures ? 1 : 0) || typeof value.output!=='string')
      throw new Error('Comparison counts or exit status disagree with the recorded traces.');
    return {...value, traces} as unknown as Exploration;
  }
  return {scenario:pair.id as PatchScenario,settings,broken:implementation(body.broken,pair.broken),corrected:implementation(body.corrected,pair.corrected)};
}
