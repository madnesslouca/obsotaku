/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "SecureTokenStore.hpp"

#include <libsecret/secret.h>

#include <memory>
#include <utility>

namespace {
using Status = SecureTokenStore::Status;
using Context = SecureTokenStore::RequestContext;

const SecretSchema schema = [] {
	SecretSchema value{};
	value.name = "org.otakudobs.Multistream";
	value.flags = SECRET_SCHEMA_NONE;
	value.attributes[0] = {"platform", SECRET_SCHEMA_ATTRIBUTE_STRING};
	value.attributes[1] = {"account", SECRET_SCHEMA_ATTRIBUTE_STRING};
	return value;
}();

struct ObjectUnref {
	template<typename T> void operator()(T *object) const { g_object_unref(object); }
};
template<typename T> using ObjectPtr = std::unique_ptr<T, ObjectUnref>;

struct Result {
	Status status = Status::Failed;
	std::optional<std::string> secret;
};

const char *ErrorText(Status status)
{
	switch (status) {
	case Status::Success:
	case Status::NotFound:
		return "";
	case Status::Cancelled:
		return "Secure credential storage was cancelled. A pending write may need reconciliation.";
	case Status::TimedOut:
		return "Secure credential storage timed out. A pending write may need reconciliation.";
	case Status::Unavailable:
		return "The operating-system Secret Service is unavailable. No insecure fallback is used.";
	case Status::Locked:
		return "The operating-system credential collection remains locked or its unlock was dismissed.";
	case Status::InvalidInput:
		return "Secure credential identifiers must be nonempty valid UTF-8 without NUL bytes; the secret must be nonempty.";
	default:
		return "The operating-system credential operation failed. No insecure fallback is used.";
	}
}

/* Each call owns its proxy and thread-default GLib context, so subsequent calls
 * cannot inherit a cached proxy targeting an already-destroyed context.
 * There is no application worker or polling when idle. */
class Operation {
public:
	explicit Operation(const std::shared_ptr<Context> &request)
		: request(request), context(g_main_context_new()), cancellable(g_cancellable_new())
	{
		g_main_context_push_thread_default(context);
		watch = g_timeout_source_new(20);
		g_source_set_callback(watch, Watch, this, nullptr);
		g_source_attach(watch, context);
	}
	~Operation()
	{
		g_source_destroy(watch);
		g_source_unref(watch);
		g_object_unref(cancellable);
		g_main_context_pop_thread_default(context);
		g_main_context_unref(context);
	}
	Operation(const Operation &) = delete;
	Operation &operator=(const Operation &) = delete;

	Status Interrupted()
	{
		if (interrupted == Status::NotStarted) {
			if (request->IsCancelled())
				interrupted = Status::Cancelled;
			else if (std::chrono::steady_clock::now() >= request->GetDeadline())
				interrupted = Status::TimedOut;
			if (interrupted != Status::NotStarted)
				g_cancellable_cancel(cancellable);
		}
		return interrupted;
	}
	template<typename Start> ObjectPtr<GAsyncResult> Await(Start start)
	{
		if (Interrupted() != Status::NotStarted)
			return {};
		Completion completion;
		start(cancellable, Complete, &completion);
		/* Drain cancellation completion before releasing stack state/proxies.
		 * This is not a nested Qt event loop and cannot reenter the GUI. */
		while (!completion.done)
			g_main_context_iteration(context, TRUE);
		return ObjectPtr<GAsyncResult>(completion.result);
	}
	Status Failure(GError *error)
	{
		Status status = Interrupted();
		if (status == Status::NotStarted) {
			if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
				status = Status::Cancelled;
			else if (error && g_error_matches(error, SECRET_ERROR, SECRET_ERROR_IS_LOCKED))
				status = Status::Locked;
			else if (error && (error->domain == G_DBUS_ERROR ||
					  g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED) ||
					  g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED) ||
					  g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)))
				status = Status::Unavailable;
			else
				status = Status::Failed;
		}
		/* Do not expose arbitrary remote error messages or identifiers. */
		g_clear_error(&error);
		return status;
	}

private:
	struct Completion {
		bool done = false;
		GAsyncResult *result = nullptr;
	};
	static void Complete(GObject *, GAsyncResult *result, gpointer data)
	{
		auto *completion = static_cast<Completion *>(data);
		completion->result = G_ASYNC_RESULT(g_object_ref(result));
		completion->done = true;
	}
	static gboolean Watch(gpointer data)
	{
		return static_cast<Operation *>(data)->Interrupted() == Status::NotStarted ? G_SOURCE_CONTINUE
											: G_SOURCE_REMOVE;
	}
	const std::shared_ptr<Context> request;
	GMainContext *context;
	GCancellable *cancellable;
	GSource *watch;
	Status interrupted = Status::NotStarted;
};

bool ValidIdentifier(const std::string &value)
{
	return !value.empty() && value.find('\0') == std::string::npos &&
	       g_utf8_validate(value.data(), static_cast<gssize>(value.size()), nullptr);
}

enum class Action { Save, Load, Remove };

Result Execute(Action action, const std::string &platform, const std::string &account, const std::string &secret,
	       const std::shared_ptr<Context> &request)
{
	if (!ValidIdentifier(platform) || !ValidIdentifier(account) || (action == Action::Save && secret.empty()))
		return {Status::InvalidInput, {}};

	Operation operation(request);
	GError *error = nullptr;
	auto ready = operation.Await([](GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
		secret_service_open(SECRET_TYPE_SERVICE, "org.freedesktop.secrets", SECRET_SERVICE_OPEN_SESSION, cancel,
				    callback, data);
	});
	if (!ready)
		return {operation.Failure(nullptr), {}};
	ObjectPtr<SecretService> service(secret_service_open_finish(ready.get(), &error));
	if (!service)
		return {operation.Failure(error), {}};

	std::unique_ptr<GHashTable, decltype(&g_hash_table_unref)> attributes(g_hash_table_new(g_str_hash, g_str_equal),
									  g_hash_table_unref);
	g_hash_table_insert(attributes.get(), const_cast<char *>("platform"), const_cast<char *>(platform.c_str()));
	g_hash_table_insert(attributes.get(), const_cast<char *>("account"), const_cast<char *>(account.c_str()));

	if (action == Action::Save) {
		std::unique_ptr<SecretValue, decltype(&secret_value_unref)> value(
			secret_value_new(secret.data(), static_cast<gssize>(secret.size()), "application/octet-stream"),
			secret_value_unref);
		ready = operation.Await([&](GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
			secret_service_store(service.get(), &schema, attributes.get(), SECRET_COLLECTION_DEFAULT,
					     "OtakudOBS credential", value.get(), cancel, callback, data);
		});
		if (!ready || !secret_service_store_finish(service.get(), ready.get(), &error))
			return {operation.Failure(error), {}};
		return {Status::Success, {}};
	}

	ready = operation.Await([&](GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
		const auto flags = static_cast<SecretSearchFlags>(
			SECRET_SEARCH_ALL | SECRET_SEARCH_UNLOCK | (action == Action::Load ? SECRET_SEARCH_LOAD_SECRETS : 0));
		secret_service_search(service.get(), &schema, attributes.get(), flags, cancel, callback, data);
	});
	if (!ready)
		return {operation.Failure(nullptr), {}};
	auto freeItems = [](GList *items) { g_list_free_full(items, g_object_unref); };
	std::unique_ptr<GList, decltype(freeItems)> items(secret_service_search_finish(service.get(), ready.get(), &error),
						       freeItems);
	if (error)
		return {operation.Failure(error), {}};
	if (!items)
		return {action == Action::Load ? Status::NotFound : Status::Success, {}};
	/* Search does not fail if unlocking is dismissed. Never interpret locked
	 * matching items as missing or successfully removed. */
	for (GList *item = items.get(); item; item = item->next) {
		if (secret_item_get_locked(SECRET_ITEM(item->data)))
			return {Status::Locked, {}};
	}
	if (action == Action::Load) {
		std::unique_ptr<SecretValue, decltype(&secret_value_unref)> value(
			secret_item_get_secret(SECRET_ITEM(items->data)), secret_value_unref);
		if (!value)
			return {Status::Failed, {}};
		gsize size = 0;
		const char *bytes = secret_value_get(value.get(), &size);
		return {Status::Success, std::string(bytes, size)};
	}
	for (GList *item = items.get(); item; item = item->next) {
		ready = operation.Await([&](GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
			secret_item_delete(SECRET_ITEM(item->data), cancel, callback, data);
		});
		if (!ready || !secret_item_delete_finish(SECRET_ITEM(item->data), ready.get(), &error))
			return {operation.Failure(error), {}};
	}
	return {Status::Success, {}};
}
} // namespace

bool SecureTokenStore::Save(const std::string &platformId, const std::string &accountId, const std::string &secret,
			  std::string &error)
{
	auto request = RequestForCurrentThread();
	auto result = Execute(Action::Save, platformId, accountId, secret, request);
	request->status.store(result.status, std::memory_order_relaxed);
	error = ErrorText(result.status);
	return result.status == Status::Success;
}

std::optional<std::string> SecureTokenStore::Load(const std::string &platformId, const std::string &accountId,
						std::string &error)
{
	auto request = RequestForCurrentThread();
	auto result = Execute(Action::Load, platformId, accountId, {}, request);
	request->status.store(result.status, std::memory_order_relaxed);
	error = ErrorText(result.status);
	return std::move(result.secret);
}

bool SecureTokenStore::Remove(const std::string &platformId, const std::string &accountId, std::string &error)
{
	auto request = RequestForCurrentThread();
	auto result = Execute(Action::Remove, platformId, accountId, {}, request);
	request->status.store(result.status, std::memory_order_relaxed);
	error = ErrorText(result.status);
	return result.status == Status::Success;
}
