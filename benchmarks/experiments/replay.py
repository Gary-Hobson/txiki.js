#!/usr/bin/env python3
"""Reconstruct historical QuickJS candidates without changing the input tree."""

import argparse
import difflib
import hashlib
import importlib.util
import json
from pathlib import Path


BASE = ('property-hint', 'load-field-fusion')
VARIANTS = {
    'hint-fusion': BASE,
    'retained-ef801eaa': BASE + ('comparison-branch',),
    'borrow-predicate': ('borrow-predicate',),
    'borrow-quickening': ('borrow-quickening',),
    'hint-fusion-quickening': BASE + ('borrow-quickening',),
    'site-read-ic': BASE + ('site-read-ic',),
    'site-read-ic-packed': BASE + ('site-read-ic-packed',),
    'object-layout': BASE + ('comparison-branch', 'object-layout'),
    'object-layout-shared': BASE + ('comparison-branch', 'object-layout-shared'),
}


def transform(source, variant):
    for name in VARIANTS[variant]:
        path = Path(__file__).with_name(name + '-transform.py')
        spec = importlib.util.spec_from_file_location(name.replace('-', '_'), path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        source = module.transform(source)
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True,
                        help='Unmodified quickjs.c from the recorded submodule revision')
    parser.add_argument('--variant', choices=VARIANTS, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New output file')
    parser.add_argument('--format', choices=('source', 'patch'), default='source')
    args = parser.parse_args()
    original = args.source.read_text()
    candidate = transform(original, args.variant)
    result = candidate
    if args.format == 'patch':
        result = ''.join(difflib.unified_diff(
            original.splitlines(True), candidate.splitlines(True),
            fromfile='a/quickjs.c', tofile='b/quickjs.c',
        ))
        result = ''.join('\n' if line == ' \n' else line for line in result.splitlines(True))
    with args.output.open('x') as stream:
        stream.write(result)
    print(json.dumps({
        'variant': args.variant,
        'source_sha256': hashlib.sha256(original.encode()).hexdigest(),
        'candidate_sha256': hashlib.sha256(candidate.encode()).hexdigest(),
        'output': str(args.output.resolve()), 'format': args.format,
    }, indent=2))


if __name__ == '__main__':
    main()
