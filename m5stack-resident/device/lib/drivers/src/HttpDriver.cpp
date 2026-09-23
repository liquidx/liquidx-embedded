#include "HttpDriver.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

extern "C" {
  #include "lua/lua.h"
  #include "lua/lauxlib.h"
}

// Reject any URL whose scheme is not http:// or https://.
// This blocks file://, ftp://, and other non-HTTP vectors before the
// request is ever queued. Full RFC1918 blocking would require DNS
// resolution at validation time, which is expensive on embedded and
// is deferred to a future hardening pass.
static bool isAllowedScheme(const char* url) {
  return strncmp(url, "http://",  7) == 0
      || strncmp(url, "https://", 8) == 0;
}

void HttpDriver::update() {
  if (_state != ReqState::DONE && _state != ReqState::ERROR) return;

  bool ok = (_state == ReqState::DONE);
  _state = ReqState::IDLE;

  Resident::EventField fields[] = {
    { "status", Resident::EventField::INT, { .i = _statusCode } },
    { "ok",     Resident::EventField::INT, { .i = ok ? 1 : 0  } },
  };
  sendEvent("http_response", fields, 2);
}

void HttpDriver::onAppReset() {
  if (_state == ReqState::DONE || _state == ReqState::ERROR) {
    _state = ReqState::IDLE;
  }
}

// http.get(url) -> true  |  false, "error"
int HttpDriver::get(lua_State* L) {
  if (_state == ReqState::PENDING) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "request in flight");
    return 2;
  }
  const char* url = luaL_checkstring(L, 1);
  if (!isAllowedScheme(url)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "url must use http:// or https://");
    return 2;
  }
  strncpy(_url, url, URL_BUF_SIZE - 1);
  _url[URL_BUF_SIZE - 1] = '\0';
  _isPost = false;
  startRequest();
  lua_pushboolean(L, true);
  return 1;
}

// http.post(url, body [, content_type]) -> true  |  false, "error"
int HttpDriver::post(lua_State* L) {
  if (_state == ReqState::PENDING) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "request in flight");
    return 2;
  }
  const char* url = luaL_checkstring(L, 1);
  if (!isAllowedScheme(url)) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "url must use http:// or https://");
    return 2;
  }
  const char* b  = luaL_checkstring(L, 2);
  const char* ct = luaL_optstring(L, 3, "application/json");
  strncpy(_url, url, URL_BUF_SIZE - 1);
  _url[URL_BUF_SIZE - 1] = '\0';
  strncpy(_postBody, b, POST_BUF_SIZE - 1);
  _postBody[POST_BUF_SIZE - 1] = '\0';
  strncpy(_contentType, ct, sizeof(_contentType) - 1);
  _contentType[sizeof(_contentType) - 1] = '\0';
  _isPost = true;
  startRequest();
  lua_pushboolean(L, true);
  return 1;
}

// http.busy() -> bool
int HttpDriver::busy(lua_State* L) {
  lua_pushboolean(L, _state == ReqState::PENDING ? 1 : 0);
  return 1;
}

// http.status() -> int
int HttpDriver::status(lua_State* L) {
  lua_pushinteger(L, _statusCode);
  return 1;
}

// http.body() -> string
int HttpDriver::body(lua_State* L) {
  lua_pushstring(L, _bodyBuf);
  return 1;
}

void HttpDriver::startRequest() {
  _state = ReqState::PENDING;
  _bodyBuf[0] = '\0';
  _statusCode = 0;
  xTaskCreate(httpTask, "http_req", TASK_STACK, this, 1, nullptr);
}

// Shared helper: run the HTTP request and store result on self.
// Called from the FreeRTOS task, not the main loop.
void doRequest(HttpDriver* self, HTTPClient& http) {
  http.setTimeout(10000);
  int code;
  if (self->_isPost) {
    http.addHeader("Content-Type", self->_contentType);
    code = http.POST(self->_postBody);
  } else {
    code = http.GET();
  }
  self->_statusCode = code;
  if (code > 0) {
    String resp = http.getString();
    strncpy(self->_bodyBuf, resp.c_str(), HttpDriver::BODY_BUF_SIZE - 1);
    self->_bodyBuf[HttpDriver::BODY_BUF_SIZE - 1] = '\0';
    self->_state = HttpDriver::ReqState::DONE;
  } else {
    self->_state = HttpDriver::ReqState::ERROR;
  }
  http.end();
}

void HttpDriver::httpTask(void* arg) {
  HttpDriver* self = static_cast<HttpDriver*>(arg);
  HTTPClient http;

  if (strncmp(self->_url, "https://", 8) == 0) {
    // Heap-allocate WiFiClientSecure to avoid blowing the task stack.
    WiFiClientSecure* client = new WiFiClientSecure();
#ifdef RESIDENT_HTTP_INSECURE_TLS
    // Explicitly opt-in to skipping cert validation via a build flag.
    // Do not enable in production — provision a root CA instead.
    client->setInsecure();
#endif
    http.begin(*client, self->_url);
    doRequest(self, http);
    delete client;
  } else {
    http.begin(self->_url);
    doRequest(self, http);
  }

  vTaskDelete(nullptr);
}
