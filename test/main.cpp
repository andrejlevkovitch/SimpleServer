// main.cpp

#include "logs.hpp"
#include "ss/Server.hpp"
#include <algorithm>
#include <boost/asio/signal_set.hpp>
#include <iterator>
#include <regex>
#include <string>


class EchoReqHandler final : public ss::AbstractRequestHandler {
public:
  ss::error_code
  atSessionStart(std::string_view remoteEndpoint) noexcept override {
    LOG_INFO("start session for remote endpoint: %1%", remoteEndpoint);
    return {};
  }

  ss::error_code handle(std::string_view request,
                        ResponseInserter respInserter,
                        size_t &         reqIgnoreLength) noexcept override {
    std::regex lineReg{R"(^[^\n]*\n)"};

    std::cregex_token_iterator iter{request.begin(), request.end(), lineReg};
    if (iter == std::cregex_token_iterator{}) {
      return ss::error::make_error_code(ss::error::SessionError::PartialData);
    }

    std::string echo = iter->str();

    std::copy(echo.begin(), echo.end(), respInserter);
    reqIgnoreLength = echo.size();

    return ss::error::make_error_code(ss::error::SessionError::Success);
  }
};

class EchoReqHandlerFactory final : public ss::AbstractRequestHandlerFactory {
public:
  ss::RequestHandler makeRequestHandler() noexcept override {
    return std::make_shared<EchoReqHandler>();
  }
};


int main(int argc, char *argv[]) {
  auto back  = std::make_shared<logs::TextStreamBackend>(std::cerr);
  auto front = std::make_shared<logs::LightFrontend>();
  LOGGER_ADD_SINK(front, back);

  if (argc < 2) {
    LOG_FAILURE("you must specify endpoint as first argument");
  }
  std::string endpoint = argv[1];


  std::string protocol = "tcp";
  if (argc >= 3) {
    protocol = argv[2];
  }

  LOG_INFO("protocol: %1%", protocol);
  LOG_INFO("endpoint: %1%", endpoint);


  namespace asio   = boost::asio;
  using error_code = boost::system::error_code;

  asio::io_context ioContext;

  ss::Server::Protocol proto;
  if (protocol == "tcp") {
    proto = ss::Server::Protocol::Tcp;
  } else if (protocol == "unix") {
    proto = ss::Server::Protocol::Unix;
  } else {
    LOG_FAILURE("unknown protocol: %1%", protocol);
  }


  ss::ServerBuilder builder{ioContext};
  ss::ServerPtr     server =
      builder.setEndpoint(proto, endpoint)
          .setRequestHandlerFactory(std::make_shared<EchoReqHandlerFactory>())
          .build();

  server->asyncRun();


  // wait for SIGTERM || SIGINT
  asio::signal_set sigSet{ioContext, SIGINT, SIGTERM};
  sigSet.async_wait([&server, &ioContext](const error_code &error, int val) {
    if (error.failed()) {
      LOG_ERROR(error.message());
    }

    if (val == SIGINT) {
      LOG_DEBUG("sigint");
    } else if (val == SIGTERM) {
      LOG_DEBUG("sigterm");
    } else {
      LOG_WARNING("unknown signal: %1%", val);
    }

    server->stop();
    ioContext.stop();
  });


  std::list<std::thread> threadPool;
  for (int i = 0; i < 3; ++i) {
    threadPool.emplace_back([&ioContext]() {
      ioContext.run();
    });
  }

  ioContext.run();

  for (std::thread &th : threadPool) {
    th.join();
  }


  return EXIT_SUCCESS;
}
