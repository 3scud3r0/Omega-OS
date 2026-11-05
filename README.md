# Omega-OS V2 — processos preemptivos, raiz transacional e rede

Implementação experimental x86-64, C99 freestanding e sem libc no kernel.
Os cinco pilares agora têm caminhos executáveis e testes de hardware em QEMU.
A documentação da fase anterior permanece em [docs/V1.md](docs/V1.md).

| Pilar | Implementação | Evidência |
|---|---|---|
| Preempção | LAPIC periódico calibrado pelo PIT, quantum ~1 ms, frames completos, seleção round-robin | Agente que nunca cede a CPU executa e é revogado após oito ticks |
| Processos | PML4/PDPT privados, páginas privadas, código RO, dados/pilha NX, TSS/RSP0 | IPC entre processos Ring 3; #PF ao ler página de outro processo ou executar a pilha |
| Syscalls | EFER.SCE, STAR, LSTAR e SFMASK; pilha de kernel; SYSCALL/SYSRET normal e IRET para troca de contexto | 256 mensagens ordenadas e getpid; medição real do custo no convidado |
| Commit multipágina | Árvore radix CoW de três níveis e superblocos de 512 bytes alternados | Dois estados de páginas sempre do mesmo epoch em quatro cortes SIGKILL |
| Autorização/VM | Ed25519, cápsula assinada e verificador tipado de política aritmética | RFC8032, assinatura incorreta, autoridade errada, mutações e políticas inválidas |
| Rede | VirtIO-Net legacy, TX DMA da página do processo e RX com empréstimo de buffer | Duas VMs trocam Ethernet, persistem peer-state e PCAP contém o quadro enviado pelo agente |

## Compilar e executar

Host: GCC x86-64, GNU binutils, Make, Python 3, OpenSSL 3 e QEMU x86.
OpenSSL pertence ao **host de build/testes**, nunca ao runtime bare-metal.
O kernel mantém `-O3 -flto -ffreestanding -nostdlib -fno-builtin -mno-red-zone
-Wall -Wextra -Werror -pedantic`; agentes C são compilados separadamente.
O ELF32 Multiboot contém o trampolim de modo protegido e o kernel de 64 bits.

```sh
make -j2
make run DISK=omega-v2.img
make test
make test-host
make bundle
```

O formato de disco V2 é incompatível com V1. O boot detecta metadados V1 e
recusa sobrescrevê-los; não há migração implementada. Use uma imagem **nova**,
como `DISK=omega-v2.img`. Make cria uma raw de 32 MiB com qemu-img, ou Python se
qemu-img não existir. `make clean` preserva imagens existentes e remove os
artefatos de build, inclusive a autoridade efêmera de desenvolvimento.

Nesta máquina, passe `QEMU=/workspace/tools/bin/qemu-system-x86_64` a `make run`
ou `make test`. Não crie worktrees: reutilize o checkout isolado existente.
Foram usados GCC 14.2 e QEMU 10.0.13 com duas vCPUs/TCG multithread.

## Processos e preempção

`user/agent.c` é C de usuário compilado em código nativo, autenticado e carregado
em cinco espaços de endereçamento. O produtor envia mensagens e entra em loop
infinito; o consumidor verifica FIFO e sai; os outros processos testam memória
privada, NX e rejeição de RSP não canônico sem executar SYSRET inseguro. Cada contexto possui CR3, registradores, stack/data e contador de
quantum próprios. O timer salva o frame, escolhe o próximo processo e troca CR3.
Os processos desta demonstração têm orçamento de oito ticks. Um loop infinito
é terminado, não simplesmente marcado como cooperativo.

A execução de processos fica no **BSP**. O AP conserva o teste SMP da V1 com
65.536 mensagens, depois fica quiescente. Não há migração de processos ou
escalonamento de usuários no AP. Código confiável de kernel executa com IF
mascarado nos handlers; esta fase não implementa preempção do próprio kernel,
NMI watchdog ou garantia de progresso perante falha física de dispositivo.

A tabela de kernel é compartilhada como supervisor. O ramo de usuário de cada
processo é privado; mapeamentos antigos dos testes V1 não ficam acessíveis aos
novos processos. Código usuário não é gravável, e pilha/dados têm NX. Em uma
falta de usuário o contexto é abandonado; faltas do kernel continuam sendo fatais.

ABI: número em RAX, argumento escalar em RDI, resultado em RAX; RCX/R11 são
clobbered. Não se aceita ponteiro arbitrário do usuário. Operações de rede usam
uma página privada fixa, validada pelo kernel e mantida estável até o DMA acabar.

| RAX | Serviço | Resultado |
|---|---|---|
| 0 | getpid | ID do processo |
| 1 | enviar escalar, produtor autorizado | 0 ou UINT64_MAX |
| 2 | receber escalar, consumidor autorizado | valor ou UINT64_MAX |
| 3 | sair | contexto encerrado |
| 4 | enviar frame Ethernet da página 0x80003000; RDI=tamanho | 0 ou UINT64_MAX |
| 7 | consultar MAC disponível | seis bytes em inteiro little-endian, ou 0 |

A volta rápida usa SYSRET somente com RIP/RSP no intervalo canônico permitido e
RFLAGS saneados. Trocas de tarefa usam IRET. Não há promessa de syscall em menos
de 20 ciclos: o teste mede a média de 1.024 chamadas; uma execução sob QEMU
registrou 336 ticks TSC por chamada, sujeitos a emulação e interrupções.

## SLS: commit global e limites físicos

O registro persistente é uma árvore radix de três níveis, fanout 256. Cada
entrada guarda setor e CRC64 do descendente. A capacidade lógica é de 16.777.216
páginas (64 GiB); até 4.096 páginas podem estar registradas/residentes no kernel
atual, cujo PMM continua limitado a 64 MiB. O teste verifica um endereço esparso
no final do primeiro GiB. Isso **não** significa que um GiB inteiro de dados foi
carregado na RAM ou escrito no teste. Não há demand paging.

Uma transação inicia da raiz confirmada, escreve novos dados e caminhos CoW em
blocos append-only e publica uma única raiz para **todas** as páginas alteradas.
Após FLUSH dos dados/árvore, escreve um setor mestre de 512 bytes no superbloco
alternativo e executa outro FLUSH. Só então os dirty bits são limpos. Páginas
não alteradas permanecem referenciadas pela nova raiz.

No boot, validam-se superbloco, árvore inteira e dados, escolhendo o maior epoch
completo. Uma raiz inválida pode levar à raiz anterior; dados compartilhados
entre ambas continuam sendo um ponto comum de falha de mídia. Sem raiz válida,
o kernel falha fechado. Não há formatação automática nem reparo silencioso.

O protocolo assume dispositivo/host obedecendo FLUSH e usa redundância e CRCs
para detectar escritas rasgadas. Os testes não são uma prova de resistência a
qualquer queda elétrica de qualquer hardware. CRC64 não autentica um disco
adversarial. Não há coleta de lixo: o log cresce até a capacidade da imagem e
falha fechado quando se esgota. Não há superbloco de 512 bytes que seja
fisicamente atômico em todos os dispositivos existentes.

## Autoridade externa, Ed25519 e verificação de política

Monocypher **4.0.2** foi incorporado ao fonte, com licença BSD-2-Clause/CC0,
manifesto de origem e hashes originais em `src/crypto/UPSTREAM.json`. A única
adaptação ao upstream troca headers de tipos da biblioteca padrão por typedefs
freestanding. Não há linkagem com biblioteca criptográfica do host.

No build de desenvolvimento, `tests/make_capsule.py` cria uma chave privada
**somente em build/dev-authority.pem**, com modo 0600; ela não entra no kernel,
Git ou TXT. O kernel recebe apenas chave pública, assinatura, código e política.
Recriar o diretório build cria uma nova identidade de desenvolvimento. Isso não
é uma raiz de confiança de produção provisionada.

Para uma autoridade própria, gere a cápsula no host confiável usando
`python3 tests/make_capsule.py --authority-key /caminho/privado.pem` após gerar
`build/agent.bin`. Exporte apenas `build/capsule.h`, mantenha a chave fora do
checkout de distribuição e compile com `AUTHORITY_BUNDLE=/caminho/capsule.h`.
Esse modo não gera chave privada. O header é entrada confiável do build e fixa
a chave pública autorizada; secure boot e rotação de autoridade não estão
implementados. Não envie chaves privadas por chat.

A assinatura cobre domínio/versionamento, tamanhos, código nativo e política.
O verificador autentica a autoridade e rejeita mutações antes do carregamento.
A mini-VM interpreta um formato total, sem saltos, memória ou chamadas, com
pilha máxima de 16 valores e 256 bytes. PUSH/ADD usam inteiros, EQ produz booleano,
ASSERT exige booleano verdadeiro, HALT exige pilha vazia e nenhuma instrução
restante. Overflow, underflow e mistura de tipos são rejeitados.

Essa VM verifica **a política aritmética fornecida**; não prova segurança do
código nativo, não aceita provas Lean e não é um compilador Ômega. O vínculo
entre código e política é a assinatura da autoridade. A tag SipHash antiga
permanece apenas nos testes de estado da V1, sem função de autenticação.

## Rede entre dois nós

VirtIO-Net PCI legacy (1af4:1000) usa filas RX/TX independentes, sem negociar
offloads. O transporte moderno 1041 é detectado e rejeitado explicitamente.
TX usa cadeia de descritores para header e payload; no syscall o payload vem
da página física do agente, sem bounce copy. RX entrega empréstimo do buffer
DMA ao kernel e exige release antes da reposição. Copiar o payload recebido
para estado persistente é uma operação intencional, distinta do caminho RX.

A suíte liga duas VMs por socket Ethernet do QEMU em localhost. Dentro dos
convidados, driver, protocolo e armazenamento são todos bare-metal. Frames
EtherType 0x88b5 implementam anúncio/ACK e confirmam peer-state no SLS. O host
verifica os dois discos e a captura PCAP de TX originado em Ring 3.

Esse protocolo é de demonstração: não implementa IP/UDP/TCP, autenticação dos
frames, proteção de replay, consenso, resolução de conflitos ou replicação
transacional de aplicações. Ed25519 de admissão não autentica a rede.
Sem NIC, o teste de rede é explicitamente omitido; `make test` instala a NIC
nos dois nós e exige a troca de frames e persistência para passar.

## Testes e fontes completos

`make test` usa exclusivamente imagens em build/, sem tocar na imagem do usuário.
Confere Multiboot, inicialização SMP, falhas de hardware, timer não cooperativo,
CR3 privado, NX, syscall/IPC, assinatura/política e rede. Para storage, verifica
CRCs e árvores raw pelo host, força SIGKILL em quatro fronteiras, exige epochs
multipágina coerentes e testa fallback da raiz global após corrupção de um
payload exclusivo do epoch mais recente. Também reinicia o nó com peer-state.

`make test-host` verifica RFC8032, 69 casos negativos de assinatura, tipos e
2.000 políticas aritméticas, mais os 40.000 casos racionais e 1.000.000 mensagens
SPSC herdados. Os testes V1 originais são recuperáveis pelo Git na versão V1;
o formato antigo foi substituído, sem fingir compatibilidade de disco.

`nm -u build/omega64.elf` não deve listar dependências externas. `make bundle`
reúne todos os fontes, inclusive agentes, criptografia, licença e o header
**público** da cápsula, em `omega_os_fontes.txt`.
