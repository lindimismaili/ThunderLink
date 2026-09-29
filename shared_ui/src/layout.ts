import type { Tile } from './contract';

export const EDGE_EPSILON = 0.01;
const near = (a: number, b: number): boolean => Math.abs(a - b) <= EDGE_EPSILON;
const overlap = (a0: number, a1: number, b0: number, b1: number): number => Math.min(a1, b1) - Math.max(a0, b0);

export function sharesEdge(a: Tile, b: Tile): boolean {
  const overlapY = overlap(a.y, a.y + a.height, b.y, b.y + b.height);
  const overlapX = overlap(a.x, a.x + a.width, b.x, b.x + b.width);
  return ((near(a.x + a.width, b.x) || near(b.x + b.width, a.x)) && overlapY > EDGE_EPSILON)
    || ((near(a.y + a.height, b.y) || near(b.y + b.height, a.y)) && overlapX > EDGE_EPSILON);
}

export interface EdgeSegment { x1: number; y1: number; x2: number; y2: number }
export function sharedEdges(tiles: Tile[]): EdgeSegment[] {
  const result: EdgeSegment[] = [];
  for (let i = 0; i < tiles.length; i++) for (let j = i + 1; j < tiles.length; j++) {
    const a = tiles[i], b = tiles[j];
    if (!sharesEdge(a, b)) continue;
    const y0 = Math.max(a.y, b.y), y1 = Math.min(a.y + a.height, b.y + b.height);
    const x0 = Math.max(a.x, b.x), x1 = Math.min(a.x + a.width, b.x + b.width);
    if (y1 - y0 > EDGE_EPSILON) {
      const x = near(a.x + a.width, b.x) ? b.x : a.x;
      result.push({ x1: x, y1: y0, x2: x, y2: y1 });
    } else if (x1 - x0 > EDGE_EPSILON) {
      const y = near(a.y + a.height, b.y) ? b.y : a.y;
      result.push({ x1: x0, y1: y, x2: x1, y2: y });
    }
  }
  return result;
}

export function validateLayout(tiles: Tile[], requireRemote = true): string {
  if (tiles.length < 1 || tiles.length > 64) return 'Arrange between 1 and 64 displays.';
  const ids = new Set<string>();
  let local = false, remote = false;
  for (const t of tiles) {
    if (!t.id || t.id.length > 256 || ids.has(t.id)) return 'Display identities are missing or ambiguous. Identify the displays again.';
    ids.add(t.id);
    if (![t.x, t.y, t.width, t.height].every(Number.isFinite) || t.width < 1 || t.height < 1 || t.width > 1e6 || t.height > 1e6 || Math.abs(t.x) > 1e7 || Math.abs(t.y) > 1e7) return 'Display positions and sizes must be finite and positive.';
    local ||= t.owner === 'windows'; remote ||= t.owner === 'mac';
    if (t.missing) return 'Remove missing Mac displays or reconnect them before applying.';
  }
  if (!local || (requireRemote && !remote)) return 'Connect at least one Windows display and one Mac display.';
  for (let i = 0; i < tiles.length; i++) for (let j = i + 1; j < tiles.length; j++) {
    const a = tiles[i], b = tiles[j];
    if (overlap(a.x, a.x + a.width, b.x, b.x + b.width) > EDGE_EPSILON && overlap(a.y, a.y + a.height, b.y, b.y + b.height) > EDGE_EPSILON) return 'Display tiles must not overlap.';
  }
  const reachable = new Set(tiles.filter(t => t.owner === 'windows').map(t => t.id));
  for (let pass = 0; pass < tiles.length; pass++) {
    let changed = false;
    for (const a of tiles) if (reachable.has(a.id)) for (const b of tiles) if (!reachable.has(b.id) && sharesEdge(a, b)) { reachable.add(b.id); changed = true; }
    if (!changed) break;
  }
  if (reachable.size !== tiles.length) return 'Every Mac display needs a touching-edge route back to Windows.';
  return '';
}

export function snapPosition(tile: Tile, others: Tile[]): { x: number; y: number } {
  const candidates: { x: number; y: number; distance: number }[] = [];
  for (const anchor of others) {
    if (anchor.id === tile.id) continue;
    for (const x of [anchor.x + anchor.width, anchor.x - tile.width]) {
      if (overlap(tile.y, tile.y + tile.height, anchor.y, anchor.y + anchor.height) > EDGE_EPSILON) candidates.push({ x, y: tile.y, distance: Math.abs(x - tile.x) });
    }
    for (const y of [anchor.y + anchor.height, anchor.y - tile.height]) {
      if (overlap(tile.x, tile.x + tile.width, anchor.x, anchor.x + anchor.width) > EDGE_EPSILON) candidates.push({ x: tile.x, y, distance: Math.abs(y - tile.y) });
    }
  }
  candidates.sort((a, b) => a.distance - b.distance);
  return candidates[0] ? { x: candidates[0].x, y: candidates[0].y } : { x: tile.x, y: tile.y };
}
