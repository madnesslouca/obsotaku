#!/usr/bin/env node
/* Explicit, local-only integration test. Creates a demo scene and records it. */
const fs = require('node:fs');
const path = require('node:path');
const { connectStudio, defaultConfig } = require('./studio-api.cjs');
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));

(async () => {
  const client = await connectStudio(defaultConfig);
  const request = client.request.bind(client);
  let recording = false;
  const report = { testedAt: new Date().toISOString(), localOnly: true, screenCaptureTested: false };
  try {
    if ((await request('GetRecordStatus')).outputActive || (await request('GetStreamStatus')).outputActive)
      throw new Error('An output is already active; test cancelled');
    const sceneName = 'Studio Linux — teste local';
    const scenes = await request('GetSceneList');
    if (!scenes.scenes.some(scene => scene.sceneName === sceneName)) await request('CreateScene', { sceneName });
    const inputs = (await request('GetInputList')).inputs;
    async function input(name, kind, settings, transform) {
      let sceneItemId;
      if (inputs.some(item => item.inputName === name)) {
        await request('SetInputSettings', { inputName: name, inputSettings: settings, overlay: true });
        const items = (await request('GetSceneItemList', { sceneName })).sceneItems;
        const found = items.find(item => item.sourceName === name);
        if (found) sceneItemId = found.sceneItemId;
        else ({ sceneItemId } = await request('CreateSceneItem', { sceneName, sourceName: name }));
      } else ({ sceneItemId } = await request('CreateInput', { sceneName, inputName: name, inputKind: kind, inputSettings: settings, sceneItemEnabled: true }));
      if (transform) await request('SetSceneItemTransform', { sceneName, sceneItemId, sceneItemTransform: transform });
      return sceneItemId;
    }
    await input('Studio — fundo', 'color_source_v3', { color: 0xff211d1d, width: 1920, height: 1080 });
    await input('Studio — título', 'text_ft2_source_v2', {
      text: 'Studio Linux', font: { face: 'DejaVu Sans', size: 64, style: 'Bold' }, color1: 0xffffffff, color2: 0xffffffff
    }, { positionX: 80, positionY: 80 });
    await input('Studio — descrição', 'text_ft2_source_v2', {
      text: 'Interface própria.\nMotor OBS nativo.\n\nCenas, camadas, áudio\ne gravação local.',
      font: { face: 'DejaVu Sans', size: 30 }, color1: 0xffded9d9, color2: 0xffded9d9
    }, { positionX: 80, positionY: 280 });
    if (process.argv.includes('--with-camera')) {
      await input('Studio — webcam C922', 'v4l2_input', {
        device_id: '/dev/video0', pixelformat: 1196444237, resolution: 5497558139600,
        framerate: 4294967356, input: 0, buffering: true
      }, { positionX: 840, positionY: 260, boundsType: 'OBS_BOUNDS_SCALE_INNER', boundsWidth: 1000, boundsHeight: 562.5 });
      report.cameraModeRequested = 'MJPEG 1280x720 60/1';
    }
    if (process.argv.includes('--with-audio')) {
      await input('Studio — microfone', 'pulse_input_capture', { device_id: 'default' });
      await request('SetInputMute', { inputName: 'Studio — microfone', inputMuted: false });
      report.microphone = 'default native PulseAudio/PipeWire source';
    }
    await request('SetCurrentProgramScene', { sceneName });
    await delay(2000);
    report.video = await request('GetVideoSettings');
    report.before = await request('GetStats');
    await request('StartRecord');
    recording = true;
    await delay(12000);
    report.recording = await request('GetRecordStatus');
    report.result = await request('StopRecord');
    recording = false;
    report.after = await request('GetStats');
    report.sourceFrameRateVerified = false;
    report.note = 'Native canvas/encoder test. Requested camera mode is not a measurement of unique camera frames.';
    const output = path.resolve(__dirname, '../validation/native-recording-test.json');
    fs.mkdirSync(path.dirname(output), { recursive: true });
    fs.writeFileSync(output, JSON.stringify(report, null, 2) + '\n');
    console.log(JSON.stringify(report, null, 2));
  } finally {
    if (recording) await request('StopRecord').catch(() => {});
    client.close();
  }
})().catch(error => { console.error(error.message); process.exitCode = 1; });
