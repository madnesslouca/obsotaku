#pragma once
#include <filesystem>
#include <QString>
#include <util/config-file.h>
#define QTStr(key) QString::fromUtf8(key)
struct OBSApp {
	config_t *config = nullptr;
	std::filesystem::path userConfigLocation;
	config_t *GetUserConfig() { return config; }
};
OBSApp *App();
