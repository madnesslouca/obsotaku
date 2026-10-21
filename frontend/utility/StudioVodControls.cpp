#include "StudioVodControls.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>
#include <obs.hpp>
#include <algorithm>
#include <QStringList>
#include <obs-frontend-api.h>

StudioVodPlan CurrentStudioVodRouting()
{
	auto *main = OBSBasic::Get();
	auto *profile = main->Config();
	auto *user = App()->GetUserConfig();
	const bool advanced = QByteArray(config_get_string(profile, "Output", "Mode")) == "Advanced";
	StudioVodDestination base;
	base.liveTrack = advanced ? int(config_get_int(profile, "AdvOut", "TrackIndex")) - 1 : 0;
	base.vodTrack = advanced ? int(config_get_int(profile, "AdvOut", "VodTrackIndex")) - 1 : 1;
	base.vodEnabled = config_get_bool(profile, advanced ? "AdvOut" : "SimpleOutput", "VodTrackEnabled");
	obs_service_t *service = main->GetService();
	if (service) {
		OBSDataAutoRelease settings = obs_service_get_settings(service);
		const bool custom = QByteArray(obs_service_get_id(service)) == "rtmp_custom";
		const bool supported = custom ? config_get_bool(user, "General", "EnableCustomServerVodTrack")
					      : QString::fromUtf8(obs_data_get_string(settings, "service"))
								.compare("Twitch", Qt::CaseInsensitive) == 0;
		base.vodEnabled = base.vodEnabled && supported &&
				  (advanced || custom || config_get_bool(profile, "SimpleOutput", "UseAdvanced"));
	} else {
		base.vodEnabled = false;
	}

	// These are the channels actually configured in this output handler. Reading
	// the persistent account store here can describe stale/pending destinations
	// and would perform unnecessary work on every mixer refresh.
	std::vector<StudioVodDestination> destinations;
	if (auto *object = main->findChild<QObject *>("studioMultistreamManager")) {
		auto *manager = static_cast<MultiStreamManager *>(object);
		for (const auto &channel : manager->ConfiguredChannels()) {
			if (!channel.enabled) {
				continue;
			}
			destinations.push_back(
				{int(channel.audioMixIndex), int(channel.vodTrackIndex),
				 channel.vodTrackEnabled && GetStreamPlatformInfo(channel.platform).supportsVodTrack});
		}
	}
	auto plan = ResolveStudioVodRouting(base, destinations);
	uint32_t actualVod = 0;
	bool actualInvalid = false;
	auto protect = [&plan, &actualVod, &actualInvalid](obs_output_t *output) {
		if (!output || !obs_output_active(output)) {
			return;
		}
		plan.activeSession = true;
		const bool multipleVideo = obs_output_get_video_encoder2(output, 1) != nullptr;
		const bool rtmp = QByteArray(obs_output_get_id(output)) == "rtmp_output";
		if (multipleVideo) {
			plan.unknownLayout = true;
		}
		for (size_t index = 0; index < MAX_AUDIO_MIXES; ++index) {
			auto *encoder = obs_output_get_audio_encoder(output, index);
			if (!encoder) {
				continue;
			}
			const size_t mix = obs_encoder_get_mixer_index(encoder);
			if (mix >= MAX_AUDIO_MIXES) {
				actualInvalid = true;
				continue;
			}
			const uint32_t bit = 1u << mix;
			// Only the established single-video RTMP layout assigns slot 1 to
			// its VOD archive. SRT/RIST and other multitrack slots are program.
			if (rtmp && !multipleVideo && index == 1) {
				actualVod |= bit;
			} else {
				plan.liveMask |= bit;
			}
		}
	};
	OBSOutputAutoRelease output = obs_frontend_get_streaming_output();
	protect(output);
	for (const auto &channel : main->MultistreamChannelOutputs()) {
		protect(channel.output);
	}
	if (actualVod) {
		// An active archive is frozen to its real encoder mix. Preferences
		// edited for a later restart must not silently become this toggle's target.
		plan.vodMask = actualVod;
		plan.configured = true;
		plan.effective = true;
		plan.invalid = actualInvalid;
	} else if (plan.activeSession) {
		plan.configured = false; // Prepared clean mix only, not an archive being sent.
	}
	plan.invalid = plan.invalid || actualInvalid || plan.unknownLayout;
	plan.conflict = (plan.liveMask & plan.vodMask) != 0;
	return plan;
}

QString StudioVodRoutingHint(const StudioVodPlan &plan)
{
	auto tracks = [](uint32_t mask) {
		QStringList names;
		for (int bit = 0; bit < 6; ++bit) {
			if (mask & (1u << bit)) {
				names.append(QString::number(bit + 1));
			}
		}
		return names.join(QStringLiteral(", "));
	};
	const char *status = plan.unknownLayout   ? "Studio.VodUnknownLayout"
			     : plan.invalid       ? "Studio.VodInvalid"
			     : plan.conflict      ? "Studio.VodConflict"
			     : plan.effective     ? "Studio.VodEffective"
			     : plan.activeSession ? "Studio.VodPending"
			     : plan.configured    ? "Studio.VodReady"
						  : "Studio.VodPreview";
	return QTStr("Studio.VodTracks").arg(tracks(plan.liveMask), tracks(plan.vodMask)) + QStringLiteral("\n") +
	       QTStr(status) + QStringLiteral("\n") + QTStr("Studio.VodSharedMixHint");
}

bool SetStudioSourceVodExcluded(obs_source_t *source, bool excluded)
{
	if (!source || obs_source_removed(source) || !(obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO)) {
		return false;
	}
	const StudioVodPlan plan = CurrentStudioVodRouting();
	if (plan.invalid || plan.conflict) {
		return false;
	}
	const uint32_t before = obs_source_get_audio_mixers(source), included = excluded ? 0 : plan.vodMask;
	const uint32_t after = MergeStudioVodRouting(before, plan.vodMask, included, plan.liveMask);
	if (before == after) {
		return true;
	}
	obs_source_set_audio_mixers(source, after);
	const std::string uuid = obs_source_get_uuid(source);
	auto restore = [uuid, target = plan.vodMask](const std::string &value) {
		OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
		if (!source || obs_source_removed(source)) {
			return;
		}
		const auto currentPlan = CurrentStudioVodRouting();
		if (currentPlan.invalid) {
			return;
		}
		obs_source_set_audio_mixers(source,
					    MergeStudioVodRouting(obs_source_get_audio_mixers(source), target,
								  uint32_t(std::stoul(value)), currentPlan.liveMask));
		OBSBasic::Get()->SaveProject();
	};
	OBSBasic::Get()->undo_s.add_action(QTStr("Studio.VodToggle"), restore, restore,
					   std::to_string(before & plan.vodMask), std::to_string(after & plan.vodMask),
					   false);
	OBSBasic::Get()->SaveProject();
	return true;
}
