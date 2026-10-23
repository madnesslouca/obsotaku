/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <oauth/OAuthHttpClient.hpp>
#include <chrono>
#include <iostream>
#include <thread>
using namespace std;
int main(int argc, char **argv)
{
 if (argc != 6) return 2;
 const string method = argv[1], url = argv[2], mode = argv[3];
 const int delay = stoi(argv[4]);
 const size_t size = stoull(argv[5]);
 OAuthHttpClient::Cancellation cancel;
 auto signal = make_shared<atomic_bool>(delay == 0);
 if (mode == "operation") cancel.operation = signal;
 if (mode == "shutdown") cancel.shutdown = signal;
 thread interrupter;
 if (delay > 0 && cancel.Enabled()) interrupter = thread([=]{
  this_thread::sleep_for(chrono::milliseconds(delay)); signal->store(true);
 });
 OAuthHttpResponse response;
 string error;
 const auto started = chrono::steady_clock::now();
 bool ok = false;
 if (method == "GET") ok = OAuthHttpClient::Get(url, {}, response, error, cancel);
 else if (method == "JSON") ok = OAuthHttpClient::SendJson("POST", url, "{}", {}, response, error, cancel);
 else if (method == "FORM") ok = OAuthHttpClient::PostForm(url, {{"test", "synthetic"}}, {}, response, error);
 else ok = OAuthHttpClient::SendBytes(method, url, string(size, 'x'), "application/octet-stream", {}, response, error, cancel);
 const auto ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now()-started).count();
 if(interrupter.joinable()) interrupter.join();
 const bool cancelledError = error == "The HTTP operation was cancelled; the remote result is unconfirmed.";
 cout << "{\"ok\":" << (ok?"true":"false") << ",\"status\":" << response.statusCode
      << ",\"cancelled_error\":" << (cancelledError?"true":"false") << ",\"duration_ms\":" << ms
      << ",\"body_size\":" << response.body.size() << "}\n";
 return 0;
}
