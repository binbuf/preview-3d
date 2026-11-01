"""External corpus inventory, entry resolution and local-corpus hashing.

T01/T20 originally resolved a small seed set of aliases. T34 completes the
matrix: every root entry in the external test-model directory is resolved to a
concrete product entry file and recorded with its local path, size and hash.
That inventory is written under ``TestResults/`` and is deliberately *not* the
checked-in compatibility manifest. The portable manifest
(``tests/compat/manifest.json``) contains redistributable in-repository
fixtures only.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

from common import sha256_file, write_json

DEFAULT_CORPUS = Path('D:/repos/binbuf/preview-3d-test-models')

# Directory metadata is not corpus content. Windows Explorer writes
# ``desktop.ini``/``Thumbs.db`` beside the models, and SMB/Apple clients write
# the dot-named equivalents that the leading-dot rule already excludes (``.git``
# is excluded for the same reason). Keep the rule in one helper so the corpus
# inventory and the full-corpus audit cannot disagree about a root entry.
OS_METADATA_NAMES = frozenset({
    'desktop.ini',
    'thumbs.db',
    'ehthumbs.db',
    'ehthumbs_vista.db',
    'thumbs.db:encryptable',
    '$recycle.bin',
    'system volume information',
})


def is_corpus_entry(name: str) -> bool:
    """Whether a corpus root entry is content rather than OS/VCS metadata."""
    if name.startswith('.'):
        return False
    return name.lower() not in OS_METADATA_NAMES


# Extensions the product can open. ``family`` is the render-matrix family, not
# a parser self-report. A directory-shaped asset (Camera_01_4k.gltf/,
# choco-bunny/, ...) is resolved to its largest supported entry file.
MODEL_SUFFIXES = {
    '.glb': 'glb',
    '.gltf': 'gltf',
    '.stl': 'stl',
    '.ply': 'ply',
    '.obj': 'obj',
    '.fbx': 'fbx',
    '.usdz': 'usdz',
    '.usda': 'usd',
    '.usdc': 'usd',
    '.3mf': '3mf',
    '.step': 'step',
    '.stp': 'step',
    '.igs': 'iges',
    '.iges': 'iges',
    '.brep': 'brep',
    '.xbf': 'xbf',
    '.fcstd': 'fcstd',
    '.dae': 'assimp',
    '.3ds': 'assimp',
    '.off': 'assimp',
    '.amf': 'assimp',
    '.x3d': 'assimp',
    '.x': 'assimp',
    '.dxf': 'assimp',
}

# T20 seeds these aliases. T34 uses the generic ``rootAssets`` inventory and
# keeps these aliases for earlier profiles/tests.
SEED_ASSETS = {
    'car': 'Car.glb',
    'camera': 'Camera_01_4k.gltf',
    'microscope': 'vintage_microscope_4k.gltf',
    'stl': 'base.stl',
    'suit': 'Advanced Crew Escape Suit.glb',
    'lid': 'lid.stl',
    'cartridge': 'cartridge.stl',
}


def format_for(path: Path) -> str | None:
    return MODEL_SUFFIXES.get(Path(path).suffix.lower())


def _model_candidates(directory: Path):
    candidates = []
    for path in directory.rglob('*'):
        if path.is_file() and format_for(path):
            candidates.append(path)
    # Prefer shallower entries; among equals, the largest payload is the
    # authored model rather than a helper/partial file. The top-level glTF
    # fixture directories put the entry beside their buffers, while downloaded
    # packages use source/ and textures/ subdirectories.
    candidates.sort(key=lambda p: (len(p.relative_to(directory).parts),
                                   -p.stat().st_size, p.name.lower()))
    return candidates


def resolve_asset(root: Path, name: str):
    """Resolve a root name to (entry, sidecars, directory-shaped, family)."""
    root = Path(root)
    candidate = root / name
    if candidate.is_file():
        return candidate, [], False, format_for(candidate)
    if not candidate.is_dir():
        return None, [], False, None
    entries = _model_candidates(candidate)
    if not entries:
        return None, [], True, None
    # Preserve the established glTF-directory preference: the largest .gltf
    # owns the scene (sidecars are resolved relative to it).
    gltfs = [p for p in entries if p.suffix.lower() == '.gltf']
    entry = (max(gltfs, key=lambda p: p.stat().st_size) if gltfs
             else max(entries, key=lambda p: p.stat().st_size))
    return entry, gltf_sidecars(entry), True, format_for(entry)


def resolve_entry(root: Path, name: str):
    """Backwards-compatible 3-tuple used by earlier lanes."""
    entry, sidecars, directory, _ = resolve_asset(root, name)
    return entry, sidecars, directory


def gltf_sidecars(entry: Path):
    """Referenced buffers and images, resolved relative to the entry file."""
    if entry.suffix.lower() != '.gltf':
        return []
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


def asset_record(root: Path, name: str, alias: str | None = None,
                 hash_entry: bool = True):
    entry, sidecars, directory, family = resolve_asset(root, name)
    record = {
        'alias': alias or name,
        'rootName': name,
        'directoryShaped': directory,
        'family': family,
        'available': entry is not None,
        'entry': None,
        'entryFormat': family,
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
    sidecar_records = []
    sidecar_total = 0
    for path in sidecars:
        sidecar_records.append({'path': str(path.resolve()),
                                'sha256': sha256_file(path),
                                'bytes': path.stat().st_size})
        sidecar_total += path.stat().st_size
    record.update({
        'entry': str(entry.resolve()),
        'entrySha256': sha256_file(entry) if hash_entry else None,
        'entryBytes': entry.stat().st_size,
        'sidecars': sidecar_records,
        'sidecarBytes': sidecar_total,
    })
    if name.lower().startswith('car'):
        record['distribution']['observedLicense'] = (
            'Sketchfab asset metadata indicates CC-BY-NC-SA-4.0')
    return record


def inventory(root: Path = DEFAULT_CORPUS):
    root = Path(root)
    if not root.is_dir():
        return {
            'root': str(root), 'available': False,
            'reason': 'external corpus root is not present',
            'rootEntries': [], 'rootAssets': {}, 'seedAssets': {},
            'status': 'incomplete',
        }
    entries = []
    assets = {}
    for item in sorted(root.iterdir(), key=lambda p: p.name.lower()):
        # Metadata/control entries such as .git, desktop.ini and Thumbs.db are
        # not corpus assets.
        if not is_corpus_entry(item.name):
            continue
        record = asset_record(root, item.name, alias=item.name)
        assets[item.name] = record
        entries.append({
            'name': item.name,
            'shape': 'directory' if item.is_dir() else 'file',
            'bytes': (sum(p.stat().st_size for p in item.rglob('*') if p.is_file())
                      if item.is_dir() else item.stat().st_size),
            'family': record['family'],
            'entry': record['entry'],
            'entryBytes': record['entryBytes'],
            'entrySha256': record['entrySha256'],
        })
    seeds = {}
    for alias, name in SEED_ASSETS.items():
        seeds[alias] = assets.get(name) or asset_record(root, name, alias)
        seeds[alias]['alias'] = alias
    return {
        'root': str(root),
        'available': True,
        'rootEntries': entries,
        'rootAssets': assets,
        'seedAssets': seeds,
        'status': 'available',
        'note': 'Local-corpus layer: every root entry is resolved and hashed here. '
                'It is not part of the checked-in portable compatibility manifest.',
    }


def write_inventory(output: Path, root: Path = DEFAULT_CORPUS):
    record = inventory(root)
    write_json(Path(output) / 'corpus-inventory.json', record)
    return record


def external_lane_status(record):
    """The external qualification lane is incomplete, never a passing skip."""
    if not record.get('available'):
        return {'status': 'incomplete', 'reason': record.get('reason')}
    missing = [name for name, item in record.get('rootAssets', {}).items()
               if not item.get('available')]
    if missing:
        return {'status': 'incomplete',
                'reason': 'unresolved root entries: ' + ', '.join(missing)}
    return {'status': 'available'}
