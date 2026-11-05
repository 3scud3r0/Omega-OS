# Omega-OS V3 — compactação, túnel e compilação no convidado

Núcleo experimental x86-64 C99 freestanding. Esta fase implementa quatro
mecanismos com execução e testes reais, sem declarar soberania total, prova
formal do sistema ou imunidade geral a falhas de hardware.

| Ponto | Implementação | Evidência |
|---|---|---|
| GC do SLS | Compactação CoW em duas regiões, com aposentadoria durável das raízes antigas | 160 commits multipágina numa imagem fixa de 1 MiB; reboot; oito fronteiras SIGKILL |
| Rede protegida e roteamento | X25519 estático/efêmero, ChaCha20-Poly1305, janela anti-replay, IPv4/UDP e ARP de gateway | Duas VMs em sub-redes diferentes, injeção de falsificações/replays, reboot e página replicada |
| Compilador no Ring 3 | `compiler.omega` original, runtime freestanding, modo IR, backend de bytecode e admissão delegada | Texto recebido após boot gera uma cápsula e um novo PID sem ferramentas do host |
| Motores Ômega | E-graph original, PUCT com priors tensoriais racionais e aritmética exata numa página SLS mapeada | Extração equivalente `x`, síntese de `2*x+1`, resultado `1/2` e checkpoint persistente |

Histórico: [V2](docs/V2.md) e [V1](docs/V1.md). Todos os fontes, incluindo
proveniência, fonte gerada e cápsulas **públicas**, estão em `omega_os_fontes.txt`.

## Build e validação

Host: GCC x86-64, GNU binutils, Make, Python 3, OpenSSL 3 e QEMU x86.
Os testes de interoperabilidade e a ferramenta de provisionamento usam
`cryptography>=44,<51` no host. Nada disso é linkado ao runtime bare-metal.
Monocypher 4.0.2 permanece incorporado ao fonte, com licença e hashes originais.

```sh
make -j2
make run DISK=omega-v3.img
make test
make test-host
make bundle
```

Nesta máquina: `make test QEMU=/workspace/tools/bin/qemu-system-x86_64`.
Reutilize o checkout isolado existente; não crie worktrees para executar o projeto.
O build mantém LTO, `-Werror -pedantic`, `-ffreestanding -nostdlib -fno-builtin`
e ausência de red zone/SIMD no kernel. O seed de usuário usa C99 freestanding e
modelo large para o ELF do compilador acima de 2 GiB.

Os testes criam apenas imagens em `build/`, nunca sobrescrevem a imagem do
usuário e encerram somente os emuladores que iniciaram. `make clean` preserva
as imagens do usuário. O formato V1 continua sendo recusado sem alteração.
V2 é legível quando a árvore e o cursor cabem integralmente na primeira região;
logs V2 que já atravessam a metade da imagem exigem migração offline, não
compactação destrutiva. Não há ferramenta de migração nessa entrega.

## 1. Compactação CoW e recuperação

O disco é dividido em duas regiões após os superblocos. Cada transação continua
fazendo CoW e publicação atômica global, sob as hipóteses de FLUSH descritas na V2.
Quando falta espaço para a próxima transação, o GC percorre a árvore, conta o
conjunto vivo e o copia para a outra região. Não existe tabela de objetos
mortos crescente nem alocação de RAM proporcional ao tamanho do disco.

Antes de reutilizar a região de destino, o kernel publica a raiz corrente em
**ambos** os superblocos, com FLUSH após cada publicação. Assim nenhuma raiz
retida referencia a região que será sobrescrita. Copia dados e nós, executa
FLUSH e só então publica a raiz compactada. Uma queda durante a cópia mantém
a raiz antiga alcançável. Os setores obsoletos podem então ser reutilizados.

O consumo físico permanece limitado para um conjunto vivo e orçamento de
transação que caibam numa região. Não é possível armazenar crescimento ilimitado
num disco finito. O GC é stop-the-world no BSP, tem custo proporcional ao conjunto
vivo e não oferece latência de tempo real. Usa reserva conservadora para todos
os frames registrados. Conjunto vivo grande demais, erro de I/O ou esgotamento
de contadores falha fechado. Não há descarte seguro de dados antigos nem gestão
de bad blocks/desgaste físico.

A árvore mantém capacidade lógica de 64 GiB; o PMM ainda é de 64 MiB, com até
4.096 páginas registradas. Capacidade lógica não equivale a RAM residente ou
capacidade útil de uma imagem de 32 MiB. CRC64 detecta corrupção, não autentica
disco adversarial. SIGKILL em oito fronteiras não é prova universal de tolerância
a qualquer corte elétrico ou comportamento de firmware.

Os hooks `sls.crash=1..4` cobrem escrita/publicação normal; `5..8` cobrem
aposentadoria de raízes, cópia do GC, FLUSH e publicação compactada.
`sls.stress` executa 160 commits adicionais para verificar reutilização real.

## 2. Túnel autenticado sobre IPv4/UDP

O transporte usa VirtIO-Net legacy, IPv4 sem fragmentação, UDP com checksum,
ARP e seleção de next-hop por máscara/gateway. O teste usa **duas sub-redes**,
10.21.0.0/24 e 10.22.0.0/24, com um roteador de teste entre elas. Não depende de
VPN externa ou pilha Linux no convidado. Não foi validado um enlace na Internet
pública; endereçamento, encaminhamento e eventuais portas NAT pertencem ao
ambiente onde os nós forem implantados.

Cada nó recebe uma identidade X25519 e a chave pública fixada do par por módulo
Multiboot/initrd confiável. A chave privada não fica no ELF, Git ou TXT. O módulo
original é apagado da RAM após a cópia; a chave de identidade é apagada depois
da derivação inicial. RDRAND fornece entropia efêmera e nonce de boot. Ausência
ou falha dessa fonte encerra a inicialização do túnel, sem fallback por relógio.
Os testes usam `-cpu max` para expor essa capacidade no QEMU.

O handshake autentica chave efêmera, nonce, IP e contador de boot com BLAKE2b
keyed derivado do DH estático. O DH efêmero e o transcript produzem chaves
direcionais. Cada pacote usa ChaCha20-Poly1305 IETF com contador único, AAD e
janela deslizante de 64 números de sequência. Só um MAC válido avança a janela.
HELLO repetido não reinicializa contadores. Boot counters e fingerprints do par
são persistidos antes da aceitação dos dados da sessão; novo boot usa novo
transcript e novas chaves. O estado secreto de sessão é apagado ao terminar.

É um protocolo experimental próprio, **não** uma implementação interoperável
ou auditada de Noise/WireGuard. Testes de primitivas e de ataques selecionados
não constituem prova criptográfica do protocolo completo. Não há PKI automática,
NAT traversal, DHCP, DNS, IPv6, mesh com descoberta, consenso ou proteção contra
DoS. A confiança depende do provisionamento/bootloader e da entropia do hardware;
não há TPM, secure boot ou defesa contra host malicioso lendo a RAM.

A aplicação transfere uma página cognitiva inteira em quatro fragmentos
cifrados, com epoch e hash BLAKE2b do snapshot. Somente após reassemblagem e
checagem ela é confirmada no SLS do receptor. Repetições são idempotentes e
há retransmissão limitada por timeout. Essa é replicação de snapshot de aplicação,
não consenso nem sincronização automática de toda a árvore lógica de 64 GiB.

O ciphertext é produzido no buffer DMA de TX. RX usa empréstimo do buffer;
a cópia para estado persistente é intencional. Quando o túnel está configurado,
a syscall de Ethernet bruto fica desabilitada para impedir bypass em texto claro.
Sem NIC, a rede é explicitamente omitida; `make test` exige os dois nós com NIC.

### Provisionar os pares

Execute em máquina confiável e troque apenas os arquivos públicos:

```sh
python3 tools/peer_config.py keygen --private /secure/a.key --public a.pub
python3 tools/peer_config.py keygen --private /secure/b.key --public b.pub
python3 tools/peer_config.py config --private-file /secure/a.key \
  --peer-public-file b.pub --local-ip 10.21.0.2 --peer-ip 10.22.0.2 \
  --gateway 10.21.0.1 --prefix 24 --output /secure/a.conf
```

Gere a configuração inversa para B. Os arquivos de configuração contêm material
privado e são criados em modo 0600; não publique esses arquivos. Passe
`-initrd /secure/a.conf -cpu max` ao QEMU com NIC VirtIO e conectividade Ethernet
adequada. O módulo tem 128 bytes: domínio, tamanho, IPv4/portas, chave privada,
chave pública do par e checksum BLAKE2b. A política confia no carregador desse
módulo, não em um checksum como autenticação de provisionamento.

## 3. Compilação dentro do Ring 3

Fontes originais: `3scud3r0/Omega`, commit
`47dd6a20f1699ac6e54ece82397ae64fa78493c4`. Estão preservados em `vendor/omega`,
com hashes e manifesto de derivação. `tests/bootstrap_omega.py` usa o seed
original **somente no build inicial**, para gerar C do compilador e do e-graph.
O convidado executa a entrada `principal` original no modo `--ir`, com entrada
e saída em memória e um runtime freestanding de valores.

Fluxo validado após o boot:

1. O produtor recebe texto numa página privada e o envia ao processo compilador.
2. O compilador original faz lexing, parsing e checagem de formas; o backend do
   convidado emite bytecode escalar racional.
3. A admissão verifica formato, gás, pilha, normalização, overflow e divisão
   por zero. Somente o compilador autenticado tem a capacidade de delegar.
4. Um novo espaço de endereçamento/PID executa o bytecode em Ring 3 e verifica
   seu resultado contra o certificado calculado na admissão.

O seed do compilador é um ELF autenticado por Ed25519. A nova cápsula recebe
**delegação restrita**, não uma assinatura fabricada com chave privada dentro
do kernel. Ela não permite carregar código nativo arbitrário. Nenhum GCC,
Python, OpenSSL ou linker é chamado durante essa sequência no convidado.

O backend dinâmico aceita escalares, variáveis, `+ - * /` e negação, com pilha
32, até 256 operações e cápsula de até 4 KiB. A aritmética é racional exata
limitada a int64; overflow é rejeitado. O runtime de valores possui arena fixa
de 8 MiB, identificadores ASCII e limites de JSON/recursão. Não é GMP nem a
linguagem Ômega inteira. Não há compilação do próprio kernel, backend nativo
completo ou fechamento integral da auto-hospedagem de todas as dependências.

O exemplo executa um trabalho de compilação e cria um filho, sob orçamento.
Não é um serviço ilimitado de spawn/exec. Mutações inválidas são colocadas em
quarentena e preservam o checkpoint anterior, em vez de autorizar código falso.
Para testar outro texto sem rebuild:

```sh
# acrescente ao comando QEMU normal:
-append 'omega.source=tensor result = (1/3+1/6)*12; mostrar result;'
```

Os testes verificam `24`, `6`, `-9/4`, `5` e a rejeição de divisão por zero.

## 4. Motores e estado cognitivo exato

O e-graph executa o `execute()` original do Ômega: union-find, congruência,
reescritas de anel, extração por custo e verificação polinomial independente.
O exemplo `(x+0)*1` resulta em `x`; há orçamento de 64 e-nodes e quatro rodadas.
Não se afirma ótimo global ou saturação de programas arbitrários.

A busca PUCT é uma adaptação racional finita de `omega_synth_mcts/search.py`:
gramática afim `a*x+b`, coeficientes -2..2, priors de um tensor racional 2x3,
exploração por raiz inteira e até 192 iterações. Encontra `2*x+1` e confere os
coeficientes independentemente. Não porta PyTorch, CUDA, o guia neural completo
ou os certificados Lean do repositório original.

O compilador recebe uma capacidade explícita para uma página SLS: alias privado
0x80004000 e alias supervisor 0x40005000. Calcula `1/3+1/6=1/2` diretamente nos
operandos/resultados dessa página. A confirmação valida e canonicaliza o registro,
que inclui resultado compilado, e-graph, PUCT e resultado do novo filho. Os dois
relatórios precisam estar completos antes de publicar o checkpoint.

A rede replica essa página como dados, sem executá-la nem tratá-la como prova
formal. O restante dos processos continua privado; a execução de usuários fica
no BSP, com AP usado para o teste SMP da base. Não há migração de processos,
agendamento por energia ou prova Lean da máquina inteira.

## Testes

`make test` recompõe o caminho de hardware: oito fronteiras SIGKILL, GC em disco
limitado, fallback de raiz após corrupção de payload exclusivo, reboot, três fontes alternativas, mutação inválida com checkpoint
preservado, dois nós roteados, falsificação/replay, reassemblagem, persistência
e reboot dos contadores. Os discos são inspecionados independentemente no host.

`make test-host` conserva RFC8032, testes de admissão/racionais/SPSC e acrescenta
interoperabilidade X25519/RFC8439 com `cryptography`, janela anti-replay,
reordenação, reboot e limites do bytecode. Teste passado é evidência delimitada,
não certificação industrial. O histórico V2 é recuperável no Git; seu runner de
rede em texto claro foi substituído, sem alegar compatibilidade daquele protocolo.

`nm -u build/omega64.elf` e `nm -u build/compiler.elf` não devem listar dependências
externas. `make bundle` inclui fontes gerados e cápsulas públicas; exclui as
identidades/configurações privadas de rede e a chave Ed25519 de desenvolvimento.
