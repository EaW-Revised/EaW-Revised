"""Aggregate a retail frame-timer capture or a viewer perf trace for the melee parity doc (#1262).

    python tools/performance/melee_parity_analyze.py retail <capture dir> [out.json]
    python tools/performance/melee_parity_analyze.py viewer <perf trace csv> [out.json]

`retail` reads the identity-gated timer's capture.json and records.bin (intervals of the loop,
logical, render-entry and present boundaries). `viewer` windows the --eawr-perf-trace rows by tick.
Output is aggregate only; raw captures stay under ignored out/.
"""
import csv
import json
from pathlib import Path
import statistics
import struct
import sys

def stats(values):
    s = sorted(values)
    if not s: return None
    def p(q):
        k=(len(s)-1)*q; lo=int(k); hi=min(lo+1,len(s)-1)
        return s[lo]+(s[hi]-s[lo])*(k-lo)
    return dict(n=len(s),mean=statistics.fmean(s),p50=p(.5),p95=p(.95),p99=p(.99),max=max(s))

# Phase k runs from boundary k to boundary k+1 of the timer's frame; the last closes on the next
# frame's boundary 0. Names follow the timer's own phase table.
PHASES = ['frameOpen','preAudio','audioMix','sfxUpdate','audioTail','uiAndMovie','preAdvance',
          'advanceWait','preSimulation','simEntry','simPre','objectManagerSync',
          'objectManagerNonSyncTail','aiAndPlayers','endFrameAndCrc','particlesAndScene',
          'renderAndUi','presentAccount','tailAndPump']
SIMULATION = ('advanceWait','preSimulation','simEntry','simPre','objectManagerSync',
              'objectManagerNonSyncTail','aiAndPlayers','endFrameAndCrc')

def phases(rows,frequency):
    """Mean ms of each phase over the frames that recorded both of its boundaries, and the
    simulation block (advanceWait to endFrameAndCrc) as the sum of those means."""
    starts=[i for i,r in enumerate(rows) if r[0]==0]
    spans={}
    for first,last in zip(starts,starts[1:]):
        frame=rows[first:last+1]
        if any(b[0]<=a[0] for a,b in zip(frame[:-2],frame[1:-1])): continue
        for a,b in zip(frame,frame[1:]):
            tail=b is frame[-1]
            if (b[0]==a[0]+1 and not tail) or (tail and a[0]==len(PHASES)-1):
                spans.setdefault(PHASES[a[0]],[]).append((b[4]-a[4])/frequency*1000)
    if len(spans)<2: return None
    means={k:dict(frames=len(v),mean_ms=statistics.fmean(v)) for k,v in spans.items()}
    return dict(per_phase=means,simulation_block_ms=sum(means[k]['mean_ms'] for k in SIMULATION if k in means))

def retail(directory):
    meta=json.loads((directory/'capture.json').read_text())
    assert meta['state']=='measured' and not meta['errors'] and meta['stopped']['allRestored']
    frequency=meta['started']['clock']['ticksPerSecond']
    records=list(struct.iter_unpack('<4IQ',(directory/'records.bin').read_bytes()))
    result=dict(state=meta['state'],restored=True,arms=[])
    for arm in meta['arms']:
        assert not any(arm[k] for k in ('recordDropped','frameDropped','foreignThreadCalls','outOfOrderTotal'))
        rows=records[arm['firstRecord']:arm['lastRecord']]
        record=dict(name=arm['arm'],wall_seconds=arm['wallSeconds'])
        for boundary,name in ((0,'loop'),(8,'logical'),(16,'render_entry'),(17,'present_account')):
            times=[r[4]/frequency for r in rows if r[0]==boundary]
            deltas=[(b-a)*1000 for a,b in zip(times,times[1:])]
            record[name]=dict(interval_ms=stats(deltas),rate=(len(times)-1)/(times[-1]-times[0]) if len(times)>1 else None)
        logical_frames=[r[1] for r in rows if r[0]==11]
        record['logical_frame_first_last']=[logical_frames[0],logical_frames[-1]] if logical_frames else None
        record['logical_frame_gap_counts']={str(g):sum(b-a==g for a,b in zip(logical_frames,logical_frames[1:])) for g in set(b-a for a,b in zip(logical_frames,logical_frames[1:]))}
        record['phases']=phases(rows,frequency)
        result['arms'].append(record)
    return result

def viewer(path):
    rows=list(csv.DictReader(path.open()))
    result=dict(rows=len(rows),windows=[])
    for lo,hi in ((1,300),(300,600),(600,900),(900,1200),(1200,1800),(1800,2400),(1,2400),(1320,2220),(1320,1770),(1770,2220)):
        w=[r for r in rows if lo<=float(r['tick'])<hi and float(r['wall_frame_ms'])>0]
        if not w: continue
        # Keep the measured stage columns, including opt-in particle CPU sums.
        # CPU sums can overlap caller elapsed intervals; they are not additive
        # parts of particle_ms. Older traces simply omit newer stage columns.
        timing={k:stats([float(r[k]) for r in w]) for k in rows[0] if k.endswith('_ms')}
        seconds=sum(float(r['wall_frame_ms']) for r in w[1:])/1000
        ticks=sum(float(r['ticks']) for r in w)
        result['windows'].append(dict(tick_window=[lo,hi],samples=len(w),timing_ms=timing,
            tick_cost_mean_ms=sum(float(r['tick_ms']) for r in w)/ticks if ticks else None,
            presented_ticks_per_second=(float(w[-1]['tick'])-float(w[0]['tick']))/seconds if seconds else None,
            units_first_last=[w[0]['units'],w[-1]['units']],projectiles_max=max(int(r['projectiles']) for r in w)))
    return result

if __name__=='__main__':
    source=Path(sys.argv[2]); result=retail(source) if sys.argv[1]=='retail' else viewer(source)
    output=json.dumps(result,indent=2)+'\n'
    if len(sys.argv)>3: Path(sys.argv[3]).write_text(output)
    else: print(output)
