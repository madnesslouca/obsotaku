# Estudo: chat do PRISM Live Studio vs nosso multistream

**Data:** 2026-08-07  
**Fonte analisada:** instalação local  
`C:\Users\Lukam\AppData\Local\PRISMLiveStudio`  
**Comparação com:** `frontend/chat/MultiStreamChatAggregator` + `frontend/docks/UnifiedChatDock`  
**Escopo:** análise e estudo — sem alterar código do OBS.

> Este documento **não** copia assets, JS, CSS ou binários do PRISM. Serve só para entender arquitetura, feature set e o que é realista recriar com APIs públicas e nosso stack Qt/libobs.

---

## 1. Resposta direta

| Pergunta | Resposta |
| --- | --- |
| Dá para ter um chat **parecido** com o do PRISM (dock unificado, badges de plataforma, filtros, status)? | **Sim** — e já temos um embrião. |
| Dá para ficar **igual** ao que o PRISM entrega de ponta a ponta? | **Não de forma honesta no short term.** O PRISM não é “só um dock Qt”: é app web embutido (CEF) + bridge nativa + backends Naver/PRISM (MQTT) + source de overlay com temas pagos. |
| O que vale perseguir no nosso produto? | **Paridade de leitura** para Twitch / YouTube / Kick + **enviar mensagem** + emotes/badges + (depois) **source de chat na cena**. Não copiar o stack MQTT/Naver do PRISM. |

---

## 2. O que o PRISM entrega (visão de produto)

O chat no PRISM aparece em **dois produtos distintos**:

### A) Dock “Chat” (painel lateral do streamer)

- Lista unificada de mensagens de várias plataformas ao vivo.
- Enviar mensagem a partir do app.
- Apagar / moderar em algumas plataformas (ex.: Chzzk delete/ban).
- Ícone de plataforma, badges, emotes, destaque de “eu” e de manager.
- Cache local de mensagens, scroll virtualizado, retry, token refresh.
- Plataformas sem chat (Custom RTMP, Twitter/X, BAND) mostram placeholder explícito.

### B) Source “PRISM Chat” / “Chat v2” (overlay na cena)

- Source OBS dedicada (`prism-chat-source`, `prism-chatv2-source`) para **mostrar o chat no vídeo**.
- Temas, fontes embutidas, cor de nick/mensagem, fundo total ou por bolha, outline, alinhamento L/R, wrap, tamanho, transparência da janela.
- Animações (wave / shaking / random).
- Efeito de sumir mensagem.
- Temas premium ligados a assinatura Plus.
- Preview com mensagens fake até a live começar.

O dock é ferramenta de **operação**; o source é ferramenta de **apresentação**. O nosso fork só tem o A, e de forma bem mais simples.

---

## 3. Arquitetura técnica do PRISM (o que a instalação revela)

### 3.1 Camadas

```text
┌─────────────────────────────────────────────────────────────┐
│  PRISMLiveStudio.exe + libui (dock nativo “chatDock”)       │
│    injeta tokens / platformInfo / eventos de live            │
└───────────────────────────┬─────────────────────────────────┘
                            │ bridge
                            │ window.sendToPrism / sendToCpp
                            │ eventos: chat-receive, token-refresh,
                            │          platform_close, type:send|token|broadcast
┌───────────────────────────▼─────────────────────────────────┐
│  CEF (obs-browser / libbrowser) carrega SPA Vue              │
│  data/prism-studio/chat/                                     │
│    all.html      → chat unificado (AllChat)                   │
│    youtube.html  → só YouTube                                │
│    naver.html    → Naver TV                                  │
│    shopping.html → Naver Shopping Live (Socket.IO)           │
│    mqtt.html     → caminho MQTT (prismMqtt)                  │
│    ncp.html      → Naver Cloud B2B                           │
│    youtubev1.html                                            │
│  + css/js minificados (Vue + chunk-common ~339 KB)           │
└───────────────────────────┬─────────────────────────────────┘
                            │
        ┌───────────────────┼───────────────────┐
        ▼                   ▼                   ▼
  APIs diretas          MQTT (mosquitto)    backends Naver
  YouTube liveChat      broker PRISM/cloud  wss chat.naver
  Twitch badges CDN     status live/stats   SOOP/Chzzk/etc.
```

### 3.2 Evidências no disco

| Artefato | Papel |
| --- | --- |
| `data/prism-studio/chat/*.html` + `js/` + `css/` | UI do dock (Vue SPA `prism-chat`) |
| `data/prism-plugins/prism-chat-source/web/` | Mesma família de UI para o **source** na cena |
| `data/prism-plugins/prism-chatv2-source/fonts/` | 19 fontes custom para overlay v2 |
| `prism-plugins/64bit/prism-chat-source.dll` | Source browser + propriedades de tema |
| `prism-plugins/64bit/prism-chatv2-source.dll` | Source v2 (motion, alpha, wrap, custom themes) |
| `bin/64bit/mosquitto.dll` + `mqtt.html` | Cliente MQTT no pipeline de chat/status |
| `bin/64bit/Qt6WebSockets.dll` + `datachannel.dll` | WebSocket / WebRTC auxiliares |
| `locale/Chat/*.ini`, `locale/ChatTemplate/*.ini` | Copy de produto e limites de plataforma |
| Login DLLs (`twitch-login`, `youtube-login`, `facebook-login`, `chzzk-login`, …) | Contas por plataforma; tokens alimentam o chat |

### 3.3 Bridge nativa ↔ web (AllChat)

Do JS `all.f8110b83.js`:

- `window.sendToPrism(JSON.stringify({ type: "send", data: { message } }))` — enviar chat.
- `type: "token"` — pedir refresh de token à app nativa.
- `type: "broadcast"` — ex.: delete de comentário Naver.
- Handlers de eventos: `chat-receive`, `token-refresh`, `platform_close`, `handle-send`.
- Também `window.sendToCpp("menu_show", …)` para menu de moderação Chzzk.
- YouTube delete: `DELETE https://www.googleapis.com/youtube/v3/liveChat/messages` com `accessToken`.

Ou seja: **a página web não é autônoma**. Sem o host C++ (tokens, lista de canais live, permissões), o AllChat não sobe.

### 3.4 Plataformas de chat no PRISM

Ícones e templates no pacote de chat:

| Plataforma | Chat no dock | Observação |
| --- | --- | --- |
| Twitch | Sim | Badges/emotes via CDN Twitch no `chunk-common` |
| YouTube | Sim | API `liveChat/messages`; página dedicada + AllChat |
| Facebook | Sim (ícone + fluxo) | Restrições de privacidade documentadas no locale |
| AfreecaTV / SOOP | Sim | Emotes `res.sooplive.com` |
| Chzzk | Sim | WS Naver, cheese/sponsor, delete/ban |
| Naver TV | Sim | delete/report/resend |
| Naver Shopping Live | Sim | Socket.IO, notices fixos, broadcast notices |
| NCP B2B | Sim | template-ncp |
| Custom RTMP | **Não** | placeholder explícito |
| Twitter / X | **Não** | locale: ver no site |
| BAND | **Não** | locale |
| **Kick** | **Não** (no pacote de chat) | “kick” no JS = emoji / “kick out”, não a plataforma Kick |

Conclusão importante: **o PRISM não entrega Kick no chat unificado**. Nosso fork já cobre Kick; o PRISM cobre o ecossistema Naver (Chzzk, SOOP, Shopping) que nós não temos.

---

## 4. Feature matrix: PRISM vs nosso `UnifiedChatDock`

| Capacidade | PRISM | Nosso fork | Gap |
| --- | --- | --- | --- |
| Dock unificado multi-plataforma | Sim (Vue + CEF) | Sim (Qt `QTextBrowser`) | UI/richness |
| Twitch leitura | Sim (rica) | Sim (IRC, texto) | emotes/badges |
| YouTube leitura | Sim (API + token nativo) | Sim (API + SecureTokenStore) | Super Chat / members styling |
| Kick leitura | Não | Sim (Pusher WS) | **nós na frente** |
| Facebook leitura | Sim | Não | API Graph + permissões |
| Chzzk / SOOP / Naver | Sim (core KR) | Não | fora do MVP atual |
| Enviar mensagem | Sim | Não | OAuth scopes + APIs write |
| Apagar / banir | Parcial (YT delete, Chzzk, Naver) | Não | moderação |
| Emotes / badges | Sim (Twitch set grande, Chzzk, SOOP) | Só texto + cor nick | assets + parse IRC/JSON |
| Logo por plataforma na linha | Sim (SVG) | Badge HTML texto | fácil |
| Filtro por plataforma | Implícito / multi-tab | Checkboxes YT/TW/Kick | ok |
| Virtualização de lista | Sim (`vue-recycle-scroller`) | Limite 500 blocos + append | performance em chat pesado |
| Auto-scroll | Sim | Sim | ok |
| Desconecta se dock oculto | (host controla) | Sim | ok |
| Reconexão / backoff | Sim | Sim | ok |
| Refresh de token transparente | Sim (bridge) | YouTube no aggregator | expandir |
| Cache offline de mensagens | Sim | Não | opcional |
| Source de chat na cena | Sim (v1 + v2 temas) | Não | grande esforço de produto |
| Temas / fontes / motion / alpha | Sim (Plus em parte) | Não | design system |
| Monetização de temas | Plus subscription | N/A | decisão de negócio |
| Backend MQTT próprio | Sim | Não | não copiar; não precisamos para YT/TW/Kick |

---

## 5. Por que o chat do PRISM “parece” melhor

Não é magia de protocolo — é **empacotamento**:

1. **UI web madura** (Vue, virtual scroller, CSS de ~12–15 KB só no item de mensagem, hover, botões de ação).
2. **Bridge com a app de contas** — o chat já nasce com token e “estou live nestas plataformas”.
3. **Anos de integração KR** (Naver/Chzzk/SOOP/Shopping) com delete, notice fixo, cheese, etc.
4. **Overlay vendável** (Chat Source v2) — o streamer vê o mesmo estilo na tela e no vídeo.
5. **Time + backend** (ApiGateway, MQTT, Nelo logging) — falhas de live e stats no mesmo pipeline.

O nosso dock é um **MVP de leitura**: certo arquiteturalmente (aggregator + dock), mas ainda “lista de texto com badge”.

---

## 6. O que é realista recriar (sem copiar o PRISM)

### 6.1 Nível 1 — Dock “bom o suficiente” (4–8 semanas, 1 dev focado)

Meta: streamer multistream BR/US sente que **pode operar o chat** sem abrir 3 abas.

| Item | Como |
| --- | --- |
| Layout de mensagem tipo PRISM | Lista virtual (`QListView`/`QAbstractListModel`) em vez de `QTextBrowser` HTML |
| Ícone de plataforma | SVG já em `frontend/data/images/platforms/` |
| Badges Twitch (mod/sub/vip/bits) | Já lemos tags IRC em parte; renderizar ícones oficiais do Helix/CDN |
| Emotes Twitch | Parse `emotes` tag + `static-cdn.jtvnw.net` |
| Emotes Kick | Campos do payload Pusher (quando existirem) |
| Super Chat / membership YT | Campos `snippet.superChatDetails` / `membershipDetails` na poll |
| Enviar no Twitch | IRC autenticado com OAuth (`PASS oauth:<token>`) + scope `chat:edit` / `chat:read` |
| Enviar no YouTube | `liveChatMessages.insert` + scope adequado |
| Enviar no Kick | API pública de chat se/quando o app tiver scope (verificar docs Kick) |
| Uma conexão **por canal**, não por enum | Hoje o aggregator é 1× Twitch, 1× Kick, 1× YT — quebra com 2 contas |
| Filtros dinâmicos | Checkboxes a partir dos canais do `MultistreamChannelStore` |

Isso chega perto da **experiência de operação** do AllChat do PRISM para o nosso set de plataformas — **sem** CEF e **sem** MQTT deles.

### 6.2 Nível 2 — Overlay na cena (2–4 semanas adicionais)

Meta: “PRISM Chat Source” light.

Opções (da mais barata à mais rica):

1. **Browser Source** apontando para HTML **nosso** (template local + WebSocket/local server que o aggregator alimenta).  
2. **Source C++** `multistream_chat_source` que desenha texto com libobs (sem CEF) — mais estável, menos “bonito”.  
3. Híbrido: CEF só no source, dock continua nativo.

Temas: 3–5 presets + cor/fonte/transparência. Motion e 19 fontes + Plus: só se for diferencial comercial.

### 6.3 Nível 3 — “Igual PRISM” (não recomendado como meta)

Incluiria:

- SPA Vue completa no dock  
- MQTT/cloud próprio  
- Chzzk/SOOP/Naver Shopping  
- Moderação completa multi-plataforma  
- Temas premium + billing  

Isso é **produto paralelo**, não feature de MVP. E copiar assets/JS do PRISM é problema legal (GPL do fork OBS ≠ direito sobre UI/assets/código PRISM proprietário).

---

## 7. Arquitetura recomendada para o nosso fork

Manter o que já está certo e evoluir em camadas:

```text
MultistreamChannelStore / OAuth tokens
              │
              ▼
     ChatService (novo, por channelId)
       ├─ TwitchChatSession  (IRC TLS, auth opcional)
       ├─ KickChatSession    (WS Pusher, N contas)
       ├─ YouTubeChatSession (poll + insert)
       └─ (futuro) FacebookGraphSession
              │
              │ ChatEvent unificado
              ▼
     ┌────────┴────────┐
     ▼                 ▼
 UnifiedChatDock    ChatOverlaySource / local WS→Browser
 (operação)         (apresentação)
```

Regras:

- **1 sessão por `channelId`**, não por `StreamPlatform`.
- Eventos: `Message`, `System`, `Donation`, `Membership`, `Deleted`, `ConnectionState`.
- UI nunca fala HTTP/IRC direto.
- Scopes de chat **só quando o usuário usa chat** (já é política do `OAUTH_PLATFORMS.md`).
- Não reutilizar client IDs do OBS Project nem do PRISM.

### Modelo de mensagem alvo (evolução do `ChatMessage` atual)

```text
id, channelId, platform
senderId, senderName, senderColor
text, emotes[], badges[]
isModerator, isSubscriber, isVip, isOwner
kind: Normal | SuperChat | Membership | System | Notice
amount / currency (opcional)
timestamp
raw (debug, nunca logar token)
```

---

## 8. Riscos e limites honestos

| Risco | Impacto |
| --- | --- |
| Cota YouTube Data API | Polling + insert consomem units; respeitar `pollingIntervalMillis` (já fazemos) |
| Kick WS / Pusher app id | Não-oficial ou frágil; pode quebrar (PRISM evita Kick no chat) |
| Facebook chat | Permissões Graph + App Review; alto custo para pouco uso no BR gamer |
| Moderação multi-plataforma | Cada API é um produto; não prometer “ban em todos” no MVP |
| CEF no dock | Pesado (PRISM carrega libcef inteiro); preferir Qt nativo no dock |
| Copiar HTML/CSS/SVG do PRISM | **Não fazer** — licença e ética; redesenhar ou usar Simple Icons + UI própria |
| Chat no first release do MVP | `MVP.md` já marca chat agregado como **fora** do first release; alinhar roadmap |

---

## 9. Comparação visual / UX (o que o usuário sente)

| Aspecto | PRISM | Nós hoje |
| --- | --- | --- |
| Densidade da linha | Nick + badges + emote inline + logo canal | `[Twitch] hora nick: texto` |
| Ação na mensagem | Hover → delete / menu | Nenhuma |
| Campo de envio | Fixo embaixo, botão send, limites por plataforma | Inexistente |
| Estados vazios | Placeholder por tipo de canal | “Nenhuma conta” / status simples |
| Performance 1h de live pesada | Scroller reciclado | 500 blocos, recria HTML |
| Overlay | Source completo com tema | — |

---

## 10. Roadmap sugerido (se a meta for “cheio de PRISM”)

### Fase 0 — base (já existe)

- Aggregator Twitch/Kick/YouTube + dock + hide disconnect.

### Fase 1 — operação mínima (paridade funcional parcial)

1. Modelo por `channelId` + múltiplas contas.  
2. Lista virtual + ícones de plataforma.  
3. Badges/emotes Twitch.  
4. Super Chat visual YouTube.  
5. Campo **Enviar** (Twitch auth IRC + YT insert).  
6. Status por canal na toolbar (não um único label).

### Fase 2 — overlay

7. HTML/CSS **próprios** servidos localmente + Browser Source ou source C++.  
8. 3 temas + cor/fonte/alpha.  
9. Preview com mensagens dummy.

### Fase 3 — polish

10. Delete YT; timeout Twitch (Helix moderation) se tiver scope.  
11. Highlight de doação / membership.  
12. Busca / pin de mensagem no dock.  
13. (Opcional) Facebook se houver demanda real.

### Não fazer

- Portar `mqtt.html` / `mosquitto` do PRISM.  
- Embutir o web pack do PRISM no nosso binário.  
- Prometer Chzzk/SOOP sem time KR e contratos de API.

---

## 11. Esforço relativo (ordem de grandeza)

| Entrega | Esforço | Valor para o nosso público |
| --- | --- | --- |
| Dock leitura polido (ícones, badges, virtual list) | Médio | Alto |
| Enviar mensagem YT + Twitch | Médio | Alto |
| Multi-conta no aggregator | Médio | Alto (multistream real) |
| Emotes Twitch | Médio | Médio-alto |
| Source overlay 3 temas | Alto | Alto para “parecer produto” |
| Moderação completa | Alto | Médio |
| Paridade Chzzk/Naver/Shopping | Muito alto | Baixo fora da Coreia |
| MQTT cloud próprio | Muito alto | Desnecessário para YT/TW/Kick |

---

## 11b. Progresso de implementação (2026-08-07)

Fase 1 parcial aplicada no código:

| Item | Estado |
| --- | --- |
| Modelo `ChatMessage` com `channelId`, roles, Super Chat / membership | Feito |
| API `SetChannels` + status por canal | Feito |
| Twitch badges de papel (MOD/SUB/VIP/HOST) | Feito |
| Twitch IRC autenticado + `SendText` (PRIVMSG) | Feito (exige re-login com `chat:read`/`chat:edit`) |
| YouTube Super Chat / membership no poll | Feito |
| YouTube `liveChatMessages.insert` (envio) | Feito (live ativa) |
| Dock: filtros dinâmicos, status multi, campo enviar | Feito |
| Kick envio | Não (sem API pública estável) |
| Multi-conexão por `channelId` (2× Twitch) | Ainda 1 sessão por plataforma |
| Emotes gráficos / overlay na cena | Pendente |

Arquivos principais: `frontend/chat/MultiStreamChatAggregator.*`, `frontend/docks/UnifiedChatDock.*`.

---

## 12. Conclusão

O chat do PRISM é um **produto completo** (dock web + bridge + cloud/MQTT + sources temáticos + ecossistema Naver), não um widget Qt.

Nós **conseguimos** entregar a experiência que o criador multistream precisa no dia a dia:

- um painel só  
- Twitch + YouTube + Kick (onde o PRISM nem tem Kick)  
- enviar resposta  
- badges/emotes  
- opcionalmente o chat **na cena**

…desde que aceitemos **paridade seletiva**, não clone.

A arquitetura atual (`MultiStreamChatAggregator` + `UnifiedChatDock`) é o ponto de partida certo. O salto de qualidade está em:

1. tratar **canal**, não plataforma singleton;  
2. trocar HTML append por **lista virtual rica**;  
3. **write path** (send);  
4. **overlay** como segundo produto.

Tudo isso é implementável com APIs públicas e o CEF/browser source que o OBS já tem — **sem** depender do binário do PRISM e **sem** copiar os assets deles.

---

## 13. Referências internas (nosso repo)

- `frontend/chat/MultiStreamChatAggregator.*`  
- `frontend/docks/UnifiedChatDock.*`  
- `docs/product/MVP.md` (chat fora do first release)  
- `docs/product/OAUTH_PLATFORMS.md` (scopes sob demanda)  
- `docs/product/01.md` (análise geral multistream)

## 14. Referências só de leitura (instalação PRISM)

- `data/prism-studio/chat/` — SPA do dock  
- `data/prism-plugins/prism-chat-source/` e `prism-chatv2-source/` — overlay  
- `data/prism-studio/locale/Chat/en-US.ini`  
- `data/prism-studio/locale/ChatTemplate/en-US.ini`  
- `prism-plugins/64bit/prism-chat*.dll`  
- `bin/64bit/mosquitto.dll`

---

*Documento de estudo. Nenhuma alteração de código do projeto OBS foi feita nesta sessão.*
