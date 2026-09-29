const frame = document.querySelector('iframe');
const role = new URLSearchParams(location.search).get('role') === 'mac' ? 'mac' : 'windows';
const capabilities = [
  'connection.connect','connection.disconnect','viewer.show','mode.set','settings.saveRestart','settings.reset',
  'cross.connect','cross.disconnect','cross.enable','cross.pause','cross.approvePairing','cross.declinePairing',
  'cross.selectPeer','cross.manualAddress','cross.forgetPeer','cross.shortcut','cross.interface','cross.modifiers',
  'cross.layoutApply','cross.layoutReset','cross.layoutRemoveMissing','cross.identifyLocal','cross.identifyRemote',
  'diagnostics.openLogs'
];
let instanceId = 'fixture-instance-1';
let state = {
  revision: '1',
  connection: { status: 'waiting', peer: 'Mac Studio' }, mode: 'screen',
  settings: { revision: '1', saved: { peerAddress: '169.254.1.1', bindAddress: '169.254.1.2', videoPort: 50001, inputPort: 50002, controlPort: 50003, codec: 'HEVC', resolution: '1920x1080', fps: 60, bitrateMbps: 30, vsync: false }, effective: { peerAddress: '169.254.1.1', bindAddress: '169.254.1.2', videoPort: 50001, inputPort: 50002, controlPort: 50003, codec: 'HEVC', resolution: '1920x1080', fps: 60, bitrateMbps: 30, vsync: false } },
  cross: { status: 'disconnected', peerId: 'mac-1', peerName: 'Mac Studio', discovered: [{ id: 'mac-1', name: 'Mac Studio' }], paired: [], enabled: false, route: 'Thunderbolt', destination: 'Windows', topologyRevision: '1', tiles: [
    { id: 'win-1', name: 'Windows display', owner: 'windows', x: 0, y: 0, width: 1920, height: 1080 },
    { id: 'mac-1', name: 'Mac display', owner: 'mac', x: 1920, y: 0, width: 2560, height: 1440 }
  ] },
  diagnostics: { protocolVersion: 4, endpoints: ['169.254.1.1:50001', '169.254.1.2:50003'], metrics: { 'Input route': 'Thunderbolt' }, logs: ['Waiting for Mac Studio', 'Cross is ready for pairing'] }
};
if (role === 'mac') {
  state.settings = { revision: '0', saved: { winIP: '169.254.1.2', bindAddress: '169.254.1.1', videoPort: 50001, inputPort: 50002, controlPort: 50003, captureDisplayIndex: '7' }, effective: { winIP: '169.254.1.2', bindAddress: '169.254.1.1', videoPort: 50001, inputPort: 50002, controlPort: 50003, captureDisplayIndex: '7' }, displays: [{ id: '0', label: 'Main display', available: true }, { id: '1', label: 'Studio Display', available: false }] };
}
window.fixtureState = () => state;
window.fixtureMessages = [];
window.fixtureReplies = [];
const send = value => { window.fixtureReplies.push(value.type); frame.contentWindow.postMessage({ thunderlink: JSON.stringify(value) }, location.origin); };
const bump = () => { state = { ...state, revision: String(BigInt(state.revision) + 1n) }; send({ type: 'state', version: 1, instanceId, state }); };
window.addEventListener('message', async event => {
  if (event.source !== frame.contentWindow || event.origin !== location.origin || typeof event.data?.thunderlink !== 'string') return;
  const message = JSON.parse(event.data.thunderlink);
  window.fixtureMessages.push(message.type);
  if (message.type === 'ready') {
    const manifest = await (await fetch('/manifest.json')).json();
    send({ type: 'hello', version: 1, buildId: manifest.buildId, instanceId, role, capabilities, state });
    return;
  }
  if (message.type !== 'command' || message.instanceId !== instanceId) return;
  let error = '';
  const data = {};
  switch (message.action) {
    case 'connection.connect': state = { ...state, connection: { status: 'streaming', peer: 'Mac Studio', width: 1920, height: 1080, fps: 60, codec: 'HEVC', bitrateMbps: 30 } }; break;
    case 'connection.disconnect': state = { ...state, connection: { status: 'disconnected' } }; break;
    case 'mode.set': state = { ...state, mode: message.payload.mode }; break;
    case 'settings.saveRestart':
      if (message.payload.revision !== state.settings.revision) { error = 'Saved settings changed. Refresh before saving.'; break; }
      if (+message.payload.values.videoPort < 1 || +message.payload.values.videoPort > 65535) { error = 'Enter a port from 1 to 65535.'; break; }
      state = { ...state, settings: { ...state.settings, revision: String(BigInt(state.settings.revision) + 1n), saved: { ...message.payload.values } } };
      data.settingsRevision = state.settings.revision; break;
    case 'cross.connect': state = { ...state, cross: { ...state.cross, status: 'ready', pairingCode: '482 931', pairingAttempt: 'attempt-1' } }; break;
    case 'cross.approvePairing': state = { ...state, cross: { ...state.cross, status: 'paired', pairingCode: undefined, paired: [{ id: 'mac-1', name: 'Mac Studio' }] } }; break;
    case 'cross.enable': state = { ...state, cross: { ...state.cross, enabled: true, status: 'active' } }; break;
    case 'cross.pause': state = { ...state, cross: { ...state.cross, enabled: false, status: 'paused' } }; break;
    case 'cross.layoutApply':
      if (message.payload.topologyRevision !== state.cross.topologyRevision || message.payload.peerId !== state.cross.peerId) error = 'Display topology changed.';
      else state = { ...state, cross: { ...state.cross, tiles: structuredClone(message.payload.tiles) } };
      break;
  }
  send({ type: 'result', version: 1, instanceId, requestId: message.requestId, status: error ? 'error' : 'ok', ...(error ? { code: 'invalid', message: error } : { data }) });
  if (!error) bump();
});
window.fixtureUpdate = partial => { state = { ...state, ...partial }; bump(); };
