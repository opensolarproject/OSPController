#pragma once
#include <Arduino.h>
#include <map>

class PubSubClient;
class WebServer;

#define DEFAULT_PERIOD -1

struct PubItem {
  String key;
  void* value;
  uint8_t type;
  int period;
  String toString() const;
};

class Publishable {
public:
  Publishable(WebServer *, PubSubClient *); //TODO serial cmdline client

  //TODO support websockets client publishing
  //TODO support std::function client
  //TODO support publishing cap in msgs/minute

  PubItem& add(String name, double &, int pubPeriod = DEFAULT_PERIOD);
  PubItem& add(String name, float &, int pubPeriod = DEFAULT_PERIOD);
  PubItem& add(String name, int &, int pubPeriod = DEFAULT_PERIOD);
  PubItem& add(String name, bool &, int pubPeriod = DEFAULT_PERIOD);
  PubItem& add(String name, String &, int pubPeriod = DEFAULT_PERIOD);

  void poll();
  String toJson() const;

private:
  PubItem& add(String, uint8_t, void*, int);
  std::map<String, PubItem> items_;
  WebServer* webServer_;
  PubSubClient* pubSub_;
  int defaultPeriod_ = 12000;
};

