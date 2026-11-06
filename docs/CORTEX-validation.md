# Validação do Cortex — 2026-11-06

Comandos executados neste ambiente com GCC 14.2 e QEMU 10.0.13/TCG:

```sh
make clean
make -j4
make test-host
make test QEMU=/workspace/tools/bin/qemu-system-x86_64
```

Kernel, compilador e `build/cortex.elf` foram linkados sem libc e sem símbolos
externos indefinidos (`nm -u`). O build mantém C99/O3/LTO e
`-Wall -Wextra -Werror -pedantic`. O kernel permanece sem uso de SIMD.

## Provas funcionais novas

- Todas as 65.536 representações FP16 foram confrontadas com a conversão
  independente do Python, incluindo subnormais, infinitos e NaNs.
- GEMM int8 escalar/SWAR e AVX2 foi comparado exatamente com NumPy int64,
  em matrizes não múltiplas dos tiles e com valores extremos assinados.
- Q8_0, Q4_K e Q6_K foram confrontados com decodificação independente.
- Seno em argumentos até ±1.000.000 e exponencial foram comparados com libm
  do host; a libm não participa dos ELFs freestanding.
- Inferência Llama completa foi executada em Q8/AVX2, Q4_K+Q6_K/AVX2 e
  Q4_K+Q6_K/escalar. Os perfis também cobrem RoPE normal, Llama 3 e linear.
  Logits mascarados, próximo token e K/V FP16 foram comparados a uma segunda
  implementação NumPy. As VMs desses ensaios possuem 64 MiB de RAM.
- Cada configuração reserva 1 GiB de KV lógico, executa posições 0–7,
  reinicia e continua até a posição 16 após conferir o hash do prefixo.
  O teste diferencia capacidade lógica de dados efetivamente escritos.
- O ensaio de escala grava e relê 17.000 páginas, totalizando 69.632.000
  bytes de payload, mais do que toda a RAM de uma VM de 64 MiB. São verificados
  todos os 512 words de cada página. Resultado: pico de 64 frames, 17.002
  escritas, 34.270 evictions e compactação real do SLS. Uma execução observada levou
  52,7 s nesse ambiente; não é uma garantia de throughput físico.
- Dois processos conservam padrões x87 e XMM/YMM diferentes através de
  syscalls, yield e preempção de hardware. Há execução em CPU sem XSAVE/AVX
  para exercitar FXSAVE/FXRSTOR e fallback escalar.
- Um GGUF com configuração inválida é colocado em quarentena; o kernel
  continua e não publica um checkpoint cognitivo desse modelo.
- O candidato racional selecionado passa pelo e-graph original, é emitido
  como código nativo, selado pela syscall 11 e executado diretamente.

## Persistência distribuída

Três nós roteados recebem o mesmo GGUF. O manifesto assinado inclui o descritor
Cortex, com hashes de modelo, prefixo KV, logits e função JIT. Antes de votar ou
instalar um certificado, cada nó verifica o prefixo no seu SLS; um nó atrasado
executa até a posição solicitada. O ensaio cobre líder isolado, recuperação de
proposta não publicada, SIGKILL de seguidor antes/depois de publicar e dois epochs.

A política é maioria assinada 2/3 com líder fixo honesto. Não é BFT geral e não
há eleição de líder. Pesos ausentes não são transferidos automaticamente.

## Limites da evidência

Os pesos do ensaio são fixtures numéricas GGUF, identificadas como tal. Não há
alegação de qualidade linguística de um modelo treinado. O motor aceita pesos
Llama GGUF externos nos formatos/limites descritos em docs/CORTEX.md. O teste
não demonstra bilhões de parâmetros nem contexto de um milhão de tokens
inteiramente inferido; demonstra o caminho completo, endereçamento paginado,
consumo físico limitado e backing real além da capacidade da RAM.

A poda lógica vale para os contratos explícitos associados a IDs de token.
Não é prova da veracidade de qualquer resposta em linguagem natural.
