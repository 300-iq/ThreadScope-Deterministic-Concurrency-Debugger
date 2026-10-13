import { useEffect, useState, type CSSProperties } from 'react';
import { canReplay, type Resource, type Trace } from '../shared/trace.ts';
import type { BranchResult } from '../shared/api.ts';
import { comparisonCheckpoint, firstSchedulingDivergence, recordedDifference, resourceDifferences, threadChoices } from './comparison.ts';

const colors=['#65d6c1','#a498fa','#e8b978','#6eb9e9','#ec96b3'];
const value=(resource: Resource | undefined,known: boolean)=> !known ? 'not recorded' : !resource ? 'absent' :
  resource.kind==='mutex' ? resource.value===null ? 'unowned' : `T${resource.value}` : String(resource.value);
interface Props {
  trace: Trace; cursor: number; busy: boolean; connected: boolean;
  run: (step: number,thread: number)=>Promise<BranchResult>;
  onVerified: (verified: boolean)=>void;
  download: (trace: Trace)=>void;
}
export default function WhatIf({trace,cursor,busy,connected,run,onVerified,download}: Props) {
  const [choice,setChoice]=useState<number|null>(null),[branch,setBranch]=useState<BranchResult|null>(null);
  const [rejected,setRejected]=useState<Set<string>>(new Set()),[error,setError]=useState('');
  const [point,setPoint]=useState<number|'final'>('final');
  useEffect(()=>{setBranch(null);setRejected(new Set());setError('');setChoice(null);setPoint('final');},[trace]);
  useEffect(()=>{setChoice(null);setError('');},[cursor]);
  const event=trace.events[cursor], choices=threadChoices(trace,cursor,rejected);
  const available=choices.filter(t=>t.reason===null);
  const divergence=branch ? firstSchedulingDivergence(trace,branch.trace) : null;
  async function execute() {
    if(choice===null || !event) return;
    setError('');
    try {
      const result=await run(cursor,choice);setBranch(result);onVerified(result.originalVerified);
      if(result.status==='invalid_prefix') {
        setRejected(old=>new Set([...old,`${result.step}:${result.thread}`]));setChoice(null);
        setError(result.trace.outcome.message);
      }
      setPoint(result.step);
    } catch(e) {setError((e as Error).message);}
  }
  function reset() {setBranch(null);setChoice(null);setError('');setPoint('final');}
  const originalPoint=comparisonCheckpoint(trace,point), modifiedPoint=branch ? comparisonCheckpoint(branch.trace,point) : null;
  const before=resourceDifferences(originalPoint?.shared,modifiedPoint?.shared);
  const after=resourceDifferences(originalPoint?.event?.shared_state_after,modifiedPoint?.event?.shared_state_after);
  const waitsChanged=recordedDifference(originalPoint?.waits,modifiedPoint?.waits);
  const threadsChanged=recordedDifference(originalPoint?.threads,modifiedPoint?.threads);
  return <section className="panel what-if" aria-label="What-If Schedule Explorer">
    <div className="panel-heading"><h2>⑂ What-If Schedule Explorer</h2><button className="button small" disabled={busy || (!branch && choice===null && !error)} onClick={reset}>Reset to Original</button></div>
    <div className="branch-builder">
      <div><strong>{event ? `Change decision ${cursor}` : 'Select a timeline decision'}</strong><p>{event ? `Original choice: T${event.thread}. ${cursor===0 ? 'No earlier decisions to preserve' : `Keep steps 0–${cursor-1}`}, then choose a runnable alternative.` : 'The outcome is a stop checkpoint, not a scheduling choice.'}</p></div>
      {event && <div className="branch-choices" role="group" aria-label={`Alternatives at step ${cursor}`}>
        {choices.map(t=><button key={t.id} className={`choice ${choice===t.id ? 'active' : ''}`} style={{'--thread':colors[t.id%colors.length]} as CSSProperties}
          aria-pressed={choice===t.id} aria-label={`Choose thread ${t.id}${t.reason ? `: ${t.reason}` : ''}`} disabled={busy || t.reason!==null || !canReplay(trace) || !connected}
          onClick={()=>setChoice(t.id)}><b>T{t.id}</b><small>{t.reason ?? 'runnable alternative'}</small></button>)}
      </div>}
      <div className="branch-submit"><button className="button primary" disabled={busy || choice===null || !event || !connected || !canReplay(trace)} onClick={()=>void execute()}>{busy ? 'Engine busy…' : 'Run what-if'}</button><span>{event && available.length===0 ? 'No alternative is runnable at this decision. ' : ''}Fresh initial state · original limits · lowest runnable ID after the prefix.</span></div>
      {!canReplay(trace) && <p className="text-bad">This environmental/error trace cannot be used as a verified branch origin.</p>}
      {error && <div className="branch-error" role="alert">{error}</div>}
    </div>
    {branch && <>
      <div className="comparison-summary" role="status"><strong>{branch.status==='invalid_prefix' ? 'Prefix rejected by the engine' : branch.status==='unverified' ? 'New execution recorded · replay unverified' : `Outcome: ${trace.outcome.status} → ${branch.trace.outcome.status}`}</strong>
        <p>Original replay verified. Modified replay {branch.replay?.verified ? 'verified' : branch.replay?.status ?? 'unavailable'}. {divergence===null ? 'No scheduling divergence recorded.' : `First scheduling divergence: step ${divergence}${trace.schedule[divergence]===undefined || branch.trace.schedule[divergence]===undefined ? ' (one execution ended)' : ` · T${trace.schedule[divergence]} → T${branch.trace.schedule[divergence]}`}.`}</p>
        <code>Requested prefix [{branch.prefix.join(', ')}] · suffix: lowest runnable ID</code>
        {branch.trace.outcome.message && <p>{branch.trace.outcome.message}</p>}
      </div>
      <div className="comparison-controls"><span>{point==='final' ? 'Comparing final outcomes' : `Comparing recorded checkpoint ${point}`}</span><div>
        <button className="button small" disabled={divergence===null} onClick={()=>setPoint(divergence!)}>First divergence</button>
        <button className="button small" onClick={()=>setPoint('final')}>Final outcomes</button>
      </div></div>
      <div className="comparison-grid">
        {[{name:'Original',current:trace,frame:originalPoint},{name:'Modified',current:branch.trace,frame:modifiedPoint}].map(({name,current,frame},side)=><article className="comparison-trace" key={name} aria-label={`${name} trace comparison`}>
          <div className="comparison-title"><strong>{name}<small>{side===0 ? 'preserved trace' : 'fresh C++ execution'}</small></strong><span className={current.outcome.status==='completed' ? 'text-good' : 'text-bad'}>{current.outcome.status}</span><button className="icon-button" aria-label={`Download ${name.toLowerCase()} trace`} onClick={()=>download(current)}>↓</button></div>
          <div className="comparison-timeline" tabIndex={0} aria-label={`${name} horizontal thread timeline`}><div style={{display:'grid',gridTemplateColumns:`62px repeat(${current.events.length+1}, 68px)`,width:'max-content',gap:'4px'}}>
            <span className="mini-label">THREAD</span>{current.events.map(e=><button key={e.step} className={`mini-heading ${divergence===e.step ? 'divergent' : ''} ${point===e.step ? 'focused' : ''}`} onClick={()=>setPoint(e.step)} aria-label={`${name} checkpoint ${e.step}`}>{e.step}{divergence===e.step ? ' ↯' : ''}</button>)}<button className="mini-heading" onClick={()=>setPoint('final')}>Outcome</button>
            {current.outcome.threads.map(t=><div style={{display:'contents','--thread':colors[t.id%colors.length]} as CSSProperties} key={t.id}><span className="mini-label">T{t.id}</span>{current.events.map(e=><button key={e.step} onClick={()=>setPoint(e.step)} aria-label={`${name} step ${e.step} thread ${t.id}: ${e.thread===t.id ? e.operation : e.threads[t.id].state}`} className={`mini-operation ${e.thread===t.id ? 'executed' : 'idle'} ${divergence===e.step ? 'divergent' : ''} ${point===e.step ? 'focused' : ''} ${e.observation.assertion_passed===false && e.thread===t.id ? 'failed' : ''}`}>
              {e.thread===t.id ? e.operation.replace('assert_equal','assert') : e.threads[t.id].state==='blocked' ? 'blocked' : e.threads[t.id].state==='terminated' ? 'finished' : '·'}</button>)}<span className="mini-final">{t.state}</span></div>)}
          </div></div>
          <div className="comparison-state">
            {!frame ? <p className="muted">This execution did not reach checkpoint {point}. No state is extrapolated.</p> : <>
              <h3>{frame.terminal ? 'Final stop state' : `Before step ${frame.cursor} · T${frame.event!.thread} ${frame.event!.operation}`}</h3>
              {before.length ? before.map(row=><div key={row.name} className={`difference-row ${row.changed ? 'changed' : ''}`}><code>{row.name}</code><span>{value(side===0 ? row.original : row.modified,frame.shared!==undefined)}</span>{row.changed && <small>differs</small>}</div>) : <p className="muted">{frame.shared===undefined ? 'Snapshots not recorded.' : 'No registered resources.'}</p>}
              {frame.event && <><h3>After this operation</h3>{after.map(row=><div key={row.name} className={`difference-row ${row.changed ? 'changed' : ''}`}><code>{row.name}</code><span>{value(side===0 ? row.original : row.modified,frame.event!.shared_state_after!==undefined)}</span>{row.changed && <small>differs</small>}</div>)}{frame.event.shared_state_after===undefined && <p className="muted">After-state not recorded.</p>}</>}
              <h3 className={threadsChanged ? 'text-amber' : ''}>Thread states {frame.terminal ? 'at stop' : 'before decision'} {threadsChanged ? '· differ' : ''}</h3><p className="mono muted">{frame.threads.map(t=>`T${t.id}: ${t.state}`).join(' · ')}</p>
              <h3 className={waitsChanged ? 'text-amber' : ''}>Wait dependencies {frame.terminal ? 'at stop' : 'before decision'} {waitsChanged ? '· differ' : ''}</h3><p className="mono muted">{frame.waits===undefined ? 'Not recorded' : frame.waits.length ? frame.waits.map(w=>`T${w.thread} → T${w.waits_for} (${w.resource || w.kind})`).join(' · ') : 'None'}</p>
              {frame.event?.observation.assertion_passed!==null && frame.event?.observation.assertion_passed!==undefined && <p className={frame.event.observation.assertion_passed ? 'text-good' : 'text-bad'}>Assertion {frame.event.observation.assertion_passed ? 'passed' : 'failed'}: {frame.event.observation.before}, expected {frame.event.observation.expected}.</p>}
            </>}
          </div>
        </article>)}
      </div>
      <p className="comparison-footnote">Aligned by logical decision index. Operations after divergence can differ. Final outcomes compares each trace's actual stop state, even when lengths differ.</p>
      <details className="branch-log"><summary>Actual branch and replay output</summary><pre>{branch.output}{branch.replay ? `\n${branch.replay.output}` : ''}</pre></details>
    </>}
  </section>;
}
