"""Development authority: private key stays on host, never enters the kernel."""
import pathlib,subprocess,argparse
parser=argparse.ArgumentParser(); parser.add_argument('--authority-key',type=pathlib.Path)
args=parser.parse_args()
b=pathlib.Path('build'); key=args.authority_key or b/'dev-authority.pem'
def run(args): subprocess.run(args,check=True,stdout=subprocess.DEVNULL)
if args.authority_key and not key.is_file(): raise SystemExit('Authority key file not found')
if not key.exists(): run(['openssl','genpkey','-algorithm','ED25519','-out',str(key)])
if not args.authority_key: key.chmod(0o600)
run(['openssl','pkey','-in',str(key),'-pubout','-outform','DER','-out','build/authority.der'])
# Stack VM: PUSH 1, PUSH 2, ADD, PUSH 3, EQ, ASSERT, HALT.
vm=bytes([1,1,1,2,2,1,3,3,4,0])
code=(b/'agent.bin').read_bytes()
assert 0<len(code)<=4096
# Version/domain, fixed code and proof lengths are authenticated as well.
import struct
message=b'OMEGA-CAPSULE-2\0'+struct.pack('<II',len(code),len(vm))+code+vm
(b/'capsule.msg').write_bytes(message)
run(['openssl','pkeyutl','-sign','-rawin','-inkey',str(key),'-in','build/capsule.msg','-out','build/capsule.sig'])
pub=(b/'authority.der').read_bytes()[-32:]
def c_array(name,data): return 'static const unsigned char '+name+'[] = {'+','.join(str(x) for x in data)+'};\n'
(b/'capsule.h').write_text(c_array('authority_public',pub)+c_array('capsule_signature',(b/'capsule.sig').read_bytes())+c_array('capsule_message',message))

compiler=(b/'compiler.elf').read_bytes()
assert len(compiler)<=256*1024
message=b'OMEGA-COMPILER3\0'+struct.pack('<II',len(compiler),len(vm))+compiler+vm
(b/'compiler.msg').write_bytes(message)
run(['openssl','pkeyutl','-sign','-rawin','-inkey',str(key),'-in','build/compiler.msg','-out','build/compiler.sig'])
with (b/'capsule.h').open('a') as f:
 f.write(c_array('compiler_signature',(b/'compiler.sig').read_bytes())+c_array('compiler_message',message))
