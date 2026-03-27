#pragma once

#include <Arduino.h>

static const char WEB_UI_HTML[] PROGMEM = R"html(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32-C3 VTX Control</title>
  <style>
    :root {
      --bg: #f3efe5;
      --panel: rgba(255,255,255,0.84);
      --ink: #16211d;
      --muted: #5d695f;
      --accent: #0f7c59;
      --accent-2: #c96b2c;
      --line: rgba(22,33,29,0.12);
      --shadow: 0 24px 60px rgba(30, 40, 35, 0.14);
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: Georgia, "Times New Roman", serif;
      color: var(--ink);
      background:
        radial-gradient(circle at top left, rgba(201,107,44,0.18), transparent 32%),
        radial-gradient(circle at bottom right, rgba(15,124,89,0.14), transparent 28%),
        linear-gradient(140deg, #efe7d7, var(--bg));
      min-height: 100vh;
    }
    .shell {
      max-width: 980px;
      margin: 0 auto;
      padding: 28px 18px 48px;
    }
    .hero {
      padding: 28px;
      border: 1px solid var(--line);
      background: linear-gradient(135deg, rgba(255,255,255,0.9), rgba(255,255,255,0.62));
      box-shadow: var(--shadow);
      border-radius: 28px;
      backdrop-filter: blur(10px);
    }
    h1 {
      margin: 0 0 8px;
      font-size: clamp(2rem, 3.2vw, 3.2rem);
      line-height: 0.95;
      letter-spacing: -0.04em;
    }
    .subtitle {
      margin: 0;
      max-width: 700px;
      color: var(--muted);
      font-size: 1rem;
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
      gap: 18px;
      margin-top: 18px;
    }
    .panel {
      border: 1px solid var(--line);
      background: var(--panel);
      border-radius: 24px;
      padding: 22px;
      box-shadow: var(--shadow);
      backdrop-filter: blur(8px);
    }
    h2 {
      margin: 0 0 16px;
      font-size: 1.15rem;
      letter-spacing: 0.02em;
      text-transform: uppercase;
    }
    label {
      display: block;
      margin-bottom: 14px;
      font-size: 0.9rem;
      color: var(--muted);
    }
    input, select, button, textarea {
      width: 100%;
      margin-top: 6px;
      border-radius: 14px;
      border: 1px solid rgba(22,33,29,0.18);
      background: rgba(255,255,255,0.92);
      padding: 12px 14px;
      font: inherit;
      color: var(--ink);
    }
    textarea {
      min-height: 280px;
      resize: vertical;
      font-family: "Courier New", monospace;
      font-size: 0.9rem;
      line-height: 1.4;
    }
    button {
      cursor: pointer;
      background: linear-gradient(135deg, var(--accent), #0e6146);
      color: #f7fbf8;
      font-weight: 700;
      border: none;
      transition: transform 140ms ease, box-shadow 140ms ease;
      box-shadow: 0 16px 30px rgba(15,124,89,0.22);
    }
    button.secondary {
      background: linear-gradient(135deg, var(--accent-2), #9b4f1f);
      box-shadow: 0 16px 30px rgba(201,107,44,0.18);
    }
    button:hover {
      transform: translateY(-1px);
    }
    .stack {
      display: grid;
      gap: 10px;
    }
    .chips {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
    }
    .chip {
      padding: 10px 12px;
      border-radius: 999px;
      border: 1px solid var(--line);
      background: rgba(255,255,255,0.78);
      font-size: 0.88rem;
    }
    .status {
      min-height: 52px;
      white-space: pre-wrap;
      color: var(--muted);
    }
    .status strong {
      color: var(--ink);
    }
    .actions {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
      gap: 10px;
    }
    .config-grid {
      display: grid;
      grid-template-columns: minmax(240px, 300px) minmax(0, 1fr);
      gap: 16px;
      align-items: start;
    }
    .config-sidebar,
    .device-board {
      border: 1px solid var(--line);
      border-radius: 20px;
      background: rgba(255,255,255,0.7);
      padding: 16px;
    }
    .config-sidebar {
      display: grid;
      gap: 12px;
    }
    .section-title {
      margin: 0;
      font-size: 0.9rem;
      letter-spacing: 0.08em;
      text-transform: uppercase;
      color: var(--muted);
    }
    .device-board-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      margin-bottom: 12px;
    }
    .device-board-header p {
      margin: 0;
      color: var(--muted);
      font-size: 0.9rem;
    }
    .device-list {
      display: grid;
      grid-auto-flow: column;
      grid-auto-columns: minmax(300px, 340px);
      gap: 14px;
      overflow-x: auto;
      padding-bottom: 8px;
      align-items: start;
      scrollbar-width: thin;
    }
    .device-card {
      border: 1px solid var(--line);
      border-radius: 18px;
      padding: 16px;
      background: rgba(255,255,255,0.78);
      min-height: 100%;
    }
    .device-card h3 {
      margin: 0 0 10px;
      font-size: 1rem;
      letter-spacing: 0.02em;
      text-transform: uppercase;
    }
    .control-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(120px, 1fr));
      gap: 10px;
      margin-top: 10px;
    }
    .control-box {
      margin-top: 12px;
      padding-top: 12px;
      border-top: 1px solid var(--line);
    }
    @media (max-width: 860px) {
      .config-grid {
        grid-template-columns: 1fr;
      }
      .device-list {
        grid-auto-columns: minmax(260px, 88vw);
      }
    }
  </style>
</head>
<body>
  <div class="shell">
    <section class="hero">
      <h1>VTX Control Deck</h1>
      <p class="subtitle">Manage several VTX devices from one ESP32-C3. Each device gets its own PWM input pin, VTX control pin, and protocol. The firmware serializes outgoing commands, so several VTXs can be controlled from one board as long as update rates stay modest.</p>
    </section>

    <div class="grid">
      <section class="panel">
        <h2>Live State</h2>
        <div class="chips" id="state-chips"></div>
        <div class="status" id="status-text">Loading state...</div>
      </section>

      <section class="panel">
        <h2>Global Config</h2>
        <form id="config-form" class="stack">
          <div class="config-grid">
            <div class="config-sidebar">
              <p class="section-title">Board Settings</p>
              <label>WiFi / ESP-NOW channel
                <input type="number" min="1" max="13" name="wifiChannel" required>
              </label>
              <label>QuickESPNow
                <select name="espNowEnabled">
                  <option value="1">Enabled</option>
                  <option value="0">Disabled</option>
                </select>
              </label>
              <div class="actions">
                <button type="button" id="add-device-button" class="secondary">Add VTX device</button>
                <button type="submit">Save configuration</button>
              </div>
            </div>
            <div class="device-board">
              <div class="device-board-header">
                <p class="section-title">Device Columns</p>
                <p>Each VTX is a separate column for faster side-by-side edits.</p>
              </div>
              <div id="device-list" class="device-list"></div>
            </div>
          </div>
        </form>
      </section>

      <section class="panel">
        <h2>Upload Table</h2>
        <form id="upload-form" class="stack">
          <label>JSON file
            <input type="file" name="file" accept=".json,application/json" required>
          </label>
          <button type="submit" class="secondary">Upload JSON table</button>
        </form>
      </section>

      <section class="panel" style="grid-column: 1 / -1;">
        <h2>JSON Table Editor</h2>
        <form id="json-form" class="stack">
          <label>Load table from file list
            <select name="path" id="json-table-select"></select>
          </label>
          <label>Device assignment target
            <select id="json-device-select"></select>
          </label>
          <div class="actions">
            <button type="button" id="load-json-button">Load selected JSON</button>
            <button type="button" id="load-active-button" class="secondary">Load selected device table</button>
          </div>
          <label>Save as file name
            <input type="text" name="savePath" id="save-path" placeholder="custom_vtx.json">
          </label>
          <label>VTX table JSON
            <textarea name="json" id="json-editor" spellcheck="false"></textarea>
          </label>
          <div class="actions">
            <button type="submit">Save JSON table</button>
            <button type="button" id="select-saved-button" class="secondary">Save and assign to device</button>
          </div>
        </form>
      </section>
    </div>
  </div>

  <script>
    const stateText = document.getElementById('status-text');
    const chips = document.getElementById('state-chips');
    const configForm = document.getElementById('config-form');
    const uploadForm = document.getElementById('upload-form');
    const jsonForm = document.getElementById('json-form');
    const jsonTableSelect = document.getElementById('json-table-select');
    const jsonDeviceSelect = document.getElementById('json-device-select');
    const jsonEditor = document.getElementById('json-editor');
    const savePathInput = document.getElementById('save-path');
    const loadJsonButton = document.getElementById('load-json-button');
    const loadActiveButton = document.getElementById('load-active-button');
    const selectSavedButton = document.getElementById('select-saved-button');
    const addDeviceButton = document.getElementById('add-device-button');
    const deviceList = document.getElementById('device-list');
    let devicesState = [];
    let tablePaths = [];
    let statePollTimer = null;
    let restartPending = false;
    let restartReloadTimer = null;

    function scheduleReloadAfterRestart(delayMs = 3000) {
      if (restartReloadTimer) {
        clearTimeout(restartReloadTimer);
      }
      restartReloadTimer = window.setTimeout(() => {
        window.location.reload();
      }, delayMs);
    }

    function createDevice(device = {}) {
      return {
        name: device.name || `VTX ${devicesState.length + 1}`,
        pwmInputPin: Number(device.pwmInputPin ?? 2),
        vtxControlPin: Number(device.vtxControlPin ?? 4),
        protocol: device.protocol || 'smartaudio',
        controlMode: device.controlMode || 'pwm',
        activeControlMode: device.activeControlMode || device.controlMode || 'pwm',
        enabled: device.enabled !== false,
        manualBand: Number(device.manualBand ?? 1),
        manualChannel: Number(device.manualChannel ?? 1),
        manualPowerIndex: Number(device.manualPowerIndex ?? 0),
        bandOptions: Array.isArray(device.bandOptions) ? device.bandOptions : [],
        channelCount: Number(device.channelCount ?? 8),
        powerOptions: Array.isArray(device.powerOptions) ? device.powerOptions : [],
        vtxTablePath: device.vtxTablePath || tablePaths[0] || '/peak_thor_t35.json',
        filteredPulse: Number(device.filteredPulse ?? 1500),
        lastMappedIndex: Number(device.lastMappedIndex ?? -1),
        currentPwmUs: Number(device.currentPwmUs ?? device.filteredPulse ?? 1500),
        band: device.band ?? null,
        bandLabel: device.bandLabel || '',
        channel: device.channel ?? null,
        powerIndex: device.powerIndex ?? null,
        powerValue: device.powerValue ?? null,
        powerLabel: device.powerLabel || '',
        frequency: device.frequency ?? null
      };
    }

    function describeDeviceLiveState(device) {
      const band = device.bandLabel || (device.band != null ? String(device.band) : 'n/a');
      const channel = device.channel != null ? device.channel : 'n/a';
      const power = device.powerLabel || (device.powerValue != null ? String(device.powerValue) : 'n/a');
      const frequency = device.frequency != null ? `${device.frequency} MHz` : 'n/a';
      return `PWM ${device.currentPwmUs} us | Band ${band} | Channel ${channel} | Power ${power} | Freq ${frequency}`;
    }

    function getTableOptions(selectedPath) {
      const options = [...tablePaths];
      if (selectedPath && !options.includes(selectedPath)) {
        options.push(selectedPath);
      }
      return options;
    }

    function getSelectedBandFrequencies(device) {
      const selectedBandValue = Number(device.manualBand || 1);
      const selectedBand = (device.bandOptions || []).find((option) => Number(option.value) === selectedBandValue);
      return Array.isArray(selectedBand?.frequencies) ? selectedBand.frequencies : [];
    }

    function addTablePath(path) {
      if (!path || tablePaths.includes(path)) {
        return;
      }
      tablePaths.push(path);
    }

    function renderJsonDeviceSelect() {
      const previousValue = Number(jsonDeviceSelect.value || 0);
      jsonDeviceSelect.innerHTML = '';
      devicesState.forEach((device, index) => {
        const option = document.createElement('option');
        option.value = String(index);
        option.textContent = `${index + 1}: ${device.name}`;
        jsonDeviceSelect.appendChild(option);
      });
      if (devicesState.length > 0) {
        jsonDeviceSelect.value = String(Math.min(previousValue, devicesState.length - 1));
      }
    }

    function renderDeviceList() {
      deviceList.innerHTML = '';
      devicesState.forEach((device, index) => {
        const tableOptions = getTableOptions(device.vtxTablePath).map((path) => `
              <option value="${path}" ${device.vtxTablePath === path ? 'selected' : ''}>${path}</option>`).join('');
        const bandOptions = (device.bandOptions || []).map((option) => `
          <option value="${option.value}" ${Number(device.manualBand) === Number(option.value) ? 'selected' : ''}>${option.label || option.value}</option>`).join('');
        const selectedBandFrequencies = getSelectedBandFrequencies(device);
        const channelOptions = Array.from({ length: Math.max(1, device.channelCount || 8) }, (_, channelIndex) => {
          const value = channelIndex + 1;
          const frequency = Number(selectedBandFrequencies[channelIndex] ?? 0);
          const label = frequency > 0 ? `${value} (${frequency})` : `${value}`;
          return `<option value="${value}" ${Number(device.manualChannel) === value ? 'selected' : ''}>${label}</option>`;
        }).join('');
        const powerOptions = (device.powerOptions || []).map((option) => `
          <option value="${option.value}" ${Number(device.manualPowerIndex) === Number(option.value) ? 'selected' : ''}>${option.label || option.powerValue || option.value}</option>`).join('');
        const serialControlEnabled = device.enabled && device.activeControlMode === 'serial' && !restartPending;
        const card = document.createElement('div');
        card.className = 'device-card';
        card.innerHTML = `
          <h3>Device ${index + 1}</h3>
          <label>Name
            <input type="text" value="${device.name}">
          </label>
          <label>PWM input pin
            <input type="number" min="0" max="21" value="${device.pwmInputPin}">
          </label>
          <label>VTX control pin
            <input type="number" min="0" max="21" value="${device.vtxControlPin}">
          </label>
          <label>Protocol
            <select>
              <option value="smartaudio" ${device.protocol === 'smartaudio' ? 'selected' : ''}>SmartAudio</option>
              <option value="tramp" ${device.protocol === 'tramp' ? 'selected' : ''}>TRAMP</option>
            </select>
          </label>
          <label>Control
            <select>
              <option value="pwm" ${device.controlMode === 'pwm' ? 'selected' : ''}>PWM</option>
              <option value="serial" ${device.controlMode === 'serial' ? 'selected' : ''}>Serial</option>
            </select>
          </label>
          <label>VTX table
            <select>${tableOptions}
            </select>
          </label>
          <label>Enabled
            <select>
              <option value="1" ${device.enabled ? 'selected' : ''}>Enabled</option>
              <option value="0" ${!device.enabled ? 'selected' : ''}>Disabled</option>
            </select>
          </label>
          ${serialControlEnabled ? `
          <div class="control-box">
            <div class="status"><strong>Manual Control</strong></div>
            <div class="control-grid">
              <label>Band
                <select data-role="manual-band">${bandOptions}</select>
              </label>
              <label>Channel
                <select data-role="manual-channel">${channelOptions}</select>
              </label>
              <label>Power
                <select data-role="manual-power">${powerOptions}</select>
              </label>
            </div>
            <div class="actions">
              <button type="button" class="apply-control-button">Apply serial control</button>
            </div>
          </div>` : ''}
          <div class="status" data-role="live-state">${describeDeviceLiveState(device)}</div>
          <div class="actions">
            <button type="button" class="secondary">Remove device</button>
          </div>
        `;

        const [nameInput, pwmInput, vtxInput, protocolInput, controlModeInput, tableInput, enabledInput] = card.querySelectorAll('input, select');
        const manualBandInput = card.querySelector('[data-role="manual-band"]');
        const manualChannelInput = card.querySelector('[data-role="manual-channel"]');
        const manualPowerInput = card.querySelector('[data-role="manual-power"]');
        const applyControlButton = card.querySelector('.apply-control-button');
        nameInput.addEventListener('input', () => { device.name = nameInput.value; });
        pwmInput.addEventListener('input', () => { device.pwmInputPin = Number(pwmInput.value); });
        vtxInput.addEventListener('input', () => { device.vtxControlPin = Number(vtxInput.value); });
        protocolInput.addEventListener('change', () => { device.protocol = protocolInput.value; });
        controlModeInput.addEventListener('change', () => {
          device.controlMode = controlModeInput.value;
          renderDeviceList();
        });
        tableInput.addEventListener('change', () => { device.vtxTablePath = tableInput.value; });
        enabledInput.addEventListener('change', () => { device.enabled = enabledInput.value === '1'; });
        if (manualBandInput) {
          manualBandInput.addEventListener('change', () => { device.manualBand = Number(manualBandInput.value); });
        }
        if (manualChannelInput) {
          manualChannelInput.addEventListener('change', () => { device.manualChannel = Number(manualChannelInput.value); });
        }
        if (manualPowerInput) {
          manualPowerInput.addEventListener('change', () => { device.manualPowerIndex = Number(manualPowerInput.value); });
        }
        if (applyControlButton) {
          applyControlButton.addEventListener('click', async () => {
            const body = new URLSearchParams();
            body.set('device', String(index));
            body.set('band', String(device.manualBand || 1));
            body.set('channel', String(device.manualChannel || 1));
            body.set('power', String(device.manualPowerIndex || 0));
            const response = await fetch('/api/control', { method: 'POST', body });
            const data = await response.json();
            setStatus(data.message || 'Manual control processed', data.ok ? 'Applied' : 'Control failed');
            if (data.ok) {
              await loadState();
            }
          });
        }
        card.querySelector('button.secondary').addEventListener('click', () => {
          devicesState.splice(index, 1);
          if (devicesState.length === 0) {
            devicesState.push(createDevice());
          }
          renderDeviceList();
          renderJsonDeviceSelect();
        });
        deviceList.appendChild(card);
      });
      renderJsonDeviceSelect();
    }

    function applyLiveState(state) {
      chips.innerHTML = '';
      [
        `SSID ${state.ssid}`,
        `IP ${state.ip}`,
        `Devices ${state.deviceCount || devicesState.length}`,
        `ESP-NOW ${state.espNowEnabled ? 'on' : 'off'}`
      ].forEach((text) => {
        const chip = document.createElement('div');
        chip.className = 'chip';
        chip.textContent = text;
        chips.appendChild(chip);
      });

      setStatus(`Configured devices: ${state.deviceCount || devicesState.length}\nTransport ready: ${state.transportReady ? 'yes' : 'no'}\nEach device keeps its own VTX table assignment.\nCommands are serialized across devices.`);
      restartPending = Boolean(state.restartPending);
      if (restartPending) {
        setStatus(`Configured devices: ${state.deviceCount || devicesState.length}\nTransport ready: ${state.transportReady ? 'yes' : 'no'}\nRestart pending; wait for the board to come back before using serial control.`, 'Restarting');
        scheduleReloadAfterRestart();
      }

      const liveDevices = Array.isArray(state.devices) ? state.devices : [];
      liveDevices.forEach((liveDevice, index) => {
        if (!devicesState[index]) {
          return;
        }
        devicesState[index].activeControlMode = liveDevice.controlMode || devicesState[index].activeControlMode;
        devicesState[index].filteredPulse = Number(liveDevice.filteredPulse ?? devicesState[index].filteredPulse);
        devicesState[index].lastMappedIndex = Number(liveDevice.lastMappedIndex ?? devicesState[index].lastMappedIndex);
        devicesState[index].currentPwmUs = Number(liveDevice.currentPwmUs ?? devicesState[index].currentPwmUs);
        devicesState[index].band = liveDevice.band ?? null;
        devicesState[index].bandLabel = liveDevice.bandLabel || '';
        devicesState[index].channel = liveDevice.channel ?? null;
        devicesState[index].powerIndex = liveDevice.powerIndex ?? null;
        devicesState[index].powerValue = liveDevice.powerValue ?? null;
        devicesState[index].powerLabel = liveDevice.powerLabel || '';
        devicesState[index].frequency = liveDevice.frequency ?? null;

        const card = deviceList.children[index];
        const liveState = card ? card.querySelector('[data-role="live-state"]') : null;
        if (liveState) {
          liveState.textContent = describeDeviceLiveState(devicesState[index]);
        }
      });
    }

    function setStatus(message, strong) {
      stateText.innerHTML = strong ? `<strong>${strong}</strong>\n${message}` : message;
    }

    function fillState(state) {
      for (const [key, value] of Object.entries({
        wifiChannel: state.wifiChannel,
        espNowEnabled: state.espNowEnabled ? '1' : '0'
      })) {
        const input = configForm.elements.namedItem(key);
        if (input) {
          input.value = value;
        }
      }

      jsonTableSelect.innerHTML = '';
      tablePaths = Array.isArray(state.tables) ? [...state.tables] : [];
      tablePaths.forEach((path) => {
        const editorOption = document.createElement('option');
        editorOption.value = path;
        editorOption.textContent = path;
        jsonTableSelect.appendChild(editorOption);
      });

      if (!savePathInput.value) {
        savePathInput.value = (tablePaths[0] || '').replace(/^\//, '');
      }

      devicesState = (state.devices || []).map((device) => createDevice(device));
      if (devicesState.length === 0) {
        devicesState = [createDevice()];
      }
      renderDeviceList();

      applyLiveState(state);
    }

    async function loadState() {
      const response = await fetch('/api/state');
      if (!response.ok) {
        throw new Error('Failed to load state');
      }
      const data = await response.json();
      fillState(data);
      return data;
    }

    async function pollLiveState() {
      try {
        const response = await fetch('/api/state');
        if (!response.ok) {
          throw new Error('Failed to refresh state');
        }
        const state = await response.json();
        if (Array.isArray(state.devices) && state.devices.length === devicesState.length) {
          applyLiveState(state);
        }
      } catch (error) {
        if (restartPending) {
          scheduleReloadAfterRestart(1500);
        }
        console.error(error);
      }
    }

    async function loadTableJson(path = '', deviceIndex = null) {
      let url = '/api/vtx-table';
      if (typeof deviceIndex === 'number') {
        url = `/api/vtx-table?device=${encodeURIComponent(deviceIndex)}`;
      } else if (path) {
        url = `/api/vtx-table?path=${encodeURIComponent(path)}`;
      }
      const response = await fetch(url);
      if (!response.ok) {
        throw new Error('Failed to load table JSON');
      }
      const data = await response.json();
      jsonEditor.value = data.json || '';
      savePathInput.value = (data.path || '').replace(/^\//, '');
      if (data.path) {
        jsonTableSelect.value = data.path;
      }
      setStatus(`Loaded ${data.path || 'device table'} into editor`, 'JSON ready');
      return data;
    }

    async function saveTableJson(selectAfterSave) {
      const body = new URLSearchParams();
      body.set('path', savePathInput.value || 'custom_vtx.json');
      body.set('json', jsonEditor.value);
      body.set('select', selectAfterSave ? '1' : '0');
      body.set('device', jsonDeviceSelect.value || '0');

      const response = await fetch('/api/vtx-table', { method: 'POST', body });
      const data = await response.json();
      setStatus(data.message || 'JSON save processed', data.ok ? 'JSON saved' : 'JSON failed');
      await loadState();
      if (data.ok) {
        await loadTableJson(data.path || '');
      }
    }

    configForm.addEventListener('submit', async (event) => {
      event.preventDefault();
      const body = new URLSearchParams();
      body.set('wifiChannel', configForm.elements.namedItem('wifiChannel').value);
      body.set('espNowEnabled', configForm.elements.namedItem('espNowEnabled').value);
      body.set('devicesJson', JSON.stringify(devicesState.map((device) => ({
        name: device.name,
        pwmInputPin: Number(device.pwmInputPin),
        vtxControlPin: Number(device.vtxControlPin),
        protocol: device.protocol,
        controlMode: device.controlMode,
        enabled: Boolean(device.enabled),
        manualBand: Number(device.manualBand || 1),
        manualChannel: Number(device.manualChannel || 1),
        manualPowerIndex: Number(device.manualPowerIndex || 0),
        vtxTablePath: device.vtxTablePath
      }))));
      const response = await fetch('/api/config', { method: 'POST', body });
      const data = await response.json();
      setStatus(data.message || 'Configuration saved', data.restartRequired ? 'Restarting' : 'Saved');
      restartPending = Boolean(data.restartRequired);
      if (restartPending) {
        scheduleReloadAfterRestart();
      }
      await loadState();
    });

    uploadForm.addEventListener('submit', async (event) => {
      event.preventDefault();
      const body = new FormData(uploadForm);
      const response = await fetch('/api/upload', { method: 'POST', body });
      const data = await response.json();
      setStatus(data.message || 'Upload processed', data.ok ? 'Upload complete' : 'Upload failed');
      if (data.ok && data.path) {
        addTablePath(data.path);
      }
      await loadState();
      uploadForm.reset();
    });

    loadJsonButton.addEventListener('click', async () => {
      await loadTableJson(jsonTableSelect.value);
    });

    loadActiveButton.addEventListener('click', async () => {
      await loadTableJson('', Number(jsonDeviceSelect.value || 0));
    });

    jsonForm.addEventListener('submit', async (event) => {
      event.preventDefault();
      await saveTableJson(false);
    });

    selectSavedButton.addEventListener('click', async () => {
      await saveTableJson(true);
    });

    addDeviceButton.addEventListener('click', () => {
      devicesState.push(createDevice({
        name: `VTX ${devicesState.length + 1}`,
        pwmInputPin: 2 + devicesState.length,
        vtxControlPin: 4 + devicesState.length,
        protocol: 'smartaudio',
        controlMode: 'pwm',
        enabled: true,
        vtxTablePath: tablePaths[0] || '/peak_thor_t35.json'
      }));
      renderDeviceList();
    });

    loadState().then(() => {
      loadTableJson('', 0);
      if (statePollTimer) {
        clearInterval(statePollTimer);
      }
      statePollTimer = window.setInterval(pollLiveState, 1000);
    }).catch((error) => {
      setStatus(error.message || String(error), 'Error');
    });
  </script>
</body>
</html>
)html";
