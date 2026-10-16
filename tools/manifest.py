"""Validate the built PS5 ELF and record its reproducible release metadata."""
import hashlib
import json
import struct
from pathlib import Path

root = Path(__file__).resolve().parent.parent
artifact = root/'build/psfoil.elf'
data = artifact.read_bytes()
if len(data) < 64 or data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', data, 18)[0] != 62:
    raise SystemExit('Expected a little-endian x86-64 ELF')
manifest = {
    'version': (root/'VERSION').read_text().strip(),
    'artifact': artifact.name,
    'bytes': len(data),
    'sha256': hashlib.sha256(data).hexdigest(),
    'target': 'PS5 x86-64 / elfldr',
    'console_tested': False,
}
(root/'build/manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
print(f'PSFoil {manifest["version"]}: {manifest["bytes"]:,} bytes, SHA-256 {manifest["sha256"]}')
