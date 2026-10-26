// Harness for the real WebSocket client (synqframe-protocol's ws-client.cpp), the code
// the plugin's layout socket and config channel and the compositor worker's socket run on.
//   wsclient-check <url> keepalive <seconds>   stay connected across server pings
//   wsclient-check <url> stop                  stop() must not wait out a server
//                                              that never answers the upgrade
// Exit: 0 pass, 1 fail, 2 could not connect.
#include <synqframe/ws-client.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using clock_type = std::chrono::steady_clock;

int main(int argc, char **argv)
{
  if (argc < 3) {
    std::fprintf(stderr, "usage: wsclient-check <url> <keepalive <seconds>|stop>\n");
    return 64;
  }
  const std::string mode = argv[2];
  std::atomic<int> connects{0}, disconnects{0};
  WsClientConfig config;
  config.url = argv[1];
  config.connectedLog = "connected";
  config.label = "check socket";
  config.log = [](bool, const std::string &line) { std::printf("%s\n", line.c_str()); };
  // Test only: the local wss server uses a throwaway self-signed certificate.
  config.insecureTls = std::getenv("SYNQ_WS_INSECURE") != nullptr;
  config.onConnected = [&] { ++connects; };
  config.onDisconnected = [&] { ++disconnects; };

  if (mode == "keepalive") {
    const int seconds = argc > 3 ? std::atoi(argv[3]) : 20;
    WsClient client(config);
    client.start();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    // Read the counters before stop(): stopping ends the connection on purpose.
    const bool stillConnected = client.connected();
    const int connected = connects.load(), lost = disconnects.load();
    client.stop();
    std::printf("connects=%d lost=%d connected at end=%d\n", connected, lost, stillConnected);
    if (connected == 0) return 2;
    return connected == 1 && lost == 0 && stillConnected ? 0 : 1;
  }

  if (mode == "stop") {
    WsClient client(config);
    client.start();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto begin = clock_type::now();
    client.stop();
    const double took = std::chrono::duration<double>(clock_type::now() - begin).count();
    std::printf("stop() took %.1fs\n", took);
    return took < 2.0 ? 0 : 1;
  }
  return 64;
}
