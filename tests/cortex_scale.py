"""Write and verify more live KV backing data than the guest's entire RAM."""
from pathlib import Path
import re,sys,time
from cortex_fixture import model
import cortex_validation as validation
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from cortex_model import provision

def main():
    model_path=Path('build/cortex-scale.gguf');image=Path('build/cortex-scale.img');model(model_path,'q8');image.unlink(missing_ok=True)
    provision(model_path,image,256*1024*1024,1,[1],[(0,1,2),(3,-1,2)],stress_pages=17000)
    begin=time.monotonic();text=validation.run(image,'cortex-scale','max')
    assert 'out-of-core KV stress >64MiB data verified PASS' in text
    reads,writes,evictions,peak=map(int,re.search(r'CORTEX pager reads=(\d+) writes=(\d+) evictions=(\d+) peak=(\d+)',text).groups())
    assert reads>=34000 and writes>=17000 and evictions>33000 and peak==64
    print(f'PASS 69632000 verified bytes >64-MiB guest RAM; peak 64 frames, {writes} writes/{evictions} evictions, {time.monotonic()-begin:.1f}s')
if __name__=='__main__':main()
