/******************************************************************************
    Copyright (C) 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "MultistreamChannelStore.hpp"
#include "MultistreamChannelPlan.hpp"
#include "SecureTokenStore.hpp"
#include "MultistreamTaskPool.hpp"
#include <oauth/ConnectedAccountManager.hpp>

#include <OBSApp.hpp>

#include <util/base.h>
#include <util/config-file.h>

#include <QUuid>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QThread>
#include <QThreadPool>
#include <condition_variable>
#include <map>
#include <mutex>
#include <chrono>

#include <algorithm>
#include <atomic>
#include <optional>
#include <unordered_set>

using namespace std;

namespace {
constexpr const char *SECTION = "MultistreamChannels";
constexpr const char *MANUAL_KEY_STORE = "manual-rtmp";
constexpr int MAX_STORED_CHANNELS = 32;
constexpr const char *CHANNEL_FIELDS[] = {
	"Id",    "Platform",   "DisplayName",  "AccountId",  "ChatAddress",     "Server",        "AvatarUrl",
	"Title", "CategoryId", "CategoryName", "VideoLayout", "PortraitFit",     "AudioTrack",
	"VodTrackEnabled", "VodTrackIndex", "Enabled"};

string Key(int index, const char *name)
{
	return "Channel" + to_string(index) + "." + name;
}

const char *StringValue(config_t *config, const string &key)
{
	const char *value = config_get_string(config, SECTION, key.c_str());
	return value ? value : "";
}

size_t ClampTrack(int64_t value, size_t fallback)
{
	return (value < 0 || value >= MAX_AUDIO_MIXES) ? fallback : static_cast<size_t>(value);
}

void ClearStoredChannels(config_t *config, int count)
{
	for (int index = 0; index < min(count, MAX_STORED_CHANNELS); ++index) {
		for (const char *name : CHANNEL_FIELDS) {
			config_remove_value(config, SECTION, Key(index, name).c_str());
		}
	}
}

void WriteStoredChannels(config_t *config, const vector<MultiStreamChannel> &channels)
{
	int index = 0;
	for (const auto &channel : channels) {
		const auto &info = GetStreamPlatformInfo(channel.platform);
		config_set_string(config, SECTION, Key(index, "Id").c_str(), channel.id.c_str());
		config_set_string(config, SECTION, Key(index, "Platform").c_str(), string(info.id).c_str());
		config_set_string(config, SECTION, Key(index, "DisplayName").c_str(), channel.displayName.c_str());
		config_set_string(config, SECTION, Key(index, "AccountId").c_str(), channel.accountId.c_str());
		config_set_string(config, SECTION, Key(index, "ChatAddress").c_str(), channel.chatAddress.c_str());
		config_set_string(config, SECTION, Key(index, "Server").c_str(), channel.server.c_str());
		config_set_string(config, SECTION, Key(index, "AvatarUrl").c_str(), channel.avatarUrl.c_str());
		config_set_string(config, SECTION, Key(index, "Title").c_str(), channel.title.c_str());
		config_set_string(config, SECTION, Key(index, "CategoryId").c_str(), channel.categoryId.c_str());
		config_set_string(config, SECTION, Key(index, "CategoryName").c_str(), channel.categoryName.c_str());
		config_set_string(config, SECTION, Key(index, "VideoLayout").c_str(),
				  channel.videoLayout == MultiStreamVideoLayout::Portrait ? "portrait" : "main");
		config_set_string(config, SECTION, Key(index, "PortraitFit").c_str(),
				  channel.portraitFit == MultiStreamPortraitFit::Fit ? "fit" : "fill");
		config_set_int(config, SECTION, Key(index, "AudioTrack").c_str(),
			       static_cast<int64_t>(channel.audioMixIndex));
		config_set_bool(config, SECTION, Key(index, "VodTrackEnabled").c_str(), channel.vodTrackEnabled);
		config_set_int(config, SECTION, Key(index, "VodTrackIndex").c_str(),
			       static_cast<int64_t>(channel.vodTrackIndex));
		config_set_bool(config, SECTION, Key(index, "Enabled").c_str(), channel.enabled);
		++index;
	}
	config_set_int(config, SECTION, "Count", index);
}

struct SecretBackup {
	string channelId;
	optional<string> previous;
};

/* Reads the pre-store layout, where each platform had one hard-coded section.
 * Runs once: Save() writes the new layout and the old keys stop being read. */
vector<MultiStreamChannel> LoadLegacyAccounts(config_t *config)
{
	vector<MultiStreamChannel> channels;
	for (const auto platform : {StreamPlatform::YouTube, StreamPlatform::Twitch, StreamPlatform::Kick}) {
		const string section = StreamPlatformConfigSection(platform);
		const char *accountId = config_get_string(config, section.c_str(), "AccountId");
		if (!accountId || !*accountId)
			continue;

		const char *displayName = config_get_string(config, section.c_str(), "DisplayName");
		const auto &info = GetStreamPlatformInfo(platform);

		MultiStreamChannel channel;
		channel.id = MultistreamChannelStore::ChannelIdForAccount(platform, accountId);
		channel.displayName = displayName && *displayName ? displayName : string(info.displayName);
		channel.platform = platform;
		channel.accountId = accountId;
		/* The old layout had no separate field: back then the display name was
		 * still the platform handle, so it is the best seed available. */
		if (displayName && *displayName)
			channel.chatAddress = displayName;
		channel.audioMixIndex = ClampTrack(config_get_int(config, section.c_str(), "AudioTrack"), 0);
		channel.vodTrackEnabled = config_get_bool(config, section.c_str(), "VodTrackEnabled");
		channel.vodTrackIndex = ClampTrack(config_get_int(config, section.c_str(), "VodTrackIndex"), 1);
		channel.enabled = config_has_user_value(config, section.c_str(), "ChannelEnabled")
					  ? config_get_bool(config, section.c_str(), "ChannelEnabled")
					  : true;
		channels.emplace_back(std::move(channel));
	}
	/* Portrait destinations share one canvas and therefore one framing mode.
	 * Normalize older configurations that stored different values per channel
	 * so the preview can never disagree with the encoded output. */
	MultistreamChannelPlan::NormalizePortraitFit(channels);
	return channels;
}
} // namespace

string MultistreamChannelStore::ChannelIdForAccount(StreamPlatform platform, const string &accountId)
{
	return string(GetStreamPlatformInfo(platform).id) + ":" + accountId;
}

string MultistreamChannelStore::NewManualChannelId(StreamPlatform platform)
{
	const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
	return string(GetStreamPlatformInfo(platform).id) + ":manual:" + uuid.toStdString();
}

static vector<MultiStreamChannel> ReadMetadataGui()
{
	config_t *config = App()->GetUserConfig();
	if (!config)
		return {};

	if (!config_has_user_value(config, SECTION, "Count"))
		return LoadLegacyAccounts(config);

	const int count = static_cast<int>(config_get_int(config, SECTION, "Count"));
	vector<MultiStreamChannel> channels;
	for (int index = 0; index < min(count, MAX_STORED_CHANNELS); ++index) {
		const string id = StringValue(config, Key(index, "Id"));
		const auto platform = StreamPlatformFromId(StringValue(config, Key(index, "Platform")));
		if (id.empty() || !platform)
			continue;

		MultiStreamChannel channel;
		channel.id = id;
		channel.platform = *platform;
		channel.displayName = StringValue(config, Key(index, "DisplayName"));
		channel.accountId = StringValue(config, Key(index, "AccountId"));
		channel.chatAddress = StringValue(config, Key(index, "ChatAddress"));
		channel.server = StringValue(config, Key(index, "Server"));
		channel.avatarUrl = StringValue(config, Key(index, "AvatarUrl"));
		channel.title = StringValue(config, Key(index, "Title"));
		channel.categoryId = StringValue(config, Key(index, "CategoryId"));
		channel.categoryName = StringValue(config, Key(index, "CategoryName"));
		const string videoLayout = StringValue(config, Key(index, "VideoLayout"));
		/* Existing TikTok destinations migrate to the portrait pipeline. Other
		 * existing destinations keep the main canvas exactly as before. */
		channel.videoLayout = videoLayout == "portrait" ||
					      (videoLayout.empty() && *platform == StreamPlatform::TikTok)
					      ? MultiStreamVideoLayout::Portrait
					      : MultiStreamVideoLayout::Main;
		channel.portraitFit = StringValue(config, Key(index, "PortraitFit")) == "fit"
					      ? MultiStreamPortraitFit::Fit
					      : MultiStreamPortraitFit::Fill;
		channel.audioMixIndex = ClampTrack(config_get_int(config, SECTION, Key(index, "AudioTrack").c_str()), 0);
		channel.vodTrackEnabled = config_get_bool(config, SECTION, Key(index, "VodTrackEnabled").c_str());
		channel.vodTrackIndex =
			ClampTrack(config_get_int(config, SECTION, Key(index, "VodTrackIndex").c_str()), 1);
		channel.enabled = config_get_bool(config, SECTION, Key(index, "Enabled").c_str());
		if (channel.displayName.empty())
			channel.displayName = string(GetStreamPlatformInfo(*platform).displayName);

		channels.emplace_back(std::move(channel));
	}
	return channels;
}

namespace {
using Store = MultistreamChannelStore;
using Request = Store::Request;
using RawFields = vector<pair<string, optional<string>>>;
struct Job;
struct State {
	QThreadPool pool;
	QObject *bridge = nullptr; // GUI-owned; Shutdown drains before its destruction.
	mutex cacheMutex;
	string scope, fingerprint;
	uint64_t revision = 0;
	vector<MultiStreamChannel> metadata;
	struct CachedKey {
		string identity, value;
	};
	map<string, CachedKey> keys;
	shared_ptr<Job> current;
	bool shuttingDown = false;
};
unique_ptr<State> state;
atomic<State *> publishedState{nullptr};
bool OnGui()
{
	return QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread();
}
State &GuiState()
{
	assert(OnGui());
	if (!state) {
		state = make_unique<State>();
		state->pool.setMaxThreadCount(1);
		state->pool.setExpiryTimeout(1000);
		state->pool.setObjectName(QStringLiteral("MultistreamCredentialTransactions"));
		state->bridge = new QObject(QCoreApplication::instance());
		MultistreamTaskPool().SetBeforeShutdown([] { Store::Shutdown(); });
		QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, state->bridge,
				 [] { Store::Shutdown(); });
		publishedState.store(state.get(), memory_order_release);
	}
	return *state;
}
RawFields ReadFields(config_t *config)
{
	RawFields fields;
	const auto add = [&](string key) {
		fields.emplace_back(key, config_has_user_value(config, SECTION, key.c_str())
						 ? optional<string>(StringValue(config, key))
						 : nullopt);
	};
	add("Count");
	for (int i = 0; i < MAX_STORED_CHANNELS; ++i) {
		for (const auto *name : CHANNEL_FIELDS) {
			add(Key(i, name));
		}
	}
	return fields;
}
void RestoreFields(config_t *config, const RawFields &fields)
{
	for (const auto &[key, value] : fields) {
		if (value) {
			config_set_string(config, SECTION, key.c_str(), value->c_str());
		} else {
			config_remove_value(config, SECTION, key.c_str());
		}
	}
}
string Scope(config_t *config)
{
	return App()->userConfigLocation.lexically_normal().u8string() + "/obs-studio/user.ini|" +
	       string(config_get_string(config, "Basic", "ProfileDir")
			      ? config_get_string(config, "Basic", "ProfileDir")
			      : "");
}
string Fingerprint(config_t *config)
{
	QJsonArray fields;
	for (const auto &[key, value] : ReadFields(config)) {
		fields.append(QJsonArray{QString::fromStdString(key), value.has_value(),
					 value ? QString::fromStdString(*value) : QString()});
	}
	// Legacy account metadata and local client registrations participate in CAS.
	for (auto platform : {StreamPlatform::YouTube, StreamPlatform::Twitch, StreamPlatform::Kick}) {
		string section = StreamPlatformConfigSection(platform);
		for (const auto *name : {"AccountId", "DisplayName", "AudioTrack", "VodTrackEnabled", "VodTrackIndex",
					 "ChannelEnabled", "ClientId"}) {
			const char *v = config_get_string(config, section.c_str(), name);
			fields.append(QJsonArray{QString::fromStdString(section + "/" + name),
						 config_has_user_value(config, section.c_str(), name),
						 QString::fromUtf8(v ? v : "")});
		}
	}
	return QCryptographicHash::hash(QJsonDocument(fields).toJson(QJsonDocument::Compact),
					QCryptographicHash::Sha256)
		.toHex()
		.toStdString();
}
string CredentialIdentity(const MultiStreamChannel &channel)
{
	if (GetStreamPlatformInfo(channel.platform).ingestMode != StreamIngestMode::ManualStreamKey) {
		return {};
	}
	return string(GetStreamPlatformInfo(channel.platform).id) + "|manual|" + channel.accountId;
}
vector<MultiStreamChannel> ObserveGui()
{
	auto &s = GuiState();
	auto *config = App()->GetUserConfig();
	if (!config) {
		return {};
	}
	auto metadata = ReadMetadataGui();
	const string scope = Scope(config), fingerprint = Fingerprint(config);
	lock_guard lock(s.cacheMutex);
	if (s.scope != scope) {
		s.keys.clear();
	}
	if (s.scope != scope || s.fingerprint != fingerprint) {
		++s.revision;
	}
	s.scope = scope;
	s.fingerprint = fingerprint;
	s.metadata = metadata;
	for (auto it = s.keys.begin(); it != s.keys.end();) {
		if (none_of(metadata.begin(), metadata.end(), [&](const auto &c) {
			    return c.id == it->first && !CredentialIdentity(c).empty() &&
				   CredentialIdentity(c) == it->second.identity;
		    })) {
			it = s.keys.erase(it);
		} else {
			++it;
		}
	}
	for (auto &c : metadata) {
		if (auto it = s.keys.find(c.id); it != s.keys.end() && !CredentialIdentity(c).empty() &&
						 it->second.identity == CredentialIdentity(c)) {
			c.streamKey = it->second.value;
		}
	}
	return metadata;
}
bool Validate(const vector<MultiStreamChannel> &channels, string &error)
{
	if (channels.size() > MAX_STORED_CHANNELS) {
		error = "Too many multistream destinations.";
		return false;
	}
	unordered_set<string> ids;
	for (const auto &c : channels) {
		if (c.id.empty() || !ids.insert(c.id).second) {
			error = "Multistream destination ids must be present and unique.";
			return false;
		}
	}
	return true;
}
bool CommitMetadata(const vector<MultiStreamChannel> &channels, string &error)
{
	auto *config = App()->GetUserConfig();
	if (!config) {
		error = "The user configuration is not available.";
		return false;
	}
	const auto before = ReadFields(config);
	ClearStoredChannels(config, MAX_STORED_CHANNELS);
	WriteStoredChannels(config, channels);
	if (config_save_safe(config, "tmp", nullptr) != CONFIG_SUCCESS) {
		RestoreFields(config, before);
		error = "Could not save the multistream channel configuration.";
		return false;
	}
	ObserveGui();
	error.clear();
	return true;
}
bool Manual(const MultiStreamChannel &c)
{
	return GetStreamPlatformInfo(c.platform).ingestMode == StreamIngestMode::ManualStreamKey;
}
bool Expired(const Request &context)
{
	return context->IsCancelled() || chrono::steady_clock::now() >= context->GetDeadline();
}
struct Job {
	Request context;
	bool write = false;
	bool external = false, externalAttempted = false;
	string externalPlatform, externalAccount, externalSecret;
	optional<string> externalPrevious;
	function<bool(string &)> guiCommit;
	string scope, fingerprint, deliveryScope, deliveryFingerprint;
	uint64_t revision = 0, deliveryRevision = 0;
	vector<MultiStreamChannel> before, after;
	vector<SecretBackup> backups, touched;
	map<string, optional<string>> loaded;
	Store::Result result;
	mutex decisionMutex;
	condition_variable decisionChanged;
	bool decided = false, accepted = false;
	// These closures are created and invoked only on GUI, never inspected in worker.
	function<bool()> receiverAlive;
	Store::Completion complete;
	QMetaObject::Connection receiverDestroyed;
	function<bool()> canCommit;
};
void InvalidateKeys(const shared_ptr<Job> &job)
{
	if (job->external) {
		return;
	}
	lock_guard lock(state->cacheMutex);
	if (state->scope != job->scope) {
		return;
	}
	for (const auto &c : job->before) {
		if (Manual(c)) {
			state->keys.erase(c.id);
		}
	}
	for (const auto &c : job->after) {
		if (Manual(c)) {
			state->keys.erase(c.id);
		}
	}
}
void Rollback(const shared_ptr<Job> &job)
{
	if (job->touched.empty()) {
		return;
	}
	for (auto it = job->touched.rbegin(); it != job->touched.rend(); ++it) {
		string error;
		bool ok = it->previous ? SecureTokenStore::Save(MANUAL_KEY_STORE, it->channelId, *it->previous, error)
				       : SecureTokenStore::Remove(MANUAL_KEY_STORE, it->channelId, error);
		if (!ok) {
			job->result.rollbackComplete = false;
			if (job->result.error.find("Rollback incomplete") == string::npos) {
				job->result.error += " Rollback incomplete: stored credential state requires repair.";
			}
		}
	}
	InvalidateKeys(job);
}
void DecideGui(const shared_ptr<Job> &job)
{
	assert(OnGui());
	unique_lock decision(job->decisionMutex);
	if (job->decided) {
		return;
	}
	// The GUI decision and worker timeout are serialized. A native config commit
	// that actually succeeded is never reported as an uncommitted cancellation.
	if (!job->receiverAlive() || Expired(job->context)) {
		job->result.error = "Credential request cancelled or timed out before configuration commit.";
	} else if (job->canCommit && !job->canCommit()) {
		job->result.error =
			"Outputs or application state changed while credentials were pending. Please retry when idle.";
	} else {
		ObserveGui();
		auto &s = *state;
		bool same;
		{
			lock_guard lock(s.cacheMutex);
			same = s.scope == job->scope && s.fingerprint == job->fingerprint &&
			       s.revision == job->revision;
		}
		if (!same) {
			job->result.error =
				"The channel configuration changed while credentials were pending. Please retry.";
		} else if (job->external ? job->guiCommit(job->result.error)
					 : (!job->write || CommitMetadata(job->after, job->result.error))) {
			if (job->external) {
				ObserveGui();
			}
			{
				lock_guard lock(s.cacheMutex);
				for (auto &[id, key] : job->loaded) {
					const auto &channels = job->write ? job->after : job->before;
					auto channel = find_if(channels.begin(), channels.end(),
							       [&](const auto &c) { return c.id == id && Manual(c); });
					if (key && channel != channels.end()) {
						s.keys[id] = {CredentialIdentity(*channel), *key};
					} else {
						s.keys.erase(id);
					}
				}
				if (job->write) {
					for (const auto &c : job->before) {
						if (Manual(c) && none_of(job->after.begin(), job->after.end(),
									 [&](const auto &n) { return n.id == c.id; })) {
							s.keys.erase(c.id);
						}
					}
				}
			}
			{
				lock_guard lock(s.cacheMutex);
				job->deliveryScope = s.scope;
				job->deliveryFingerprint = s.fingerprint;
				job->deliveryRevision = s.revision;
			}
			job->accepted = true;
			job->result.configCommitted = job->write || job->external;
		}
	}
	job->decided = true;
	decision.unlock();
	job->decisionChanged.notify_all();
}
void RunJob(const shared_ptr<Job> &job)
{
	// Leases acquire and release their mutex on THIS worker only. They are never
	// placed in Job, because queued GUI closures also own Job references.
	vector<unique_ptr<ConnectedAccountManager::CredentialRemoval>> oauthRemovals;
	{
		SecureTokenStore::RequestScope scope(job->context);
		if (job->external) {
			job->externalPrevious =
				SecureTokenStore::Load(job->externalPlatform, job->externalAccount, job->result.error);
			if (job->result.error.empty()) {
				job->externalAttempted = true;
				if (!SecureTokenStore::Save(job->externalPlatform, job->externalAccount,
							    job->externalSecret, job->result.error) &&
				    job->result.error.empty()) {
					job->result.error = "Could not save the provider credential.";
				}
			}
		}
		vector<string> ids;
		for (const auto &c : job->before) {
			if (!job->external && Manual(c) && find(ids.begin(), ids.end(), c.id) == ids.end()) {
				ids.push_back(c.id);
			}
		}
		for (const auto &c : job->after) {
			if (!job->external && Manual(c) && find(ids.begin(), ids.end(), c.id) == ids.end()) {
				ids.push_back(c.id);
			}
		}
		// Every affected secret is backed up before ANY mutation, including a write
		// whose cancellation response can race a remote commit.
		for (const auto &id : ids) {
			string error;
			auto previous = SecureTokenStore::Load(MANUAL_KEY_STORE, id, error);
			if (!error.empty()) {
				job->result.error = error;
				break;
			}
			job->backups.push_back({id, previous});
			job->loaded[id] = previous;
		}
		if (job->result.error.empty() && job->write) {
			map<pair<int, string>, ConnectedStreamAccount> accounts;
			for (const auto &old : job->before) {
				if (!Manual(old) && !old.accountId.empty() &&
				    none_of(job->after.begin(), job->after.end(), [&](const auto &c) {
					    return !Manual(c) && c.platform == old.platform &&
						   c.accountId == old.accountId;
				    })) {
					accounts[{int(old.platform), old.accountId}] = {old.platform, old.accountId,
											old.displayName, old.enabled};
				}
			}
			// Stable lock ordering and ALL OAuth backups before the first mutation.
			for (const auto &[identity, account] : accounts) {
				string error;
				auto lease =
					ConnectedAccountManager::BeginCredentialRemoval(account, job->context, error);
				if (!lease) {
					job->result.error = error.empty() ? "Could not back up the account credential."
									  : error;
					break;
				}
				oauthRemovals.emplace_back(std::move(lease));
			}
		}
		if (job->result.error.empty() && job->write) {
			for (auto &lease : oauthRemovals) {
				string error;
				if (!lease->Remove(error)) {
					job->result.error = error.empty() ? "Could not remove the account credential."
									  : error;
					break;
				}
			}
		}
		if (job->result.error.empty() && job->write) {
			for (const auto &backup : job->backups) {
				auto next = find_if(job->after.begin(), job->after.end(), [&](const auto &c) {
					return Manual(c) && c.id == backup.channelId;
				});
				const bool remove = next == job->after.end();
				if (!remove && (next->streamKey.empty() ||
						(backup.previous && *backup.previous == next->streamKey))) {
					continue;
				}
				job->touched.push_back(backup); // BEFORE the possibly-committed request.
				string error;
				bool ok = remove ? SecureTokenStore::Remove(MANUAL_KEY_STORE, backup.channelId, error)
						 : SecureTokenStore::Save(MANUAL_KEY_STORE, backup.channelId,
									  next->streamKey, error);
				if (!ok) {
					job->result.error = error.empty() ? "Could not update the stored stream key."
									  : error;
					break;
				}
				if (remove) {
					job->loaded[backup.channelId] = nullopt;
				} else {
					job->loaded[backup.channelId] = next->streamKey;
				}
			}
		}
		job->result.credentialStatus = job->context->GetStatus();
	}
	if (job->result.error.empty() && !Expired(job->context)) {
		QMetaObject::invokeMethod(state->bridge, [job] { DecideGui(job); }, Qt::QueuedConnection);
		unique_lock lock(job->decisionMutex);
		while (!job->decided && !Expired(job->context)) {
			job->decisionChanged.wait_for(lock, chrono::milliseconds(20));
		}
		if (!job->decided) {
			job->decided = true;
			job->result.error = "Credential request cancelled or timed out before configuration commit.";
		}
	} else if (job->result.error.empty()) {
		job->result.error = "Credential request cancelled or timed out.";
	}
	if (!job->accepted) {
		auto recovery = make_shared<SecureTokenStore::RequestContext>();
		SecureTokenStore::RequestScope recoveryScope(recovery); // One15s deadline for the WHOLE recovery.
		Rollback(job);
		if (job->externalAttempted) {
			string error;
			bool restored =
				job->externalPrevious
					? SecureTokenStore::Save(job->externalPlatform, job->externalAccount,
								 *job->externalPrevious, error)
					: SecureTokenStore::Remove(job->externalPlatform, job->externalAccount, error);
			if (!restored) {
				job->result.rollbackComplete = false;
				job->result.error +=
					" Provider credential rollback incomplete; stored state requires repair.";
			}
		}
		for (auto it = oauthRemovals.rbegin(); it != oauthRemovals.rend(); ++it) {
			string error;
			if (!(*it)->Rollback(error)) {
				job->result.rollbackComplete = false;
				job->result.error +=
					" OAuth credential rollback incomplete; account state requires repair.";
			}
		}
		InvalidateKeys(job);
	} else {
		for (auto &lease : oauthRemovals) {
			lease->Commit();
		}
	}
	oauthRemovals.clear(); // Worker-thread unlock, before queuing GUI completion.
	if (!job->result.rollbackComplete) {
		blog(LOG_ERROR, "Credential transaction rollback incomplete; stored credential state requires repair.");
	}
	if (!job->result.configCommitted && Expired(job->context)) {
		job->result.credentialStatus = job->context->IsCancelled() ? SecureTokenStore::Status::Cancelled
									   : SecureTokenStore::Status::TimedOut;
	}
	job->result.success = job->accepted;
	job->result.channels = job->write ? job->after : job->before;
	for (auto &channel : job->result.channels) {
		if (Manual(channel)) {
			channel.streamKey.clear();
			if (job->accepted) {
				if (auto it = job->loaded.find(channel.id); it != job->loaded.end() && it->second) {
					channel.streamKey = *it->second;
				}
			}
			if (job->accepted && channel.enabled && channel.streamKey.empty()) {
				job->result.warning += "Stored stream key is missing for " + channel.displayName + ". ";
			}
		}
	}
	if (!job->accepted || job->external) {
		job->result.channels.clear();
		job->result.warning.clear();
	}
	QMetaObject::invokeMethod(
		state->bridge,
		[job] {
			assert(OnGui());
			QObject::disconnect(job->receiverDestroyed);
			bool closing;
			{
				lock_guard lock(state->cacheMutex);
				closing = state->shuttingDown;
			}
			if (!closing && job->accepted && job->receiverAlive()) {
				ObserveGui();
				{
					lock_guard lock(state->cacheMutex);
					job->result.deliveryCurrent = state->scope == job->deliveryScope &&
								      state->fingerprint == job->deliveryFingerprint &&
								      state->revision == job->deliveryRevision;
				}
				if (job->canCommit && !job->canCommit()) {
					job->result.deliveryCurrent = false;
				}
				if (!job->result.deliveryCurrent) {
					job->result.channels.clear();
					job->result.warning +=
						job->result.configCommitted
							? "Settings were saved, but the current profile or output state changed before delivery. Refresh destinations when idle."
							: "Credentials were read, but the current profile or output state changed before delivery. Refresh destinations when idle.";
				}
			}
			// Keep Busy through this GUI handoff. No intervening GUI turn can replace the
			// profile between the final check and the caller applying its runtime copy.
			{
				lock_guard lock(state->cacheMutex);
				if (state->current == job) {
					state->current.reset();
				}
			}
			if (!closing && job->receiverAlive()) {
				job->complete(std::move(job->result));
			}
		},
		Qt::QueuedConnection);
}
Request Start(bool write, vector<MultiStreamChannel> desired, QObject *receiver, Store::Completion complete,
	      function<bool()> canCommit, string externalPlatform = {}, string externalAccount = {},
	      string externalSecret = {}, function<bool(string &)> guiCommit = {})
{
	assert(OnGui());
	auto &s = GuiState();
	auto context = make_shared<SecureTokenStore::RequestContext>();
	auto reject = [receiver, complete](Store::Result result) mutable {
		QPointer<QObject> guard(receiver);
		QMetaObject::invokeMethod(
			receiver,
			[guard, complete = std::move(complete), result = std::move(result)]() mutable {
				if (guard) {
					complete(std::move(result));
				}
			},
			Qt::QueuedConnection);
	};
	if (!receiver) {
		return context;
	}
	{
		lock_guard lock(s.cacheMutex);
		if (s.shuttingDown || s.current) {
			Store::Result result;
			result.error = s.shuttingDown
					       ? "The application is shutting down."
					       : "A credential operation is already pending. Please wait or cancel it.";
			reject(std::move(result));
			return context;
		}
	}
	string error;
	if (write && !Validate(desired, error)) {
		Store::Result result;
		result.error = error;
		reject(std::move(result));
		return context;
	}
	auto before = ObserveGui();
	if (!App()->GetUserConfig()) {
		Store::Result result;
		result.error = "The user configuration is not available.";
		reject(std::move(result));
		return context;
	}
	auto job = make_shared<Job>();
	job->canCommit = std::move(canCommit);
	job->context = context;
	job->write = write;
	job->before = before;
	job->after = write ? std::move(desired) : before;
	job->external = bool(guiCommit);
	job->guiCommit = std::move(guiCommit);
	job->externalPlatform = std::move(externalPlatform);
	job->externalAccount = std::move(externalAccount);
	job->externalSecret = std::move(externalSecret);
	QPointer<QObject> guard(receiver);
	job->receiverAlive = [guard] {
		return !guard.isNull();
	};
	job->complete = std::move(complete);
	{
		lock_guard lock(s.cacheMutex);
		job->scope = s.scope;
		job->fingerprint = s.fingerprint;
		job->revision = s.revision;
		s.current = job;
	}
	job->receiverDestroyed = QObject::connect(receiver, &QObject::destroyed, s.bridge, [weak = weak_ptr<Job>(job)] {
		if (auto job = weak.lock()) {
			job->context->Cancel();
			job->decisionChanged.notify_all();
		}
	});
	s.pool.start([job] { RunJob(job); });
	return context;
}
} // namespace

vector<MultiStreamChannel> MultistreamChannelStore::Load()
{
	if (OnGui()) {
		return ObserveGui();
	}
	// Worker callers only consume an immutable metadata/cache copy; no config_t
	// and no GUI object is accessed off the frontend thread.
	auto *snapshot = publishedState.load(memory_order_acquire);
	if (!snapshot) {
		return {};
	}
	lock_guard lock(snapshot->cacheMutex);
	auto channels = snapshot->metadata;
	for (auto &channel : channels) {
		if (auto it = snapshot->keys.find(channel.id); it != snapshot->keys.end() &&
							       !CredentialIdentity(channel).empty() &&
							       it->second.identity == CredentialIdentity(channel)) {
			channel.streamKey = it->second.value;
		}
	}
	return channels;
}
std::function<bool()> MultistreamChannelStore::CaptureMetadataGuard()
{
	assert(OnGui());
	if (!App()->GetUserConfig()) {
		return [] {
			return false;
		};
	}
	ObserveGui();
	auto &s = *state;
	string scope, fingerprint;
	uint64_t revision;
	{
		lock_guard lock(s.cacheMutex);
		scope = s.scope;
		fingerprint = s.fingerprint;
		revision = s.revision;
	}
	return [scope = std::move(scope), fingerprint = std::move(fingerprint), revision] {
		assert(OnGui());
		if (!App()->GetUserConfig()) {
			return false;
		}
		{
			lock_guard lock(state->cacheMutex);
			if (state->shuttingDown) {
				return false;
			}
		}
		ObserveGui();
		lock_guard lock(state->cacheMutex);
		return state->scope == scope && state->fingerprint == fingerprint && state->revision == revision;
	};
}
bool MultistreamChannelStore::IsBusy()
{
	auto *snapshot = publishedState.load(memory_order_acquire);
	if (!snapshot) {
		return false;
	}
	lock_guard lock(snapshot->cacheMutex);
	return bool(snapshot->current);
}
void MultistreamChannelStore::Shutdown()
{
	if (!state) {
		return;
	}
	{
		lock_guard lock(state->cacheMutex);
		state->shuttingDown = true;
		if (state->current) {
			state->current->context->Cancel();
			state->current->decisionChanged.notify_all();
		}
	}
	// Worker cancellation and rollback do not require the GUI queue to run.
	state->pool.waitForDone();
	{
		lock_guard lock(state->cacheMutex);
		state->keys.clear();
		state->metadata.clear();
		state->current.reset();
	}
}
MultistreamChannelStore::Request MultistreamChannelStore::LoadAsync(QObject *receiver, Completion complete,
								    function<bool()> canCommit)
{
	return Start(false, {}, receiver, std::move(complete), std::move(canCommit));
}
MultistreamChannelStore::Request MultistreamChannelStore::SaveAsync(vector<MultiStreamChannel> channels,
								    QObject *receiver, Completion complete,
								    function<bool()> canCommit)
{
	return Start(true, std::move(channels), receiver, std::move(complete), std::move(canCommit));
}
MultistreamChannelStore::Request MultistreamChannelStore::SaveSecretAsync(string platformId, string accountId,
									  string secret, QObject *receiver,
									  function<bool(string &)> guiCommit,
									  Completion complete,
									  function<bool()> canCommit)
{
	return Start(false, {}, receiver, std::move(complete), std::move(canCommit), std::move(platformId),
		     std::move(accountId), std::move(secret), std::move(guiCommit));
}
MultistreamChannelStore::Request MultistreamChannelStore::UpsertAsync(MultiStreamChannel channel, QObject *receiver,
								      Completion complete, function<bool()> canCommit)
{
	auto channels = Load();
	if (channel.videoLayout == MultiStreamVideoLayout::Portrait) {
		for (auto &c : channels) {
			if (c.videoLayout == MultiStreamVideoLayout::Portrait) {
				c.portraitFit = channel.portraitFit;
			}
		}
	}
	auto it = find_if(channels.begin(), channels.end(), [&](const auto &c) { return c.id == channel.id; });
	if (it == channels.end()) {
		channels.push_back(std::move(channel));
	} else {
		*it = std::move(channel);
	}
	return SaveAsync(std::move(channels), receiver, std::move(complete), std::move(canCommit));
}
MultistreamChannelStore::Request MultistreamChannelStore::RemoveAsync(string id, QObject *receiver, Completion complete,
								      function<bool()> canCommit)
{
	auto channels = Load();
	channels.erase(remove_if(channels.begin(), channels.end(), [&](const auto &c) { return c.id == id; }),
		       channels.end());
	return SaveAsync(std::move(channels), receiver, std::move(complete), std::move(canCommit));
}
bool MultistreamChannelStore::Save(const vector<MultiStreamChannel> &channels, string &error)
{
	if (!OnGui()) {
		error = "Channel metadata must be committed on the frontend thread.";
		return false;
	}
	auto before = Load();
	auto &s = GuiState();
	{
		lock_guard lock(s.cacheMutex);
		if (s.shuttingDown || s.current) {
			error = "A credential operation is pending. Please wait or cancel it.";
			return false;
		}
	}
	if (!Validate(channels, error)) {
		return false;
	}
	for (const auto &previous : before) {
		if (Manual(previous) && none_of(channels.begin(), channels.end(),
						[&](const auto &c) { return Manual(c) && c.id == previous.id; })) {
			error = "Removing a stored key requires an asynchronous credential transaction.";
			return false;
		}
	}
	for (const auto &previous : before) {
		if (!Manual(previous) && !previous.accountId.empty() &&
		    none_of(channels.begin(), channels.end(), [&](const auto &c) {
			    return !Manual(c) && c.platform == previous.platform && c.accountId == previous.accountId;
		    })) {
			error = "Removing OAuth credentials requires an asynchronous credential transaction.";
			return false;
		}
	}
	for (const auto &channel : channels) {
		if (Manual(channel) && !channel.streamKey.empty()) {
			auto old = find_if(before.begin(), before.end(),
					   [&](const auto &c) { return c.id == channel.id; });
			if (old == before.end() || old->streamKey != channel.streamKey) {
				error = "Changing a stored key requires an asynchronous credential transaction.";
				return false;
			}
		}
	}
	return CommitMetadata(channels, error);
}

bool MultistreamChannelStore::Upsert(const MultiStreamChannel &channel, string &error)
{
	if (channel.id.empty()) {
		error = "A multistream destination needs an id.";
		return false;
	}

	auto channels = Load();
	if (channel.videoLayout == MultiStreamVideoLayout::Portrait) {
		for (auto &item : channels) {
			if (item.videoLayout == MultiStreamVideoLayout::Portrait)
				item.portraitFit = channel.portraitFit;
		}
	}
	auto existing = find_if(channels.begin(), channels.end(),
				[&](const MultiStreamChannel &item) { return item.id == channel.id; });
	if (existing != channels.end())
		*existing = channel;
	else
		channels.push_back(channel);
	return Save(channels, error);
}

bool MultistreamChannelStore::Remove(const string &channelId, string &error)
{
	auto channels = Load();
	const auto removed = remove_if(channels.begin(), channels.end(),
				       [&](const MultiStreamChannel &item) { return item.id == channelId; });
	if (removed == channels.end()) {
		error.clear();
		return true;
	}
	channels.erase(removed, channels.end());

	return Save(channels, error);
}

bool MultistreamChannelStore::UpdateIdentity(const vector<MultiStreamChannel> &resolved, string &error)
{
	auto channels = Load();
	bool changed = false;
	for (const auto &source : resolved) {
		auto item = find_if(channels.begin(), channels.end(),
				    [&](const MultiStreamChannel &channel) { return channel.id == source.id; });
		if (item == channels.end())
			continue;
		const auto adopt = [&changed](string &target, const string &value) {
			if (value.empty() || target == value)
				return;
			target = value;
			changed = true;
		};
		adopt(item->displayName, source.displayName);
		adopt(item->chatAddress, source.chatAddress);
		adopt(item->avatarUrl, source.avatarUrl);
	}

	if (!changed) {
		error.clear();
		return true;
	}
	return Save(channels, error);
}

bool MultistreamChannelStore::SetEnabled(const string &channelId, bool enabled, string &error)
{
	auto channels = Load();
	auto item = find_if(channels.begin(), channels.end(),
			    [&](const MultiStreamChannel &channel) { return channel.id == channelId; });
	if (item == channels.end()) {
		error = "The selected multistream channel does not exist.";
		return false;
	}
	item->enabled = enabled;
	return Save(channels, error);
}
