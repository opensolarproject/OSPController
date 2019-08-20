#pragma once
#include "Arduino.h"

class PowerSupply {
    Stream *_port;
  public:
    bool debug_;
    float outVolt_ = 0, outCurr_ = 0;
    float limitVolt_ = 0, limitCurr_ = 0;
    bool outEn_ = false;

    PowerSupply(Stream &port);
    bool begin();

    String cmdReply(String cmd);
    bool setVoltage(float);
    bool setCurrent(float);
    bool enableOutput(bool);

    bool doUpdate(); //runs these next three:
    bool readVoltage();
    bool readCurrent();
    bool getOutputEnabled();
    void flush();

  private:
    bool handleReply(String);
    String fourCharStr(uint16_t input);
};

float mapfloat(long x, long in_min, long in_max, long out_min, long out_max);

extern const char* adafruitRootCert;

String str(const char *fmtStr, ...);
String str(std::string s);
String str(bool v);
