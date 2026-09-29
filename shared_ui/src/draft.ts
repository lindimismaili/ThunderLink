import { compareRevision, type Settings, type Snapshot, type Tile } from './contract';

export class SettingsDraft {
  values: Settings = {};
  revision = '0';
  private initialized = false;
  dirty = false;
  conflict = false;
  fieldErrors: Record<string, string> = {};

  accept(snapshot: Snapshot): void {
    const next = snapshot.settings;
    if (this.initialized && compareRevision(next.revision, this.revision) <= 0) return;
    if (this.dirty) { this.conflict = true; return; }
    this.values = { ...next.saved };
    this.revision = next.revision;
    this.initialized = true;
    this.fieldErrors = {};
  }
  edit(key: string, value: string | boolean): void { this.values[key] = value; this.dirty = true; delete this.fieldErrors[key]; }
  saved(revision: string): void { this.revision = revision; this.dirty = false; this.conflict = false; this.fieldErrors = {}; }
  refresh(snapshot: Snapshot): void { this.dirty = false; this.conflict = false; this.initialized = false; this.accept(snapshot); }
}

export class LayoutDraft {
  tiles: Tile[] = [];
  peerId = '';
  topologyRevision = '';
  dirty = false;
  stale = false;
  accept(snapshot: Snapshot): void {
    const peerId = snapshot.cross.peerId ?? '';
    const revision = snapshot.cross.topologyRevision ?? '';
    if (peerId === this.peerId && revision === this.topologyRevision) return;
    if (this.dirty) { this.stale = true; return; }
    this.peerId = peerId;
    this.topologyRevision = revision;
    this.tiles = structuredClone(snapshot.cross.tiles ?? []);
  }
  move(id: string, x: number, y: number): void {
    const tile = this.tiles.find(item => item.id === id);
    if (!tile || tile.owner !== 'mac') return;
    tile.x = x; tile.y = y; this.dirty = true;
  }
  refresh(snapshot: Snapshot): void { this.dirty = false; this.stale = false; this.peerId = ''; this.topologyRevision = ''; this.accept(snapshot); }
}
