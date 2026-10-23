# Controles de console — auditoria revisada em 22/10/2026

## Estrutura confirmada

O APK fornecido contém `lib/armeabi-v7a/libCq.so` e um ZIP de dados em
`assets/ant.mp3`: 35.321 entradas, incluindo 37 filmes GFX. Não contém o
código-fonte C++ original; as conclusões nativas vêm de símbolos exportados
e desmontagem do binário.

- `Data/Config/input.lua` reconhece o prefixo `SDL_Gamepad` e associa os
  botões às ações virtuais. `SDLGamepad::UpdateDeviceState_GameController`
  lê 6 eixos, os botões e o direcional. Guide é pulado pelo próprio jogo;
  remapeá-lo não cria uma ação nova.
- `VirtualGamepadManager::m_bIsGamepad` seleciona a interface. O callback Lua
  `IsGamepadMode` em `0x6e81c4` lê essa flag; `IsTouchMode` em `0x6e821c`
  devolve seu inverso. Não é necessário reescrever cada menu.
- Fantasias, caderno, pausa, diálogos, créditos, cutscenes e lojas contêm
  consultas de modo ou controles próprios. 21 filmes importam símbolos de
  `ControllerIcons.swf`. Todos esses símbolos existem na biblioteca combinada:
  53 exports, 53 texturas existentes, 1.154 bytes comprimidos, dentro do espaço
  da entrada original.
- `BattleHUD_DLC` usa `ShowPCButtons`; a adaptação existente mantém os frames
  de console e os ícones PlayStation, preservando símbolos especiais mobile
  que fazem parte das habilidades.
- `Battle_HandleInput` conserva ações, alvos, cancelamento, pausa e fuga pelo
  controle. Seus minigames ainda passam por lógica mobile.
- O minigame de maçãs também tem caminho nativo: A em
  `OnInputReceived+0x134` e leitura do analógico. Seu caminho de gestos não
  precisa substituir o caminho de controle.

## Alterações

1. A tela frontal fica habilitada também em modo gamepad, como alternativa
   aos controles físicos. A interface de console continua selecionada.
2. O dispositivo informa suporte a touchscreen no boot. `controls touch`
   continua disponível para selecionar a interface Android após reiniciar.
3. Os três patches da flag de gamepad são validados pelas instruções exatas
   antes de qualquer escrita, evitando aplicar parcialmente um conjunto incompatível.
4. Cada atualização SDL lê uma amostra consistente de todos os controles.
   Falhas de leitura deixam botões soltos e eixos centralizados. O analógico
   alcança os dois extremos da faixa SDL.
5. A batalha usa as bordas de pressão do `VirtualGamepad`, incluindo o
   direcional, sem uma segunda leitura física fora do estado do jogo.
6. A posição do botão vem do índice do prompt e de `Battle_GetHitBox`: uma
   consulta em lugar de até 3.600 testes por pressão. Botão ausente produz
   uma posição fora dos alvos; não vira acerto automático.
7. O minigame 20, `Swipe`, recebe deslocamentos em unidades de analógico.
   A ponte anterior entregava pixels ao ramo gamepad, cujo ganho é 0,12,
   contra 0,0007 no ramo touch (aproximadamente 171 vezes maior). Agora a
   projeção compensa a resolução e a proporção da tela. Cada movimento
   inicializa a posição anterior pelo caminho nativo antes de enviar o delta;
   entrar na ação ou manter o analógico parado não acrescenta carga.
   A zona morta é 20%, a diagonal é limitada ao círculo unitário e pequenos
   deltas são acumulados até ultrapassarem 0,01 unidade. Pontuação, duração
   e dificuldade continuam controladas pelo jogo.
8. Prompts direcionais aceitam o direcional ou um movimento do analógico
   esquerdo. A ativação em 60% e liberação em 35% evitam repetição ao segurar.
9. Logs extensos de batalha ficam condicionados a `profiler`.

## Comandos de combate

Mapeamento padrão; configurações `map_*` podem mudar os botões físicos.
As ações normais continuam no caminho nativo de controle.

| Ação | Controle Vita |
| --- | --- |
| Ataque básico | Quadrado |
| Especial, quando disponível | Triângulo |
| Carta de batalha | Círculo, no menu de ações |
| Confirmar alvo ou seleção | X |
| Cancelar seleção | Círculo, conforme o contexto |
| Selecionar alvo | Direcional ou analógico esquerdo |
| Pausar | Start |
| Fugir, quando permitido | LT + RT: manter os quadrantes superiores do touch traseiro |

Todos os 24 tipos de prompt identificados em `Battle_PrepareMiniGame` e
`Battle_HandleInput` ficam cobertos pelas seguintes famílias:

| IDs nativos | Ação | Entrada |
| --- | --- | --- |
| 0–3 | Pressionar verde/vermelho/azul/amarelo | X/Círculo/Quadrado/Triângulo, respectivamente |
| 4–7 | Esquerda/direita/cima/baixo | Direcional ou movimento do analógico esquerdo |
| 8–11 | Repetir verde/vermelho/azul/amarelo | Pressões repetidas de X/Círculo/Quadrado/Triângulo |
| 12–15 | Barra de precisão, botão | Nova pressão do botão indicado na janela correta |
| 16–19 | Barra de precisão, direção | Nova direção no direcional ou analógico na janela correta |
| 20 | Carregar com movimento | Mover continuamente o analógico esquerdo; segurar não carrega |
| 21 | Pressão aleatória | O jogo resolve para um dos tipos 0–3 |
| 22 | Repetição aleatória | O jogo resolve para um dos tipos 8–11 |
| 23 | Precisão aleatória | O jogo resolve para um dos tipos 12–15 |

Botão errado continua sendo erro. Segurar não gera novas pressões nem acertos
automáticos nas barras de precisão. O cursor sintético fica restrito ao
handler de movimento, sem enviar toques ao Android ou modificar o estado
compartilhado do controle.

Offsets conferidos em `VirtualGamepad::Update` e nos consumidores: bordas em
168 + ButtonID, soltura em 218 + ButtonID, pressionado em 268 + ButtonID,
A/B/X/Y com IDs 2–5, direcional com IDs 12–15, mouse esquerdo com ID 16,
eixos esquerdos em 108/112. `Clear` copia 724 bytes.

## Correção após teste no Vita

O log enviado confirmou falha no prompt 20. A versão anterior confundia a
redução normal de `g_BattleMiniGameTimer` com um reinício, descartando o delta
do analógico a cada quadro. `_UpdateMinigame` em `0x7cc61c` subtrai o tempo
transcorrido: agora somente um aumento do cronômetro reinicializa a posição.
O teste foi alterado para contagem regressiva e reproduziu a falha antes da
correção. O log de inicialização identifica esta versão como
`motion bridge v2 (countdown timer)`. O usuário confirmou no Vita que a correção do analógico funcionou.

## Verificação reproduzível

Na raiz do port:

```sh
python3 tests/controls_host.py
python3 tests/apk_controls_audit.py ../CostumeQuestPowerVR.apk
./build-vita.sh Release console
```

Os testes locais exercitam o código C com substitutos das APIs do Vita:
mapeamento, traseiro, amostra consistente, extremos dos eixos, falha de leitura,
pressão/segurar, timing, direções, botão errado, fallback de toque e escopo
do cursor, entrada sem salto, ausência de carga parado, normalização diagonal e
ganho comparável a 30/60 FPS. Não executam o motor ARM ou o Scaleform.

A auditoria abre todos os GFX, confere imports de ícones e texturas e valida
assinaturas nativas usadas pelos patches. Não comprova execução de todos os
ramos de ActionScript, renderização ou legibilidade em hardware.

## Roteiro no Vita — pendente nesta revisão

| Contexto | Verificar |
| --- | --- |
| Inicialização, seleção e carregamento | Start/X, seleção, confirmação, voltar e ícones desde a primeira tela |
| Exploração e diálogos | Analógicos conforme ações do jogo, interação, fantasias, avanço e cancelamento |
| Caderno, mapa, cartas e fantasias | Navegação, abas pelos comandos indicados (L/R no controle; LT/RT no traseiro superior), confirmar e sair |
| Lojas, venda e diálogos | Seleção e confirmação/cancelamento sem tocar |
| Pausa, opções, cutscenes e créditos | Abrir/fechar, pular quando permitido e retomar sem botões presos |
| Maçãs | A e movimento lateral nas três posições, pausa e saída |
| Combate normal e DLC | Ataque, especial, cartas, alvos, fuga e cancelamento |
| Cada fantasia/habilidade | Pressão, repetição, barras de precisão, quatro direções e movimento contínuo |
| Alternativa frontal | Tocar/arrastar executa ações disponíveis, mantendo a interface de console |
| Remapeamento | Trocar A/B e fontes traseiras; verificar ações e minigames |
| Menu do port | L+R+Start abre apenas o menu do port; fechamento e retomada normais |
| Modo alternativo | Reiniciar com `controls touch` e verificar toque, gestos e interface Android |

Os ícones representam ações lógicas PlayStation. Remapeamento arbitrário não
redesenha dinamicamente os ícones para a nova fonte física. Guide não tem ação
no executável original. Não há comprovação de funcionamento 100% no Vita sem
executar o roteiro, especialmente todas as habilidades e o DLC.

## Revisão: toque complementar

Todos os comandos SDL de controle permanecem mapeados: A/B/X/Y, Start/Back,
LB/RB, LT/RT, L3/R3, quatro direções e os dois analógicos. Guide continua sem
consumidor no jogo. Os 24 prompts de combate usam a ponte descrita acima;
as ações nativas de menus e exploração permanecem com seus consumidores
originais, sem inventar ações globais para botões específicos de um contexto.

O toque frontal agora atravessa amostragem, callback Android e interpretação
nativa dos prompts. Em prompts de botão/direção, uma pressão física simultânea
tem prioridade. No prompt de movimento, um gesto frontal ativo tem prioridade;
seu deslocamento é ajustado pela razão 0,0007/0,12 para preservar o ganho touch.
Ao voltar ao analógico, a referência é reinicializada para evitar saltos.

O roteiro físico fica a cargo do usuário, conforme solicitado. Testes locais
cobrem encaminhamento de toque, escala do cursor e retorno ao analógico;
não equivalem a executar cada tela ou habilidade no Vita.

## Ícones embutidos e confirmação de fuga

Algumas telas não importam os botões da biblioteca compartilhada: usam bitmaps
locais (`HUDInteractions_I12`, `MessageBox_IA/I7`, `notebook_IE7`, com equivalentes
no DLC). O binding dessas imagens agora aponta para os recursos originais de PS3.
As referências explícitas a `Xbox360_ControllerIcons/BUTTON_*` também são resolvidas
para a biblioteca PS3. Nenhuma textura do jogo é incluída no código-fonte.

O comando de fuga chama `ModalDialog::Show` com tipo 4 (Sim/Não). O `onEvent`
de `MessageBox.gfx` só fecha via A nos tipos 1/3 e via B nos tipos 2/3.
A ponte de entrada trata o tipo 4 com as bordas virtuais A/B e chama
`ModalDialog::Close(1/2)`, mantendo o callback nativo e a liberação normal da tela.
Ela roda no input Flash da própria janela; não descarrega um filme durante seu
callback e não afeta as demais caixas de diálogo.

Além disso, `Battle_HandleInput` testa LT+RT pressionados em todos os quadros,
sem verificar se já existe um modal. O port bloqueia a entrada de combate enquanto
há um modal ativo e aguarda soltar o comando de fuga antes de retomá-la. Isso evita
empilhar confirmações, reabrir a janela ao cancelar ou enviar o toque de Sim/Não
para o combate que está por baixo.

Regressão no computador:

```sh
cc -Wall -Wextra -Werror -Isource tests/controls_compat_test.c source/controls_compat.c -o /tmp/cq-controls-test
/tmp/cq-controls-test
```

Validação no Vita ainda necessária: abrir/cancelar repetidamente com X/círculo e
toque, segurar LT+RT ao cancelar, confirmar a fuga e conferir as telas dos prints.

## Revisão de fantasias, tamanho do X e rolagem

`CostumeChooser` e sua variante DLC importam `Shared/PC_Keys.swf`; os símbolos
A/B/X/Y, gatilhos, ombros e analógicos dessa biblioteca agora também apontam
para PS3. As teclas reais de teclado permanecem disponíveis.

`HUDInteractions_I12` e `HUDInteractions_DLC_IF` são bitmaps de 64 pixels,
enquanto `PS3/BUTTON_A` tem 32. O fill matrix original usa 10 twips por pixel;
a troca direta deixava o desenho pela metade. Os filmes de interação recebem
uma matriz de 20 twips por pixel para preencher os limites originais do botão.

O jogo carrega `Notebook_DLC`. Seu `Quests.update` aplicava a barra de rolagem,
mas depois sempre restaurava `QuestHolder.target_y` e chamava `ForcePosition`.
Assim, desfazia tanto o input dos analógicos quanto o das barras. A correção
atual conserva `Quests.update` byte por byte e altera apenas o getter
`QuestHolder.getTargetY`: ele retorna `target_y` durante o arraste e `_y`
fora dele. O `init` mantém suas duas atribuições e omite apenas seu trace;
a soma dos tamanhos dos dois métodos permanece igual.

O log do teste no Vita da versão anterior confirmou o carregamento do override,
mas registrou `Invoked method is not a function` repetidamente. A substituição
sintética de `Quests.update` foi retirada; o teste no computador agora exige que
o método e seu cabeçalho sejam idênticos aos originais, além de executar o getter
e o init corrigidos e os casos de rolagem. O boot identifica a nova revisão como
`scroll v2 (native update + drag-aware target)`.

`controls_gfx.c` valida o tamanho e CRC dos filmes originais antes de alterar
qualquer byte. Os overrides são comprimidos e servidos da memória, sem alterar
o APK. O script abaixo executa o AVM1 original e o corrigido: reproduz a falha
original e verifica ambos os analógicos, toque na barra, arraste, soltura e limites.
Ele também confere os overrides contra os arquivos originais do usuário:

```sh
python3 tests/notebook_gfx_test.py
```

O usuário confirmou a rolagem funcionando no Vita com a revisão `scroll v2`.
A cobertura visual das demais telas continua dependendo de teste no aparelho.

## Pausas e gravação de diagnóstico

No log recebido, `profiler 1` estava ativo e os shaders permaneceram em zero
compilações durante as pausas. Há esperas por recursos e picos no trabalho do
jogo; o overlay sozinho não permite atribuir todas as pausas a uma única causa.
Também havia duas ou três gravações síncronas no cartão por mensagem de log,
feitas sob o mesmo mutex usado pelo jogo e pelo heartbeat.

O logger agora usa uma fila limitada de 64 KB e uma thread que agrupa gravações.
O produtor apenas copia a mensagem; nunca espera uma escrita normal do cartão.
Se a fila encher, descarta novas mensagens de diagnóstico em vez de pausar o
jogo. O encerramento normal drena a fila; o relatório de crash usa gravação direta
sem esperar os mutexes de uma thread que pode ter falhado.
Release também deixa de duplicar essas mensagens no console. A fila tem testes
de ordem, overflow, wrap e lotes sucessivos em `tests/log_queue_test.c`.

Com profiler ativo, quadros de 250 ms ou mais geram uma linha `hitch:` com os
tempos de jogo/troca de buffer e a atividade de leitura do cartão no período.
Isso permite separar as pausas remanescentes de carregamento, jogo e GPU no
próximo teste em hardware; não se afirma que todo engasgo esteja eliminado.

## Log com rolagem confirmada e investigação das pausas

O log da compilação de 22/10/2026 às 19:00:34 confirma o logger assíncrono e não
repete o erro de função do Flash. Mostra quadros de aproximadamente 0,9 segundo
com zero leituras do cartão (por exemplo, aos 110938, 113148 e 193833 ms).
Nesses períodos, a atualização de partículas aparece entre os maiores custos;
isso aponta para trabalho do jogo, mas o agregado do profiler não prova qual
rotina causou cada quadro. Os carregamentos iniciais têm leituras substanciais
e devem ser avaliados separadamente.

As duas rotinas nativas `_WarmupParticles`, para partículas e ribbons, fazem
`max(requested, 90)` antes de simular o efeito. Isso força aproximadamente 338
iterações, mesmo quando `Init` já limitou a preparação pela duração do efeito.
A correção muda apenas a condição ARM desse limite para
`min(requested, 90)`: um pedido de um segundo exige quatro iterações.
O passo de simulação e o restante do efeito permanecem os do jogo.
É uma hipótese específica para os engasgos; ainda requer validação no Vita,
inclusive da aparência dos efeitos ao surgirem.

`particle_compat.c` confere as instruções e a constante em ambas as rotinas
antes de instalar qualquer alteração. APKs com código diferente são recusados
nesse ponto. Com `profiler 1`, chamadas de preparação que demoram pelo menos
50 ms geram `particle warmup:` com tipo, duração solicitada e tempo real,
permitindo avaliar as pausas que restarem.

```sh
python3 tests/particle_warmup_test.py
```

O teste usa as duas rotinas da biblioteca original, verifica a condição ARM e
os limites, compara a quantidade de passos e exige que código incompatível
seja recusado sem modificações. O pacote desta revisão é
`build/costume_quest_scroll_v2_particles.vpk`, para a instalação com `game.apk`.

## Cache de arquivos: esperas independentes

No teste seguinte, o usuário confirmou melhora importante nos engasgos. O log
mostra períodos longos a 30 FPS e preparações de partículas de 20 segundos
custando aproximadamente 110 ms. As maiores pausas restantes incluem
carregamentos: aos 203646 ms, por exemplo, o quadro de 8276 ms coincide com
790 leituras e 4910 ms acumulados de IO. Ainda existem pausas sem leitura;
não se atribui todas elas ao cache.

`get_cache_locked` esperava pelo mutex de um arquivo mantendo o mutex global
da tabela de descritores. Assim, uma segunda operação no arquivo ocupado
impedia também operações em arquivos independentes. `fdcache_detach` tinha
o mesmo problema ao esperar uma leitura terminar.

O cache agora mantém referências para operações ativas e para as que aguardam
seu mutex. O mutex global protege apenas a tabela e essas referências;
as esperas por arquivo e por fechamento acontecem fora dele. O fechamento
usa um semáforo para aguardar a última referência antes de liberar o cache.
A inicialização do mutex global também passa a ser única entre threads.
Não se aumenta o tamanho dos buffers nem se altera o APK.

O teste abaixo reproduz o bloqueio na implementação anterior e verifica que
um descritor independente responde enquanto outro está ocupado e sendo
fechado. Também confere leitores enfileirados, dados entre blocos, EOF e
fronteiras dos overlays, com AddressSanitizer e UndefinedBehaviorSanitizer:

```sh
cc -Wall -Wextra -Werror -fsanitize=address,undefined -g -pthread \
  -Itests/fdcache_host -Isource tests/fdcache_concurrency_test.c \
  -o /tmp/cq-fdcache-test
/tmp/cq-fdcache-test
```

O boot identifica a revisão com `fdcache: per-descriptor waits ... (v2)`.
O pacote é `build/costume_quest_particles_io_v2.vpk`, para `game.apk`.
O ganho real ainda precisa ser medido no Vita; leituras lentas do cartão e
construção de recursos continuam tendo custo próprio.
