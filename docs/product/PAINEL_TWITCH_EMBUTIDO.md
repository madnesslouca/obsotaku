# Painel da Twitch embutido, com várias contas

**Status:** investigado, não implementado. Registrado em 2026-08-08 para decidir depois.

## A pergunta

Dá para abrir `https://dashboard.twitch.tv/u/<canal>/stream-manager` dentro do OBS
sem obrigar o usuário a fazer login no Chromium embutido, aproveitando o token
OAuth que o multistream já guarda?

## Resposta curta

Não pelo token. E, do jeito que o OBS está montado hoje, o painel só serviria a
uma conta Twitch — o que inviabiliza o produto, que existe para tocar várias.
Mas o `obs-browser` tem a peça que resolve isso, e ninguém usa.

## Por que o token não serve

`dashboard.twitch.tv` é a aplicação web da Twitch e autentica por **cookie de
sessão do navegador**. O access token que guardamos é da API Helix — outro
sistema. Não existe forma suportada de trocar um pelo outro, nem de injetar o
token e a página se considerar logada. Qualquer coisa que embuta aquela URL
precisa de uma sessão web de verdade.

Isso vale para qualquer plataforma: o mesmo raciocínio se aplica ao YouTube
Studio e ao painel da Kick.

## O que já existe

O OBS de origem já traz painéis da Twitch quando a conta é conectada pelo
caminho clássico (Configurações → Transmissão → Conectar conta), em
`frontend/oauth/TwitchAuth.cpp`:

| Painel | URL |
| --- | --- |
| Informações da live | `dashboard.twitch.tv/popout/u/<login>/stream-manager/edit-stream-info` |
| Feed de atividades | `dashboard.twitch.tv/popout/u/<login>/stream-manager/activity-feed` |
| Estatísticas | `twitch.tv/popout/<login>/…` |
| Chat | `twitch.tv/popout/<login>/chat` |

Todos usam `panel_cookies` — o jar único do CEF. E o usuário pode montar
qualquer um deles à mão hoje, sem código: **Painéis → Painéis personalizáveis
com URL...**, colando a URL do stream-manager. O CEF pede login uma vez e a
sessão persiste.

## Por que o nosso fork perdeu isso

O OAuth nativo do OBS roda **dentro do CEF** (`OAuthLogin` em
`frontend/dialogs/OAuthLogin.cpp:34`, com `panel_cookies`). Autorizar já deixava
os cookies de sessão, então os painéis funcionavam com um login só.

O OAuth do multistream que construímos usa o **navegador externo** com callback
em `127.0.0.1` (`AuthListener` + `PlatformOAuthClient`). Foi uma escolha
deliberada: é o que permite conectar várias contas da mesma plataforma, já que o
jar único do CEF faria a segunda autorização herdar a sessão da primeira. O
efeito colateral é que o Chromium embutido nunca vê a sessão da Twitch.

## A descoberta

`obs-browser` expõe criação de jars independentes, e o OBS só cria **um**:

```cpp
// plugins/obs-browser/panel/browser-panel.hpp:65
virtual QCefCookieManager *create_cookie_manager(const std::string &storage_path,
                                                 bool persist_session_cookies = false) = 0;
```

Cada chamada monta um `CefRequestContext` próprio com `cache_path` separado
(`plugins/obs-browser/panel/browser-panel.cpp:103`). É o mesmo mecanismo dos
perfis do Chrome. O único jar existente nasce em
`frontend/widgets/OBSBasic_Browser.cpp:190` como `obs_profile_cookies/<CookieId>`.

Ou seja: **um perfil de cookies por conta é possível hoje, sem tocar no plugin.**

## Desenho proposto

- Um jar por conta conectada: `obs_multistream_cookies/<platform>/<accountId>`.
- O painel de cada canal abre com o jar da sua própria conta, então duas contas
  Twitch viram dois painéis independentes.
- Criação sob demanda: uma entrada "Abrir painel" no menu `⋮` do card do canal,
  em vez de abrir tudo no boot.
- De quebra, isso destravaria o OAuth pelo CEF por conta, se algum dia
  quisermos voltar a esse caminho — a segunda conta não herdaria a sessão da
  primeira, que era o impedimento original.

## Custos a pesar antes

- **Um login por conta, inevitável.** Cada jar nasce vazio, então cada canal
  exige um login na Twitch dentro do CEF. O token OAuth não ajuda.
- **Peso.** Cada contexto é cache em disco e processos próprios do Chromium.
  Três ou quatro painéis abertos equivalem a três ou quatro janelas de Chrome
  rodando junto com a codificação. Numa máquina que já está no limite, pesa.
- **Fragilidade.** São páginas web da Twitch embutidas: mudam sem aviso, e o
  layout do popout não é contrato público.

## Alternativa sem navegador

Reconstruir as partes úteis nativamente com Helix + EventSub, usando o token que
já temos: espectadores, seguidores e inscritos recentes, alertas, saúde do
stream. Não pede login nenhum e fica coerente com o resto da interface, mas é
reimplementar — e nunca vai cobrir tudo que o dashboard cobre. O painel de
informações da live (`frontend/docks/StreamInfoDock.cpp`) já é um pedaço disso.
