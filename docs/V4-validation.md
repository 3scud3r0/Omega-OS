# Validação V4

Executado em 2026-11-06 no ambiente de desenvolvimento: GCC 14.2,
QEMU 10.0.13, emulação TCG, dois vCPUs por VM. São testes de integração,
não uma prova formal nem medições de desempenho em processador físico.

## Comandos concluídos

```sh
make clean
make -j4
make test-host
make test QEMU=/workspace/tools/bin/qemu-system-x86_64
```

Build do kernel e compilador Ring 3: C99, O3/LTO, freestanding, sem libc,
sem avisos sob `-Wall -Wextra -Werror -pedantic`. `nm -u` dos dois ELFs
não lista dependências externas.

## Evidência nova

- 1.002 execuções do JIT comparadas a `fractions.Fraction`; código executado
  em páginas com mprotect RW→RX no teste host. Página alterada é recusada pela
  auditoria, inclusive alterações de opcodes e bytes fora do comprimento.
- Certificados RFC8032 produzidos por implementação independente e checados
  pelo kernel: recusa de assinatura/payload/identidade/domínio alterados,
  ausência de quórum, duplicação de assinatura e semântica racional inválida.
- Três QEMU em sub-redes 10.21/22/23.0.0/24, com gateways ARP e UDP roteado.
  Payloads de linhagem cifrados; assinaturas e CRC64 do estado em disco
  conferidos fora das VMs.
- Líder isolado conserva epoch lógico zero. Depois de SIGKILL nesse estado,
  recupera exatamente sua proposta durável ainda não publicada.
- Com dois nós disponíveis, o epoch 1 recebe duas assinaturas distintas e
  é publicado. O terceiro nó sofre SIGKILL antes de publicar, reinicia com o
  mesmo disco/identidade e instala o checkpoint certificado dos sobreviventes.
  Um segundo SIGKILL após a publicação recupera o certificado local no boot.
- Mesmo processo compilador retomado, 100 ciclos acumulados, 100 consultas
  de equivalência e-graph/PUCT e epoch 2 replicado nos três discos.
- 16 processos privados simultaneamente em cada burst, incluindo um filho
  que tenta escrever no JIT RX e recebe #PF; os demais filhos continuam.
  Slots, frames e tabelas são reciclados no burst seguinte.
- Falta do JIT cruza a página SLS: escrita parcial é revertida ao checkpoint
  e a incubadora retoma a continuação ELF. HLT adulterado e divisão por zero
  são rejeitados na admissão.

Amostra de telemetria do líder reiniciado no ensaio final:

```text
JIT native TSC=395 reference interpreter TSC=1784
INCUBATOR cycles=50 promotions=22 rejected=3 egraph/PUCT mutations=50
QUORUM recovered durable uncommitted proposal
QUORUM durable lineage epoch=1 voters=0x0000000000000003
JIT native TSC=672 reference interpreter TSC=1829
INCUBATOR cycles=100 promotions=25 rejected=6 egraph/PUCT mutations=100
QUORUM durable lineage epoch=2 voters=0x0000000000000005
```

Valores temporais variam entre execuções. O harness exige melhora medida
naquela execução; não há um fator de aceleração fixo prometido.

## Regressão preservada

Passaram: oito fronteiras SIGKILL de commit/GC, 160 commits multipágina em
imagem de 1 MiB, reboot, fallback de raiz após corrupção, mutação inválida com
checkpoint anterior intacto, compilador original Ômega, isolamento Ring 3,
preempção, rede V3 com adulteração/replay/reboot e recusa do formato V1 sem
escrever no disco. Testes host incluem 40.000 operações racionais, 1.000.000
mensagens SPSC, RFC8032, políticas tipadas e interoperabilidade RFC8439/X25519.

O protocolo de três nós é maioria assinada com líder fixo e honesto,
sem eleição e sem garantia BFT geral. Gerações locais do SLS continuam
independentes dos epochs lógicos certificados de linhagem.
