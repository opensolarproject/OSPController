#include "utils.h"
#include "publishable.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <PubSubClient.h>

PowerSupply psu(Serial2);
const uint8_t VMEASURE_PIN = 36;
float inVolt_ = 0, wh_ = 0;
double setpoint_ = 0, pgain_ = 0.1;
int collapses_ = 0; //collapses, reset every.. minute?
int measperiod_ = 100, printPeriod_ = 1000, pubPeriod_ = 12000, psuperiod_ = 2000;
float vadjust_ = 105.0;
bool autoStart_ = true;
String wifiap, wifipass;
String mqttServ, mqttUser, mqttPass, mqttFeed;
uint32_t ignoreSubsUntil_;

WebServer server(80);
WiFiClient espClient;
PubSubClient psClient(espClient);
Publishable pub;

void setup() {
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
  pub.add("wifiap",     wifiap).hide().pref();
  pub.add("wifipass", wifipass).hide().pref();
  pub.add("mqttServ", mqttServ).hide().pref();
  pub.add("mqttUser", mqttUser).hide().pref();
  pub.add("mqttPass", mqttPass).hide().pref();
  pub.add("mqttFeed", mqttFeed).hide().pref();
  pub.add("outputEN",[](String s){ if (s.length()) psu.enableOutput(s == "on"); return String(psu.outEn_); });
  pub.add("outvolt", [](String s){ if (s.length()) psu.setVoltage(s.toFloat()); return String(psu.outVolt_); });
  pub.add("outcurr", [](String s){ if (s.length()) psu.setCurrent(s.toFloat()); return String(psu.outCurr_); });
  pub.add("outpower",[](String s){ return String(psu.outVolt_ * psu.outCurr_); });
  pub.add("pgain",      pgain_          ).pref();
  pub.add("setpoint",   setpoint_       ).pref();
  pub.add("vadjust",    vadjust_        ).pref();
  pub.add("autostart",  autoStart_      ).pref();
  pub.add("printperiod",printPeriod_    ).pref();
  pub.add("pubperiod",  pubPeriod_      ).pref();
  pub.add("PSUperiod",  psuperiod_      ).pref();
  pub.add("measperiod", measperiod_     ).pref();
  pub.add("involt",  inVolt_);
  pub.add("wh",      wh_    );
  pub.add("collapses",  collapses_);
  pub.add("connect",[](String s){ doConnect(); return "connected"; }).hide();
  pub.add("disconnect",[](String s){ psClient.disconnect(); WiFi.disconnect(); return "dissed"; }).hide();
  pub.add("restart",[](String s){ ESP.restart(); return ""; }).hide();
  pub.add("clear",[](String s){ pub.clearPrefs(); return "cleared"; }).hide();

  server.on("/", HTTP_ANY, []() {
    Serial.println("got req " + server.uri() + " -> " + server.hostHeader());
    String ret;
    for (int i = 0; i < server.args(); i++)
      ret += pub.handleSet(server.argName(i), server.arg(i)) + "\n";
    server.sendHeader("Connection", "close");
    if (! ret.length()) ret = pub.toJson();
    server.send(200, "application/json", ret.c_str());
  });
  pub.loadPrefs();
  // wifi & mqtt is connected by pubsubConnect below

  psu.flush();
  psu.doUpdate();

  xTaskCreate(publishTask, "publish", 10000, NULL, 1, NULL); //fn, name, stack size, parameter, priority, handle
  Serial.println("finished setup");
}

void doConnect() {
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
        server.begin();
      }
    } else Serial.println("no wifiap or wifipass set!");
  }
  if (WiFi.isConnected() && !psClient.connected()) {
    if (mqttServ.length() && mqttFeed.length()) {
      Serial.println("Connecting MQTT to " + mqttUser + "@" + mqttServ);
      psClient.setServer(mqttServ.c_str(), 1883); //TODO split serv:port
      if (psClient.connect("MPPT", mqttUser.c_str(), mqttPass.c_str()))
        Serial.println("PubSub connect success! " + psClient.state());
        auto pubs = pub.items(true);
        for (auto i : pubs)
          if (i->pref_)
            psClient.subscribe((mqttFeed + "/prefs/" + i->key).c_str()); //subscribe to preference changes!
      else Serial.println("PubSub connect ERROR! " + psClient.state());
    } else Serial.println("no MQTT user / pass / server / feed set up!");
  } else Serial.printf("can't pub connect, wifi %d pub %d\n", WiFi.isConnected(), psClient.connected());
}

uint32_t lastV = 0, lastpub = 20000, lastLog_ = 0;
uint32_t lastPSUpdate_ = 0, lastPSUadjust_ = 1000, lastCollapseReset_ = 0;
double newDesiredCurr_ = 0;
bool needsQuickAdj_ = false;
String logme;

void applyAdjustment() {
  if (newDesiredCurr_ > 0) {
    if (psu.setCurrent(newDesiredCurr_))
      logme += str("[adjusting %0.1fA (from %0.1fA)] ", newDesiredCurr_ - psu.outCurr_, psu.outCurr_);
    else Serial.println("error setting current");
    psu.readCurrent();
    pub.setDirty({"outcurr", "outpower"});
    if (needsQuickAdj_)
      needsQuickAdj_ = false;
    printStatus();
  }
  newDesiredCurr_ = 0;
}

void loop() {
  uint32_t now = millis();
  if ((now - lastV) >= measperiod_) {
    int analogval = analogRead(VMEASURE_PIN);
    inVolt_ = analogval * 3.3 * (vadjust_ / 3.3) / 4096.0;
    pub.setDirty(&inVolt_);
    if (setpoint_ > 0 && psu.outEn_) { //corrections enabled
      double error = inVolt_ - setpoint_;
      double dcurr = constrain(error * pgain_, -3, 1); //limit ramping speed
      if (error > 0.3 || (-error > 0.2)) { //adjustment deadband, more sensitive when needing to ramp down
        newDesiredCurr_ = psu.outCurr_ + dcurr;
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
      if (psu.outEn_)
        applyAdjustment();
      if (psu.outEn_ && inVolt_ < (psu.outVolt_ * 3 / 2)) { //collapse detection
        newDesiredCurr_ = psu.outCurr_ / 2;
        ++collapses_;
        pub.setDirty(&collapses_);
        Serial.printf("collapsed! %0.1fV set recovery to %0.1fA\n", inVolt_ ,newDesiredCurr_);
        psu.enableOutput((psu.outEn_ = false));
        psu.setCurrent(newDesiredCurr_);
      } else if (autoStart_ && !psu.outEn_ && inVolt_ > (setpoint_ * 1.02)) {
        Serial.println("restoring from collapse");
        psu.enableOutput(true);
      }
    }
    lastPSUadjust_ = now;
  }
  if ((now - lastLog_) >= printPeriod_) {
    printStatus();
    lastLog_ = now;
  }
  if ((now - lastPSUpdate_) >= 5000) {
    bool res = psu.doUpdate();
    if (res) {
      wh_ += psu.outVolt_ * psu.outCurr_ * (now - lastPSUpdate_) / 1000.0 / 60 / 60;
      pub.setDirty({"outvolt", "outcurr", "outputEN", "wh", "outpower"});
    } else {
      Serial.println("psu update fail");
      psu.flush();
      //psu.debug_ = true;
    }
    lastPSUpdate_ = now;
  }
}

void publishTask(void*) {
  doConnect();
  psClient.loop();
  ignoreSubsUntil_ = millis() + 3000;
  psClient.setCallback([](char*topicbuf, uint8_t*buf, unsigned int len){
    String topic(topicbuf), val = str(std::string((char*)buf, len));
    Serial.println("got sub value " + topic + " -> " + val);
    if (topic == (mqttFeed + "/wh")) {
      wh_ = val.toFloat();
      Serial.println("restored wh value to " + val);
      psClient.unsubscribe((mqttFeed + "/wh").c_str());
    } else if (millis() > ignoreSubsUntil_) { //don't load old values
      topic.replace(mqttFeed + "/prefs/", ""); //replaces in-place, sadly
      String ret = pub.handleSet(topic, val);
      Serial.println("MQTT cmd: " + ret);
    }
  });
  psClient.subscribe((mqttFeed + "/wh").c_str());

  while (true) {
    uint32_t now = millis();
    if ((now - lastpub) >= (psu.outEn_? pubPeriod_ : pubPeriod_ * 4)) { //slow-down when not enabled
      if (psClient.connected()) {
        int wins = 0;
        auto pubs = pub.items(true);
        for (auto i : pubs) {
          wins += psClient.publish((mqttFeed + "/" + (i->pref_? "prefs/":"") + i->key).c_str(), i->toString().c_str(), true)? 1 : 0;
          if (i->pref_) ignoreSubsUntil_ = now + 3000;
        }
        logme += str("[published %d] ", wins);
        pub.clearDirty();
      } else {
        logme += "[pub disconnected] ";
        doConnect();
      }
      lastpub = now;
    }
    if ((now - lastCollapseReset_) >= 60000) {
      collapses_ = 0;
      pub.setDirty(&collapses_);
      lastCollapseReset_ = now;
    }
    psClient.loop();
    pub.poll(&Serial);
    server.handleClient();
    delay(1);
  }
}

void printStatus() {
  Serial.println(str("%0.1fVin -> %0.2fWh <%0.1fV out %0.1fA %den> ", inVolt_, wh_, psu.outVolt_, psu.outCurr_, psu.outEn_) + logme);
  logme = "";
}
