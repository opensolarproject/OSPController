#include <solar.h>

//GIT_VERSION pulled from platformio.ini src_build_flags, only for this file
Solar controller(GIT_VERSION);

void setup() {
  controller.setup();
}
void loop() {
  controller.loop();
}
