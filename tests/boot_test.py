"""Validate actual Multiboot header, UART behavior and stable halt under QEMU."""
import pathlib
import struct
import subprocess
import sys

binary = pathlib.Path('omega_os.elf').read_bytes()
magic = struct.pack('<I', 0x1BADB002)
offset = binary.find(magic, 0, 8192)
assert offset >= 0 and offset % 4 == 0, 'Multiboot header location'
a, b, c = struct.unpack_from('<III', binary, offset)
assert (a + b + c) & 0xffffffff == 0, 'Multiboot checksum'
assert binary[4] == 1, 'QEMU Multiboot ELF32 container'
cmd = [sys.argv[1] if len(sys.argv)>1 else 'qemu-system-x86_64',
       '-kernel', 'omega_os.elf', '-serial', 'stdio', '-display', 'none',
       '-no-reboot', '-m', '128M']
proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
try:
    output, _ = proc.communicate(timeout=12)
except subprocess.TimeoutExpired:
    proc.terminate()
    output, _ = proc.communicate(timeout=3)
text = output.decode(errors='replace')
pathlib.Path('build/boot.log').write_text(text)
print(text)
assert 'FAIL:' not in text, 'kernel assertion failed'
for marker in ['BOOT x86-64', 'PMM allocated', 'SLS dirty 0x0000000040000000',
               'SLS dirty 0x0000000040001000', 'arithmetic/overflow PASS',
               'SPSC full/empty/FIFO/wrap PASS', 'IPC transferred 16 messages',
               'quarantine/frame rollback PASS', 'CPU TSC cycles:', 'STABLE:']:
    assert marker in text, f'missing: {marker}'
print(f'PASS: Multiboot at file offset {offset}; all boot assertions completed')
