#include <oauth/OAuthPkce.hpp>
#include <oauth/OAuthTokenSet.hpp>
#include <oauth/PlatformOAuthClient.hpp>
#include <utility/StreamPlatform.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <string_view>

using namespace std;

static bool IsBase64Url(string_view value)
{
	return all_of(value.cbegin(), value.cend(), [](char character) {
		return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
		       (character >= '0' && character <= '9') || character == '-' || character == '_';
	});
}

static int Fail(const char *message)
{
	cerr << message << '\n';
	return 1;
}

int main()
{
	static constexpr string_view rfcVerifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
	static constexpr string_view rfcChallenge = "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM";
	if (OAuthPkce::ChallengeForVerifier(string(rfcVerifier)) != rfcChallenge)
		return Fail("RFC 7636 PKCE challenge vector failed.");

	const OAuthPkceData generated = OAuthPkce::Generate();
	if (generated.verifier.size() < 43 || generated.verifier.size() > 128)
		return Fail("Generated PKCE verifier length is outside RFC 7636 limits.");
	if (!IsBase64Url(generated.verifier) || !IsBase64Url(generated.challenge) || !IsBase64Url(generated.state))
		return Fail("Generated OAuth values are not base64url strings.");
	if (OAuthPkce::ChallengeForVerifier(generated.verifier) != generated.challenge)
		return Fail("Generated PKCE verifier and challenge do not match.");

	const auto &platforms = SupportedStreamPlatforms();
	if (platforms.size() != SelectableStreamPlatforms().size())
		return Fail("Every catalog platform must be offered in the add-channel grid.");
	for (const auto platform : SelectableStreamPlatforms()) {
		const auto &info = GetStreamPlatformInfo(platform);
		if (info.platform != platform)
			return Fail("A selectable platform is missing from the catalog.");
		/* A manual platform without a key help URL leaves the user with no
		 * way to find the value the dialog is asking for. */
		if (info.ingestMode == StreamIngestMode::ManualStreamKey && platform != StreamPlatform::CustomRtmp &&
		    (info.defaultIngestServer.empty() || info.streamKeyHelpUrl.empty()))
			return Fail("A manual platform is missing its ingest server or help URL.");
		if (info.ingestMode == StreamIngestMode::ResolvedByApi && info.oauthFlow == StreamOAuthFlow::None)
			return Fail("An API-resolved platform must define an OAuth flow.");
		if (info.brandColor.size() != 7 || info.brandColor.front() != '#')
			return Fail("Every platform needs a #rrggbb brand color.");
	}
	const auto &youtube = GetStreamPlatformInfo(StreamPlatform::YouTube);
	const auto &twitch = GetStreamPlatformInfo(StreamPlatform::Twitch);
	const auto &kick = GetStreamPlatformInfo(StreamPlatform::Kick);
	if (youtube.oauthFlow != StreamOAuthFlow::AuthorizationCodePkce || !youtube.apiProvidesStreamKey)
		return Fail("YouTube OAuth capabilities are incorrect.");
	if (twitch.oauthFlow != StreamOAuthFlow::DeviceCode || !twitch.apiProvidesStreamKey)
		return Fail("Twitch OAuth capabilities are incorrect.");
	if (!kick.requiresBackendTokenExchange || !kick.apiProvidesIngestServer || !kick.apiProvidesStreamKey)
		return Fail("Kick OAuth capabilities are incorrect.");
	if (find(kick.scopes.cbegin(), kick.scopes.cend(), "streamkey:read") == kick.scopes.cend())
		return Fail("Kick streamkey:read scope is missing.");

	/* Every component must derive the account section from this helper, or the
	 * saved account state silently splits across configuration sections. */
	if (StreamPlatformConfigSection(StreamPlatform::YouTube) != "MultistreamAccount.youtube" ||
	    StreamPlatformConfigSection(StreamPlatform::Twitch) != "MultistreamAccount.twitch" ||
	    StreamPlatformConfigSection(StreamPlatform::Kick) != "MultistreamAccount.kick")
		return Fail("Unexpected multistream account configuration section.");

	OAuthAuthorizationSession youtubeSession;
	string error;
	if (!PlatformOAuthClient::CreateAuthorizationSession(StreamPlatform::YouTube, {"test client", {}},
							    "http://127.0.0.1:9876/callback", youtubeSession,
							    error))
		return Fail("Could not create a YouTube authorization session.");
	if (youtubeSession.authorizationUrl.find("code_challenge=") == string::npos ||
	    youtubeSession.authorizationUrl.find("client_id=test%20client") == string::npos ||
	    youtubeSession.authorizationUrl.find("access_type=offline") == string::npos)
		return Fail("YouTube authorization URL is incomplete.");

	OAuthAuthorizationSession kickSession;
	if (PlatformOAuthClient::CreateAuthorizationSession(StreamPlatform::Kick, {"test-client", {}},
							    "http://127.0.0.1:9876/callback", kickSession, error))
		return Fail("Kick authorization unexpectedly accepted a missing token proxy.");
	if (!PlatformOAuthClient::CreateAuthorizationSession(StreamPlatform::Kick,
							    {"test-client", "https://token-proxy.invalid/oauth/token"},
							    "http://127.0.0.1:9876/callback", kickSession, error))
		return Fail("Could not create a Kick authorization session with a token proxy.");
	if (!PlatformOAuthClient::CreateAuthorizationSession(StreamPlatform::Kick,
							    {"test-client", {}, "test-client-secret"},
							    "http://127.0.0.1:9876/callback", kickSession, error))
		return Fail("Could not create a Kick authorization session with a local development secret.");
	if (kickSession.authorizationUrl.find("streamkey%3Aread") == string::npos)
		return Fail("Kick authorization URL does not request streamkey:read.");

	/* Unique per run so a crashed earlier run cannot leave a credential behind
	 * that this one would read, delete, or collide with. */
	const string credentialAccount =
		"product-oauth-self-test-" +
		to_string(chrono::duration_cast<chrono::nanoseconds>(chrono::system_clock::now().time_since_epoch())
				  .count());
	OAuthTokenSet tokenSet;
	tokenSet.accessToken = "temporary-access-token";
	tokenSet.refreshToken = "temporary-refresh-token";
	tokenSet.scope = "test:scope";
	tokenSet.expiresAt = 2000000000;
	if (!tokenSet.Save(StreamPlatform::YouTube, string(credentialAccount), error))
		return Fail("Could not save the disposable OAuth credential test value.");
	auto loadedTokenSet = OAuthTokenSet::Load(StreamPlatform::YouTube, string(credentialAccount), error);
	if (!loadedTokenSet || loadedTokenSet->accessToken != tokenSet.accessToken ||
	    loadedTokenSet->refreshToken != tokenSet.refreshToken || loadedTokenSet->scope != tokenSet.scope) {
		OAuthTokenSet::Remove(StreamPlatform::YouTube, string(credentialAccount), error);
		return Fail("OAuth credential did not round-trip through Windows Credential Manager.");
	}
	if (!OAuthTokenSet::Remove(StreamPlatform::YouTube, string(credentialAccount), error))
		return Fail("Could not remove the disposable OAuth credential test value.");
	error.clear();
	if (OAuthTokenSet::Load(StreamPlatform::YouTube, string(credentialAccount), error) || !error.empty())
		return Fail("Disposable OAuth credential still exists after removal.");

	cout << "OAuth primitive tests passed.\n";
	return 0;
}
