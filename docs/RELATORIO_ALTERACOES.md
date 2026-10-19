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
