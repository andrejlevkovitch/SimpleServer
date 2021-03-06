// Server.cpp

#include "ss/Server.hpp"
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/coroutine.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/version.hpp>
#include <boost/asio/write.hpp>
#include <list>
#include <regex>
#include <simple_logs/logs.hpp>
#include <sstream>
#include <thread>

#define REQ_BUFFER_RESERVED 1024 * 1000
#define RES_BUFFER_RESERVED 1024 * 1000

// XXX must be after <thread>
#include <boost/asio/yield.hpp>

namespace ss {
namespace asio = boost::asio;

using tcp             = asio::ip::tcp;
using stream_protocol = asio::local::stream_protocol;


template <typename Protocol>
class Session final
    : public asio::coroutine
    , public std::enable_shared_from_this<Session<Protocol>> {
public:
  using Self     = std::shared_ptr<Session>;
  using Socket   = asio::basic_stream_socket<Protocol>;
  using Endpoint = typename Protocol::endpoint;
  using Strand   = asio::strand<typename Socket::executor_type>;

  Session(Socket socket, RequestHandler handler) noexcept
      : socket_ {
    std::move(socket)
  }
#if BOOST_ASIO_VERSION > 101400
  , strand_ {
    asio::make_strand(socket_.get_executor())
  }
#else
  , strand_ {
    socket_.get_executor()
  }
#endif
  , reqHandler_{handler} {
    LOG_TRACE("construct session");

    reqBuffer_.reserve(REQ_BUFFER_RESERVED);
    resBuffer_.reserve(RES_BUFFER_RESERVED);
  }

  void start() {
    LOG_TRACE("start new session");

    Self self = this->shared_from_this();


    std::stringstream remoteEndpoint;
    remoteEndpoint << socket_.remote_endpoint();
    error_code err = self->reqHandler_->atSessionStart(remoteEndpoint.str());
    if (err.failed()) {
      LOG_ERROR(err.message());
      LOG_WARNING("session doesn't start, because get handler error");
      return;
    }

    this->operator()(std::move(self), error_code{}, 0);
  }

  void close() {
    if (socket_.is_open() == false) {
      LOG_WARNING("session already closed");
      return;
    }

    LOG_TRACE("close session");

    error_code err;

    // cancel all async operation with this socket
    socket_.cancel(err);
    if (err.failed()) {
      LOG_ERROR(err.message());
    }
  }

  /**\note all async operations with socket must be already canceled
   */
  void atClose() {
    LOG_TRACE("at session close");

    reqHandler_->atSessionClose();

    error_code err;
    socket_.shutdown(Socket::shutdown_both, err);
    if (err.failed()) {
      LOG_ERROR(err.message());
    }

    socket_.close(err);
    if (err.failed()) {
      LOG_ERROR(err.message());
    }
  }

  bool isOpen() const {
    return socket_.is_open();
  }


private:
  void operator()(Self self, error_code err, size_t transfered) {
    if (err.failed()) {
      if (err == asio::error::eof) {
        LOG_DEBUG("client close connection");
      } else if (err == asio::error::operation_aborted) {
        LOG_DEBUG("session canceled");
      } else {
        LOG_ERROR(err.message());
      }

      this->atClose();

      LOG_DEBUG("end of session coroutine");
      return;
    }

    reenter(this) {
      for (;;) {
        yield asio::async_read(
            socket_,
            asio::dynamic_buffer(reqBuffer_),
            asio::transfer_at_least(1),
            asio::bind_executor(strand_,
                                std::bind(&Session::operator(),
                                          this,
                                          std::move(self),
                                          std::placeholders::_1,
                                          std::placeholders::_2)));

        LOG_DEBUG("readed: %1.3fKb", transfered / 1024.);


        // request handling
        {
          std::string_view reqBufferView = reqBuffer_;
          for (;;) {
            size_t reqIgnoreLength = 0;
            err                    = reqHandler_->handle(reqBufferView,
                                      std::back_inserter(resBuffer_),
                                      reqIgnoreLength);
            if (err.failed() == false) {
              if (reqIgnoreLength == 0 ||
                  reqIgnoreLength >= reqBufferView.size()) {
                reqBuffer_.clear();
                break;
              } else {
                reqBufferView =
                    std::string_view{reqBufferView.data() + reqIgnoreLength,
                                     reqBufferView.size() - reqIgnoreLength};
                continue;
              }
            } else { // if some error caused
              if (err == error::SessionError::PartialData) {
                reqBuffer_.erase(0,
                                 reqBufferView.data() - reqBuffer_.data() +
                                     reqIgnoreLength);

                LOG_WARNING("partial data in request: %1.3fKb",
                            reqBuffer_.size());

                break;
              }

              // unexpected errors
              this->operator()(std::move(self), err, 0);
              return;
            }
          }
        }

        if (resBuffer_.empty()) {
          continue;
        }

        yield asio::async_write(
            socket_,
            asio::dynamic_buffer(resBuffer_),
            asio::transfer_all(),
            asio::bind_executor(strand_,
                                std::bind(&Session::operator(),
                                          this,
                                          std::move(self),
                                          std::placeholders::_1,
                                          std::placeholders::_2)));

        LOG_DEBUG("writed: %1.3fKb", transfered / 1024.);

        // clear after every write
        resBuffer_.clear();
      }
    }
  }

private:
  Socket         socket_;
  Strand         strand_;
  RequestHandler reqHandler_;
  std::string    reqBuffer_;
  std::string    resBuffer_;
};


class ServerImpl {
public:
  virtual ~ServerImpl() = default;


  virtual void startAccepting() noexcept = 0;

  virtual void stopAccepting() noexcept(false) = 0;

  virtual void closeAllSessions() = 0;
};


template <typename Protocol>
class ServerImplStream
    : public ServerImpl
    , public asio::coroutine
    , public std::enable_shared_from_this<ServerImplStream<Protocol>> {
public:
  using Self       = std::shared_ptr<ServerImpl>;
  using Endpoint   = typename Protocol::endpoint;
  using SessionPtr = std::shared_ptr<Session<Protocol>>;
  using Socket     = asio::basic_stream_socket<Protocol>;
  using Acceptor   = asio::basic_socket_acceptor<Protocol>;

  ServerImplStream(asio::io_context &    ioContext,
                   Endpoint              endpoint,
                   RequestHandlerFactory reqHandlerFactory)
      : ioContext_{ioContext}
      , acceptor_{ioContext}
      , reqHandlerFactory_{std::move(reqHandlerFactory)} {
    LOG_TRACE("construct sever");

    Protocol protocol = endpoint.protocol();
    acceptor_.open(protocol);
    acceptor_.set_option(typename Acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(Socket::max_listen_connections);
  }

  void startAccepting() noexcept override {
    LOG_TRACE("start accepting");

    Self self = this->shared_from_this();

    this->operator()(std::move(self), error_code{}, Socket{this->ioContext_});
  }

  void stopAccepting() noexcept(false) override {
    LOG_TRACE("stop accepting");

    acceptor_.cancel();
  }

  void closeAllSessions() override {
    LOG_TRACE("close all sessions");

    for (SessionPtr &session : sessions_) {
      if (session->isOpen()) {
        session->close();
      }
    }

    sessions_.clear();
  }


private:
  void operator()(Self self, error_code err, Socket socket) noexcept {
    if (err.failed()) {
      if (err.value() == asio::error::operation_aborted) {
        LOG_DEBUG("accepting canceled");
      } else {
        LOG_ERROR(err.message());
      }

      LOG_DEBUG("break the server coroutine");
      return;
    }

    reenter(this) {
      for (;;) {
        yield acceptor_.async_accept(std::bind(&ServerImplStream::operator(),
                                               this,
                                               std::move(self),
                                               std::placeholders::_1,
                                               std::placeholders::_2));

        LOG_DEBUG("accept new connection");
        try {
          LOG_DEBUG("accept connection from: %1%", socket.remote_endpoint());

          // at first remove already closed sessions
          sessions_.remove_if([](const SessionPtr &session) {
            if (session->isOpen() == false) {
              return true;
            }
            return false;
          });


          RequestHandler reqHandler = reqHandlerFactory_->makeRequestHandler();
          SessionPtr     session =
              std::make_shared<Session<Protocol>>(std::move(socket),
                                                  std::move(reqHandler));


          session->start();
          sessions_.emplace_back(std::move(session));

          LOG_DEBUG("sessions opened: %1%", sessions_.size());
        } catch (std::exception &e) {
          LOG_ERROR(e.what());
        }
      }
    }
  }


private:
  asio::io_context &    ioContext_;
  Acceptor              acceptor_;
  RequestHandlerFactory reqHandlerFactory_;

  std::list<SessionPtr> sessions_;
};


Server &Server::asyncRun() {
  impl_->startAccepting();
  return *this;
}

Server &Server::stop() {
  impl_->stopAccepting();
  impl_->closeAllSessions();
  return *this;
}


ServerBuilder::ServerBuilder(asio::io_context &ioContext)
    : ioContext_{ioContext} {
}

ServerBuilder &
ServerBuilder::setRequestHandlerFactory(RequestHandlerFactory factory) {
  reqHandlerFactory_ = std::move(factory);
  return *this;
}

ServerBuilder &ServerBuilder::setEndpoint(Server::Protocol protocol,
                                          std::string_view endpoint) {
  protocol_ = protocol;
  endpoint_ = endpoint;
  return *this;
}

ServerPtr ServerBuilder::build() const {
  if (reqHandlerFactory_ == nullptr) {
    LOG_THROW(std::invalid_argument, "invalid request handler factory");
  }


  std::shared_ptr<ServerImpl> impl;
  switch (protocol_) {
  case Server::Protocol::Tcp: {
    std::regex  hostAndPortReg{R"(([^:]+):(\d{1,5}))"};
    std::smatch match;

    if (std::regex_match(endpoint_, match, hostAndPortReg) == false) {
      LOG_THROW(std::invalid_argument, "invalid host or port");
    }

    std::string host = match[1];
    std::string port = match[2];


    error_code err;
    auto       addr = asio::ip::make_address(host, err);
    if (err.failed()) {
      LOG_THROW(std::invalid_argument, "invalid address: %1%", host);
    }

    tcp::endpoint endpoint(addr, std::stoi(port));

    LOG_DEBUG("listen endpoint: %1%", endpoint);


    impl = std::make_shared<ServerImplStream<tcp>>(ioContext_,
                                                   endpoint,
                                                   reqHandlerFactory_);
  } break;
  case Server::Protocol::Unix: {
    stream_protocol::endpoint endpoint{endpoint_};

    LOG_DEBUG("listen endpoint: %1%", endpoint);


    impl =
        std::make_shared<ServerImplStream<stream_protocol>>(ioContext_,
                                                            endpoint,
                                                            reqHandlerFactory_);
  } break;
  }


  ServerPtr retval = std::make_shared<Server>(Server{});
  retval->impl_    = impl;

  return retval;
}
} // namespace ss
