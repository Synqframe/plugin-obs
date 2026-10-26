// Harness for the plugin's layout websocket (module.cpp, ensureLayoutSocket),
// linked against the same libcurl the plugin ships with.
//   client.exe <url> <variant> <seconds>
// variants:
//   timeout  the pre-fix behaviour: CURLOPT_TIMEOUT_MS set for the connect phase
//   progress the current behaviour: connect phase bounded by a progress callback
// Exit: 0 survived the full duration, 1 lost the connection, 2 connect failed.
#include <curl/curl.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using clock_type = std::chrono::steady_clock;

struct Handshake { clock_type::time_point deadline; };
static int handshakeProgress(void *data, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
  return clock_type::now() > static_cast<Handshake *>(data)->deadline ? 1 : 0;
}

int main(int argc, char **argv)
{
  if (argc < 4) { std::fprintf(stderr, "usage: client <url> <timeout|progress> <seconds>\n"); return 64; }
  const std::string url = argv[1], variant = argv[2];
  const int runSeconds = std::atoi(argv[3]);
  CURL *handle = curl_easy_init();
  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_CONNECT_ONLY, 2L);
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
  // Test-only: the local wss server uses a throwaway self-signed certificate.
  if (std::getenv("SYNQ_WS_INSECURE")) {
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 0L);
  }
  Handshake handshake{clock_type::now() + std::chrono::seconds(5)};
  if (variant == "progress") {
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, handshakeProgress);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &handshake);
  } else {
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, 5000L);
  }
  const auto begin = clock_type::now();
  auto seconds = [&] { return std::chrono::duration<double>(clock_type::now() - begin).count(); };
  CURLcode result = curl_easy_perform(handle);
  if (result != CURLE_OK) { std::printf("connect failed after %.1fs: %s\n", seconds(), curl_easy_strerror(result)); return 2; }
  std::printf("connected after %.1fs\n", seconds());
  const auto start = clock_type::now();
  while (std::chrono::duration<double>(clock_type::now() - start).count() < runSeconds) {
    char buffer[65536];
    size_t received = 0;
    const struct curl_ws_frame *frame = nullptr;
    result = curl_ws_recv(handle, buffer, sizeof(buffer), &received, &frame);
    if (result == CURLE_AGAIN) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
    if (result != CURLE_OK || !frame) {
      std::printf("connection lost after %.1fs: %s\n", seconds(), curl_easy_strerror(result));
      return 1;
    }
  }
  std::printf("survived %ds\n", runSeconds);
  curl_easy_cleanup(handle);
  return 0;
}
