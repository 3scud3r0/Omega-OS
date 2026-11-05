# Omega OS — núcleo de hardware V1

Kernel experimental x86-64 C99 freestanding, modular, sem libc, heap ou
bibliotecas de driver. Implementa hardware real emulado: Multiboot, MMU,
exceções, VirtIO-Block PCI legacy, persistência por página e SMP em dois vCPUs.
Não é uma certificação de prontidão industrial ou de segurança.

## Build e execução

Pré-requisitos: GCC x86-64, GNU binutils, Make, Python 3 e QEMU x86.
Em Debian: `apt-get install gcc binutils make python3 qemu-system-x86 qemu-utils`.

```sh
make -j2
make run
make test
make test-host
make bundle
```

`make` cria `disk.img` raw de 32 MiB somente se ainda não existir. Usa `qemu-img`
quando disponível, ou truncamento esparso Python com leitura inicial em zeros.
`make clean` preserva `disk.img`. Os testes usam imagens descartáveis em `build/`.

O alvo `run` usa:

```sh
qemu-system-x86_64 -kernel omega_os.elf \
  -drive file=disk.img,if=virtio,format=raw \
  -smp 2 -m 128M -serial stdio -display none
```

Nesta máquina, QEMU foi instalado localmente, sem sudo, de pacotes Debian
verificados. Use `make run QEMU=/workspace/tools/bin/qemu-system-x86_64` e o mesmo
override em `make test`. O diretório de ferramentas não pertence ao repositório.

Validado com GCC 14.2.0, GNU binutils e QEMU 10.0.13/TCG multithread. Pode-se
selecionar cross-toolchain com `CC=x86_64-elf-gcc OBJCOPY=x86_64-elf-objcopy`;
este cross-compiler específico não foi instalado ou validado aqui.

O Makefile mantém `-std=c99 -ffreestanding -nostdlib -O3 -flto -mno-red-zone
-Wall -Wextra -Werror -pedantic`, desabilita builtins e geração de SIMD no kernel.
O cabeçalho Multiboot fica nos primeiros 8 KiB (offset validado pelo teste).
`link.ld` coloca código em 0x100000 e alinha BSS, IDT e DMA em páginas.
O ELF64 intermediário é convertido em contêiner ELF32 pelo objcopy para o
loader Multiboot do QEMU; o código continua x86-64 após o trampolim de 32 bits.

## Memória e persistência

O PMM mantém o limite de 64 MiB e bitmap hierárquico com bit-scan, reservas do
kernel, rejeição de double-free e validação do limite de RAM Multiboot.
A inicialização pressupõe RAM contígua informada por `mem_upper`, como no PC QEMU
validado; não substitui um parser completo de E820 para máquinas arbitrárias.
A MMU também permite mapear MMIO/ACPI abaixo de 4 GiB, com PCD/PWT, e páginas U/S
para o contexto de usuário. DMA usa buffers estáticos identity-mapped em RAM
coerente x86. Há um request em voo, com três descritores de cabeçalho/dados/status.

PCI usa CF8/CFC, varre barramentos, slots e funções, habilita I/O, bus mastering
e desabilita INTx. O driver detecta 1af4:1001 e negocia `VIRTIO_BLK_F_FLUSH`.
Dispositivos somente modernos (1af4:1042) são identificados mas rejeitados com
mensagem explícita: este driver implementa o transporte **legacy**, não modern
PCI capabilities. `virtio_blk_submit` publica DMA assincronamente;
`virtio_blk_poll` consome o used ring. As funções read/write_page esperam a
conclusão com timeout; FLUSH espera a conclusão e exige suporte do dispositivo.

Duas correções importantes ao layout solicitado:

- O tamanho da fila legacy é anunciado pelo dispositivo e não é configurável
  pelo guest. São aceitos tamanhos potência de dois entre 128 e 1024, com arena
  estática de 32 KiB. O QEMU 10 desta validação anuncia 1024, inclusive quando a
  propriedade moderna `queue-size=128` é solicitada. Somente o tamanho anunciado
  é usado; não foi validado um dispositivo que anuncie 128 nesta máquina.
- O available ring fica **imediatamente depois dos descritores**. O used ring é
  alinhado em 4096. Alinhar cada uma das três estruturas separadamente em páginas
  viola o layout legacy para vários tamanhos. A arena contígua começa em 4096.

`mmu_sls_commit` procura dirty bits de hardware nas páginas registradas no SLS.
As páginas transitórias do kernel não são publicadas no disco. Para cada página
registrada, mantém dois slots alternados:

1. Escreve os 4096 bytes do novo estado no slot inativo; aguarda DMA e FLUSH.
2. Escreve uma página de metadados com magic, geração, endereço virtual, CRC64
   dos dados e CRC64 dos metadados; aguarda DMA e outro FLUSH.
3. Só então avança a geração em RAM, limpa PAGE_DIRTY e executa `invlpg`.

No boot/rollback, lê e valida os dois slots, usando a maior geração válida.
Se há metadados não vazios mas nenhum checkpoint válido, falha explicitamente
em vez de reinicializar o estado silenciosamente. Slots da página de índice i
começam nos setores `8 + 32*i` e `24 + 32*i`; cada um tem 8 setores de metadados
seguidos de 8 setores de dados. Registro e ordem dos endereços são fixos.

Isso implementa publicação copy-on-write **por página**; não há um ponteiro raiz
atômico global, transação atômica entre páginas, WAL completo, coletor de lixo,
filesystem, FUA, NVMe ou Ed25519. CRC detecta corrupção acidental, não ataques.
A durabilidade depende de o backend/hardware cumprir FLUSH. Os testes não provam
comportamento sob todos os cortes elétricos, firmwares, corrupção adversarial
ou falhas simultâneas dos dois slots.

## Exceções e fronteira de privilégio

A IDT tem 256 gates de 16 bytes e alinhamento de 4096. Os 32 stubs de exceção
normalizam error codes, salvam todos os GPRs e alinham a pilha antes do handler C.
GDT, TSS, RSP0 e pilha IST de emergência são individuais para BSP/AP.

O contexto de teste entra de fato em CPL3 com `iretq`, CS=0x23, SS=0x1b e IOPL=0.
Somente código e pilha desse contexto têm folhas PAGE_USER. Testes executam:

- Leitura de página supervisor: #PF presente, CR2=0x100000.
- `out dx,al` na COM1: #GP por privilégio de I/O.
- Leitura de CR3: #GP por instrução privilegiada.
- Leitura de página não mapeada: #PF não presente, CR2=0x60000000.

Para #PF/#GP de um contexto de usuário associado a uma tarefa, o handler restaura
o frame do disco, revalida o contrato, põe a tarefa em quarentena e abandona o
contexto violador, devolvendo o controle à pilha salva do kernel. Os testes
corrompem o estado em RAM antes das faltas e verificam a restauração. Exceções
inesperadas no kernel continuam fatais e imprimem vetor, error code, RIP e CR2.

**Os workers produtor/consumidor SMP ainda rodam em Ring 0.** O contexto Ring 3
é o teste da fronteira de hardware e da recuperação, não um runtime completo
de agentes isolados. Não há syscalls, espaços de endereços por processo,
preempção, escalonamento de contextos de usuário, capacidades ou isolamento DMA.
A tag SipHash com chave pública de demonstração continua sendo integridade de
estado; não é autenticação adversarial ou prova Lean.

## SMP

Descobre RSDP por EBDA/BIOS, valida checksums, percorre XSDT ou RSDT e MADT,
incluindo entradas LAPIC e override de endereço. Seleciona um AP habilitado com
ID xAPIC de 8 bits. Mapeia LAPIC sem cache, checa MSR, envia INIT assert/deassert
e dois SIPIs. Delays usam PIT canal 2, sem presumir uma frequência de TSC.

O trampolim copiado para 0x8000 começa em modo real de 16 bits, entra em modo
protegido, carrega CR3 compartilhado, habilita long mode e usa uma pilha própria
estática. O AP carrega sua GDT/TSS/IDT e sinaliza online via release/acquire.

BSP produz 65.536 mensagens enquanto o AP consome exclusivamente o SPSC e
valida seu estado racional. O BSP verifica a conclusão e os IDs APIC distintos.
Depois, o AP fica em HLT e o BSP realiza commits/isolamento. Não há mudança
concorrente de page tables, shootdown TLB remoto ou scheduler SMP geral.
A implementação usa xAPIC MMIO e rejeita x2APIC ativo; não implementa RISC-V.
No QEMU, IDs distintos são vCPUs, não evidência de teste em duas CPUs físicas.

## Testes e evidência

`make test` executa QEMU com `-accel tcg,thread=multi`, verifica Multiboot, UART,
DMA, bytes diretamente na imagem raw pelo host, epochs de ambas as tarefas,
AP online e os faults Ring 3. Encerra QEMU com SIGKILL e verifica recuperação
no reboot. Corrompe o slot mais recente e verifica o fallback para o anterior.

A suíte também usa `-append sls.crash=N` para parar em quatro fronteiras reais:
após DMA de dados, após FLUSH de dados, após DMA de metadados e após FLUSH de
metadados. O hook, inativo por padrão, apenas pausa o guest em HLT; não simula
sucesso de I/O. O host aplica SIGKILL, verifica os slots no arquivo e reinicia.
São cortes nas fronteiras do protocolo, não uma enumeração de todos os instantes
possíveis durante DMA ou da persistência interna de hardware físico.

Logs e imagens de teste ficam em `build/v1-*.log` e `build/v1-*.img`.
`make test-host` verifica 40.000 operações racionais contra frações exatas Python
e 1.000.000 mensagens em duas threads pthread do host. A libc desses testes
não é vinculada ao kernel (`nm -u build/omega64.elf` deve produzir saída vazia).

O kernel termina em HLT depois de validar os subsistemas. Ciclos TSC são
telemetria, não nanossegundos calibrados, previsão térmica ou garantia de latência.
`omega_os_fontes.txt` reúne fontes, headers, linker, Makefile, testes e este README;
regenere com `make bundle` após mudanças.
