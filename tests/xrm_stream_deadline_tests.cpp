// Exercise the real stream loop with an injected connector. This models a
// Redis connect completing after the run deadline or after a stop request,
// without opening any XRM or Redis socket.
#define main xrm_stream_companion_main
#include "../tools/xrm_stream.cpp"
#undef main

int main() {
  try {
    Options options;
    options.xrmHost = "127.0.0.2";
    options.redisHost = "127.0.0.1";
    options.base = "deadline";
    options.config = {{1}, 4, 1000, 4};
    options.durationMs = 30;
    unsigned redisConnections = 0, xrmConnections = 0;
    stopping = 0;
    run(options, [&](Socket&, const std::string& host, unsigned, Deadline) {
      if (host == options.redisHost) {
        ++redisConnections;
        std::this_thread::sleep_for(80ms); // Redis connect returns after deadline.
      } else ++xrmConnections;
    });
    require(redisConnections == 1 && xrmConnections == 0,
            "opened XRM after slow Redis connection exceeded duration");

    options.durationMs = 500;
    redisConnections = xrmConnections = 0;
    stopping = 0;
    run(options, [&](Socket&, const std::string& host, unsigned, Deadline) {
      if (host == options.redisHost) {
        ++redisConnections;
        stopping = 1; // SIGINT/SIGTERM while Redis connect is in flight.
      } else ++xrmConnections;
    });
    require(redisConnections == 1 && xrmConnections == 0,
            "opened XRM after a stop request during Redis connection");

    // Socket::connect itself checks the deadline and stop before opening or
    // connecting an FD, including the immediate-call race at the boundary.
    stopping = 0;
    Socket source;
    bool denied = false;
    try { source.connect("127.0.0.2", 4210, Clock::now() - 1ms); }
    catch (const std::runtime_error&) { denied = true; }
    require(denied && !source.connected(), "expired socket connect was attempted");
    stopping = 1;
    denied = false;
    try { source.connect("127.0.0.2", 4210, Clock::now() + 1s); }
    catch (const std::runtime_error&) { denied = true; }
    require(denied && !source.connected(), "socket connect was attempted after stop");
    stopping = 0;
    std::cout << "XRM connection deadline and stop fences passed\n";
  } catch (const std::exception& error) {
    std::cerr << "XRM deadline test: " << error.what() << '\n';
    return 1;
  }
}
