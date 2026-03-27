#!/usr/bin/env python3
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VTX_TABLE_PATH = ROOT / "data" / "peak_thor_t35.json"
OUTPUT_PATH = ROOT / "data" / "pwm_to_vtx_map.csv"

with VTX_TABLE_PATH.open("r", encoding="utf-8") as f:
    vtx_table = json.load(f)["vtx_table"]

bands = vtx_table["bands_list"]
power_levels = vtx_table["powerlevels_list"]
pit_modes = [0, 1]

channel_count = len(bands[0]["frequencies"])
if any(len(band["frequencies"]) != channel_count for band in bands):
    raise SystemExit("All bands must have the same number of channels")

total = len(bands) * channel_count * len(power_levels) * len(pit_modes)

def idx_from_pulse(pulse):
    # Arduino map: idx = (pulse-500)*(total-1)/(2000)
    return int((pulse - 500) * (total - 1) / 2000)

# Build mapping for each discrete index by enumerating pulses 500..2500.
pulse_to_idx = {}
for p in range(500, 2501):
    i = idx_from_pulse(p)
    pulse_to_idx.setdefault(i, []).append(p)

rows = []
for idx in range(total):
    pulses = pulse_to_idx.get(idx, [])
    if pulses:
        lb = pulses[0]
        ub = pulses[-1]
        center = (lb + ub) // 2
    else:
        lb = ub = center = None
    rem = idx
    pitIdx = rem % len(pit_modes); rem //= len(pit_modes)
    powIdx = rem % len(power_levels); rem //= len(power_levels)
    chanIdx = rem % channel_count; rem //= channel_count
    bandIdx = rem % len(bands)
    band = bands[bandIdx]
    power = power_levels[powIdx]
    frequency = band["frequencies"][chanIdx]
    rows.append((
        idx,
        bandIdx + 1,
        band.get("name", ""),
        band.get("letter", ""),
        chanIdx + 1,
        frequency,
        power["value"],
        power.get("label", "").strip(),
        pit_modes[pitIdx],
        lb,
        ub,
        center,
    ))

with OUTPUT_PATH.open("w", encoding="utf-8", newline="") as f:
    f.write("idx,band_index,band_name,band_letter,channel,frequency,power_value,power_label,pit,lb_us,ub_us,center_us\n")
    for r in rows:
        f.write(",".join(f'\"{x}\"' if isinstance(x, str) else (str(x) if x is not None else "") for x in r) + "\n")

print(f"Wrote {OUTPUT_PATH} ({len(rows)} entries)")
print("\nSample lines:")
with OUTPUT_PATH.open("r", encoding="utf-8") as f:
    for _ in range(1+20):
        line = f.readline()
        if not line: break
        print(line.strip())
