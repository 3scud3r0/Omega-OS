# Omega-OS — Cortex neural-simbólico paginado

Kernel x86-64 C99 freestanding com compilador Ômega e inferência Llama em Ring 3.
`user/omega/cortex.c` reúne leitura GGUF, kernels quantizados, RMSNorm, RoPE,
atenção paginada GQA, SwiGLU, logits, arbitragem racional e emissão JIT.
Os modelos são arquivos GGUF reais fornecidos pelo operador; não há download
implícito de pesos nem modelo de linguagem treinado embutido no kernel.

| Contrato | Implementação |
|---|---|
| Contexto vetorial | CR4.OSXSAVE/XCR0, áreas privadas alinhadas de 832 bytes, XSAVE/XRSTOR e fallback FXSAVE/FXRSTOR |
| Memória limitada | PMM de 64 MiB; cache de até 64 frames de 4 KiB; páginas de pesos/KV acessadas por capacidades, LRU e backing SLS |
| Quantização | F32/F16, Q8_0, Q4_K e Q6_K — incluindo a combinação Q4_K_M |
| MatMul | Painéis de inteiros com acumulação int64 escalar/SWAR ou AVX2; projeções quantizadas com desquantização Q8/Q4 em registradores e FMA |
| Atenção | KV FP16 por página, GQA e softmax online; sem buffer de scores proporcional ao contexto |
| Arbitragem | Máscara nos logits reais antes da seleção, contratos racionais explícitos e e-graph original do Ômega |
| Código sintetizado | AST racional derivada do candidato aceito, encoder nativo e syscall 11 com auditoria canônica W^X |
| Persistência/cluster | Checkpoint CoW de KV/descritor/logits; hashes de modelo/KV/JIT vinculados ao certificado assinado; votante confere seu prefixo SLS |

O código e os testes são executáveis; não constituem prova formal de um kernel
ou garantia de correção de linguagem natural. O consenso permanece maioria
assinada 2/3 com líder fixo honesto, sem BFT geral ou eleição.

## Build e testes

```sh
python3 -m pip install -r requirements-test.txt
make clean
make -j4
make test-host
make test-cortex-scale QEMU=/workspace/tools/bin/qemu-system-x86_64
make test-cortex QEMU=/workspace/tools/bin/qemu-system-x86_64
make test QEMU=/workspace/tools/bin/qemu-system-x86_64
make bundle
```

Use `QEMU=qemu-system-x86_64` numa instalação comum. Neste ambiente, o wrapper
em `/workspace/tools/bin/` fornece bibliotecas e firmware. Reutilize o checkout
isolado existente; não crie worktrees para executar o projeto. Não há serviço
persistente do host a iniciar: cada teste cria e encerra seus QEMU.

O build gera `build/cortex.elf`, `build/compiler.elf` e `omega_os.elf`. Kernel e
ELFs usam C99/O3/LTO, `-ffreestanding -nostdlib -fno-builtin`, sem red zone e
`-Wall -Wextra -Werror -pedantic`. Somente o córtex permite instruções FP/SIMD;
o kernel continua compilado com `-mgeneral-regs-only`. As referências NumPy,
OpenSSL e cryptography pertencem exclusivamente ao host de build/teste.

Os testes do córtex executam Q8/AVX2, Q4_K+Q6_K/AVX2 e Q4_K+Q6_K/escalar,
comparam logits e KV com uma implementação NumPy independente e reiniciam os
discos. As VMs desse teste possuem 64 MiB de RAM. Configuram uma KV lógica de **1 GiB**, usando no máximo **256 KiB** de
frames do pager. Os pesos dessas provas são fixtures numéricas GGUF claramente
identificadas; validam o motor completo, sem alegar qualidade de um LLM treinado.

O teste de escala escreve e verifica **17.000 páginas / 66,4 MiB de dados reais**,
mais que toda a RAM da VM, com 64 frames de cache e GC em disco.

A suíte de cluster inicia três nós roteados, testa isolamento, SIGKILL e retomada,
confere assinaturas e dois epochs com checkpoints do córtex. A suíte completa
preserva os testes V4, GC em disco de 1 MiB, oito fronteiras SIGKILL, corrupção,
rede cifrada e isolamento. Os testes de quórum configuram uma cadência maior
entre bursts para permitir auditar os checkpoints enquanto os três nós vivem.

## Executar pesos próprios

```sh
python3 tools/cortex_model.py modelo.gguf cortex.img --size-mib 4096 \
    --steps 1 --prompt 1,128000
make run DISK=cortex.img QEMU=/workspace/tools/bin/qemu-system-x86_64
```

A ferramenta cria uma **nova** imagem, recusando sobrescrita. Dimensione o disco
para pesos, KV e as duas regiões CoW. `--prompt` recebe IDs do tokenizer do modelo;
não existe tokenizer de texto geral nessa entrega. Para AVX2, acrescente `-cpu max`
ao QEMU ou use o teste Cortex. O `make run` padrão também funciona com fallback.
Uma imagem sem GGUF inicia o processo e informa que aguarda pesos; não finge inferência.

`--contracts regras.json` ativa decodificação lógica fechada. Exemplo:

```json
[{"token":0,"num":1,"den":2},{"token":3,"num":-1,"den":2}]
```

Só os IDs anotados, com transições racionais válidas e norma quadrática ≤1,
permanecem elegíveis. Tokens desconhecidos ou inválidos recebem -INFINITY. Sem
esse arquivo o modo é inferência neural normal, sem promessa de poda semântica.
O vocabulário/tokenizador externo deve dar a esses IDs o significado declarado.

Consulte [arquitetura e contratos Cortex](docs/CORTEX.md),
[V4](docs/V4.md), [V3](docs/V3.md) e [evidência V4](docs/V4-validation.md).
O arquivo [omega_os_fontes.txt](omega_os_fontes.txt) reúne fontes e cápsulas
públicas geradas; exclui pesos binários, imagens, initrds e chaves privadas.
