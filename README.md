# Omega-OS

Nanokernel experimental **x86-64**, modular, C99 freestanding, sem libc ou
alocação de heap. A especificação modular final tem precedência sobre o pedido
inicial de arquivo único. Todos os fontes também estão reunidos em
`omega_os_fontes.txt` (`make bundle`).

## Compilar e executar

Pré-requisitos: GCC x86-64, GNU binutils, Make, QEMU x86; Python 3 para testes.
Em Debian: `apt-get install gcc binutils make qemu-system-x86 python3`.

```sh
make -j2
qemu-system-x86_64 -kernel omega_os.elf -serial stdio -display none
make test
make test-host
make bundle
```

Um cross-compiler pode ser selecionado com `make CC=x86_64-elf-gcc
OBJCOPY=x86_64-elf-objcopy`. O build foi validado com GCC 14.2.0 (Debian), LTO,
`-Wall -Wextra -Werror -pedantic`, e QEMU 10.0.13 (TCG).
Não foi validado um cross-compiler `x86_64-elf-gcc` neste ambiente.

No ambiente desta implementação, QEMU foi extraído de pacotes Debian assinados
em `/workspace/tools/qemu` por não haver sudo. Use
`make test QEMU=/workspace/tools/bin/qemu-system-x86_64` e o mesmo override para
`make run`. Essa instalação local é externa ao repositório.

O cabeçalho Multiboot 1 fica no offset 4096, dentro dos primeiros 8 KiB.
`link.ld` organiza um ELF64 intermediário com código carregado em 0x100000.
`objcopy -O elf32-i386` produz o contêiner ELF32 aceito pelo loader Multiboot do
QEMU; não converte as instruções x86-64. `_start` executa código de 32 bits,
zera BSS, instala GDT e quatro níveis de paginação e habilita long mode antes
de chamar C com pilha alinhada de 16 KiB. Compilar somente com `-Ttext`, sem
este script e conversão, não reproduz esse caminho de boot.

## Subsistemas

- UART 16550 COM1: 115200 baud, 8N1, polling; texto, hexadecimal e decimal.
- PMM: limite de 64 MiB, frames de 4096 bytes; bitmap hierárquico de três níveis
  com bit-scan, seleção em tempo constante; reservas até o fim do kernel,
  limite de RAM Multiboot, rejeição de double-free. Executado apenas no BSP.
- MMU: identity mapping de 64 MiB com páginas de 4 KiB e aliases SLS em 1 GiB.
  `mmu_map` aceita endereços inferiores canônicos, páginas dentro dos 64 MiB e
  flags present/write. Frames de page tables são obtidos do PMM. Falta de
  memória pode deixar tabelas intermediárias vazias alocadas; retorna falha.
- SLS experimental: varredura real de PTEs, detecção/limpeza de bit dirty e
  `invlpg`. Checkpoints de tarefas copiam frames completos para frames de RAM.
  Commit e rollback são transações cooperativas de um processador, sem escrita
  concorrente durante a cópia. **Limpar dirty não persiste dados.** Não há SSD,
  NVRAM, journal, recuperação de energia ou atomicidade entre CPUs.
- SPSC: 1024 slots, índices monotônicos unsigned com wrap definido, cache lines
  separadas de 64 bytes, release/acquire. Apenas um produtor e um consumidor;
  inicialização exige exclusividade. Testado também com duas threads no host.
- Racionais: representação reduzida com denominador positivo; operações
  verificam overflow e divisão por zero; comparação usa frações contínuas.
  Overflow intermediário é rejeitado mesmo se o resultado final coubesse.
- Contratos: norma quadrática exata <= 1 e tag SipHash-2-4 dos campos canônicos.
  Corrupção de tag e violação de norma causam restauração do frame e quarentena.
  O checkpoint é revalidado antes do rollback. Uma chave pública fixa de demo
  não fornece autenticação contra um adversário com acesso ao kernel.
  **Não há certificado Lean, verificador de prova formal ou isolamento ring 3.**

## Execução e evidência

O kernel executa 16 ciclos cooperativos de produtor/consumidor no BSP, testa
corrupção e norma inválida e termina em `cli; hlt`. Não inicia APs, não executa
simultaneamente em múltiplos cores, não instala IRQs ou IDT e não tem recuperação
de exceções. Exige CPU x86-64 e firmware compatível com Multiboot; RISC-V não
está implementado. Não há outros serviços ou credenciais para o kernel.

`make test` verifica o cabeçalho no arquivo e inicia de fato o QEMU, captura a
serial em `build/boot.log` e exige que cada teste chegue a PASS e STABLE sem FAIL.
O timeout de 12 s encerra o QEMU, pois o kernel permanece em HLT por projeto.
Uma execução sem mensagens de estabilidade falha no teste.

`make test-host` compara 40.000 operações racionais com `fractions.Fraction`
e transfere 1.000.000 mensagens ordenadas entre duas threads pthread. A libc
nesses testes pertence exclusivamente ao host. O kernel não tem símbolos
externos pendentes (`nm -u build/omega64.elf` não produz saída).

Trecho observado na validação:

```text
[OMEGA-OS] BOOT x86-64 / 4-level paging
TEST rational arithmetic/overflow PASS
TEST SPSC full/empty/FIFO/wrap PASS
SLS dirty 0x0000000040000000 checkpoint tracking (RAM)
SLS dirty 0x0000000040001000 checkpoint tracking (RAM)
IPC transferred 16 messages; contracts accepted
IMMUNE quarantine + full frame rollback id=2
IMMUNE quarantine + full frame rollback id=1
TEST contract/tag/norm/quarantine/frame rollback PASS
CPU TSC cycles: 27930136
STABLE: 16 cooperative cycles completed; RAM checkpoints only; entering HLT
```

Ciclos de TSC variam por execução e incluem logs seriais. Não são necessariamente
ciclos efetivos do core; não são convertidos em nanossegundos sem calibração.
Não existe baseline anterior para alegar ausência de regressão de latência.
Previsões sobre tempos de boot, taxas de branch miss, consumo, desgaste,
convergência cognitiva ou meses de operação não foram medidas nem são garantidas.
