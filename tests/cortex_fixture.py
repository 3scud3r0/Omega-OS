"""Numerical GGUF fixture, not a trained language model. Same loader as production.
Produces standard Llama tensors with quantized Q8_0 or Q4_K/Q6_K blocks.
"""
import random,struct
from pathlib import Path
D=256;HIDDEN=512;LAYERS=2;HEADS=4;KV=2;VOCAB=256;CONTEXT=1048576

def text(s):b=s.encode();return struct.pack('<Q',len(b))+b

def model(path,mode='q8',seed=8,rope_scaling=None):
    rng=random.Random(seed);metadata={'general.architecture':'llama','llama.embedding_length':D,'llama.feed_forward_length':HIDDEN,'llama.block_count':LAYERS,'llama.attention.head_count':HEADS,'llama.attention.head_count_kv':KV,'llama.context_length':CONTEXT,'llama.attention.layer_norm_rms_epsilon':1e-5,'llama.rope.freq_base':10000.0}
    if rope_scaling:
        metadata.update({"llama.rope.scaling.type":rope_scaling,"llama.rope.scaling.factor":2.0 if rope_scaling=="linear" else 8.0,"llama.rope.scaling.original_context_length":64})
    tables=[];data=bytearray();decoded={}
    def tensor(name,columns,rows=1,norm=False):
        kind=0 if norm else (8 if mode=='q8' else (14 if 'ffn_down' in name else 12))
        values=[];payload=bytearray();n=columns*rows
        if norm:values=[1.0]*n;payload+=struct.pack('<%df'%n,*values)
        elif kind==8:
            for _ in range(n//32):
                scale=struct.unpack('<e',struct.pack('<e',0.002))[0];quant=[rng.randint(-12,12) for _ in range(32)];payload+=struct.pack('<e32b',scale,*quant);values += [scale*x for x in quant]
        elif kind==12:
            for _ in range(n//256):
                # Scale/min fields exercise both the low-six-bit and packed cases.
                scale=struct.unpack('<e',struct.pack('<e',0.002))[0];minimum=struct.unpack("<e",struct.pack("<e",scale*7))[0];scales=bytes([1]*4+[1]*4+[0x11]*4);quant=bytes(rng.randrange(256) for _ in range(128));payload+=struct.pack('<ee',scale,minimum)+scales+quant
                for group in range(8):values += [scale*((quant[group//2*32+j]>>(4*(group%2)))&15)-minimum for j in range(32)]
        else:
            for _ in range(n//256):
                scale=struct.unpack('<e',struct.pack('<e',0.001))[0];ql=bytes(rng.randrange(256) for _ in range(128));qh=bytes(rng.randrange(256) for _ in range(64));scales=[rng.choice([-2,-1,1,2]) for _ in range(16)];payload+=ql+qh+struct.pack('<16be',*scales,scale)
                for part in range(2):
                    for group in range(4):
                        for j in range(32):
                            low=ql[part*64+j+(group%2)*32];low=low&15 if group<2 else low>>4;q=(low|(((qh[part*32+j]>>(group*2))&3)<<4))-32;values.append(scale*scales[part*8+group*2+j//16]*q)
        offset=(len(data)+31)&~31;data.extend(bytes(offset-len(data)));data.extend(payload);tables.append(text(name)+struct.pack('<I',1 if norm else 2)+struct.pack('<Q',columns)+(b'' if norm else struct.pack('<Q',rows))+struct.pack('<IQ',kind,offset));decoded[name]=(rows,columns,values)
    tensor('token_embd.weight',D,VOCAB)
    tensor('output_norm.weight',D,norm=True)
    for layer in range(LAYERS):
        prefix=f'blk.{layer}.'
        for name in ['attn_norm.weight','ffn_norm.weight']:tensor(prefix+name,D,norm=True)
        for name,rows,cols in [('attn_q',D,D),('attn_k',D*KV//HEADS,D),('attn_v',D*KV//HEADS,D),('attn_output',D,D),('ffn_gate',HIDDEN,D),('ffn_up',HIDDEN,D),('ffn_down',D,HIDDEN)]:tensor(prefix+name+'.weight',cols,rows)
    entries=[]
    for key,value in metadata.items():
        kind=8 if isinstance(value,str) else (6 if isinstance(value,float) else 4);entries.append(text(key)+struct.pack('<I',kind)+(text(value) if kind==8 else struct.pack('<f' if kind==6 else '<I',value)))
    header=b'GGUF'+struct.pack('<IQQ',3,len(tables),len(entries))+b''.join(entries)+b''.join(tables);header=header.ljust((len(header)+31)&~31,b'\0');Path(path).write_bytes(header+data);return decoded
