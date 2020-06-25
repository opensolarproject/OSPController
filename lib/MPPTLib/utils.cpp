#include <Arduino.h>
#include <rom/rtc.h>
#include "publishable.h"
#include "utils.h"


float mapfloat(long x, long in_min, long in_max, long out_min, long out_max) {
  return (float)(x - in_min) * (out_max - out_min) / (float)(in_max - in_min) + out_min;
}


String str(const char *fmtStr, ...) {
  static char buf[201] = {'\0'};
  va_list arg_ptr;
  va_start(arg_ptr, fmtStr);
  vsnprintf(buf, 200, fmtStr, arg_ptr);
  va_end(arg_ptr);
  return String(buf);
}
String str(const std::string &s) {
  return String(s.c_str());
}
String str(bool v) {
  return v ? " WIN" : " FAIL";
}

// float lifepo4_soc[] = {13.4, 13.3, 13.28, 13.};

Publishable* pub_; //static
void log(const String &s) { pub_->log(s); }
void addLogger(Publishable* p) { pub_ = p; }

String timeAgo(int sec) {
  int days = (sec / (3600 * 24));
  String ret = str("%ds", sec % 60);
  if (sec >= 60  ) ret = str("%dm ", (sec % 3600) / 60) + ret;
  if (sec >= 3600) ret = str("%dh ", ((sec % (3600 * 24)) / 3600)) + ret;
  if (days)        ret = str("%dd ", days % 365) + ret;
  if (days >= 365) ret = str("%dy ", days / 365) + ret;
  return ret;
}


PowerSupply::PowerSupply(Stream &port) : _port(&port), debug_(false) { }

bool PowerSupply::begin() {
  flush();
  return doUpdate();
}

bool PowerSupply::doUpdate() {
  bool res = readVoltage() &&
         readCurrent() &&
         getOutputEnabled();
  if (res && !limitVolt_) {
    handleReply(cmdReply("arc")); //read current limit
    handleReply(cmdReply("arv")); //read voltage limit
    log(str("finished PSU begin, got %0.3fV %0.3fA limits\n", limitVolt_, limitCurr_));
  }
  return res;
}

bool PowerSupply::isCV() const { return ((limitVolt_ - outVolt_) / limitVolt_) < 0.004; }
bool PowerSupply::isCC() const { return ((limitCurr_ - outCurr_) / limitCurr_) < 0.02; }
bool PowerSupply::isCollapsed() const { return outEn_ && !isCV() && !isCC(); }

String PowerSupply::toString() const {
  return str("PSU-out[%0.2fV %0.2fA]-lim[%0.2fV %0.2fA]", outVolt_, outCurr_, limitVolt_, limitCurr_)
    + (outEn_? " ENABLED":"") + (isCV()? " CV":"") + (isCC()? " CC":"") + (isCollapsed()? " CLPS":"");
}


bool PowerSupply::readVoltage() { return handleReply(cmdReply("aru")); }
bool PowerSupply::readCurrent() { return handleReply(cmdReply("ari")); }
bool PowerSupply::getOutputEnabled() { return handleReply(cmdReply("aro")); }

template<typename T>void setCheck(T &save, float in, float max) { if (in < max) save = in; }

bool PowerSupply::handleReply(const String &msg) {
  if (!msg.length()) return false;
  String hdr = msg.substring(0, 3);
  String body = msg.substring(3);
  if      (hdr == "#ro") setCheck(outEn_, (body.toInt() == 1), 2);
  else if (hdr == "#ru") setCheck(outVolt_, body.toFloat() / 100.0, 80);
  else if (hdr == "#rv") setCheck(limitVolt_, body.toFloat() / 100.0, 80);
  else if (hdr == "#ra") setCheck(limitCurr_, body.toFloat() / 100.0, 15);
  else if (hdr == "#ri") {
    setCheck(outCurr_, body.toFloat() / 100.0, 15);
    wh_ += outVolt_ * outCurr_ * (millis() - lastAmpUpdate_) / 1000.0 / 60 / 60;
    currFilt_ = currFilt_ - 0.1 * (currFilt_ - outCurr_);
    lastAmpUpdate_ = millis();
  } else {
    log("PSU got unknown msg > '" + hdr + "' / '" + body + "'");
    return false;
  }
  lastSuccess_ = millis();
  return true;
}

void PowerSupply::flush() {
  _port->flush();
}

String PowerSupply::cmdReply(const String &cmd) {
  _port->print(cmd + "\r\n");
  if (debug_) log("PSU > " + cmd + "CRLF");
  String reply;
  uint32_t start = millis();
  char c;
  while ((millis() - start) < 1000 && !reply.endsWith("\n"))
    if (_port->readBytes(&c, 1))
      reply.concat(c);
  if (debug_ && reply.length()) {
    String debug = reply;
    debug.replace("\r", "CR");
    debug.replace("\n", "NL");
    log("PSU < " + debug);
  }
  if (!reply.length() && debug_ && _port->available())
    log("PSU nothing read.. stuff available!? " + String(_port->available()));
  reply.trim();
  return reply;
}

bool PowerSupply::enableOutput(bool status) {
  String r = cmdReply(str("awo%d\r\n", status ? 1 : 0));
  if (r == "#wook") {
    outEn_ = status;
    return true;
  } else return false;
}

String PowerSupply::fourCharStr(uint16_t input) {
  char buf[] = "    ";
  // Iterate through units, tens, hundreds etc
  for (int digit = 0; digit < 4; digit++)
    buf[3 - digit] = ((input / ((int) pow(10, digit))) % 10) + '0';
  return String(buf);
}

bool PowerSupply::setVoltage(float v) {
  limitVolt_ = v;
  String r = cmdReply("awu" + fourCharStr(v * 100.0));
  return (r == "#wuok");
}

bool PowerSupply::setCurrent(float v) {
  limitCurr_ = v;
  String r = cmdReply("awi" + fourCharStr(v * 100.0));
  return (r == "#wiok");
}

String getResetReason(RESET_REASON r) {
  switch (r) {
    case NO_MEAN: return "";
    case POWERON_RESET         : return "Vbat power on reset";
    case SW_RESET              : return "Software reset digital core";
    case OWDT_RESET            : return "Legacy watch dog reset digital core";
    case DEEPSLEEP_RESET       : return "Deep Sleep reset digital core";
    case SDIO_RESET            : return "Reset by SLC module, reset digital core";
    case TG0WDT_SYS_RESET      : return "Timer Group0 Watch dog reset digital core";
    case TG1WDT_SYS_RESET      : return "Timer Group1 Watch dog reset digital core";
    case RTCWDT_SYS_RESET      : return "RTC Watch dog Reset digital core";
    case INTRUSION_RESET       : return "Instrusion tested to reset CPU";
    case TGWDT_CPU_RESET       : return "Time Group reset CPU";
    case SW_CPU_RESET          : return "Software reset CPU";
    case RTCWDT_CPU_RESET      : return "RTC Watch dog Reset CPU";
    case EXT_CPU_RESET         : return "for APP CPU, reseted by PRO CPU";
    case RTCWDT_BROWN_OUT_RESET: return "Reset when the vdd voltage is not stable";
    case RTCWDT_RTC_RESET      : return "RTC Watch dog reset digital core and rtc module";
  }
  return "";
}

String getResetReasons() {
  return "Reset0: " + getResetReason(rtc_get_reset_reason(0)) + ". Reason1: " + getResetReason(rtc_get_reset_reason(1));
}