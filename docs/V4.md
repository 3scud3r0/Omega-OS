# Omega-OS V4 — execução nativa e linhagens com maioria assinada

Kernel experimental x86-64 C99 freestanding. O compilador Ômega em Ring 3 gera
código nativo; uma incubadora mantém 16 linhagens tipadas; três nós replicam
checkpoints assinados sobre túneis cifrados. Código e testes executam instruções
reais em QEMU. O projeto não possui uma prova formal completa nem certificação
para produção.

| Mecanismo | Implementação e evidência |
|---|---|
| JIT racional | Emissão x86-64, overflow detectado, MDC Euclidiano e gás; 1.002 execuções nativas comparadas a `Fraction` |
| W^X | Syscall 11, página privada, auditoria canônica completa, RX no usuário e alias físico RO+NX; rearmamento remove execução antes de devolver escrita |
| Incubadora | Pool de 16 genomas; 50 mutações por burst; comparação racional exata; promoção somente com bytes não maiores e ciclos estritamente menores |
| Processos | 16 CR3 privados simultaneamente; filhos executam cópias privadas do código nativo; tabelas e frames dos filhos mortos são reciclados entre bursts |
| Rede e persistência | Propostas/votos/certificados Ed25519, três túneis X25519/ChaCha20-Poly1305, lock de voto durável e checkpoint SLS CoW com FLUSH |
| Recuperação | Líder isolado não publica linhagem; SIGKILL do terceiro nó antes e depois da publicação; recuperação do disco e sincronização online |

Histórico: [V3](docs/V3.md), [V2](docs/V2.md), [V1](docs/V1.md).
Todos os fontes compilados, fontes geradas e cápsulas **públicas** estão em
[omega_os_fontes.txt](omega_os_fontes.txt). Chaves privadas e módulos initrd são excluídos.

## Compilar e testar

Host: GCC x86-64, binutils, Make, Python 3, OpenSSL 3, QEMU e
`cryptography>=44,<51` para provisionamento e referências independentes dos testes.
O kernel e o compilador de usuário usam `-O3 -flto -Wall -Wextra -Werror -pedantic`,
C99 freestanding, sem libc, stack protector, red zone ou SIMD. O pequeno loader
assinado de usuário continua compilado separadamente. Monocypher 4.0.2 tem fontes,
licença e hashes preservados.

```sh
make clean
make -j4
make test-host
make test-v4 QEMU=/workspace/tools/bin/qemu-system-x86_64
make test QEMU=/workspace/tools/bin/qemu-system-x86_64
make bundle
```

O caminho QEMU acima é o wrapper instalado neste ambiente; outra máquina pode
usar `QEMU=qemu-system-x86_64`. Reutilize o checkout isolado existente; não crie
worktrees para executar o projeto. Nenhum serviço do host precisa ficar ativo.
`make run DISK=omega-v4.img` executa o ensaio local de 50 ciclos, depois HLT.
`make clean` preserva as imagens de disco do usuário; testes substituem somente
os próprios arquivos em `build/`.

`make test-v4` inicia três VMs em 10.21/22/23.0.0/24, com gateway ARP e roteamento
IPv4 no harness. Verifica a página no disco com parser CRC64 independente e
confere as assinaturas com a biblioteca `cryptography` do host. Compara o JIT
com o avaliador V3 usando a mesma expressão e relógio, sem impor um ganho fixo.
Valida dois epochs e 100 ciclos no mesmo processo compilador, incluindo reciclagem
de processos. O teste rejeita qualquer ensaio no qual o tempo medido não melhore.
A suíte completa preserva as oito fronteiras SIGKILL do SLS, GC de 160 commits
num disco de 1 MiB, corrupção, isolamento, rede V3 e recusa do formato V1.

## Emissão nativa, auditoria e recuperação

O frontend original `compiler.omega` continua executando no convidado. Sua IR
escalar é convertida em instruções x86-64: literais `movabs`, soma/subtração,
`imul` com teste de overflow, divisão e redução por MDC. O certificado tipado
linear continua existindo como formato de admissão e referência de resultados;
o filho executa a função nativa diretamente, sem interpretador no seu caminho
de execução. Não se trata de remover o formato de provas/certificados.

A syscall 11 aceita exclusivamente a página JIT 0x83000000 do compilador e um
comprimento válido. O kernel reproduz o encoder e compara **todos os 4096 bytes**,
incluindo saltos internos e a cauda INT3. Instruções fora dos templates são
rejeitadas; não há busca ingênua por substrings em constantes. A disciplina de
entrada única depende do ELF assinado do compilador. Isso não é um sandbox SFI
para x86 arbitrário: processos continuam dependendo de MMU, Ring 3 e preempção.
Syscalls verificam o PC do ELF e a pilha antes de realizar qualquer serviço.

Após os testes legados de isolamento, a MMU ativa proteção estável: texto do
kernel RX, rodata RO+NX, dados RW+NX, código de usuário RX e alias físico RO+NX.
As permissões transitórias do bootstrap são aposentadas antes do runtime.
O AP permanece quiescente; o escalonador de processos roda no BSP. Não existe
um protocolo geral de shootdown para processos executando em vários cores.

O código tem no máximo uma página, operandos em pilha de profundidade 32 e
certificado de até 256 operações. Operações e iterações do MDC consomem gás
compartilhado de 2048. Overflow ou divisão indefinida falham fechado; o backend
pode recusar intermediários que um algoritmo com cancelamento prévio aceitaria.
A ABI de saída é uma fração reduzida int64/int64. Não há ponto flutuante.

Uma chamada guardada registra no kernel a continuação do ELF e a pilha antes
de executar o JIT. #PF/#GP/#UD/#DE dentro dessa região restaura a página cognitiva
do último checkpoint e retoma a continuação com falha. O teste cruza o limite
da capacidade SLS: a primeira escrita altera a página; a segunda gera #PF.
A incubadora continua e o checkpoint anterior é restaurado. Outros processos
infratores continuam sendo colocados em quarentena pelo runtime normal.

## Incubação, cotas e continuidade

A gramática de mutação é finita: anel racional escalar, comutação, distribuição
e eliminação de neutros/folding para expressões equivalentes a 24. Cada ciclo consulta o e-graph original para checar a equivalência polinomial e
a extração de constante, e executa o PUCT racional para orientar a forma.
A arena temporária e o gás do frontend são reiniciados entre essas consultas;
os genomas copiados no pool não contêm ponteiros para a arena.
Esta versão não demonstra evolução aberta, novos teoremas Lean ou aprendizagem
neural. Genomas são estruturas e expressões tipadas, não mutações de bytes.

O ancestral expandido calcula efetivamente as operações neutras. Seus extratos
menores são medidos por RDTSCP/LFENCE quando disponível; há fallback serializado
RDTSC para CPUs sem RDTSCP. O QEMU de rede usa `-cpu max`. A medição usa o menor
de sete lotes de 32 execuções, reduzindo interrupções na comparação. Não há
números de ciclos fabricados nem garantia de tempo físico em nanossegundos.
Se nenhum mutante dominar o ancestral no primeiro burst, o ensaio falha.

O processo compilador mantém arena e pool entre bursts. Depois de um checkpoint
certificado, o serviço de rede retoma esse mesmo contexto Ring 3 para mais
50 ciclos, devolvendo o controle ao kernel entre bursts. São criados 15 filhos
simultâneos, com código/pilha/dados privados, ao lado do compilador. Os filhos
mortos são desmontados antes de reutilizar seus slots. As cotas de timer são
renovadas por burst; o gás do JIT e a arena estática limitam cada candidato.
Esse caminho não é um `fork` POSIX geral nem oferece alocação ilimitada.

## Consenso: contrato de segurança explícito

**Dois votos em três nós não constituem BFT geral com um nó bizantino.**
Dois quóruns podem se intersectar somente no nó malicioso. Esta implementação
oferece maioria assinada com líder fixo (nó 1), assumido honesto, e locks de voto
duráveis. Tolera a indisponibilidade de um seguidor. Não possui eleição de líder,
view change ou progresso se o líder falhar. Um protocolo BFT clássico para f=1
requer pelo menos quatro réplicas e quórum três, além das fases de consenso.

Cada cluster tem três identidades X25519/Ed25519 distintas e chaves públicas
fixadas no módulo de boot. As chaves privadas ficam em BSS após provisionamento,
sem segredo estático dentro do ELF. O módulo original e seeds temporários são
apagados. `tools/cluster_config.py --output-dir build/meu-cluster` gera módulos
privados 0600, recusando sobrescrita; guarde-os para reiniciar as mesmas identidades.
O teste fornece esses módulos por `-initrd` e um roteador de laboratório.

Uma proposta contém epoch lógico, hash anterior e hash BLAKE2b do manifesto
completo de linhagens. O voto Ed25519 cobre domínio, identidade do cluster,
epoch e ambos os hashes. O seguidor valida conteúdo e assinatura do líder,
grava seu lock no SLS com FLUSH e somente então envia o voto. O certificado
exige duas identidades distintas, incluindo o líder. O kernel verifica esse
certificado novamente ao persistir a página de linhagem 0x40006000.

O checkpoint distribuído identifica **conteúdo lógico**, porque os LBAs e
raízes físicas CoW são locais a cada disco. Os demais metadados locais (boot,
floors anti-replay, locks de voto e checkpoints de trabalho) continuam podendo
ser gravados sem quórum. Bloqueá-los impediria iniciar o protocolo. O epoch
lógico da linhagem e a geração física do SLS são contadores diferentes.
Nenhuma linhagem é publicada sem certificado; mutantes locais não publicados
podem existir em RAM. A proposta do líder também é registrada antes do lock: após reiniciar, ele
retoma exatamente o mesmo conteúdo, sem assinar uma proposta conflitante.
O certificado e o manifesto são publicados na mesma
transação CoW da V3. Um nó atrasado pode instalar um checkpoint completo
certificado mais novo, sem reproduzir LBAs ou todo o histórico do par.

As sessões mantêm contadores de boot e floors anti-replay duráveis por par.
Há retransmissão e reassembly limitado a quatro fragmentos. Os nós preservam o
serviço após publicação para recuperar pares reiniciados. O ensaio usa SIGKILL
real, mas não simula firmware de SSD mentindo sobre FLUSH, rollback malicioso
de discos, comprometimento do kernel ou extração física de chaves.
