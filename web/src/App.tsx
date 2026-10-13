import { api } from './api.ts';
import { useEffect, useRef, useState, type CSSProperties } from 'react';
import { parseTrace, canReplay, isFailure, type Trace, type Status, type ThreadState } from '../shared/trace.ts';
import type { BranchResult, Exploration, PatchOptions, PatchVerification, ReplayResult, Scenario } from '../shared/api.ts';
import { checkpoint, failureIndex, navigate, type Navigation } from './navigation.ts';
import WhatIf from './WhatIf.tsx';
import PatchVerify from './PatchVerify.tsx';

const colors=['#65d6c1','#a498fa','#e8b978','#6eb9e9','#ec96b3'];
const labels: Record<Status,string>={completed:'Passed',assertion_failed:'Assertion failed',deadlock:'Deadlock',step_limit:'Step bound',timeout:'Timed out',replay_mismatch:'Diverged',worker_error:'Worker error'};
const shortOp={load:'load',store:'store',fetch_add:'fetch_add',assert_equal:'assert',lock:'acquire',unlock:'release',join:'join',yield:'yield'};
function Icon({name,size=18}: {name:string;size?:number}) {
  const paths: Record<string,string>={ play:'m8 5 11 7-11 7V5Z', back:'m15 6-6 6 6 6',forward:'m9 6 6 6-6 6',start:'M5 5v14m13-13-8 6 8 6',end:'M19 5v14M6 6l8 6-8 6',scope:'M3 3h6M3 3v6m18-6h-6m6 0v6M3 21h6m-6 0v-6m18 6h-6m6 0v-6M8 12h8m-4-4v8',layers:'m12 3 10 5-10 5L2 8l10-5Zm-10 10 10 5 10-5M2 18l10 5 10-5',check:'m5 12 4 4L19 6',alert:'m12 3 10 18H2L12 3Zm0 5v6m0 3v1',code:'m8 5-6 7 6 7m8-14 6 7-6 7m-3-18-2 22',lock:'M6 10h12v11H6V10Zm3 0V6a3 3 0 0 1 6 0v4m-3 4v3',refresh:'M20 8a8 8 0 1 0 1 7M20 3v5h-5',upload:'M12 16V3m-5 5 5-5 5 5M4 15v6h16v-6',download:'M12 3v13m-5-5 5 5 5-5M4 17v4h16v-4',arrow:'M4 12h16m-6-6 6 6-6 6',settings:'M4 6h16M4 12h16M4 18h16M8 3v6m8 0v6M10 15v6',file:'M6 3h8l4 4v14H6V3Zm8 0v5h4M9 12h6m-6 4h6',close:'m6 6 12 12M6 18 18 6'};
  return <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true"><path d={paths[name] ?? paths.scope}/></svg>;
}
function download(trace:Trace) { const url=URL.createObjectURL(new Blob([JSON.stringify(trace,null,2)+'\n'],{type:'application/json'})); const link=document.createElement('a'); link.href=url;link.download=`${trace.program.replace('/','-')}-${trace.outcome.status}.json`;link.click();setTimeout(()=>URL.revokeObjectURL(url),1000); }
function Badge({status}: {status:Status}) { return <span className={`badge ${isFailure(status) ? 'bad' : status === 'completed' ? 'good' : 'warn'}`}><span/>{labels[status]}</span>; }

export default function App() {
  const [view,setView]=useState<'debugger'|'patch'>('debugger');
  const [scenarios,setScenarios]=useState<Scenario[]>([]),[connection,setConnection]=useState<'loading'|'ready'|'error'>('loading');
  const [scenario,setScenario]=useState('lost-update'),[maxSchedules,setMaxSchedules]=useState(32),[maxSteps,setMaxSteps]=useState(64),[timeoutMs,setTimeoutMs]=useState(1000);
  const [result,setResult]=useState<Exploration|null>(null),[imported,setImported]=useState<Trace|null>(null),[selected,setSelected]=useState(0),[cursor,setCursor]=useState(0);
  const [busy,setBusy]=useState<'explore'|'replay'|'branch'|'compare'|null>(null),[error,setError]=useState(''),[replays,setReplays]=useState<Record<number,ReplayResult>>({});
  const [branchVerified,setBranchVerified]=useState(false);
  const [logs,setLogs]=useState(false);
  const inputRef=useRef<HTMLInputElement>(null), selectedBlock=useRef<HTMLButtonElement>(null),request=useRef<AbortController|null>(null);
  const traces=imported ? [imported] : result?.traces ?? [],trace=traces[selected], frame=trace ? checkpoint(trace,cursor) : null;
  const replay=replays[selected];
  useEffect(()=>{setBranchVerified(false);},[trace]);
  async function connect() { setConnection('loading');setError('');try { const data=await api<{scenarios:Scenario[]}>('scenarios');setScenarios(data.scenarios);setConnection('ready'); } catch(e) {setConnection('error');setError((e as Error).message);} }
  useEffect(()=>{ void connect();return ()=>request.current?.abort(); },[]);
  useEffect(()=>{ selectedBlock.current?.scrollIntoView({block:'nearest',inline:'nearest',behavior:'instant'}); },[cursor,selected]);
  function go(action:Navigation) { if(trace) setCursor(navigate(trace,cursor,action)); }
  useEffect(()=>{
    const key=(e:KeyboardEvent)=>{ if(view!=='debugger' || !trace || busy || e.altKey || e.ctrlKey || e.metaKey || (e.target instanceof HTMLElement && ['INPUT','SELECT','TEXTAREA'].includes(e.target.tagName))) return;
      const action=e.key === 'ArrowRight' ? 'forward' : e.key === 'ArrowLeft' ? 'back' : e.key === 'Home' ? 'start' : e.key === 'End' ? 'end' : undefined;
      if(action) {e.preventDefault();setCursor(current=>navigate(trace,current,action));} };
    window.addEventListener('keydown',key);return ()=>window.removeEventListener('keydown',key);
  },[trace,busy,view]);
  async function explore() {
    setBusy('explore');setError('');setResult(null);setImported(null);setReplays({});setCursor(0);setSelected(0);
    const controller=new AbortController();request.current=controller;
    try { const data=await api<Exploration>('explore',{scenario,maxSchedules,maxSteps,timeoutMs},controller.signal);data.traces=data.traces.map(parseTrace);setResult(data);const first=data.traces.findIndex(t=>isFailure(t.outcome.status));setSelected(Math.max(0,first)); }
    catch(e) {setError((e as Error).message);} finally {setBusy(null);request.current=null;}
  }
  async function replayTrace() {
    if(!trace) return;setBusy('replay');setError('');const controller=new AbortController();request.current=controller;
    try { const data=await api<ReplayResult>('replay',{trace},controller.signal);setReplays(old=>({...old,[selected]:data})); }
    catch(e) {setError((e as Error).message);} finally {setBusy(null);request.current=null;}
  }
  async function branchTrace(step:number,thread:number):Promise<BranchResult> {
    if(!trace) throw new Error('No original trace selected.');
    setBusy('branch');setError('');const controller=new AbortController();request.current=controller;
    try {const data=await api<BranchResult>('branch',{trace,step,thread},controller.signal);data.trace=parseTrace(data.trace);return data;}
    finally {setBusy(null);request.current=null;}
  }
  async function compareImplementations(options:PatchOptions):Promise<PatchVerification> {
    setBusy('compare');setError('');const controller=new AbortController();request.current=controller;
    try {return await api<PatchVerification>('patch-verify',options,controller.signal);}
    finally {setBusy(null);request.current=null;}
  }
  async function replayImplementation(trace:Trace):Promise<ReplayResult> {
    setBusy('replay');setError('');const controller=new AbortController();request.current=controller;
    try {return await api<ReplayResult>('replay',{trace},controller.signal);}
    finally {setBusy(null);request.current=null;}
  }
  function inspectImplementation(exploration:Exploration,index:number,verification?:ReplayResult) {
    setResult(exploration);setImported(null);setSelected(index);setScenario(exploration.scenario);
    setMaxSchedules(exploration.options.maxSchedules);setMaxSteps(exploration.options.maxSteps);setTimeoutMs(exploration.options.timeoutMs);
    setCursor(failureIndex(exploration.traces[index]) ?? 0);setReplays(verification ? {[index]:verification} : {});setView('debugger');
  }
  async function importTrace(file?:File) {
    if(!file) return;setError('');try {if(file.size>8*1024*1024) throw new Error('Trace exceeds 8 MiB.');const next=parseTrace(await file.text());setImported(next);setView('debugger');setResult(null);setSelected(0);setCursor(0);setReplays({});}catch(e){setError((e as Error).message);} if(inputRef.current) inputRef.current.value='';
  }
  const passes=result?.traces.filter(t=>t.outcome.status==='completed').length;
  const failureTypes=result ? [...new Set(result.traces.filter(t=>isFailure(t.outcome.status)).map(t=>labels[t.outcome.status]))].join(' · ') : '';
  return <div className="app-shell">
    <aside className="sidebar">
      <a className="brand" href="/" aria-label="ThreadScope home"><span className="brand-icon"><Icon name="scope" size={22}/></span><span>ThreadScope<small>CONCURRENCY LAB</small></span></a>
      <div className="nav-caption">WORKSPACE</div><div className="nav-active"><Icon name="layers"/>{view==='patch' ? 'Patch & Verify' : 'Schedule debugger'}<span className="nav-mark"/></div>
      <div className="nav-caption library-label">SCENARIO LIBRARY <span>{scenarios.length || '—'}</span></div>
      <div className="scenario-library">{scenarios.map(s=><button key={s.name} className={s.name === scenario ? 'scenario-link active' : 'scenario-link'} disabled={!!busy} onClick={()=>{setView('debugger');setScenario(s.name);setResult(null);setImported(null);setReplays({});setCursor(0);}}><Icon name={s.name.startsWith('mutex') ? 'lock' : 'code'} size={16}/><span>{s.name}</span></button>)}</div>
      <div className="sidebar-bottom"><span className="code-tag">C++20</span><p>Real threads.<br/>Controlled interleavings.</p><div className="scope-note">Instrumented operations only.<br/>Bounded search is not a proof.</div><span className="version">LOCAL WORKSPACE <span>v0.1</span></span></div>
    </aside>
    <main>
      <header className="topbar"><div className="breadcrumb">Workspace <span>/</span> <strong>{view==='patch' ? 'Patch & Verify' : 'Schedule debugger'}</strong></div><div className={`engine-status ${connection}`}><span className="status-dot"/>{connection === 'ready' ? 'Local engine connected' : connection === 'loading' ? 'Connecting to engine' : 'Engine unavailable'}<span className="localhost">127.0.0.1</span></div></header>
      <div className="workspace">
        <section className="page-heading"><div><div className="eyebrow">DETERMINISTIC CONCURRENCY DEBUGGER</div><h1>Every interleaving tells a story.</h1><p>Explore execution orders. Find the failure. Replay the evidence.</p></div><button className="button secondary" onClick={()=>inputRef.current?.click()} disabled={!!busy}><Icon name="upload"/>Open trace</button><input ref={inputRef} type="file" accept=".json,application/json" hidden onChange={e=>void importTrace(e.target.files?.[0])}/></section>
        {error && <div role="alert" className="notice error"><Icon name="alert"/><div><strong>Unable to complete the request</strong><p>{error}</p></div>{connection==='error' && <button className="button small" onClick={()=>void connect()}>Reconnect</button>}<button className="icon-button" aria-label="Dismiss error" onClick={()=>setError('')}><Icon name="close"/></button></div>}
        <div className="workflow-tabs" role="group" aria-label="ThreadScope workflows"><button disabled={!!busy} aria-pressed={view==='debugger'} onClick={()=>setView('debugger')}>Schedule debugger</button><button disabled={!!busy} aria-pressed={view==='patch'} onClick={()=>setView('patch')}>Patch & Verify<span>COMPARE IMPLEMENTATIONS</span></button></div>
        <div hidden={view!=='patch'}><PatchVerify busy={!!busy} connected={connection==='ready'} compare={compareImplementations} replay={replayImplementation} inspect={inspectImplementation}/></div>
        <div hidden={view!=='debugger'}>
        <form className="run-panel" onSubmit={e=>{e.preventDefault();void explore();}}>
          <label className="scenario-select"><span className="field-label">TEST SCENARIO</span><select value={scenario} disabled={!!busy || connection !== 'ready'} onChange={e=>{setScenario(e.target.value);setResult(null);setImported(null);setReplays({});}}>{scenarios.length ? scenarios.map(s=><option value={s.name} key={s.name}>{s.name}</option>) : <option value="lost-update">Waiting for engine…</option>}</select><small>{scenarios.find(s=>s.name===scenario)?.description ?? 'Build the C++ executable to connect.'}</small></label>
          <div className="bounds"><Icon name="settings" size={16}/><label><span>Schedules</span><input aria-label="Maximum schedules" type="number" min="1" max="100" required value={maxSchedules} disabled={!!busy} onChange={e=>setMaxSchedules(e.target.valueAsNumber)}/></label><label><span>Steps / run</span><input aria-label="Maximum steps" type="number" min="1" max="256" required value={maxSteps} disabled={!!busy} onChange={e=>setMaxSteps(e.target.valueAsNumber)}/></label><label><span>Timeout (ms)</span><input aria-label="Run timeout in milliseconds" type="number" min="100" max="5000" required value={timeoutMs} disabled={!!busy} onChange={e=>setTimeoutMs(e.target.valueAsNumber)}/></label></div>
          <button className="button primary explore" type="submit" disabled={!!busy || connection !== 'ready'}>{busy==='explore' ? <span className="spinner"/> : <Icon name="play"/>}{busy==='explore' ? 'Exploring…' : 'Explore schedules'}</button>
        </form>
        <div className="summary-grid" aria-live="polite">
          <div className="stat"><span><Icon name="layers" size={16}/>Schedules explored</span><strong>{result?.summary.exploredSchedules ?? '—'}<small>{result ? result.summary.frontierExhausted ? 'bounded tree exhausted' : 'search incomplete' : imported ? 'imported trace' : 'awaiting execution'}</small></strong></div>
          <div className="stat"><span><Icon name="check" size={16}/>Completed passes</span><strong className={passes ? 'text-good' : ''}>{passes ?? '—'}<small>{result ? 'recorded complete runs' : 'no results yet'}</small></strong></div>
          <div className="stat"><span><Icon name="alert" size={16}/>Failures found</span><strong className={result?.summary.failures ? 'text-bad' : ''}>{result?.summary.failures ?? '—'}<small>{failureTypes || (result ? result.summary.incomplete ? 'none in explored prefix' : 'none in these schedules' : 'assertions & deadlocks')}</small></strong></div>
          <div className="stat"><span><Icon name="refresh" size={16}/>Selected replay</span><strong className="replay-stat">{busy==='replay' ? 'Checking…' : replay?.verified || branchVerified ? 'Verified' : replay?.status === 'diverged' ? 'Diverged' : replay ? 'Unverified' : 'Not run'}<small>{replay?.verified || branchVerified ? 'C++ CLI confirmed a match' : 'explicit verification required'}</small></strong></div>
        </div>
        {busy==='explore' && <div className="notice loading" role="status"><span className="spinner"/><div><strong>Exploring with the C++ engine</strong><p>Waiting for recorded schedules. No execution data is displayed until the engine returns.</p></div></div>}
        {result?.summary.incomplete && <div className="notice bounded"><Icon name="alert"/><div><strong>Search incomplete{result.summary.scheduleBoundReached ? ' · schedule limit reached' : result.summary.stepBoundReached ? ' · step limit reached' : ' · execution interrupted'}</strong><p>Only the recorded runs below were explored. Increase the bounds to search further; no correctness proof is implied.</p></div></div>}
        {result && !result.summary.incomplete && result.summary.failures===0 && <div className="notice no-failure"><Icon name="check"/><div><strong>No failures in the explored schedules</strong><p>The finite modeled tree was exhausted within these bounds. This is not a universal race-freedom claim.</p></div></div>}
        {imported && <div className="notice bounded"><Icon name="file"/><div><strong>Imported trace · {imported.program}</strong><p>{branchVerified || replay?.verified ? 'The local C++ engine verified this imported history and outcome through replay.' : 'This file has not been verified by the local engine. Verify replay or run a what-if branch to check it.'}</p></div></div>}
        <div className={`debug-layout ${trace ? '' : 'empty-layout'}`}>
          <div className="execution-area">
            <section className="panel schedule-panel"><div className="panel-heading"><h2><Icon name="layers"/>Recorded schedules</h2><span>{traces.length ? `${traces.length} ${imported ? 'file' : 'actual runs'}` : 'No run loaded'}</span></div>
              {traces.length ? <div className="schedule-list" role="group" aria-label="Recorded schedules">{traces.map((t,i)=><button className={`schedule-card ${selected===i ? 'selected' : ''}`} key={i} disabled={!!busy} aria-pressed={selected===i} onClick={()=>{setSelected(i);setCursor(0);}}><span className="schedule-number">{String(i+1).padStart(2,'0')}</span><div><Badge status={t.outcome.status}/><small>{t.events.length} operations · {t.schedule.length ? t.schedule.map(id=>`T${id}`).join(' → ') : 'No executed operations'}</small></div></button>)}</div> : <div className="empty-schedules">Your engine results will appear here. Passing and failing schedules are both recorded.</div>}
            </section>
            <section className="panel timeline-panel"><div className="panel-heading"><h2><Icon name="scope"/>Thread timeline</h2><span className="mono">{trace ? trace.program : 'EXPLICIT INSTRUMENTATION'}</span></div>
              {trace && frame ? <>
                <div className="timeline-toolbar"><div className="playback"><button className="icon-button" aria-label="Jump to start" disabled={cursor===0} onClick={()=>go('start')}><Icon name="start"/></button><button className="icon-button" aria-label="Step backward" disabled={cursor===0} onClick={()=>go('back')}><Icon name="back"/></button><span className="cursor-readout">{frame.terminal ? 'Outcome' : `Step ${cursor}`} <small>/ {trace.events.length}</small></span><button className="icon-button" aria-label="Step forward" disabled={frame.terminal} onClick={()=>go('forward')}><Icon name="forward"/></button><button className="icon-button" aria-label="Jump to outcome" disabled={frame.terminal} onClick={()=>go('end')}><Icon name="end"/></button></div><button className="button small jump-failure" disabled={failureIndex(trace)===null} onClick={()=>go('failure')}><Icon name="alert" size={15}/>Jump to failure</button></div>
                <div className="timeline-scroll" tabIndex={0} aria-label="Horizontal thread timeline"><div className="timeline-grid" style={{gridTemplateColumns:`105px repeat(${trace.events.length+1}, 103px)`}}>
                  <div className="lane-label heading">THREAD</div>{trace.events.map(e=><div key={e.step} className={`step-heading ${cursor===e.step ? 'current' : ''}`}>STEP {String(e.step).padStart(2,'0')}</div>)}<div className={`step-heading ${frame.terminal ? 'current' : ''}`}>OUTCOME</div>
                  {trace.outcome.threads.map(t=><div className="thread-lane" key={t.id} style={{display:'contents','--thread':colors[t.id%colors.length]} as CSSProperties}><div className="lane-label"><span className="thread-dot"/>Thread {t.id}</div>{trace.events.map(e=>{
                    const chosen=e.thread===t.id,state=e.threads[t.id].state;
                    return <div key={e.step} className={`timeline-cell ${cursor===e.step ? 'selected-column' : ''}`}>
                      {chosen ? <button ref={cursor===e.step ? selectedBlock : undefined} aria-label={`Step ${e.step}: thread ${t.id} ${e.operation}`} className={`operation ${e.type} ${e.observation.assertion_passed===false ? 'failed' : ''} ${cursor===e.step ? 'selected-operation' : ''}`} onClick={()=>setCursor(e.step)}><span className="op-top">{shortOp[e.operation]}{e.observation.assertion_passed===false ? <Icon name="alert" size={12}/> : e.type==='lock_acquire' ? <Icon name="lock" size={12}/> : null}</span><small>{e.resource || e.label || 'checkpoint'}</small></button> : <button className={`idle-state ${state}`} aria-label={`Thread ${t.id} ${state} before step ${e.step}`} onClick={()=>setCursor(e.step)}>{state==='blocked' ? <><Icon name="lock" size={12}/>blocked</> : state==='terminated' ? 'finished' : <span className="idle-dash"/>}</button>}
                    </div>;
                  })}<div className={`timeline-cell terminal ${frame.terminal ? 'selected-column' : ''}`}><button ref={frame.terminal && t.id===0 ? selectedBlock : undefined} className={`idle-state ${t.state}`} onClick={()=>setCursor(trace.events.length)}>{t.state}</button></div></div>)}
                </div></div>
                <div className="timeline-legend"><span><i className="legend-operation"/>executed operation</span><span><i className="legend-blocked"/>blocked request</span><span><i className="legend-failure"/>assertion failure</span><small>Columns are logical steps, not elapsed time.</small></div>
                <div className="trace-actions"><div><Badge status={trace.outcome.status}/><span>{frame.terminal ? 'Final stop decision · before cancellation' : `Checkpoint before step ${cursor} · next operation T${frame.event!.thread}`}</span></div><div><button className="button small" disabled={!!busy || !canReplay(trace) || connection!=='ready'} onClick={()=>void replayTrace()}>{busy==='replay' ? <span className="spinner"/> : <Icon name="refresh" size={15}/>}Verify replay</button><button className="icon-button" aria-label="Download selected trace JSON" onClick={()=>download(trace)}><Icon name="download"/></button></div></div>
                {!canReplay(trace) && <p className="inline-note">Environmental diagnostics cannot be replay verified.</p>}
                {replay && <div className={`replay-result ${replay.verified ? 'good' : 'bad'}`} role="status">{replay.verified ? <Icon name="check"/> : <Icon name="alert"/>}{replay.verified ? 'Replay verified by the C++ engine. Recorded events and outcome match.' : replay.status==='diverged' ? 'Replay diverged from this trace. See engine output for the diagnostic.' : 'Replay could not be verified. See engine output.'}</div>}
              </> : <div className="timeline-empty"><span className="empty-icon"><Icon name="scope" size={35}/></span><h3>See what happened between the threads.</h3><p>Choose a scenario and explore its schedules.<br/>Every operation here will come from a recorded C++ trace.</p><button className="button secondary" disabled={!!busy || connection!=='ready'} onClick={()=>void explore()}>Explore your first schedule<Icon name="arrow" size={16}/></button><div className="empty-footnote">No simulated execution · No invented state</div></div>}
            </section>
            {trace && <WhatIf trace={trace} cursor={cursor} busy={!!busy} connected={connection==='ready'} run={branchTrace} onVerified={setBranchVerified} download={download}/> }
            {trace && frame && <section className="panel event-detail"><div className="panel-heading"><h2><Icon name="code"/>Event details</h2><span>{frame.terminal ? 'STOP DECISION' : `EVENT ${cursor}`}</span></div>{frame.event ? <div className="detail-content"><div className="detail-title"><span className="thread-token" style={{color:colors[frame.event.thread%colors.length]}}>T{frame.event.thread}</span><code>{frame.event.operation}{frame.event.resource ? `(${frame.event.resource})` : '()'}</code><span className="type-label">{frame.event.type.replaceAll('_',' ')}</span></div><p>{frame.event.label || (frame.event.operation==='lock' ? 'The modeled lock was available; this acquisition was selected and ownership changed without a native lock wait.' : frame.event.operation==='unlock' ? 'The owner released this modeled lock. Pending requests are reconsidered at the next scheduling barrier.' : frame.event.operation==='join' ? 'All requested logical threads had terminated before this join was enabled.' : 'One explicitly instrumented operation was selected from the runnable set.')}</p><div className="observation-row"><span>Observed before <b>{frame.event.observation.before ?? '—'}</b></span><Icon name="arrow" size={16}/><span>Observed after <b>{frame.event.observation.after ?? '—'}</b></span>{frame.event.observation.expected !== null && <span>Assertion expects <b>{frame.event.observation.expected}</b></span>}</div>{frame.event.observation.assertion_passed !== null && <p className={frame.event.observation.assertion_passed ? 'text-good' : 'text-bad'}>Assertion {frame.event.observation.assertion_passed ? 'passed' : 'failed'}: observed {frame.event.observation.before}, expected {frame.event.observation.expected}.</p>}{frame.event.observation.error && <p className="text-bad">{frame.event.observation.error}</p>}</div> : <div className="detail-content"><Badge status={trace.outcome.status}/><p>{trace.outcome.message || 'Every logical worker terminated. This trace records no assertion failure.'}</p><p className="muted">The terminal checkpoint is not an additional executed operation.</p></div>}</section>}
            {(result || replay) && <section className="engine-log"><button aria-expanded={logs} onClick={()=>setLogs(!logs)}><Icon name="code" size={15}/>Engine output<span>{logs ? '−' : '+'}</span></button>{logs && <pre>{replay?.output ?? result?.output}</pre>}</section>}
          </div>
          <aside className="inspector">
            <section className="panel"><div className="panel-heading"><h2><Icon name="layers"/>Shared state</h2><span className="live-label">{frame?.terminal ? 'FINAL' : frame ? 'BEFORE' : '—'}</span></div><div className="inspector-content">{frame ? frame.shared ? frame.shared.length ? frame.shared.map(r=><div className="resource-row" key={r.name}><div><code>{r.name}</code><small>{r.kind==='atomic_int' ? 'std::atomic<int>' : 'modeled mutex'}</small></div><span className="resource-value">{r.kind==='mutex' ? r.value===null ? 'unowned' : `T${r.value}` : r.value}</span></div>) : <p className="muted">No registered resources.</p> : <p className="muted">Full snapshots were not recorded in this legacy trace. Operation observations remain available.</p> : <div className="inspector-placeholder"><Icon name="layers" size={24}/><p>Values appear at the selected checkpoint.</p></div>}</div></section>
            <section className="panel"><div className="panel-heading"><h2><Icon name="lock"/>Locks & waiting</h2></div><div className="inspector-content">{frame ? <>{frame.shared?.filter(r=>r.kind==='mutex').map(r=><div className="lock-row" key={r.name}><Icon name="lock" size={15}/><code>{r.name}</code><span>{r.value===null ? 'available' : `held by T${r.value}`}</span></div>)}{frame.waits === undefined ? <p className="muted">Dependencies were not recorded in this trace.</p> : frame.waits.length ? <div className="wait-edges">{frame.waits.map((w,i)=><div className="wait-edge" key={i}><span>T{w.thread}</span><Icon name="arrow" size={14}/><span>T{w.waits_for}</span><small>{w.kind==='mutex' ? `lock: ${w.resource}` : 'join: termination'}</small></div>)}</div> : <p className="muted">No blocked dependencies at this checkpoint.</p>}<p className="panel-footnote">Requests wait without an execution permit. Ownership is controlled by the scheduler.</p></> : <div className="inspector-placeholder"><Icon name="lock" size={24}/><p>Ownership and wait dependencies will appear here.</p></div>}</div></section>
            <section className="panel"><div className="panel-heading"><h2><Icon name="scope"/>Scheduling decision</h2></div><div className="inspector-content">{frame ? <>{frame.event && <div className="enabled-row"><span>Enabled</span><div>{frame.event.runnable.map(id=><span className={`thread-chip ${id===frame.event!.thread ? 'chosen' : ''}`} key={id} style={{'--thread':colors[id%colors.length]} as CSSProperties}>T{id}{id===frame.event!.thread ? ' ✓' : ''}</span>)}</div></div>}<div className="state-list">{frame.threads.map(t=><div key={t.id}><span><i style={{background:colors[t.id%colors.length]}}/>Thread {t.id}</span><span className={`state-tag ${t.state}`}>{t.state}</span></div>)}</div><p className="panel-footnote">{frame.terminal ? 'State recorded at the stop decision, before cleanup.' : 'Eligibility recorded before the selected operation. Arrival order is not a scheduling input.'}</p></> : <p className="muted inspector-content">Only runnable threads can be selected. Blocked requests are never shown as executed.</p>}</div></section>
          </aside>
        </div>
        </div>
        <footer><span><Icon name="lock" size={13}/>Local execution. Your traces stay on this machine.</span><span>Instrumented interleavings · No weak-memory analysis</span></footer>
      </div>
    </main>
  </div>;
}
