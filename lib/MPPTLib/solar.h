#pragma once
#include <Arduino.h>
#include "utils.h"
#include "publishable.h"
#include <PubSubClient.h>
#include <WebServer.h>

struct DBConnection {
  String serv, user, pass, feed;
  //TODO support set of allowed keys
  PubSubClient client;
  int32_t period = 1000;
};

class FreeTheSun {
public:
  FreeTheSun();
  void setup();
  void loop();
  void publishTask();
  void doConnect();
  void applyAdjustment();
  void printStatus();

  uint8_t pinInvolt_ = 36;
  float inVolt_ = 0, wh_ = 0;
  double setpoint_ = 0, pgain_ = 0.1;
  int collapses_ = 0; //collapses, reset every.. minute?
  int measperiod_ = 200, printPeriod_ = 1000, psuperiod_ = 2000;
  float vadjust_ = 116.50;
  bool autoStart_ = false;
  String wifiap, wifipass;
  uint32_t ignoreSubsUntil_ = 0;

  PowerSupply psu_;
  WebServer server_;
  Publishable pub_;
  DBConnection db_;
};

