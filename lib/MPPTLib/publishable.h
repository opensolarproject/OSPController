#pragma once
#include <Arduino.h>
#include <functional>
#include <map>
#include <list>
#include "utils.h"

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
  bool pref_, hidden_, dirty_;
  PubItem(String k, int p) : key(k), period(p), pref_(false), hidden_(false), dirty_(false) { }
  virtual ~PubItem() { }
  virtual String toString() const = 0;
  virtual String set(String v) = 0;
  virtual void const* val() const = 0;
  virtual void save(Preferences&) = 0;
  virtual void load(Preferences&) = 0;
  virtual PubItem& pref() { pref_ = true; return *this; }
  virtual PubItem& hide() { hidden_ = true; return *this; }
  virtual bool isAction() const = 0;
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
  String handleCmd(String cmd);
  String handleSet(String key, String val);
  String toJson() const;
  int loadPrefs();
  int savePrefs();
  bool clearPrefs();
  std::list<PubItem const*> items(bool dirtyOnly=true) const;
  void setDirty(String key);
  void setDirtyAddr(void const*);
  void setDirty(std::list<String>);
  void clearDirty();
  void printHelp() const;

  void log(const String &);
  bool popLog(String*);
  void logNote(const String &); //adds note to next status
  String popNotes();
  // void log(const char *fmtStr, ...);

private:
  PubItem& add(PubItem*);
  std::map<String, PubItem*> items_;
  int defaultPeriod_ = 12000;
  String logNote_;
  CircularArray<String, 16> logPub_;
  SemaphoreHandle_t lock_;
};

