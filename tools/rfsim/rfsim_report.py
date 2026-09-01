#!/usr/bin/env python3
"""Turn one rfsim_multi_ue.sh run into a summary table and a per-UE CSV row set.

Reads the iperf3 json the sweep left in /tmp, so it works after the tmux session is
gone. The CSV is appended across runs and keyed by run_id, so a directory of runs can be
loaded straight into a dataframe.
"""
import csv
import datetime
import json
import os
import subprocess
import sys

IMSI_FMT = os.environ.get('IMSI_FMT', '00101%010d')
IMSI_BASE = int(os.environ.get('IMSI_BASE', '0'))

def load(direction, i):
    """Parse one iperf3 result.

    iperf3 can prefix stdout with a WARNING line, and at a low real-time factor its
    final results exchange times out and leaves "end" empty. Parse leniently and fall
    back to summing the interval samples, which the client already received.
    """
    path = f'/tmp/rfsim_{direction}_{i}.json'
    try:
        raw = open(path).read()
    except OSError:
        return None
    a, b = raw.find('{'), raw.rfind('}')
    if a < 0 or b < 0:
        return None
    try:
        d = json.loads(raw[a:b + 1])
    except Exception:
        return None
    key = 'sum_received' if direction == 'dl' else 'sum_sent'
    end = d.get('end', {}).get(key)
    if end and end.get('seconds'):
        return dict(mbps=end['bits_per_second'] / 1e6, nbytes=end['bytes'],
                    seconds=end['seconds'],
                    retr=d['end'].get('sum_sent', {}).get('retransmits', ''),
                    src='end')
    iv = d.get('intervals') or []
    if not iv:
        return None
    total = sum(x['sum']['bytes'] for x in iv)
    span = iv[-1]['sum']['end'] - iv[0]['sum']['start']
    if span <= 0:
        return None
    return dict(mbps=total * 8 / span / 1e6, nbytes=total, seconds=span, retr='',
                src='intervals')

def _num(tok):
    """Parse a scheduler metric value: 107Mbps, 846kbps, 1.66M, 2.01k, 4%, 260ns, n/a."""
    if tok in ('n/a', '', 'nan'):
        return None
    t = tok.rstrip('%')
    mult = 1.0
    for suf, m in (('Mbps', 1e6), ('kbps', 1e3), ('bps', 1.0),
                   ('us', 1e-6), ('ms', 1e-3), ('ns', 1e-9),
                   ('M', 1e6), ('k', 1e3)):
        if t.endswith(suf):
            t, mult = t[:-len(suf)], m
            break
    try:
        return float(t) * mult
    except ValueError:
        return None

def parse_gnb_metrics(path, t_start, t_end):
    """Aggregate per-UE scheduler metrics over the traffic window.

    The counters are per reporting period, not cumulative, so occupancy counts are
    summed and quality figures averaged. Restricting to [t_start, t_end] keeps idle
    samples before and after the sweep out of the averages.
    """
    import re
    per_ue = {}
    try:
        fh = open(path, 'rb')
    except OSError:
        return {}
    with fh:
        for raw in fh:
            if b'Scheduler UE' not in raw:
                continue
            line = raw.decode('utf-8', 'replace')
            m = re.match(r'(\S+)\s', line)
            if not m:
                continue
            ts = m.group(1)
            if t_start and (ts < t_start or ts > t_end):
                continue
            fields = dict(re.findall(r'([a-z_0-9]+)=(\S+)', line))
            if 'rnti' not in fields or 'ue' not in fields:
                continue
            try:
                du_ue = int(fields['ue'])
            except ValueError:
                continue
            if not (_num(fields.get('dl_brate', '0')) or _num(fields.get('ul_brate', '0'))):
                continue
            acc = per_ue.setdefault(du_ue, {'rnti': fields.get('rnti', ''), 'n': 0,
                                            'sum': {}, 'cnt': {}})
            acc['n'] += 1
            acc['rnti'] = fields.get('rnti', acc['rnti'])
            for k, v in fields.items():
                if k in ('ue', 'pci', 'rnti'):
                    continue
                val = _num(v)
                if val is None:
                    continue
                acc['sum'][k] = acc['sum'].get(k, 0.0) + val
                acc['cnt'][k] = acc['cnt'].get(k, 0) + 1

    SUMMED = {'dl_nof_ok', 'dl_nof_nok', 'ul_nof_ok', 'ul_nof_nok', 'dl_nof_prbs',
              'ul_nof_prbs', 'sr_count', 'f0f1_invalid_harqs', 'f2f3f4_invalid_harqs',
              'f2f3f4_invalid_csis', 'pusch_invalid_harqs', 'pusch_invalid_csis'}
    out = {}
    for du_ue, acc in per_ue.items():
        r = {'du_ue_id': du_ue, 'rnti': acc['rnti'], 'metric_samples': acc['n']}
        for k, tot in acc['sum'].items():
            r[k] = round(tot, 4) if k in SUMMED else round(tot / acc['cnt'][k], 4)
        dok, dnok = r.get('dl_nof_ok', 0), r.get('dl_nof_nok', 0)
        uok, unok = r.get('ul_nof_ok', 0), r.get('ul_nof_nok', 0)
        r['dl_harq_nack_pct'] = round(dnok / (dok + dnok) * 100, 3) if (dok + dnok) else ''
        r['ul_harq_nack_pct'] = round(unok / (uok + unok) * 100, 3) if (uok + unok) else ''
        out[du_ue] = r
    return out

def ue_ip(i):
    try:
        out = subprocess.run(
            ['ip', 'netns', 'exec', f'ue{i}', 'ip', '-4', '-o', 'addr', 'show', 'oaitun_ue1'],
            capture_output=True, text=True, timeout=5).stdout
        return out.split()[3].split('/')[0] if out.strip() else ''
    except Exception:
        return ''

def expand_cpus(spec):
    cores = []
    for part in filter(None, spec.split(',')):
        if '-' in part:
            lo, hi = part.split('-')
            cores += list(range(int(lo), int(hi) + 1))
        else:
            cores.append(int(part))
    return cores

def write_report_csv(path, rows, run_id, bw, prb, n, ue_cpus, dur, stagger, rt, rtt):
    """Human-facing spreadsheet: banner, per-UE table, then the aggregates.

    CSV carries no formatting, so "bold" headers are not possible here - open this in a
    spreadsheet and the header row is row 5, ready to freeze or style. The machine
    readable table is the other file.
    """
    def agg(key, only_positive=True):
        v = [r[key] for r in rows if r.get(key) not in ('', None)]
        if only_positive:
            v = [x for x in v if x > 0.05]
        return v

    def line(vals):
        return [('' if v is None else v) for v in vals]

    hdr = list(rows[0].keys())
    with open(path, 'a', newline='') as f:
        w = csv.writer(f)
        w.writerow([f'UE METRIC SUMMARY - {os.path.basename(path)}'])
        w.writerow([f'run {run_id}', f'{bw} MHz / {prb} PRB', f'N={n}',
                    f'ue_cpus={ue_cpus or "unpinned"}', f'iperf={dur}s',
                    f'stagger={stagger}s', f'rt={rt or "n/a"}', f'rtt_ms={rtt or "n/a"}'])
        w.writerow([])
        w.writerow([h.upper() for h in hdr])
        for r in rows:
            w.writerow(line([r[h] for h in hdr]))

        w.writerow([])
        w.writerow(['SUMMARY'])
        w.writerow(['metric', 'count', 'aggregate', 'mean', 'min', 'max', 'spread_pct'])

        def summarise(label, key):
            v = agg(key)
            if not v:
                w.writerow([label, 0, '', '', '', '', ''])
                return
            lo, hi = min(v), max(v)
            mean = sum(v) / len(v)
            sp = (hi - lo) / abs(mean) * 100 if mean else 0.0
            w.writerow([label, len(v), round(sum(v), 3), round(mean, 4),
                        round(lo, 4), round(hi, 4), round(sp, 2)])

        summarise('dl_mbps', 'dl_mbps')
        summarise('ul_mbps', 'ul_mbps')
        for key in ('cqi', 'dl_mcs', 'ul_mcs', 'dl_error_rate', 'ul_error_rate',
                    'dl_harq_nack_pct', 'ul_harq_nack_pct', 'pusch_snr_db',
                    'pusch_rsrp_db', 'dl_olla', 'ul_olla'):
            v = [r[key] for r in rows if r.get(key) not in ('', None)]
            if not v:
                continue
            lo, hi = min(v), max(v)
            mean = sum(v) / len(v)
            sp = (hi - lo) / abs(mean) * 100 if mean else 0.0
            w.writerow([key, len(v), '', round(mean, 4),
                        round(lo, 4), round(hi, 4), round(sp, 2)])

        w.writerow([])
        w.writerow(['attached', sum(r['attached'] for r in rows), f'of {n}'])
        if rt:
            try:
                w.writerow(['sim_time_capacity_mbps',
                            round(sum(agg('dl_mbps')) / float(rt), 2)])
            except (ValueError, ZeroDivisionError):
                pass

def main():
    (n, bw, prb, ssb, band, freq, ue_cpus, dur, stagger, rt, rtt,
     out_txt, out_csv, run_id) = sys.argv[1:15]
    gnb_log = sys.argv[15] if len(sys.argv) > 15 else '/tmp/gnb.log'
    t_start = sys.argv[16] if len(sys.argv) > 16 else ''
    t_end = sys.argv[17] if len(sys.argv) > 17 else ''
    n = int(n)
    sched = parse_gnb_metrics(gnb_log, t_start, t_end)
    cores = expand_cpus(ue_cpus)
    now = datetime.datetime.now().isoformat(timespec='seconds')

    rows = []
    for i in range(1, n + 1):
        dl, ul = load('dl', i), load('ul', i)
        ip = ue_ip(i)
        rows.append({
            'run_id': run_id, 'ts': now, 'n_ues': n, 'bandwidth_mhz': bw, 'prb': prb,
            'ssb': ssb, 'band': band, 'freq_hz': freq, 'ue_cpus': ue_cpus,
            'stagger_s': stagger, 'iperf_s': dur, 'rt': rt, 'rtt_ms': rtt,
            'ue': i, 'imsi': IMSI_FMT % (IMSI_BASE + i), 'tunnel_ip': ip,
            'ue_core': cores[(i - 1) % len(cores)] if cores else '',
            'attached': int(bool(ip)),
            'dl_mbps': round(dl['mbps'], 4) if dl else '',
            'dl_bytes': dl['nbytes'] if dl else '',
            'dl_seconds': round(dl['seconds'], 2) if dl else '',
            'dl_retransmits': dl['retr'] if dl else '',
            'dl_source': dl['src'] if dl else 'missing',
            'ul_mbps': round(ul['mbps'], 4) if ul else '',
            'ul_bytes': ul['nbytes'] if ul else '',
            'ul_seconds': round(ul['seconds'], 2) if ul else '',
            'ul_retransmits': ul['retr'] if ul else '',
            'ul_source': ul['src'] if ul else 'missing',
        })
        radio = sched.get(i - 1, {})
        for col in ('du_ue_id', 'rnti', 'metric_samples', 'cqi', 'dl_ri', 'dl_mcs',
                    'dl_brate', 'dl_nof_ok', 'dl_nof_nok', 'dl_error_rate',
                    'dl_harq_nack_pct', 'dl_nof_prbs', 'dl_olla',
                    'pusch_snr_db', 'pusch_rsrp_db',
                    'ul_ri', 'ul_mcs', 'ul_brate', 'ul_nof_ok', 'ul_nof_nok',
                    'ul_error_rate', 'ul_harq_nack_pct', 'ul_nof_prbs', 'ul_olla',
                    'bsr', 'sr_count', 'ta', 'last_phr',
                    'f0f1_invalid_harqs', 'f2f3f4_invalid_harqs', 'f2f3f4_invalid_csis',
                    'pusch_invalid_harqs', 'pusch_invalid_csis',
                    'avg_crc_delay', 'avg_pusch_harq_delay', 'avg_ul_ce_delay'):
            rows[-1][col] = radio.get(col, '')

    write_report_csv(out_csv, rows, run_id, bw, prb, n, ue_cpus, dur, stagger, rt, rtt)

    def stats(key):
        v = [r[key] for r in rows if r[key] != '' and r[key] > 0.05]
        if not v:
            return 0, 0.0, 0.0, 0.0, 0.0
        return len(v), sum(v), sum(v) / len(v), min(v), max(v)

    def spread(lo, hi):
        return (hi - lo) / ((hi + lo) / 2) * 100 if hi > 0 else 0.0

    def fmt(v, w, p=3):
        return f'{v:>{w}.{p}f}' if v != '' else f'{"-":>{w}}'

    out = []
    out.append(f'run {run_id}   {bw} MHz / {prb} PRB   N={n}   '
               f'ue_cpus={ue_cpus or "unpinned"}   iperf={dur}s   stagger={stagger}s')
    out.append('')
    out.append(f'  {"ue":<4}{"tunnel":<17}{"rnti":<8}{"DL Mbps":>9}{"UL Mbps":>9}'
               f'{"cqi":>5}{"dlMCS":>7}{"ulMCS":>7}{"dlBLER%":>9}{"nack%":>7}'
               f'{"snr":>7}')
    for r in rows:
        out.append(f'  {r["ue"]:<4}{r["tunnel_ip"] or "-":<17}{str(r["rnti"] or "-"):<8}'
                   f'{fmt(r["dl_mbps"], 9)}{fmt(r["ul_mbps"], 9)}'
                   f'{fmt(r["cqi"], 5, 1)}{fmt(r["dl_mcs"], 7, 1)}{fmt(r["ul_mcs"], 7, 1)}'
                   f'{fmt(r["dl_error_rate"], 9, 2)}{fmt(r["dl_harq_nack_pct"], 7, 2)}'
                   f'{fmt(r["pusch_snr_db"], 7, 1)}')

    att = sum(r['attached'] for r in rows)
    dn, dtot, davg, dlo, dhi = stats('dl_mbps')
    un, utot, uavg, ulo, uhi = stats('ul_mbps')
    out.append('')
    out.append(f'  attached      {att}/{n}')
    out.append(f'  DL  carrying {dn:<3}  aggregate {dtot:8.2f} Mbps   per UE {davg:7.3f}   '
               f'(min {dlo:.3f} max {dhi:.3f}, spread {spread(dlo, dhi):.1f}%)')
    out.append(f'  UL  carrying {un:<3}  aggregate {utot:8.2f} Mbps   per UE {uavg:7.3f}   '
               f'(min {ulo:.3f} max {uhi:.3f}, spread {spread(ulo, uhi):.1f}%)')
    def avg(key):
        v = [r[key] for r in rows if r.get(key) not in ('', None)]
        return sum(v) / len(v) if v else None

    cell = []
    for label, key, p in (('cqi', 'cqi', 1), ('dl_mcs', 'dl_mcs', 1),
                          ('ul_mcs', 'ul_mcs', 1), ('dl_bler%', 'dl_error_rate', 2),
                          ('ul_bler%', 'ul_error_rate', 2),
                          ('dl_nack%', 'dl_harq_nack_pct', 2),
                          ('ul_nack%', 'ul_harq_nack_pct', 2),
                          ('pusch_snr', 'pusch_snr_db', 1)):
        a = avg(key)
        if a is not None:
            cell.append(f'{label} {a:.{p}f}')
    if cell:
        out.append('  radio (mean over UEs): ' + '  '.join(cell))
        matched = sum(1 for r in rows if r.get('rnti'))
        out.append(f'  radio matched {matched}/{att} attached UEs by DU admission order'
                   f'  (check rnti column if a UE re-attached)')
    out.append(f'  real-time factor {rt or "n/a"}      tunnel rtt {rtt or "n/a"} ms')
    if rt:
        try:
            out.append(f'  simulated-time capacity (aggDL / rt) {dtot / float(rt):.1f} Mbps')
        except (ValueError, ZeroDivisionError):
            pass

    text = '\n'.join(out) + '\n'
    open(out_txt, 'w').write(text)
    print()
    print(text)

if __name__ == '__main__':
    main()
