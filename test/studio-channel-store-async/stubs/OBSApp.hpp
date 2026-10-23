#pragma once
#include <filesystem>
#include <util/config-file.h>
struct OBSApp {
	config_t *config = nullptr;
	std::filesystem::path userConfigLocation;
	config_t *GetUserConfig() { return config; }
};
OBSApp *App();
