# Costume Quest — PS Vita (so-loader)

Port experimental do Costume Quest (build Android/OUYA "PowerVR", 2015) para
PS Vita, carregando as bibliotecas ARM originais do APK com
[so_util](https://github.com/Rinnegatamante/so_util),
[FalsoJNI](https://github.com/v-atamanenko/FalsoJNI) e
[vitaGL](https://github.com/Rinnegatamante/vitaGL), a partir do
[soloader-boilerplate](https://github.com/v-atamanenko/soloader-boilerplate).

**Estado:** esqueleto. Gera o VPK, ainda não foi testado em hardware.

## Como funciona

| Android | No Vita |
| --- | --- |
| `libfmodex.so`, `libfmodevent.so`, `libCq.so` | Extraídos do APK no primeiro boot e carregados por so_util em 0x98010000 / 0x98210000 / 0x98410000; imports FMOD do libCq são ligados entre módulos via `DT_NEEDED` |
| `CqActivity.onCreate` | `main.c`: `dfNativeSetLanguage`, `dfNativeInit`, `setDeviceInfo` |
| `SDLSurface.surfaceChanged` + `SDLMain` | `onNativeResize(960x544)` e `nativeInit()` em uma thread com 4MB de pilha |
| EGL do SDL (`SDL_EGL_*`, `Android_GLES_SwapWindow`) | Hooks em `patch.c` → vitaGL (`vglInit` na primeira criação de contexto) |
| `AssetManager.openFd` do SDL (`Android_JNI_File*`) | Hooks em `patch.c` → stdio em `ux0:data/costumequest/assets/` |
| `org.fmod.FMODAudioDevice` (AudioTrack) | `fmod_audio.c`: thread que chama `fmodGetInfo`/`fmodProcess` e envia para `sceAudioOut` |
| Controle OUYA / toque | `vita_pad.c`: controle virtual "PS Vita" via SDL GameController (botões, analógicos, touch traseiro); toque frontal via `onNativeTouch` |
| Checagem de compra (OUYA trial) | `isPurchased()` retorna `true` |

Os dados do jogo (`assets/ant.mp3`, um zip de ~670MB sem compressão) são lidos
pelo próprio jogo de dentro do APK: `dfNativeInit` recebe o caminho do APK,
abre com `ZipArchive` e procura `assets/ant.mp3`. Por isso o APK é copiado
inteiro, sem extrair nada.

## Requisitos

- [VitaSDK **softfp**](https://github.com/vitasdk-softfp) (GCC 10.3) — as libs
  do APK são `armeabi-v7a` softfp. O VitaSDK hard-float em `~/vitasdk`
  (GCC 15.2) **não** serve, e não há build softfp para macOS.
- No console: `kubridge.skprx` e `libshacccg.suprx` (compilador de shaders; o
  jogo compila GLSL em tempo de execução).

### Ambiente no macOS (OrbStack)

O SDK roda numa máquina Linux amd64 do OrbStack chamada `vitadev`, instalado
em `~/vitasdk-softfp` dentro dela, com os pacotes `zlib`, `vitashark`,
`SceShaccCgExt`, `libmathneon`, `kubridge` e `taihen` (via
[vdpm softfp](https://github.com/vitasdk-softfp/vdpm)).

## Build

```bash
./build-vita.sh
```

- `./build-vita.sh Release` → `build/costume_quest.vpk`: para jogar (otimizado,
  só erros e relatório de crash no log).
- `./build-vita.sh` → `build/costume_quest_debug.vpk`: log detalhado, bem mais
  lento.
- `./build-vita.sh Release fmod` → `build/costume_quest_fmod.vpk`: Release com
  as bibliotecas debug do FMOD do APK (`libfmodexD`/`libfmodeventD`), que
  registram os erros de áudio no `debug.log` (linhas `[FMOD]`).

O heartbeat do log mostra FPS, tempo médio de quadro (jogo + espera na troca
de buffer) e o pior quadro a cada 5 s.

O vitaGL é compilado a partir de `lib/vitagl` com `SOFTFP_ABI=1`.

## Instalação no Vita

Basta copiar o APK original:

```
ux0:data/costumequest/
└── game.apk     ← o CostumeQuestPowerVR.apk original, renomeado
```

No primeiro boot o loader extrai `libCq.so`, `libfmodex.so` e
`libfmodevent.so` de `lib/armeabi-v7a/` dentro do APK para a raiz de
`ux0:data/costumequest/` (`source/apk_extract.c`, com checagem de CRC32).
Nos boots seguintes elas já existem e a extração é pulada; para forçar de
novo, apague as `.so` dessa pasta.

`libfmodexD.so`/`libfmodeventD.so` (debug) e `libfaad.so` não são usadas.

## Menu do port

**L + R + Start** abre o menu durante o jogo (o jogo fica pausado enquanto
ele está aberto):

- **Limite de FPS:** 60, 45, 30 ou sem limite. Vale na hora.
- **Antialiasing (MSAA):** desligado, 2x ou 4x. Vale no próximo boot.
- **Controles:** cada botão do controle do jogo pode vir de qualquer botão do
  Vita ou de um quadrante do touch traseiro.
- **Restaurar controles padrão**.

Cima/Baixo escolhe, Esquerda/Direita (ou X) muda o valor, O ou Start fecha.
Ao fechar, tudo é gravado no `config.txt`. O Start da combinação não chega ao
jogo, então ela não abre também o pause do jogo.

O menu é desenhado pela CPU direto na tela (`source/port_menu.c`), sem passar
pelo OpenGL do jogo.

## Configurações

`ux0:data/costumequest/config.txt` (criado no primeiro boot), uma opção por
linha no formato `chave valor`:

| Chave | Valores | Padrão | Efeito |
| --- | --- | --- | --- |
| `msaa` | 0, 2, 4 | 2 | Antisserrilhado; 0 alivia a GPU. Vale no próximo boot |
| `fps_cap` | 60, 45, 30, 0 | 60 | Limite de FPS. 60/30 com vsync; 45 e 0 (sem limite) sem vsync |

## Controles

O Vita aparece para o jogo como um controle (SDL GameController), com
analógicos de verdade. Padrão:

| Controle (Xbox) | Vita |
| --- | --- |
| A / B / X / Y | Cross / Circle / Square / Triangle |
| Back / Start | Select / Start |
| LB / RB | L / R |
| LT / RT | Touch traseiro, quadrante superior esquerdo / direito |
| L3 / R3 | Touch traseiro, quadrante inferior esquerdo / direito |
| D-pad, analógicos | D-pad, analógicos |

**Combate:** a versão Android só tem interface de toque nas batalhas
(ataques, especiais e defesa por toque/gesto). No Vita elas são jogadas na
tela de toque frontal; fora do combate dá para jogar só com os botões.

Remapeável no `config.txt` com `map_a`, `map_b`, `map_x`, `map_y`,
`map_back`, `map_start`, `map_lb`, `map_rb`, `map_lt`, `map_rt`, `map_l3`,
`map_r3`, `map_guide`, usando: `cross circle square triangle start select l r
up down left right rear_tl rear_tr rear_bl rear_br none`. Exemplo para
confirmar com Circle: `map_a circle` e `map_b cross`.

## Debug

Cada execução grava `ux0:data/costumequest/debug.log`; a anterior fica em
`debug.prev.log`. O log recebe o loader, o `__android_log_print` do jogo,
FalsoJNI e so_util, com o tempo em ms desde o início. Se o jogo crashar, o
handler de exceção do kubridge acrescenta um relatório com PC/LR já traduzidos
para `libCq.so+offset (símbolo)`, registradores e endereços de código na pilha.
Builds Debug registram tudo; Release só erros e o relatório de crash.

Diagnósticos pesados, desligados por padrão (deixam o jogo bem mais lento):

- `-DHEAPGUARD=ON`: guarda todas as alocações do processo (canário, quarentena
  de blocos liberados, varredura do heap a cada segundo) e loga o dono do
  bloco que estourou.
- `-DGL_TRACE=ON`: loga quais threads chamam GL e de qual função do jogo.
- `-DGL_CENSUS=ON`: loga cada formato de textura e layout de vértice distinto.

## Shaders

O jogo compila GLSL em tempo de execução (~1 s por shader no Vita). O vitaGL
guarda os shaders compilados em `ux0:data/shader_cache/CSTMQST01/`, então só
a primeira vez que cada shader aparece é lenta.

## Pendências conhecidas

- **Nunca rodou em hardware.** A ordem de chamadas imita o Java, mas faltam
  testes; erros de FalsoJNI (`GetMethodID ... not found`) vão indicar métodos
  Java ainda não implementados em `source/java.c`.
- **Memória:** o OUYA tinha 1GB; o Vita tem bem menos. Texturas e áudio podem
  estourar o heap (`_newlib_heap_size_user` em `main.c`, memória do vitaGL em
  `gl_init`).
- **Shaders:** `Data/Shaders/*.fxo` contêm GLSL ES; a compatibilidade com o
  tradutor GLSL do vitaGL ainda precisa ser verificada.
- **Controles:** o mapeamento por keycodes Android é um palpite baseado no
  controle OUYA; o jogo tem uma classe `SDLGamepad` que pode precisar de hooks
  para analógicos de verdade.
- **Saves:** o jogo grava em `/sdcard/dfp/cq-droid/_write`; o loader
  redireciona só essa pasta para `ux0:data/costumequest/save/`
  (`source/reimpl/io.c`). O resto de `/sdcard` continua inexistente, como o
  jogo espera.
- O APK foi reempacotado por terceiros (classes Java ofuscadas injetadas, ex.
  `C0001`, `MobileLinkQualityInfo`). Isso não afeta o lado nativo, mas o ideal
  é usar um APK original.
