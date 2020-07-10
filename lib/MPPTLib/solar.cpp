#include "solar.h"
#include "utils.h"
#include "powerSupplies.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include <HTTPUpdate.h>

using namespace std::placeholders;

WiFiClient espClient;

Solar::~Solar() { }

Solar::Solar(String version) :
        version_(version),
        state_(States::off),
        server_(80),
        pub_() {
  db_.client.setClient(espClient);
}

// void runLoop(void*c) { ((Solar*)c)->loopTask(); }
void runPubt(void*c) { ((Solar*)c)->publishTask(); }

//TODO: make these class members instead?
uint32_t nextVmeas_ = 0, nextPub_ = 20000, nextPrint_ = 0;
uint32_t nextPSUpdate_ = 0, nextSolarAdjust_ = 1000;
uint32_t nextAutoSweep_ = 0, lastAutoSweep_ = 0;
double newDesiredCurr_ = 0;
extern const String updateIndex;
String doOTAUpdate_ = "";
uint32_t espSketchSize_ = 0;

class Backoff : public std::runtime_error { public:
  Backoff(String s) : std::runtime_error(s.c_str()) { }
};

void Solar::setup() {
  Serial.begin(115200);
  Serial.setTimeout(10); //very fast, need to keep the ctrl loop running
  addLogger(&pub_); //sets global context
  espSketchSize_ = ESP.getSketchSize();
  delay(100);
  log(getResetReasons());
  uint64_t chipid = ESP.getEfuseMac();
  log(str("startup, ID %08llX %04X\n", chipid, (uint16_t)(chipid >> 32)));
  analogSetCycles(32);

  pub_.add("wifiap",     wifiap).hide().pref();
  pub_.add("wifipass", wifipass).hide().pref();
  pub_.add("mqttServ", db_.serv).hide().pref();
  pub_.add("mqttUser", db_.user).hide().pref();
  pub_.add("mqttPass", db_.pass).hide().pref();
  pub_.add("mqttFeed", db_.feed).hide().pref();
  pub_.add("inPin",    pinInvolt_).pref();
  pub_.add("lvProtect", std::bind(&Solar::setLVProtect, this, _1)).pref();
  pub_.add("psu",       std::bind(&Solar::setPSU, this, _1)).pref();
  pub_.add("outputEN",[=](String s){ if (s.length()) psu_->enableOutput(s == "on"); return String(psu_->outEn_); });
  pub_.add("outvolt", [=](String s){ if (s.length()) psu_->setVoltage(s.toFloat()); return String(psu_->outVolt_); });
  pub_.add("outcurr", [=](String s){ if (s.length()) psu_->setCurrent(s.toFloat()); return String(psu_->outCurr_); });
  pub_.add("outpower",[=](String){ return String(psu_->outVolt_ * psu_->outCurr_); });
  pub_.add("currFilt",[=](String){ return String(psu_->currFilt_); });
  pub_.add("state",      state_          );
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
  pub_.add("wh", [=](String s) { if (s.length()) psu_->wh_ = s.toFloat(); return String(psu_->wh_); });
  pub_.add("collapses", [=](String) { return String(getCollapses()); });
  pub_.add("sweep",[=](String){ startSweep(); return "starting sweep"; }).hide();
  pub_.add("connect",[=](String s){ doConnect(); return "connected"; }).hide();
  pub_.add("disconnect",[=](String s){ db_.client.disconnect(); WiFi.disconnect(); return "dissed"; }).hide();
  pub_.add("restart",[](String s){ ESP.restart(); return ""; }).hide();
  pub_.add("clear",[=](String s){ pub_.clearPrefs(); return "cleared"; }).hide();
  pub_.add("debug",[=](String s){ psu_->debug_ = !(s == "off"); return String(psu_->debug_); }).hide();
  pub_.add("version",[=](String){ log("Version " + version_); return version_; }).hide();
  pub_.add("update",[=](String s){ doOTAUpdate_ = s; return "OK, will try "+s; }).hide();
  pub_.add("uptime",[=](String){ String ret = "Uptime " + timeAgo(millis()/1000); log(ret); return ret; }).hide();

  server_.on("/", HTTP_ANY, [=]() {
    log("got req " + server_.uri() + " -> " + server_.hostHeader());
    String ret;
    for (int i = 0; i < server_.args(); i++)
      ret += pub_.handleSet(server_.argName(i), server_.arg(i)) + "\n";
    server_.sendHeader("Connection", "close");
    if (! ret.length()) ret = pub_.toJson();
    server_.send(200, "application/json", ret.c_str());
  });

  server_.on("/update", HTTP_GET, [this](){
    server_.sendHeader("Connection", "close");
    server_.send(200, "text/html", updateIndex);
  });
  server_.on("/update", HTTP_POST, [this](){
    server_.sendHeader("Connection", "close");
    server_.send(200, "text/plain", (Update.hasError())?"FAIL":"OK");
    ESP.restart();
  },[=](){
    HTTPUpload& upload = server_.upload();
    if (upload.status == UPLOAD_FILE_START){
      log(str("Update: %s\n", upload.filename.c_str()));
      doOTAUpdate_ = " "; //stops tasks
      db_.client.disconnect(); //helps reliability
      esp_task_wdt_init(120, true); //slows watchdog
      if (!Update.begin(UPDATE_SIZE_UNKNOWN))//start with max available size
        Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_WRITE){
      log(str("OTA upload at %dKB ~%0.1%%%", Update.progress() / 1000, Update.progress() * 100 / espSketchSize_));
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
        Update.printError(Serial);
    } else if(upload.status == UPLOAD_FILE_END){
      if (Update.end(true))
        log(str("Update Success: %u\nRebooting...\n", upload.totalSize));
      else Update.printError(Serial);
    } else if (upload.status == UPLOAD_FILE_ABORTED){
      log("Update ABORTED, rebooting.");
      Update.abort();
      delay(500);
      ESP.restart();
    } else log(str("Update ELSE %d", upload.status));
  });

  pub_.loadPrefs();
  // wifi & mqtt is connected by pubsubConnect below

  if (digitalPinToAnalogChannel(pinInvolt_) < 0)
    log(str("ERROR, inPin %d isn't actually an ADC pin", pinInvolt_));
  if (digitalPinToAnalogChannel(pinInvolt_) > 7)
    log(str("ERROR, inPin %d is an ADC2 pin and WILL NOT WORK", pinInvolt_));

  //fn, name, stack size, parameter, priority, handle
  xTaskCreate(runPubt, "publish", 10000, this, 1, NULL);

  if (!psu_->begin())
    log("PSU begin failed");
  newDesiredCurr_ = psu_->currFilt_ = psu_->limitCurr_ = psu_->outCurr_;
  log(str("startup current is %0.3fAdes/%0.3fAfilt/%0.3fAout", newDesiredCurr_, psu_->currFilt_, psu_->outCurr_));
  nextAutoSweep_ = millis() + 10000;
  log("finished setup");
  log("OSPController Version " + version_);
}

String Solar::setLVProtect(String s) {
  if (s.length()) {
    lvProtect_.reset(new LowVoltageProtect(s)); //may throw!
    log("low-voltage cutoff enabled: " + lvProtect_->toString() + " (pin[i]:cutoff:recovery)");
    lvProtect_->nextCheck_ = millis() + 5000; //don't check right away
    return "new " + lvProtect_->toString() + " ok";
  } else return lvProtect_? lvProtect_->toString() : "";
}

String Solar::setPSU(String s) {
  log("setPSU " + s);
  if (s.length() || !psu_) {
    //TODO Parse softserial pins, bluetooth comms, and moar.
    s.toUpperCase();
    if (s.startsWith("DP")) {
      psu_.reset(new DPS(&Serial2));
      Serial2.begin(19200, SERIAL_8N1, -1, -1, false, 1000);
      if (measperiod_ == 200) //default
        measperiod_ = 500;
    } else { //default
      psu_.reset(new Drok(&Serial2));
      Serial2.begin(4800, SERIAL_8N1, -1, -1, false, 1000);
    }
    psu_->begin();
    return "created PSU=" + psu_->getType();
  }
  return psu_->getType();
}

void Solar::doConnect() {
  if (! WiFi.isConnected()) {
    if (wifiap.length() && wifipass.length()) {
      WiFi.begin(wifiap.c_str(), wifipass.c_str());
      uint64_t chipid = ESP.getEfuseMac();
      String hostname = str("mpptESP-%02X", chipid & 0xff);
      WiFi.setHostname(hostname.c_str());
      if (WiFi.waitForConnectResult() == WL_CONNECTED) {
        log("Wifi connected! hostname: " + hostname);
        log("IP: " + WiFi.localIP().toString());
        MDNS.begin("mppt");
        MDNS.addService("http", "tcp", 80);
        server_.begin();
        lastConnected_ = millis();
      }
    } else log("no wifiap or wifipass set!");
  }
  if (WiFi.isConnected() && !db_.client.connected()) {
    if (db_.serv.length() && db_.feed.length()) {
      log("Connecting MQTT to " + db_.user + "@" + db_.serv);
      db_.client.setServer(db_.getEndpoint().c_str(), db_.getPort());
      if (db_.client.connect("MPPT", db_.user.c_str(), db_.pass.c_str())) {
        log("PubSub connect success! " + db_.client.state());
        db_.client.subscribe((db_.feed + "/cmd").c_str()); //subscribe to cmd topic for any actions
        lastConnected_ = millis();
      } else pub_.logNote("[PubSub connect ERROR]" + db_.client.state());
    } else pub_.logNote("[no MQTT user/pass/serv/feed set up]");
  } else pub_.logNote(str("[can't pub connect, wifi %d pub %d]", WiFi.isConnected(), db_.client.connected()));
}

String SPoint::toString() const {
  return str("[%0.3fVin %0.3fVout %0.3fAout", input, v, i) + (collapsed? " COLLAPSED]" : " ]");
}

void Solar::applyAdjustment() {
  if (newDesiredCurr_ != psu_->limitCurr_) {
    if (psu_->setCurrent(newDesiredCurr_))
      pub_.logNote(str("[adjusting %0.3fA (from %0.3fA)]", newDesiredCurr_ - psu_->limitCurr_, psu_->limitCurr_));
    else log("error setting current");
    delay(50);
    psu_->readCurrent();
    pub_.setDirty({"outcurr", "outpower"});
    printStatus();
  }
}

void Solar::startSweep() {
  if (state_ == States::error)
    return log("can't sweep, system is in error state");
  log(str("SWEEP START c=%0.3f, (setpoint was %0.3f)", newDesiredCurr_, setpoint_));
  if (state_ == States::collapsemode) {
    psu_->enableOutput(false);
    psu_->setCurrent(psu_->currFilt_* 0.75);
    delay(200);
    log(str("First coming out of collapse-mode to clim of %0.2fA", psu_->limitCurr_));
  }
  setState(States::sweeping);
  if (!psu_->outEn_)
      psu_->enableOutput(true);
  lastAutoSweep_ = millis();
}

void Solar::doSweepStep() {
  if (!psu_->outEn_)
    return setState(States::mppt);

  newDesiredCurr_ = psu_->limitCurr_ + (inVolt_ * 0.001); //speed porportional to input voltage
  if (newDesiredCurr_ >= currentCap_) {
    setpoint_ = inVolt_ - (pgain_ * 4);
    newDesiredCurr_ = currentCap_;
    setState(States::mppt);
    log(str("SWEEP DONE, currentcap of %0.1fA reached (setpoint=%0.3f)", currentCap_, setpoint_));
    return applyAdjustment();
  } else if (psu_->isCV()) {
    setState(States::full_cv);
    return log("SWEEP DONE, constant-voltage state reached");
  }
  psu_->doUpdate();

  bool isCollapsed = hasCollapsed();
  sweepPoints_.push_back({v: psu_->outVolt_, i: psu_->outCurr_, input: inVolt_, collapsed: isCollapsed});
  int collapsedPoints = 0;
  for (int i = 0; i < sweepPoints_.size(); i++)
    if (sweepPoints_[i].collapsed) collapsedPoints++;
  if (isCollapsed) pub_.logNote(str("COLLAPSED[%d]", collapsedPoints));

  if (isCollapsed && collapsedPoints >= 2) { //great, sweep finished
    int maxIndex = 0;
    SPoint collapsePoint = sweepPoints_.back();

    for (int i = 0; i < sweepPoints_.size(); i++) {
      log(str("point %i = ", i) + sweepPoints_[i].toString());
      if (!sweepPoints_[i].collapsed && sweepPoints_[i].p() > sweepPoints_[maxIndex].p())
        maxIndex = i; //find max
    }
    log("SWEEP DONE. max point = " + sweepPoints_[maxIndex].toString() + str(", (setpoint was %0.3f)", setpoint_));
    if (sweepPoints_[maxIndex].p() < collapsePoint.p()) {
      log(str("Max point is actually running collapsed. Will sweep again in %0.1f mins", ((float)autoSweep_) / 3.0 / 60.0));
      setState(States::collapsemode);
      psu_->setCurrent(currentCap_ > 0? currentCap_ : 10);
      nextAutoSweep_ = millis() + autoSweep_ * 1000 / 3; //reschedule soon
      setpoint_ = collapsePoint.input;
    } else {
      maxIndex = max(0, maxIndex - 2);
      log("Using safe max point of " + sweepPoints_[maxIndex].toString());
      setState(States::mppt);
      psu_->enableOutput(false);
      psu_->setCurrent(sweepPoints_[maxIndex].i * 0.90);
      delay(300);
      psu_->enableOutput(true);
      delay(300);
      setpoint_ = sweepPoints_[maxIndex].input;
    }
    pub_.setDirtyAddr(&setpoint_);
    nextSolarAdjust_ = millis() + 1000; //don't recheck the voltage too quickly
    sweepPoints_.clear();
    //the output should be re-enabled below
  }

  applyAdjustment();
}

bool Solar::hasCollapsed() const {
  if (!psu_->outEn_) return false;
  bool simpleClps = (inVolt_ < (psu_->outVolt_ * 1.11)); //simple voltage match method
  float collapsePct = (inVolt_ - psu_->outVolt_) / psu_->outVolt_;
  if (simpleClps && psu_->isCollapsed())
    return true;
  if ((collapsePct < 0.05) && psu_->isCollapsed()) { //secondary method
    log(str("hasCollapsed used secondary method. collapse %0.3f%%", collapsePct));
    return true;
  }
  return false;
}

int Solar::getCollapses() const { return collapses_.size(); }

void Solar::loop() {
  uint32_t now = millis();
  if (doOTAUpdate_.length())
    return delay(100);

  if (now > nextVmeas_) {
    if (psu_->getInputVolt(&inVolt_)) {
      psu_->doUpdate();
      psu_->getInputVolt(&inVolt_);
    } else {
      int analogval = analogRead(pinInvolt_);
      inVolt_ = analogval * 3.3 * (vadjust_ / 3.3) / 4096.0;
    }
    pub_.setDirtyAddr(&inVolt_);
    if (state_ == States::sweeping) {
      doSweepStep();
    } else if (setpoint_ > 0 && psu_->outEn_) { //corrections enabled
      double error = inVolt_ - setpoint_;
      double dcurr = constrain(error * pgain_, -ramplimit_ * 2, ramplimit_); //limit ramping speed
      if (error > 0.3 || (-error > 0.2)) { //adjustment deadband, more sensitive when needing to ramp down
        newDesiredCurr_ = min(psu_->limitCurr_ + dcurr, currentCap_);
        if ((error < 0.6) && (state_ == States::mppt)) { //ramp down, quick!
          pub_.logNote("[QUICK]");
          nextSolarAdjust_ = now;
        }
      }
    }

    //update system states:
    if (state_ != States::sweeping && state_ != States::collapsemode) {
      int lastPSUsecs = (millis() - psu_->lastSuccess_) / 1000;
      if (psu_->outEn_) {
        if      (lastPSUsecs > 11) setState(States::error, "enabled but no PSU comms");
        else if (psu_->outCurr_ > (currentCap_     * 0.95 )) setState(States::capped);
        else if (psu_->isCV()) setState(States::full_cv);
        else setState(States::mppt);
      } else { //disabled
        if ((inVolt_ > 1) && lastPSUsecs > 120) //psu active at least every 2m when shut down
          setState(States::error, "inactive PSU");
        else setState(States::off);
      }
      if ((inVolt_ > 1) && (lastPSUsecs > 5 * 60)) {
        log("VERY UNRESPONSIVE PSU, RESTARTING");
        delay(1000);
        ESP.restart();
      }
    }
    nextVmeas_ = now + ((state_ == States::sweeping)? measperiod_ * 4 : measperiod_);
  }

  if (now > nextSolarAdjust_) {
    try {
      if (state_ == States::error) {
        if ((now - psu_->lastSuccess_) < 30000) { //for 30s after failure try and shut it down
          psu_->enableOutput(false);
          psu_->setCurrent(0);
          throw Backoff("PSU failure, disabling");
        }
      } else if (setpoint_ > 0 && (state_ != States::sweeping)) {
        if (hasCollapsed() && state_ != States::collapsemode) {
          newDesiredCurr_ = psu_->currFilt_ * 0.95; //restore at 90% of previous point
          collapses_.push_back(now);
          pub_.setDirty("collapses");
          log(str("collapsed! %0.2fV set recovery to %0.1fA ", inVolt_, newDesiredCurr_) + psu_->toString());
          psu_->enableOutput(false);
          psu_->setCurrent(newDesiredCurr_);
        } else if (!psu_->outEn_) { //power supply is off. let's check about turning it on
          if (inVolt_ < psu_->outVolt_ || psu_->outVolt_ < 0.1) {
            throw Backoff("not starting up, input voltage too low (is it dark?)");
          } else if ((psu_->outVolt_ > psu_->limitVolt_) || (psu_->outVolt_ < (psu_->limitVolt_ * 0.60) && psu_->outVolt_ > 1)) {
            //li-ion 4.1-2.5 is 60% of range. the last && condition allows system to work with battery drain diode in place
            throw Backoff(str("not starting up, battery %0.1fV too far from Supply limit %0.1fV. ", psu_->outVolt_, psu_->limitVolt_) +
            "Use outvolt command (or PSU buttons) to set your appropiate battery voltage and restart");
          } else {
            log("restoring from collapse");
            psu_->enableOutput(true);
          }
        }
        if (psu_->outEn_ && state_ != States::collapsemode) {
          applyAdjustment();
        }
      }
      backoffLevel_ = max(backoffLevel_ - 1, 0); //successes means less backoff
    } catch (const Backoff &b) {
      backoffLevel_ = min(backoffLevel_ + 1, 8);
      log(str("backoff now at %ds: ", getBackoff(adjustPeriod_) / 1000) + String(b.what()));
    }
    if (collapses_.size() && (millis() - collapses_.front()) > (5 * 60000)) { //5m age
      pub_.logNote(str("[clear collapse (%ds ago)]", (now - collapses_.pop_front())/1000));
      pub_.setDirty("collapses");
    }
    heap_caps_check_integrity_all(true);
    nextSolarAdjust_ = now + getBackoff(adjustPeriod_);
  }

  if (now > nextPrint_) {
    printStatus();
    nextPrint_ = now + printPeriod_;
  }

  if (now > nextPSUpdate_) {
    bool res = psu_->doUpdate();
    if (res) {
      pub_.setDirty({"outvolt", "outcurr", "outputEN", "outpower", "currFilt"});
      if (psu_->wh_ > 2.0 || (millis() - lastConnected_) > 60000) pub_.setDirty("wh"); //don't publish at first
    } else {
      log("psu update fail" + String(psu_->debug_? " serial debug output enabled" : ""));
      psu_->begin(); //try and reconnect
    }
    nextPSUpdate_ = now + min(getBackoff(5000), 100000); //100s
  }

  if (lvProtect_ && now > lvProtect_->nextCheck_) {
    if (!lvProtect_->isTriggered() && psu_->outVolt_ < lvProtect_->threshold_) {
      log(str("LOW VOLTAGE PROTECT TRIGGERED (now at %0.2fV)", psu_->outVolt_));
      sendOutgoingLogs(); //send logs, tripping this relay may power us down
      delay(200);
      lvProtect_->trigger(true);
      lvProtect_->nextCheck_ = now + 5 * 1000;
    } else if (lvProtect_->isTriggered() && psu_->outVolt_ > lvProtect_->threshRecovery_) {
      log("low voltage recovery, re-enabling.");
      lvProtect_->trigger(false);
      lvProtect_->nextCheck_ = now + 10000;
    }
  }

  if (getCollapses() > 2)
    nextAutoSweep_ = lastAutoSweep_ + autoSweep_ / 3.0 * 1000;

  if (autoSweep_ > 0 && (now > nextAutoSweep_)) {
    if (state_ == States::capped) {
      log(str("Skipping auto-sweep. Already at currentCap (%0.1fA)", currentCap_));
    } else if (state_ == States::full_cv) {
      log(str("Skipping auto-sweep. Battery-full voltage reached (%0.1fV)", psu_->outVolt_));
    } else if (state_ == States::mppt || state_ == States::collapsemode) {
      log(str("Starting AUTO-SWEEP (last run %0.1f mins ago)", (now - lastAutoSweep_)/1000.0/60.0));
      startSweep();
    }
    nextAutoSweep_ = now + autoSweep_ * 1000;
    lastAutoSweep_ = now;
  }
}

void Solar::sendOutgoingLogs() {
  String s;
  while (db_.client.connected() && pub_.popLog(&s))
    db_.client.publish((db_.feed + "/log").c_str(), s.c_str(), false);
}

void Solar::publishTask() {
  doConnect();
  db_.client.loop();
  db_.client.setCallback([=](char*topicbuf, uint8_t*buf, unsigned int len){
    String topic(topicbuf), val = str(std::string((char*)buf, len));
    log("got sub value " + topic + " -> " + val);
    if (topic == (db_.feed + "/wh")) {
      psu_->wh_ = (psu_->wh_ > 2.0)? val.toFloat() : psu_->wh_ + val.toFloat();
      log("restored wh value to " + val);
      db_.client.unsubscribe((db_.feed + "/wh").c_str());
    } else if (topic == db_.feed + "/cmd") {
      log("MQTT cmd " + topic + ":" + val + " -> " + pub_.handleCmd(val));
    } else {
      log("MQTT unknown message " + topic + ":" + val);
    }
  });
  db_.client.subscribe((db_.feed + "/wh").c_str());

  while (true) {
    uint32_t now = millis();
    if (now > nextPub_) {
      while (doOTAUpdate_ == " ") //stops this task while an upload-OTA is running
        delay(1000);
      if (doOTAUpdate_.length()) {
        doUpdate(doOTAUpdate_);
        doOTAUpdate_ = "";
      }
      if (db_.client.connected()) {
        int wins = 0;
        auto pubs = pub_.items(true);
        for (auto i : pubs)
          wins += db_.client.publish((db_.feed + "/" + (i->pref_? "prefs/":"") + i->key).c_str(), i->toString().c_str(), true)? 1 : 0;
        pub_.logNote(str("[pub-%d]", wins));
        pub_.clearDirty();
      } else {
        pub_.logNote("[pub disconnected]");
        doConnect();
      }
      sendOutgoingLogs();
      heap_caps_check_integrity_all(true);
      nextPub_ = now + (psu_->outEn_? db_.period : db_.period * 4); //slower when disabled
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
  s += str(" %0.1fVin -> %0.2fWh ", inVolt_, psu_->wh_) + psu_->toString();
  if (lvProtect_ && lvProtect_->isTriggered()) s += " [LV PROTECTED]";
  s += pub_.popNotes();
  if (psu_->debug_) log(s);
  else Serial.println(s);
}

int Solar::getBackoff(int period) const {
  if (backoffLevel_ <= 0) return period;
  return ((backoffLevel_ * backoffLevel_ + 2) / 2) * period;
}

void Solar::setState(const String state, String reason) {
  if (state_ != state) {
    pub_.setDirty("state");
    log("state change to " + state + " (from " + state_ + ") " + reason);
  }
  state_ = state;
}

int DBConnection::getPort() const {
  int sep = serv.indexOf(':');
  return (sep >= 0)? serv.substring(sep + 1).toInt() : 1883;
}
String DBConnection::getEndpoint() const {
  int sep = serv.indexOf(':');
  return (sep >= 0)? serv.substring(0, sep) : serv;
}

void Solar::doUpdate(String url) {
  log("[OTA] running from " + url);
  sendOutgoingLogs(); //send any outstanding log() messages
  db_.client.disconnect(); //helps to disconnect everything
  esp_task_wdt_init(120, true); //way longer watchdog timeout
  t_httpUpdate_return ret = httpUpdate.update(espClient, url, version_);
  if (ret == HTTP_UPDATE_FAILED) {
    log(str("[OTA] Error (%d):", httpUpdate.getLastError()) + httpUpdate.getLastErrorString());
  } else if (ret == HTTP_UPDATE_NO_UPDATES) {
    log("[OTA] no updates");
  } else if (ret == HTTP_UPDATE_OK) {
    log("[OTA] SUCCESS!!! restarting");
    delay(100);
    ESP.restart();
  }
}


String LowVoltageProtect::toString() const {
  return String(pin_) + (invert_? "i" : "") + str(":%0.2f:%0.2f", threshold_, threshRecovery_);
}
LowVoltageProtect::LowVoltageProtect(String config) {
  StringPair sp1 = split(config, ":");
  invert_ = sp1.first.endsWith("i");
  if (invert_) sp1.first.remove(sp1.first.lastIndexOf("i"));
  pin_ = sp1.first.length()? sp1.first.toInt() : 22;
  if (digitalPinToAnalogChannel(pin_) > 7)
    throw std::runtime_error("sorry, lv-protect pin can't use an ADC2 pin");
  if (sp1.second.length()) {
    StringPair sp2 = split(sp1.second, ":");
    threshold_ = sp2.first.toFloat();
    if (sp2.second.length())
      threshRecovery_ = sp2.second.toFloat();
    else threshRecovery_ = threshold_ * 1.08;
  }
  log("created lvProtect=" + toString());
}

void LowVoltageProtect::trigger(bool trigger) {
  pinMode(pin_, OUTPUT);
  digitalWrite(pin_, !(trigger ^ invert_));
}

bool LowVoltageProtect::isTriggered() const {
  return !(digitalRead(pin_) ^ invert_);
}

//page styling
const String style =
"<style>#file-input,input{width:100%;height:44px;border-radius:4px;margin:10px auto;font-size:15px}"
"input{background:#f1f1f1;border:0;padding:0 15px}body{background:#3498db;font-family:sans-serif;font-size:14px;color:#777}"
"#file-input{padding:0;border:1px solid #ddd;line-height:44px;text-align:left;display:block;cursor:pointer}"
"#bar,#prgbar{background-color:#f1f1f1;border-radius:10px}#bar{background-color:#3498db;width:0%;height:10px}"
"form{background:#fff;max-width:258px;margin:75px auto;padding:30px;border-radius:5px;text-align:center}"
".btn{background:#3498db;color:#fff;cursor:pointer}</style>";

// Update page
const String updateIndex =
"<script src='https://ajax.googleapis.com/ajax/libs/jquery/3.2.1/jquery.min.js'></script>"
"<form method='POST' action='#' enctype='multipart/form-data' id='upload_form'>"
"<input type='file' name='update' id='file' onchange='sub(this)' style=display:none>"
"<label id='file-input' for='file'>   Choose file...</label>"
"<input type='submit' class=btn value='Update'>"
"<br><br>"
"<div id='prg'></div>"
"<br><div id='prgbar'><div id='bar'></div></div><br></form>"
"<script>"
"function sub(obj){"
"var fileName = obj.value.split('\\\\');"
"document.getElementById('file-input').innerHTML = '   '+ fileName[fileName.length-1];"
"};"
"$('form').submit(function(e){"
"e.preventDefault();"
"var form = $('#upload_form')[0];"
"var data = new FormData(form);"
"$.ajax({ url: '/update', type: 'POST', data: data, contentType: false, processData:false,"
"xhr: function() {"
"var xhr = new window.XMLHttpRequest();"
"xhr.upload.addEventListener('progress', function(evt) {"
"if (evt.lengthComputable) {"
"var per = evt.loaded / evt.total;"
"$('#prg').html('progress: ' + Math.round(per*100) + '%');"
"$('#bar').css('width',Math.round(per*100) + '%');"
"}"
"}, false);"
"return xhr;"
"},"
"success:function(d, s) { console.log('success!') },"
"error: function (a, b, c) { }"
"});"
"});"
"</script>" + style;
