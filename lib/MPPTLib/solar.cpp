#include "solar.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <ESPmDNS.h>
#include <Update.h>

FreeTheSun::FreeTheSun() :
        psu_(Serial1),
        server_(80),
        pub_() { }

void runLoop(void*c) { ((FreeTheSun*)c)->loopTask(); }
void runPubt(void*c) { ((FreeTheSun*)c)->publishTask(); }

void FreeTheSun::run() {
  Serial.begin(115200);
  Serial.setTimeout(10); //very fast, need to keep the ctrl loop running
  delay(100);
  uint64_t chipid = ESP.getEfuseMac();
  Serial.printf("startup, ID %08X %04X\n", chipid, (uint16_t)(chipid >> 32));
  Serial2.begin(4800, SERIAL_8N1, 16, 17, false, 1000);
  analogSetCycles(32);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
    Serial.println("wifi event");
  });
  pub_.add("wifiap",     wifiap).hide().pref();
  pub_.add("wifipass", wifipass).hide().pref();
  pub_.add("mqttServ", db_.serv).hide().pref();
  pub_.add("mqttUser", db_.user).hide().pref();
  pub_.add("mqttPass", db_.pass).hide().pref();
  pub_.add("mqttFeed", db_.feed).hide().pref();
  pub_.add("outputEN",[=](String s){ if (s.length()) psu_.enableOutput(s == "on"); return String(psu_.outEn_); });
  pub_.add("outvolt", [=](String s){ if (s.length()) psu_.setVoltage(s.toFloat()); return String(psu_.outVolt_); });
  pub_.add("outcurr", [=](String s){ if (s.length()) psu_.setCurrent(s.toFloat()); return String(psu_.outCurr_); });
  pub_.add("outpower",[=](String s){ return String(psu_.outVolt_ * psu_.outCurr_); });
  pub_.add("pgain",      pgain_          ).pref();
  pub_.add("setpoint",   setpoint_       ).pref();
  pub_.add("vadjust",    vadjust_        ).pref();
  pub_.add("autostart",  autoStart_      ).pref();
  pub_.add("printperiod",printPeriod_    ).pref();
  pub_.add("pubperiod",  db_.period      ).pref();
  pub_.add("PSUperiod",  psuperiod_      ).pref();
  pub_.add("measperiod", measperiod_     ).pref();
  pub_.add("involt",  inVolt_);
  pub_.add("wh",      wh_    );
  pub_.add("collapses",  collapses_);
  pub_.add("connect",[=](String s){ doConnect(); return "connected"; }).hide();
  pub_.add("disconnect",[=](String s){ db_.client.disconnect(); WiFi.disconnect(); return "dissed"; }).hide();
  pub_.add("restart",[](String s){ ESP.restart(); return ""; }).hide();
  pub_.add("clear",[=](String s){ pub_.clearPrefs(); return "cleared"; }).hide();

  server_.on("/", HTTP_ANY, [=]() {
    Serial.println("got req " + server_.uri() + " -> " + server_.hostHeader());
    String ret;
    for (int i = 0; i < server_.args(); i++)
      ret += pub_.handleSet(server_.argName(i), server_.arg(i)) + "\n";
    server_.sendHeader("Connection", "close");
    if (! ret.length()) ret = pub_.toJson();
    server_.send(200, "application/json", ret.c_str());
  });
  pub_.loadPrefs();
  // wifi & mqtt is connected by pubsubConnect below

  psu_.flush();
  psu_.doUpdate();

  //fn, name, stack size, parameter, priority, handle
  xTaskCreate(runLoop,    "loop", 10000, this, 1, NULL);
  xTaskCreate(runPubt, "publish", 10000, this, 1, NULL);
  Serial.println("finished setup");
}

void FreeTheSun::doConnect() {
  if (! WiFi.isConnected()) {
    if (wifiap.length() && wifipass.length()) {
      WiFi.begin(wifiap.c_str(), wifipass.c_str());
      uint64_t chipid = ESP.getEfuseMac();
      String hostname = str("mpptESP-%02X", chipid & 0xff);
      WiFi.setHostname(hostname.c_str());
      if (WiFi.waitForConnectResult() == WL_CONNECTED) {
        Serial.println("Wifi connected! hostname: " + hostname);
        MDNS.begin("mppt");
        MDNS.addService("http", "tcp", 80);
        server_.begin();
      }
    } else Serial.println("no wifiap or wifipass set!");
  }
  if (WiFi.isConnected() && !db_.client.connected()) {
    if (db_.serv.length() && db_.feed.length()) {
      Serial.println("Connecting MQTT to " + db_.user + "@" + db_.serv);
      db_.client.setServer(db_.serv.c_str(), 1883); //TODO split serv:port
      if (db_.client.connect("MPPT", db_.user.c_str(), db_.pass.c_str()))
        Serial.println("PubSub connect success! " + db_.client.state());
        auto pubs = pub_.items(true);
        for (auto i : pubs)
          if (i->pref_)
            db_.client.subscribe((db_.feed + "/prefs/" + i->key).c_str()); //subscribe to preference changes!
      else Serial.println("PubSub connect ERROR! " + db_.client.state());
    } else Serial.println("no MQTT user / pass / server / feed set up!");
  } else Serial.printf("can't pub connect, wifi %d pub %d\n", WiFi.isConnected(), db_.client.connected());
}

uint32_t lastV = 0, lastpub = 20000, lastLog_ = 0;
uint32_t lastPSUpdate_ = 0, lastPSUadjust_ = 1000, lastCollapseReset_ = 0;
double newDesiredCurr_ = 0;
bool needsQuickAdj_ = false;
String logme;

void FreeTheSun::applyAdjustment() {
  if (newDesiredCurr_ > 0) {
    if (psu_.setCurrent(newDesiredCurr_))
      logme += str("[adjusting %0.1fA (from %0.1fA)] ", newDesiredCurr_ - psu_.outCurr_, psu_.outCurr_);
    else Serial.println("error setting current");
    psu_.readCurrent();
    pub_.setDirty({"outcurr", "outpower"});
    if (needsQuickAdj_)
      needsQuickAdj_ = false;
    printStatus();
  }
  newDesiredCurr_ = 0;
}

void FreeTheSun::loopTask() {
  uint32_t now = millis();
  if ((now - lastV) >= measperiod_) {
    int analogval = analogRead(pinInvolt_);
    inVolt_ = analogval * 3.3 * (vadjust_ / 3.3) / 4096.0;
    pub_.setDirty(&inVolt_);
    if (setpoint_ > 0 && psu_.outEn_) { //corrections enabled
      double error = inVolt_ - setpoint_;
      double dcurr = constrain(error * pgain_, -3, 1); //limit ramping speed
      if (error > 0.3 || (-error > 0.2)) { //adjustment deadband, more sensitive when needing to ramp down
        newDesiredCurr_ = psu_.outCurr_ + dcurr;
        if (error < 0.6) { //ramp down, quick!
          logme += "[QUICK] ";
          needsQuickAdj_ = true;
        }
      }
    }
    lastV = millis();
  }
  if ((now - lastPSUadjust_) >= (needsQuickAdj_? 100 : psuperiod_)) {
    if (setpoint_ > 0) {
      if (psu_.outEn_)
        applyAdjustment();
      if (psu_.outEn_ && inVolt_ < (psu_.outVolt_ * 3 / 2)) { //collapse detection
        newDesiredCurr_ = psu_.outCurr_ / 2;
        ++collapses_;
        pub_.setDirty(&collapses_);
        Serial.printf("collapsed! %0.1fV set recovery to %0.1fA\n", inVolt_ ,newDesiredCurr_);
        psu_.enableOutput((psu_.outEn_ = false));
        psu_.setCurrent(newDesiredCurr_);
      } else if (autoStart_ && !psu_.outEn_ && inVolt_ > (setpoint_ * 1.02)) {
        Serial.println("restoring from collapse");
        psu_.enableOutput(true);
      }
    }
    lastPSUadjust_ = now;
  }
  if ((now - lastLog_) >= printPeriod_) {
    printStatus();
    lastLog_ = now;
  }
  if ((now - lastPSUpdate_) >= 5000) {
    bool res = psu_.doUpdate();
    if (res) {
      wh_ += psu_.outVolt_ * psu_.outCurr_ * (now - lastPSUpdate_) / 1000.0 / 60 / 60;
      pub_.setDirty({"outvolt", "outcurr", "outputEN", "wh", "outpower"});
    } else {
      Serial.println("psu update fail");
      psu_.flush();
      //psu_.debug_ = true;
    }
    lastPSUpdate_ = now;
  }
}

void FreeTheSun::publishTask() {
  doConnect();
  db_.client.loop();
  ignoreSubsUntil_ = millis() + 3000;
  db_.client.setCallback([=](char*topicbuf, uint8_t*buf, unsigned int len){
    String topic(topicbuf), val = str(std::string((char*)buf, len));
    Serial.println("got sub value " + topic + " -> " + val);
    if (topic == (db_.feed + "/wh")) {
      wh_ = val.toFloat();
      Serial.println("restored wh value to " + val);
      db_.client.unsubscribe((db_.feed + "/wh").c_str());
    } else if (millis() > ignoreSubsUntil_) { //don't load old values
      topic.replace(db_.feed + "/prefs/", ""); //replaces in-place, sadly
      String ret = pub_.handleSet(topic, val);
      Serial.println("MQTT cmd: " + ret);
    }
  });
  db_.client.subscribe((db_.feed + "/wh").c_str());

  while (true) {
    uint32_t now = millis();
    if ((now - lastpub) >= (psu_.outEn_? db_.period : db_.period * 4)) { //slow-down when not enabled
      if (db_.client.connected()) {
        int wins = 0;
        auto pubs = pub_.items(true);
        for (auto i : pubs) {
          wins += db_.client.publish((db_.feed + "/" + (i->pref_? "prefs/":"") + i->key).c_str(), i->toString().c_str(), true)? 1 : 0;
          if (i->pref_) ignoreSubsUntil_ = now + 3000;
        }
        logme += str("[published %d] ", wins);
        pub_.clearDirty();
      } else {
        logme += "[pub disconnected] ";
        doConnect();
      }
      lastpub = now;
    }
    if ((now - lastCollapseReset_) >= 60000) {
      collapses_ = 0;
      pub_.setDirty(&collapses_);
      lastCollapseReset_ = now;
    }
    db_.client.loop();
    pub_.poll(&Serial);
    server_.handleClient();
    delay(1);
  }
}

void FreeTheSun::printStatus() {
  Serial.println(str("%0.1fVin -> %0.2fWh <%0.1fV out %0.1fA %den> ", inVolt_, wh_, psu_.outVolt_, psu_.outCurr_, psu_.outEn_) + logme);
  logme = "";
}
