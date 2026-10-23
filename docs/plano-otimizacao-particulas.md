# Plano de otimização da preparação de partículas

Estado: diagnósticos v1/v2 e preparação ambiental progressiva testados no Vita;
desempenho aceito pelo usuário para a versão 1.0.0. Veja
`docs/release-1.0.0.md` para os números da captura final e o escopo de validação.
Base funcional:
`build/costume_quest_particles_io_v2.vpk`, instalação com `game.apk`.

## Objetivo e evidências

Reduzir os picos durante a exploração ao preparar efeitos, preservando sua
aparência, duração e resposta às ações. Manter as correções de controles,
rolagem, diálogos, partículas e cache já confirmadas pelo usuário.

O `build/debug.log` analisado identifica o cache v2 no boot. Registra chamadas
de preparação de 20 segundos simulados com custo de 110–112 ms, e janelas de
quadros com máximos de 194–195 ms próximas de algumas dessas chamadas.
Essa proximidade é evidência para investigação; o agregado de cinco segundos
não permite atribuir integralmente cada pico à preparação.

Entre aproximadamente 60 e 293 segundos não há registros `hitch:` acima de
250 ms. Partículas custam frequentemente 5–7 ms por quadro nessas janelas.
Os carregamentos iniciais ainda contêm quadros de 4–6 segundos e muita leitura
do cartão. Esses carregamentos serão uma frente posterior de trabalho.

## Restrições encontradas no código nativo

- As duas variantes `_WarmupParticles`, para ParticleState e RibbonState,
  consomem o tempo pendente em `ParticleSystemInstance + 308`.
- A iteração usa até aproximadamente 0,266667 segundo simulado. O limite
  corrigido de 90 segundos deve continuar sendo aplicado uma única vez.
- `StepParticles` chama essas variantes e, em seguida, zera explicitamente
  o campo pendente em `libCq.so + 0x7ac6e4`. Há caminhos distintos para efeitos
  que já possuem partículas. Manter apenas o campo pendente não basta para
  garantir que a próxima chamada volte ao caminho de preparação.
- A preparação modifica emissão, partículas, metas, ruído e estado aleatório;
  os limites de retomada precisam conservar toda essa sequência.
- `ParticleManager::_UpdateSystems` contém despacho de tarefas. A propriedade
  dos buffers, do gerador aleatório e dos dados de ruído entre threads ainda
  precisa ser mapeada antes de qualquer preparação em segundo plano.

Dividir 110 ms em fatias de 4 ms exige aproximadamente 28 quadros, ou 0,93 s
a 30 FPS. Portanto, diminuir o pico por divisão de trabalho pode introduzir
atraso visual. Esse custo precisa ser medido e resolvido; a divisão, sozinha,
não é uma solução completa.

## Etapa 1 — Medir por efeito e completar o mapa de estado

Primeira entrega de implementação: diagnóstico limitado, mantendo a execução
atual dos efeitos. Associar às chamadas lentas um identificador de instância
com geração, tipo, quadro, thread, duração solicitada, contagem de partículas
e tempo real. Identificar o recurso do efeito quando houver um acesso seguro
ao nome; não resolver nomes ou alocar memória em cada iteração.

Mapear Init, Reset, destruição, reutilização de instâncias, buffers e acessos
ao gerador aleatório. Identificar quais efeitos de 20 segundos se repetem e
se são ambientais ou disparados por ações. Confirmar se existe antecedência
entre a criação e a primeira exibição que permita preparação antecipada.

Critério de saída: distinguir custo de preparação de custo normal, determinar
quem possui cada estado e localizar um ponto seguro para suspender/retomar.
O log deve continuar limitado e usar o escritor assíncrono existente.

## Etapa 2 — Escolher o caminho conforme o efeito

Priorizar preparação antecipada quando houver antecedência de carregamento
ou ativação. Ela pode absorver o trabalho antes de o efeito ser visível.

Prototipar preparação incremental para efeitos cujo ciclo de vida permita
adiamento. O estado será explícito: novo, preparando, pronto ou cancelado.
Definir como o renderizador e a atualização normal tratam a instância antes
de ela estar pronta. Não permitir atualização normal simultânea da mesma
instância nem reaplicar o clamp inicial a cada retomada.

Para efeitos que precisam aparecer imediatamente, medir primeiro a origem
do custo interno e a possibilidade de antecipação. Não aceitar quase um
segundo de atraso para esconder uma pausa de 110 ms. Uma otimização interna
ou híbrida pode ser necessária antes de habilitar a divisão nesses casos.

Critério de saída: selecionar o caminho com menor custo total, considerando
pico, tempo até a primeira exibição e equivalência visual. Não compartilhar
buffers preparados entre instâncias sem provar sua independência de posição,
aleatoriedade e parâmetros do emissor.

## Etapa 3 — Protótipo com orçamento por quadro

Implementar uma opção experimental reversível. Começar com uma meta de
2–4 ms adicionais de preparação por quadro, ajustável após medição. O orçamento
é global para todas as instâncias e threads envolvidas, não um limite separado
por efeito ou por chamada a Particle Manager Update.

Interromper somente entre iterações completas. O limite é uma meta: uma única
iteração pode excedê-lo, e seu pior tempo também deve ser registrado.
Usar uma fila limitada com progresso justo entre instâncias e memória limitada;
dimensionar a capacidade a partir da etapa 1. Definir uma política explícita
para fila cheia, sem descarte silencioso nem trabalho ilimitado em um quadro.

Preservar tempo simulado restante, emissão, contagens, buffers e estado
aleatório. Revalidar a geração antes de retomar: um endereço reutilizado não
pode herdar trabalho antigo. Reset, destruição, troca de mapa e saída para o
menu devem cancelar ou invalidar tarefas e liberar suas referências.

Não avançar a idade normal do efeito enquanto ele estiver sendo preparado sem
definir como isso se relaciona com a simulação pendente. Não guardar ponteiros
de argumentos temporários entre chamadas. A validação de instruções do APK e
o caminho de retorno à implementação atual permanecem obrigatórios.

## Etapa 4 — Testes e critérios de aceitação

No computador, testar orçamento compartilhado, progresso e justiça da fila,
conclusão, cancelamento, Reset, reutilização de endereço, fila cheia e mudanças
de cena. Validar assinaturas das duas variantes nativas e do chamador antes de
aplicar patches. Reutilizar os testes existentes das correções preservadas.

A equivalência da simulação nativa precisa ser verificada no Vita: comparar
execução contínua e incremental em estado inicial controlado, incluindo
partículas emitidas, estado aleatório final e parâmetros de simulação. Se não
for possível capturar estados independentes de forma segura, registrar essa
limitação e não afirmar equivalência numérica com base apenas nos testes locais.

Comparar o VPK funcional e o candidato no mesmo percurso, save, configuração,
relógios e estado do cache de shaders. Separar primeira passagem de repetições.
Cobrir exploração, vários efeitos simultâneos, combate, troca de fantasia,
fuga, menu e transições de mapa. A validação visual no aparelho é necessária.

Medir percentis 95 e 99, máximo de tempo de quadro, quadros acima de 50/100/250
ms, custo e maior fatia de preparação, profundidade da fila, tempo até o efeito
ficar pronto e memória adicional. Não somar médias de estágios paralelos como
se fossem o tempo de um único quadro.

Aceitar quando os picos atribuídos à preparação diminuírem de forma repetível,
sem piora sustentada de FPS, atraso perceptível nos efeitos de ação, alteração
visual relevante, falhas de ciclo de vida ou crescimento contínuo de memória.
Os alvos numéricos finais serão definidos após a instrumentação; 2–4 ms é
uma hipótese inicial de orçamento, não um ganho já comprovado.

## Etapa 5 — Entrega e próxima frente

Gerar um VPK experimental com identificação inequívoca no boot e manter o
VPK funcional anterior disponível. Documentar configuração, alterações,
resultados locais e o que depende de teste no aparelho. Só incorporar como
padrão após comparação no Vita.

Após essa avaliação, investigar separadamente os carregamentos grandes:
atribuir tempo a leitura, descompressão e construção de recursos antes de
escolher pré-leitura ou ajustes de cache. As pausas restantes de menus/Flash
também exigem atribuição própria; não presumir que sejam partículas.

## Primeira entrega — diagnóstico por instância

Pacote: `build/costume_quest_particle_trace_v1.vpk`, para `game.apk`.
Mantém a simulação da versão funcional, acrescentando diagnóstico quando
`profiler 1` estiver ativo. Não contém preparação incremental e não se espera
um ganho de desempenho desta instrumentação. Seu custo precisa ser avaliado
na comparação em hardware.

`particle_trace.c` mantém até 256 identidades em memória fixa, protegidas por
um mutex curto no integrador. Reset atribui uma nova geração; o destrutor
remove a identidade. Ambos os hooks exigem os prólogos ARM esperados. A Init
nativa chama Reset em `0x7aa004`, e o construtor também chama Reset. O destrutor
ARM retorna `this`; o wrapper preserva esse retorno.

Chamadas de preparação de pelo menos 1 ms geram até 256 registros detalhados
por execução. Cada `particle trace:` contém tipo, endereço da instância,
geração, handle opaco do recurso, ponteiro dos dados, quadro inicial/final,
thread, idade desde Reset, duração solicitada, contagem antes/depois,
capacidade e ponteiros de RNG/ruído/buffer. Os identificadores de recurso são
opacos e válidos para correlacionar a execução atual; não são nomes de arquivo.
Uma geração zero e idade -1 indicam identidade não acompanhada, por exemplo
quando a tabela está cheia; nenhuma instância é expulsa silenciosamente.

`particle frame:` correlaciona quadros de pelo menos 50 ms com chamadas
terminadas desde a última troca de buffer, até 256 registros. O identificador
de quadro é monotônico e separado do contador zerado pelo heartbeat. A soma
`sum_call_us` mede chamadas e pode incluir sobreposição de threads ou chamadas
que atravessam quadros; não representa tempo exclusivo do quadro.
`particle trace totals:` continua a cada cinco segundos mesmo após o limite
dos detalhes, com contagens, soma, máximo, identidades ativas e resets sem vaga.
Nenhum ponteiro dos registros é usado para executar trabalho futuro.

O mapeamento de `ParticleUpdateTask::Execute` em `0x7658a4` mostrou que o RNG
fica em `task + 12`, usa um índice derivado de SDL_GetTicks e é passado a
StepParticles em `0x765938`. A tarefa percorre várias instâncias e chama seu
destrutor virtual ao final, em `0x765974`. O ruído vem de um ponteiro no campo
`task + 24`. Assim, reter o RNG da chamada atual após o retorno seria inseguro;
a próxima etapa exige estado próprio e uma política para a sequência aleatória
entre instâncias. `InitializeJobHeader` é vazio neste binário e não oferece
um mecanismo pronto de retomada.

Teste local das identidades, reutilização de endereço, tabela cheia,
cancelamento repetido, limite de detalhes e continuidade dos totais:

```sh
cc -Wall -Wextra -Werror -fsanitize=address,undefined -g -Isource \
  tests/particle_trace_test.c source/particle_trace.c -o /tmp/cq-particle-trace-test
/tmp/cq-particle-trace-test
python3 tests/particle_warmup_test.py
```

Próximo dado necessário: executar o pacote de diagnóstico no Vita com
`profiler 1`, percorrendo os mesmos locais, disparando efeitos de fantasia e
combate e fazendo uma transição de mapa. O novo debug.log permitirá agrupar os
recursos caros, verificar a antecedência desde Reset, threads e limites da
tabela antes de selecionar os primeiros efeitos para o protótipo incremental.

## Resultado da captura v1 e revisão v2

Na captura recebida após a primeira entrega, `particle trace v1` está ativo.
Os últimos totais são 193 preparações, 15 acima de 50 ms, soma de 2552364 us e
máximo de 112740 us. Os detalhes não atingiram o limite: foram 83 registros.

| Recurso nesta execução | Chamadas detalhadas | Soma | Faixa por chamada |
| --- | ---: | ---: | ---: |
| 0001EDE1 | 15 | 1666,117 ms | 108,879–112,740 ms |
| 0001EDF1 | 15 | 299,948 ms | 19,886–20,138 ms |
| 0001EE11 | 15 | 242,850 ms | 15,699–16,361 ms |
| 0001EE01 | 15 | 166,573 ms | 10,913–11,234 ms |

Todas as 15 chamadas lentas pertencem ao primeiro recurso. Ele pede 20 s
simulados, tem capacidade 661 e termina com 644 partículas nas 15 amostras.
Isso corresponde a aproximadamente 65,3% da soma de tempos de preparação,
não a 65,3% do tempo total de jogo ou de um quadro. As gerações dessas chamadas
foram identificadas; sua idade desde Reset varia de 7 a 161 ms, com várias
amostras entre 7 e 11 ms. A contagem final igual não prova igualdade das
posições ou do estado aleatório e não autoriza reutilizar um buffer pronto.

A tabela de 256 identidades ficou cheia e houve 1004 resets sem vaga. Isso
limita a cobertura do ciclo de vida geral, mas não invalida a identificação
das 15 chamadas lentas, que possuem geração não zero.

Decisão: começar o futuro protótipo pelo recurso principal, depois de
identificá-lo pelo nome de arquivo e validar seu comportamento. O handle
0001EDE1 não é uma chave persistente entre execuções. A pouca antecedência
desde Reset reforça que antecipação nesse ponto, isoladamente, não basta.
Não habilitar um atraso potencial de cerca de um segundo sem definir a
resposta visual e preservar o estado compartilhado da tarefa.

A revisão `build/costume_quest_particle_trace_v2.vpk` amplia a tabela para 2048
entradas fixas e mantém os 256 detalhes máximos. Ela usa o getter nativo
`RsRefBase::GetAssetName`, após validar seu prólogo, para registrar até 64
associações de handle e nome. O getter é chamado apenas após a preparação,
enquanto a tarefa ainda possui a referência, fora do mutex do diagnóstico e
do trecho cronometrado. O retorno é usado imediatamente pelo logger, sem
reter a string nativa. A função nativa usa sincronização do gerenciador de
recursos; seu custo adicional ainda precisa ser conferido no aparelho.
Essa revisão continua sem preparação incremental ou mudança visual esperada.

O analisador abaixo agrupa os registros e alerta sobre tabela cheia ou limite
de detalhes. Também aceita os nomes adicionais da revisão v2:

```sh
python3 extras/analyze-particle-trace.py build/debug.log
```

Resultado desta captura: `build/particle_trace_v1_analysis.txt`. A cópia
preservada é `build/debug_particle_trace_v1_capture.log`. Próximo checkpoint
em hardware: confirmar o nome do recurso principal e a cobertura da tabela
ampliada durante o mesmo percurso. Só então fixar a seleção por nome no
protótipo e testar equivalência da simulação antes de limitar trabalho.

## Captura v2 — efeito identificado e protótipo ambiental

O usuário enviou a captura v2. Seus totais finais são 225 preparações,
15 acima de 50 ms, máximo de 112586 us e nenhum reset sem identidade.
O maior recurso é `particles/effects/weather/precipitation/pollenlight/leaves`:
as 15 chamadas caras somam 1666,253 ms, variam de 110,131 a 112,586 ms e
terminam com 644 partículas, capacidade 661, preparação de 20 s.
Os outros três recursos do grupo são nuvens/stars do céu de suburbs, com
custos individuais de aproximadamente 20, 16 e 11 ms. A captura está em
`build/debug_particle_trace_v2_capture.log`; o resumo está em
`build/particle_trace_v2_analysis.txt`.

Decisão de implementação: prototipar o preenchimento progressivo **somente
das folhas ambientais**, sem suspender e retomar a função nativa de warmup.
Isso evita reter o RNG pertencente à tarefa que será destruída e reaproveita
a atualização nativa de partículas, emissão, limites e transformações.
É uma alternativa à retomada exata inicialmente investigada, com uma
diferença visual deliberada: as folhas começam menos densas e se preenchem
durante os primeiros segundos. O estado aleatório final também pode diferir
da preparação contínua. Não se afirma equivalência numérica dessa estratégia.
Sua aceitação depende da aparência e da fluidez no Vita.

O candidato `build/costume_quest_leaves_progressive_v1.vpk` habilita essa
experiência por padrão. A seleção exige o nome exato, variante ParticleState,
20 s solicitados e capacidade 661. Outros parâmetros, nomes e tipos conservam
a preparação nativa. O patch de StepParticles exige o prólogo esperado e os
hooks de identidade e warmup disponíveis.

Funcionamento:

1. Na preparação desse efeito, registrar 20 s adicionais em uma fila fixa de
   oito identidades e deixar a chamada nativa continuar sem o warmup contínuo.
2. Nas atualizações seguintes, somar parte desse tempo ao delta normal da
   instância selecionada, com delta total máximo de 0,266667 s. O RNG, ruído,
   dados e buffers são sempre os da chamada corrente; não são guardados.
3. Permitir no máximo um avanço adicional por instância/quadro e um avanço
   adicional em execução globalmente. Usar alternância entre instâncias
   recentes e uma meta global de 4000 us de chamadas nativas por quadro.
4. Debitar apenas o tempo simulado adicional, mantendo o delta normal, até
   completar os 20 s. Reset/destruição invalidam o trabalho por geração.
   Fila cheia ou identidade desconhecida retornam ao warmup original.

O custo cronometrado inclui toda a chamada nativa selecionada, de modo
conservador; uma chamada não pode ser interrompida, portanto a meta de 4 ms
pode ser ultrapassada por uma etapa. Os contadores registram esse excesso e
o maior tempo individual. Trabalho em voo mantém sua reserva mesmo se
atravessar uma troca de buffer. Instâncias que deixam de ser atualizadas não
impedem indefinidamente o avanço de instâncias presentes.

No modelo local a 30 FPS, cada instância requer 86 avanços para os 20 s extras,
aproximadamente 2,9 s se houver orçamento e atualização em todos os quadros.
Concorrência, visibilidade, pausas e custo real podem prolongar esse período.
O modelo verifica a soma do tempo extra e o escalonamento, não reproduz a
simulação nativa nem prova igualdade visual.

```sh
cc -Wall -Wextra -Werror -fsanitize=address,undefined -g -Isource \
  tests/particle_progressive_test.c source/particle_progressive.c \
  -o /tmp/cq-particle-progressive-test
/tmp/cq-particle-progressive-test
```

Os testes cobrem seleção restrita do recurso, duas instâncias simultâneas,
orçamento compartilhado, reserva em voo, justiça, delta nulo/inválido,
conclusão, tabela cheia, cancelamento e endereço reutilizado. Também continuam
passando os testes de identidade e da correção do clamp nativo.

Configuração reversível no `ux0:data/costumequest/config.txt`:
`particle_leaves 1` habilita o candidato; `particle_leaves 0` retorna à
preparação nativa no próximo boot. Os hooks e o relógio de quadros funcionam
mesmo com profiler desligado. Com `profiler 1`, o boot registra
`particle leaves progressive v1: enabled`, os eventos `particle leaves:`
indicam início/conclusão e `particle leaves totals:` registra contagens de
conclusão/cancelamento/fallback, pendências, etapas, maior custo e excessos.

Validação pendente: repetir a exploração que gerou as 15 preparações caras,
conferir a densidade inicial e o movimento das folhas, entrar/sair de mapas e
combate, e comparar quadros e memória com `particle_leaves 0`. O objetivo é
remover os picos desse warmup sem trocar a pausa por um preenchimento visual
inaceitável ou custo sustentado maior. Este protótipo ainda não é aprovado
como comportamento definitivo. Carregamentos de mapa continuam separados.
