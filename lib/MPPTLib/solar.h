#pragma once
#include "publishable.h"
#include <WString.h>
#include <PubSubClient.h>
#include <WebServer.h>

class PowerSupply;
struct LowVoltageProtect;

struct DBConnection {
  String serv, user, pass, feed;
  PubSubClient client;
  int32_t period = 1000;
  int getPort() const;
  String getEndpoint() const;
};

struct SPoint {
  double v, i, input; bool collapsed;
  String toString() const;
  double p() const { return v * i; }
};

class Solar {
public:
  Solar(String version);
  ~Solar();
  void setup();
  String setLVProtect(String);
  String setPSU(String);

  void loop();
  void sendOutgoingLogs();
  void publishTask();
  void doConnect();
  void applyAdjustment();
  void printStatus();
  void startSweep();
  void doSweepStep();
  bool hasCollapsed() const;
  int getCollapses() const;
  void doUpdate(String url);

  int getBackoff(int period) const;
  void setState(const String state, String reason="");

  const String version_;
  String state_;
  int pinInvolt_ = 32;
  float inVolt_ = 0;
  double setpoint_ = 0, pgain_ = 0.005, ramplimit_ = 2;
  double currentCap_ = 8.5;
  CircularArray<uint32_t, 32> collapses_;
  int measperiod_ = 200, printPeriod_ = 1000, adjustPeriod_ = 2000;
  int autoSweep_ = 10 * 60; //every 10m
  float vadjust_ = 116.50;
  CircularArray<SPoint, 10> sweepPoints_; //size here is important, larger == more stable setpoint
  String wifiap, wifipass;
  uint32_t lastConnected_ = 0;
  int8_t backoffLevel_ = 0;
  std::unique_ptr<LowVoltageProtect> lvProtect_;

  std::unique_ptr<PowerSupply> psu_;
  WebServer server_;
  Publishable pub_;
  DBConnection db_;
};

#define STATE(x) static constexpr const char* x = #x

struct States {
  STATE(error);
  STATE(off);
  STATE(mppt);
  STATE(sweeping);
  STATE(full_cv);
  STATE(capped);
  STATE(collapsemode);
};


struct LowVoltageProtect {
  uint8_t pin_ = 22;
  float threshold_ = 12.0;
  float threshRecovery_ = 13.0;
  bool invert_ = false;
  uint32_t nextCheck_ = 0;
  String toString() const;
  LowVoltageProtect(String configuration);
  void init();
  void trigger(bool trigger=true);
  bool isTriggered() const;
};
