import { parseJson } from './json.ts';
export const operations = ['load','store','fetch_add','assert_equal','yield','lock','unlock','join'] as const;
export type Operation = typeof operations[number];
export type ThreadState = 'running' | 'runnable' | 'blocked' | 'terminated';
export type Status = 'completed' | 'assertion_failed' | 'deadlock' | 'step_limit' | 'timeout' | 'replay_mismatch' | 'worker_error';
export type EventType = 'shared_state' | 'lock_acquire' | 'lock_release' | 'assertion' | 'join' | 'yield';
export interface Thread { id: number; state: ThreadState }
export interface Resource { name: string; kind: 'atomic_int' | 'mutex'; value: number | null }
export interface Dependency { thread: number; waits_for: number; kind: 'mutex' | 'join'; resource: string }
export interface TraceEvent {
  step: number; thread: number; runnable: number[]; threads: Thread[];
  operation: Operation; type: EventType; resource: string; label: string;
  observation: { before: number | null; after: number | null; expected: number | null; assertion_passed: boolean | null; error: string | null };
  shared_state_before?: Resource[]; shared_state_after?: Resource[]; wait_for?: Dependency[];
}
export interface Trace {
  schema_version: 1; program: string; limits: { max_steps: number; timeout_ms: number };
  schedule: number[]; events: TraceEvent[];
  outcome: { status: Status; message: string; threads: Thread[]; shared_state?: Resource[]; wait_for?: Dependency[] };
}
export const eventType = (op: Operation): EventType => ({ load:'shared_state', store:'shared_state', fetch_add:'shared_state', assert_equal:'assertion', lock:'lock_acquire', unlock:'lock_release', join:'join', yield:'yield' })[op] as EventType;
const fail = (path: string, expected: string): never => { throw new Error(`${path}: expected ${expected}`); };
export function record(v: unknown, path: string): Record<string, unknown> {
  if (!v || typeof v !== 'object' || Array.isArray(v)) return fail(path, 'an object');
  return v as Record<string, unknown>;
}
const array = (v: unknown, path: string): unknown[] => Array.isArray(v) ? v : fail(path, 'an array');
const text = (v: unknown, path: string): string => typeof v === 'string' ? v : fail(path, 'a string');
export const integer = (v: unknown, path: string, min: number, max: number): number => typeof v === 'number' && Number.isSafeInteger(v) && v >= min && v <= max ? v : fail(path, `an integer in ${min}..${max}`);
const nullableInt = (v: unknown, path: string) => v === null ? null : integer(v, path, -2147483648, 2147483647);
const utf8 = new TextEncoder();
function compareResourceNames(left: string, right: string): number {
  // Match the engine's UTF-8 std::string byte order, not UTF-16 code units.
  const a=utf8.encode(left), b=utf8.encode(right);
  for(let i=0;i<Math.min(a.length,b.length);i++) if(a[i] !== b[i]) return a[i]-b[i];
  return a.length-b.length;
}
const states = (v: unknown, path: string): Thread[] => {
  const list = array(v,path); if (list.length > 64) fail(path,'at most 64 threads');
  return list.map((v,i) => { const item=record(v,path); const id=integer(item.id,path,0,63);
    if (id !== i || !['running','runnable','blocked','terminated'].includes(String(item.state))) fail(path,'ordered contiguous thread IDs and valid states');
    return { id, state:item.state as ThreadState }; });
};
const resources = (v: unknown, path: string, count: number): Resource[] => {
  let previous='';
  return array(v,path).map(v => { const item=record(v,path); const name=text(item.name,path); const kind=item.kind;
    if (!name || compareResourceNames(name,previous) <= 0 || (kind !== 'atomic_int' && kind !== 'mutex')) fail(path,'sorted unique resource names and valid kinds');
    previous=name; const value=nullableInt(item.value,path);
    if ((kind === 'atomic_int' && value === null) || (kind === 'mutex' && value !== null && (value < 0 || value >= count))) fail(path,'a value or registered mutex owner');
    return { name, kind:kind as Resource['kind'], value }; });
};
const graph = (v: unknown, path: string, threads: Thread[], shared?: Resource[]): Dependency[] => {
  const list=array(v,path); if(list.length>4096) fail(path,'at most 4096 dependencies');
  let previous: Dependency | undefined;
  return list.map(v => { const item=record(v,path); const thread=integer(item.thread,path,0,threads.length-1); const target=integer(item.waits_for,path,0,threads.length-1);
    const kind=item.kind; const resource=text(item.resource,path);
    if (threads[thread].state !== 'blocked' || (kind !== 'mutex' && kind !== 'join')) fail(path,'a blocked source and valid dependency kind');
    if(kind === 'join' && (resource !== '' || thread === target || threads[target].state === 'terminated')) fail(path,'an unfinished, different join target');
    const owner=shared?.find(s => s.name === resource);
    if(kind === 'mutex' && owner && (owner.kind !== 'mutex' || owner.value !== target)) fail(path,'consistent mutex ownership');
    if(previous && (thread < previous.thread || (thread === previous.thread && (target <= previous.waits_for || kind === 'mutex' || previous.kind === 'mutex')))) fail(path,'sorted unique dependencies');
    const edge={ thread, waits_for:target, kind:kind as Dependency['kind'], resource }; previous=edge; return edge; });
};
export function parseTrace(input: unknown): Trace {
  if(typeof input === 'string') { if(new TextEncoder().encode(input).length > 8*1024*1024) fail('trace','at most 8 MiB'); input=parseJson(input); }
  const root=record(input,'trace'); if(root.schema_version !== 1) fail('schema_version','1');
  const program=text(root.program,'program'); if(!program) fail('program','a nonempty identifier');
  const limits=record(root.limits,'limits'); const max_steps=integer(limits.max_steps,'max_steps',1,1_000_000); const timeout_ms=integer(limits.timeout_ms,'timeout_ms',1,60_000);
  const end=record(root.outcome,'outcome'); const endThreads=states(end.threads,'outcome.threads');
  if(!['completed','assertion_failed','deadlock','step_limit','timeout','replay_mismatch','worker_error'].includes(String(end.status))) fail('outcome.status','an engine status');
  const endShared=end.shared_state === undefined ? undefined : resources(end.shared_state,'outcome.shared_state',endThreads.length);
  const endGraph=end.wait_for === undefined ? undefined : graph(end.wait_for,'outcome.wait_for',endThreads,endShared);
  const schedule=array(root.schedule,'schedule').map(v=>integer(v,'schedule',0,63)); const rawEvents=array(root.events,'events');
  if(rawEvents.length !== schedule.length || rawEvents.length > max_steps || rawEvents.length > 1000) fail('events','matching schedule length, within step limit and UI limit of 1000 events');
  const events=rawEvents.map((v,i): TraceEvent => {
    const event=record(v,`events[${i}]`); const step=integer(event.step,'step',0,max_steps); const thread=integer(event.thread,'thread',0,endThreads.length-1);
    const threads=states(event.threads,'event.threads');
    if(step !== i || schedule[i] !== thread || threads.length !== endThreads.length || threads.some(t=>t.state === 'running')) fail('event','a quiescent, consistent decision');
    const runnable=array(event.runnable,'runnable').map(v=>integer(v,'runnable',0,63));
    if(JSON.stringify(runnable) !== JSON.stringify(threads.filter(t=>t.state === 'runnable').map(t=>t.id)) || !runnable.includes(thread)) fail('runnable','the ordered enabled set containing the selected thread');
    const operation=text(event.operation,'operation') as Operation; if(!operations.includes(operation)) fail('operation','an instrumented operation');
    const type=eventType(operation); if(event.type !== undefined && event.type !== type) fail('type','the category matching operation');
    if((event.shared_state_before !== undefined) !== (endShared !== undefined) || (event.shared_state_after !== undefined) !== (endShared !== undefined)) fail('shared state','snapshots on every event and outcome, or none');
    const before=endShared === undefined ? undefined : resources(event.shared_state_before,'shared_state_before',threads.length);
    const after=endShared === undefined ? undefined : resources(event.shared_state_after,'shared_state_after',threads.length);
    const layout=(s: Resource[])=>JSON.stringify(s.map(({name,kind})=>({name,kind})));
    if(endShared && (layout(before!) !== layout(endShared) || layout(after!) !== layout(endShared))) fail('shared state','the same resource layout throughout');
    if((event.wait_for !== undefined) !== (endGraph !== undefined)) fail('wait_for','graphs on every event and outcome, or none');
    const waits=endGraph === undefined ? undefined : graph(event.wait_for,'wait_for',threads,before);
    const obs=record(event.observation,'observation');
    if(obs.assertion_passed !== null && typeof obs.assertion_passed !== 'boolean') fail('assertion_passed','boolean or null');
    if(obs.error !== null && typeof obs.error !== 'string') fail('error','string or null');
    if(operation==='assert_equal' && (obs.before===null || obs.before!==obs.after || obs.expected===null || typeof obs.assertion_passed!=='boolean' || obs.assertion_passed!==(obs.before===obs.expected) || obs.error!==null)) fail('observation','an assertion result consistent with its recorded values');
    if(operation!=='assert_equal' && (obs.assertion_passed!==null || obs.expected!==null)) fail('observation','assertion fields only on assertion operations');
    return { step,thread,runnable,threads,operation,type,resource:text(event.resource,'resource'),label:text(event.label,'label'),
      observation:{ before:nullableInt(obs.before,'before'),after:nullableInt(obs.after,'after'),expected:nullableInt(obs.expected,'expected'),assertion_passed:obs.assertion_passed as boolean|null,error:obs.error as string|null },
      shared_state_before:before,shared_state_after:after,wait_for:waits };
  });
  const failed=events.filter(e=>e.observation.assertion_passed===false), last=events.at(-1);
  const stable=['completed','assertion_failed','deadlock','step_limit'].includes(String(end.status));
  if(stable && (endThreads.some(t=>t.state==='running') || events.some(e=>e.observation.error!==null))) fail('outcome','a stable stop without running workers or operation errors');
  if(stable && end.status!=='assertion_failed' && failed.length) fail('outcome','a status consistent with the failed assertion');
  if(end.status==='completed' && endThreads.some(t=>t.state!=='terminated')) fail('outcome','all workers terminated for completion');
  if(end.status==='assertion_failed' && (failed.length!==1 || last?.operation!=='assert_equal' || last.observation.assertion_passed!==false || last.observation.before===null || last.observation.expected===null)) fail('outcome','a final failed assertion event');
  if(end.status==='deadlock' && (!endThreads.some(t=>t.state==='blocked') || endThreads.some(t=>t.state!=='blocked' && t.state!=='terminated'))) fail('outcome','unfinished blocked workers with no runnable work');
  if(end.status==='step_limit' && (events.length!==max_steps || !endThreads.some(t=>t.state==='runnable'))) fail('outcome','the configured step count and runnable unfinished work');
  return { schema_version:1,program,limits:{max_steps,timeout_ms},schedule,events,outcome:{status:end.status as Status,message:text(end.message,'message'),threads:endThreads,shared_state:endShared,wait_for:endGraph} };
}
export const isFailure = (status: Status) => status === 'assertion_failed' || status === 'deadlock';
export const canReplay = (trace: Trace) => !['timeout','worker_error','replay_mismatch'].includes(trace.outcome.status);
