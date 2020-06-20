#include "solar.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <ESPmDNS.h>
#include <Update.h>

WiFiClient espClient;

Solar::Solar() :
        state_(States::off),
        psu_(Serial2),
        server_(80),
        pub_() {
  db_.client.setClient(espClient);
}

// void runLoop(void*c) { ((Solar*)c)->loopTask(); }
void runPubt(void*c) { ((Solar*)c)->publishTask(); }

//TODO: make these statics members instead
uint32_t lastV = 0, lastpub = 20000, lastLog_ = 0;
uint32_t nextPSUpdate_ = 0, nextSolarAdjust_ = 1000;
uint32_t lastPSUpdate_ = 0, lastPSUSuccess_ = 0;
uint32_t nextAutoSweep_ = 0, lastAutoSweep_ = 0;
double newDesiredCurr_ = 0;
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
  pub_.add("outpower",[=](String){ return String(psu_.outVolt_ * psu_.outCurr_); });
  pub_.add("state",      state_          );
  pub_.add("currFilt",   currFilt_       );
  pub_.add("pgain",      pgain_          ).pref();
  pub_.add("ramplimit",  ramplimit_      ).pref();
  pub_.add("setpoint",   setpoint_       ).pref();
  pub_.add("vadjust",    vadjust_        ).pref();
  pub_.add("printperiod",printPeriod_    ).pref();
  pub_.add("pubperiod",  db_.period      ).pref();
  pub_.add("adjustperiod",adjustPeriod_  ).pref();
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
  pub_.add("debug",[=](String s){ psu_.debug_ = !(s == "off"); return String(psu_.debug_); }).hide();

  server_.on("/", HTTP_ANY, [=]() {
    log("got req " + server_.uri() + " -> " + server_.hostHeader());
    String ret;
    for (int i = 0; i < server_.args(); i++)
      ret += pub_.handleSet(server_.argName(i), server_.arg(i)) + "\n";
    server_.sendHeader("Connection", "close");
    if (! ret.length()) ret = pub_.toJson();
    server_.send(200, "application/json", ret.c_str());
  });
  server_.on("/update", HTTP_POST, [this](){
    server_.sendHeader("Connection", "close");
    server_.send(200, "text/plain", (Update.hasError())?"FAIL":"OK");
    // esp_wifi_wps_disable();
    ESP.restart();
  },[this](){
    HTTPUpload& upload = server_.upload();
    if(upload.status == UPLOAD_FILE_START){
      log(str("Update: %s\n", upload.filename.c_str()));
      if (!Update.begin(UPDATE_SIZE_UNKNOWN))//start with max available size
        Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_WRITE){
      /* flashing firmware to ESP*/
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
        Update.printError(Serial);
    } else if(upload.status == UPLOAD_FILE_END){
      if(Update.end(true)) //true to set the size to the current progress
        log(str("Update Success: %u\nRebooting...\n", upload.totalSize));
      else Update.printError(Serial);
    }
  });

  pub_.loadPrefs();
  // wifi & mqtt is connected by pubsubConnect below

  //fn, name, stack size, parameter, priority, handle
  xTaskCreate(runPubt, "publish", 10000, this, 1, NULL);

  if (!psu_.begin())
    log("PSU begin failed");
  nextAutoSweep_ = millis() + 10000;
  Serial.println("finished setup");
  log("OSPController Version " GIT_VERSION);
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
        db_.client.subscribe((db_.feed + "/cmd").c_str()); //subscribe to cmd topic for any actions
      } else Serial.println("PubSub connect ERROR! " + db_.client.state());
    } else Serial.println("no MQTT user / pass / server / feed set up!");
  } else Serial.printf("can't pub connect, wifi %d pub %d\n", WiFi.isConnected(), db_.client.connected());
}

void Solar::applyAdjustment() {
  if (newDesiredCurr_ != psu_.limitCurr_) {
    if (psu_.setCurrent(newDesiredCurr_))
      logme += str("[adjusting %0.2fA (from %0.2fA)] ", newDesiredCurr_ - psu_.limitCurr_, psu_.limitCurr_);
    else log("error setting current");
    delay(50);
    psu_.readCurrent();
    pub_.setDirty({"outcurr", "outpower"});
    printStatus();
  }
}

void Solar::startSweep() {
  if (state_ == States::error)
    return log("can't sweep, system is in error state");
  log(str("SWEEP START c=%0.3f, (setpoint was %0.3f)", newDesiredCurr_, setpoint_));
  if (state_ == States::collapsemode) {
    psu_.enableOutput(false);
    psu_.setCurrent(currFilt_* 0.75);
    delay(200);
    log(str("First coming out of collapse-mode to clim of %0.2fA", psu_.limitCurr_));
  }
  setState(States::sweeping);
  if (!psu_.outEn_)
      psu_.enableOutput(true);
  lastAutoSweep_ = millis();
}

void Solar::doSweepStep() {
  if (!psu_.outEn_)
    return setState(States::mppt);

  newDesiredCurr_ = psu_.limitCurr_ + (inVolt_ * 0.001); //speed porportional to input voltage
  if (newDesiredCurr_ >= currentCap_) {
    setpoint_ = inVolt_ - (pgain_ * 4);
    newDesiredCurr_ = currentCap_;
    setState(States::mppt);
    log(str("SWEEP DONE, currentcap of %0.1fA reached (setpoint=%0.3f)", currentCap_, setpoint_));
    return applyAdjustment();
  }

  bool isCollapsed = hasCollapsed();
  sweepPoints_.push_back({v: psu_.outVolt_, i: psu_.outCurr_, input: inVolt_, collapsed: isCollapsed});
  int collapsedPoints = 0;
  for (int i = 0; i < sweepPoints_.size(); i++)
    if (sweepPoints_[i].collapsed) collapsedPoints++;
  if (isCollapsed) logme += str("COLLAPSED[%d] ", collapsedPoints);

  if (isCollapsed && collapsedPoints > 3) { //great, sweep finished
    printStatus();
    if (sweepPoints_.size()) {
      VI mp = sweepPoints_.front(); //furthest back point
      for (int i = 0; i < sweepPoints_.size(); i++)
        if (sweepPoints_[i].i * sweepPoints_[i].v > mp.i * mp.v) mp = sweepPoints_[i]; //find max
      log(str("SWEEP DONE. max point = %0.3fA %0.3fV %0.3fVin , (setpoint was %0.3f)", mp.i, mp.v, mp.input, setpoint_));
      if (mp.collapsed) {
        log(str("Max point is actually running collapsed. Will sweep again in %0.1f mins", ((float)autoSweep_) / 3.0 / 60.0));
        setState(States::collapsemode);
        psu_.setCurrent(currentCap_ > 0? currentCap_ : 10);
        nextAutoSweep_ = millis() + autoSweep_ * 1000 / 3; //reschedule soon
      } else {
        setState(States::mppt);
        psu_.enableOutput(false);
        psu_.setCurrent(mp.i * 0.95);
      }
      setpoint_ = mp.input * 1.01; //stability offset
      pub_.setDirtyAddr(&setpoint_);
      nextSolarAdjust_ = millis() + 1000; //don't recheck the voltage too quickly
    } else log("SWEEP DONE, no points?!");
    sweepPoints_.clear();
    //the output should be re-enabled below
  }

  applyAdjustment();
}

bool Solar::hasCollapsed() const {
  if (!psu_.outEn_) return false;
  if (inVolt_ < (psu_.outVolt_ * 1.11)) //simple voltage match method
    return true;
  float vunder = psu_.outVolt_ / psu_.limitVolt_;
  float cunder = psu_.outCurr_ / psu_.limitCurr_;
  if (psu_.debug_) Serial.printf("hasCollapsed under[v%0.3f c%0.3f] PSU[%0.3fV %0.3fVlim %0.3fA %0.3fClim]",
      vunder, cunder, psu_.outVolt_, psu_.limitVolt_,psu_.outCurr_, psu_.limitCurr_);
  if (psu_.limitCurr_ > 1.5 && (vunder < 0.9) && (cunder < 0.7) && (inVolt_ < (psu_.outVolt_ * 1.25)))
    return true;
  return false;
}

int Solar::getCollapses() const { return collapses_.size(); }

void Solar::loop() {
  uint32_t now = millis();
  if ((now - lastV) >= ((state_ == States::sweeping)? measperiod_ * 3 : measperiod_)) {
    int analogval = analogRead(pinInvolt_);
    inVolt_ = analogval * 3.3 * (vadjust_ / 3.3) / 4096.0;
    pub_.setDirtyAddr(&inVolt_);
    if (state_ == States::sweeping) {
      doSweepStep();
    } else if (setpoint_ > 0 && psu_.outEn_) { //corrections enabled
      double error = inVolt_ - setpoint_;
      double dcurr = constrain(error * pgain_, -ramplimit_ * 2, ramplimit_); //limit ramping speed
      if (error > 0.3 || (-error > 0.2)) { //adjustment deadband, more sensitive when needing to ramp down
        newDesiredCurr_ = min(psu_.limitCurr_ + dcurr, currentCap_);
        if (error < 0.6) { //ramp down, quick!
          logme += "[QUICK] ";
          nextSolarAdjust_ = now;
        }
      }
    }

    //update system states:
    if (state_ != States::sweeping && state_ != States::collapsemode) {
      bool psuActive = (millis() - lastPSUSuccess_) < 11000;
      if (psu_.outEn_ && psuActive) {
        if      (psu_.outCurr_ > (currentCap_     * 0.95 )) setState(States::capped);
        else if (psu_.outVolt_ > (psu_.limitVolt_ * 0.999)) setState(States::full_cv);
        else setState(States::mppt);
      } else if (!psuActive) {
        if (inVolt_ < 1) setState(States::off); //diode connected, power supply is off when dark
        else setState(States::error);
      }
    }
    lastV = millis();
  }

  if (now > nextSolarAdjust_) {
    if (state_ == States::error) {
      if ((now - lastPSUSuccess_) < 30000) { //for 30s after failure try and shut it down
        psu_.enableOutput(false);
        psu_.setCurrent(0);
        return backoff("PSU failure, disabling");
      }
    } else if (setpoint_ > 0 && (state_ != States::sweeping)) {
      if (hasCollapsed() && state_ != States::collapsemode) {
        newDesiredCurr_ = currFilt_ * 0.95; //restore at 90% of previous point
        collapses_.push_back(now);
        pub_.setDirty("collapses");
        log(str("collapsed! %0.1fV [%0.1fV %0.1fVlim %0.1fA %0.1fVlim] set recovery to %0.1fA", inVolt_, psu_.outVolt_, psu_.limitVolt_,psu_.outCurr_, psu_.limitCurr_, newDesiredCurr_));
        psu_.enableOutput(false);
        psu_.setCurrent(newDesiredCurr_);
      } else if (!psu_.outEn_) { //power supply is off. let's check about turning it on
        if (inVolt_ < psu_.outVolt_ || psu_.outVolt_ < 0.1) {
          return backoff("not starting up, input voltage too low (is it dark?)");
        } else if ((psu_.outVolt_ > psu_.limitVolt_) || (psu_.outVolt_ < (psu_.limitVolt_ * 0.60) && psu_.outVolt_ > 1)) {
          //li-ion 4.1-2.5 is 60% of range. the last && condition allows system to work with battery drain diode in place
          return backoff(str("not starting up, battery %0.1fV too far from Supply limit %0.1fV. ", psu_.outVolt_, psu_.limitVolt_) +
          "Use outvolt command (or PSU buttons) to set your appropiate battery voltage and restart");
        } else {
          log("restoring from collapse");
          psu_.enableOutput(true);
        }
      }

      if (psu_.outEn_ && state_ != States::collapsemode)
          applyAdjustment();
    }
    if (collapses_.size() && (millis() - collapses_.front()) > (5 * 60000)) { //5m age
      logme += str("[clear collapse (%ds ago)]", (now - collapses_.pop_front())/1000);
      pub_.setDirty("collapses");
    }
    heap_caps_check_integrity_all(true);
    backoffLevel_ = max(backoffLevel_ - 1, 0); //successes means no more backoff!
    nextSolarAdjust_ = now + adjustPeriod_;
  }
  if ((now - lastLog_) >= printPeriod_) {
    printStatus();
    lastLog_ = now;
  }
  if (now > nextPSUpdate_) {
    bool res = psu_.doUpdate();
    if (res) {
      wh_ += psu_.outVolt_ * psu_.outCurr_ * (now - lastPSUpdate_) / 1000.0 / 60 / 60;
      currFilt_ = currFilt_ - 0.1 * (currFilt_ - psu_.outCurr_);
      pub_.setDirty({"outvolt", "outcurr", "outputEN", "wh", "outpower", "currFilt"});
      lastPSUSuccess_ = now;
    } else {
      log("psu update fail" + String(psu_.debug_? " serial debug output enabled" : ""));
      psu_.flush();
    }
    nextPSUpdate_ = now + 5000;
    lastPSUpdate_ = now;
  }
  if (getCollapses() > 2) {
    nextAutoSweep_ = lastAutoSweep_ + autoSweep_ / 3.0 * 1000;
  }
  if (autoSweep_ > 0 && (now > nextAutoSweep_)) {
    if (state_ == States::capped) {
      log(str("Skipping auto-sweep. Already at currentCap (%0.1fA)", currentCap_));
    } else if (state_ == States::full_cv) {
      log(str("Skipping auto-sweep. Battery-full voltage reached (%0.1fV)", psu_.outVolt_));
    } else if (state_ == States::mppt) {
      log(str("Starting AUTO-SWEEP (last run %0.1f mins ago)", (now - lastAutoSweep_)/1000.0/60.0));
      startSweep();
    }
    nextAutoSweep_ = now + autoSweep_ * 1000;
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
      wh_ += val.toFloat();
      log("restored wh value to " + val);
      db_.client.unsubscribe((db_.feed + "/wh").c_str());
    } else if (millis() > ignoreSubsUntil_) { //don't load old values
      if (topic == db_.feed + "/cmd") {
        log("MQTT cmd " + topic + ":" + val + " -> " + pub_.handleCmd(val));
      } else {
        topic.replace(db_.feed + "/prefs/", ""); //replaces in-place, sadly
        log("MQTT cmd " + topic + ":" + val + " -> " + pub_.handleSet(topic, val));
      }
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
  String s = state_;
  s.toUpperCase();
  s += str(" %0.1fVin -> %0.2fWh <%0.2fV out %0.2fA %den> ", inVolt_, wh_, psu_.outVolt_, psu_.outCurr_, psu_.outEn_);
  s += logme;
  logme = ""; //clear
  Serial.println(s);
  if (psu_.debug_) logPub_.push_back(s);
}

void Solar::log(String s) {
  Serial.println(s);
  logPub_.push_back(s);
}

void Solar::backoff(String reason) {
  backoffLevel_ = min(backoffLevel_ + 1, 8);
  int back = (backoffLevel_ * backoffLevel_) / 2;
  log(str("backoff %ds: ", back * adjustPeriod_ / 1000) + reason);
  nextSolarAdjust_ = millis() + back * adjustPeriod_; //big backoff
  nextPSUpdate_ = nextSolarAdjust_; //backoff this too
  if (psu_.outEn_)
    psu_.enableOutput(false);
}

void Solar::setState(const String state) {
  if (state_ != state) {
    pub_.setDirty("state");
    log("state change to " + state + " (from " + state_ + ")");
  }
  state_ = state;
}

