"""Embed browser assets, optional UI icons, and the replaceable PS5 tile PNG."""
from pathlib import Path
import hashlib
import json
import struct
import re

root = Path(__file__).resolve().parent.parent
out = root / 'build'
out.mkdir(exist_ok=True)
version = (root/'VERSION').read_text().strip()
if not re.fullmatch(r'\d+\.\d{1,2}', version):
    raise SystemExit('VERSION must contain a numeric release such as 0.03')

png = (root/'assets/icon0.png').read_bytes()
if png[:8] != b'\x89PNG\r\n\x1a\n' or len(png) < 33 or png[12:16] != b'IHDR':
    raise SystemExit('assets/icon0.png must be a 512 x 512 PNG')
if struct.unpack('>II',png[16:24]) != (512,512):
    from PIL import Image, ImageOps
    import io
    with Image.open(io.BytesIO(png)) as image:
        image.load()
        tile = ImageOps.pad(image.convert('RGBA'), (512,512), method=Image.Resampling.LANCZOS, color=(0,0,0,0))
        buffer = io.BytesIO()
        tile.save(buffer, format='PNG')
        png = buffer.getvalue()
(out/'icon0.png').write_bytes(png)
assets = {name: (root/'web'/path).read_bytes() for name,path in
          [('index_html','index.html'),('app_js','app.js'),('style_css','style.css'),('uploads_js','uploads.js'),('boot_css','boot.css')]}
assets['tile_png'] = png
assets['index_html'] = assets['index_html'].replace(b'__PSFOIL_VERSION__', version.encode())
icons = []
for name in ('logo','store','library','download','downloads','settings','quit','add','tick','trash'):
    for extension,mime in (('png','image/png'),('svg','image/svg+xml')):
        path = root/'web/icons'/f'{name}.{extension}'
        if not path.exists(): continue
        data = path.read_bytes()
        if len(data) > 2*1024*1024: raise SystemExit(f'{path.name} exceeds 2 MB')
        key = 'icon_'+name
        assets[key] = data
        icons.append((name,key,mime))
        break
assets['icons_js'] = ('window.PSFOIL_ICONS='+json.dumps({name:'/icons/'+name for name,_,_ in icons})+';').encode()
with (out/'assets.h').open('w',encoding='utf-8',newline='\n') as f:
    f.write('#pragma once\n#include <cstddef>\n')
    f.write('inline constexpr char app_version[] = '+json.dumps(version)+';\n')
    for name, data in assets.items():
        f.write('inline const unsigned char '+name+'[] = {'+','.join(map(str,data))+'};\n')
        f.write('inline constexpr size_t '+name+'_size = sizeof('+name+');\n')
    f.write('inline constexpr char tile_asset_id[] = "'+hashlib.sha256(png).hexdigest()[:16]+'";\n')
    f.write('struct EmbeddedIcon {const char* name; const unsigned char* data; size_t size; const char* mime;};\n')
    f.write('inline const EmbeddedIcon ui_icons[] = {')
    for name,key,mime in icons: f.write('{"'+name+'",'+key+','+key+'_size,"'+mime+'"},')
    f.write('{nullptr,nullptr,0,nullptr}};\n')
