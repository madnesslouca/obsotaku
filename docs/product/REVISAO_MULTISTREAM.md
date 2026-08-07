# Revisão de código — branch `product/multistream-mvp`

Revisão completa das mudanças pendentes (24 arquivos modificados + 26 arquivos novos) sobre a base OBS Studio 32.2.1.
Data da revisão: 2026-08-07.

> **Estado:** os achados C1–C9 e I1–I17 foram corrigidos em 2026-08-07. O que mudou em cada um está registrado na
> seção [7. Correções aplicadas](#7-correções-aplicadas); o texto original de cada achado foi mantido para
> referência histórica.

Escopo revisado:

- Fan-out RTMP: `MultiStreamManager`, `SimpleOutput`, `AdvancedOutput`, `BasicOutputHandler`
- OAuth: `PlatformOAuthClient`, `OAuthHttpClient`, `OAuthPkce`, `OAuthTokenSet`, `ConnectedAccountManager`,
  `SecureTokenStore_Windows`, `AuthListener`
- UI: `MultistreamAccountsDialog`, `MultistreamChannelBar`, `UnifiedChatDock`, `OBSBasic*`, `OBSBasic.ui`, `Yami.obt`
- Chat: `MultiStreamChatAggregator`
- Build/i18n/testes: `cmake/*`, `locale/*.ini`, `test/product-oauth`

---

## 1. Resumo executivo

A arquitetura está correta: um manager de fan-out desacoplado do OAuth, tokens fora do `.ini`, resolução de ingest
por API oficial e UI reativa por callback. O PKCE e a barra de canais estão bem construídos.

Os problemas concentram-se em três áreas:

1. **Convenções de armazenamento divergentes** entre diálogo, manager e dock de chat — três seções de config
   diferentes para o mesmo dado. Isso quebra a persistência do toggle e deixa o chat permanentemente vazio.
2. **Camada de chat imatura** — a implementação de WebSocket é inválida pelo RFC 6455 e o polling do YouTube não
   autentica. Nenhuma das três integrações de chat funciona hoje.
3. **Portabilidade e ciclo de vida** — o build quebra fora do Windows, há caminhos sem timeout/cancelamento e
   destruição de objetos concorrente com callbacks do libobs.

| Severidade | Quantidade |
| --- | --- |
| Crítico (quebra funcionalidade ou build) | 9 |
| Importante (bug, risco ou UX ruim) | 17 |
| Menor (qualidade, consistência) | 14 |

---

## 2. Críticos

### C1 — Três convenções de seção de configuração conflitantes

O mesmo dado é gravado e lido em seções diferentes:

| Componente | Seção usada | Chaves |
| --- | --- | --- |
| `MultistreamAccountsDialog` | `MultistreamAccount.<id>` | `AccountId`, `DisplayName`, `AudioTrack`, `VodTrackEnabled`, `VodTrackIndex`, `ChannelEnabled`, `ClientId` |
| `MultiStreamManager::SetChannelEnabled` | `MultistreamYouTube` / `MultistreamTwitch` / `MultistreamKick` | `ChannelEnabled` |
| `UnifiedChatDock::AutoConnectAccounts` | `MultistreamTwitch` / `MultistreamKick` / `MultistreamYouTube` | `DisplayName`, `AccountId` |

Referências: [MultiStreamManager.cpp:226](../../frontend/utility/MultiStreamManager.cpp:226),
[MultistreamAccountsDialog.cpp:72](../../frontend/dialogs/MultistreamAccountsDialog.cpp:72),
[MultistreamAccountsDialog.cpp:658](../../frontend/dialogs/MultistreamAccountsDialog.cpp:658),
[UnifiedChatDock.cpp:98](../../frontend/docks/UnifiedChatDock.cpp:98).

Consequências reais:

- O toggle da barra de canais grava em `MultistreamYouTube/ChannelEnabled`, mas o diálogo lê
  `MultistreamAccount.youtube/ChannelEnabled` → **o estado ligado/desligado nunca persiste**.
- O dock de chat lê `DisplayName` de seções onde só existe `ChannelEnabled` → **nunca encontra conta, o chat
  jamais conecta**.

Correção: centralizar em uma única função `ConfigSection(platform)` compartilhada (mover `ConfigSection` do
anônimo do diálogo para `StreamPlatform.hpp/cpp`) e usá-la nos três pontos.

---

### C2 — `audioMixIndex` / `vodTrackIndex` não correspondem às faixas de áudio reais

O vetor entregue ao manager é montado assim:

```cpp
audioEncoders.push_back(obs_output_get_audio_encoder(streamOutput, 0));  // posição 0
for (int i = 0; i < MAX_AUDIO_MIXES; i++)
        if (audioTrack[i])            // pula nulos → desloca os índices
                audioEncoders.push_back(audioTrack[i]);
```

[SimpleOutput.cpp:753](../../frontend/utility/SimpleOutput.cpp:753),
[AdvancedOutput.cpp:776](../../frontend/utility/AdvancedOutput.cpp:776).

O manager então indexa por `channel.audioMixIndex`
([MultiStreamManager.cpp:127](../../frontend/utility/MultiStreamManager.cpp:127)), mas esse índice veio dos
radio buttons "1..6" do diálogo, que representam faixas do OBS. Com faixas nulas no meio, o índice aponta para
outro encoder — **o usuário escolhe a faixa 3 e recebe a faixa 5**, ou o fallback silencioso para `[0]`.

Agrava: no VOD o default é `1` mas o `idClicked` grava o próprio `id` (radio "1" → índice 0), enquanto no áudio
principal o default é `0`. Os dois controles têm semânticas diferentes para o mesmo widget.
[MultistreamAccountsDialog.cpp:617-645](../../frontend/dialogs/MultistreamAccountsDialog.cpp:617).

Correção: passar um `std::array<obs_encoder_t *, MAX_AUDIO_MIXES>` posicional (com nulos preservados) em vez de
`std::vector` compactado, e validar `audioMixIndex < MAX_AUDIO_MIXES` no `Configure`.

---

### C3 — Fechar o diálogo cedo apaga todos os canais

`MultistreamAccountsDialog::Channels()` devolve um canal com `server`/`streamKey` **vazios** enquanto a
resolução assíncrona ainda está em voo (fallback em
[MultistreamAccountsDialog.cpp:434-447](../../frontend/dialogs/MultistreamAccountsDialog.cpp:434)), mas com
`enabled = true`.

`MultiStreamManager::Configure` rejeita exatamente esse caso
([MultiStreamManager.cpp:57](../../frontend/utility/MultiStreamManager.cpp:57)), então
`on_multistreamAccounts_triggered` mostra "Não foi possível atualizar os canais" e **descarta os canais que já
estavam configurados** ([OBSBasic_MainControls.cpp:668](../../frontend/widgets/OBSBasic_MainControls.cpp:668)).

Reprodução: abrir Ferramentas ▸ Contas de transmissão e fechar em menos de ~1s.

Correção: não incluir no retorno canais sem credencial resolvida, ou marcá-los `enabled = false`; e em caso de
erro de `Configure`, preservar a configuração anterior.

---

### C4 — Cliente WebSocket do Kick é inválido (RFC 6455)

[MultiStreamChatAggregator.cpp:259-320](../../frontend/chat/MultiStreamChatAggregator.cpp:259):

- **Frames do cliente não são mascarados.** O RFC 6455 §5.3 exige `MASK = 1` e chave de 4 bytes em todo frame
  enviado por um cliente. O Pusher fecha a conexão com status 1002. A assinatura de subscribe nunca chega.
- `Sec-WebSocket-Key` é a constante de exemplo do RFC (`dGhlIHNhbXBsZSBub25jZQ==`) — deve ser 16 bytes
  aleatórios por conexão; a resposta `Sec-WebSocket-Accept` não é validada.
- A leitura trata o buffer binário como texto e procura o primeiro `{` até o último `}`
  ([:333](../../frontend/chat/MultiStreamChatAggregator.cpp:333)): ignora cabeçalho de frame, `payload len`,
  fragmentação, múltiplos frames por `readAll()` e leituras parciais.
- Não há resposta a `ping` (opcode 0x9) nem a `pusher:ping` → a conexão cai em ~2 min mesmo se o resto funcionasse.

Correção: usar `QWebSocket` (módulo `Qt::WebSockets`), que já resolve handshake, masking, framing e ping/pong.
Reescrever à mão só se houver restrição de dependência — e, nesse caso, implementar o framing completo.

---

### C5 — Chat do YouTube não pode funcionar como está

- `ConnectPlatform(YouTube, ...)` recebe o **channel ID** (`AccountId`) e o usa como `liveChatId`
  ([UnifiedChatDock.cpp:115](../../frontend/docks/UnifiedChatDock.cpp:115)). O `liveChatId` só é obtido via
  `liveBroadcasts?part=snippet&mine=true` → `snippet.activeLiveChatId`.
- A requisição a `liveChat/messages` não envia `Authorization: Bearer` nem `key=` — resposta 401/403 em 100% dos
  casos ([:398](../../frontend/chat/MultiStreamChatAggregator.cpp:398)). O parâmetro `token` de
  `ConnectPlatform` existe na assinatura e **nunca é usado**.
- Polling fixo de 3 s ignora `pollingIntervalMillis` da resposta. `liveChatMessages.list` custa 5 unidades: 3 s
  ⇒ 6.000 unidades/hora, contra uma cota diária padrão de 10.000. **A cota estoura em menos de 2 horas.**

---

### C6 — Fluxos OAuth sem timeout nem cancelamento

`SetBusy()` desabilita todos os botões **e o botão Fechar**, e `reject()` bloqueia o fechamento da janela
enquanto `busy` for verdadeiro ([MultistreamAccountsDialog.cpp:452](../../frontend/dialogs/MultistreamAccountsDialog.cpp:452),
[:1001](../../frontend/dialogs/MultistreamAccountsDialog.cpp:1001)).

No fluxo PKCE não existe nenhum timeout: se o usuário fechar a aba do navegador sem autorizar, o `AuthListener`
espera indefinidamente e **o diálogo fica permanentemente travado** — o único escape é encerrar o processo.
O Device Code da Twitch tem expiração, mas também não tem botão de cancelar.

Correção: `QTimer` de 5 min armando `FinishConnection(..., false, timeout)`, e um botão **Cancelar** durante o
`busy` que chama `ClearLoopback()` + `twitchPollTimer.stop()`.

---

### C7 — Destruição de `Destination` concorrente com callbacks do libobs

`DisconnectSignals()` é chamado e logo em seguida o `unique_ptr<Destination>` é destruído
([MultiStreamManager.cpp:63](../../frontend/utility/MultiStreamManager.cpp:63),
[:157](../../frontend/utility/MultiStreamManager.cpp:157), destrutor em
[:22](../../frontend/utility/MultiStreamManager.cpp:22)). `signal_handler_disconnect` não espera um callback
que já esteja em execução em outra thread — `OnOutputStop` pode estar dentro de
`destination->owner->UpdateState(*destination, ...)` com o objeto sendo liberado ⇒ use-after-free.

Também: em `Start()`, se `obs_service_create`/`obs_output_create` falhar no meio do laço, a função retorna sem
desconectar os sinais já registrados nos itens anteriores de `prepared`
([:100-145](../../frontend/utility/MultiStreamManager.cpp:100)).

Correção: manter os `Destination` em `shared_ptr` e passar um `weak_ptr` como userdata do sinal, ou parar o
output (`obs_output_stop` + espera do sinal `stop`) antes de liberar. No mínimo, adicionar um `scope guard` que
desconecte tudo em `prepared` no caminho de erro.

---

### C8 — Falha do multistream é invisível para o usuário

Se `multiStreamManager->Start()` retornar `false` depois que o output principal já subiu, o código só emite
`blog(LOG_WARNING, ...)` ([SimpleOutput.cpp:762](../../frontend/utility/SimpleOutput.cpp:762),
[AdvancedOutput.cpp:785](../../frontend/utility/AdvancedOutput.cpp:785)).

O usuário vê "AO VIVO" na interface principal, os cartões da barra continuam em OFFLINE e **nada explica que os
destinos extras não subiram**. O MVP.md exige "um estado de falha parcial claro" — esse é justamente o caso.

Correção: propagar o erro para a `MultistreamChannelBar` (marcar todos os canais habilitados como `Failed` com
`lastError`) e, se todos falharem, exibir um aviso não modal.

---

### C9 — O build quebra fora do Windows

`ui-oauth.cmake` adiciona `oauth/OAuthTokenSet.cpp` e `ui-dialogs.cmake` adiciona
`dialogs/MultistreamAccountsDialog.cpp` **incondicionalmente**, mas os dois usam `SecureTokenStore`, cuja única
implementação (`utility/SecureTokenStore_Windows.cpp`) é adicionada apenas em `os-windows.cmake`.

Em Linux/macOS isso resulta em erro de link (`undefined reference to SecureTokenStore::Save/Load/Remove`).

Correção: adicionar stubs `SecureTokenStore_Posix.cpp` (libsecret / Keychain, ou stub que retorna erro), ou
condicionar todos os fontes novos a `if(OS_WINDOWS)` de forma consistente.

---

## 3. Importantes

### I1 — `Stop()` não zera `active` quando nenhum output estava ativo

[MultiStreamManager.cpp:191](../../frontend/utility/MultiStreamManager.cpp:191) só percorre destinos com
`obs_output_active()` verdadeiro. Se `active == true` mas nenhum output subiu de fato (falha logo após
`obs_output_start` retornar `true`), `active` só é recalculado dentro de `UpdateState`, que depende do sinal.
Ficando preso em `true`, **todo `Configure()` posterior falha** com "cannot be changed while an output is
active" e o usuário não consegue mais editar canais sem reiniciar.

Correção: definir `active = false` explicitamente ao final de `Stop()` quando nenhum destino restar em
`Starting/Live/Stopping`.

### I2 — `BasicOutputHandler::Active()` pode travar o OBS inteiro

[BasicOutputHandler.hpp:98](../../frontend/utility/BasicOutputHandler.hpp:98) passou a incluir
`multiStreamManager->IsActive()`. Um destino preso em `Starting` faz o OBS considerar que há saída ativa,
bloqueando mudança de configurações de saída e o encerramento limpo. Combine com I1 e o efeito é permanente.

### I3 — A barra de canais reseta o estado visual para OFFLINE

`SetChannels()` sempre cria o rótulo com `StateText(Idle)`
([MultistreamChannelBar.cpp:124](../../frontend/widgets/MultistreamChannelBar.cpp:124)) e é chamada por
`BindMultistreamManager()`, que roda em `ResetOutputs()` e ao fechar o diálogo
([OBSBasic_OutputHandler.cpp:70](../../frontend/widgets/OBSBasic_OutputHandler.cpp:70)). Durante uma live, abrir
e fechar o diálogo faz todos os canais aparecerem como OFFLINE mesmo transmitindo.

Correção: após `SetChannels`, aplicar `manager->Snapshot()` em loop.

### I4 — `emptyState` vira ponteiro pendente

`SetChannels()` faz `deleteLater()` em todos os widgets do layout, mas o membro `emptyState` continua apontando
para o `QLabel` agendado para deleção ([MultistreamChannelBar.cpp:93](../../frontend/widgets/MultistreamChannelBar.cpp:93)).
Hoje só sobrevive porque todo caminho que o usa recria antes. Use `QPointer<QLabel>` ou `emptyState = nullptr`.

### I5 — Condição sem efeito em `UpdateState`

```cpp
if (snapshot.state == Idle && !item->enabled->isChecked()) {
        QSignalBlocker blocker(item->enabled);
        item->enabled->setChecked(false);   // já está false
}
```
[MultistreamChannelBar.cpp:174](../../frontend/widgets/MultistreamChannelBar.cpp:174). A intenção provável era
desmarcar o checkbox quando o canal cai (`Failed`) — a condição está invertida.

### I6 — Injeção de HTML no chat via `userColor`

[UnifiedChatDock.cpp:142](../../frontend/docks/UnifiedChatDock.cpp:142) interpola `msg.userColor` dentro de um
atributo `style="color:%3"` sem escape. `senderName` e `messageText` são escapados, `userColor` não. O valor vem
do JSON do Kick (`sender.identity.color`) e das tags IRC — conteúdo controlado por terceiros. Uma aspa fecha o
atributo e permite injetar markup; o Qt rich text não executa JS, mas `<img src="http://...">` é carregado,
**vazando o IP do streamer para quem enviar a mensagem**.

Correção: validar com `QColor::isValidColorName()` / regex `^#[0-9a-fA-F]{6}$` e cair no padrão se não bater.

### I7 — Chat cresce sem limite de memória

`chatView->append()` sem limite de blocos ([UnifiedChatDock.cpp:147](../../frontend/docks/UnifiedChatDock.cpp:147)).
Uma live de 6 h em canal movimentado acumula centenas de milhares de blocos HTML. Use `QPlainTextEdit` com
`setMaximumBlockCount(500)`, ou remova blocos antigos do `QTextDocument` em FIFO.

### I8 — Chat continua conectado com o dock oculto

Não há reação a `hideEvent`/`visibilityChanged`: IRC, WebSocket e polling do YouTube seguem ativos mesmo com o
dock fechado. Some com o custo de cota do C5.

### I9 — Stylesheets inline anulam o tema

O diálogo aplica ~140 linhas de `setStyleSheet` no próprio widget
([MultistreamAccountsDialog.cpp:208](../../frontend/dialogs/MultistreamAccountsDialog.cpp:208)), o que tem
precedência sobre a folha da aplicação. As 132 linhas adicionadas ao `Yami.obt` para esse mesmo diálogo
**nunca são aplicadas**. Além disso as cores fixas (`#0d0f12`, `#f8fafc`) ignoram temas claros.

Correção: remover o `setStyleSheet` local e manter só o `Yami.obt` com `var(--...)`; replicar nos demais temas.

### I10 — Strings fora do sistema de tradução

Hardcoded: `"Configurar ID"`, `"Configurações de transmissão"`, `"Faixa de áudio"`, `"Faixa de VOD da Twitch"`
(diálogo); `"Chat Unificado Multistream"`, `"Limpar"`, `"Rolagem"`, `"Status do Chat: Pronto"` (dock); todos os
`statusChanged` do aggregator; `"configuração OAuth incompleta."` em
[OBSBasic_OutputHandler.cpp:112](../../frontend/widgets/OBSBasic_OutputHandler.cpp:112).

Simetricamente, há chaves órfãs no `.ini` (definidas e nunca usadas): `Multistream.Accounts.Configure`,
`MissingClientId`, `KickSecretTitle`, `KickSecretPrompt`, `KickSecretEmpty`, `KickSecretSaved`,
`KickSecretSaveFailed`.

### I11 — `ConfigureKickClientSecret()` não configura secret nenhum

Apesar do nome, do botão e da documentação, a função só pede o **Client ID**
([MultistreamAccountsDialog.cpp:745](../../frontend/dialogs/MultistreamAccountsDialog.cpp:745)). O caminho que
gravava o secret no Credential Manager sumiu (as chaves de tradução ficaram). Pior: se `clientId` já existe, a
função não faz nada — **não há como corrigir um Client ID digitado errado** pela interface.

### I12 — `CardFor()` desreferencia `end()`

[MultistreamAccountsDialog.cpp:461](../../frontend/dialogs/MultistreamAccountsDialog.cpp:461) faz `return *card;`
sem verificar o iterador. Chamar com `StreamPlatform::CustomRtmp` (hoje não acontece, mas nada impede) é UB.

### I13 — Resolução de ingest a cada boot

`RestoreMultistreamAccounts()` dispara refresh de token + resolução de chave de stream para as três plataformas
em todo início de aplicação ([OBSBasic_OutputHandler.cpp:83](../../frontend/widgets/OBSBasic_OutputHandler.cpp:83)),
mesmo que o usuário nunca vá transmitir. Cada tarefa usa curl bloqueante com `CURLOPT_TIMEOUT 30` no
`QThreadPool::globalInstance()`, que é o mesmo pool usado pelo resto do OBS.

Correção: resolver sob demanda (ao clicar "Iniciar transmissão") ou com backoff/cache; usar um `QThreadPool`
dedicado com `maxThreadCount` próprio.

### I14 — `server->close()` no ramo de falha do `AuthListener`

[AuthListener.cpp:95-105](../../frontend/oauth/AuthListener.cpp:95): o listener passou a fechar o socket servidor
também quando não há `code`. Uma requisição espúria do navegador (`/favicon.ico`, prefetch, extensão) chega
primeiro, cai no ramo de falha e **aborta a autorização em andamento**.

Correção: ignorar requisições cujo path não seja o callback ou que não tragam `code`/`error`, e só fechar no
desfecho real.

### I15 — Validação de HTTPS por prefixo

```cpp
if (url.rfind("http://", 0) == 0 && url.find("http://127.0.0.1") != 0 && url.find("http://localhost") != 0)
```
[OAuthHttpClient.cpp:41](../../frontend/oauth/OAuthHttpClient.cpp:41). Aceita `http://127.0.0.1.evil.com/` e
`http://localhost.attacker.net/` — a comparação é de prefixo, não de host. Como os endpoints hoje são
constantes, o risco é baixo, mas a checagem existe justamente para o caso em que deixarem de ser
(`tokenExchangeEndpoint` vem de env/CMake).

Correção: parsear com `QUrl` e comparar `url.host()` com `127.0.0.1`/`::1`/`localhost`.

### I16 — Sem tratamento de 429 / 5xx / retry

`PlatformOAuthClient` trata qualquer status fora de 2xx como erro final. As três plataformas devolvem 429 com
`Retry-After` sob carga, e a Twitch usa 503 transitório. Um refresh no boot que pegue um 429 marca a conta como
falha permanente até o próximo restart.

### I17 — Testes fora do CI e com efeito colateral

- `add_subdirectory(test/product-oauth)` está dentro de `frontend/CMakeLists.txt`
  ([CMakeLists.txt:52](../../frontend/CMakeLists.txt:52)) — pertence ao `CMakeLists.txt` raiz, sob `ENABLE_TESTS`.
- O alvo é `EXCLUDE_FROM_ALL` e não há `add_test()`/`enable_testing()` → **nunca roda em CI**.
- O teste escreve e apaga uma credencial real no Credential Manager da máquina
  ([OAuthPrimitivesTest.cpp:88](../../test/product-oauth/OAuthPrimitivesTest.cpp:88)). Se abortar no meio, deixa
  lixo em `OBS-Multistream/OAuth/youtube/product-oauth-self-test`.
- Usa `CMAKE_SOURCE_DIR` em vez de `CMAKE_CURRENT_SOURCE_DIR`/targets — quebra se o projeto virar subdiretório.

---

## 4. Menores e melhorias

1. **Chaves de stream em `std::string` comum.** `MultiStreamChannel::streamKey` é copiado para o diálogo, para a
   barra e para o vetor `channels` do manager, sem zerar na destruição — contraria a regra do `MVP.md` ("stream
   keys are only held in memory by this component"). Considere um tipo `SecretString` com limpeza no destrutor.
2. `OAuthPkce::RandomBytes` gera byte a byte com `bounded(256)`
   ([OAuthPkce.cpp:21](../../frontend/oauth/OAuthPkce.cpp:21)); `QRandomGenerator::system()->fillRange()` é mais
   direto e mais rápido.
3. `OAuthTokenSet::ClearSecret` não alcança cópias feitas por `json11::Json` durante `Save`/`Load` — a limpeza dá
   uma falsa sensação de completude.
4. O aggregator usa `https://kick.com/api/v1/channels/...` (API não documentada) com User-Agent de navegador
   falsificado ([MultiStreamChatAggregator.cpp:64](../../frontend/chat/MultiStreamChatAggregator.cpp:64)),
   enquanto o resto do código usa a API pública oficial `api.kick.com/public/v1`. Sujeito a bloqueio por
   Cloudflare e inconsistente com a política do projeto.
5. Twitch IRC anônimo: `PASS oauth:justinfan12345` não é um token válido
   ([:162](../../frontend/chat/MultiStreamChatAggregator.cpp:162)). O padrão é `PASS SCHMOOPIIE` ou omitir o PASS.
6. O `PONG` deveria ecoar o payload do `PING` recebido, não uma constante
   ([:179](../../frontend/chat/MultiStreamChatAggregator.cpp:179)).
7. Sem reconexão automática em nenhuma das integrações de chat.
8. `QTimer::singleShot(1500, ...)` para auto-conectar o chat
   ([UnifiedChatDock.cpp:93](../../frontend/docks/UnifiedChatDock.cpp:93)) é sincronização por chute; conecte ao
   sinal de conclusão de `RestoreMultistreamAccounts`.
9. `SetBusy()` não desabilita o `configureButton` do Kick — dá para abrir o `QInputDialog` no meio de um fluxo.
10. Custom RTMP está no catálogo `StreamPlatform` e é requisito do MVP, mas **não há UI para adicioná-lo** — o
    diálogo só cria cartões para as três plataformas OAuth.
11. `ui-utility.cmake`: `StreamPlatform.*` inserido fora da ordem alfabética (entre `SceneRenameDelegate` e
    `ScreenshotObj`).
12. `ui-docks.cmake` compila `chat/MultiStreamChatAggregator.*` — merece um `ui-chat.cmake` próprio.
13. Ruído no diff: reindentação do bloco `menuTools` em `OBSBasic.ui` e remoção de uma linha em branco em
    `OBSBasic.hpp:601` sem relação com a mudança.
14. **Documentação desatualizada**: `MVP.md:49` afirma que "the first multistream manager is present but is not
    connected to user-interface actions yet" (já está conectado); `OAUTH_PLATFORMS.md:54` descreve o fluxo de
    Client Secret do Kick pelo diálogo, que não existe mais no código (ver I11).

---

## 5. Pontos positivos

- PKCE correto e validado contra o vetor de teste do RFC 7636.
- Migração do `AuthListener` de regex para `QUrlQuery` com comparação de `state` decodificado — mais robusta e
  menos suscetível a falsos positivos.
- Remoção do log do `code` de autorização em `YoutubeAuth.cpp` e do dump do redirect no `AuthListener`.
- Separação limpa entre camada OAuth e camada de saída: o `MultiStreamManager` nunca vê um token.
- Uso de `QPointer` + `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` em todos os retornos de thread para
  a UI — o padrão está consistente.
- `SecureTokenStore_Windows` valida tamanho de target e blob, e trata `ERROR_NOT_FOUND` corretamente.
- Barra de canais com toggle por destino durante a live é a peça de UX mais valiosa do conjunto.

---

## 6. Ordem sugerida de correção

**Antes de qualquer teste com usuário real:**

1. C1 (seções de config) — desbloqueia persistência e chat de uma vez.
2. C3 + I1 + I2 (ciclo de vida do `Configure`/`active`) — evita estados travados irrecuperáveis.
3. C6 (timeout/cancelar no OAuth) — evita o travamento do diálogo.
4. C9 (build fora do Windows) se multiplataforma ainda for meta; senão, condicionar tudo explicitamente.

**Antes de anunciar o chat:**

5. C4 + C5 — hoje nenhuma das três integrações entrega mensagem; a mais barata é trocar por `QWebSocket` e
   implementar `liveBroadcasts` → `activeLiveChatId` com o token já armazenado.
6. I6 + I7 (injeção de HTML e memória).

**Antes de anunciar o multistream:**

7. C2 (mapeamento de faixas de áudio) — hoje entrega a faixa errada em silêncio.
8. C8 (falha parcial visível) — é requisito explícito do MVP.
9. C7 (use-after-free) — raro, mas é crash em produção durante a live.

**Higiene:**

10. I9 + I10 (tema e i18n), I17 (testes no CI), item 14 (docs).

---

## 7. Correções aplicadas

Aplicadas em 2026-08-07. O frontend compila (MSVC 19.44, `RelWithDebInfo`, x64) e
`product-oauth-tests` passa.

### Críticos

| # | Correção |
| --- | --- |
| C1 | `StreamPlatformConfigSection()` em [StreamPlatform.cpp](../../frontend/utility/StreamPlatform.cpp) passa a ser a única origem do nome da seção. Diálogo, `MultiStreamManager::SetChannelEnabled`, `UnifiedChatDock` e `RestoreMultistreamAccounts` usam essa função. Um teste cobre os três nomes. |
| C2 | `MultiStreamManager::Start` recebe `audioEncodersByTrack` indexado por faixa (com nulos preservados) mais um `defaultAudioEncoder` explícito. `SimpleOutput`/`AdvancedOutput` montam o vetor com `MAX_AUDIO_MIXES` posições. `Configure` rejeita índices fora da faixa. |
| C3 | O cartão passou a ter `credentialsResolved`. `Channels()` entrega canais não resolvidos desabilitados e sem credencial, e `on_multistreamAccounts_triggered` restaura a configuração anterior se o `Configure` falhar. |
| C4 | Cliente WebSocket reescrito: `Sec-WebSocket-Key` aleatório por conexão, validação do `Sec-WebSocket-Accept`, frames de cliente mascarados conforme o RFC 6455, parser incremental com fragmentação, `ping`/`pong` no nível do protocolo e do Pusher. |
| C5 | O chat do YouTube resolve `activeLiveChatId` via `liveBroadcasts`, autentica com Bearer (com refresh pelo `SecureTokenStore`), descarta a primeira página de backlog e respeita `pollingIntervalMillis` com piso de 5 s. Sem transmissão ativa, verifica a cada 30 s em vez de pollar o chat. |
| C6 | Botão **Cancelar conexão** durante o `busy` e timeout de 5 minutos para PKCE e Device Code. |
| C7 | `Destination` virou `shared_ptr` com lista de aposentados: os objetos só são liberados quando o output correspondente não está mais ativo. Falha no meio do `Start` desconecta os sinais já registrados. |
| C8 | `MultiStreamManager::ReportFailure` emite snapshots `Failed` pelo callback de estado, então a barra mostra a falha parcial em vez de só logar. |
| C9 | `SecureTokenStore_Stub.cpp` para plataformas sem backend de credenciais, adicionado por `ui-utility.cmake` quando `NOT OS_WINDOWS`. |

### Importantes

| # | Correção |
| --- | --- |
| I1 | `Stop()` recalcula `active` ao final, então um output que nunca subiu não trava o manager. |
| I2 | Consequência de I1: `Active()` volta a refletir a realidade. |
| I3 | `MultistreamChannelBar::ApplySnapshots()` repinta o estado atual depois de reconstruir os cartões. |
| I4 | `emptyState` virou `QPointer` e é criado por `ShowPlaceholder()`. |
| I5 | A condição passou a desmarcar o toggle quando o destino entra em `Failed`. |
| I6 | `SafeUserColor()` aceita apenas `#RRGGBB` e cai na cor da plataforma; `openExternalLinks`/`openLinks` desligados. |
| I7 | Documento limitado a 500 blocos. |
| I8 | `showEvent`/`hideEvent` conectam e desconectam o chat; o `QTimer::singleShot(1500)` saiu. |
| I9 | Stylesheets inline removidos do diálogo e da barra; o estilo vive em `Yami.obt` com `var(--...)`, incluindo os novos seletores de ícone, painel de faixas, barra de canais e dock de chat. |
| I10 | Todas as strings passaram para `en-US.ini`/`pt-BR.ini`, incluindo as do chat. As chaves `KickSecret*` voltaram a ser usadas por I11. |
| I11 | `ConfigureKickRegistration()` edita o Client ID já existente e, quando não há proxy configurado, pede o Client Secret e o grava no Credential Manager. |
| I12 | `CardFor()` retorna ponteiro e todos os chamadores verificam nulo. |
| I13 | `MultistreamTaskPool()` isola o trabalho OAuth do pool global (3 threads). |
| I14 | O `AuthListener` ignora requisições sem `code`/`state`/`error` e só encerra no desfecho real. |
| I15 | A checagem de HTTPS compara o host extraído, não o prefixo da URL. |
| I16 | `Get`/`PostForm` repetem 429 e 5xx até 3 vezes com backoff linear. |
| I17 | Os testes saíram de `frontend/CMakeLists.txt` para a raiz sob `ENABLE_PRODUCT_TESTS`, com `add_test()`, caminhos relativos e conta de credencial única por execução. |

### Menores

Aplicados: 2 (`fillRange`), 4 (API v2 oficial do Kick e User-Agent próprio), 5 (`PASS SCHMOOPIIE`), 6 (PONG ecoando o token), 7 (reconexão com backoff exponencial até 60 s), 8, 9, 11, 12 (`ui-chat.cmake`), 13, 14.

Item 1 (chaves de stream em `std::string`) foi parcialmente endereçado: `MultiStreamManager` zera as chaves ao
reconfigurar e ao destruir. Um tipo `SecretString` completo continua pendente.

Item 3 (cópias residuais em `json11`) e item 10 (UI de Custom RTMP) seguem em aberto.

### Achados da execução real (2026-08-07)

O aplicativo foi executado em modo portátil e as telas novas foram abertas uma a uma. O que a execução revelou,
e que nenhuma compilação teria mostrado:

| Achado | Correção |
| --- | --- |
| A última linha do grid de plataformas ficava cortada. `QPushButton` com um `QLayout` interno não considera os filhos no `sizeHint`, então o grid dimensionava pelo `minimumSize` e o diálogo nascia baixo demais. | Os tiles passaram a ser `QToolButton` em `ToolButtonTextUnderIcon`, com o emblema desenhado como `QIcon` (`StreamPlatformBadge`). O layout do diálogo usa `QLayout::SetFixedSize`. |
| Título "Adicionar canal" repetido: uma vez na barra de título da janela e outra dentro do diálogo. | O texto interno foi removido; ficou só o subtítulo. |
| "Custom RTMP" aparecia em inglês no meio de uma interface em português. | `StreamPlatformDisplayName()` traduz apenas a entrada genérica; nomes de marca continuam como são. |
| "Destino da Custom RTMP" e "A Custom RTMP não oferece API…" — concordância quebrada, e a explicação sobre API não faz sentido para um servidor RTMP arbitrário. | Textos neutros (`Destino: %1`), com uma introdução própria para o RTMP personalizado. |
| A cor de marca do X (`#e7e9ea`) é quase branca e sumiria em tema claro. | Trocada pelo cinza neutro `#71767b`, legível nos dois temas. |
| A barra vazia dizia a mesma coisa duas vezes: "nenhum canal ainda" no subtítulo e "Conecte um canal…" no lugar dos cartões. | O subtítulo fica vazio quando não há canais. |

### Chat da Kick não aparecia (2026-08-07)

Três defeitos empilhados, cada um suficiente para deixar o chat mudo. Nenhum aparecia no log, porque o
agregador não registrava nada — a instrumentação foi a primeira coisa a entrar.

| Defeito | Correção |
| --- | --- |
| O endereço do chat vinha de `displayName`, que é editável e ganha o nome da plataforma como reserva. Um canal rotulado "Kick" fazia o chat se conectar ao canal `kick.com/kick` — o canal oficial da própria Kick, que existe e responde. | `MultiStreamChannel::chatAddress` guarda o handle que a plataforma informa (slug da Kick, login da Twitch), separado do rótulo. `MultistreamChannelStore::UpdateIdentity()` grava o que a resolução de credenciais descobriu, e o dock lê dali. |
| `https://kick.com/api/v2/channels/<slug>/chatroom` fica atrás do Cloudflare, que devolve 403 para quem não parece navegador. O `User-Agent` era `OBS-Multistream/0.1`. | Cabeçalhos completos de navegador (UA do Chrome, `Accept`, `Accept-Language`, `Referer`) e HTTP/2 desligado. |
| A app key do Pusher estava desatualizada. O servidor aceitava o WebSocket e logo respondia `pusher:error` 4001 — "App key … not in this cluster" — e fechava. | Key atualizada para a que o site usa hoje. Ela não é segredo (está no JavaScript da página), mas muda: quando o chat parar, releia numa requisição WebSocket de `kick.com`. |

### Como validar

```bash
cmake --preset product-windows-x64 -DENABLE_PRODUCT_TESTS=ON
```

```bash
cmake --build --preset product-windows-x64 --target product-oauth-tests
```

```bash
ctest --test-dir build_product_x64 -C RelWithDebInfo -R product-oauth
```
