# Revisão geral — agosto de 2026

Segunda leitura completa do código do produto, depois de ele passar a ser usado
de verdade. A [revisão anterior](REVISAO_MULTISTREAM.md) foi feita sobre código
recém-escrito; esta é sobre código que já transmitiu, e por isso encontra outra
categoria de problema.

Escopo: 12.247 linhas em 98 arquivos, de `0052d024f` (base OBS 32.2.1) até
`26513e5c1`.

## 1. Onde o produto está

Funciona, e funciona de ponta a ponta: conectar contas, transmitir para vários
destinos com encoders compartilhados, ler chat de três plataformas numa dock,
editar título e categoria durante a live, acompanhar saúde por destino na barra
e na janela de estatísticas.

Os dois marcos que faltam do `MVP.md` são os mesmos de sempre, e são os que
separam "funciona aqui" de "produto":

- **Marco 6** — assistente de primeira execução. Hoje o usuário cai num OBS
  cru com uma barra de canais vazia.
- **Marco 7** — empacotamento, avisos GPL e distribuição do código
  correspondente. Obrigatório para distribuir binário de um fork GPL.

## 2. Defeitos encontrados agora

### D1 — O apelido de IRC da Twitch sai do nome editável

`MultiStreamChatAggregator.cpp:415`

```cpp
twitchIrcNick = twitchDisplayName.trimmed().toLower();
```

`twitchDisplayName` vem de `channel.displayName`, que o usuário pode editar. Se
ele renomear o canal para "Meu canal", o `NICK` vira `meu canal` — com espaço —
o login autenticado falha, e a dock cai silenciosamente para `justinfan`. O
chat continua sendo lido, mas **enviar mensagem para de funcionar** sem nenhum
aviso.

É exatamente o defeito que já foi corrigido para o endereço do canal com o campo
`chatAddress`; o apelido de login ficou para trás. A correção é usar
`twitchChannel`, que já carrega o handle canônico.

**Severidade: alta.** Silencioso e quebra uma função anunciada.

### D2 — Envio no YouTube sempre relata sucesso

`MultiStreamChatAggregator.cpp:299`

```cpp
SendYouTubeText(trimmed, error);
error.clear();
return true;
```

O erro que `SendYouTubeText` acabou de preencher é apagado na linha seguinte. Se
o envio falhar de imediato — sem token, sem live ativa — o usuário vê a mensagem
sumir da caixa e nada acontecer.

**Severidade: média.**

### D3 — Servidor do TikTok fixo num CDN regional

`StreamPlatform.cpp:138`

```cpp
"rtmp://push-rtmp-l1-va01.tiktokcdn.com/live/",
```

Esse host é de um datacenter específico (Virgínia). O TikTok entrega servidor e
chave **juntos**, por sessão, no Live Studio. Pré-preencher um host fixo dá ao
usuário a impressão de que só falta a chave, e ele vai transmitir para o
servidor errado — ou para nenhum.

O certo é deixar o campo vazio e pedir os dois valores, como no RTMP
personalizado.

**Severidade: média.** Falha na hora de subir, que é a pior hora.

### D4 — Selos de papel da Twitch aplicados a todas as plataformas

Um moderador da Kick recebe a espada da Twitch. Já está anotado no README de
`frontend/data/images/chat-badges/`, mas continua sendo uma imprecisão visível.

**Severidade: baixa**, cosmética — mas é o tipo de detalhe que denuncia um
produto montado às pressas.

### D5 — A tag `badges` do IRC é lida em dois ramos exclusivos

`MultiStreamChatAggregator.cpp:519`

```cpp
else if (key == "badges" && value.contains("broadcaster/"))
        msg.isBroadcaster = true;
else if (key == "badges" && value.contains("vip/"))
        msg.isVip = true;
```

O segundo ramo só é alcançado quando o primeiro falha. Funciona por acidente —
quem é broadcaster e VIP perde o VIP, o que não importa — mas o padrão convida
ao erro na próxima adição. Falta também `founder/`, então assinantes antigos
aparecem sem selo.

**Severidade: baixa.**

### D6 — Bitrate não é conferido no modo Avançado

`MultistreamPreflight.cpp:71`

O aviso de bitrate acima do limite da plataforma só existe no modo Simples. Quem
usa o modo Avançado — justamente quem mexe em bitrate — não recebe aviso nenhum.
O valor está nas configurações do encoder e varia por encoder, o que explica a
omissão, mas não a resolve.

**Severidade: baixa.**

## 3. Limitações estruturais

Não são defeitos: são consequências de decisões tomadas. Estão aqui para serem
decididas de novo, agora com uso real.

### L1 — Uma conexão de chat por plataforma

`MultiStreamChatAggregator::SetChannels` usa `FirstOf()` e guarda um único
conjunto de estado por plataforma (`twitchChannel`, `kickChannel`, `ytAccountId`).
Com duas contas Twitch, **só a primeira aparece no chat**.

Esta é a limitação mais séria do conjunto, porque contradiz o que o produto
promete: o resto do sistema trata N contas por plataforma corretamente — a
barra, o store, o fan-out, as estatísticas — e só o chat não.

Custo de resolver: o agregador precisa deixar de ter campos por plataforma e
passar a ter uma lista de conexões. É a maior refatoração pendente, algo como
400 linhas reorganizadas.

### L2 — Kick não envia mensagem

Ler o chat da Kick usa o WebSocket do Pusher, que é só leitura. Enviar exige a
API oficial com o escopo `chat:write`, que existe. É trabalho pequeno e
fecharia a lacuna mais visível da dock.

### L3 — YouTube sem busca de categoria

`PlatformMetadataClient::SupportsCategories()` cobre Twitch e Kick. No YouTube o
usuário digita o título e não tem como escolher categoria pelo painel.

### L4 — Limites de plataforma escritos à mão

Os valores em `StreamPlatform.cpp` vieram das páginas de ingest de cada
plataforma e envelhecem em silêncio. Não há como saber que estão errados a não
ser lendo as páginas de novo. Vale pelo menos datar cada entrada.

### L5 — O canal principal é sempre o primeiro da lista

`FirstReadyChannel()` pega o primeiro pronto. O usuário não escolhe qual canal
vai no output principal, e é ele que recebe o tratamento diferente (aparece na
linha "Transmissão" das estatísticas, não passa pelo fan-out).

## 4. O que vale adicionar

Em ordem do que eu acho que rende mais por esforço:

### A1 — Eventos ao vivo da Twitch por EventSub WebSocket

Seguidores, inscrições, raids e bits chegando na dock, ao lado do chat.
**Não precisa de servidor público**: o EventSub tem transporte WebSocket, e o
token que já guardamos serve. É a adição de maior impacto percebido — é o que
faz a dock virar um painel de verdade — e reaproveita toda a camada de OAuth já
construída.

### A2 — Emotes no chat

Twitch manda as posições na tag `emotes`, e a Kick manda `[emote:id:nome]` no
próprio texto — hoje o usuário vê literalmente `[emote:37225:KEKLEO]`. Ambos
resolvem para URL de imagem previsível, e a dock já sabe embutir imagem por
resource. Muda muito a sensação de estar lendo um chat de verdade.

### A3 — Contagem de espectadores por canal

A barra mostra saúde de envio, mas não quantas pessoas estão assistindo. Twitch,
YouTube e Kick devolvem isso na mesma chamada que já fazemos para resolver
ingest. Um número por card.

### A4 — Assistente de primeira execução

Marco 6 do MVP. Conectar a primeira conta, escolher resolução e bitrate a partir
dos limites do catálogo, explicar a faixa de VOD. Hoje o primeiro contato é uma
barra vazia.

### A5 — Escolher o canal principal

Resolve L5 e é barato: uma opção no menu `⋮` do card e um campo no store.

### A6 — Perfis de destino

Conjuntos de canais que se ativam juntos ("só Twitch", "tudo"), para quem muda
de formato entre lives.

## 5. Dívidas de manutenção

### M1 — Sete `bool` em sequência no catálogo de plataformas

`StreamPlatformInfo` termina com sete booleanos, inicializados por posição:

```cpp
false, true, false, false, true, true, true,
```

Conferi um a um e estão todos certos hoje. Mas trocar dois é invisível na
revisão e não quebra a compilação — e a consequência seria, por exemplo, oferecer
faixa de VOD numa plataforma que a recusa. Vale usar inicializadores nomeados
(C++20, já disponível) ou uma estrutura `capabilities` aninhada.

### M2 — `MultiStreamChatAggregator.cpp` com 1.236 linhas

Três protocolos diferentes num arquivo só: IRC, WebSocket Pusher e polling
HTTP. Cada um já tem seu bloco separado por comentário, o que ajuda, mas a
refatoração de L1 é a oportunidade natural de dividir em três classes com uma
interface comum.

### M3 — Testes cobrem só as primitivas de OAuth

`test/product-oauth` valida PKCE, construção da URL de autorização, política por
provedor, seção de configuração e cofre de credenciais. Nada cobre o store de
canais, o preflight ou o parser de IRC — que são justamente as partes onde os
defeitos apareceram. O parser de IRC e o `MultistreamChannelStore` são puros o
bastante para testar sem interface.

## 6. O que eu faria primeiro

1. **D1** — é silencioso, quebra função anunciada, e a correção é de uma linha.
2. **D3** — falha na hora de subir a live.
3. **D2** e **D5** — pequenos, mesma área do código.
4. **L1** — a refatoração do chat para várias contas. É o maior trabalho da
   lista e o que mais aproxima o produto do que ele diz ser.
5. **A1** — eventos por EventSub, depois que o chat estiver multi-conta.

Deixaria **A4** (assistente) e o marco 7 (empacotamento) para quando o conjunto
de funções parar de mudar, porque os dois documentam o que existe.
