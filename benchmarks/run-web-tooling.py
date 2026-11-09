#!/usr/bin/env python3
"""Compare existing txiki binaries against the classic Web Tooling bundle."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import re
from statistics import median
import subprocess
import time


SCRIPT = (
    'globalThis.eval(new TextDecoder().decode('
    'await tjs.readFile(tjs.env.WEB_TOOLING_BUNDLE))); tjs.exit(0);'
)
WORKLOADS = {
    'acorn', 'babel', 'babel-minify', 'babylon', 'buble', 'chai', 'espree',
    'esprima', 'jshint', 'lebab', 'postcss', 'prepack', 'prettier',
    'source-map', 'terser', 'typescript', 'uglify-js', 'Geometric mean',
}


def scores_from_output(output):
    matches = re.findall(
        r'^\s*([a-z][a-z-]*|Geometric mean):\s+([0-9.]+) runs/s$',
        output, re.M,
    )
    scores = {name: float(value) for name, value in matches}
    if (set(scores) != WORKLOADS or len(matches) != len(WORKLOADS)
            or any(value <= 0 for value in scores.values())):
        raise ValueError('Expected one positive score for each of the 17 workloads and geometric mean')
    return scores


def fingerprint(path):
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return {'path': str(path), 'sha256': digest}


def write_json(path, value):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def run_order(names, trial):
    # Each three-binary cycle uses every order, matching the final triad study.
    if len(names) == 3:
        orders = ((0, 1, 2), (2, 1, 0), (1, 0, 2),
                  (2, 0, 1), (1, 2, 0), (0, 2, 1))
        return [names[i] for i in orders[trial % 6]]
    return names if trial % 2 == 0 else list(reversed(names))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', action='append', required=True,
                        metavar='NAME=PATH', help='Repeat two or three times; first is the control')
    parser.add_argument('--suite', type=Path, required=True, help='Classic dist/cli.js bundle')
    parser.add_argument('--output', type=Path, required=True, help='New results directory')
    parser.add_argument('--rounds', type=int, default=6)
    parser.add_argument('--timeout', type=float, default=120)
    args = parser.parse_args()
    binaries = {}
    for item in args.binary:
        name, separator, filename = item.partition('=')
        if not separator or not re.fullmatch(r'[a-zA-Z0-9_-]+', name) or name in binaries:
            parser.error('Each binary requires a unique NAME=PATH with a simple alphanumeric name')
        binaries[name] = Path(filename).resolve(strict=True)
    if len(binaries) not in (2, 3) or args.rounds < 1 or args.timeout <= 0:
        parser.error('Supply two or three binaries, positive rounds and a positive timeout')
    suite = args.suite.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    home = output / 'tjs-home'
    home.mkdir()
    env = dict(os.environ, TJS_HOME=str(home), WEB_TOOLING_BUNDLE=str(suite))
    metadata = {
        'started_utc': datetime.now(timezone.utc).isoformat(),
        'platform': platform.platform(), 'machine': platform.machine(),
        'suite': fingerprint(suite),
        'binaries': {name: fingerprint(path) for name, path in binaries.items()},
        'rounds': args.rounds, 'warmups_per_binary': 1, 'script': SCRIPT,
        'note': 'Build flags must be verified separately; these are existing binaries.',
    }
    write_json(output / 'metadata.json', metadata)
    runs = []

    def execute(name, label):
        start = time.monotonic()
        with (output / f'{label}-{name}.log').open('w+') as log:
            subprocess.run(
                [str(binaries[name]), 'eval', SCRIPT], env=env,
                stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout,
                check=True,
            )
            elapsed = time.monotonic() - start
            log.seek(0)
            scores = scores_from_output(log.read())
        return scores, elapsed

    for name in binaries:
        print(f'warmup {name}', flush=True)
        execute(name, 'warmup')
    names = list(binaries)
    for trial in range(args.rounds):
        for name in run_order(names, trial):
            scores, elapsed = execute(name, f'web-{trial + 1}')
            runs.append({'trial': trial + 1, 'mode': name,
                         'scores': scores, 'seconds': elapsed})
            write_json(output / 'comparison.json', runs)
            print(f'{trial + 1} {name}: {scores["Geometric mean"]:.2f} runs/s', flush=True)
    control = {run['trial']: run['scores'] for run in runs if run['mode'] == names[0]}
    summary = {}
    for name in names[1:]:
        candidate = [run for run in runs if run['mode'] == name]
        summary[name] = {}
        for workload in sorted(WORKLOADS):
            changes = [100 * (run['scores'][workload] / control[run['trial']][workload] - 1)
                       for run in candidate]
            summary[name][workload] = {
                'median_paired_percent': median(changes),
                'wins': sum(value > 0 for value in changes),
                'rounds': len(changes),
            }
        result = summary[name]['Geometric mean']
        print(f'{name} vs {names[0]}: {result["median_paired_percent"]:+.2f}%, '
              f'{result["wins"]}/{args.rounds} wins')
    write_json(output / 'summary.json', summary)


if __name__ == '__main__':
    main()
