#include "publishable.h"
#include "utils.h"
#include <Arduino.h>
#include <Preferences.h>

template<typename T>
struct Pub : PubItem {
  T value;
  Pub(String k, T v, int p) : PubItem(k,p), value(v) { }
  ~Pub() { }
  String toString() const override { return String(*value); }
  String set(String v) override { *value = v.toFloat(); return toString(); }
  void save(Preferences&p) override { p.putBytes(key.c_str(), value, sizeof(*value)); }
  void load(Preferences&p) override { p.getBytes(key.c_str(), value, sizeof(*value)); }
};

template<> String Pub<bool* >::toString() const { return (*value)? "on":"off"; }
template<> String Pub<Action>::toString() const { return "action"+key; }
template<> String Pub<bool* >::set(String v) { *value = v.indexOf("on") > 0; return toString(); } //TODO support '1' && 'true'
template<> String Pub<Action>::set(String v) { return (value)(v); }
template<> void Pub<String*>::save(Preferences&p) { p.putString(key.c_str(), *value); }
template<> void Pub<String*>::load(Preferences&p) { p.getString(key.c_str(), *value); }
template<> void Pub<Action>::save(Preferences&p) { }
template<> void Pub<Action>::load(Preferences&p) { }

Publishable::Publishable() {
  add("save", [this](String s){
    return str("saved %d prefs", this->savePrefs());
  });
  add("load", [this](String s){
    return str("loaded %d prefs", this->loadPrefs());
  });
  Action a = [this](String s){
    String ret = "help:";
    for (auto i : items_)
      ret += "\n- " + i.first + " = " + i.second->toString();
    return ret;
  };
  add("help", a);
  add("list", a);
}

PubItem& Publishable::add(PubItem* p) { items_[p->key] = p; return *p; }
PubItem& Publishable::add(String k, double &v, int p) { return add(new Pub<double*>(k,&v,p)); }
PubItem& Publishable::add(String k, float  &v, int p) { return add(new Pub<float*> (k,&v,p)); }
PubItem& Publishable::add(String k, int    &v, int p) { return add(new Pub<int*>   (k,&v,p)); }
PubItem& Publishable::add(String k, bool   &v, int p) { return add(new Pub<bool*>  (k,&v,p)); }
PubItem& Publishable::add(String k, String &v, int p) { return add(new Pub<String*>(k,&v,p)); }
PubItem& Publishable::add(String k, Action  v, int p) { return add(new Pub<Action >(k, v,p)); }

int Publishable::loadPrefs() {
  Preferences prefs; //destructor calls end()
  prefs.begin("Publishable", true); //read only
  int ret = 0;
  for (const auto & i : items_)
    if (i.second->pref_) {
      i.second->load(prefs);
      Serial.println("loaded key " + i.first + " to " + i.second->toString());
      ret++;
    }
  return ret;
}
int Publishable::savePrefs() {
  Preferences prefs;
  prefs.begin("Publishable", false); //read-write
  int ret = 0;
  for (const auto & i : items_)
    if (i.second->pref_) {
      i.second->save(prefs);
      Serial.println("saved key " + i.first + " to " + i.second->toString());
      ret++;
    }
  return ret;
}

String Publishable::handleSet(String key, String val) {
  for (auto i : items_)
    if (i.first == key) {
      String ret = i.second->set(val);
      return (ret.length())? ret : ("set " + key + " to " + val);
    }
  return "unknown key " + key;
}

std::list<PubItem const*> Publishable::items() const {
  std::list<PubItem const*> ret;
  for (const auto & i : items_)
    ret.push_back(i.second);
  return ret;
}

void Publishable::poll(Stream* stream) {
  if (stream->available()) { //cmd val
    String cmd = stream->readStringUntil(' ');
    cmd.trim();
    const String val = stream->readStringUntil('\n');

    String ret = handleSet(cmd, val);
    stream->println("<" + cmd + " " + val + "> " + ret);
    while (stream->available())
      stream->read();
  }
}

String Publishable::toJson() const {
  String ret = "{\n";
  for (const auto & i : items_)
    if (!i.second->hidden_)
      ret += "  \"" + i.first + "\":" + i.second->toString() + ",\n";
  if (items_.size())
    ret.remove(ret.length() - 2, 2); //remove trailing comma + LF
  return ret + "}\n";
}
