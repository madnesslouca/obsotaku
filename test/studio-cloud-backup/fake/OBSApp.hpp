#pragma once
#include <util/config-file.h>
#include <filesystem>
struct FakeApp {
 std::filesystem::path userConfigLocation = "/fixture/A";
 config_t *GetUserConfig();
 config_t *GetAppConfig();
};
FakeApp *App();
