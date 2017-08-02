#pragma once
#include <atomic>
#include <chrono>
#include <vector>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/array.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/lexical_cast.hpp>

#include <boost/container/flat_map.hpp>

#include "crow/http_parser_merged.h"

#include "crow/dumb_timer_queue.h"
#include "crow/http_response.h"
#include "crow/logging.h"
#include "crow/middleware_context.h"
#include "crow/parser.h"
#include "crow/settings.h"
#include "crow/socket_adaptors.h"

namespace crow {
using namespace boost;
using tcp = asio::ip::tcp;

namespace detail {
template <typename MW>
struct check_before_handle_arity_3_const {
  template <typename T, void (T::*)(request&, response&, typename MW::context&)
                            const = &T::before_handle>
  struct get {};
};

template <typename MW>
struct check_before_handle_arity_3 {
  template <typename T, void (T::*)(request&, response&,
                                    typename MW::context&) = &T::before_handle>
  struct get {};
};

template <typename MW>
struct check_after_handle_arity_3_const {
  template <typename T, void (T::*)(request&, response&, typename MW::context&)
                            const = &T::after_handle>
  struct get {};
};

template <typename MW>
struct check_after_handle_arity_3 {
  template <typename T, void (T::*)(request&, response&,
                                    typename MW::context&) = &T::after_handle>
  struct get {};
};

template <typename T>
struct is_before_handle_arity_3_impl {
  template <typename C>
  static std::true_type f(
      typename check_before_handle_arity_3_const<T>::template get<C>*);

  template <typename C>
  static std::true_type f(
      typename check_before_handle_arity_3<T>::template get<C>*);

  template <typename C>
  static std::false_type f(...);

 public:
  static const bool value = decltype(f<T>(nullptr))::value;
};

template <typename T>
struct is_after_handle_arity_3_impl {
  template <typename C>
  static std::true_type f(
      typename check_after_handle_arity_3_const<T>::template get<C>*);

  template <typename C>
  static std::true_type f(
      typename check_after_handle_arity_3<T>::template get<C>*);

  template <typename C>
  static std::false_type f(...);

 public:
  static const bool value = decltype(f<T>(nullptr))::value;
};

template <typename MW, typename Context, typename ParentContext>
typename std::enable_if<!is_before_handle_arity_3_impl<MW>::value>::type
before_handler_call(MW& mw, request& req, response& res, Context& ctx,
                    ParentContext& /*parent_ctx*/) {
  mw.before_handle(req, res, ctx.template get<MW>(), ctx);
}

template <typename MW, typename Context, typename ParentContext>
typename std::enable_if<is_before_handle_arity_3_impl<MW>::value>::type
before_handler_call(MW& mw, request& req, response& res, Context& ctx,
                    ParentContext& /*parent_ctx*/) {
  mw.before_handle(req, res, ctx.template get<MW>());
}

template <typename MW, typename Context, typename ParentContext>
typename std::enable_if<!is_after_handle_arity_3_impl<MW>::value>::type
after_handler_call(MW& mw, request& req, response& res, Context& ctx,
                   ParentContext& /*parent_ctx*/) {
  mw.after_handle(req, res, ctx.template get<MW>(), ctx);
}

template <typename MW, typename Context, typename ParentContext>
typename std::enable_if<is_after_handle_arity_3_impl<MW>::value>::type
after_handler_call(MW& mw, request& req, response& res, Context& ctx,
                   ParentContext& /*parent_ctx*/) {
  mw.after_handle(req, res, ctx.template get<MW>());
}

template <int N, typename Context, typename Container, typename CurrentMW,
          typename... Middlewares>
bool middleware_call_helper(Container& middlewares, request& req, response& res,
                            Context& ctx) {
  using parent_context_t = typename Context::template partial<N - 1>;
  before_handler_call<CurrentMW, Context, parent_context_t>(
      std::get<N>(middlewares), req, res, ctx,
      static_cast<parent_context_t&>(ctx));

  if (res.is_completed()) {
    after_handler_call<CurrentMW, Context, parent_context_t>(
        std::get<N>(middlewares), req, res, ctx,
        static_cast<parent_context_t&>(ctx));
    return true;
  }

  if (middleware_call_helper<N + 1, Context, Container, Middlewares...>(
          middlewares, req, res, ctx)) {
    after_handler_call<CurrentMW, Context, parent_context_t>(
        std::get<N>(middlewares), req, res, ctx,
        static_cast<parent_context_t&>(ctx));
    return true;
  }

  return false;
}

template <int N, typename Context, typename Container>
bool middleware_call_helper(Container& /*middlewares*/, request& /*req*/,
                            response& /*res*/, Context& /*ctx*/) {
  return false;
}

template <int N, typename Context, typename Container>
typename std::enable_if<(N < 0)>::type after_handlers_call_helper(
    Container& /*middlewares*/, Context& /*context*/, request& /*req*/,
    response& /*res*/) {}

template <int N, typename Context, typename Container>
typename std::enable_if<(N == 0)>::type after_handlers_call_helper(
    Container& middlewares, Context& ctx, request& req, response& res) {
  using parent_context_t = typename Context::template partial<N - 1>;
  using CurrentMW = typename std::tuple_element<
      N, typename std::remove_reference<Container>::type>::type;
  after_handler_call<CurrentMW, Context, parent_context_t>(
      std::get<N>(middlewares), req, res, ctx,
      static_cast<parent_context_t&>(ctx));
}

template <int N, typename Context, typename Container>
typename std::enable_if<(N > 0)>::type after_handlers_call_helper(
    Container& middlewares, Context& ctx, request& req, response& res) {
  using parent_context_t = typename Context::template partial<N - 1>;
  using CurrentMW = typename std::tuple_element<
      N, typename std::remove_reference<Container>::type>::type;
  after_handler_call<CurrentMW, Context, parent_context_t>(
      std::get<N>(middlewares), req, res, ctx,
      static_cast<parent_context_t&>(ctx));
  after_handlers_call_helper<N - 1, Context, Container>(middlewares, ctx, req,
                                                        res);
}
}

#ifdef CROW_ENABLE_DEBUG
static int connectionCount;
#endif
template <typename Adaptor, typename Handler, typename... Middlewares>
class Connection {
 public:
  Connection(boost::asio::io_service& io_service, Handler* handler,
             const std::string& server_name,
             std::tuple<Middlewares...>* middlewares,
             std::function<std::string()>& get_cached_date_str_f,
             detail::dumb_timer_queue& timer_queue,
             typename Adaptor::context* adaptor_ctx_)
      : adaptor_(io_service, adaptor_ctx_),
        handler_(handler),
        parser_(this),
        server_name_(server_name),
        middlewares_(middlewares),
        get_cached_date_str(get_cached_date_str_f),
        timer_queue(timer_queue) {
#ifdef CROW_ENABLE_DEBUG
    connectionCount++;
    CROW_LOG_DEBUG << "Connection open, total " << connectionCount << ", "
                   << this;
#endif
  }

  ~Connection() {
    res.complete_request_handler_ = nullptr;
    cancel_deadline_timer();
#ifdef CROW_ENABLE_DEBUG
    connectionCount--;
    CROW_LOG_DEBUG << "Connection closed, total " << connectionCount << ", "
                   << this;
#endif
  }

  decltype(std::declval<Adaptor>().raw_socket()) & socket() {
    return adaptor_.raw_socket();
  }

  void start() {
    adaptor_.start([this](const boost::system::error_code& ec) {
      if (!ec) {
        start_deadline();

        do_read();
      } else {
        check_destroy();
      }
    });
  }

  void handle_header() {
    // HTTP 1.1 Expect: 100-continue
    if (parser_.check_version(1, 1) && parser_.headers.count("expect") &&
        get_header_value(parser_.headers, "expect") == "100-continue") {
      buffers_.clear();
      static std::string expect_100_continue = "HTTP/1.1 100 Continue\r\n\r\n";
      buffers_.emplace_back(expect_100_continue.data(),
                            expect_100_continue.size());
      do_write();
    }
  }

  void handle() {
    cancel_deadline_timer();
    bool is_invalid_request = false;
    add_keep_alive_ = false;

    req_ = std::move(parser_.to_request());
    request& req = req_;

    if (parser_.check_version(1, 0)) {
      // HTTP/1.0
      if (req.headers.count("connection")) {
        if (boost::iequals(req.get_header_value("connection"), "Keep-Alive")) {
          add_keep_alive_ = true;
        }
      } else
        close_connection_ = true;
    } else if (parser_.check_version(1, 1)) {
      // HTTP/1.1
      if (req.headers.count("connection")) {
        if (req.get_header_value("connection") == "close")
          close_connection_ = true;
        else if (boost::iequals(req.get_header_value("connection"),
                                "Keep-Alive")) {
          add_keep_alive_ = true;
        }
      }
      if (!req.headers.count("Host")) {
        is_invalid_request = true;
        res = response(400);
      }
      if (parser_.is_upgrade()) {
        if (req.get_header_value("upgrade") == "h2c") {
          // TODO HTTP/2
          // currently, ignore upgrade header
        } else {
          close_connection_ = true;
          handler_->handle_upgrade(req, res, std::move(adaptor_));
          return;
        }
      }
    }

    CROW_LOG_INFO << "Request: " << boost::lexical_cast<std::string>(
                                        adaptor_.remote_endpoint())
                  << " " << this << " HTTP/" << parser_.http_major << "."
                  << parser_.http_minor << ' ' << method_name(req.method) << " "
                  << req.url;

    need_to_call_after_handlers_ = false;

    if (!is_invalid_request) {
      res.complete_request_handler_ = [] {};
      res.is_alive_helper_ = [this]() -> bool { return adaptor_.is_open(); };

      ctx_ = detail::context<Middlewares...>();
      req.middleware_context = (void*)&ctx_;
      req.io_service = &adaptor_.get_io_service();
      detail::middleware_call_helper<0, decltype(ctx_), decltype(*middlewares_),
                                     Middlewares...>(*middlewares_, req, res,
                                                     ctx_);

      if (!res.completed_) {
        res.complete_request_handler_ = [this] { this->complete_request(); };
        need_to_call_after_handlers_ = true;
        handler_->handle(req, res);
        if (add_keep_alive_) res.add_header("connection", "Keep-Alive");
      } else {
        complete_request();
      }
    } else {
      complete_request();
    }
  }

  void complete_request() {
    CROW_LOG_INFO << "Response: " << this << ' ' << req_.raw_url << ' '
                  << res.code << ' ' << close_connection_;

    if (need_to_call_after_handlers_) {
      need_to_call_after_handlers_ = false;

      // call all after_handler of middlewares
      detail::after_handlers_call_helper<((int)sizeof...(Middlewares) - 1),
                                         decltype(ctx_),
                                         decltype(*middlewares_)>(
          *middlewares_, ctx_, req_, res);
    }

    // auto self = this->shared_from_this();
    res.complete_request_handler_ = nullptr;

    if (!adaptor_.is_open()) {
      // CROW_LOG_DEBUG << this << " delete (socket is closed) " << is_reading
      // << ' ' << is_writing;
      // delete this;
      return;
    }

    static boost::container::flat_map<int, std::string> statusCodes = {
        {200, "HTTP/1.1 200 OK\r\n"},
        {201, "HTTP/1.1 201 Created\r\n"},
        {202, "HTTP/1.1 202 Accepted\r\n"},
        {204, "HTTP/1.1 204 No Content\r\n"},

        {300, "HTTP/1.1 300 Multiple Choices\r\n"},
        {301, "HTTP/1.1 301 Moved Permanently\r\n"},
        {302, "HTTP/1.1 302 Moved Temporarily\r\n"},
        {304, "HTTP/1.1 304 Not Modified\r\n"},

        {400, "HTTP/1.1 400 Bad Request\r\n"},
        {401, "HTTP/1.1 401 Unauthorized\r\n"},
        {403, "HTTP/1.1 403 Forbidden\r\n"},
        {404, "HTTP/1.1 404 Not Found\r\n"},

        {500, "HTTP/1.1 500 Internal Server Error\r\n"},
        {501, "HTTP/1.1 501 Not Implemented\r\n"},
        {502, "HTTP/1.1 502 Bad Gateway\r\n"},
        {503, "HTTP/1.1 503 Service Unavailable\r\n"},
    };

    buffers_.clear();
    buffers_.reserve(20);

    if (res.body.empty() && !res.json_value.empty()) {
      res.body = res.json_value.dump();
    }

    if (!statusCodes.count(res.code)) res.code = 500;
    {
      auto& status = statusCodes.find(res.code)->second;
      buffers_.emplace_back(status.data(), status.size());
    }

    if (res.code >= 400 && res.body.empty())
      res.body = statusCodes[res.code].substr(9);

    const static std::string crlf = "\r\n";
    content_length_ = std::to_string(res.body.size());
    static const std::string content_length_tag = "Content-Length";
    res.add_header(content_length_tag, content_length_);

    static const std::string server_tag = "Server: ";
    res.add_header(server_tag, server_name_);

    static const std::string date_tag = "Date: ";
    date_str_ = get_cached_date_str();
    res.add_header(date_tag, date_str_);

    if (add_keep_alive_) {
      static const std::string keep_alive_tag = "Connection";
      static const std::string keep_alive_value = "Keep-Alive";
      res.add_header(keep_alive_tag, keep_alive_value);
    }

    buffers_.emplace_back(res.headers.data(), res.headers.size());

    buffers_.emplace_back(crlf.data(), crlf.size());
    buffers_.emplace_back(res.body.data(), res.body.size());

    do_write();

    if (need_to_start_read_after_complete_) {
      need_to_start_read_after_complete_ = false;
      start_deadline();
      do_read();
    }
  }

 private:
  void do_read() {
    // auto self = this->shared_from_this();
    is_reading = true;
    adaptor_.socket().async_read_some(
        boost::asio::buffer(buffer_),
        [this](const boost::system::error_code& ec,
               std::size_t bytes_transferred) {
          CROW_LOG_ERROR << "Read " << bytes_transferred << " Bytes";
          bool error_while_reading = true;
          if (!ec) {
            bool ret = parser_.feed(buffer_.data(), bytes_transferred);
            if (parser_.upgrade) {
              error_while_reading = false;
            } else {
              if (ret && adaptor_.is_open()) {
                error_while_reading = false;
              }
            }
          } else {
            CROW_LOG_ERROR << "Error while reading: " << ec.message();
            if (ec.category() == boost::asio::error::get_ssl_category()) {
              auto err =
                  std::string(" (") +
                  boost::lexical_cast<std::string>(ERR_GET_LIB(ec.value())) +
                  "," +
                  boost::lexical_cast<std::string>(ERR_GET_FUNC(ec.value())) +
                  "," +
                  boost::lexical_cast<std::string>(ERR_GET_REASON(ec.value())) +
                  ") ";
              // ERR_PACK /* crypto/err/err.h */
              char buf[128];
              ::ERR_error_string_n(ec.value(), buf, sizeof(buf));
              err += buf;
              CROW_LOG_ERROR << err;
            }
          }
          if (error_while_reading) {
            cancel_deadline_timer();
            parser_.done();
            adaptor_.close();
            is_reading = false;
            CROW_LOG_DEBUG << this << " from read(1)";
            check_destroy();
          } else if (close_connection_) {
            cancel_deadline_timer();
            parser_.done();
            is_reading = false;
            check_destroy();
            // adaptor will close after write
          } else if (!need_to_call_after_handlers_) {
            start_deadline();
            do_read();
          } else {
            // res will be completed later by user
            need_to_start_read_after_complete_ = true;
          }
        });
  }

  void do_write() {
    // auto self = this->shared_from_this();
    is_writing = true;
    CROW_LOG_DEBUG << "Doing Write";
    boost::asio::async_write(
        adaptor_.socket(), buffers_, [&](const boost::system::error_code& ec,
                                         std::size_t bytes_transferred) {
          CROW_LOG_DEBUG << "Wrote " << bytes_transferred << "bytes";

          is_writing = false;
          res.clear();
          if (!ec) {
            if (close_connection_) {
              adaptor_.close();
              CROW_LOG_DEBUG << this << " from write(1)";
              check_destroy();
            }
          } else {
            CROW_LOG_DEBUG << this << " from write(2)";
            check_destroy();
          }
        });
  }

  void check_destroy() {
    CROW_LOG_DEBUG << this << " is_reading " << is_reading << " is_writing "
                   << is_writing;
    if (!is_reading && !is_writing) {
      CROW_LOG_DEBUG << this << " delete (idle) ";
      delete this;
    }
  }

  void cancel_deadline_timer() {
    CROW_LOG_DEBUG << this << " timer cancelled: " << timer_cancel_key_.first
                   << ' ' << timer_cancel_key_.second;
    timer_queue.cancel(timer_cancel_key_);
  }

  void start_deadline(/*int timeout = 5*/) {
    cancel_deadline_timer();

    timer_cancel_key_ = timer_queue.add([this] {
      if (!adaptor_.is_open()) {
        return;
      }
      adaptor_.close();
    });
    CROW_LOG_DEBUG << this << " timer added: " << timer_cancel_key_.first << ' '
                   << timer_cancel_key_.second;
  }

 private:
  Adaptor adaptor_;
  Handler* handler_;

  boost::array<char, 4096> buffer_;

  HTTPParser<Connection> parser_;
  request req_;
  response res;

  bool close_connection_ = false;

  const std::string& server_name_;
  std::vector<boost::asio::const_buffer> buffers_;

  std::string content_length_;
  std::string date_str_;

  // boost::asio::deadline_timer deadline_;
  detail::dumb_timer_queue::key timer_cancel_key_;

  bool is_reading{};
  bool is_writing{};
  bool need_to_call_after_handlers_{};
  bool need_to_start_read_after_complete_{};
  bool add_keep_alive_{};

  std::tuple<Middlewares...>* middlewares_;
  detail::context<Middlewares...> ctx_;

  std::function<std::string()>& get_cached_date_str;
  detail::dumb_timer_queue& timer_queue;
};
}
