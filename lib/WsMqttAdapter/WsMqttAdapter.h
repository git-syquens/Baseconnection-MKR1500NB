#pragma once

#include <Arduino.h>
#include <Client.h>
#include <ArduinoHttpClient.h>
#include <WebSocketClient.h>

// A thin adapter that exposes ArduinoHttpClient's WebSocketClient as a Client
// for use by 256dpi/arduino-mqtt. It frames writes as binary WS messages and
// buffers incoming WS frames for reads.
class WsMqttAdapter : public Client {
public:
  explicit WsMqttAdapter(WebSocketClient& ws) : _ws(ws), _rxPos(0) {}

  // Call from loop() regularly to parse incoming frames
  void poll() {
    if (_rxPos == _rxBuf.length()) {
      // buffer empty; try to parse the next WS message
      int sz = _ws.parseMessage();
      if (sz > 0) {
        _rxBuf.reserve(sz);
        _rxBuf = ""; // clear
        while (_ws.available()) {
          _rxBuf += (char)_ws.read();
        }
        _rxPos = 0;
      }
    }
  }

  // Client interface
  virtual int connect(IPAddress ip, uint16_t port) override { (void)ip; (void)port; return 0; }
  virtual int connect(const char *host, uint16_t port) override { (void)host; (void)port; return 0; }
  virtual void stop() override { _ws.stop(); }
  virtual uint8_t connected() override { return _ws.connected() ? 1 : 0; }
  virtual operator bool() override { return connected() == 1; }

  virtual size_t write(uint8_t b) override { return write(&b, 1); }
  virtual size_t write(const uint8_t *buf, size_t size) override {
    if (!_ws.connected()) return 0;
    if (_ws.beginMessage(TYPE_BINARY) != 0) return 0;
    size_t w = _ws.write(buf, size); // write into WS TX buffer
    if (_ws.endMessage() != 0) return 0;
    return w;
  }

  virtual int available() override {
    if (_rxPos < _rxBuf.length()) return (int)(_rxBuf.length() - _rxPos);
    // try to parse a new message lazily
    int sz = _ws.parseMessage();
    if (sz > 0) {
      _rxBuf.reserve(sz);
      _rxBuf = "";
      while (_ws.available()) { _rxBuf += (char)_ws.read(); }
      _rxPos = 0;
      return (int)(_rxBuf.length());
    }
    return 0;
  }

  virtual int read() override {
    if (_rxPos >= _rxBuf.length()) return -1;
    return (uint8_t)_rxBuf[_rxPos++];
  }

  virtual int read(uint8_t *buf, size_t size) override {
    int avail = available();
    if (avail <= 0) return 0;
    size_t n = min<size_t>(size, (size_t)avail);
    memcpy(buf, _rxBuf.c_str() + _rxPos, n);
    _rxPos += n;
    return (int)n;
  }

  virtual int peek() override {
    if (_rxPos >= _rxBuf.length()) return -1;
    return (uint8_t)_rxBuf[_rxPos];
  }

  virtual void flush() override {}

private:
  WebSocketClient& _ws;
  String _rxBuf;
  size_t _rxPos;
};
