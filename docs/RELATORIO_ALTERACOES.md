# Relatório de alterações — branch `feat/dual-canvas-meld-ui`

Base: `product/multistream-mvp` (commit `b8f4e19f6`). Data: 18/10/2026.

Objetivo: trazer para o OBS Otaku as ideias 1 e 2 da análise do Meld Studio
(cenas com duas telas e painel único de saídas) e deixar a interface mais
bonita, funcionando igual no Windows e no Linux. Nada do código do Meld foi
copiado; só ideias de recurso e de visual (ver `docs/product/MELD_IDEIAS.md`).

## O que foi feito

### 1. Cenas com duas telas (horizontal 16:9 + vertical 9:16)
- Nova tela (canvas) persistente **"Vertical 9:16"** (1080×1920), criada com a
  API `obs_canvas` do libobs 32.2 e salva junto com a coleção de cenas.
- Cada cena horizontal ganha uma **cena vertical ligada** a ela
  ("<nome da cena> · 9:16"). Ao trocar a cena do programa, a tela vertical troca
  junto, automaticamente (também no Modo Estúdio, seguindo o programa).
- Uma cena vertical nova começa como **espelho** da horizontal (a cena inteira,
  preenchendo ou encaixando no 9:16), então nada fica preto.
- Ao apagar uma cena horizontal, a vertical ligada é removida; ao renomear, a
  vertical é renomeada.
- A **prévia vertical** agora mostra a tela vertical de verdade e virou um
  editor simples de camadas:
  - lista das camadas da cena vertical (marcar/desmarcar = mostrar/esconder);
  - **+** adiciona uma fonte da cena horizontal (ou a cena inteira);
  - **−** remove, **▲/▼** muda a ordem, **⤢** encaixa a camada no quadro 9:16;
  - **Espelhar horizontal** volta a cena vertical para o espelho;
  - duplo clique abre as propriedades da fonte;
  - "Enquadramento" (Preencher/Encaixar) agora vale para o espelho da cena atual
    e pode ser mudado mesmo ao vivo.
- Os canais verticais (TikTok etc.) passam a transmitir **essa tela vertical**,
  com as camadas que você montou. Se por algum motivo a tela não existir, o
  comportamento antigo (espelho automático) continua como reserva.

### 2. Painel único de saídas (dock "Saídas")
- Lista todos os destinos do multistream com: selo da tela (16:9 azul / 9:16
  rosa), nome e plataforma, status colorido (DESLIGADO, OFFLINE, CONECTANDO,
  AO VIVO, FALHOU — com o erro no tooltip) e botão Editar.
- Cada destino tem um seletor **Principal 16:9 / Vertical 9:16**. A troca é
  salva no canal (bloqueada durante a live, com aviso).
- Mostra o **compartilhamento de encoder**: destinos na mesma tela usam o mesmo
  encoder de vídeo ("Divide o encoder Principal 16:9 com 3 destinos"), e o
  topo resume "N destinos ativos · M encoders de vídeo". Ao vivo, mostra o id
  do encoder em uso.

### 3. Visual (UI/UX)
- Novo tema **"Otaku Dark"** (variante do Yami, arquivo
  `frontend/data/themes/Yami_Otaku.ovt`): superfícies escuras azuladas, destaque
  violeta, cantos arredondados, linhas de cenas/fontes mais altas com seleção
  arredondada e marcador lateral, botões Transmitir/Gravar destacados, tooltips
  arredondados, moldura rosa na prévia vertical.
- Cores fixas por tela em todos os temas Yami e no tema Sistema: azul = principal
  16:9, rosa = vertical 9:16 (selos `canvasBadge`).
- Estilos do painel de saídas e do editor de camadas nos temas Yami e Sistema.
- Corrigido: o tema Yami usava as variáveis `--bg_secondary` e `--border` sem
  defini-las (os cartões das docks de multistream ficavam sem fundo/borda).

### 4. Correção de build no Linux
- `OBSBasic_MainControls.cpp` só incluía os cabeçalhos do multistream dentro de
  `#ifdef _WIN32`, então **o fork não compilava no Linux**. Agora compila.

## Arquivos alterados

Novos:
- `frontend/utility/DualCanvasController.cpp/.hpp` — tela vertical e cenas ligadas
- `frontend/docks/OutputsDock.cpp/.hpp` — painel de saídas
- `frontend/data/themes/Yami_Otaku.ovt` — tema Otaku Dark
- `docs/product/MELD_IDEIAS.md`, `docs/product/img/dual-canvas-linux.png`, este relatório

Modificados:
- `frontend/utility/MultiStreamManager.cpp/.hpp` — usa a tela vertical da coleção
  quando existe; `EncoderGroups()` para o painel
- `frontend/docks/VerticalPreviewDock.cpp/.hpp` — renderiza a tela vertical e edita camadas
- `frontend/widgets/OBSBasic.cpp/.hpp`, `OBSBasic_OutputHandler.cpp`,
  `OBSBasic_Docks.cpp` — cria e liga o controlador e as docks
- `frontend/widgets/OBSBasic_MainControls.cpp` — correção do build no Linux
- `frontend/cmake/ui-docks.cmake`, `frontend/cmake/ui-utility.cmake` — novos arquivos
- `frontend/data/locale/pt-BR.ini`, `en-US.ini` — textos novos
- `frontend/data/themes/Yami.obt`, `System.obt` — estilos e variáveis

## Testes feitos (Linux, Debian 13, Qt 6.8.2, GCC 14)
- Build completo com `-Werror` (todos os alvos, sem browser/websocket/VLC): **OK**.
- Execução em tela virtual (Xvfb, OpenGL por software):
  - 1ª abertura: cria a tela "Vertical 9:16" e a cena "Scene · 9:16"; tema
    Otaku Dark aplicado; encerramento limpo, 0 vazamentos de memória.
  - 2ª abertura com uma fonte de cor na cena: a tela e a cena vertical são
    recarregadas da coleção (sem duplicar) e a prévia vertical mostra o espelho
    corretamente (print em `docs/product/img/dual-canvas-linux.png`).
- **Não testado**: transmissão real para um destino vertical, Windows, e o tema
  em telas com escala de DPI alta. Os testes de produto (`product-oauth-tests`)
  só existem para Windows.

## Como testar

### Linux (seu PC, Fedora/KDE)
```bash
cd /run/media/luka/D274D96274D94A3D/Obs
git fetch origin
git checkout feat/dual-canvas-meld-ui
git submodule update --init plugins/obs-browser plugins/obs-websocket
# dependências: veja o guia oficial "Build Instructions For Linux" do OBS (Fedora)
cmake -S . -B build_linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOBS_VERSION_OVERRIDE=32.2.2 -DENABLE_BROWSER=OFF -DENABLE_AJA=OFF \
  -DENABLE_WEBRTC=OFF -DENABLE_NVENC=OFF -DENABLE_QSV11=OFF
cmake --build build_linux
./build_linux/rundir/RelWithDebInfo/bin/obs
```

### Windows
```powershell
git fetch origin
git checkout feat/dual-canvas-meld-ui
cmake --build build_product_x64 --config RelWithDebInfo   # reconfigura sozinho
```
(ou `cmake --preset windows-x64` e `cmake --build --preset windows-x64`).

### Roteiro de teste
1. Configurações → Aparência → Tema: **Otaku Dark**.
2. Docks → marque **Prévia vertical** e **Saídas**.
3. Crie 2 cenas. Troque entre elas: a prévia vertical deve trocar junto, e o
   cabeçalho mostra "Versão vertical de …".
4. Na prévia vertical, use **+** para colocar a câmera em cima e o jogo embaixo;
   use ▲/▼, ⤢ e o duplo clique. Feche e abra o OBS: o layout vertical continua.
5. No painel **Saídas**, mude um canal para Vertical 9:16 e entre ao vivo: o canal
   vertical deve sair com o layout montado; o status muda para AO VIVO.
6. Tente trocar a tela de um canal ao vivo: deve aparecer o aviso de bloqueio.

## Pendências
- Edição de posição/tamanho arrastando direto na prévia vertical (hoje só pelos
  botões de camada: encaixar, ordem, mostrar/esconder).
- Transição animada na tela vertical (hoje ela troca de cena na hora).
- Ideias 3 a 8 do `MELD_IDEIAS.md`.
- Destinos com configurações de encoder diferentes na mesma tela (hoje todos os
  destinos de uma tela usam o mesmo encoder).
- Teste de live real e teste no Windows.

## Riscos conhecidos
- A tela vertical fica sempre renderizando (1080×1920), mesmo sem live: custo
  extra de GPU pequeno, mas existe. Em PCs fracos pode pesar.
- Trocar de coleção de cenas **durante uma live com canal vertical** remove a
  tela vertical que o encoder está usando; evite fazer isso ao vivo.
- A tela vertical guarda o FPS do momento em que foi criada (máx. 60). Se mudar
  o FPS principal, ela não acompanha automaticamente.
- Coleções de cenas salvas com esta versão têm uma tela extra; versões antigas do
  fork ignoram ela, mas as cenas "· 9:16" podem aparecer em plugins que listam
  todas as telas.
- O tema muda estilos globais de listas e botões; se algum plugin ficar estranho,
  volte para o tema Yami padrão.

---

# Rodada 2 — ideias 3 a 8 (branch `feat/meld-ideias-3-8`)

Tudo foi reimplementado do zero com Qt/libobs (sem código ou QML do Meld) e
funciona igual no Windows e no Linux.

## O que foi feito
1. **Transição animada na tela vertical.** A tela 9:16 agora troca de cena com
   transição própria, começando junto com a principal. Na prévia vertical há o
   seletor **Transição**: *Igual à principal* (copia tipo e ajustes), *Fade* ou
   *Corte*. Fica salvo na config (`DualCanvas/Transition`).
2. **Replay instantâneo na tela (ideia 4).** Novo dock **Replay na tela** com
   botões 10/30/60/90 s e **Fechar replay**. Ele salva o replay buffer e mostra
   os últimos N segundos por cima do programa (canal de saída 10, sem mexer em
   nenhuma cena), e fecha sozinho no fim do vídeo. Atalhos em Configurações →
   Atalhos: "Replay: últimos 10/30/60/90 s" e "Replay: fechar".
   O botão **Iniciar replay buffer** aparece quando o buffer está desligado.
3. **Áudio por fonte e por cena (ideia 5).** No menu do mixer (botão direito
   ou ⋮):
   - **Fora do VOD (trilha N)**: tira a fonte da trilha de VOD (ex.: música com
     direito autoral fica só na live).
   - **Lembrar volume e mudo na cena "X"**: ao voltar para essa cena, o volume e
     o mudo da fonte são reaplicados. **Esquecer volume da cena** desfaz.
4. **Trilha de VOD da Twitch no painel Saídas (ideia 3, parte).** Destinos Twitch
   mostram **Trilha de VOD separada** + escolha da trilha (1–6). Bloqueado
   durante a live e não deixa usar a mesma trilha da live.
5. **Backups versionados da coleção (ideia 7).** A cada salvamento, se a coleção
   mudou e passaram 10 min, é guardada uma cópia em
   `<pasta de cenas>/otaku_backups/<coleção>/AAAAMMDD-HHMMSS.json` (mantém 30).
   Menu **Coleção de cenas → Backups da coleção (versões)…** força um backup e
   abre a pasta. Para restaurar: Coleção de cenas → Importar o arquivo. Para
   arquivos sumidos use **Arquivo → Mostrar arquivos ausentes** (assistente de
   relink que já existe no OBS).
6. **Tema Otaku Dark.** Menus arredondados, barras de rolagem finas, barra de
   status, botões do replay e destaque roxo nas caixas de seleção.

## Arquivos
- Novos: `frontend/docks/ReplayDock.*`, `frontend/utility/SceneAudio.*`,
  `frontend/widgets/OBSBasic_LocalBackups.cpp`.
- Alterados: `DualCanvasController.*`, `VerticalPreviewDock.*`, `OutputsDock.*`,
  `VolumeControl.cpp`, `OBSBasic.*`, `OBSBasic_Docks.cpp`,
  `OBSBasic_OutputHandler.cpp`, `OBSBasic_SceneCollections.cpp`, CMake (docks,
  utility, widgets), locales en-US/pt-BR, temas `Yami.obt` e `Yami_Otaku.ovt`.

## Testes feitos (Linux, Xvfb)
- Build com `-Werror` sem erros/avisos.
- Troca Cena → Cena 2 com transição de 3 s: no meio dela a prévia principal e a
  vertical mostram a mesma mistura de cores (sincronizadas).
- Replay buffer ligado pelo dock, botão 10 s: arquivo salvo e log
  "Replay: playing last 10 s"; overlay mostrado e status "Mostrando…".
- Backup criado no salvamento (log "Backup: saved scene collection version").

## Como testar
1. Docks → **Replay na tela**. Clique **Iniciar replay buffer**, espere alguns
   segundos e clique **10 s**: o replay aparece por cima; **Fechar replay** some.
2. Crie 2 cenas e troque: a prévia vertical faz a transição junto. Teste o
   seletor *Fade/Corte*.
3. No mixer, botão direito numa fonte → **Fora do VOD**; e **Lembrar volume…**
   numa cena, mude o volume em outra cena e volte.
4. Saídas → destino Twitch → **Trilha de VOD separada**.
5. Coleção de cenas → **Backups da coleção (versões)…**.

## Pendências
- Formato duplo da Twitch (horizontal+vertical na mesma live) — exige o
  Enhanced Broadcasting/multitrack; arriscado, ficou de fora.
- Replay aparece só na tela horizontal (a vertical ainda não tem overlay).
- Ideias 6 e 8 não foram feitas.
- Restaurar backup com um clique (hoje é via Importar).
- Teste no Windows e live real.

## Riscos
- O replay usa o canal de saída 10; um plugin que use esse mesmo canal pode
  conflitar.
- "Igual à principal" copia os ajustes da transição; transições de plugin muito
  específicas (stinger com arquivo de outro tamanho) podem ficar diferentes na
  vertical — use Fade nesse caso.
- Os backups ocupam espaço (30 cópias por coleção).
