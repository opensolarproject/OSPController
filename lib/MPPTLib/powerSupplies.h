#pragma once
#include <cstdint>
#include <WString.h>

class Stream;

class PowerSupply {
  public:
    String type_;
    Stream *port_ = NULL;
    bool debug_ = false;
    float outVolt_ = 0, outCurr_ = 0;
    float limitVolt_ = 0, limitCurr_ = 0;
    float currFilt_ = 0.0, wh_ = 0;
    bool outEn_ = false;
    uint32_t lastSuccess_ = 0, lastAmpUpdate_ = 0;

    static PowerSupply* make(const String &type);
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
    String getType() const { return type_; }
    virtual bool isDrok() const { return true; }
  protected:
    void doTotals();
};

class Drok : public PowerSupply {
  public:
    Drok(Stream*);
    ~Drok();
    bool begin() override;

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
    ModbusMaster* bus_;
  public:
    float inputVolts_ = 0;
    bool cc_ = false;

    DPS(Stream*);
    ~DPS();
    bool begin() override;

    bool setVoltage(float) override;
    bool setCurrent(float) override;
    bool enableOutput(bool) override;

    bool doUpdate() override; //runs these next three:

    bool isCC() const override;
    bool getInputVolt(float* v) const override;
    bool isDrok() const override { return false; }
};
