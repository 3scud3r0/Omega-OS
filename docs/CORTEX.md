# Contratos físicos do Cortex

## Imagens e isolamento

O novo ELF é assinado pela mesma autoridade de desenvolvimento dos demais
processos. O loader valida seus segmentos e cria CR3, stack e dados privados.
Os processos 8 e 9 exercitam padrões x87/XMM/YMM distintos através de syscalls,
yield e interrupções LAPIC. O teste também executa CPUs sem XSAVE/AVX.

XSAVE usa o formato padrão, máscara XCR0=7 quando AVX existe, ou x87/SSE
quando apenas estes estados existem. CPUID.0Dh confirma que a área cabe em
832 bytes. O fallback usa os 512 bytes FXSAVE. Cada área tem alinhamento 64.
O estado é salvo na entrada de syscall/interrupção e restaurado para a tarefa
selecionada antes do retorno. O kernel não utiliza registradores vetoriais.
AVX-512 não é habilitado; CPUs SMP com estados incompatíveis são recusadas.

O loader agora fornece a entrada com alinhamento de pilha SysV e uma posição
válida para o endereço de retorno sintético. A desmontagem de espaços mortos
usa um bitmap físico fixo para liberar também ELFs grandes sem duplicar aliases.

## GGUF e operações

O leitor suporta GGUF v3 com arquitetura Llama e tensores rank 1/2. Os limites
configuráveis da implementação atual são dimensão 8192, FFN 32768, 128 camadas,
128 heads, vocabulário 262144 e contexto 1048576. Há limites de capacidade de
disco e de namespace além destes limites de forma. Não se aloca KV na BSS.

O caminho de inferência inclui embedding, RMSNorm, projeções Q/K/V, GQA, RoPE,
atenção causal com softmax online, projeção residual, SwiGLU e output norm/logits.
RoPE aceita frequência base, fatores de tensor, escala linear e Llama 3;
formas como YaRN não são silenciosamente interpretadas como RoPE padrão.

Q8_0 usa FP16+32 int8; Q4_K usa os campos d/dmin, escalas/mins de seis bits e
nibbles de blocos 256; Q6_K usa low/high bits e escalas assinadas. Q4_K_M é uma
receita de mistura Q4_K/Q6_K, não um tipo de bloco único. F32/F16 são suportados
para normas e demais tensores. Formatos desconhecidos falham fechado.

A deserialização não materializa uma cópia densa do modelo. O bloco é lido na
página SLS; apenas blocos que cruzam uma fronteira são reunidos num temporário
limitado a 210 bytes. Q8 e Q4 têm desquantização AVX em registradores fundida
com FMA. Q6 usa um painel temporário limitado a 256 floats. O GEMM int8 público
aceita M/N/K, usa painéis 4×32×256 e acumula em int64 sem saturação intermediária.
O fallback neural calcula em FP32; a aritmética racional pertence ao árbitro.

Não há BLAS, CUDA, libm, tokenizer geral, treinamento ou modelos de arquitetura
MoE. Seno/exp/raiz são implementações locais aproximadas para inferência FP32;
correção racional exata não transforma operações neurais em matemática exata.
Não há alegação de 90% dos FLOPS de hardware físico.

## Páginas e atomicidade

O SLS mantém seu radix de três níveis, com 2^24 páginas lógicas (64 GiB). A nova
API de I/O lógico não exige uma entrada RAM por página: a árvore em disco é a
tabela de backing. Pesos ocupam a região lógica abaixo de 32 GiB; KV ocupa a
segunda metade. A forma do modelo define os strides de camada e token.

O pager tem 64 descritores residentes com frame, ID lógico, idade LRU, pin e
permissão. As capacidades são mapeadas na janela 0xa0000000, sempre NX. Pesos
são somente leitura. Acessos são adquiridos/liberados explicitamente por syscall;
não se transforma qualquer #PF em I/O de disco implícito. Uma falta fora dessas
capacidades continua sujeita à quarentena do runtime.

Antes de reutilizar um frame, o pager persiste a página suja por VirtIO e revoga
a capacidade anterior. Dirty bits são preservados nos hits e invalidados após
commit. Upgrades RO→RW invalidam TLB. A reserva de GC considera a quantidade de
páginas de uma transação do pager, além do conjunto residente antigo.
O driver mantém um prazo limitado de 120 bilhões de TSC ticks para conclusão
de I/O/FLUSH, com diagnóstico de índices da fila. Um timeout retém a propriedade
dos buffers DMA e falha fechado; não há reenvio que possa duplicar uma escrita.

A KV é append-only por posição. Evictions podem tornar duráveis páginas de
trabalho antes de publicar uma nova posição; não alteram o prefixo já aprovado.
O checkpoint final publica, na mesma raiz CoW, os frames sujos, um descritor de
256 bytes e uma página diagnóstica com os primeiros 1024 logits. O descritor
contém forma, posição, próximo token e hashes BLAKE2b de modelo, prefixo KV,
logits completos e função JIT. Reboot confere o prefixo antes de continuar.

A memória residente do pager é constante, mas atenção causal ainda lê o passado.
Contextos longos em disco podem ser lentos. O conjunto vivo deve caber num
semispace; GC não cria capacidade infinita. O formato não promete execução
instantânea após ligar a máquina nem preservação de RAM sem energia.

## Contratos simbólicos e JIT

As regras de token são fornecidas explicitamente. O árbitro testa racionais
canônicos, overflow e q²≤1, depois consulta o e-graph original para a equivalência
polinomial da transição. A máscara é aplicada aos logits reais antes de argmax.
Sem regra válida, a seleção falha, em vez de fabricar um token.

O candidato aceito dá origem a uma expressão racional, passada ao encoder
existente. A syscall 11 valida toda a página canônica, sela RW+NX→RO+X e protege
também o alias físico. O resultado nativo é confrontado com o racional esperado;
o hash da função passa ao checkpoint. O whitelist do JIT não foi relaxado.

Essas garantias abrangem a linguagem de contratos definida. Não provam que uma
frase em linguagem natural é verdadeira, que um prompt é benigno ou que o kernel
inteiro foi verificado por Lean.

## Quórum e retomada

O descritor Cortex cabe na parte livre do manifesto V4; a assinatura de epoch
cobre seus hashes. Antes de votar ou instalar um certificado, o nó confere o
modelo local e recalcula o hash do prefixo KV no seu próprio SLS. Se atrasado,
executa o motor até a posição solicitada. Pesos devem ser provisionados nos três
nós; não há transferência automática de um modelo ausente.

No boot de cluster, o motor prefere a posição/token do último certificado
publicado, em vez de assumir que qualquer checkpoint local de trabalho foi
aprovado. O líder também retoma sua proposta durável após SIGKILL. Dados novos
podem estar em disco sem estarem publicados como estado do cluster.

A política continua sendo maioria assinada 2/3 com líder fixo honesto, não BFT
geral. Não existe eleição. Inferência deve produzir o mesmo prefixo nos votantes;
uma divergência numérica ou de pesos impede o voto. A cadência entre bursts
é configurável no módulo de provisionamento; os testes de quórum usam 60 bilhões de
TSC ticks para dar tempo à auditoria/reboots enquanto o serviço permanece ativo.

## Evidência de escala

`tests/cortex_scale.py` grava e relê 17.000 páginas (69.632.000 bytes), com
canários determinísticos verificados em todos os 512 words de cada página.
A VM possui 64 MiB de RAM; o PMM continua limitado a 64 MiB. O mesmo caminho
de aquisição de páginas usado pela atenção atingiu 64 frames residentes,
34.335 leituras, 17.002 escritas e 34.270 evictions no ensaio inicial. O SLS
compactou o log em semispaces durante o trabalho. Não é apenas uma reserva
lógica esparsa: mais de 64 MiB de payload foram efetivamente transferidos.

O workload é opcional e ocupa uma faixa KV não usada pela forma do modelo;
é ativado somente pelo descritor de teste. Em inferência normal esse custo
não existe. O tempo não é uma garantia de throughput de processador físico.
