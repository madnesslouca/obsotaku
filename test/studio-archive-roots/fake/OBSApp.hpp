#pragma once
#include <filesystem>
#include <string>
#include <cstddef>
struct FakeApp {
 std::filesystem::path userConfigLocation,userProfilesLocation,userScenesLocation,userPluginManagerSettingsLocation;
 std::string GetVersionString(bool) const { return "fixture-version"; }
};
FakeApp *App();
int GetAppConfigPath(char *path,size_t size,const char *file);
extern std::filesystem::path globalConfigBase;
extern int credentialCalls,credentialMetadataCalls;
