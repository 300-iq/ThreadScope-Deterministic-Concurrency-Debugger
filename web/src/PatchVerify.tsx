import { useState } from 'react';
import type { Exploration, PatchOptions, PatchVerification, ReplayResult } from '../shared/api.ts';
import { canReplay, isFailure, type Trace } from '../shared/trace.ts';
import { demoSettings, implementationStats, parsePatchVerification, patchScenario, patchScenarios } from '../shared/patch.ts';

type Implementation='broken'|'corrected';
interface Props {
  busy: boolean; connected: boolean;
  compare: (options: PatchOptions)=>Promise<PatchVerification>;
  replay: (trace: Trace)=>Promise<ReplayResult>;
  inspect: (exploration: Exploration,index: number,replay?: ReplayResult)=>void;
}
function searchStatus(run: Exploration) {
  if(run.summary.incomplete) return `Incomplete search${run.summary.scheduleBoundReached ? ' · schedule cap' : ''}${run.summary.stepBoundReached ? ' · step bound' : ''}${implementationStats(run).interrupted && !run.summary.stepBoundReached ? ' · interrupted execution' : ''}`;
  return run.summary.failures ? 'Failures observed · modeled tree exhausted' : 'No observed failures · modeled tree exhausted';
}
export default function PatchVerify({busy,connected,compare,replay,inspect}: Props) {
  const [family,setFamily]=useState<PatchOptions['scenario']>('counter');
  const [maxSchedules,setMaxSchedules]=useState<number>(demoSettings.maxSchedules),[maxSteps,setMaxSteps]=useState<number>(demoSettings.maxSteps),[timeoutMs,setTimeoutMs]=useState<number>(demoSettings.timeoutMs);
  const [result,setResult]=useState<PatchVerification|null>(null),[active,setActive]=useState<Implementation>('broken');
  const [selected,setSelected]=useState({broken:0,corrected:0}),[replays,setReplays]=useState<Record<string,ReplayResult>>({});
  const [error,setError]=useState(''),[pending,setPending]=useState<'compare'|'replay'|null>(null);
  const pair=patchScenario(family), implementation=result?.[active], index=selected[active], trace=implementation?.traces[index];
  const verification=replays[`${active}:${index}`];
  const settingsChanged=result && (result.settings.maxSchedules!==maxSchedules || result.settings.maxSteps!==maxSteps || result.settings.timeoutMs!==timeoutMs);
  function clear() {setResult(null);setReplays({});setSelected({broken:0,corrected:0});setError('');}
  async function runBoth() {
    clear();setPending('compare');
    try {
      const next=parsePatchVerification(await compare({scenario:family,maxSchedules,maxSteps,timeoutMs}));setResult(next);setActive('broken');
      const failure=next.broken.traces.findIndex(t=>isFailure(t.outcome.status));setSelected({broken:Math.max(0,failure),corrected:0});
    } catch(e) {setError((e as Error).message);} finally {setPending(null);}
  }
  async function replayFailure() {
    if(!trace || active!=='broken' || !isFailure(trace.outcome.status) || !canReplay(trace)) return;
    const key=`broken:${index}`;setPending('replay');setError('');
    try {const next=await replay(trace);setReplays(old=>({...old,[key]:next}));}
    catch(e) {setError((e as Error).message);} finally {setPending(null);}
  }
  return <section className="patch-workflow" aria-label="Patch & Verify workflow">
    <div className="patch-intro"><div><div className="eyebrow">COMPARE ACTUAL IMPLEMENTATIONS</div><h2>Patch & Verify</h2><p>Run the existing Broken and Corrected programs from fresh state with identical search settings.</p></div><span className="patch-demo">Quick demo: Compare → Replay broken failure → Inspect trace</span></div>
    <div className="notice bounded patch-limit" role="note"><div><strong>No observed failure in a bounded search does not prove general correctness.</strong><p>Only instrumented operations are controlled. Different implementations have different scheduling trees; identical bounds do not imply identical schedules or coverage. OS timeouts remain diagnostic.</p></div></div>
    <form className="run-panel patch-run" onSubmit={e=>{e.preventDefault();void runBoth();}}>
      <label className="scenario-select"><span className="field-label">SUPPORTED SCENARIO</span><select aria-label="Comparison scenario" value={family} disabled={busy} onChange={e=>{setFamily(e.target.value as PatchOptions['scenario']);clear();setActive('broken');}}>{patchScenarios.map(pair=><option key={pair.id} value={pair.id}>{pair.name}</option>)}</select><small>Both implementations use these bounds.</small></label>
      <div className="bounds"><label><span>Schedules / implementation</span><input aria-label="Comparison maximum schedules" type="number" min="1" max="100" required value={maxSchedules} disabled={busy} onChange={e=>setMaxSchedules(e.target.valueAsNumber)}/></label><label><span>Steps / run</span><input aria-label="Comparison maximum steps" type="number" min="1" max="256" required value={maxSteps} disabled={busy} onChange={e=>setMaxSteps(e.target.valueAsNumber)}/></label><label><span>Timeout (ms)</span><input aria-label="Comparison timeout milliseconds" type="number" min="100" max="5000" required value={timeoutMs} disabled={busy} onChange={e=>setTimeoutMs(e.target.valueAsNumber)}/></label></div>
      <button className="button primary explore" type="submit" disabled={busy || !connected}>{pending==='compare' ? <><span className="spinner"/>Running both…</> : 'Compare implementations'}</button>
    </form>
    {error && <div className="notice error" role="alert"><div><strong>Comparison request failed</strong><p>{error}</p></div></div>}
    {pending==='compare' && <div className="notice" role="status"><div><strong>Running two real C++ searches</strong><p>No results are shown until both bounded explorations return their recorded traces. Each schedule starts with fresh resources and workers.</p></div></div>}
    {result && <div className="patch-settings" role="status"><span>Recorded settings: <b>{result.settings.maxSchedules}</b> schedules / implementation · <b>{result.settings.maxSteps}</b> steps / run · <b>{result.settings.timeoutMs} ms</b> timeout / run</span>{settingsChanged && <strong>Settings changed — compare again to apply them.</strong>}</div>}
    <div className="patch-cards" role="group" aria-label="Implementation comparison">
      {(['broken','corrected'] as const).map(kind=>{
        const run=result?.[kind], stats=run ? implementationStats(run) : null;
        return <article className={`panel implementation-card ${active===kind ? 'active' : ''}`} key={kind}>
          <button className="implementation-select" aria-pressed={active===kind} aria-label={`Select ${kind} implementation`} disabled={busy} onClick={()=>setActive(kind)}><span className={`implementation-tag ${kind}`}>{kind==='broken' ? 'Broken' : 'Corrected'}</span><strong>{pair[kind]}</strong><span>{active===kind ? 'Selected' : 'Inspect'}</span></button>
          <div className="implementation-counts"><div><span>Schedules explored</span><b>{stats?.explored ?? '—'}</b></div><div><span>Assertion failures</span><b className={stats?.assertions ? 'text-bad' : ''}>{stats?.assertions ?? '—'}</b></div><div><span>Deadlocks</span><b className={stats?.deadlocks ? 'text-bad' : ''}>{stats?.deadlocks ?? '—'}</b></div></div>
          <div className={`implementation-status ${run?.summary.incomplete ? 'text-amber' : run?.summary.failures ? 'text-bad' : run ? 'text-good' : ''}`}>{run ? searchStatus(run) : 'Awaiting actual C++ exploration'}{stats && <small>{stats.completed} completed · {stats.interrupted} bounded/error outcomes · exit {run!.exitCode}</small>}</div>
          <pre aria-label={`${kind} implementation excerpt`}>{kind==='broken' ? pair.brokenCode : pair.correctedCode}</pre>
        </article>;
      })}
    </div>
    <section className="panel patch-difference"><div className="panel-heading"><h2>Engineering difference</h2><span>Existing instrumented C++</span></div><div className="detail-content"><p>{pair.difference}</p><p className="muted">The excerpts show the relevant operations from the compiled examples. This workflow selects existing implementations; it does not rewrite source code.</p></div></section>
    {implementation && trace && <section className="panel patch-evidence" aria-label="Implementation evidence"><div className="panel-heading"><h2>{active==='broken' ? 'Broken' : 'Corrected'} implementation evidence</h2><span className="mono">{trace.program}</span></div>
      <div className="patch-trace-controls"><label>Recorded schedule<select aria-label="Comparison recorded schedule" disabled={busy} value={index} onChange={e=>setSelected(old=>({...old,[active]:Number(e.target.value)}))}>{implementation.traces.map((t,i)=><option key={i} value={i}>Schedule {i+1} · {t.outcome.status} · {t.events.length} operations</option>)}</select></label><div>
        {active==='broken' && <button className="button primary" disabled={busy || !connected || !isFailure(trace.outcome.status) || !canReplay(trace)} onClick={()=>void replayFailure()}>{pending==='replay' ? <><span className="spinner"/>Replaying…</> : 'Replay broken failure'}</button>}
        <button className="button secondary" disabled={busy} onClick={()=>inspect(implementation,index,verification)}>Inspect {active==='broken' ? 'broken' : 'corrected'} trace</button>
      </div></div>
      <p className="patch-schedule mono">Actual choices: {trace.schedule.map(id=>`T${id}`).join(' → ') || 'No executed operations'}</p>
      {active==='broken' && !isFailure(trace.outcome.status) && <p className="inline-note">This recorded schedule has no assertion failure or deadlock. Select a failing schedule, or increase the exploration bounds if none were observed.</p>}
      {verification && <div role="status" className={`replay-result ${verification.verified ? 'good' : 'bad'}`}>{verification.verified ? `Broken replay verified by C++. The recorded ${trace.outcome.status} was reproduced; it remains a program failure.` : `Replay ${verification.status}. See the actual engine diagnostic below.`}</div>}
      <details className="branch-log"><summary>Actual {active} exploration{verification ? ' and replay' : ''} output</summary><pre>{implementation.output}{verification ? `\n${verification.output}` : ''}</pre></details>
    </section>}
    <div className="notice bounded replay-boundary"><div><strong>Replay stays with the implementation that recorded the trace.</strong><p>{pair.replayDifference} A trace binds to its program ID, decisions, runnable sets, operations and observations. The corrected program is explored independently; we never relabel or silently reuse a broken trace against it.</p></div></div>
    <p className="patch-footnote">Counts are terminal schedule outcomes: each execution stops at its first assertion failure or deadlock. Exhausting these finite modeled trees does not establish general race freedom or deadlock freedom.</p>
  </section>;
}
