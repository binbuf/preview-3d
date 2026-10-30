"""External corpus inventory, glTF directory entry resolution and hashing."""
from __future__ import annotations

import json
from pathlib import Path

from common import sha256_file, write_json

DEFAULT_CORPUS = Path('D:/repos/binbuf/preview-3d-test-models')

# T20 seeds these four assets. T34 expands the matrix to every root asset.
SEED_ASSETS = {
    'car': 'Car.glb',
    'camera': 'Camera_01_4k.gltf',
    'microscope': 'vintage_microscope_4k.gltf',
    'stl': 'base.stl',
}


def resolve_entry(root: Path, name: str):
    """Resolve a root name to (entry path, sidecar paths, directory-shaped)."""
    candidate = Path(root) / name
    if candidate.is_file():
        return candidate, [], False
    if not candidate.is_dir():
        return None, [], False
    # A directory-shaped glTF asset: choose the .gltf file that owns the most
    # bytes of referenced buffers/images, or the lexicographically first.
    entries = sorted(candidate.glob('*.gltf'))
    if not entries:
        return None, [], True
    entry = max(entries, key=lambda p: (p.stat().st_size, p.name))
    return entry, [], True


def gltf_sidecars(entry: Path):
    """Referenced buffers and images, resolved relative to the entry file."""
    try:
        document = json.loads(entry.read_text(encoding='utf-8'))
    except Exception:
        return []
    paths = []
    for key in ('buffers', 'images'):
        for item in document.get(key, []) or []:
            uri = item.get('uri')
            if not uri or uri.startswith('data:'):
                continue
            path = (entry.parent / uri).resolve()
            if path.exists() and path.is_file():
                paths.append(path)
    return paths


def asset_record(root: Path, name: str, alias: str | None = None):
    entry, _, directory = resolve_entry(root, name)
    record = {
        'alias': alias or name,
        'rootName': name,
        'directoryShaped': directory,
        'available': entry is not None,
        'entry': None,
        'entrySha256': None,
        'entryBytes': None,
        'sidecars': [],
        'sidecarBytes': 0,
        'distribution': {
            'committed': False,
            'note': 'Restricted local corpus; screenshots/models stay out of the repository '
                    'and package until redistribution rights are established.',
        },
    }
    if entry is None:
        return record
    sidecars = gltf_sidecars(entry)
    record.update({
        'entry': str(entry.resolve()),
        'entrySha256': sha256_file(entry),
        'entryBytes': entry.stat().st_size,
        'sidecars': [{'path': str(p.resolve()), 'sha256': sha256_file(p),
                      'bytes': p.stat().st_size} for p in sidecars],
        'sidecarBytes': sum(p.stat().st_size for p in sidecars),
    })
    if name.lower().startswith('car'):
        record['distribution']['observedLicense'] = 'Sketchfab asset metadata indicates CC-BY-NC-SA-4.0'
    return record


def inventory(root: Path = DEFAULT_CORPUS):
    root = Path(root)
    if not root.is_dir():
        return {
            'root': str(root), 'available': False,
            'reason': 'external corpus root is not present',
            'rootEntries': [], 'seedAssets': {},
            'status': 'incomplete',
        }
    entries = []
    for item in sorted(root.iterdir(), key=lambda p: p.name.lower()):
        if item.is_dir():
            entries.append({'name': item.name, 'shape': 'directory',
                            'bytes': sum(p.stat().st_size for p in item.rglob('*') if p.is_file())})
        else:
            entries.append({'name': item.name, 'shape': 'file', 'bytes': item.stat().st_size})
    seeds = {}
    for alias, name in SEED_ASSETS.items():
        seeds[alias] = asset_record(root, name, alias)
    return {
        'root': str(root),
        'available': True,
        'rootEntries': entries,
        'seedAssets': seeds,
        'status': 'available',
        'note': 'Inventory covers every root entry. Captures seeded for Car, Camera, '
                'microscope and one STL; T34 completes the cross-format matrix.',
    }


def write_inventory(output: Path, root: Path = DEFAULT_CORPUS):
    record = inventory(root)
    write_json(Path(output) / 'corpus-inventory.json', record)
    return record


def external_lane_status(record):
    """The external qualification lane is incomplete, never a passing skip."""
    if not record.get('available'):
        return {'status': 'incomplete', 'reason': record.get('reason')}
    missing = [alias for alias, item in record['seedAssets'].items() if not item['available']]
    if missing:
        return {'status': 'incomplete', 'reason': 'missing seed assets: ' + ', '.join(missing)}
    return {'status': 'available'}
