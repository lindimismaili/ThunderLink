import { LitElement, html, svg, nothing, type TemplateResult } from 'lit';
import { live } from 'lit/directives/live.js';
import { repeat } from 'lit/directives/repeat.js';
import { Bridge } from './bridge';
import { LayoutDraft, SettingsDraft } from './draft';
import { sharedEdges, snapPosition, validateLayout } from './layout';
import type { Result, Snapshot, Tile } from './contract';

const labels: Record<string, string> = {
  winIP: 'Windows address', macIP: 'Mac address', bindAddress: 'Local bind address', peerAddress: 'Peer address',
  videoPort: 'Video port', inputPort: 'Input port', controlPort: 'Control port',
  captureDisplayIndex: 'Capture display', codec: 'Codec', resolution: 'Resolution (WxH)', fps: 'Frame rate',
  bitrateMbps: 'Bitrate (Mbps)', vsync: 'VSync', keepAwake: 'Keep awake on AC',
};

export class ThunderlinkApp extends LitElement {
  static properties = { page: { state: true }, notice: { state: true }, busy: { state: true }, selectedTile: { state: true }, zoom: { state: true }, panX: { state: true }, panY: { state: true } };
  page: 'connection' | 'settings' | 'diagnostics' = 'connection';
  notice = '';
  busy = false;
  selectedTile = '';
  zoom = 0.14;
  panX = 0;
  panY = 0;
  bridge!: Bridge;
  settingsDraft = new SettingsDraft();
  layoutDraft = new LayoutDraft();
  private manualAddress = '';
  private shortcut = '';
  private pointerDrag: { id: number; tileId?: string; startX: number; startY: number; originX: number; originY: number } | null = null;
  private resultHandler = (event: Event) => {
    const result = (event as CustomEvent<Result>).detail;
    if (result.status === 'error') { this.notice = result.message ?? 'Action failed.'; if (result.fieldErrors) this.settingsDraft.fieldErrors = result.fieldErrors; }
    else if (result.status === 'pending') this.notice = 'Working…';
    else this.notice = result.message ?? 'Done.';
    this.requestUpdate();
  };
  private changeHandler = () => {
    if (this.bridge.snapshot) { this.settingsDraft.accept(this.bridge.snapshot); this.layoutDraft.accept(this.bridge.snapshot); }
    this.requestUpdate();
  };

  createRenderRoot(): HTMLElement { return this; }
  setBridge(bridge: Bridge): void {
    this.bridge?.removeEventListener('change', this.changeHandler);
    this.bridge?.removeEventListener('result', this.resultHandler);
    this.bridge = bridge;
    if (this.isConnected) { bridge.addEventListener('change', this.changeHandler); bridge.addEventListener('result', this.resultHandler); }
    this.changeHandler();
  }
  connectedCallback(): void { super.connectedCallback(); this.bridge?.addEventListener('change', this.changeHandler); this.bridge?.addEventListener('result', this.resultHandler); if (this.bridge?.snapshot) this.changeHandler(); }
  disconnectedCallback(): void { this.bridge?.removeEventListener('change', this.changeHandler); this.bridge?.removeEventListener('result', this.resultHandler); super.disconnectedCallback(); }

  private async act(action: string, payload: Record<string, unknown> = {}): Promise<Result> {
    this.busy = true;
    try { return await this.bridge.command(action, payload); }
    finally { this.busy = false; }
  }
  private action(label: string, name: string, payload: Record<string, unknown> = {}, kind = ''): TemplateResult | typeof nothing {
    return this.bridge.has(name) ? html`<button class=${kind} ?disabled=${this.busy} @click=${() => this.act(name, payload)}>${label}</button>` : nothing;
  }
  private field(key: string, type = 'text'): TemplateResult {
    const value = this.settingsDraft.values[key];
    const error = this.settingsDraft.fieldErrors[key];
    const effective = this.bridge.snapshot?.settings.effective[key];
    const displays = key === 'captureDisplayIndex' ? this.bridge.snapshot?.settings.displays : undefined;
    const savedDisplayAvailable = displays?.some(display => display.id === String(value));
    return html`<label class="field"><span>${labels[key] ?? key}</span>
      ${displays
        ? html`<select .value=${live(String(value ?? ''))} aria-invalid=${error ? 'true' : 'false'} @change=${(event: Event) => { this.settingsDraft.edit(key, (event.target as HTMLSelectElement).value); this.requestUpdate(); }}>
            ${!savedDisplayAvailable ? html`<option value=${String(value ?? '')}>Saved display unavailable</option>` : nothing}
            ${displays.map(display => html`<option value=${display.id} ?disabled=${!display.available}>${display.label}${display.available ? '' : ' (unavailable)'}</option>`)}
          </select>`
        : type === 'checkbox'
        ? html`<input type="checkbox" .checked=${live(Boolean(value))} @change=${(event: Event) => { this.settingsDraft.edit(key, (event.target as HTMLInputElement).checked); this.requestUpdate(); }}>`
        : html`<input type=${type} .value=${live(String(value ?? ''))} aria-invalid=${error ? 'true' : 'false'} @input=${(event: Event) => { this.settingsDraft.edit(key, (event.target as HTMLInputElement).value); this.requestUpdate(); }}>`}
      ${effective !== undefined && String(effective) !== String(value) ? html`<small>Active now: ${String(effective)}</small>` : nothing}
      ${error ? html`<small class="error" role="alert">${error}</small>` : nothing}
    </label>`;
  }
  private async save(): Promise<void> {
    if (this.settingsDraft.conflict) { this.notice = 'Saved settings changed elsewhere. Refresh and review your edits before saving.'; return; }
    const result = await this.act('settings.saveRestart', { revision: this.settingsDraft.revision, values: this.settingsDraft.values });
    if (result.status === 'ok') this.settingsDraft.saved(String(result.data?.settingsRevision ?? this.settingsDraft.revision));
  }
  private async applyLayout(): Promise<void> {
    if (this.layoutDraft.stale) { this.notice = 'Displays changed. Refresh the arrangement before applying.'; return; }
    const result = await this.act('cross.layoutApply', { peerId: this.layoutDraft.peerId, topologyRevision: this.layoutDraft.topologyRevision, tiles: this.layoutDraft.tiles });
    if (result.status === 'ok') this.layoutDraft.dirty = false;
  }
  private nav(page: 'connection' | 'settings' | 'diagnostics', title: string): TemplateResult {
    return html`<button class=${this.page === page ? 'nav active' : 'nav'} aria-current=${this.page === page ? 'page' : nothing} @click=${() => { this.page = page; }}>${title}</button>`;
  }

  render(): TemplateResult {
    const snapshot = this.bridge?.snapshot;
    const role = this.bridge?.hello?.role;
    return html`<div class="shell">
      <aside class="sidebar"><div class="brand"><span class="brand-icon">⌁</span><div><strong>ThunderLink</strong><small>REMOTE DESKTOP</small></div></div>
        <nav aria-label="Main navigation">${this.nav('connection', 'Connection')}${this.nav('settings', 'Settings')}${this.nav('diagnostics', 'Diagnostics')}</nav>
        <div class="sidebar-foot"><span class="status-dot ${snapshot?.connection.status ?? ''}"></span>${snapshot?.connection.status ?? 'Loading'}<small>${role === 'mac' ? 'Mac sender' : role === 'windows' ? 'Windows receiver' : 'Offline preview'}</small></div>
      </aside>
      <main id="main"><div class="page-heading"><div><p class="eyebrow">${role === 'mac' ? 'MAC SENDER' : role === 'windows' ? 'WINDOWS RECEIVER' : 'THUNDERLINK'}</p><h1>${this.page === 'connection' ? 'Connection' : this.page === 'settings' ? 'Settings' : 'Diagnostics'}</h1></div><span class="pill">${snapshot?.mode === 'cross' ? 'Cross' : 'Screen Share'}</span></div>
        ${this.bridge?.error ? html`<div class="alert error" role="alert">${this.bridge.error}</div>` : nothing}
        ${this.notice ? html`<div class="alert" role="status">${this.notice}<button class="plain" aria-label="Dismiss message" @click=${() => { this.notice = ''; }}>×</button></div>` : nothing}
        ${!snapshot ? html`<section class="card"><h2>Waiting for native controls</h2><p>The local dashboard will appear when ThunderLink connects this view.</p></section>` : this.page === 'connection' ? this.connection(snapshot) : this.page === 'settings' ? this.settings(snapshot) : this.diagnostics(snapshot)}
      </main>
    </div>`;
  }

  private connection(s: Snapshot): TemplateResult {
    const c = s.connection;
    return html`<div class="stack"><section class="card hero"><div class="hero-head"><div><p class="eyebrow">SESSION STATUS</p><h2><span class="status-dot ${c.status}"></span>${c.status[0].toUpperCase() + c.status.slice(1)}</h2><p>${c.error ?? (c.peer ? `Peer: ${c.peer}` : 'Waiting for a peer device')}</p></div><div class="actions">${this.action('Connect', 'connection.connect', {}, 'primary')}${this.action('Disconnect', 'connection.disconnect', {}, 'danger')}${this.action('Back to Video', 'viewer.show', {}, 'primary')}</div></div>
      ${s.mode === 'screen' && c.status === 'streaming' ? html`<div class="metric-grid"><div><small>RESOLUTION</small><strong>${c.width ?? '—'} × ${c.height ?? '—'}</strong></div><div><small>FRAME RATE</small><strong>${c.fps ?? '—'} fps</strong></div><div><small>CODEC</small><strong>${c.codec ?? '—'}</strong></div><div><small>BITRATE</small><strong>${c.bitrateMbps ?? '—'} Mbps</strong></div></div>` : nothing}</section>
      ${this.bridge.has('mode.set') ? html`<section class="card"><p class="eyebrow">SESSION MODE</p><div class="segmented" role="group" aria-label="Session mode"><button class=${s.mode === 'screen' ? 'selected' : ''} ?disabled=${this.busy} @click=${() => this.act('mode.set', { mode: 'screen' })}>Screen Share</button><button class=${s.mode === 'cross' ? 'selected' : ''} ?disabled=${this.busy} @click=${() => this.act('mode.set', { mode: 'cross' })}>Cross</button></div><p class="muted">Switching modes safely ends the current session.</p></section>` : nothing}
      ${s.mode === 'cross' ? this.cross(s) : this.permissions(s)}
      ${s.mode === 'screen' && this.bridge.hello?.role === 'windows' ? html`<section class="card"><p class="eyebrow">DIRECT LINK SETUP</p><h2>Ready to connect</h2><ol><li>Connect the dedicated network adapters and cable.</li><li>Open ThunderLink on the Mac and grant Screen Recording and Accessibility.</li><li>For the default direct link, set the PC adapter to 169.254.1.2 with subnet 255.255.255.0.</li><li>Allow ThunderLink through Windows Firewall on that network profile.</li></ol><div class="actions">${this.action('Network Settings', 'system.networkSettings')}${this.action('Firewall Settings', 'system.firewallSettings')}</div></section>` : nothing}
    </div>`;
  }

  private permissions(s: Snapshot): TemplateResult | typeof nothing {
    if (this.bridge.hello?.role !== 'mac') return nothing;
    const p = s.permissions;
    return html`<section class="card"><p class="eyebrow">MAC PERMISSIONS</p><div class="row"><div><strong>Screen Recording</strong><p>${p?.screenRecording ?? 'Unknown'}</p></div>${this.action('Open Settings', 'permissions.screenRecording')}</div><div class="row"><div><strong>Accessibility</strong><p>${p?.accessibility ?? 'Unknown'}</p></div>${this.action('Open Settings', 'permissions.accessibility')}</div><div class="row"><div><strong>Closed lid</strong><p>${p?.lid ?? 'Not authorized'}</p></div>${this.action('Authorize', 'power.authorizeLid')}</div>${this.action('Refresh permissions', 'permissions.refresh')}</section>`;
  }

  private cross(s: Snapshot): TemplateResult {
    const x = s.cross;
    return html`<section class="card"><p class="eyebrow">CROSS CONTROL</p><div class="row"><div><h2>${x.peerName ?? 'No Mac selected'}</h2><p>${x.status ?? 'Disconnected'} · ${x.route ?? 'No route'} · ${x.destination ?? 'Windows'}${x.latencyMs !== undefined ? ` · ${x.latencyMs} ms` : ''}</p></div><div class="actions">${this.action('Connect Cross', 'cross.connect', {}, 'primary')}${this.action('Disconnect', 'cross.disconnect')}${this.action(x.enabled ? 'Pause Cross' : 'Enable Cross', x.enabled ? 'cross.pause' : 'cross.enable', {}, 'primary')}</div></div>
      ${x.error ? html`<p class="error" role="alert">${x.error}</p>` : nothing}
      ${x.pairingCode ? html`<div class="pair-code"><span>Compare this code on both devices</span><strong>${x.pairingCode}</strong><div class="actions">${this.action('Approve pairing', 'cross.approvePairing', { peerId: x.peerId, attempt: x.pairingAttempt }, 'primary')}${this.action('Decline', 'cross.declinePairing', { peerId: x.peerId, attempt: x.pairingAttempt })}</div></div>` : nothing}
      <div class="divider"></div><h3>Devices</h3><div class="peer-list">${repeat(x.discovered ?? [], peer => peer.id, peer => html`<div class="row"><span>${peer.name}</span>${this.action('Select', 'cross.selectPeer', { peerId: peer.id })}</div>`)}${!x.discovered?.length ? html`<p class="muted">No nearby devices found.</p>` : nothing}</div>
      <div class="row"><label class="field grow">Manual address<input .value=${live(this.manualAddress)} @input=${(e: Event) => { this.manualAddress = (e.target as HTMLInputElement).value; this.requestUpdate(); }}></label>${this.action('Use address', 'cross.manualAddress', { address: this.manualAddress })}</div>
      ${repeat(x.paired ?? [], peer => peer.id, peer => html`<div class="row"><span>Paired: ${peer.name}</span>${this.action('Forget device', 'cross.forgetPeer', { peerId: peer.id }, 'danger')}</div>`)}
      ${this.bridge.hello?.role === 'windows' ? html`<div class="divider"></div><div class="row"><label class="field grow">Emergency return shortcut<input .value=${live(this.shortcut)} @input=${(e: Event) => { this.shortcut = (e.target as HTMLInputElement).value; this.requestUpdate(); }}></label>${this.action('Apply shortcut', 'cross.shortcut', { shortcut: this.shortcut })}</div><div class="row">${this.action('Automatic route', 'cross.interface', { interfaceId: '' })}${this.action('Windows to Mac modifiers', 'cross.modifiers', { mode: 'windows' })}${this.action('Native Mac modifiers', 'cross.modifiers', { mode: 'native' })}</div>${this.arrangement(s)}` : nothing}
    </section>`;
  }

  private arrangement(s: Snapshot): TemplateResult {
    const tiles = this.layoutDraft.tiles;
    const layoutError = validateLayout(tiles);
    const selected = tiles.find(t => t.id === this.selectedTile && t.owner === 'mac');
    const minX = Math.min(0, ...tiles.map(t => t.x));
    const minY = Math.min(0, ...tiles.map(t => t.y));
    const canvasWidth = Math.max(600, ...tiles.map(t => t.x + t.width - minX)) * this.zoom + 80;
    const canvasHeight = Math.max(350, ...tiles.map(t => t.y + t.height - minY)) * this.zoom + 80;
    return html`<div class="divider"></div><div class="row"><div><h3>Arrange displays</h3><p>Move Mac displays next to a Windows edge. Apply to save.</p></div><div class="actions">${this.action('Identify Windows', 'cross.identifyLocal')}${this.action('Identify Mac', 'cross.identifyRemote')}</div></div>
      ${this.layoutDraft.stale ? html`<div class="alert error" role="alert">The peer or displays changed. Refresh and review this arrangement.<button @click=${() => { this.layoutDraft.refresh(s); this.requestUpdate(); }}>Refresh</button></div>` : nothing}
      <div class="canvas-toolbar"><button @click=${() => { this.zoom = Math.min(0.5, this.zoom * 1.25); }}>Zoom in</button><button @click=${() => { this.zoom = Math.max(0.04, this.zoom / 1.25); }}>Zoom out</button><button @click=${() => { this.zoom = 0.14; this.panX = this.panY = 0; }}>Reset view</button><span>${Math.round(this.zoom * 100)}%</span></div>
      <div class="canvas-scroll"><svg class="layout-canvas" width=${canvasWidth} height=${canvasHeight} role="img" aria-label="Display arrangement preview" @pointerdown=${this.startPan} @pointermove=${this.dragPointer} @pointerup=${this.endPointer} @pointercancel=${this.endPointer}><g transform="translate(${40 + this.panX},${40 + this.panY}) scale(${this.zoom}) translate(${-minX},${-minY})">${repeat(tiles, tile => tile.id, tile => this.tile(tile))}${sharedEdges(tiles).map(edge => svg`<line class="shared-edge" x1=${edge.x1} y1=${edge.y1} x2=${edge.x2} y2=${edge.y2}></line>`)}</g></svg></div>
      <div class="actions display-list" role="group" aria-label="Mac display selection">${tiles.filter(tile => tile.owner === 'mac').map(tile => html`<button aria-pressed=${this.selectedTile === tile.id} @click=${() => { this.selectedTile = tile.id; }}>${tile.name}${tile.missing ? ' (missing)' : ''}</button>`)}</div>
      <div class="tile-controls">${selected ? html`<h3>${selected.name}</h3><div class="actions"><button @click=${() => this.moveSelected(-20, 0)}>Left</button><button @click=${() => this.moveSelected(20, 0)}>Right</button><button @click=${() => this.moveSelected(0, -20)}>Up</button><button @click=${() => this.moveSelected(0, 20)}>Down</button><button @click=${() => this.scaleSelected(0.9)}>Smaller</button><button @click=${() => this.scaleSelected(1.1)}>Larger</button><button @click=${() => this.snapSelected()}>Snap</button></div><div class="form-grid"><label class="field">X<input type="number" .value=${live(String(selected.x))} @change=${(e: Event) => { this.layoutDraft.move(selected.id, Number((e.target as HTMLInputElement).value), selected.y); this.requestUpdate(); }}></label><label class="field">Y<input type="number" .value=${live(String(selected.y))} @change=${(e: Event) => { this.layoutDraft.move(selected.id, selected.x, Number((e.target as HTMLInputElement).value)); this.requestUpdate(); }}></label></div>` : html`<p class="muted">Select a Mac tile for keyboard and position controls.</p>`}</div>
      ${layoutError ? html`<p class="error" role="status">${layoutError}</p>` : nothing}<div class="actions">${this.action('Reset Mac tiles', 'cross.layoutReset', { peerId: this.layoutDraft.peerId, topologyRevision: this.layoutDraft.topologyRevision })}${this.action('Remove missing', 'cross.layoutRemoveMissing', { peerId: this.layoutDraft.peerId, topologyRevision: this.layoutDraft.topologyRevision })}<button class="primary" ?disabled=${this.busy || this.layoutDraft.stale || !!layoutError || !this.layoutDraft.dirty || !this.bridge.has('cross.layoutApply')} @click=${this.applyLayout}>Apply arrangement</button></div>`;
  }
  private tile(tile: Tile): TemplateResult {
    return svg`<g class=${tile.owner === 'mac' ? 'tile mac' : 'tile windows'}><rect x=${tile.x} y=${tile.y} width=${tile.width} height=${tile.height} rx="18" tabindex=${tile.owner === 'mac' ? '0' : '-1'} aria-label=${`${tile.name}, ${tile.owner}, X ${tile.x}, Y ${tile.y}`} @pointerdown=${(e: PointerEvent) => this.startTileDrag(e, tile)} @click=${() => { this.selectedTile = tile.id; }} @keydown=${(e: KeyboardEvent) => { if (tile.owner !== 'mac') return; this.selectedTile = tile.id; const step = e.shiftKey ? 100 : 20; const delta = e.key === 'ArrowLeft' ? [-step, 0] : e.key === 'ArrowRight' ? [step, 0] : e.key === 'ArrowUp' ? [0, -step] : e.key === 'ArrowDown' ? [0, step] : null; if (delta) { e.preventDefault(); this.moveSelected(delta[0], delta[1]); } }}></rect><text x=${tile.x + 26} y=${tile.y + 50}>${tile.name}${tile.missing ? ' (missing)' : ''}</text></g>`;
  }
  private startTileDrag(e: PointerEvent, tile: Tile): void {
    this.selectedTile = tile.id;
    if (tile.owner !== 'mac') return;
    e.stopPropagation();
    this.pointerDrag = { id: e.pointerId, tileId: tile.id, startX: e.clientX, startY: e.clientY, originX: tile.x, originY: tile.y };
    (e.currentTarget as SVGElement).closest('svg')?.setPointerCapture(e.pointerId);
  }
  private startPan(e: PointerEvent): void {
    if (e.button !== 0 || this.pointerDrag) return;
    this.pointerDrag = { id: e.pointerId, startX: e.clientX, startY: e.clientY, originX: this.panX, originY: this.panY };
    (e.currentTarget as SVGSVGElement).setPointerCapture(e.pointerId);
  }
  private dragPointer(e: PointerEvent): void {
    const drag = this.pointerDrag;
    if (!drag || drag.id !== e.pointerId) return;
    const dx = e.clientX - drag.startX, dy = e.clientY - drag.startY;
    if (drag.tileId) this.layoutDraft.move(drag.tileId, Math.round(drag.originX + dx / this.zoom), Math.round(drag.originY + dy / this.zoom));
    else { this.panX = drag.originX + dx; this.panY = drag.originY + dy; }
    this.requestUpdate();
  }
  private endPointer(e: PointerEvent): void { if (this.pointerDrag?.id === e.pointerId) this.pointerDrag = null; }
  private moveSelected(dx: number, dy: number): void { const t = this.layoutDraft.tiles.find(t => t.id === this.selectedTile); if (t) { this.layoutDraft.move(t.id, t.x + dx, t.y + dy); this.requestUpdate(); } }
  private scaleSelected(factor: number): void { const t = this.layoutDraft.tiles.find(t => t.id === this.selectedTile && t.owner === 'mac'); if (t) { t.width = Math.round(t.width * factor); t.height = Math.round(t.height * factor); this.layoutDraft.dirty = true; this.requestUpdate(); } }
  private snapSelected(): void { const t = this.layoutDraft.tiles.find(t => t.id === this.selectedTile && t.owner === 'mac'); if (t) { const position = snapPosition(t, this.layoutDraft.tiles); this.layoutDraft.move(t.id, position.x, position.y); this.requestUpdate(); } }

  private settings(s: Snapshot): TemplateResult {
    const keys = Object.keys(this.settingsDraft.values);
    return html`<div class="stack"><section class="card"><div class="row"><div><p class="eyebrow">SAVED CONFIGURATION</p><h2>Connection settings</h2><p>Changes to connection settings take effect after restart.</p></div><span class="pill">Revision ${this.settingsDraft.revision}</span></div>
      ${this.settingsDraft.conflict ? html`<div class="alert error" role="alert">Saved settings changed elsewhere. Refresh to review the new values.<button @click=${() => { this.settingsDraft.refresh(s); this.requestUpdate(); }}>Refresh</button></div>` : nothing}
      <div class="form-grid">${keys.length ? keys.map(key => this.field(key, typeof this.settingsDraft.values[key] === 'boolean' ? 'checkbox' : 'text')) : html`<p>No editable settings provided by this device.</p>`}</div>
      <div class="actions footer-actions"><button class="primary" ?disabled=${this.busy || this.settingsDraft.conflict || !this.bridge.has('settings.saveRestart')} @click=${this.save}>Save and Restart</button>${this.action('Reset to Defaults', 'settings.reset')}</div></section>
      ${this.bridge.hello?.role === 'mac' ? html`<section class="card"><p class="eyebrow">POWER</p><div class="row"><div><h3>Keep awake on AC</h3><p>${s.permissions?.keepAwake ? 'Enabled' : 'Disabled'} for active sessions</p></div>${this.action(s.permissions?.keepAwake ? 'Turn off' : 'Turn on', 'power.keepAwake', { enabled: !s.permissions?.keepAwake })}</div><p class="muted">Closed-lid protection requires separate authorization and also blocks manual Sleep.</p>${this.action('Authorize closed-lid control', 'power.authorizeLid')}</section>` : nothing}
    </div>`;
  }

  private diagnostics(s: Snapshot): TemplateResult {
    const d = s.diagnostics;
    return html`<div class="stack"><section class="card"><p class="eyebrow">LOCAL DIAGNOSTICS</p><h2>System status</h2><dl class="details"><div><dt>Protocol</dt><dd>v${d.protocolVersion ?? '—'}</dd></div><div><dt>UI contract</dt><dd>v${this.bridge.hello?.version ?? '—'}</dd></div><div><dt>UI build</dt><dd class="mono">${this.bridge.hello?.buildId ?? '—'}</dd></div>${(d.endpoints ?? []).map(value => html`<div><dt>Endpoint</dt><dd>${value}</dd></div>`)}${Object.entries(d.metrics ?? {}).map(([key, value]) => html`<div><dt>${key}</dt><dd>${value}</dd></div>`)}</dl><div class="actions">${this.action('Open Logs', 'diagnostics.openLogs')}${this.action('Refresh permissions', 'permissions.refresh')}</div></section>
      ${d.logs ? html`<section class="card"><div class="row"><div><p class="eyebrow">RECENT EVENTS</p><h2>Logs</h2></div><button @click=${() => navigator.clipboard.writeText(d.logs!.join('\n'))}>Copy</button></div><pre class="log">${d.logs.join('\n')}</pre>${d.truncated ? html`<p class="muted">Older log lines were omitted.</p>` : nothing}</section>` : nothing}</div>`;
  }
}

customElements.define('thunderlink-app', ThunderlinkApp);
