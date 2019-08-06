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
double setpoint_ = 0, pgain_ = 0.2;
float vadjust_ = 105.0;

WebServer server(80);
WiFiClient espClient;
PubSubClient psClient(espClient);
#define MQTT_USERNAME ""
char apikey[] = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
Publishable varManager(&server, &psClient);

void setup() {
  Serial.begin(115200);
  delay(100);
  uint64_t chipid = ESP.getEfuseMac();
  Serial.printf("startup, ID %08X %04X\n", chipid, (uint16_t)(chipid >> 32));
  Serial2.begin(4800, SERIAL_8N1, 16, 17, false, 1000);
  analogSetCycles(32);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
    Serial.println("wifi event");
  });
  WiFi.begin();//TODO load from prefs
  WiFi.setHostname(str("mpptESP-%02X", chipid & 0xff).c_str());
  if (WiFi.waitForConnectResult() == WL_CONNECTED) {
    MDNS.begin("mppt");
    MDNS.addService("http", "tcp", 80);
  }
  varManager.add("pgain", pgain_);
  varManager.add("pgain", setpoint_, 0);
  varManager.add("outputEN",psu.outEn_); //TODO setter: psu.enableOutput()
  varManager.add("outvolt", psu.outVolt_); //TODO setter: psu.setVoltage ()
  varManager.add("outcurr", psu.outCurr_); //TODO setter: psu.setCurrent()
  // varManager.add("outpower", outPower_); //TODO add getter std::function option
  varManager.add("involt", inVolt_);
  varManager.add("wh", wh_); //TODO disable setter

  server.on("/", HTTP_ANY, []() {
    Serial.println("got req " + server.uri() + " -> " + server.hostHeader());
    String ;
    server.sendHeader("Connection", "close");
    if (! ret.length()) ret = getState();
    server.send(200, "application/json", ret.c_str());
  });
  pubsubConnect();
  server.begin();
//  xTaskCreate([](void*) {
//    while (true) { }
//  }, "psu", 10000, NULL, 1, NULL); //fn, name, stack size, parameter, priority, handle
  Serial.println("finished setup");
}

void pubsubConnect() {
  Serial.println("Connecting pubsub with key " + String(apikey));
  psClient.setServer("io.adafruit.com", 1883);
  if (psClient.connect("MPPT", MQTT_USERNAME, apikey))
    Serial.println("PubSub connect success! " + psClient.state());
  else Serial.println("PubSub connect ERROR! " + psClient.state());
}

uint32_t lastV = 0, lastpub = 20000, lastLog_ = 0;
uint32_t lastPSUpdate_ = 0, lastPSUadjust_ = 1000;
double newDesiredCurr_ = 0;
bool needsQuickAdj_ = false;
String logme;
int analogval = 0;

void applyAdjustment() {
  if (newDesiredCurr_ > 0) {
    if (psu.setCurrent(newDesiredCurr_))
      logme += str("[adjusting %0.1fA (from %0.1fA)] ", newDesiredCurr_ - psu.outCurr_, psu.outCurr_);
    else Serial.println("error setting current");
    psu.readCurrent();
    if (needsQuickAdj_)
      needsQuickAdj_ = false;
    printStatus();
  }
  newDesiredCurr_ = 0;
}

void loop() {
  uint32_t now = millis();
  if ((now - lastV) >= 200) {
    analogval = analogRead(VMEASURE_PIN);
    inVolt_ = analogval * 3.3 * (vadjust_ / 3.3) / 4096.0;//mapfloat(analogval, 2743, 2906, 74.8, 80.0); //
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
    psClient.loop();
    lastV = millis();
  }
  if ((now - lastPSUadjust_) >= (needsQuickAdj_? 500 : 5000)) {
    if (setpoint_ > 0) {
      if (psu.outEn_)
        applyAdjustment();
      if (psu.outEn_ && inVolt_ < (62.0)) { //collapse detection
        newDesiredCurr_ = psu.outCurr_ / 3;
        Serial.printf("collapsed! %0.1fV set recovery %0.1fA\n", inVolt_ ,newDesiredCurr_);
        psu.enableOutput(false);
        psu.outEn_ = false;
        psu.setCurrent(newDesiredCurr_);
      } else if (!psu.outEn_) {
        Serial.println("restoring from collapse");
        psu.enableOutput(true);
      }
    }
    lastPSUadjust_ = now;
  }
  if ((now - lastLog_) >= 1000) {
    printStatus();
    lastLog_ = now;
  }
  if ((now - lastPSUpdate_) >= 5000) {
    bool res = psu.doUpdate();
    if (res) wh_ += psu.outVolt_ * psu.outCurr_ * (now - lastPSUpdate_) / 1000.0 / 60 / 60;
    else {
      Serial.println("psu update fail");
      psu.flush();
      psu.debug_ = true;
    }
    lastPSUpdate_ = now;
  }
  if ((now - lastpub) >= 12000) {
    if (psClient.connected() && psu.outVolt_ > 0.0) {
      if (psClient.publish(MQTT_USERNAME"/feeds/outvolt", String(psu.outVolt_,2).c_str()) &&
          psClient.publish(MQTT_USERNAME"/feeds/outcurr", String(psu.outCurr_,2).c_str()) &&
          psClient.publish(MQTT_USERNAME"/feeds/outpower", String(psu.outVolt_ * psu.outCurr_,2).c_str()) &&
          psClient.publish(MQTT_USERNAME"/feeds/involt", String(inVolt_,2).c_str()) &&
          psClient.publish(MQTT_USERNAME"/feeds/wh", String(wh_,3).c_str()))
        logme += "[published]";
    }
    lastpub = now;
  }
  server.handleClient();
}

void printStatus() {
  Serial.println(str("%d %0.1fVin -> %0.2fWh [%0.1fV out %0.1fA %den] ", analogval, inVolt_, wh_, psu.outVolt_, psu.outCurr_, psu.outEn_) + logme);
  logme = "";
}

String getState() {
  return str("{\"outvolt\":%0.2f,\"outcurr\":%0.2f,\"involt\":%0.2f}", psu.outVolt_, psu.outCurr_, inVolt_);
}
