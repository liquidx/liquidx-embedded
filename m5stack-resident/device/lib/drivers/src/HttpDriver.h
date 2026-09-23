#ifndef HTTP_DRIVER_H
#define HTTP_DRIVER_H

#include <cstdint>
#include <ResidentDriver.h>
#include <ResidentLuaModule.h>

// Resident driver for async HTTP requests.
//
// Lua API:
//   http.get(url)                        -> true, or false, "error msg"
//   http.post(url, body [, content_type]) -> true, or false, "error msg"
//   http.busy()                          -> bool
//   http.status()                        -> int  (last HTTP status code)
//   http.body()                          -> string (last response body)
//
// Requests run on a FreeRTOS task so the main loop is never blocked.
// When the request completes, on_event fires with:
//   event.name   == "http_response"
//   event.status == HTTP status code (or 0 on connection error)
//   event.ok     == 1 on HTTP success, 0 on failure
//
// Only one request may be in-flight at a time. Starting a new request
// while busy returns false + an error string to Lua.
class HttpDriver : public Resident::Driver {
public:
  const char* name() const override { return "http"; }
  void update() override;
  void onAppReset() override;
  void registerModule(Resident::LuaModule& m) override {
    m.method<HttpDriver, &HttpDriver::get>("get")
     .method<HttpDriver, &HttpDriver::post>("post")
     .method<HttpDriver, &HttpDriver::busy>("busy")
     .method<HttpDriver, &HttpDriver::status>("status")
     .method<HttpDriver, &HttpDriver::body>("body");
  }

  int get(lua_State* L);
  int post(lua_State* L);
  int busy(lua_State* L);
  int status(lua_State* L);
  int body(lua_State* L);

  // Public so the static httpTask helper in the .cpp can access them.
  enum class ReqState : uint8_t { IDLE, PENDING, DONE, ERROR };
  static constexpr size_t BODY_BUF_SIZE = 8192;

  ReqState _state    = ReqState::IDLE;
  char _bodyBuf[BODY_BUF_SIZE] = {};
  int  _statusCode   = 0;
  bool _isPost       = false;
  char _contentType[64]        = {};

private:
  static constexpr size_t URL_BUF_SIZE  = 512;
  static constexpr size_t POST_BUF_SIZE = 2048;
  static constexpr int    TASK_STACK    = 12288;

  char _url[URL_BUF_SIZE]       = {};
  char _postBody[POST_BUF_SIZE] = {};

  void startRequest();
  static void httpTask(void* arg);
  friend void doRequest(HttpDriver*, class HTTPClient&);
};

#endif // HTTP_DRIVER_H
