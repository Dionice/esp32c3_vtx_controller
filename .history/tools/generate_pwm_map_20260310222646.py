#!/usr/bin/env python3
from math import floor

# Read VTX JSON from data directory
import json
with open('data/peak_thor_t35.json','r', encoding='utf-8') as jf:
    j = json.load(jf)

bands_list = j['vtx_table']['bands_list']
powerlevels = j['vtx_table']['powerlevels_list']
pitModes = [0,1]

# validate bands have equal channel counts
channel_counts = [len(b['frequencies']) for b in bands_list]
if len(set(channel_counts)) != 1:
    raise SystemExit('bands have differing channel counts; script expects uniform count')

bands = bands_list
channels = list(range(1, channel_counts[0]+1))
powers = [p['value'] for p in powerlevels]

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
        freq = band['frequencies'][chanIdx]
        band_name = band.get('name','')
        band_letter = band.get('letter','')
        power_label = next((p['label'] for p in powerlevels if p['value']==power), '')
        rows.append((idx, bandIdx+1, band_name, band_letter, chan, freq, power, power_label, pit, lb, ub, center))

out_path = 'data/pwm_to_vtx_map.csv'
with open(out_path, 'w', encoding='utf-8') as f:
    f.write('idx,band_index,band_name,band_letter,channel_index,frequency_khz,power_value,power_label,pit,lb_us,ub_us,center_us\n')
    for r in rows:
        f.write(','.join('"%s"' % x if isinstance(x,str) else (str(x) if x is not None else '') for x in r) + '\n')

print(f'Wrote {out_path} ({len(rows)} entries)')
print('\nSample lines:')
with open(out_path,'r') as f:
    for _ in range(1+20):
        line = f.readline()
        if not line: break
        print(line.strip())
