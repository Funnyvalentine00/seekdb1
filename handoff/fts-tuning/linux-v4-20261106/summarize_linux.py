from pathlib import Path
import hashlib
import json
import re
import statistics

R = Path(__file__).resolve().parent
O = R / 'collected' / 'results'
KEYS = ['build_ik_all_sec', 'build_ik_content_sec', 'build_beng_en_sec',
        'tokenize_ik_avg_ms', 'tokenize_beng_avg_ms', 'query_cn_avg_ms',
        'query_beng_avg_ms', 'query_mixed_avg_ms', 'query_limit_avg_ms']
EXPECTED = dict(query_cn_hits=8001, query_beng_hits=11000,
                query_mixed_hits=7332, query_limit_hits=20)


def load(tag):
    text = (O / (tag + '.txt')).read_text()
    value = {m[1]: float(m[2]) for m in re.finditer(
        r'^([A-Za-z0-9_]+):\s*([0-9.]+)\s*$', text, re.M)}
    assert all(value[k] == v for k, v in EXPECTED.items()), tag
    return value


def stats(values):
    return dict(samples=values, mean=statistics.mean(values),
                stdev=statistics.stdev(values), median=statistics.median(values))


pairs = [{v: load(f'ab{i}-{v}') for v in ['v3', 'fixed']} for i in range(1, 7)]
wall = {}
for key in KEYS + ['build_total_sec']:
    data = {v: [p[v][key] for p in pairs] for v in ['v3', 'fixed']}
    improvements = [100 * (1 - p['fixed'][key] / p['v3'][key]) for p in pairs]
    wall[key] = dict(statistics={v: stats(x) for v, x in data.items()},
                     paired_improvement_pct=improvements,
                     median_improvement_pct=statistics.median(improvements),
                     ratio_of_medians_improvement_pct=100 * (
                         1 - statistics.median(data['fixed']) / statistics.median(data['v3'])))
aa = {v: load('aa-v3-' + v) for v in ['a', 'b']}
aa_change = {k: 100 * (1 - aa['b'][k] / aa['a'][k]) for k in KEYS}
for variant in ['v3', 'fixed']:
    load('check-' + variant)
accept = json.loads((O / 'acceptance-summary.json').read_text())
semantics_equal = accept['v3']['semantics'] == accept['fixed']['semantics']
edges_equal = all(accept['v3']['edges'][case][key] == accept['fixed']['edges'][case][key]
                  for case in accept['v3']['edges'] for key in ['rc', 'stdout_sha256', 'stderr_sha256'])
regress = {v: {s: {'passed': sum(r['rc'] == 0 for r in rows), 'total': len(rows),
                   'failures': [r['case'] for r in rows if r['rc'] != 0], 'cases': rows}
                for s, rows in accept[v]['regression'].items()} for v in ['v3', 'fixed']}
isolated_ai = json.loads((O / 'ai-isolated-native-summary.json').read_text())
tokenize = json.loads((O / 'tokenize-statements/summary.json').read_text())
tokenize_equal = tokenize['v3'] == tokenize['fixed']
boundary_success = all(r['rc'] == 0 for v in ['v3', 'fixed'] for r in accept[v]['edges'].values())
regression_gate = all(
    all(r['rc'] == 0 or (s == 'fts_index' and r['case'] == 'partitioned_simple_query')
        for r in accept[v]['regression'][s])
    for v in ['v3', 'fixed'] for s in ['fts_index', 'file_sql', 'ai_column'])
rss = {v: [json.loads((O / f'ab{i}-{v}-rss.json').read_text())['VmHWM']
           for i in range(1, 7)] for v in ['v3', 'fixed']}
rss_delta = 100 * (statistics.median(rss['fixed']) / statistics.median(rss['v3']) - 1)
failures = [k for k in KEYS if wall[k]['median_improvement_pct'] < -1]
result = dict(platform='Linux x86_64 WSL2', pairs=6, order='3 v3-first, 3 fixed-first',
              baseline_commit='7accd1ace8ee5d035b3ef8aaf426c46b1d0a595e',
              candidate_code_commit='194150497aca9a69e395317bd6d5a64f26340a61',
              expected_hits=EXPECTED, all_hits_exact=True, benchmark_runs_with_exact_hits=16, wall=wall,
              benchmark_parameters=dict(ROWS=20000, BATCH=500, ROUNDS=3000, QUERY_ROUNDS=200, SAMPLES=3, WARMUP=30, SKIP_LOAD=0),
              query_median_improvements_mean_pct=statistics.mean(wall[k]['median_improvement_pct'] for k in KEYS if k.startswith('query_')),
              aa=dict(raw=aa, improvement_pct=aa_change),
              semantics_equal=semantics_equal, semantics_records=accept['v3']['semantics'],
              original_tokenize_coverage_note='Original batch stopped at line 6 NULL error; later SELECTs were not exercised in that batch. Per-statement supplemental results cover all original SELECTs.',
              tokenize_statements=tokenize, tokenize_statements_equal=tokenize_equal,
              boundary_outputs_equal=edges_equal, boundary_success=boundary_success, boundary_cases_per_version=len(accept['v3']['edges']),
              regression=regress, isolated_ai_retest=isolated_ai,
              isolated_ai_retest_passed=all(r['rc'] == 0 for rows in isolated_ai.values() for r in rows),
              initial_ai_failure_root_cause='Unproven; original failure retained; inline --real_sleep parsing hypothesis ruled out.',
              strict_regression_gate_passed=regression_gate,
              nine_metric_gate_passed=not failures, metric_gate_failures=failures,
              peak_rss_bytes=rss, peak_rss_statistics={k: stats(v) for k, v in rss.items()}, rss_median_change_pct=rss_delta,
              gates=dict(max_nine_metric_slowdown_pct=1, rss_increase_allowed=False, fts_required='14/15; only partitioned_simple_query excepted'),
              binary_sha256=json.loads((R/'collected/logs/build.complete.json').read_text()),
              toolchain=json.loads((R/'collected/logs/recovered-toolchain.json').read_text()),
              resource_policy=json.loads((R/'resource-policy.json').read_text()),
              final_resources=json.loads((R/'collected/logs/final-resource-state-after-tokenize.json').read_text()),
              interruption_marker_regression=json.loads((R/'interruption-marker-regression.json').read_text()),
              hardware_counters='Not collected: matching WSL kernel perf tool unavailable.',
              full_acceptance=not failures and regression_gate and semantics_equal and edges_equal and boundary_success and tokenize_equal and rss_delta <= 0,
              scope='Local Linux acceptance only; no official CI score inferred.')
(R / 'linux-acceptance-evidence.json').write_text(json.dumps(result, indent=2))
print(json.dumps({k: v['median_improvement_pct'] for k, v in wall.items()}, indent=2))
print('metric failures:', failures, 'regression gate:', regression_gate, 'RSS median change:', rss_delta)
print('full_acceptance:', result['full_acceptance'])
