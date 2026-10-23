# Costume Quest Vita 1.0.0

A versão 1.0 consolida as correções de interface PlayStation, rolagem do caderno,
confirmação de fuga e as otimizações aceitas pelo usuário no Vita. O título
permanece `CSTMQST01`, com versão de aplicativo `01.00`.

## Instalação e atualização

1. Mantenha o APK PowerVR compatível em `ux0:data/costumequest/game.apk`.
2. Instale `CostumeQuestVita-v1.0.0.vpk` pelo VitaShell sobre a instalação atual.
3. Preserve `ux0:data/costumequest/save/`, traduções e caches existentes.
4. Abra o jogo. O log identifica o port como `Costume Quest Vita 1.0.0`.

O console precisa de `kubridge.skprx` e `libshacccg.suprx`. O pacote não inclui
APK, bibliotecas originais do jogo, saves nem traduções. As bibliotecas são
extraídas do APK no primeiro início. Esta distribuição usa o APK inteiro,
conforme a instalação testada; não é a variante de arquivos extraídos.

Padrões para novas configurações: `fps_cap 30`, `msaa 0`, `profiler 0`,
`particle_leaves 1`, `allocator fast`, `controls gamepad`, `language auto`.
Atualizações preservam os valores existentes no `config.txt`. Para adotar os
padrões de vídeo e desligar o diagnóstico numa instalação já usada nos testes,
abra o port menu com L + R + Start e escolha FPS limit 30, MSAA Off e Profiler
Off. Alterar MSAA requer reabrir o jogo. O port menu contém apenas as opções
de vídeo, profiler e retorno ao jogo; os mapeamentos existentes são mantidos.

## Alterações consolidadas

- Port menu em inglês, inclusive botões, touch traseiro, avisos e instruções.
- Interface PlayStation nas interações, seleção de fantasias e diálogos.
- Botão X de interação no tamanho corrigido.
- Rolagem das missões pelos analógicos e caminhos de interação do caderno.
- Confirmação de fuga com confirmação/cancelamento e fechamento do modal;
  comandos segurados não empilham confirmações.
- Limite de preparação nativa de partículas corrigido.
- Preenchimento progressivo das folhas ambientais, com orçamento compartilhado
  entre instâncias. As folhas começam menos densas nos primeiros segundos.
- Cache de arquivos sem espera global por uma leitura de outro descritor.
- Log assíncrono com fila limitada e diagnóstico desligado por padrão.

## Evidências e limites

No último teste aceito pelo usuário, o diagnóstico registrou 11 preparações
progressivas iniciadas, cinco concluídas e seis canceladas pelo ciclo de vida
das instâncias, sem pendências no último resumo. Foram 578 etapas, com máximo
de 2663 us e nenhum excesso da meta compartilhada de 4000 us. Não houve resets
sem identidade. As preparações contínuas restantes não ultrapassaram 50 ms;
o maior tempo registrado foi 20270 us. Os trechos de exploração mostram
frequentemente aproximadamente 30 FPS.

Carregamentos iniciais e transições ainda podem pausar por vários segundos.
A captura não comprova campanha completa, todos os efeitos ou todo o DLC.
O menu em inglês, a identificação de versão e os padrões consolidados foram
verificados localmente na preparação do pacote; são mudanças posteriores à
captura de desempenho fornecida pelo usuário.

A configuração `particle_leaves 0` restaura a preparação contínua no próximo
início. `profiler 1` habilita métricas detalhadas se for necessário investigar
um problema; o nome do arquivo de log continua sendo `debug.log`.

## Compilação reproduzível

O código-fonte da versão acompanha a distribuição separadamente. Requer
VitaSDK softfp e as dependências descritas no README do projeto. Use um caminho
sem espaços para o código ou um link simbólico, devido ao empacotador do SDK.

```sh
export VITASDK=/caminho/para/vitasdk-softfp
export PATH="$VITASDK/bin:$PATH"
export CPATH="$(pwd)/lib/vitagl/source${CPATH:+:$CPATH}"
cmake -S . -B /tmp/cq-release-build -DCMAKE_BUILD_TYPE=Release \
  -DFMOD_DEBUG=OFF -DHEAPGUARD=OFF -DGL_TRACE=OFF -DGL_CENSUS=OFF
cmake --build /tmp/cq-release-build -j4
```

CMake recente pode precisar de `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` para
bibliotecas auxiliares. O exemplo exige que o diretório do código não tenha
espaços. O script `build-vita.sh` mantém o caminho alternativo via OrbStack.

O pacote de distribuição inclui VPK, instruções em inglês, notas da versão,
licenças e checksums SHA-256. O arquivo de fontes inclui as alterações locais
consolidadas, sem o APK ou arquivos de dados originais do jogo.
