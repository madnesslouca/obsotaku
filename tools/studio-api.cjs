#!/usr/bin/env node
/* Local OBS WebSocket client. Credentials stay in the isolated OBS config. */
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const crypto = require('node:crypto');

async function connectStudio(configPath) {
  const config = JSON.parse(fs.readFileSync(configPath, 'utf8'));
  if (!config.server_enabled) throw new Error('The Studio WebSocket server is disabled');
  const socket = new WebSocket(`ws://127.0.0.1:${config.server_port}`);
  const pending = new Map();
  let readyResolve, readyReject;
  const ready = new Promise((resolve, reject) => { readyResolve = resolve; readyReject = reject; });
  const startupTimer = setTimeout(() => readyReject(new Error('OBS handshake timed out')), 10000);
  const fail = () => {
    const error = new Error('OBS connection closed');
    readyReject(error);
    for (const { reject, timer } of pending.values()) { clearTimeout(timer); reject(error); }
    pending.clear();
  };
  socket.addEventListener('error', fail);
  socket.addEventListener('close', fail);
  socket.addEventListener('message', event => {
    try {
      const message = JSON.parse(event.data);
      if (message.op === 0) {
        const identify = { rpcVersion: 1, eventSubscriptions: 0 };
        if (message.d.authentication) {
          const hash = value => crypto.createHash('sha256').update(value).digest('base64');
          identify.authentication = hash(hash(config.server_password + message.d.authentication.salt) + message.d.authentication.challenge);
        }
        socket.send(JSON.stringify({ op: 1, d: identify }));
      } else if (message.op === 2) {
        clearTimeout(startupTimer);
        readyResolve();
      } else if (message.op === 7) {
        const request = pending.get(message.d.requestId);
        if (!request) return;
        pending.delete(message.d.requestId);
        clearTimeout(request.timer);
        if (message.d.requestStatus.result) request.resolve(message.d.responseData || {});
        else request.reject(new Error(`${message.d.requestType}: ${message.d.requestStatus.code} ${message.d.requestStatus.comment || ''}`));
      }
    } catch (error) { readyReject(error); }
  });
  try { await ready; } catch (error) { clearTimeout(startupTimer); socket.close(); throw error; }
  return {
    request(type, data = {}) {
      return new Promise((resolve, reject) => {
        const id = crypto.randomUUID();
        const timer = setTimeout(() => { pending.delete(id); reject(new Error(`${type} timed out`)); }, 10000);
        pending.set(id, { resolve, reject, timer });
        socket.send(JSON.stringify({ op: 6, d: { requestType: type, requestId: id, requestData: data } }));
      });
    },
    close() { socket.close(); }
  };
}

const configRoot = process.env.OBS_STUDIO_CONFIG_HOME || path.join(process.env.XDG_CONFIG_HOME || path.join(os.homedir(), '.config'), 'obs-zyon');
const defaultConfig = path.join(configRoot, 'obs-studio/plugin_config/obs-websocket/config.json');
module.exports = { connectStudio, defaultConfig };
if (require.main === module) {
  (async () => {
    const client = await connectStudio(defaultConfig);
    try { console.log(JSON.stringify(await client.request(process.argv[2] || 'GetStats', JSON.parse(process.argv[3] || '{}')), null, 2)); }
    finally { client.close(); }
  })().catch(error => { console.error(error.message); process.exitCode = 1; });
}
