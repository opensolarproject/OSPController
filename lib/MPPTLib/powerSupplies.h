#pragma once
#include <cstdint>

class String;
class Stream;

class PowerSupply {
  public:
    bool debug_ = false;
    float outVolt_ = 0, outCurr_ = 0;
    float limitVolt_ = 0, limitCurr_ = 0;
    float currFilt_ = 0.0, wh_ = 0;
    bool outEn_ = false;
    uint32_t lastSuccess_ = 0, lastAmpUpdate_ = 0;

    PowerSupply();
    virtual ~PowerSupply();
    virtual bool begin() = 0;
    virtual bool doUpdate() = 0;
    virtual bool readCurrent() { return doUpdate(); };

    virtual bool setVoltage(float) = 0;
    virtual bool setCurrent(float) = 0;
    virtual bool enableOutput(bool) = 0;

    virtual bool isCV() const;
    virtual bool isCC() const;
    virtual bool isCollapsed() const;
    virtual bool getInputVolt(float* v) const { return false; }
    virtual String toString() const;
    virtual String getType() const = 0;
  protected:
    void doTotals();
};

class Drok : public PowerSupply {
    Stream *port_;
  public:
    Drok(Stream* port);
    ~Drok();
    bool begin() override;
    virtual String getType() const;

    String cmdReply(const String &cmd);
    bool setVoltage(float) override;
    bool setCurrent(float) override;
    bool enableOutput(bool) override;

    bool doUpdate() override; //runs these next three:
    bool readCurrent() override;
    bool readVoltage();
    bool readOutputEnabled();
    void flush();

  private:
    bool handleReply(const String &);
    String fourCharStr(uint16_t input);
};

class ModbusMaster;

class DPS : public PowerSupply {
    Stream *port_;
    ModbusMaster* bus_;
  public:
    float inputVolts_ = 0;
    bool cc_ = false;

    DPS(Stream* port);
    ~DPS();
    bool begin() override;
    virtual String getType() const;

    bool setVoltage(float) override;
    bool setCurrent(float) override;
    bool enableOutput(bool) override;

    bool doUpdate() override; //runs these next three:

    bool isCC() const override;
    bool getInputVolt(float* v) const override;
};
