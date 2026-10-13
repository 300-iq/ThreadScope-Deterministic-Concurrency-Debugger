import { spawn } from 'node:child_process';
import { mkdtemp, readFile, rm, stat, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { integer, isFailure, canReplay, parseTrace, record } from '../shared/trace.ts';
import type { BranchResult, ExploreOptions, Exploration, PatchOptions, PatchVerification, ReplayResult, Scenario, SearchSummary } from '../shared/api.ts';
import type { Trace } from '../shared/trace.ts';
import { patchScenario, parsePatchVerification } from '../shared/patch.ts';

export const repository = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const executable = join(repository,'build',process.platform === 'win32' ? 'threadscope.exe' : 'threadscope');
const scenarioNames = ['lost-update','lost-update-rmw','mutex-deadlock','mutex-ordered'];
export class ApiError extends Error { constructor(public status: number, message: string) { super(message); } }
function requestValue<T>(read:()=>T):T {
  try {return read();} catch(error) {throw error instanceof ApiError ? error : new ApiError(400,error instanceof Error ? error.message : 'Invalid request.');}
}
async function recordedTrace(path:string):Promise<{trace:Trace;bytes:number}> {
  try {
    const info=await stat(path);
    if(info.size>8*1024*1024) throw new Error('Recorded trace exceeds 8 MiB.');
    return {trace:parseTrace(await readFile(path,'utf8')),bytes:info.size};
  } catch(error) {throw new ApiError(502,`Engine did not produce a valid trace: ${error instanceof Error ? error.message : 'unknown output failure'}`);}
}
export function validateExplore(input: unknown): ExploreOptions {
  const value=record(input,'options');
  if(Object.keys(value).some(k=>!['scenario','maxSchedules','maxSteps','timeoutMs'].includes(k))) throw new ApiError(400,'Unknown option. Executable paths and commands are not accepted.');
  if(typeof value.scenario !== 'string' || !scenarioNames.includes(value.scenario)) throw new ApiError(400,'Choose a registered ThreadScope scenario.');
  return { scenario:value.scenario,maxSchedules:integer(value.maxSchedules,'maxSchedules',1,100),maxSteps:integer(value.maxSteps,'maxSteps',1,256),timeoutMs:integer(value.timeoutMs,'timeoutMs',100,5000) };
}
export function validatePatch(input: unknown): PatchOptions {
  const body=record(input,'comparison options');
  if(Object.keys(body).some(k=>!['scenario','maxSchedules','maxSteps','timeoutMs'].includes(k)))
    throw new ApiError(400,'Comparison accepts only a supported scenario and shared bounds. Arbitrary implementations or commands are not accepted.');
  const pair=patchScenario(body.scenario);
  const validated=validateExplore({...body,scenario:pair.broken});
  return {scenario:pair.id,maxSchedules:validated.maxSchedules,maxSteps:validated.maxSteps,timeoutMs:validated.timeoutMs};
}
export function parseSummary(output: string): SearchSummary {
  const pattern=/^runs=(\d+) failures=(\d+) frontier_exhausted=(true|false) incomplete=(true|false) schedule_bound_reached=(true|false) step_bound_reached=(true|false)$/gm;
  const matches=[...output.matchAll(pattern)];
  if(matches.length !== 1) throw new ApiError(502,'Engine did not return a complete exploration summary. No success result is available.');
  const m=matches[0]; return { exploredSchedules:Number(m[1]),failures:Number(m[2]),frontierExhausted:m[3]==='true',incomplete:m[4]==='true',scheduleBoundReached:m[5]==='true',stepBoundReached:m[6]==='true' };
}
export function invokeEngine(args: string[], signal?: AbortSignal, deadline=30_000): Promise<{output: string; exitCode: number}> {
  return new Promise((resolvePromise,reject) => {
    if(signal?.aborted) return reject(new ApiError(499,'Request cancelled.'));
    // The executable and cwd are fixed by this module, never supplied in HTTP.
    const child=spawn(executable,args,{cwd:repository,shell:false,stdio:['ignore','pipe','pipe']});
    let stdout='', stderr='', failure: string | undefined;
    const kill=(message: string) => { failure ??= message; child.kill('SIGKILL'); };
    const timer=setTimeout(()=>kill(`Engine exceeded the ${deadline/1000}-second process watchdog. Exploration/replay was not verified.`),deadline);
    const abort=()=>kill('Request cancelled; engine process terminated.');
    signal?.addEventListener('abort',abort,{once:true});
    const clean=()=>{ clearTimeout(timer); signal?.removeEventListener('abort',abort); };
    child.stdout.on('data',(data: Buffer)=>{ stdout+=data.toString(); if(Buffer.byteLength(stdout)>512*1024) kill('Engine output exceeded the local API limit.'); });
    child.stderr.on('data',(data: Buffer)=>{ stderr+=data.toString(); if(Buffer.byteLength(stderr)>128*1024) kill('Engine diagnostics exceeded the local API limit.'); });
    child.once('error',()=>{ clean(); reject(new ApiError(503,'Cannot launch build/threadscope. Build the C++ engine with CMake first.')); });
    child.once('close',(code,termination)=>{
      clean(); if(failure || termination || code === null) return reject(new ApiError(502,failure ?? 'Engine terminated without a result.'));
      resolvePromise({output:stdout+(stderr ? `\n${stderr}` : ''),exitCode:code});
    });
  });
}
export class EngineService {
  private busy=false;
  async scenarios(signal?: AbortSignal): Promise<Scenario[]> {
    const result=await invokeEngine(['--help'],signal,5000);
    if(result.exitCode !== 0 || !result.output.includes('--all-traces') || !result.output.includes('threadscope branch')) throw new ApiError(503,'The engine binary is out of date. Rebuild it with CMake.');
    return scenarioNames.map(name=>{
      const match=result.output.split('\n').find(line=>line.startsWith(`  ${name}: `));
      if(!match) throw new ApiError(503,`Engine does not expose registered scenario ${name}. Rebuild it.`);
      return {name,description:match.slice(name.length+4)};
    });
  }
  private async job<T>(work: (directory: string)=>Promise<T>): Promise<T> {
    if(this.busy) throw new ApiError(409,'An engine run is already in progress. Wait for it to finish.');
    this.busy=true;
    let directory: string | undefined;
    try { directory=await mkdtemp(join(tmpdir(),'threadscope-ui-')); return await work(directory); }
    finally { try { if(directory) await rm(directory,{recursive:true,force:true}); } finally { this.busy=false; } }
  }
  async explore(input: unknown,signal?: AbortSignal): Promise<Exploration> {
    const options=requestValue(()=>validateExplore(input));
    return this.job(directory=>this.runExploration(options,directory,signal));
  }
  private async runExploration(options: ExploreOptions,directory: string,signal?: AbortSignal): Promise<Exploration> {
    const result=await invokeEngine(['explore',options.scenario,'--max-schedules',String(options.maxSchedules),'--max-steps',String(options.maxSteps),'--timeout-ms',String(options.timeoutMs),'--all-traces','--out',directory],signal);
    const summary=parseSummary(result.output);
    if(summary.exploredSchedules < 1 || summary.exploredSchedules > options.maxSchedules) throw new ApiError(502,'Engine returned an inconsistent schedule count.');
    let bytes=0;
    const traces=[];
    for(let i=1;i<=summary.exploredSchedules;i++) {
      const {trace,bytes:traceBytes}=await recordedTrace(join(directory,`schedule-${i}.json`));
      bytes+=traceBytes; if(bytes>16*1024*1024) throw new ApiError(502,'Trace output exceeds the local API memory limit. Reduce search bounds.');
      if(trace.program !== `${options.scenario}/v1` || trace.limits.max_steps !== options.maxSteps || trace.limits.timeout_ms !== options.timeoutMs) throw new ApiError(502,'Recorded trace does not match the requested engine configuration.');
      traces.push(trace);
    }
    const failures=traces.filter(t=>isFailure(t.outcome.status)).length;
    const incomplete=!summary.frontierExhausted || traces.some(t=>!['completed','assertion_failed','deadlock'].includes(t.outcome.status));
    if(failures !== summary.failures || incomplete !== summary.incomplete ||
       traces.some(t=>t.outcome.status==='step_limit') !== summary.stepBoundReached ||
       result.exitCode !== (summary.incomplete ? 2 : failures ? 1 : 0)) throw new ApiError(502,'Engine summary and recorded outcomes disagree.');
    return {scenario:options.scenario,options,summary,traces,output:result.output,exitCode:result.exitCode};
  }
  async patchVerify(input: unknown,signal?: AbortSignal): Promise<PatchVerification> {
    const {scenario,...settings}=requestValue(()=>validatePatch(input)), pair=patchScenario(scenario);
    return this.job(async directory=>{
      // Hold one guard across both independent fresh searches. No trace is
      // passed to the other implementation; only the bounds are shared.
      const broken=await this.runExploration({...settings,scenario:pair.broken},join(directory,'broken'),signal);
      const corrected=await this.runExploration({...settings,scenario:pair.corrected},join(directory,'corrected'),signal);
      const result=parsePatchVerification({scenario,settings,broken,corrected});
      if(Buffer.byteLength(JSON.stringify(result))>16*1024*1024) throw new ApiError(502,'Combined comparison output exceeds 16 MiB. Reduce search bounds.');
      return result;
    });
  }
  async replay(input: unknown,signal?: AbortSignal): Promise<ReplayResult> {
    const body=requestValue(()=>record(input,'request')); if(Object.keys(body).some(k=>k !== 'trace')) throw new ApiError(400,'Replay accepts a trace object, never a file or executable path.');
    const trace=requestValue(()=>this.stableTrace(body.trace));
    return this.job(directory=>this.verify(trace,join(directory,'replay.json'),signal));
  }
  private stableTrace(input: unknown): Trace {
    const trace=parseTrace(input);
    if(!scenarioNames.some(name=>trace.program === `${name}/v1`)) throw new ApiError(400,'Trace program is not a registered scenario.');
    if(!canReplay(trace)) throw new ApiError(400,'Environmental-error and timeout traces cannot be replay verified.');
    if(trace.limits.max_steps>1000 || trace.limits.timeout_ms>5000) throw new ApiError(400,'Replay requires trace limits of at most 1000 steps and 5000 ms. Use the CLI for larger traces.');
    return trace;
  }
  private async verify(trace: Trace,path: string,signal?: AbortSignal): Promise<ReplayResult> {
      await writeFile(path,JSON.stringify(trace));
      const result=await invokeEngine(['replay',path],signal);
      const verified=result.output.split('\n').includes('Replay verified: every event and final outcome match.');
      if(verified && result.exitCode !== (trace.outcome.status==='completed' ? 0 : trace.outcome.status==='step_limit' ? 2 : 1)) throw new ApiError(502,'Replay exit code disagrees with its recorded outcome.');
      return {...result,verified,status:verified ? 'verified' : result.output.includes('replay mismatch:') ? 'diverged' : 'unverified'};
  }
  async branch(input: unknown,signal?: AbortSignal): Promise<BranchResult> {
    const body=requestValue(()=>record(input,'request'));
    if(Object.keys(body).some(k=>!['trace','step','thread'].includes(k))) throw new ApiError(400,'Branch accepts only trace, step and thread.');
    const original=requestValue(()=>this.stableTrace(body.trace));
    const step=requestValue(()=>integer(body.step,'step',0,original.events.length-1));
    const thread=requestValue(()=>integer(body.thread,'thread',0,63));
    // Do not use the client's runnable set to decide the result. The C++
    // driver verifies the original and gates the fresh branch itself.
    return this.job(async directory=>{
      const inputPath=join(directory,'original.json'), outputPath=join(directory,'modified.json');
      await writeFile(inputPath,JSON.stringify(original));
      const result=await invokeEngine(['branch',inputPath,'--step',String(step),'--thread',String(thread),'--trace',outputPath],signal);
      if(!result.output.split('\n').includes('Original replay verified; running a fresh branch.'))
        throw new ApiError(409,`Original trace could not be verified; no branch was run.\n${result.output}`);
      const {trace}=await recordedTrace(outputPath);
      if(trace.program!==original.program || JSON.stringify(trace.limits)!==JSON.stringify(original.limits) ||
         result.exitCode!==(trace.outcome.status==='completed' ? 0 : isFailure(trace.outcome.status) ? 1 : 2))
        throw new ApiError(502,'Branch trace disagrees with the requested configuration or engine exit code.');
      const prefix=[...original.schedule.slice(0,step),thread];
      const invalid=trace.outcome.status==='replay_mismatch' && trace.events.length===step &&
        trace.outcome.message.startsWith(`selected thread T${thread} is not runnable at step ${step};`);
      const replay=canReplay(trace) ? await this.verify(trace,join(directory,'modified-replay.json'),signal) : null;
      return {trace,step,thread,prefix,policy:'lowest_runnable_id',originalVerified:true,
        status:invalid ? 'invalid_prefix' : replay?.verified ? 'executed' : 'unverified',
        replay,output:result.output,exitCode:result.exitCode};
    });
  }
}
