#!/usr/bin/env node
/* Uses only the disposable fixtures created by studio-scene-audio-check. */
const fs = require('node:fs');
const path = require('node:path');
const {connectStudio, defaultConfig} = require('./studio-api.cjs');
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
(async () => {
 const c = await connectStudio(defaultConfig);
 const sceneA = 'Studio Mixer QA — A', sceneB = 'Studio Mixer QA — B';
 const inputName = 'Studio Mixer QA — shared';
 const added = [], segments = [];
 let original, recording = false, result, stateA, stateB;
 const switchScene = async sceneName => { await c.request('SetCurrentProgramScene', {sceneName}); await wait(700); };
 const state = async () => ({...await c.request('GetInputVolume', {inputName}), ...await c.request('GetInputMute', {inputName})});
 const restoreState = async (sceneName, snapshot) => {
  if (!snapshot) return;
  await switchScene(sceneName);
  await c.request('SetInputVolume', {inputName, inputVolumeMul: snapshot.inputVolumeMul});
  await c.request('SetInputMute', {inputName, inputMuted: snapshot.inputMuted});
 };
 try {
  for (const request of ['GetRecordStatus', 'GetStreamStatus', 'GetReplayBufferStatus'])
   if ((await c.request(request)).outputActive) throw new Error(`Output active: ${request}`);
  original = (await c.request('GetCurrentProgramScene')).currentProgramSceneName;
  await switchScene(sceneA); stateA = await state();
  await switchScene(sceneB); stateB = await state();
  if (Math.abs(stateA.inputVolumeDb + 12) > .15 || !stateA.inputMuted || Math.abs(stateB.inputVolumeDb + 6) > .15 || stateB.inputMuted)
   throw new Error('Disposable fixtures do not have the expected test state');
  for (const sceneName of [sceneA, sceneB]) {
   const item = await c.request('CreateSceneItem', {sceneName, sourceName: 'Widget — Relógio', sceneItemEnabled: true});
   added.push({sceneName, sceneItemId: item.sceneItemId});
  }
  await c.request('StartRecord'); recording = true;
  const segment = async label => {
   const startMs = (await c.request('GetRecordStatus')).outputDuration;
   await wait(2400);
   const endMs = (await c.request('GetRecordStatus')).outputDuration;
   segments.push({label, startMs, endMs, ...await state()});
  };
  await wait(700); await segment('global-minus-6');
  await switchScene(sceneA); await segment('local-muted');
  await c.request('SetInputMute', {inputName, inputMuted: false});
  await wait(700); await segment('local-minus-12');
  await switchScene(sceneB); await segment('global-return-minus-6');
  result = await c.request('StopRecord'); recording = false;
 } finally {
  try {
   if (recording) { result = await c.request('StopRecord'); recording = false; }
   await restoreState(sceneA, stateA);
   await restoreState(sceneB, stateB);
   if (original) await switchScene(original);
   for (const item of added) await c.request('RemoveSceneItem', item);
  } finally { c.close(); }
 }
 const report = {result, segments, originalSceneRestored: original, temporaryWidgetItemsRemoved: added.length, streamStarted: false};
 const reportPath = path.join(__dirname, '../validation/scene-audio-recording.json');
 fs.mkdirSync(path.dirname(reportPath), {recursive: true});
 fs.writeFileSync(reportPath, JSON.stringify(report, null, 2) + '\n');
 console.log(JSON.stringify(report, null, 2));
})().catch(error => {console.error(error.message); process.exitCode = 1;});
