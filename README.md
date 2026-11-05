# Omega-OS — núcleo experimental V1

Kernel x86-64 modular C99 freestanding, sem libc, heap ou dependências de runtime.
Esta fase acrescenta hardware real ao protótipo: IDT/TSS, contextos de teste em
Ring 3, armazenamento VirtIO PCI por DMA e um Application Processor iniciado
por INIT-SIPI-SIPI. Não é um sistema industrial certificado.

## Build e execução

Pré-requisitos do host: GCC x86-64, GNU binutils, Make, Python 3 e QEMU x86.
`qemu-img` é usado se disponível; o fallback Python cria a mesma imagem raw de
32 MiB com leitura inicial em zeros. A imagem existente nunca é recriada por
`make` ou removida por `make clean`.

```sh
make -j2
make run
# equivalente:
qemu-system-x86_64 -kernel omega_os.elf \
  -drive file=disk.img,if=virtio,format=raw \
  -smp 2 -m 128M -serial stdio -display none
make test
make test-host
make bundle
```

No ambiente atual, use `QEMU=/workspace/tools/bin/qemu-system-x86_64` nos alvos
`run` e `test`. QEMU foi instalado localmente a partir de pacotes Debian
verificados, sem sudo. Reutilize o checkout existente; não crie worktrees para
executar ou validar este kernel.

O Makefile mantém `-O3 -flto -Wall -Wextra -Werror -pedantic`, `-nostdlib`,
`-ffreestanding -fno-builtin -mno-red-zone` e proíbe geração de instruções SIMD
nos handlers. Build validado com GCC 14.2 e QEMU 10.0.13/TCG multithread.
O cross-compiler é selecionável com `CC=x86_64-elf-gcc
OBJCOPY=x86_64-elf-objcopy`; esse cross-compiler não foi testado nesta máquina.

O ELF64 intermediário é convertido em contêiner ELF32 para o loader Multiboot 1
com `-kernel`. Isso não converte o código de 64 bits: a entrada `.code32` instala
paginação/GDT e habilita long mode. Cabeçalho no offset 4096, entrada em 1 MiB,
BSS zerada explicitamente e pilha inicial de 16 KiB. O linker reserva alinhamento
de página para DMA e IDT.

## Hardware implementado

- **IDT/TSS:** 256 gates de 16 bytes, tabela alinhada em 4096; 32 stubs de
  exceções com salvamento dos 15 GPRs, normalização do error code, dispatcher C
  e `iretq`. TSS e pilhas independentes por CPU; IST para NMI/double fault.
  `#PF` lê CR2. As falhas do contexto usuário ativo restauram o frame a partir
  do disco e colocam a tarefa em quarentena; o contexto ofensivo é abandonado.
  Falhas inesperadas no kernel geram diagnóstico e halt.
- **Ring 3:** seletores CS=0x23, DS=0x1b, entrada por `iretq` e RSP0 da TSS.
  Testes reais tentam ler memória supervisor, executar OUT e ler CR3. A CPU
  gera respectivamente #PF, #GP e #GP; os testes conferem CPL, endereço de
  falta, erro de acesso e restauração de estado deliberadamente corrompido.
- **PCI/VirtIO:** configuração 0xCF8/0xCFC, todos os buses/slots/functions,
  detecção 1af4:1001, BAR I/O e Bus Master habilitados. Descriptor/avail/used
  obedecem o layout legacy: avail logo depois dos descritores, used na próxima
  página. Alinhar avail isoladamente a 4096 seria incorreto. A fila anunciada
  pelo dispositivo é respeitada, de 128 até 1024 entradas, com reserva estática
  de 32 KiB. O QEMU deste ambiente anuncia **1024 mesmo com queue-size=128**
  no dispositivo transicional; não alegamos teste físico de uma fila de 128.
  O ID 1042 é detectado e rejeitado com diagnóstico, pois o transporte moderno
  PCI capabilities não está implementado.
- **DMA:** requests IN, OUT e FLUSH com buffer físico de 4096 bytes, barreiras,
  avail/used acquire/release e validação de completion/status. A API submit/poll
  permite uma operação pendente; os wrappers aguardam por polling. Não há fila
  de múltiplos requests simultâneos. FLUSH é obrigatório e negociado, FUA não
  é negociado. A fila é BSP-only; DMA coerente x86 usa RAM normal cacheável.
- **SMP:** procura RSDP no EBDA/BIOS, valida checksums e limites das tabelas,
  usa XSDT ou RSDT e enumera MADT, incluindo LAPIC address override. Mapeia
  LAPIC/ACPI acima dos 64 MiB; MMIO LAPIC usa PCD/PWT. Trampolim em 0x8000,
  GDT de modo real/protegido/longo, CR3 compartilhado e pilha própria do AP.
  Envia INIT assert/deassert e dois SIPIs, com delays PIT independentes de
  frequência TSC. Verifica APIC ID do AP antes de publicar online. Suporta
  exatamente BSP+um AP em xAPIC; x2APIC ativo ou falta de um segundo AP falham
  explicitamente.
- **SPSC:** BSP produz e AP consome 65.536 mensagens FIFO, com racionais e
  contratos validados em cada CPU. Completion release/acquire torna os frames
  quiescentes antes de commit. O AP então entra em HLT. Não há escalonamento
  preemptivo, IRQs de dispositivos ou shootdown geral de TLB entre CPUs.

## Persistência e contrato de recuperação

Somente páginas explicitamente registradas no SLS são persistidas. Para cada
uma, o disco reserva dois slots copy-on-write, cada qual com uma página de
metadados e uma de dados. Até quatro páginas registradas; a demo usa três.
Dados e metadados têm CRC64-ECMA. Cada commit escreve dados no slot inativo,
executa FLUSH, escreve metadados com geração incrementada e executa outro
FLUSH. Só então o bit dirty da PTE é limpo e a tradução é invalidada. Os buffers
são fixos e as fontes devem permanecer quiescentes durante toda a operação.

No boot/rollback, ambos os slots são lidos por VirtIO; a maior geração com CRCs
válidos é recuperada. Um slot incompleto permite fallback para o anterior. Se
há metadados não vazios mas nenhum checkpoint válido, o kernel falha fechado:
não reinicializa silenciosamente o estado. O formato depende da ordem fixa de
registro das páginas e dos respectivos endereços virtuais.

**A atomicidade é por página.** Não há raiz transacional atômica para um grafo
de múltiplas páginas nem recuperação de tabelas de páginas inteiras do disco.
Uma interrupção entre commits de duas páginas pode recuperar gerações distintas.
CRCs detectam corrupção, não autenticam adversários. Durabilidade depende da
implementação correta de FLUSH pelo dispositivo e pelo host. SIGKILL no QEMU
não é equivalente a desligar a alimentação física do host.

Os contextos Ring 3 desta fase são testes de fronteira de privilégio. O produtor
e consumidor cognitivos ainda executam no kernel. Não existe runtime geral de
agentes usuários, syscall ABI, CR3 por processo, NX/W^X, Ed25519 ou verificador
de bytecode/Lean. A tag SipHash com chave fixa permanece um detector de corrupção
acidental, sem autenticação contra quem pode modificar o kernel.

## Validação reproduzível

`make test` cria imagens isoladas em `build/`, sem modificar `disk.img` do
usuário. Verifica cabeçalho Multiboot e executa QEMU com duas vCPUs e TCG
multithread. Cada teste exige os eventos seriais correspondentes e termina
com SIGKILL; timeout, FAIL ou ausência de estabilidade falham no host.

A suíte verifica os bytes raw e CRCs fora do emulador, epochs de ambas as tarefas
(65.536 no primeiro boot, 131.072 no segundo), recuperação após reinício e
fallback após corrupção deliberada dos dados do slot mais novo. Quatro testes
adicionais interrompem o processo após escrita de dados, FLUSH de dados,
escrita de metadados e FLUSH de metadados. O estado novo nesses testes difere
do anterior; somente o estado anterior ou novo completo é aceito na recuperação.
Isso cobre fronteiras do protocolo e uma escrita rasgada induzida, não todas
as temporizações de falha possíveis em hardware físico.

A instrumentação de cortes usa `-append sls.crash=1` até `sls.crash=4`: o kernel
pausa em HLT na fronteira indicada, e o host o mata. Não use essa opção no boot
normal. Logs de cada execução são gravados em `build/v1-*.log`.

`make test-host` conserva 40.000 verificações de aritmética contra frações exatas
Python e 1.000.000 de mensagens entre duas threads host. A libc/pthread usada
por esses testes não integra o binário bare-metal; `nm -u build/omega64.elf`
não apresenta dependências externas. Telemetria TSC representa ticks medidos,
sem conversão para nanossegundos ou garantia de ausência de regressão de latência.

`make bundle` atualiza `omega_os_fontes.txt` com todos os fontes, headers,
linker, Makefile, documentação e testes.
