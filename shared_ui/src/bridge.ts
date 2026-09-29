import { CONTRACT_VERSION, compareRevision, parseMessage, type Command, type Hello, type HostMessage, type Result, type Snapshot } from './contract';

declare global {
  interface Window {
    chrome?: { webview?: { postMessage: (message: string) => void; addEventListener: (name: string, callback: (event: MessageEvent) => void) => void } };
    webkit?: { messageHandlers?: { thunderlink?: { postMessage: (message: string) => void } } };
    thunderlinkReceive?: (message: string) => void;
    __THUNDERLINK_FIXTURE__?: boolean;
    __THUNDERLINK_BUILD_ID__?: string;
  }
}

export class Bridge extends EventTarget {
  hello: Hello | null = null;
  snapshot: Snapshot | null = null;
  error = '';
  private pending = new Map<string, (result: Result) => void>();
  private seen = new Set<string>();
  private transport: ((message: string) => void) | null = null;

  constructor(private buildId: string) {
    super();
    if (window.chrome?.webview) {
      this.transport = message => window.chrome!.webview!.postMessage(message);
      window.chrome.webview.addEventListener('message', event => this.receive(typeof event.data === 'string' ? event.data : JSON.stringify(event.data)));
    } else if (window.webkit?.messageHandlers?.thunderlink) {
      this.transport = message => window.webkit!.messageHandlers!.thunderlink!.postMessage(message);
    } else if (window.__THUNDERLINK_FIXTURE__ && window.parent !== window) {
      this.transport = message => window.parent.postMessage({ thunderlink: message }, location.origin);
      window.addEventListener('message', event => {
        if (event.origin === location.origin && event.source === window.parent && typeof event.data?.thunderlink === 'string') this.receive(event.data.thunderlink);
      });
    }
    window.thunderlinkReceive = raw => this.receive(raw);
  }

  start(): void {
    if (!this.transport) { this.error = 'Native UI bridge unavailable. Open ThunderLink to use these controls.'; this.changed(); return; }
    this.transport(JSON.stringify({ type: 'ready', version: CONTRACT_VERSION, buildId: this.buildId }));
  }

  receive(raw: string): void {
    const value = parseMessage(raw, 1024 * 1024) as HostMessage | null;
    if (!value || !['hello', 'state', 'result'].includes(value.type)) return;
    if (value.type === 'hello') {
      if (value.buildId !== this.buildId) { this.error = 'UI assets and native host have different versions. Retry or update ThunderLink.'; this.changed(); return; }
      this.cancelPending('UI was reloaded. Review the current state before retrying.');
      this.hello = value;
      this.snapshot = value.state;
      this.error = '';
      this.changed();
      return;
    }
    if (!this.hello || value.instanceId !== this.hello.instanceId) return;
    if (value.type === 'state') {
      if (this.snapshot && compareRevision(value.state.revision, this.snapshot.revision) <= 0) return;
      this.snapshot = value.state;
    } else {
      const pending = this.pending.get(value.requestId);
      if (pending && value.status !== 'pending') { this.pending.delete(value.requestId); pending(value); }
      this.dispatchEvent(new CustomEvent('result', { detail: value }));
    }
    this.changed();
  }

  command(action: string, payload: Record<string, unknown> = {}): Promise<Result> {
    if (!this.hello || !this.transport) return Promise.resolve(this.failure('bridge_unavailable', 'Native controls are unavailable.'));
    if (!this.hello.capabilities.includes(action)) return Promise.resolve(this.failure('not_supported', 'This action is unavailable on this device.'));
    if (this.pending.size >= 32) return Promise.resolve(this.failure('busy', 'Too many actions are pending.'));
    const requestId = crypto.randomUUID();
    if (this.seen.has(requestId)) return Promise.resolve(this.failure('duplicate', 'Duplicate action.'));
    this.seen.add(requestId);
    if (this.seen.size > 256) this.seen.delete(this.seen.values().next().value!);
    const message: Command = { type: 'command', version: CONTRACT_VERSION, instanceId: this.hello.instanceId, requestId, action, payload };
    const raw = JSON.stringify(message);
    if (new TextEncoder().encode(raw).byteLength > 64 * 1024) return Promise.resolve(this.failure('too_large', 'Action payload is too large.'));
    return new Promise(resolve => { this.pending.set(requestId, resolve); this.transport!(raw); });
  }

  has(action: string): boolean { return this.hello?.capabilities.includes(action) === true; }
  private changed(): void { this.dispatchEvent(new Event('change')); }
  private failure(code: string, message: string): Result { return { type: 'result', version: 1, instanceId: this.hello?.instanceId ?? '', requestId: '', status: 'error', code, message }; }
  private cancelPending(message: string): void {
    for (const resolve of this.pending.values()) resolve(this.failure('instance_changed', message));
    this.pending.clear();
  }
}
