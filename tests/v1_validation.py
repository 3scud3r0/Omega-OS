"""Hardware acceptance: DMA bytes on host, SIGKILL/reboot, CRC fallback, SMP, CPL3."""
import os
from pathlib import Path
import selectors
import shutil
import struct
import subprocess
import sys
import time

QEMU = sys.argv[1] if len(sys.argv) > 1 else 'qemu-system-x86_64'
MASK = (1 << 64) - 1
MAGIC = 0x314C53534147454D
POLY = 0x42F0E1EBA9EA3693

def crc(data):
    value = 0
    for b in data:
        value ^= b << 56
        for _ in range(8):
            value = ((value << 1) ^ (POLY if value >> 63 else 0)) & MASK
    return value

def candidates(image, index):
    result = []
    with open(image, 'rb') as f:
        for slot in range(2):
            sector = 8 + index * 32 + slot * 16
            f.seek(sector * 512)
            meta = bytearray(f.read(4096))
            magic, generation, va, data_crc, header_crc = struct.unpack_from('<5Q', meta)
            meta[32:40] = bytes(8)
            if magic != MAGIC or va != 0x40000000 + index * 4096 or not generation or crc(meta) != header_crc:
                continue
            f.seek((sector + 8) * 512)
            data = f.read(4096)
            if crc(data) == data_crc:
                result.append((generation, sector, data))
    assert result, f'no valid committed slot for page {index}'
    return sorted(result)

def boot(image, label, crash=0):
    cmd = [QEMU, '-kernel', 'omega_os.elf', '-smp', '2', '-m', '128M',
           '-serial', 'stdio', '-display', 'none', '-no-reboot', '-accel', 'tcg,thread=multi']
    cmd += ['-drive', f'file={image},if=virtio,format=raw']
    if crash:
        cmd += ['-append', f'sls.crash={crash}']
    stop = (f'SLS crash-test boundary={crash}\n'.encode() if crash else b'validated; HLT\n')
    process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    captured = bytearray()
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + 30
    try:
        while time.monotonic() < deadline and stop not in captured:
            for key, _ in selector.select(0.2):
                chunk = os.read(key.fd, 65536)
                if chunk:
                    captured.extend(chunk)
                    if b'FAIL:' in captured:
                        raise AssertionError(captured.decode(errors='replace'))
                elif process.poll() is not None:
                    raise AssertionError(captured.decode(errors='replace'))
        text = captured.decode(errors='replace')
        assert 'FAIL:' not in text, text
        assert stop in captured, text
        if crash:
            return text
        for marker in ['IDT 256 gates', 'PCI VirtIO legacy detected',
                       'VirtIO disk DMA readback PASS', 'ACPI MADT BSP=',
                       'SMP Core1 online', 'SMP SPSC 65536',
                       'SLS disk committed page', 'vector=14', 'vector=13',
                       'Ring3 PF/GP + disk rollback PASS', 'Ring3 CR3 privilege GP PASS', 'STABLE V1:']:
            assert marker in text, f'{label}: missing {marker}\n{text}'
        return text
    finally:
        process.kill()  # Deliberate SIGKILL, not graceful guest shutdown.
        process.wait(timeout=5)
        selector.close()
        Path(f'build/{label}.log').write_bytes(captured)

binary = Path('omega_os.elf').read_bytes()
offset = binary.find(struct.pack('<I', 0x1BADB002), 0, 8192)
assert offset >= 0 and offset % 4 == 0
assert sum(struct.unpack_from('<3I', binary, offset)) & 0xffffffff == 0
assert binary[4] == 1
image = Path('build/v1-test.img')
with image.open('wb') as f:
    f.truncate(32 * 1024 * 1024)
first = boot(image, 'v1-first')
expected = bytes((j ^ 0xa5) & 255 for j in range(4096))
assert candidates(image, 2)[-1][2] == expected, 'host raw disk bytes'
for index in [0, 1]:
    assert struct.unpack_from('<Q', candidates(image, index)[-1][2], 80)[0] == 65536
second = boot(image, 'v1-reboot')
assert 'persistent probe recovered after reboot PASS' in second
assert second.count('TASK recovered/start epoch=65536') == 2
for index in [0, 1]:
    assert struct.unpack_from('<Q', candidates(image, index)[-1][2], 80)[0] == 131072
assert candidates(image, 2)[-1][2] == expected
# Reproduce a torn latest data slot. Preserve and validate the preceding slot.
old, latest = candidates(image, 2)[-2:]
with image.open('r+b') as f:
    f.seek((latest[1] + 8) * 512)
    f.write(b'CORRUPTED-LATEST-DATA')
    f.flush()
    os.fsync(f.fileno())
assert candidates(image, 2)[-1][0] == old[0]
third = boot(image, 'v1-fallback')
assert f'SLS recovered disk generation {old[0]}\n' in third
assert candidates(image, 2)[-1][2] == expected
# Kill the actual emulator at each boundary in the write/FLUSH protocol.
for boundary in range(1, 5):
    cut = Path(f'build/v1-cut-{boundary}.img')
    shutil.copyfile(image, cut)
    before_record = candidates(cut, 0)[-1]
    before = before_record[0]
    before_epoch = struct.unpack_from('<Q', before_record[2], 80)[0]
    boot(cut, f'v1-cut-{boundary}', crash=boundary)
    after_record = candidates(cut, 0)[-1]
    after = after_record[0]
    after_epoch = struct.unpack_from('<Q', after_record[2], 80)[0]
    assert after_epoch == before_epoch + (after != before), 'old/new state atomicity'
    if after != before:
        assert struct.unpack_from('<2q', after_record[2], 8) == (1, 2)
    assert (after == before if boundary < 3 else after in (before, before + 1))
    if boundary == 4:
        assert after == before + 1, 'flushed metadata must be visible'
    restarted = boot(cut, f'v1-cut-{boundary}-reboot')
    assert 'persistent probe recovered after reboot PASS' in restarted
    assert candidates(cut, 2)[-1][2] == expected
print('PASS: four SIGKILL write/flush boundaries recovered with valid data')
print('PASS: Multiboot, DMA raw bytes, two-vCPU SPSC, Ring3 PF/GP, SIGKILL/reboot, torn-slot fallback')
