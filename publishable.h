#pragma once
#include <Arduino.h>
#include <functional>
#include <map>

class PubSubClient;
class WebServer;
class Preferences;
typedef std::function<String(String)> Action;
typedef std::function<String()> StrFn;
typedef std::function<void(String)> SetFn;

#define DEFAULT_PERIOD -1

struct PubItem {
  String key;
  int period;
  bool pref_, hidden_;
  PubItem(String k, int p) : key(k), period(p), pref_(false), hidden_(false) { }
  virtual ~PubItem() { }
  virtual String toString() const = 0;
  virtual String set(String v) = 0;
  virtual void save(Preferences&) = 0;
  virtual void load(Preferences&) = 0;
  virtual PubItem& pref() { pref_ = true; return *this; }
  virtual PubItem& hide() { hidden_ = true; return *this; }
};

class Publishable {
public:
  Publishable();

  //TODO support publishing cap in msgs/minute

  PubItem& add(String name, double &, int pubPeriod = DEFAULT_PERIOD);
  PubItem& add(String name, float &, int pubPeriod = DEFAULT_PERIOD);
  PubItem& add(String name, int &, int pubPeriod = DEFAULT_PERIOD);
  PubItem& add(String name, bool &, int pubPeriod = DEFAULT_PERIOD);
  PubItem& add(String name, String &, int pubPeriod = DEFAULT_PERIOD);
  PubItem& add(String name, Action, int pubPeriod = DEFAULT_PERIOD);

  void poll(Stream*);
  String handleSet(String key, String val);
  String toJson() const;
  int loadPrefs();
  int savePrefs();

private:
  PubItem& add(PubItem*);
  std::map<String, PubItem*> items_;
  int defaultPeriod_ = 12000;
};

