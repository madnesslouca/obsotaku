#pragma once

#include <obs.hpp>
#include <QWidget>
#include <functional>
#include <vector>

class QTimer;

class StudioEffectControls final : public QWidget {
	OBSWeakSource weakFilter;
	std::function<void(const std::string &, const std::string &)> commit;
	std::vector<std::function<void(obs_data_t *)>> readers;
	QTimer *pending;
	std::string before, observed;
	bool editing = false;
	void Update(const char *key, double value, bool integer = false);
	void Number(const char *key, const QString &label, double low, double high, double step,
		    int decimals, double scale, const QString &suffix, bool integer = false);
	void Dial(const char *key, const QString &label, bool pan);
	void Refresh();

public:
	static bool Supports(const char *id);
	StudioEffectControls(obs_source_t *filter,
		std::function<void(const std::string &, const std::string &)> commit, QWidget *parent);
	~StudioEffectControls() override;
	void Flush();
	void Reset();
};
