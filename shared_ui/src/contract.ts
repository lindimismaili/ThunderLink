import validate from './generated-validator.js';

export const CONTRACT_VERSION = 1 as const;
export type Role = 'mac' | 'windows';
export type Mode = 'screen' | 'cross';
export type ConnectionStatus = 'disconnected' | 'waiting' | 'preparing' | 'streaming' | 'error';
export type SettingValue = string | number | boolean;
export type Settings = Record<string, SettingValue>;
export interface Tile { id: string; name: string; owner: Role; x: number; y: number; width: number; height: number; missing?: boolean }
export interface Snapshot {
  revision: string;
  connection: { status: ConnectionStatus; peer?: string; error?: string; width?: number; height?: number; fps?: number; codec?: string; bitrateMbps?: number };
  mode: Mode;
  settings: { revision: string; saved: Settings; effective: Settings; displays?: { id: string; label: string; available: boolean }[] };
  permissions?: { screenRecording?: string; accessibility?: string; keepAwake?: boolean; lid?: string };
  cross: { status?: string; peerId?: string; peerName?: string; pairingCode?: string; pairingAttempt?: string; discovered?: { id: string; name: string }[]; paired?: { id: string; name: string }[]; enabled?: boolean; route?: string; latencyMs?: number; destination?: string; topologyRevision?: string; tiles?: Tile[]; error?: string };
  diagnostics: { protocolVersion?: number; logs?: string[]; endpoints?: string[]; metrics?: Record<string, string | number>; truncated?: boolean };
}
export interface Ready { type: 'ready'; version: 1; buildId: string }
export interface Hello { type: 'hello'; version: 1; buildId: string; instanceId: string; role: Role; capabilities: string[]; state: Snapshot }
export interface Command { type: 'command'; version: 1; instanceId: string; requestId: string; action: string; payload: Record<string, unknown> }
export interface Result { type: 'result'; version: 1; instanceId: string; requestId: string; status: 'ok' | 'pending' | 'error'; data?: Record<string, unknown>; code?: string; message?: string; fieldErrors?: Record<string, string> }
export interface StateMessage { type: 'state'; version: 1; instanceId: string; state: Snapshot }
export type HostMessage = Hello | Result | StateMessage;
export type UiMessage = Ready | Command;

export function validMessage(value: unknown): boolean { return validate(value) === true; }
export function parseMessage(raw: string, maxBytes: number): unknown | null {
  if (new TextEncoder().encode(raw).byteLength > maxBytes) return null;
  try { const value: unknown = JSON.parse(raw); return validMessage(value) ? value : null; }
  catch { return null; }
}

export function compareRevision(a: string, b: string): number {
  const left = BigInt(a), right = BigInt(b);
  return left < right ? -1 : left > right ? 1 : 0;
}
