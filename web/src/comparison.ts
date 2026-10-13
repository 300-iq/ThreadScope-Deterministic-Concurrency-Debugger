import type { Resource, Trace } from '../shared/trace.ts';
import { checkpoint } from './navigation.ts';

export function firstSchedulingDivergence(original: Trace, modified: Trace): number | null {
  const common=Math.min(original.schedule.length,modified.schedule.length);
  for(let i=0;i<common;i++) if(original.schedule[i]!==modified.schedule[i]) return i;
  return original.schedule.length===modified.schedule.length ? null : common;
}
// Unlike ordinary navigation, comparison never extends a shorter trace by
// repeating its terminal snapshot at scheduling points it did not reach.
export function comparisonCheckpoint(trace: Trace,index: number | 'final') {
  if(index==='final') return checkpoint(trace,trace.events.length);
  return Number.isInteger(index) && index>=0 && index<=trace.events.length ? checkpoint(trace,index) : null;
}
export function resourceDifferences(left?: Resource[],right?: Resource[]) {
  const names=[...new Set([...(left ?? []).map(r=>r.name),...(right ?? []).map(r=>r.name)])];
  return names.map(name=>{
    const original=left?.find(r=>r.name===name), modified=right?.find(r=>r.name===name);
    return {name,original,modified,changed:left===undefined || right===undefined ? null :
      original?.kind!==modified?.kind || original?.value!==modified?.value};
  });
}
export function recordedDifference(left: unknown,right: unknown): boolean | null {
  return left===undefined || right===undefined ? null : JSON.stringify(left)!==JSON.stringify(right);
}
export function threadChoices(trace: Trace,step: number,rejected: ReadonlySet<string> = new Set()) {
  const event=trace.events[step];
  return event?.threads.map(thread=>({id:thread.id,state:thread.state,
    reason:thread.id===event.thread ? 'original choice' : rejected.has(`${step}:${thread.id}`) ? 'rejected by engine' :
      !event.runnable.includes(thread.id) ? thread.state : null})) ?? [];
}
