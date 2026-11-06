"""Run Cortex checkpoints through the existing three-node signed quorum faults."""
from hardware import *
from cortex_fixture import model
from v4_validation import main as cluster_validation,verify_record
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from cortex_model import provision
model_path=Path('build/cortex-cluster.gguf');model(model_path,'q8')
def setup(image):
    image.unlink(missing_ok=True);provision(model_path,image,32*1024*1024,1,[1,2,3],[(0,1,2),(1,3,2),(2,1,0),(3,-1,2)])
cluster_validation(setup,burst_ticks=60000000000)
for i in range(1,4):
    pages=roots(Path(f'build/v4-node-{i}.img'))[-1][1];record=pages[6][1];checkpoint=record[280+2320:280+2320+256]
    assert struct.unpack_from('<Q',checkpoint)[0]==0x315854524f434d4f
    assert struct.unpack_from('<Q',checkpoint,16)[0]>=1
print('PASS Cortex model/KV/JIT checkpoint bound to 2/3 signatures; each voter verified its own SLS prefix before publication')
