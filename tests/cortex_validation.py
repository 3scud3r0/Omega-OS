"""Hardware Cortex acceptance with a standard numerical GGUF and real disk paging."""
from hardware import *
import hashlib,re
import numpy as np
from cortex_reference import infer
from cortex_fixture import model
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from cortex_model import provision

def run(image,label,cpu='max',expect_rejection=False):
    cmd=[QEMU,'-kernel','omega_os.elf','-smp','2','-m','64M','-cpu',cpu,'-serial','stdio','-display','none','-no-reboot','-drive',f'file={image},if=virtio,format=raw']
    p=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT);output=bytearray();selector=selectors.DefaultSelector();selector.register(p.stdout,selectors.EVENT_READ);end=time.monotonic()+240
    try:
        while time.monotonic()<end:
            for key,_ in selector.select(.1):
                chunk=os.read(key.fd,65536);output+=chunk
                if b'FAIL:' in output or (b'OMEGA USER ERROR:' in output and not expect_rejection):raise AssertionError(output.decode(errors='replace'))
                if b'complete; HLT\n' in output:return output.decode()
                if not chunk and p.poll() is not None:raise AssertionError(output.decode(errors='replace'))
        raise AssertionError('Cortex timeout:\n'+output.decode(errors='replace')[-2000:])
    finally:
        if p.poll() is None:p.kill()
        p.wait(timeout=5);selector.close();Path(f'build/{label}.log').write_bytes(output)

def main():
    for mode,cpu,profile in [('q8','max','q8-avx'),('q4','max','q4-avx'),('q4','qemu64,-xsave','q4-scalar')]:
        path=Path(f'build/cortex-{profile}.gguf');image=Path(f'build/cortex-{profile}.img');scaling='llama3' if profile=='q4-avx' else ('linear' if profile=='q4-scalar' else None);decoded=model(path,mode,rope_scaling=scaling);image.unlink(missing_ok=True);provision(path,image,32*1024*1024,8,[1,2,3],[(0,1,2),(1,3,2),(2,1,0),(3,-1,2)]);
        text=run(image,f'cortex-{profile}-first',cpu)
        assert 'GGUF inference + PagedAttention + symbolic pruning + native JIT PASS' in text
        assert ('AVX2/FMA dispatch' if cpu=='max' else 'scalar/SWAR dispatch') in text
        assert ('XSAVE x87/SSE/YMM' if cpu=='max' else 'FXSAVE x87/SSE') in text
        reads,writes,evictions,peak=map(int,re.search(r'CORTEX pager reads=(\d+) writes=(\d+) evictions=(\d+) peak=(\d+)',text).groups());assert reads>64 and writes>0 and evictions>0 and peak<=64
        state=roots(image)[-1][1][0x1001][1];values=struct.unpack_from('<7Q8I',state);assert values[2]==8 and values[6]>768*1024*1024
        pages=roots(image)[-1][1];token,reference,keys,values=infer(decoded,8,[1,2,3],rope_scaling=scaling)
        actual=np.frombuffer(pages[0x1002][1],dtype='<f4',count=256);assert np.all(np.isneginf(actual)==np.isneginf(reference))
        np.testing.assert_allclose(actual[[0,3]],reference[[0,3]],rtol=0.004,atol=0.0004)
        assert values is not None and struct.unpack_from('<Q',state,24)[0]==token
        # Compare complete stored K/V blocks, not merely their checksums.
        for layer_id in range(2):
            offset=0x800000+layer_id*131072
            packed=np.frombuffer(b''.join(pages[offset+i][1] for i in range(1)),dtype='<f2')[:8*256].reshape(8,256)
            np.testing.assert_allclose(packed[:,:128],keys[layer_id],rtol=0.008,atol=0.001)
            np.testing.assert_allclose(packed[:,128:],values[layer_id],rtol=0.008,atol=0.001)
        second=run(image,f'cortex-{profile}-reboot',cpu);assert 'restored SLS KV-cache hash PASS' in second
        recovered=roots(image)[-1][1][0x1001][1];assert struct.unpack_from('<Q',recovered,16)[0]==16
        print(f'PASS Cortex {profile}: full Llama path, logical KV >768 MiB, 64 resident frames, page eviction, arbiter/JIT and durable reboot')
    path=Path('build/cortex-invalid.gguf');image=Path('build/cortex-invalid.img');model(path,'q8');raw=bytearray(path.read_bytes());key=b'llama.feed_forward_length';where=raw.index(key)+len(key);struct.pack_into('<I',raw,where+4,0);path.write_bytes(raw);image.unlink(missing_ok=True);provision(path,image,32*1024*1024,1,[1])
    rejected=run(image,'cortex-invalid',expect_rejection=True)
    assert 'OMEGA USER ERROR: GGUF model configuration' in rejected and 'CORTEX task quarantined; kernel retained prior checkpoint' in rejected
    assert 0x1001 not in roots(image)[-1][1]
    print('PASS malformed GGUF quarantined; kernel continues; no cognitive checkpoint published')
if __name__=='__main__':main()
