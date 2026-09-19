"""Materialize bounded multi-domain libFuzzer seeds for the STEP admission boundary."""

import argparse
from pathlib import Path
import struct


MAGIC = 0x5A465453  # "STFZ"
PART21_ADMISSION = 0
DECLARATION_DISCOVERY = 1
CONTROL_FRAME = 2
NORMALIZED_OUTPUT = 3


def envelope(domain: int, payload: bytes, flags: int = 0) -> bytes:
    return struct.pack('<IBBH', MAGIC, domain, flags, 0) + payload


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    fixtures = root / 'tests/fixtures/stp-spike'
    if args.output.exists() and any(args.output.iterdir()):
        raise SystemExit(
            f'refusing non-empty seed directory: {args.output}; use a new path')
    args.output.mkdir(parents=True, exist_ok=True)

    count = 0
    for source in sorted(fixtures.glob('*.stp')):
        payload = source.read_bytes()
        (args.output / f'{source.stem}.admission.seed').write_bytes(
            envelope(PART21_ADMISSION, payload))
        (args.output / f'{source.stem}.declaration.seed').write_bytes(
            envelope(DECLARATION_DISCOVERY, payload, flags=1))
        count += 2

    # External-declaration discovery variants: every path form must be rejected
    # before the OCCT reader is reachable.
    declarations = [
        b"FILE_POPULATION(('AP214'),('x'),($),$);",
        b"DOCUMENT_FILE('sub/other.stp',('AUTOMOTIVE_DESIGN'),'x',#1);",
        b"DOCUMENT_FILE('C:\\\\models\\\\other.stp',('AUTOMOTIVE_DESIGN'),'x',#1);",
        b"DOCUMENT_FILE('\\\\\\\\server\\\\share\\\\other.stp',('AUTOMOTIVE_DESIGN'),'x',#1);",
        b"DOCUMENT_FILE('https://example.invalid/other.stp',('AUTOMOTIVE_DESIGN'),'x',#1);",
    ]
    for index, declaration in enumerate(declarations):
        text = (b'ISO-10303-21;\nHEADER;\n' + declaration
                + b'\nENDSEC;\nDATA;\n#1=APPLICATION_CONTEXT(\'core data\');\n'
                  b'ENDSEC;\nEND-ISO-10303-21;\n')
        (args.output / f'declaration-{index}.seed').write_bytes(
            envelope(DECLARATION_DISCOVERY, text, flags=1))
        count += 1

    # Mutated production control frames enter the exact framed decoder without a
    # GPU or child process.
    step_request = struct.pack('<IIQQQQIIQ', 20, 48, 1, 0, 0, 4096, 0, 0, 0)
    (args.output / 'start-step-control.seed').write_bytes(
        envelope(CONTROL_FRAME, step_request))
    progress = struct.pack('<IIQIIIIQQQ', 21, 48, 1, 5, 3, 4, 0, 0, 0, 0)
    (args.output / 'step-progress-control.seed').write_bytes(
        envelope(CONTROL_FRAME, progress))

    section_header = struct.pack('<IIQQIIQ48x', 0x50334457, 10, 0, 88, 0, 0, 0)
    (args.output / 'normalized-header.seed').write_bytes(
        envelope(NORMALIZED_OUTPUT, section_header))
    count += 3

    print(f'materialized {count} STEP fuzz seeds in {args.output}')


if __name__ == '__main__':
    main()
