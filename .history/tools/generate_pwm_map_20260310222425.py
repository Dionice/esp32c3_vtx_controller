#!/usr/bin/env python3
from math import floor

# Mirror arrays from src/main.cpp
bands = [1,2,3,4]
channels = [1,2,3,4,5,6,7,8]
powers = [0,1,2,3]
pitModes = [0,1]

total = len(bands)*len(channels)*len(powers)*len(pitModes)

def idx_from_pulse(pulse):
    # Arduino map: idx = (pulse-500)*(total-1)/(2000)
    return int((pulse - 500) * (total - 1) / 2000)

# Build mapping for each discrete index by enumerating pulses 500..2500
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
    pitIdx = rem % len(pitModes); rem //= len(pitModes)
    powIdx = rem % len(powers); rem //= len(powers)
    chanIdx = rem % len(channels); rem //= len(channels)
    bandIdx = rem % len(bands)
    band = bands[bandIdx]
    chan = channels[chanIdx]
    power = powers[powIdx]
    pit = pitModes[pitIdx]
    rows.append((idx, band, chan, power, pit, lb, ub, center))

out_path = 'data/pwm_to_vtx_map.csv'
with open(out_path, 'w') as f:
    f.write('idx,band,channel,power,pit,lb_us,ub_us,center_us\n')
    for r in rows:
        f.write(','.join(str(x) if x is not None else '' for x in r) + '\n')

print(f'Wrote {out_path} ({len(rows)} entries)')
print('\nSample lines:')
with open(out_path,'r') as f:
    for _ in range(1+20):
        line = f.readline()
        if not line: break
        print(line.strip())
