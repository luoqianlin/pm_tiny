#pragma once

#include <cstddef>
#include <cstdint>

namespace pm_tiny {
namespace fuzz {

void protocol_input(const std::uint8_t *data, std::size_t size);
void config_input(const std::uint8_t *data, std::size_t size);
void log_input(const std::uint8_t *data, std::size_t size);

} // namespace fuzz
} // namespace pm_tiny
