#pragma once

#include <Arduino.h>

static const char WEB_UI_HTML[] PROGMEM = R"html(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Панель керування VTX</title>
  <style>
    :root {
      --bg: #0f1720; /* deep navy */
      --panel: rgba(18,25,30,0.6);
      --ink: #e6f3ef;
      --muted: #98a6a0;
      --accent: #6ee7b7; /* mint */
      --accent-2: #ffb570; /* warm orange */
      --line: rgba(255,255,255,0.06);
      --glass: rgba(255,255,255,0.03);
      --shadow: 0 10px 30px rgba(2,8,12,0.6);
      --radius-lg: 16px;
      --radius-md: 10px;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: Inter, system-ui, -apple-system, "Segoe UI", Roboto, "Helvetica Neue", Arial;
      color: var(--ink);
      background: linear-gradient(180deg, #071018 0%, #0b1320 60%);
      min-height: 100vh;
      -webkit-font-smoothing: antialiased;
      -moz-osx-font-smoothing: grayscale;
    }
    .shell {
      max-width: 1200px;
      margin: 28px auto 56px;
      padding: 24px;
    }
    .hero {
      padding: 26px;
      border-radius: var(--radius-lg);
      background: linear-gradient(180deg, rgba(255,255,255,0.03), rgba(255,255,255,0.02));
      border: 1px solid var(--line);
      box-shadow: var(--shadow);
      display: flex;
      gap: 18px;
      align-items: center;
      justify-content: space-between;
    }
    h1 { margin: 0; font-size: 1.7rem; letter-spacing: -0.02em; }
    .subtitle { margin: 0; color: var(--muted); font-size: 0.95rem; max-width: 820px; }
    .grid { display: grid; grid-template-columns: 1fr; gap: 18px; margin-top: 18px; }
    @media(min-width:980px){ .grid { grid-template-columns: 1fr; } }
    .panel {
      border-radius: var(--radius-lg);
      padding: 18px;
      background: linear-gradient(180deg, rgba(255,255,255,0.02), rgba(255,255,255,0.01));
      border: 1px solid var(--line);
      box-shadow: 0 8px 24px rgba(2,8,12,0.45);
      backdrop-filter: blur(6px) saturate(1.05);
    }
    h2 { margin: 0 0 12px; font-size: 0.92rem; color: var(--muted); text-transform: uppercase; }
    label { display: block; margin-bottom: 10px; font-size: 0.9rem; color: var(--muted); }
    .inline-row { display:flex; gap:12px; align-items:center; }
    .inline-row label { flex:1 1 0; margin-bottom:0; }
    input, select, button, textarea {
      width:100%; margin-top:6px; padding:10px 12px; border-radius: var(--radius-md);
      border: 1px solid rgba(255,255,255,0.04); background: rgba(255,255,255,0.02); color:var(--ink);
      font: inherit;
    }
    textarea { min-height:220px; font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, "Courier New", monospace; }
    button { cursor:pointer; padding:10px 14px; border-radius:999px; border:none; background: linear-gradient(90deg,var(--accent), #4fd19f); color:#071018; font-weight:700; box-shadow: 0 8px 20px rgba(0,0,0,0.45); }
    button.secondary { background: linear-gradient(90deg,var(--accent-2), #f48f4a); color:#071018; }
    button:active{ transform: translateY(1px); }
    .stack{ display:grid; gap:10px; }
    .chips{ display:flex; gap:10px; flex-wrap:wrap; }
    .chip{ padding:8px 12px; border-radius:999px; background:var(--glass); border:1px solid rgba(255,255,255,0.03); color:var(--muted); font-size:0.88rem; }
    .status{ min-height:56px; color:var(--muted); white-space:pre-wrap; }
    .status strong{ color:var(--ink); }
    .actions{ display:flex; gap:10px; flex-wrap:wrap; }
    .device-list{ overflow-x:auto; overflow-y:visible; padding-bottom:6px; }
    .device-card{ border-radius:12px; padding:12px; background: rgba(255,255,255,0.015); border:1px solid rgba(255,255,255,0.02); }
    .control-grid{ display:grid; grid-template-columns: repeat(auto-fit, minmax(120px,1fr)); gap:10px; }
    .control-box{ margin-top:12px; padding-top:12px; border-top:1px dashed rgba(255,255,255,0.02); }
    .device-table{ width:100%; border-collapse:separate; border-spacing:0; margin-top:8px; min-width:680px; background:transparent; }
    .device-table th, .device-table td{ padding:10px 12px; vertical-align:middle; text-align:left; border-bottom:1px solid rgba(255,255,255,0.03); }
    .device-table thead th{ position:sticky; top:0; z-index:3; background: linear-gradient(180deg, rgba(8,12,16,0.7), rgba(8,12,16,0.6)); color:var(--muted); font-size:0.78rem; text-transform:uppercase; }
    .device-table tbody tr:hover td{ background: rgba(255,255,255,0.01); }
    .device-table tbody td{ background: rgba(255,255,255,0.01); }
    .device-table tr td:first-child{ font-weight:600; color:var(--muted); width:220px; }
    .device-table select, .device-table input{ background: rgba(255,255,255,0.015); border:1px solid rgba(255,255,255,0.03); }
    .apply-control-button{ background: linear-gradient(90deg,var(--accent), #4fd19f); color:#051014; padding:8px 10px; border-radius:10px; }
    .remove-device{ background: linear-gradient(90deg,#ff7b7b,#ffb3b3); color:#071018; padding:8px 10px; border-radius:10px; }
    @media (max-width:720px){ .device-table tr td:first-child{ width:140px; font-size:0.86rem; } }
    /* Improve dropdown/readability for various platforms */
    select {
      color: var(--ink) !important;
      background: linear-gradient(180deg, rgba(255,255,255,0.02), rgba(255,255,255,0.01));
      border: 1px solid rgba(255,255,255,0.06);
      padding-right: 34px; /* space for custom arrow */
      -webkit-appearance: none;
      -moz-appearance: none;
      appearance: none;
      background-repeat: no-repeat;
      background-position: right 10px center;
    }
    select:disabled { color: var(--muted) !important; opacity: 0.9; }
    /* Attempt to style native option list where supported */
    option {
      background: #0b1320; /* match page background */
      color: var(--ink);
    }
    /* fallback for MS Edge/IE arrow */
    select::-ms-expand { display: none; }
    /* subtle focus ring */
    select:focus { outline: none; box-shadow: 0 0 0 3px rgba(110,231,183,0.12); border-color: rgba(110,231,183,0.4); }
  </style>
</head>
<body>
  <div class="shell">
    <section class="hero">
      <h1>Панель керування VTX</h1>
      <p class="subtitle">Керуйте локальними та віддаленими VTX-пристроями з одного ESP32-C3. Плата може працювати автономно, як міст MAVLink від польотного контролера, або як вузол VTX, що приймає команди через ESP-NOW.</p>
    </section>

    <div class="grid">
      <section class="panel" style="grid-column: 1 / -1;">
        <h2>Поточний стан</h2>
        <div class="chips" id="state-chips"></div>
        <div class="status" id="status-text">Завантаження стану...</div>
      </section>

      <section class="panel" style="grid-column: 1 / -1;">
        <h2>Глобальні налаштування</h2>
        <form id="config-form" class="stack">
          <div class="inline-row">
            <label>Канал WiFi / ESP-NOW
              <input type="number" min="1" max="13" name="wifiChannel" required>
            </label>
            <label>QuickESPNow
              <select name="espNowEnabled">
                <option value="1">Увімкнено</option>
                <option value="0">Вимкнено</option>
              </select>
            </label>
          </div>
          <div class="inline-row">
            <label>Роль плати
              <select name="boardRole">
                <option value="standalone">Автономна</option>
                <option value="fc_bridge">FC міст MAVLink</option>
                <option value="vtx_node">VTX вузол</option>
              </select>
            </label>
            <label>Локальний MAVLink System ID
              <input type="number" min="1" max="254" name="localNodeId" required>
            </label>
          </div>
          <div class="inline-row">
            <label>MAVLink RX pin
              <input type="number" min="-1" max="21" name="mavlinkRxPin" required>
            </label>
            <label>MAVLink TX pin
              <input type="number" min="-1" max="21" name="mavlinkTxPin" required>
            </label>
            <label>MAVLink baud
              <input type="number" min="1200" max="1000000" name="mavlinkBaud" required>
            </label>
          </div>
          <div class="actions">
            <button type="button" id="add-device-button" class="secondary">Додати VTX-пристрій</button>
          </div>
          <div id="device-list" class="device-list"></div>
          <button type="submit">Save configuration</button>
        </form>
      </section>

      <section class="panel" style="grid-column: 1 / -1;">
        <h2>Завантажити таблицю</h2>
        <form id="upload-form" class="stack">
          <label>JSON файл
            <input type="file" name="file" accept=".json,application/json" required>
          </label>
          <button type="submit" class="secondary">Завантажити JSON таблицю</button>
        </form>
      </section>

      <section class="panel" style="grid-column: 1 / -1;">
        <h2>Редактор JSON таблиці</h2>
        <form id="json-form" class="stack">
          <label>Завантажити таблицю зі списку файлів
            <select name="path" id="json-table-select"></select>
          </label>
          <label>Призначити для пристрою
            <select id="json-device-select"></select>
          </label>
          <div class="actions">
            <button type="button" id="load-json-button">Завантажити вибраний JSON</button>
            <button type="button" id="load-active-button" class="secondary">Завантажити таблицю вибраного пристрою</button>
          </div>
          <label>Зберегти як ім'я файлу
            <input type="text" name="savePath" id="save-path" placeholder="custom_vtx.json">
          </label>
          <label>JSON таблиця VTX
            <textarea name="json" id="json-editor" spellcheck="false"></textarea>
          </label>
          <div class="actions">
            <button type="submit">Зберегти JSON таблицю</button>
            <button type="button" id="select-saved-button" class="secondary">Зберегти і призначити пристрою</button>
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

    function formatMavlinkDebug(debug) {
      if (!debug) {
        return 'MAVLink debug: no data';
      }
      const seen = debug.lastFrame || null;
      const last = debug.lastCommand || null;
      const seenText = seen
        ? `seen msg=${seen.messageId} src=${seen.sourceSystem}/${seen.sourceComponent} tgt=${seen.targetSystem || 0}/${seen.targetComponent || 0} cmd=${seen.commandId || 0}`
        : 'seen msg=n/a';
      const lastText = last
        ? `last cmd=${last.commandId} src=${last.sourceSystem || 0}/${last.sourceComponent || 0} tgt=${last.targetSystem || 0}/${last.targetComponent || 0} vtxNode=${last.nodeId} vtxDev=${last.deviceId} band=${last.band} ch=${last.channel} pwr=${last.powerIndex}`
        : 'last cmd=n/a';
      return [
        `MAV bytes=${Number(debug.rxByteCount || 0)}`,
        `frames=${Number(debug.seenFrameCount || 0)}`,
        `cmd76=${Number(debug.seenCommandLongCount || 0)}`,
        `parsed=${Number(debug.parsedCommandCount || 0)}`,
        `local=${Number(debug.localApplyCount || 0)}`,
        `forwarded=${Number(debug.forwardCount || 0)}`,
        `espnowRx=${Number(debug.espNowRxCount || 0)}`,
        `hbTx=${Number(debug.heartbeatTxCount || 0)}`,
        `ackTx=${Number(debug.ackTxCount || 0)}`,
        seenText,
        lastText
      ].join(' | ');
    }

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
        pwmInputPin: Number(device.pwmInputPin ?? 3),
        vtxControlPin: Number(device.vtxControlPin ?? 4),
        protocol: device.protocol || 'smartaudio',
        controlMode: device.controlMode || 'pwm',
        activeControlMode: device.activeControlMode || device.controlMode || 'pwm',
        enabled: device.enabled !== false,
        manualBand: Number(device.manualBand ?? 1),
        manualChannel: Number(device.manualChannel ?? 1),
        manualPowerIndex: Number(device.manualPowerIndex ?? 0),
        mavlinkNodeId: Number(device.mavlinkNodeId ?? 1),
        mavlinkDeviceId: Number(device.mavlinkDeviceId ?? (devicesState.length + 1)),
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
      const frequency = device.frequency != null ? `${device.frequency} МГц` : 'n/a';
      return `Застосовано: діапазон ${band} | канал ${channel} | потужність ${power} | частота ${frequency}`;
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
      if (!Array.isArray(devicesState) || devicesState.length === 0) {
        devicesState = [createDevice()];
      }
      const table = document.createElement('table');
      table.className = 'device-table';

      // header row: first column is parameter label, then one column per device
      const headerCells = [`<th>Параметр</th>`, ...devicesState.map((d, i) => `<th>VTX ${i + 1}</th>`)];
      const thead = document.createElement('thead');
      thead.innerHTML = `<tr>${headerCells.join('')}</tr>`;
      table.appendChild(thead);

      // helper to build per-device input cells with data-device attr
      const buildCell = (index, innerHtml) => `<td data-device="${index}">${innerHtml}</td>`;

      // rows of parameters
      const rows = [];
      // Name
      rows.push(`<tr><td><strong>Ім'я</strong></td>${devicesState.map((d, i) => buildCell(i, `<input class="name-input" data-device="${i}" type="text" value="${d.name}">`)).join('')}</tr>`);
      // PWM
      rows.push(`<tr><td><strong>PWM</strong></td>${devicesState.map((d, i) => buildCell(i, `<input class="pwm-input" data-device="${i}" type="number" min="-1" max="21" value="${d.pwmInputPin}">`)).join('')}</tr>`);
      // VTX Pin
      rows.push(`<tr><td><strong>VTX Pin</strong></td>${devicesState.map((d, i) => buildCell(i, `<input class="vtx-input" data-device="${i}" type="number" min="0" max="21" value="${d.vtxControlPin}">`)).join('')}</tr>`);
      // Protocol
      rows.push(`<tr><td><strong>Протокол</strong></td>${devicesState.map((d, i) => buildCell(i, `<select class="protocol-input" data-device="${i}"><option value="smartaudio" ${d.protocol === 'smartaudio' ? 'selected' : ''}>SmartAudio</option><option value="tramp" ${d.protocol === 'tramp' ? 'selected' : ''}>TRAMP</option></select>`)).join('')}</tr>`);
      // Control Mode
      rows.push(`<tr><td><strong>Режим</strong></td>${devicesState.map((d, i) => buildCell(i, `<select class="controlmode-input" data-device="${i}"><option value="pwm" ${d.controlMode === 'pwm' ? 'selected' : ''}>PWM</option><option value="serial" ${d.controlMode === 'serial' ? 'selected' : ''}>Серійний</option><option value="mavlink" ${d.controlMode === 'mavlink' ? 'selected' : ''}>MAVLink</option></select>`)).join('')}</tr>`);
      // Per-VTX addressing inside the bridge command payload
      rows.push(`<tr><td><strong>VTX Node ID</strong></td>${devicesState.map((d, i) => buildCell(i, `<input class="mavlink-node-input" data-device="${i}" type="number" min="1" max="254" value="${d.mavlinkNodeId}">`)).join('')}</tr>`);
      rows.push(`<tr><td><strong>VTX Device ID</strong></td>${devicesState.map((d, i) => buildCell(i, `<input class="mavlink-device-input" data-device="${i}" type="number" min="1" max="254" value="${d.mavlinkDeviceId}">`)).join('')}</tr>`);
      // Table
      rows.push(`<tr><td><strong>Таблиця</strong></td>${devicesState.map((d, i) => buildCell(i, `<select class="table-input" data-device="${i}">${getTableOptions(d.vtxTablePath).map((p) => `<option value="${p}" ${d.vtxTablePath === p ? 'selected' : ''}>${p}</option>`).join('')}</select>`)).join('')}</tr>`);
      // Enabled
      rows.push(`<tr><td><strong>Увімкнений</strong></td>${devicesState.map((d, i) => buildCell(i, `<select class="enabled-input" data-device="${i}"><option value="1" ${d.enabled ? 'selected' : ''}>Так</option><option value="0" ${!d.enabled ? 'selected' : ''}>Ні</option></select>`)).join('')}</tr>`);
      // Requested band
      rows.push(`<tr><td><strong>Запитуваний Band</strong></td>${devicesState.map((d, i) => {
        const bandOpts = (d.bandOptions || []).map((o) => `<option value="${o.value}" ${Number(d.manualBand) === Number(o.value) ? 'selected' : ''}>${o.label || o.value}</option>`).join('');
        const enabled = d.enabled && d.activeControlMode === 'serial' && !restartPending;
        return buildCell(i, `<select data-role="manual-band" class="manual-band" data-device="${i}" ${enabled ? '' : 'disabled'}>${bandOpts}</select>`);
      }).join('')}</tr>`);
      // Requested channel
      rows.push(`<tr><td><strong>Запитуваний Channel</strong></td>${devicesState.map((d, i) => {
        const freqs = getSelectedBandFrequencies(d);
        const chOpts = Array.from({ length: Math.max(1, d.channelCount || 8) }, (_, idx) => {
          const val = idx + 1;
          const freq = Number(freqs[idx] ?? 0);
          const label = freq > 0 ? `${val} (${freq})` : `${val}`;
          return `<option value="${val}" ${Number(d.manualChannel) === val ? 'selected' : ''}>${label}</option>`;
        }).join('');
        const enabled = d.enabled && d.activeControlMode === 'serial' && !restartPending;
        return buildCell(i, `<select data-role="manual-channel" class="manual-channel" data-device="${i}" ${enabled ? '' : 'disabled'}>${chOpts}</select>`);
      }).join('')}</tr>`);
      // Requested power
      rows.push(`<tr><td><strong>Запитувана Power</strong></td>${devicesState.map((d, i) => {
        const pOpts = (d.powerOptions || []).map((opt) => `<option value="${opt.value}" ${Number(d.manualPowerIndex) === Number(opt.value) ? 'selected' : ''}>${opt.label || opt.powerValue || opt.value}</option>`).join('');
        const enabled = d.enabled && d.activeControlMode === 'serial' && !restartPending;
        return buildCell(i, `<select data-role="manual-power" class="manual-power" data-device="${i}" ${enabled ? '' : 'disabled'}>${pOpts}</select>`);
      }).join('')}</tr>`);
      // Applied state
      rows.push(`<tr><td><strong>Останній застосований стан</strong></td>${devicesState.map((d, i) => buildCell(i, `<div data-role="live-state" style="font-size:0.9rem;color:var(--muted);">${describeDeviceLiveState(d)}${d.activeControlMode === 'mavlink' ? '<br><span style="font-size:0.78rem;opacity:0.8;">Локальний стан після застосування, не пряме telemetry/read-back.</span>' : ''}</div>`)).join('')}</tr>`);
      // Actions row
      rows.push(`<tr><td><strong>Дії</strong></td>${devicesState.map((d, i) => buildCell(i, `<div style="display:flex;gap:6px;align-items:center;"><button type="button" class="apply-control-button" data-device="${i}">Застосувати</button><button type="button" class="secondary remove-device" data-device="${i}">Видалити</button></div>`)).join('')}</tr>`);

      const tbody = document.createElement('tbody');
      tbody.innerHTML = rows.join('');
      table.appendChild(tbody);
      deviceList.appendChild(table);

      // attach handlers per device using data-device attr
      devicesState.forEach((device, index) => {
        const q = (sel) => document.querySelector(`${sel}[data-device=\"${index}\"]`);
        const nameInput = q('.name-input');
        const pwmInput = q('.pwm-input');
        const vtxInput = q('.vtx-input');
        const protocolInput = q('.protocol-input');
        const controlModeInput = q('.controlmode-input');
        const mavlinkNodeInput = q('.mavlink-node-input');
        const mavlinkDeviceInput = q('.mavlink-device-input');
        const tableInput = q('.table-input');
        const enabledInput = q('.enabled-input');
        const manualBandInput = q('.manual-band');
        const manualChannelInput = q('.manual-channel');
        const manualPowerInput = q('.manual-power');
        const applyControlButton = document.querySelector(`.apply-control-button[data-device=\"${index}\"]`);
        const removeButton = document.querySelector(`.remove-device[data-device=\"${index}\"]`);

        if (nameInput) nameInput.addEventListener('input', () => { device.name = nameInput.value; renderJsonDeviceSelect(); });
        if (pwmInput) pwmInput.addEventListener('input', () => { device.pwmInputPin = Number(pwmInput.value); });
        if (vtxInput) vtxInput.addEventListener('input', () => { device.vtxControlPin = Number(vtxInput.value); });
        if (protocolInput) protocolInput.addEventListener('change', () => { device.protocol = protocolInput.value; });
        if (controlModeInput) controlModeInput.addEventListener('change', () => { device.controlMode = controlModeInput.value; renderDeviceList(); });
        if (mavlinkNodeInput) mavlinkNodeInput.addEventListener('input', () => { device.mavlinkNodeId = Number(mavlinkNodeInput.value); });
        if (mavlinkDeviceInput) mavlinkDeviceInput.addEventListener('input', () => { device.mavlinkDeviceId = Number(mavlinkDeviceInput.value); });
        if (tableInput) tableInput.addEventListener('change', () => { device.vtxTablePath = tableInput.value; });
        if (enabledInput) enabledInput.addEventListener('change', () => { device.enabled = enabledInput.value === '1'; renderDeviceList(); });
        if (manualBandInput) manualBandInput.addEventListener('change', () => { device.manualBand = Number(manualBandInput.value); renderDeviceList(); });
        if (manualChannelInput) manualChannelInput.addEventListener('change', () => { device.manualChannel = Number(manualChannelInput.value); });
        if (manualPowerInput) manualPowerInput.addEventListener('change', () => { device.manualPowerIndex = Number(manualPowerInput.value); });
        if (applyControlButton) {
          applyControlButton.addEventListener('click', async () => {
            const body = new URLSearchParams();
            body.set('device', String(index));
            body.set('band', String(device.manualBand || 1));
            body.set('channel', String(device.manualChannel || 1));
            body.set('power', String(device.manualPowerIndex || 0));
            const response = await fetch('/api/control', { method: 'POST', body });
            const data = await response.json();
            setStatus(data.message || 'Ручове керування виконано', data.ok ? 'Застосовано' : 'Помилка керування');
            if (data.ok) await loadState();
          });
        }
        if (removeButton) {
          removeButton.addEventListener('click', () => {
            devicesState.splice(index, 1);
            if (devicesState.length === 0) devicesState.push(createDevice());
            renderDeviceList();
            renderJsonDeviceSelect();
          });
        }
      });
      renderJsonDeviceSelect();
    }

    function applyLiveState(state) {
      chips.innerHTML = '';
      [
        `SSID ${state.ssid}`,
        `IP ${state.ip}`,
        `Роль ${state.boardRole || 'standalone'}`,
        `MAVLink SysID ${state.localNodeId || 1}`,
        `Пристрої ${state.deviceCount || devicesState.length}`,
        `ESP-NOW ${state.espNowEnabled ? 'вкл' : 'викл'}`,
        `MAV ${Number(state.mavlinkDebug?.rxByteCount || 0)}B / ${Number(state.mavlinkDebug?.parsedCommandCount || 0)} cmd`
      ].forEach((text) => {
        const chip = document.createElement('div');
        chip.className = 'chip';
        chip.textContent = text;
        chips.appendChild(chip);
      });

      setStatus(`Роль: ${state.boardRole || 'standalone'}\nMAVLink System ID: ${state.localNodeId || 1}\nНалаштовано пристроїв: ${state.deviceCount || devicesState.length}\nТранспорт готовий: ${state.transportReady ? 'так' : 'ні'}\n${formatMavlinkDebug(state.mavlinkDebug)}\nКожен пристрій має власну таблицю VTX.`);
      restartPending = Boolean(state.restartPending);
      if (restartPending) {
        setStatus(`Налаштовано пристроїв: ${state.deviceCount || devicesState.length}\nТранспорт готовий: ${state.transportReady ? 'так' : 'ні'}\nОчікується перезапуск; зачекайте, поки плата перезапуститься перед використанням серійного керування.`, 'Перезапуск');
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

        const liveState = document.querySelector(`[data-role="live-state"][data-device="${index}"]`) || document.querySelector(`td[data-device="${index}"] [data-role="live-state"]`);
        if (liveState) {
          liveState.textContent = describeDeviceLiveState(devicesState[index]);
          if (devicesState[index].activeControlMode === 'mavlink') {
            liveState.innerHTML = `${describeDeviceLiveState(devicesState[index])}<br><span style="font-size:0.78rem;opacity:0.8;">Локальний стан після застосування, не пряме telemetry/read-back.</span>`;
          }
        }
      });
    }

    function setStatus(message, strong) {
      stateText.innerHTML = strong ? `<strong>${strong}</strong>\n${message}` : message;
    }

    function fillState(state) {
      for (const [key, value] of Object.entries({
        wifiChannel: state.wifiChannel,
        espNowEnabled: state.espNowEnabled ? '1' : '0',
        boardRole: state.boardRole || 'standalone',
        localNodeId: state.localNodeId ?? 1,
        mavlinkRxPin: state.mavlinkRxPin ?? -1,
        mavlinkTxPin: state.mavlinkTxPin ?? -1,
        mavlinkBaud: state.mavlinkBaud ?? 115200
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
        throw new Error('Не вдалося завантажити стан');
      }
      const data = await response.json();
      fillState(data);
      return data;
    }

    async function pollLiveState() {
      try {
        const response = await fetch('/api/state');
        if (!response.ok) {
          throw new Error('Не вдалося оновити стан');
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
      setStatus(`Завантажено ${data.path || 'таблицю пристрою'} у редактор`, 'JSON готово');
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
      setStatus(data.message || 'Оброблено збереження JSON', data.ok ? 'JSON збережено' : 'Не вдалося зберегти JSON');
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
      body.set('boardRole', configForm.elements.namedItem('boardRole').value);
      body.set('localNodeId', configForm.elements.namedItem('localNodeId').value);
      body.set('mavlinkRxPin', configForm.elements.namedItem('mavlinkRxPin').value);
      body.set('mavlinkTxPin', configForm.elements.namedItem('mavlinkTxPin').value);
      body.set('mavlinkBaud', configForm.elements.namedItem('mavlinkBaud').value);
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
        mavlinkNodeId: Number(device.mavlinkNodeId || 1),
        mavlinkDeviceId: Number(device.mavlinkDeviceId || 1),
        vtxTablePath: device.vtxTablePath
      }))));
      const response = await fetch('/api/config', { method: 'POST', body });
      const data = await response.json();
      setStatus(data.message || 'Конфігурацію збережено', data.restartRequired ? 'Перезапуск' : 'Збережено');
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
      setStatus(data.message || 'Завантаження оброблено', data.ok ? 'Завантаження завершено' : 'Завантаження не вдалося');
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
        pwmInputPin: 3 + devicesState.length,
        vtxControlPin: 4 + devicesState.length,
        protocol: 'smartaudio',
        controlMode: 'pwm',
        enabled: true,
        mavlinkNodeId: Number(configForm.elements.namedItem('localNodeId')?.value || 1),
        mavlinkDeviceId: devicesState.length + 1,
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
      setStatus(error.message || String(error), 'Помилка');
    });
  </script>
</body>
</html>
)html";
