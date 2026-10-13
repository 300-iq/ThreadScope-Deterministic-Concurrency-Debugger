import type { Trace } from '../shared/trace.ts';
export type Navigation = 'start' | 'back' | 'forward' | 'failure' | 'end';
const clamp = (trace: Trace, index: number) => Number.isFinite(index) ? Math.min(trace.events.length,Math.max(0,Math.trunc(index))) : 0;
export function failureIndex(trace: Trace): number | null {
  const assertion=trace.events.findIndex(event=>event.observation.assertion_passed === false);
  if(assertion >= 0) return assertion;
  return trace.outcome.status === 'deadlock' ? trace.events.length : null;
}
export function navigate(trace: Trace, index: number, action: Navigation): number {
  const current=clamp(trace,index);
  if(action === 'start') return 0;
  if(action === 'end') return trace.events.length;
  if(action === 'failure') return failureIndex(trace) ?? current;
  return Math.min(trace.events.length,Math.max(0,current+(action === 'forward' ? 1 : -1)));
}
export function checkpoint(trace: Trace, index: number) {
  const cursor=clamp(trace,index);
  const event=trace.events[cursor];
  return event ? { cursor,event,threads:event.threads,shared:event.shared_state_before,waits:event.wait_for,terminal:false }
    : { cursor,event:undefined,threads:trace.outcome.threads,shared:trace.outcome.shared_state,waits:trace.outcome.wait_for,terminal:true };
}
