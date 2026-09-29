import { describe, expect, it } from 'vitest';
import { sharedEdges, snapPosition, validateLayout } from '../src/layout';
import type { Tile } from '../src/contract';
const win = (id: string, x: number, y: number, width: number, height: number): Tile => ({ id, name: id, owner: 'windows', x, y, width, height });
const mac = (id: string, x: number, y: number, width: number, height: number): Tile => ({ id, name: id, owner: 'mac', x, y, width, height });

describe('Cross arrangement preview matches native edge rules', () => {
  it('accepts shared edges at negative origins and different scales', () => {
    const tiles = [win('w1', -1920, 0, 1920, 1080), win('w2', 0, 0, 2560, 1440), mac('m', 2560, 180, 960, 540)];
    expect(validateLayout(tiles)).toBe('');
    expect(sharedEdges(tiles).some(e => e.x1 === 2560 && e.y1 === 180 && e.y2 === 720)).toBe(true);
  });
  it('rejects corners, gaps, overlaps, islands, and missing tiles', () => {
    const base = win('w', 0, 0, 100, 100);
    expect(validateLayout([base, mac('m', 100, 100, 100, 100)])).toContain('touching-edge');
    expect(validateLayout([base, mac('m', 101, 0, 100, 100)])).toContain('touching-edge');
    expect(validateLayout([base, mac('m', 99, 0, 100, 100)])).toContain('overlap');
    expect(validateLayout([base, mac('m', 100, 0, 100, 100), mac('island', 400, 0, 100, 100)])).toContain('touching-edge');
    expect(validateLayout([base, { ...mac('m', 100, 0, 100, 100), missing: true }])).toContain('missing');
  });
  it('snaps the closest touching edge', () => {
    const position = snapPosition(mac('m', 105, 0, 100, 100), [win('w', 0, 0, 100, 100)]);
    expect(position).toEqual({ x: 100, y: 0 });
  });
});
