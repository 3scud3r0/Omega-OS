"""Create a reviewable, deterministic text bundle of all source/build/test files."""
from pathlib import Path
paths = [Path('README.md'), Path('Makefile'), Path('link.ld'), Path('.gitignore')]
paths += sorted(Path('include').rglob('*.h'))
paths += sorted(Path('src').rglob('*.c'))
paths += sorted(Path('tests').glob('*.py'))
paths += sorted(Path('tests').glob('*.c'))
paths += sorted(p for p in Path('user').rglob('*') if p.suffix in ('.c','.h','.ld'))
paths += sorted(Path('tools').glob('*.py'))
paths += sorted(Path('docs').glob('*.md'))
paths += sorted(p for p in Path('vendor/omega').rglob('*') if p.suffix in ('.omega','.py','.json','.md'))
for generated in ['build/omega_generated.c','build/omega_seed_report.json']:
    if Path(generated).exists(): paths.append(Path(generated))
paths += [Path('src/crypto/LICENCE.md'),Path('src/crypto/UPSTREAM.json')]
if Path('build/capsule.h').exists(): paths.append(Path('build/capsule.h'))
parts = ['OMEGA OS — fontes completos da implementação modular\n'
         'Gerado por make bundle. Cada marcador identifica o arquivo original.\n']
for path in paths:
    parts.append(f'\n{"=" * 72}\nARQUIVO: {path.as_posix()}\n{"=" * 72}\n{path.read_text()}')
Path('omega_os_fontes.txt').write_text(''.join(parts))
print(f'omega_os_fontes.txt: {len(paths)} arquivos; {Path("omega_os_fontes.txt").stat().st_size} bytes')
