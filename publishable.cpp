#include "Arduino.h"
#include "publishable.h"
#include <WebServer.h>
#include <PubSubClient.h>

String PubItem::toString() const {
  switch (type) {
    case 0: return String(*((double*)value));
    case 1: return String(*((float*)value));
    case 2: return String(*((int*)value));
    case 3: return *((bool*)value)? "true":"false";
    case 4: return "\"" + *((String*)value) + "\""; //TODO escaping
  }
  return "<toString error>";
}

Publishable::Publishable(WebServer* w, PubSubClient* p) : webServer_(w), pubSub_(p) {

}

PubItem& Publishable::add(String k, uint8_t t, void* v, int p) {
  items_[k] = PubItem({k, v, t, (p==DEFAULT_PERIOD)? defaultPeriod_ : p});
  return items_[k];
}
PubItem& Publishable::add(String key, double &v, int period) { return add(key, 0, (void*)&v, period); }
PubItem& Publishable::add(String key, float  &v, int period) { return add(key, 1, (void*)&v, period); }
PubItem& Publishable::add(String key, int    &v, int period) { return add(key, 2, (void*)&v, period); }
PubItem& Publishable::add(String key, bool   &v, int period) { return add(key, 3, (void*)&v, period); }
PubItem& Publishable::add(String key, String &v, int period) { return add(key, 4, (void*)&v, period); }

void Publishable::poll() {

}

String Publishable::toJson() const {
  String ret = "{\n";
  for (const auto & i : items_)
    ret += "  \"" + i.first + "\":" + i.second.toString() + ",\n";
  if (items_.size())
    ret.remove(ret.length() - 2, 2); //remove trailing comma + LF
  return ret + "}\n";
}
