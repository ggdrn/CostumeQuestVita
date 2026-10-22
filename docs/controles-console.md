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
