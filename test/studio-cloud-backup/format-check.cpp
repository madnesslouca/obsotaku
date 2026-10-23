/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <oauth/OAuthTokenSet.hpp>
#include <utility/SecureTokenStore.hpp>
#include <QApplication>
#include "facts.hpp"
#include <iostream>
#include <thread>
#include <vector>
using namespace std;
int checks=0, failures=0;
void Check(bool ok,const char*name){++checks;if(!ok){++failures;cerr<<"FAIL "<<name<<'\n';}}
bool Equal(const OAuthTokenSet&a,const OAuthTokenSet&b){return a.accessToken==b.accessToken&&a.refreshToken==b.refreshToken&&a.tokenType==b.tokenType&&a.scope==b.scope&&a.expiresAt==b.expiresAt&&a.refreshExpiresAt==b.refreshExpiresAt;}
int main(int argc,char**argv){QApplication app(argc,argv);ResetFacts();thread worker([]{
 string error;
 OAuthTokenSet tokens;tokens.accessToken="qa-access";tokens.refreshToken="qa-refresh";tokens.tokenType="Bearer";tokens.scope="drive.appdata";tokens.expiresAt=4102444800;tokens.refreshExpiresAt=4102444900;
 const string golden=R"({"access_token": "qa-access", "expires_at": 4102444800, "refresh_expires_at": 4102444900, "refresh_token": "qa-refresh", "scope": "drive.appdata", "token_type": "Bearer"})";
 Check(tokens.SaveAs("qa","primary",error),"ASCII SaveAs succeeds");Check(CloudRaw()==optional<string>(golden),"ASCII RAW byte encoding/schema/order matches frozen golden");
 auto loaded=OAuthTokenSet::LoadAs("qa","primary",error);Check(loaded&&Equal(tokens,*loaded)&&error.empty(),"ASCII LoadAs preserves all fields");
 tokens.accessToken="アクセストークン✓";tokens.refreshToken="renovação-😀";tokens.scope="espaço/日本語";
 Check(tokens.SaveAs("qa","primary",error),"Unicode SaveAs succeeds");auto unicodeRaw=CloudRaw();
 Check(unicodeRaw&&unicodeRaw->find(tokens.accessToken)!=string::npos&&unicodeRaw->find(tokens.refreshToken)!=string::npos,"Unicode RAW remains UTF8 rather than transformed bytes");
 loaded=OAuthTokenSet::LoadAs("qa","primary",error);Check(loaded&&Equal(tokens,*loaded),"Unicode LoadAs preserves all fields");
 tokens.expiresAt=1;Check(tokens.AccessTokenExpired(),"expired timestamp remains expired");tokens.expiresAt=4102444800;Check(!tokens.AccessTokenExpired(),"future timestamp remains future");tokens.expiresAt=0;Check(!tokens.AccessTokenExpired(),"unknown expiry with access token retains legacy behavior");
 OAuthTokenSet empty;error="previous";Check(!empty.SaveAs("qa","primary",error)&&error=="An OAuth token set must contain an access or refresh token.","empty set keeps exact validation error");
 for(const auto&raw:vector<string>{"not-json","[]","{}",R"({"access_token":"","refresh_token":""})"}){
  ReplaceCloudRaw(raw);loaded=OAuthTokenSet::LoadAs("qa","primary",error);Check(!loaded&&!error.empty(),"invalid/empty stored credential stays rejected");
 }
 ReplaceCloudRaw(R"({"refresh_token":"qa-only-refresh"})");loaded=OAuthTokenSet::LoadAs("qa","primary",error);
 Check(loaded&&loaded->accessToken.empty()&&loaded->refreshToken=="qa-only-refresh"&&loaded->tokenType=="Bearer"&&loaded->scope.empty()&&loaded->expiresAt==0&&loaded->refreshExpiresAt==0,"missing fields retain defaults and refresh-only set is valid");
 ReplaceCloudRaw(nullopt);loaded=OAuthTokenSet::LoadAs("qa","primary",error);Check(!loaded&&error.empty(),"missing keyring value remains distinct from parse failure");
#ifdef FORMAT_HELPERS
 auto serialized=tokens.Serialize(error);Check(serialized.has_value(),"Serialize helper succeeds");
 if(serialized){auto parsed=OAuthTokenSet::Parse(*serialized,error);Check(parsed&&Equal(tokens,*parsed),"Parse Serialize helper roundtrip");}
 auto raw=OAuthTokenSet::Parse(golden,error);Check(raw&&raw->Serialize(error)==optional<string>(golden),"new helpers preserve frozen ASCII RAW exactly");
 Check(!OAuthTokenSet::Parse("not-json",error)&&error=="The stored OAuth credential is invalid.","Parse exact invalid JSON error retained");
 Check(!OAuthTokenSet::Parse("{}",error)&&error=="The stored OAuth credential contains no tokens.","Parse exact missing tokens error retained");
 Check(!empty.Serialize(error)&&error=="An OAuth token set must contain an access or refresh token.","Serialize exact empty token error retained");
#endif
 });worker.join();Check(ioOnGui==0&&configOnWorker==0,"format tests use only worker keyring IO");cout<<"{\"checks\":"<<checks<<",\"failures\":"<<failures<<",\"passed\":"<<(failures?"false":"true")<<"}\n";return failures?1:0;}
