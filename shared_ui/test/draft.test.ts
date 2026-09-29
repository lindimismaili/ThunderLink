import { describe, expect, it } from 'vitest';
import { LayoutDraft, SettingsDraft } from '../src/draft';
import { compareRevision, parseMessage, type Snapshot } from '../src/contract';
import hello from '../contract/fixtures/valid-hello.json';

const baseline = (hello.state as Snapshot);
describe('local UI drafts', () => {
  it('loads the initial saved settings at revision zero', () => {
    const draft = new SettingsDraft();
    draft.accept({ ...baseline, settings: { ...baseline.settings, revision: '0' } });
    expect(draft.values.videoPort).toBe(50001);
  });
  it('keeps an edited setting when status or saved settings change', () => {
    const draft = new SettingsDraft(); draft.accept(baseline);
    draft.edit('videoPort', '50020');
    draft.accept({ ...baseline, settings: { ...baseline.settings, revision: '2', saved: { videoPort: 50003 } } });
    expect(draft.values.videoPort).toBe('50020'); expect(draft.conflict).toBe(true);
  });
  it('marks an arrangement stale after topology changes', () => {
    const draft = new LayoutDraft();
    const state = { ...baseline, cross: { peerId: 'peer', topologyRevision: '1', tiles: [{ id: 'mac', name: 'Mac', owner: 'mac' as const, x: 100, y: 0, width: 500, height: 300 }] } };
    draft.accept(state); draft.move('mac', 200, 0);
    draft.accept({ ...state, cross: { ...state.cross, topologyRevision: '2' } });
    expect(draft.tiles[0].x).toBe(200); expect(draft.stale).toBe(true);
  });
  it('compares string revisions without JavaScript number precision loss', () => {
    expect(compareRevision('9007199254740993', '9007199254740992')).toBe(1);
  });
  it('rejects malformed and oversized bridge messages', () => {
    expect(parseMessage('{', 1024)).toBeNull();
    expect(parseMessage(JSON.stringify(hello), 20)).toBeNull();
    expect(parseMessage(JSON.stringify({ ...hello, role: 'admin' }), 1024 * 1024)).toBeNull();
  });
});
