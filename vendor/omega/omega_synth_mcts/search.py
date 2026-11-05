"""PUCT over partial Omega ASTs with independent universal admission."""
from dataclasses import dataclass,field
from fractions import Fraction as Q
import math
import random
import time
from omega_refine_check.syntax import parse,evaluate,rational,render,substitute
from omega_refine_check.contracts import verify_contract
from .grammar import Grammar,holes,size
from .policy import HeuristicPolicy
from .pruning import LeanPruner


@dataclass
class Node:
    ast:tuple
    prior:float=1.0
    label:str='root'
    visits:int=0
    value:float=0.0
    children:list=field(default_factory=list)
    expanded:bool=False


def specification(spec):
    fields={'variables','ensures','examples','constants','requires','operators'}
    if type(spec) is not dict or not {'variables','ensures','examples'}<=set(spec)<=fields:raise ValueError('Explicit synthesis contract and examples required')
    variables=spec['variables']
    if type(variables) is not dict or len(variables)>32 or type(spec['examples']) is not list or not 1<=len(spec['examples'])<=64:raise ValueError('Problem budget exceeded')
    normalized=[]
    for observation in spec['examples']:
        if type(observation) is not dict or set(observation)!={'inputs','output'} or set(observation['inputs'])!=set(variables):raise ValueError('Example schema mismatch')
        env={v:rational(q) for v,q in observation['inputs'].items()}
        for v,p in variables.items():
            if evaluate(parse(p),{**env,'self':env[v]}) is not True:raise ValueError('Example violates variable contract')
        if any(evaluate(parse(p),env) is not True for p in spec.get('requires',[])):raise ValueError('Example violates precondition')
        target=rational(observation['output'])
        if evaluate(parse(spec['ensures']),{**env,'result':target}) is not True:raise ValueError('Example target violates requested postcondition')
        normalized.append((env,target))
    return normalized


def search(spec,*,iterations=1024,max_nodes=15,max_depth=8,seconds=30,seed=0,policy=None,verify=True,max_proof_checks=16):
    if type(iterations) is not int or not 1<=iterations<=100000 or type(seconds) not in (int,float) or not math.isfinite(seconds) or not 0<seconds<=3600 or type(seed) is not int:
        raise ValueError('MCTS execution budget exceeded')
    if type(max_proof_checks) is not int or not 1<=max_proof_checks<=128:raise ValueError('Proof budget exceeded')
    examples=specification(spec)
    grammar=Grammar(spec['variables'],spec.get('constants',[0,1,-1]),operators=spec.get('operators',['+','-','*','/','neg','relu']),max_nodes=max_nodes,max_depth=max_depth)
    guide=policy or HeuristicPolicy();pruner=LeanPruner(spec['variables'],enabled=verify)
    root=Node(('hole',));rng=random.Random(seed);started=time.monotonic()
    considered=set();rejected=[];proof_checks=0;pruned=0;evaluations=0;expansions=0;best=None;best_loss=None
    for iteration in range(iterations):
        if time.monotonic()-started>=seconds:break
        node=root;path=[node]
        while node.expanded and node.children:
            node=max(node.children,key=lambda n:(n.value/max(1,n.visits)+1.4*n.prior*math.sqrt(max(1,path[-1].visits))/(1+n.visits),rng.random()))
            path.append(node)
        if holes(node.ast) and not node.expanded:
            options=grammar.productions(node.ast);priors=guide.priors(node.ast,options,grammar.variables)
            if len(priors)!=len(options) or any(type(p) not in (int,float) or not math.isfinite(p) or p<0 for p in priors) or sum(priors)<=0:raise ValueError('Invalid neural/heuristic proposal distribution')
            total=sum(priors)
            for (label,ast),prior in zip(options,priors):
                if pruner.prune(ast):pruned+=1;continue
                node.children.append(Node(ast,float(prior/total),label))
            node.expanded=True;expansions+=len(options)
            if node.children:
                node=rng.choices(node.children,weights=[max(n.prior,1e-12) for n in node.children])[0];path.append(node)
        candidate=grammar.complete(node.ast,rng) if holes(node.ast) else node.ast
        try:
            loss=sum((abs(evaluate(candidate,env)-target) for env,target in examples),Q(0))
            if max(abs(loss.numerator).bit_length(),loss.denominator.bit_length())>16384:raise ValueError('Fitness arithmetic budget exceeded')
            reward=1/(1+float(loss))
        except (ValueError,ZeroDivisionError,OverflowError,TypeError):loss=None;reward=0.0
        evaluations+=1
        if loss is not None and (best_loss is None or loss<best_loss):best_loss=loss;best=candidate
        for visited in path:visited.visits+=1;visited.value+=reward
        guide.update([n.label for n in path[1:]],reward)
        key=repr(candidate)
        if loss==0 and key not in considered:
            considered.add(key)
            if not verify:
                return {'candidate':candidate,'admission':None,'verified':False,'evaluations':evaluations,'expansions':expansions,'pruned':pruned,'seed':seed}
            proof_checks+=1
            try:
                admission=verify_contract(spec['variables'],candidate,spec['ensures'],requires=spec.get('requires',()))
                return {'candidate':candidate,'source':render(candidate),'verified':True,'admission':admission,
                        'evaluations':evaluations,'expansions':expansions,'pruned':pruned,'pruning_certificates':pruner.certificates,
                        'proof_checks':proof_checks,'rejected':rejected,'seed':seed,'optimality_proved':False}
            except ValueError as error:rejected.append({'candidate':candidate,'reason':str(error)[-2000:]})
            if proof_checks>=max_proof_checks:break
    return {'candidate':None,'verified':False,'evaluations':evaluations,'expansions':expansions,'pruned':pruned,
            'pruning_certificates':pruner.certificates,'proof_checks':proof_checks,'rejected':rejected,
            'best_observed':best,'best_loss':str(best_loss),'seed':seed,'reason':'search or proof budget exhausted','optimality_proved':False}


def synthesize(spec,directory,**limits):
    import json
    from pathlib import Path
    result=search(spec,**limits)
    root=Path(directory);root.mkdir(parents=True,exist_ok=False)
    (root/'search.json').write_text(json.dumps(result,sort_keys=True,indent=2)+'\n')
    if result['verified']:
        (root/'contract.lean').write_text(result['admission']['source'])
        lines=[]
        first=spec['examples'][0]['inputs']
        for name,value in first.items():lines.append(f'parametro {name}={render(("const",str(rational(value))))};')
        lines += ['tensor result='+result['source']+';','mostrar result;']
        source=root/'candidate.omega';source.write_text('\n'.join(lines)+'\n')
        from omega_selfhost.driver import build,run
        build(source,root/'candidate');observed,native=run(root/'candidate')
        if Q(observed['values']['result'])!=rational(spec['examples'][0]['output']):raise ValueError('Native synthesized result mismatch')
        result['native_admission']=native
    return result


def isolated_search(spec,*,budget=None,**limits):
    """No-network AST proposal process, followed by independent host admission."""
    import json,sys,tempfile
    from pathlib import Path
    from omega_platform.sandbox import run
    from omega_platform.budget import Budget
    from omega_refine_check.syntax import tree
    from dataclasses import asdict
    specification(spec)
    if set(limits)-{'iterations','max_nodes','max_depth','seconds','seed'}:raise ValueError('Unknown isolated search limit')
    budget=budget or Budget(wall_seconds=60,cpu_seconds=45)
    package=Path(__file__).resolve().parent;root=package.parent
    with tempfile.TemporaryDirectory(prefix='omega-mcts-proposal-') as temporary:
        path=Path(temporary)/'problem.json';path.write_text(json.dumps({'spec':spec,'limits':limits}))
        mounts=[root/name for name in ('omega_synth_mcts','omega_refine_check','omega','omega_selfhost','omega_platform')]
        outcome=run([sys.executable,'-E','-S','-B',package/'worker.py',path],
                    readonly=[*mounts,path,Path(sys.base_prefix)/'lib'],budget=budget,check=True)
    proposal=json.loads(outcome.stdout)
    if type(proposal) is not dict or proposal.get('verified') is not False:raise ValueError('Invalid proposal envelope')
    if proposal.get('candidate') is None:return {**proposal,'sandboxed':outcome.sandboxed,'budget':asdict(budget)}
    candidate=tree(proposal['candidate'])
    if size(candidate)>limits.get('max_nodes',15):raise ValueError('Proposal exceeds host AST budget')
    examples=specification(spec)
    if any(evaluate(candidate,env)!=target for env,target in examples):raise ValueError('Proposal fails host replay')
    admission=verify_contract(spec['variables'],candidate,spec['ensures'],requires=spec.get('requires',()))
    return {**proposal,'candidate':candidate,'source':render(candidate),'verified':True,'admission':admission,
            'sandboxed':outcome.sandboxed,'budget':asdict(budget),'optimality_proved':False}
