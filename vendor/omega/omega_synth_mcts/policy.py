"""Proposal guides have no admission authority; malformed priors are rejected."""
import math


def features(partial,variables):
    from .grammar import size,holes
    operations={'+':0,'-':0,'*':0,'/':0}
    def visit(node):
        if node[0] in operations:operations[node[0]]+=1
        for child in node[1:]:
            if isinstance(child,(tuple,list)):visit(child)
    visit(partial)
    return [size(partial)/63,holes(partial)/32,len(variables)/32,*[operations[op]/32 for op in operations],1.0]


class HeuristicPolicy:
    def __init__(self):self.statistics={}
    def priors(self,partial,productions,variables):
        weights=[]
        for label,_ in productions:
            visits,reward=self.statistics.get(label,(0,0.0))
            base=2.0 if label.startswith('var:') else 1.2 if label.startswith('const:') else 1.0
            weights.append(base*(1+reward/max(1,visits)))
        total=sum(weights)
        return [w/total for w in weights]
    def update(self,labels,reward):
        for label in labels:
            visits,total=self.statistics.get(label,(0,0.0));self.statistics[label]=(visits+1,total+reward)


class NeuralPolicy:
    """Tiny optional PyTorch guide, trained on grammar decisions and rewards."""
    def __init__(self,labels,*,seed=0):
        import torch
        if not labels or len(set(labels))!=len(labels) or len(labels)>128:raise ValueError('Bounded unique production labels required')
        self.labels=tuple(labels);self.index={v:i for i,v in enumerate(labels)}
        with torch.random.fork_rng():
            torch.manual_seed(seed);self.network=torch.nn.Sequential(torch.nn.Linear(8,32),torch.nn.Tanh(),torch.nn.Linear(32,len(labels)))
        self.optimizer=torch.optim.Adam(self.network.parameters(),lr=0.01)
        self.history=[]
    def priors(self,partial,productions,variables):
        import torch
        with torch.no_grad():logits=self.network(torch.tensor(features(partial,variables),dtype=torch.float32))
        selected=[float(logits[self.index[label]]) if label in self.index else 0.0 for label,_ in productions]
        maximum=max(selected);weights=[math.exp(v-maximum) for v in selected];total=sum(weights)
        return [v/total for v in weights]
    def train(self,observations,*,epochs=8):
        import torch
        if type(epochs) is not int or not 1<=epochs<=128 or not 1<=len(observations)<=4096:raise ValueError('Training budget exceeded')
        vectors=[];targets=[]
        for vector,label in observations:
            if len(vector)!=8 or any(not math.isfinite(float(x)) for x in vector) or label not in self.index:raise ValueError('Invalid policy observation')
            vectors.append(vector);targets.append(self.index[label])
        x=torch.tensor(vectors,dtype=torch.float32);y=torch.tensor(targets)
        for _ in range(epochs):
            self.optimizer.zero_grad();loss=torch.nn.functional.cross_entropy(self.network(x),y)
            if not torch.isfinite(loss):raise ValueError('Nonfinite guide loss')
            loss.backward();self.optimizer.step();self.history.append(float(loss.detach()))
        return tuple(self.history[-epochs:])
    def update(self,labels,reward):pass
