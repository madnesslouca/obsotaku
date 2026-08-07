# OAuth platform integration

YouTube, Twitch, and Kick are the supported connected-account platforms for the first release. Custom RTMP remains a
fallback, not the primary setup experience.

## Provider matrix

| Platform | Desktop OAuth flow | Minimum initial scopes | Ingest credentials |
| --- | --- | --- | --- |
| YouTube | Authorization Code + PKCE, loopback callback | `https://www.googleapis.com/auth/youtube` | `liveStreams` returns the ingestion address and stream name |
| Twitch | Public Device Code flow | `channel:read:stream_key`, `channel:manage:broadcast` | Helix `streams/key` returns the key; the ingest API returns the server list |
| Kick | Authorization Code + PKCE | `channel:read`, `streamkey:read`, `channel:write`, `user:read` | `public/v1/channels` returns `stream.url` and `stream.key` |

The app must request additional chat or moderation scopes only when the corresponding feature exists. They are not
part of initial sign-in.

## Security rules

- Register separate application client IDs for this product. Never reuse OBS Project credentials or its OAuth relay.
- Generate a new PKCE verifier, challenge, and anti-CSRF state for every YouTube or Kick authorization attempt.
- Validate Twitch tokens with its documented validation endpoint before API use.
- Store refresh tokens, rotating Twitch device-flow refresh tokens, and any cached stream keys in Windows Credential
  Manager through `SecureTokenStore`.
- Keep only non-secret account identifiers and display names in the normal application configuration.
- Never include authorization codes, tokens, stream keys, or credential API responses in logs.
- Remove the platform credential when the user disconnects an account.

## Provider responsibilities

Each OAuth provider will expose the same product-facing operations:

1. Connect or reconnect an account.
2. Refresh and validate its user token.
3. Return the account id and display name.
4. Resolve a server and stream key immediately before going live.
5. Update supported title/category/privacy metadata.
6. Revoke authorization and remove local credentials.

The resolved server and key are passed to `MultiStreamManager`; the output layer does not handle OAuth tokens.

## Developer registrations required

- A Google Cloud project with YouTube Data API v3 enabled and a Desktop OAuth client.
- A Twitch developer application configured as a public client for Device Code authorization.
- A Kick developer application with an approved loopback redirect and the four initial scopes above.

Client IDs are build/deployment configuration. Client secrets must not be committed or embedded in a desktop binary.

Kick currently requires a client secret at its token endpoint. The desktop therefore sends the provider-compatible
token form plus `platform=kick` to the configured HTTPS token-exchange service. That service adds the client secret,
forwards the request to Kick, and returns only the provider token JSON. The service must never log authorization
codes, refresh tokens, access tokens, or response bodies containing credentials.

For personal development builds only, the accounts dialog also accepts the developer's own Kick Client Secret and
stores it in Windows Credential Manager. The direct token request still uses PKCE. Distributed production builds
must use the HTTPS token-exchange service because a desktop application cannot protect a shared product secret.

## Current implementation status

- `StreamPlatform` contains the provider endpoints, flows, scopes, and ingest capability flags.
- `MultiStreamChannel` identifies its platform and connected account.
- `SecureTokenStore` persists provider secrets in Windows Credential Manager.
- `PlatformOAuthClient` implements PKCE sessions, Twitch Device Code polling, token exchange/refresh, token
  validation, identity lookup, and ingest resolution for all three providers.
- `ConnectedAccountManager` stores a successful account under its stable provider id and converts it into a
  `MultiStreamChannel` without exposing OAuth tokens to the output layer.
- `MultistreamAccountsDialog`, available from **Tools > Streaming Accounts**, provides connected-account cards,
  opens the external browser, handles YouTube/Kick loopback callbacks, polls Twitch Device Code authorization off the
  UI thread, persists only account metadata, and removes credentials on disconnect. Every pending authorization can
  be cancelled and times out after five minutes, so a closed browser tab never leaves the dialog stuck.
- Account metadata is stored in `MultistreamAccount.<platform-id>` only. `StreamPlatformConfigSection()` is the
  single source of that name.
- OAuth network calls run on a dedicated thread pool, retry 429 and transient 5xx responses with backoff, and
  reject plain HTTP for anything but a real loopback host.
- Deterministic tests cover the RFC 7636 PKCE vector, authorization requests, provider configuration, the mandatory
  Kick proxy, and a disposable Windows Credential Manager round trip.
- Remaining external work is registering the three applications and deploying the Kick token-exchange service.

## Development configuration

The current development build reads public application configuration from the process environment:

```text
OBS_MULTISTREAM_YOUTUBE_CLIENT_ID
OBS_MULTISTREAM_TWITCH_CLIENT_ID
OBS_MULTISTREAM_KICK_CLIENT_ID
OBS_MULTISTREAM_KICK_TOKEN_PROXY
OBS_MULTISTREAM_KICK_CALLBACK_PORT   (optional, defaults to 49327)
```

Register `http://127.0.0.1:49327/` as the Kick callback when using the default fixed port. YouTube uses a dynamic
loopback port. These values contain no user token or stream key; production packaging can replace the environment
source with signed deployment configuration.

Production-style builds should set the equivalent public CMake cache values instead:

```text
PRODUCT_YOUTUBE_CLIENT_ID
PRODUCT_TWITCH_CLIENT_ID
PRODUCT_KICK_CLIENT_ID
PRODUCT_KICK_TOKEN_PROXY
```

They are compiled into the application; environment variables remain development overrides. When a provider is not
registered, its account card is disabled with a product-facing status instead of showing environment-variable
instructions to the user.
