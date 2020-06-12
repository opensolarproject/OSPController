#include "solar.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <ESPmDNS.h>
#include <Update.h>

WiFiClient espClient;

Solar::Solar() :
        psu_(Serial2),
        server_(80),
        pub_() {
  db_.client.setClient(espClient);
}

// void runLoop(void*c) { ((Solar*)c)->loopTask(); }
void runPubt(void*c) { ((Solar*)c)->publishTask(); }

//TODO: make these statics members instead
uint32_t lastV = 0, lastpub = 20000, lastLog_ = 0;
uint32_t lastPSUpdate_ = 0, lastPSUadjust_ = 1000;
uint32_t lastAutoSweep_ = 0;
double newDesiredCurr_ = 0;
bool needsQuickAdj_ = false;
String logme;

void Solar::setup() {
  Serial.begin(115200);
  Serial.setTimeout(10); //very fast, need to keep the ctrl loop running
  delay(100);
  uint64_t chipid = ESP.getEfuseMac();
  Serial.printf("startup, ID %08llX %04X\n", chipid, (uint16_t)(chipid >> 32));
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
  pub_.add("ramplimit",  ramplimit_      ).pref();
  pub_.add("setpoint",   setpoint_       ).pref();
  pub_.add("vadjust",    vadjust_        ).pref();
  pub_.add("autostart",  autoStart_      ).pref();
  pub_.add("printperiod",printPeriod_    ).pref();
  pub_.add("pubperiod",  db_.period      ).pref();
  pub_.add("PSUperiod",  psuperiod_      ).pref();
  pub_.add("measperiod", measperiod_     ).pref();
  pub_.add("autosweep",  autoSweep_      ).pref();
  pub_.add("currentcap", currentCap_     ).pref();
  pub_.add("involt",  inVolt_);
  pub_.add("wh",      wh_    );
  pub_.add("collapses", [=](String) { return String(getCollapses()); });
  pub_.add("sweep",[=](String){ startSweep(); return "starting sweep"; }).hide();
  pub_.add("connect",[=](String s){ doConnect(); return "connected"; }).hide();
  pub_.add("disconnect",[=](String s){ db_.client.disconnect(); WiFi.disconnect(); return "dissed"; }).hide();
  pub_.add("restart",[](String s){ ESP.restart(); return ""; }).hide();
  pub_.add("clear",[=](String s){ pub_.clearPrefs(); return "cleared"; }).hide();

  server_.on("/", HTTP_ANY, [=]() {
    log("got req " + server_.uri() + " -> " + server_.hostHeader());
    String ret;
    for (int i = 0; i < server_.args(); i++)
      ret += pub_.handleSet(server_.argName(i), server_.arg(i)) + "\n";
    server_.sendHeader("Connection", "close");
    if (! ret.length()) ret = pub_.toJson();
    server_.send(200, "application/json", ret.c_str());
  });
  pub_.loadPrefs();
  // wifi & mqtt is connected by pubsubConnect below

  psu_.begin();

  //fn, name, stack size, parameter, priority, handle
  // xTaskCreate(runLoop,    "loop", 10000, this, 1, NULL);
  xTaskCreate(runPubt, "publish", 10000, this, 1, NULL);
  Serial.println("finished setup");
}

void Solar::doConnect() {
  if (! WiFi.isConnected()) {
    if (wifiap.length() && wifipass.length()) {
      WiFi.begin(wifiap.c_str(), wifipass.c_str());
      uint64_t chipid = ESP.getEfuseMac();
      String hostname = str("mpptESP-%02X", chipid & 0xff);
      WiFi.setHostname(hostname.c_str());
      if (WiFi.waitForConnectResult() == WL_CONNECTED) {
        Serial.println("Wifi connected! hostname: " + hostname);
        Serial.println("IP: " + WiFi.localIP().toString());
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
      if (db_.client.connect("MPPT", db_.user.c_str(), db_.pass.c_str())) {
        Serial.println("PubSub connect success! " + db_.client.state());
        auto pubs = pub_.items(true);
        for (auto i : pubs)
          if (i->pref_)
            db_.client.subscribe((db_.feed + "/prefs/" + i->key).c_str()); //subscribe to preference changes
      } else Serial.println("PubSub connect ERROR! " + db_.client.state());
    } else Serial.println("no MQTT user / pass / server / feed set up!");
  } else Serial.printf("can't pub connect, wifi %d pub %d\n", WiFi.isConnected(), db_.client.connected());
}

void Solar::applyAdjustment() {
  if (newDesiredCurr_ != psu_.limitCurr_) {
    if (psu_.setCurrent(newDesiredCurr_))
      logme += str("[adjusting %0.2fA (from %0.2fA)] ", newDesiredCurr_ - psu_.limitCurr_, psu_.limitCurr_);
    else log("error setting current");
    psu_.readCurrent();
    pub_.setDirty({"outcurr", "outpower"});
    needsQuickAdj_ = false;
    printStatus();
  }
}

void Solar::startSweep() {
  log(str("SWEEP START c=%0.3f, (setpoint was %0.3f)\n", newDesiredCurr_, setpoint_));
  sweeping_ = true;
  lastAutoSweep_ = millis();
}

void Solar::doSweepStep() {
  newDesiredCurr_ = psu_.limitCurr_ + 0.02; //TODO maybe add a pref for sweep speed or use pgain?
  if (newDesiredCurr_ >= currentCap_) {
    setpoint_ = inVolt_ - 4 * pgain_;
    newDesiredCurr_ = currentCap_;
    sweeping_ = false;
    log(str("SWEEP DONE, currentcap reached (setpoint=%0.3f)\n", setpoint_));
    return applyAdjustment();
  }
  applyAdjustment();

  if (sweepPoints_.empty() || (psu_.outCurr_ > sweepPoints_.back().i))
    sweepPoints_.push_back({v: inVolt_, i: psu_.outCurr_});

  if (hasCollapsed()) {
    sweeping_ = false;
    printStatus();
    if (sweepPoints_.size()) {
      VI mp = sweepPoints_.front(); //furthest back point
      log(str("SWEEP DONE. c=%0.3f v=%0.3f, (setpoint was %0.3f)\n", mp.i, mp.v, setpoint_));
      psu_.enableOutput((psu_.outEn_ = false));
      psu_.setCurrent(mp.i * 0.95);
      setpoint_ = mp.v; //+stability offset?
      pub_.setDirtyAddr(&setpoint_);
    } else log("SWEEP DONE, no points?!");
    sweepPoints_.clear();
    autoStart_ = true; //main output will be enabled below by autostart logic
  }
}

bool Solar::hasCollapsed() const {
  return (psu_.outEn_ && inVolt_ < (psu_.outVolt_ * 10 / 9)); //voltage match method;
}

int Solar::getCollapses() const { return collapses_.size(); }

void Solar::loop() {
  uint32_t now = millis();
  if ((now - lastV) >= measperiod_) {
    int analogval = analogRead(pinInvolt_);
    inVolt_ = analogval * 3.3 * (vadjust_ / 3.3) / 4096.0;
    pub_.setDirtyAddr(&inVolt_);
    if (sweeping_) {
      doSweepStep();
    } else if (setpoint_ > 0 && psu_.outEn_) { //corrections enabled
      double error = inVolt_ - setpoint_;
      double dcurr = constrain(error * pgain_, -ramplimit_ * 2, ramplimit_); //limit ramping speed
      if (error > 0.3 || (-error > 0.2)) { //adjustment deadband, more sensitive when needing to ramp down
        newDesiredCurr_ = min(psu_.limitCurr_ + dcurr, currentCap_);
        if (error < 0.6) { //ramp down, quick!
          logme += "[QUICK] ";
          needsQuickAdj_ = true;
        }
      }
    }
    lastV = millis();
  }
  if ((now - lastPSUadjust_) >= (needsQuickAdj_? 100 : psuperiod_)) {
    if (setpoint_ > 0 && !sweeping_) {
      if (psu_.outEn_)
        applyAdjustment();

      if (hasCollapsed()) {
        newDesiredCurr_ = max(psu_.limitCurr_ * 0.95, 0.00); //restore at 90% of previous point
        collapses_.push_back(now);
        pub_.setDirty("collapses");
        needsQuickAdj_ = false;
        log(str("collapsed! %0.1fV set recovery to %0.1fA\n", inVolt_ ,newDesiredCurr_));
        psu_.enableOutput((psu_.outEn_ = false));
        psu_.setCurrent(newDesiredCurr_);
      } else if (autoStart_ && !psu_.outEn_ && inVolt_ > (setpoint_ * 1.02)) {
        log("restoring from collapse");
        psu_.enableOutput((psu_.outEn_ = true));
      }
    }
    if (collapses_.size() && (millis() - collapses_.front()) > (5 * 60000)) { //5m age
      logme += str("[clear collapse (%ds ago)]", (now - collapses_.pop_front())/1000);
      pub_.setDirty("collapses");
    }
    heap_caps_check_integrity_all(true);
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
      log("psu update fail");
      psu_.flush();
      //psu_.debug_ = true;
    }
    lastPSUpdate_ = now;
  }
  bool forceSweep = (getCollapses() > 2) && (now - lastAutoSweep_) >= (autoSweep_ / 3.0 * 1000);
  if (autoStart_ && autoSweep_ > 0 && ((now - lastAutoSweep_) >= (autoSweep_ * 1000) || forceSweep)) {
    if (psu_.outEn_ && now > autoSweep_*1000) { //skip this sweep if disabled or just started up
      log(str("Starting AUTO-SWEEP (last run %0.1f mins ago)\n", (now - lastAutoSweep_)/1000.0/60.0));
      startSweep();
    }
    lastAutoSweep_ = now;
  }
}

void Solar::publishTask() {
  doConnect();
  db_.client.loop();
  ignoreSubsUntil_ = millis() + 3000;
  db_.client.setCallback([=](char*topicbuf, uint8_t*buf, unsigned int len){
    String topic(topicbuf), val = str(std::string((char*)buf, len));
    log("got sub value " + topic + " -> " + val);
    if (topic == (db_.feed + "/wh")) {
      wh_ = val.toFloat();
      log("restored wh value to " + val);
      db_.client.unsubscribe((db_.feed + "/wh").c_str());
    } else if (millis() > ignoreSubsUntil_) { //don't load old values
      topic.replace(db_.feed + "/prefs/", ""); //replaces in-place, sadly
      String ret = pub_.handleSet(topic, val);
      log("MQTT cmd: " + ret);
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
      heap_caps_check_integrity_all(true);
      lastpub = now;
    }
    if (db_.client.connected() && logPub_.size()) {
      String s = logPub_.pop_front();
      db_.client.publish((db_.feed + "/log").c_str(), s.c_str(), false);
    }
    db_.client.loop();
    pub_.poll(&Serial);
    server_.handleClient();
    delay(1);
  }
}

void Solar::printStatus() {
  Serial.println(str("%0.1fVin -> %0.2fWh <%0.2fV out %0.2fA %den> ", inVolt_, wh_, psu_.outVolt_, psu_.outCurr_, psu_.outEn_) + logme);
  logme = "";
}

void Solar::log(String s) {
  Serial.println(s);
  logPub_.push_back(s);
}

