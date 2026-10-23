#pragma once
#include <atomic>
#include <optional>
#include <string>
extern std::atomic_int keyringDelay, loads, saves, deletes, ioOnGui, configOnWorker, httpCalls, archives, refreshCalls;
extern std::atomic_bool unavailable, failSave, deleteCommitCancel, blockRefresh, refreshEntered, releaseRefresh;
extern std::atomic_int keyringActive, httpDelay, httpActive, httpCancelledCalls;
void SeedCloud(bool expired=false);
std::optional<std::string> CloudRaw();
void ResetFacts();

void SelectCloudConfigScope(bool alternate);

extern std::atomic_int configSaveFailures;
void ReplaceCloudRaw(std::optional<std::string> raw);

extern bool configUnavailable, appUnavailable;

extern std::atomic_bool richListResponses;

extern std::atomic_int stageRestores;

extern std::atomic_int httpDownloadCalls;

extern std::atomic_bool saveCommitCancel, allowAuthorization, blockExchange, exchangeEntered, releaseExchange;
extern std::atomic_int exchangeCalls, browserCalls;

extern std::atomic_bool richListExtra;
extern std::atomic_int externalLoadReplacements;
void SetCloudReplacementAtLoad(int number,std::string raw);

extern std::atomic_bool nextPageOnce;
