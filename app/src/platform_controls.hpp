#pragma once

namespace swplatform {

void initialize();
void shutdown();
void preventSleep(bool enable);
bool toggleScreenOff();
bool isScreenOff();

}  // namespace swplatform
