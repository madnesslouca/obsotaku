# Ideias tiradas do Meld Studio

Análise estática do instalador público do Meld Studio 0.10.7.7 para Windows (MSIX),
feita com o REA. O programa não foi executado. O que serve aqui são ideias de
recurso e de UX. **Nenhum código, QML ou arte do Meld foi copiado**: tudo neste
fork foi escrito do zero sobre Qt e libobs.

O que o Meld é por dentro: C++ nativo sobre partes do Chromium, interface em
Qt 6/QML, gráficos com Skia e Direct3D 12. Só existe para Windows (e macOS); não
tem versão para Linux. O OBS Otaku já sai na frente rodando em Windows e Linux.

## As 8 ideias, em ordem de prioridade

1. **Cena com duas telas**: cada cena tem uma versão horizontal (16:9) e uma
   vertical (1080×1920). Trocar a cena troca as duas. → *feito nesta branch*
   (`DualCanvasController`, API `obs_canvas` do OBS 31.1+).
2. **Painel único de saídas**: cada destino escolhe qual tela transmite, e
   destinos da mesma tela dividem o mesmo encoder. → *feito nesta branch*
   (dock "Saídas").
3. **Twitch formato duplo e trilha de VOD** visíveis nesse painel. → pendente.
4. **Clipe com replay na tela**: atalho salva os últimos 10/30/60/90 s e o clipe
   toca por cima da cena. → pendente.
5. **Áudio seguro para VOD**: opção "fora do VOD" em cada fonte do mixer, volume
   e mudo por cena. → pendente.
6. **Pacote de efeitos prontos**: brilho, LUT, cantos arredondados, sombra,
   corner pin, com predefinições importáveis. No Linux, remoção de fundo via
   ONNX/OpenCV. → pendente.
7. **Backups com versão e importador**: histórico de versões no backup do Google
   Drive e assistente que reconecta arquivos perdidos. → pendente.
8. **Widgets e chat juntos**: alertas, timers e chat unificado com edição de
   título/categoria. → parcialmente existente (chat unificado e painel de
   informações da live).

## Ideias de visual (UI/UX) aproveitadas

- Superfícies escuras com leve tom azul, cartões arredondados e mais espaço.
- Uma cor de destaque só (violeta) para ações principais e seleção.
- Cada tela tem sua cor fixa: azul = horizontal 16:9, rosa = vertical 9:16,
  usada nos selos (badges), na moldura da prévia vertical e no painel de saídas.
- Status de cada saída como "pílula" colorida (AO VIVO, CONECTANDO, FALHOU).
