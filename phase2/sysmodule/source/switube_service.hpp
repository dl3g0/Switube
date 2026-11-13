#pragma once

#include "switube/service_core.hpp"

#include <switch.h>

namespace switube::phase2::switch_service {

Result initialize(ServiceCore& core);
Result exit();
void loop();

}  // namespace switube::phase2::switch_service
