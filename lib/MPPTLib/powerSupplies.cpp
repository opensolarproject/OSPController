#include "powerSupplies.h"
#include <stdexcept>
#include <ModbusMaster.h> // ModbusMaster
#include "utils.h"

PowerSupply* PowerSupply::make(const String &type) {
  PowerSupply* ret = NULL;
  String typeUp = type;
  typeUp.toUpperCase();
  if (typeUp.startsWith("DP")) {
    ret = new DPS(&Serial2);
    Serial2.begin(19200, SERIAL_8N1, -1, -1, false, 1000);
  } else if (typeUp.startsWith("DROK")) {
    ret = new Drok(&Serial2);
    Serial2.begin(4800, SERIAL_8N1, -1, -1, false, 1000);
  } else { //default
    ret = NULL;
  }
  if (ret) ret->type_ = type;
  return ret;
}


// ----------------------- //
// ----- PowerSupply ----- //
// ----------------------- //

PowerSupply::PowerSupply() { }
PowerSupply::~PowerSupply() { }

String PowerSupply::toString() const {
  return str("PSU-out[%0.2fV %0.2fA]-lim[%0.2fV %0.2fA]", outVolt_, outCurr_, limitVolt_, limitCurr_)
    + (outEn_? " ENABLED":"") + (isCV()? " CV":"") + (isCC()? " CC":"") + (isCollapsed()? " CLPS":"");
}

bool PowerSupply::isCV() const { return ((limitVolt_ - outVolt_) / limitVolt_) < 0.004; }
bool PowerSupply::isCC() const { return ((limitCurr_ - outCurr_) / limitCurr_) < 0.02; }
bool PowerSupply::isCollapsed() const { return outEn_ && !isCV() && !isCC(); }

void PowerSupply::doTotals() {
  wh_ += outVolt_ * outCurr_ * (millis() - lastAmpUpdate_) / 1000.0 / 60 / 60;
  currFilt_ = currFilt_ - 0.1 * (currFilt_ - outCurr_);
  lastAmpUpdate_ = millis();
}


// ---------------------- //
// -------- Drok -------- //
// ---------------------- //

Drok::Drok(Stream* port) : PowerSupply() { port_ = port; }
Drok::~Drok() { }

bool Drok::begin() {
  flush();
  return doUpdate();
}

bool Drok::doUpdate() {
  bool res = readVoltage() &&
         readCurrent() &&
         readOutputEnabled();
  if (res && !limitVolt_) {
    handleReply(cmdReply("arc")); //read current limit
    handleReply(cmdReply("arv")); //read voltage limit
    log(str("finished PSU begin, got %0.3fV %0.3fA limits\n", limitVolt_, limitCurr_));
  }
  return res;
}

bool Drok::readVoltage() { return handleReply(cmdReply("aru")); }
bool Drok::readCurrent() { return handleReply(cmdReply("ari")); }
bool Drok::readOutputEnabled() { return handleReply(cmdReply("aro")); }

template<typename T>void setCheck(T &save, float in, float max) { if (in < max) save = in; }

bool Drok::handleReply(const String &msg) {
  if (!msg.length()) return false;
  String hdr = msg.substring(0, 3);
  String body = msg.substring(3);
  if      (hdr == "#ro") setCheck(outEn_, (body.toInt() == 1), 2);
  else if (hdr == "#ru") setCheck(outVolt_, body.toFloat() / 100.0, 80);
  else if (hdr == "#rv") setCheck(limitVolt_, body.toFloat() / 100.0, 80);
  else if (hdr == "#ra") setCheck(limitCurr_, body.toFloat() / 100.0, 15);
  else if (hdr == "#ri") {
    setCheck(outCurr_, body.toFloat() / 100.0, 15);
    doTotals();
  } else {
    log("PSU got unknown msg > '" + hdr + "' / '" + body + "'");
    return false;
  }
  lastSuccess_ = millis();
  return true;
}

void Drok::flush() {
  port_->flush();
}

String Drok::cmdReply(const String &cmd) {
  port_->print(cmd + "\r\n");
  if (debug_) log("PSU > " + cmd + "CRLF");
  String reply;
  uint32_t start = millis();
  char c;
  while ((millis() - start) < 1000 && !reply.endsWith("\n"))
    if (port_->readBytes(&c, 1))
      reply.concat(c);
  if (debug_ && reply.length()) {
    String debug = reply;
    debug.replace("\r", "CR");
    debug.replace("\n", "NL");
    log("PSU < " + debug);
  }
  if (!reply.length() && debug_ && port_->available())
    log("PSU nothing read.. stuff available!? " + String(port_->available()));
  reply.trim();
  return reply;
}

bool Drok::enableOutput(bool status) {
  String r = cmdReply(str("awo%d\r\n", status ? 1 : 0));
  if (r == "#wook") {
    outEn_ = status;
    return true;
  } else return false;
}

String Drok::fourCharStr(uint16_t input) {
  char buf[] = "    ";
  // Iterate through units, tens, hundreds etc
  for (int digit = 0; digit < 4; digit++)
    buf[3 - digit] = ((input / ((int) pow(10, digit))) % 10) + '0';
  return String(buf);
}

bool Drok::setVoltage(float v) {
  limitVolt_ = v;
  String r = cmdReply("awu" + fourCharStr(v * 100.0));
  return (r == "#wuok");
}

bool Drok::setCurrent(float v) {
  limitCurr_ = v;
  String r = cmdReply("awi" + fourCharStr(v * 100.0));
  return (r == "#wiok");
}


// ----------------------- //
// --------- DPS --------- //
// ----------------------- //

DPS::DPS(Stream* port) : PowerSupply(), bus_(new ModbusMaster) { port_ = port; }
DPS::~DPS() { }

bool DPS::begin() {
  bus_->begin(1, *port_);
  return doUpdate();
}

bool DPS::doUpdate() {
  //read a range of 16-bit registers starting at register 0 to 10
  try {
    if (bus_->readHoldingRegisters(0, 10) == bus_->ku8MBSuccess) {
      outVolt_    = ((float)bus_->getResponseBuffer(2) / 100 );
      outCurr_    = ((float)bus_->getResponseBuffer(3) / 1000 );
      inputVolts_ = ((float)bus_->getResponseBuffer(5) / 100 );
      // float power = ((float)bus_->getResponseBuffer(4) / 100 );
      limitVolt_  = ((float)bus_->getResponseBuffer(0) / 100 );
      limitCurr_  = ((float)bus_->getResponseBuffer(1) / 1000 );
      outEn_      = ((bool)bus_->getResponseBuffer(9) );
      cc_      = ((bool)bus_->getResponseBuffer(8) );
      doTotals();
      lastSuccess_ = millis();
      return true;
    }
  } catch (std::runtime_error e) {
    log("caught exception in DPS::update " + String(e.what()));
  } catch (...) {
    log("caught unknown exception in DPS::update");
  }
  return false;
}
bool DPS::enableOutput(bool en) {
  return bus_->writeSingleRegister(9, en) == bus_->ku8MBSuccess;
}

bool DPS::setVoltage(float v) {
  return bus_->writeSingleRegister(0, ((limitVolt_ = v)) * 100) == bus_->ku8MBSuccess;
}
bool DPS::setCurrent(float c) {
  return bus_->writeSingleRegister(1, ((limitCurr_ = c)) * 1000) == bus_->ku8MBSuccess;
}

bool DPS::isCC() const { return cc_; }

bool DPS::getInputVolt(float* v) const {
  if (v) *v = inputVolts_;
  return true;
}
