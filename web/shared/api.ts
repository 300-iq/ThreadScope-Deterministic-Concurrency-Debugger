import type { Trace } from './trace.ts';
export interface Scenario { name: string; description: string }
export interface ExploreOptions { scenario: string; maxSchedules: number; maxSteps: number; timeoutMs: number }
export interface SearchSummary { exploredSchedules: number; failures: number; frontierExhausted: boolean; incomplete: boolean; scheduleBoundReached: boolean; stepBoundReached: boolean }
export interface Exploration { scenario: string; options: ExploreOptions; summary: SearchSummary; traces: Trace[]; output: string; exitCode: number }
export interface ReplayResult { verified: boolean; status: 'verified' | 'diverged' | 'unverified'; output: string; exitCode: number }
export interface BranchResult {
  trace: Trace; step: number; thread: number; prefix: number[];
  policy: 'lowest_runnable_id'; originalVerified: true;
  status: 'executed' | 'invalid_prefix' | 'unverified';
  replay: ReplayResult | null; output: string; exitCode: number;
}
export type PatchScenario = 'counter' | 'lock-order';
export interface PatchSettings { maxSchedules: number; maxSteps: number; timeoutMs: number }
export interface PatchOptions extends PatchSettings { scenario: PatchScenario }
export interface PatchVerification {
  scenario: PatchScenario; settings: PatchSettings;
  broken: Exploration; corrected: Exploration;
}
