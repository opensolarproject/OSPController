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
  void const* val() const override { return value; }
  void save(Preferences&p) override { p.putBytes(key.c_str(), value, sizeof(*value)); }
  void load(Preferences&p) override { p.getBytes(key.c_str(), value, sizeof(*value)); }
};

template<> String Pub<bool* >::toString() const { return (*value)? "true":"false"; }
template<> String Pub<Action>::toString() const { return (value)(""); }
template<> String Pub<bool* >::set(String v) { (*value) = v=="on" || v=="true" || v=="1"; return toString(); }
template<> String Pub<Action>::set(String v) { return (value)(v); }
template<> void const* Pub<Action>::val() const { return &value; }
  
template<> void Pub<Action>::save(Preferences&p) { }
template<> void Pub<Action>::load(Preferences&p) { }
template<> String Pub<String*>::set(String v) { return (*value) = v; }
template<> String Pub<String*>::toString() const { return (*value); }
template<> void Pub<String*>::save(Preferences&p) { p.putBytes(key.c_str(), value->c_str(), value->length()); }
template<> void Pub<String*>::load(Preferences&p) {
  char buf[128];
  size_t l = p.getBytes(key.c_str(), buf, 128);
  buf[l] = 0; //null terminate
  (*value) = String(buf);
}

Publishable::Publishable() {
  add("save", [this](String s){
    return str("saved %d prefs", this->savePrefs());
  }).hide();
  add("load", [this](String s){
    return str("loaded %d prefs", this->loadPrefs());
  }).hide();
  Action a = [this](String s){
    String ret = "help:";
    for (auto i : items_)
      ret += "\n- " + i.first + " = " + i.second->toString();
    return ret;
  };
  add("help", a).hide();
  add("list", a).hide();
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
      i.second->dirty_ = true;
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
      Serial.println("saved key " + i.first + " to " + i.second->toString() + str(" (%d free)", prefs.freeEntries()));
      ret++;
    }
  return ret;
}
bool Publishable::clearPrefs() {
  Preferences prefs;
  prefs.begin("Publishable", false); //read-write
  return prefs.clear();
}

String Publishable::handleSet(String key, String val) {
  for (auto i : items_)
    if (i.first == key) {
      String ret = i.second->set(val);
      i.second->dirty_ = true;
      return (ret.length())? ret : ("set " + key + " to " + val);
    }
  return "unknown key " + key;
}

std::list<PubItem const*> Publishable::items(bool dirtyOnly) const {
  std::list<PubItem const*> ret;
  for (const auto & i : items_)
    if (!dirtyOnly || i.second->dirty_)
      if (! i.second->hidden_)
        ret.push_back(i.second);
  return ret;
}

void Publishable::clearDirty() { for (auto &i : items_) i.second->dirty_ = false; }
void Publishable::setDirty(String key) { auto it = items_.find(key); if (it != items_.end()) it->second->dirty_ = true; }
void Publishable::setDirty(std::list<String>dlist) { for (auto i : dlist) setDirty(i); }
void Publishable::setDirty(void const* v) {
  for (const auto & i : items_)
    if (v == i.second->val())
      { i.second->dirty_ = true; return; }
  Serial.printf("Pub::setDirty missing addr %P\n", v);
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
  return ret + "\n}\n";
}
