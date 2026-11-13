#pragma once

#include <cstdint>
#include <string>

namespace switube::artwork {

bool initialize();
void shutdown();
void request(const std::string& url, int targetSize);
void borrow(const uint8_t** pixels, int* width, int* height);

}  // namespace switube::artwork
