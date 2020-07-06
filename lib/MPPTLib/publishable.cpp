#include "publishable.h"
#include "utils.h"
#include <WiFi.h>
#include <Preferences.h>

template<typename T>
struct Pub : PubItem {
  T value;
  Pub(String k, T v, int p) : PubItem(k,p), value(v) { }
  ~Pub() { }
  String toString() const override { return String(*value); }
  String jsonValue() const override { return toString(); }
  String set(String v) override { *value = v.toFloat(); return toString(); }
  void const* val() const override { return value; }
  void save(Preferences&p) override { p.putBytes(key.c_str(), value, sizeof(*value)); }
  void load(Preferences&p) override { p.getBytes(key.c_str(), value, sizeof(*value)); }
  bool isAction() const override { return false; }
};

String prefGetString(Preferences&p, String key) {
  char buf[128];
  size_t l = p.getBytes(key.c_str(), buf, 128);
  if (l == 0) return "";
  buf[l] = 0; //null terminate
  return String(buf);
}

template<> String Pub<double*>::toString() const { return String(*value, 3); }
template<> String Pub<bool* >::toString() const { return (*value)? "true":"false"; }
template<> String Pub<Action>::toString() const { return (value)(""); }
template<> String Pub<Action>::jsonValue() const { return "\"" + toString() + "\""; }
template<> String Pub<bool* >::set(String v) { (*value) = v=="on" || v=="true" || v=="1"; return toString(); }
template<> String Pub<Action>::set(String v) { return (value)(v); }
template<> void const* Pub<Action>::val() const { return &value; }

template<> bool Pub<Action>::isAction()const { return true; }
template<> void Pub<Action>::save(Preferences&p) { String v = (value)(""); p.putBytes(key.c_str(), v.c_str(), v.length()); }
template<> void Pub<Action>::load(Preferences&p) { String v = prefGetString(p, key); if (v.length()) try { (value)(v); } catch(...) { } }
template<> String Pub<String*>::set(String v) { return (*value) = v; }
template<> String Pub<String*>::toString() const { return (*value); }
template<> String Pub<String*>::jsonValue() const { return "\"" + toString() + "\""; }
template<> void Pub<String*>::save(Preferences&p) { p.putBytes(key.c_str(), value->c_str(), value->length()); }
template<> void Pub<String*>::load(Preferences&p) {
  (*value) = prefGetString(p, key);
}

Publishable::Publishable() : lock_(xSemaphoreCreateMutex()) {
  add("save", [this](String s){
    return str("saved %d prefs", this->savePrefs());
  }).hide();
  add("load", [this](String s){
    return str("loaded %d prefs", this->loadPrefs());
  }).hide();
  add("help", [this](String s){ printHelp(); return ""; }).hide();
  add("list", [this](String s){ printHelp(); return ""; }).hide();
}

void Publishable::log(const String &s) {
  Serial.println(s);
  if (xSemaphoreTake(lock_, (TickType_t) 100) == pdTRUE) {
    logPub_.push_back(s);
    xSemaphoreGive(lock_);
  }
}
bool Publishable::popLog(String *s) {
  if (xSemaphoreTake(lock_, (TickType_t) 100) == pdTRUE) {
    bool got = logPub_.size() > 0;
    if (got) (*s) = logPub_.pop_front();
    xSemaphoreGive(lock_);
    return got;
  }
  return false;
}
void Publishable::logNote(const String &s) {
  if (xSemaphoreTake(lock_, (TickType_t) 100) == pdTRUE) {
    logNote_ +=  " " + s;
    xSemaphoreGive(lock_);
  } else Serial.println("LOGNOTE couldn't get mutex! " + s);
}
String Publishable::popNotes() {
  if (xSemaphoreTake(lock_, (TickType_t) 100) == pdTRUE) {
    String ret = logNote_;
    logNote_ = "";
    xSemaphoreGive(lock_);
    return ret;
  }
  return "";
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

String Publishable::handleCmd(String cmd) {
  cmd.trim();
  int pivot = cmd.indexOf('=');
  if (pivot < 0) pivot = cmd.indexOf(' ');
  return handleSet(cmd.substring(0,pivot), cmd.substring(pivot + 1));
}

String Publishable::handleSet(String key, String val) {
  for (auto i : items_)
    if (i.first == key) {
      try {
        String ret = i.second->set(val);
        i.second->dirty_ = true;
        return (ret.length())? ret : ("set " + key + " to " + val);
      } catch (std::runtime_error e) {
        return "error setting '" + key + "' to '" + val + "': " + String(e.what());
      }
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
void Publishable::setDirty(std::list<String>dlist) { for (auto i : dlist) setDirty(i); }
void Publishable::setDirty(String key) {
  auto it = items_.find(key);
  if (it != items_.end()) it->second->dirty_ = true;
  else Serial.println("Pub::setDirty missing key" + key);
}
void Publishable::setDirtyAddr(void const* v) {
  for (const auto & i : items_)
    if (v == i.second->val())
      { i.second->dirty_ = true; return; }
  Serial.printf("Pub::setDirty missing addr %p\n", v);
}

void Publishable::poll(Stream* stream) {
  static String buff;
  if (stream->available()) { //cmd val
    buff += stream->readString();
    int end = -1;
    while ((end = buff.indexOf('\n')) > 0) {
      stream->println(handleCmd(buff.substring(0, end))); //TODO - 1 to exclude newline?
      buff = buff.substring(end + 1);
      buff.trim();
    }
  }
}

void Publishable::printHelp() const {
  Serial.println("help:");
  std::list<PubItem const*> sorted;
  for (auto i : items_)
    if (i.second->pref_) sorted.push_front(i.second);
    else sorted.push_back(i.second);
  for (auto i : sorted)
    if (i->isAction()) Serial.println("- " + i->key + " [action]");
    else Serial.println("- " + i->key + " = " + i->toString());
  if (WiFi.isConnected())
    Serial.println("** IP: " + WiFi.localIP().toString());
}

String Publishable::toJson() const {
  String ret = "{\n";
  for (const auto & i : items_)
    if (!i.second->hidden_ && !i.second->pref_)
      ret += "  \"" + i.first + "\":" + i.second->jsonValue() + ",\n";
  ret += "\"prefs\":{\n";
  for (const auto & i : items_)
    if (!i.second->hidden_ && i.second->pref_)
      ret += "    \"" + i.first + "\":" + i.second->jsonValue() + ",\n";
  ret.remove(ret.length() - 2, 2); //remove trailing comma + LF
  ret += "  }\n";
  return ret + "\n}\n";
}
