"""Create a reviewable, deterministic text bundle of all source/build/test files."""
from pathlib import Path
paths = [Path('README.md'), Path('Makefile'), Path('link.ld'), Path('.gitignore')]
paths += sorted(Path('include').glob('*.h'))
paths += sorted(Path('src').glob('*.c'))
paths += sorted(Path('tests').glob('*.py'))
paths += sorted(Path('tests').glob('*.c'))
parts = ['OMEGA OS — fontes completos da implementação modular\n'
         'Gerado por make bundle. Cada marcador identifica o arquivo original.\n']
for path in paths:
    parts.append(f'\n{"=" * 72}\nARQUIVO: {path.as_posix()}\n{"=" * 72}\n{path.read_text()}')
Path('omega_os_fontes.txt').write_text(''.join(parts))
print(f'omega_os_fontes.txt: {len(paths)} arquivos; {Path("omega_os_fontes.txt").stat().st_size} bytes')
