"""Independent NumPy Llama reference for the GGUF numerical fixture."""
import numpy as np
from cortex_fixture import D,HIDDEN,LAYERS,HEADS,KV,VOCAB

def infer(decoded,steps,prompt,contracts=True,rope_scaling=None):
    weights={name:np.asarray(value[2],dtype=np.float32).reshape(value[0],value[1]) for name,value in decoded.items()}
    key=np.zeros((LAYERS,steps,D*KV//HEADS),dtype=np.float16);value=np.zeros_like(key)
    def rms(x,w):return x/np.sqrt(np.mean(x*x,dtype=np.float32)+np.float32(1e-5))*w[0]
    token=1
    for pos in range(steps):
        if pos<len(prompt):token=prompt[pos]
        x=weights['token_embd.weight'][token].copy()
        for layer in range(LAYERS):
            name=f'blk.{layer}.';normalized=rms(x,weights[name+'attn_norm.weight'])
            q=weights[name+'attn_q.weight']@normalized;k=weights[name+'attn_k.weight']@normalized;v=weights[name+'attn_v.weight']@normalized
            size=D//HEADS
            for vector,heads in [(q,HEADS),(k,KV)]:
                for head in range(heads):
                    for j in range(0,size,2):
                        frequency=np.exp(np.float32(-j/size)*np.log(np.float32(10000)))
                        if rope_scaling=="linear":frequency/=2
                        elif rope_scaling=="llama3":
                            wavelength=2*np.pi/frequency
                            if wavelength>64:frequency/=8
                            elif wavelength>16:
                                smooth=(64/wavelength-1)/3;frequency=frequency*(smooth+(1-smooth)/8)
                        angle=np.float32(pos)*frequency
                        cosine,sine=np.cos(angle),np.sin(angle);a,b=vector[head*size+j:head*size+j+2].copy();vector[head*size+j:head*size+j+2]=[a*cosine-b*sine,a*sine+b*cosine]
            key[layer,pos]=k;value[layer,pos]=v;attention=np.zeros(D,dtype=np.float32)
            for head in range(HEADS):
                kh=head//(HEADS//KV);keys=key[layer,:pos+1,kh*size:(kh+1)*size].astype(np.float32);values=value[layer,:pos+1,kh*size:(kh+1)*size].astype(np.float32)
                score=keys@q[head*size:(head+1)*size]/np.sqrt(np.float32(size));prob=np.exp(score-np.max(score));prob/=np.sum(prob);attention[head*size:(head+1)*size]=prob@values
            x+=weights[name+'attn_output.weight']@attention
            normalized=rms(x,weights[name+'ffn_norm.weight']);up=weights[name+'ffn_up.weight']@normalized;gate=weights[name+'ffn_gate.weight']@normalized;x+=weights[name+'ffn_down.weight']@(up*gate/(1+np.exp(-gate)))
        logits=weights.get('output.weight',weights['token_embd.weight'])@rms(x,weights['output_norm.weight'])
        if contracts:
            mask=np.ones(VOCAB,dtype=bool);mask[[0,3]]=False;logits[mask]=-np.inf
        token=int(np.argmax(logits))
    return token,logits,key,value
