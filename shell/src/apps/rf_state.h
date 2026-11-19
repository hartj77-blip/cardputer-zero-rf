// What the operator last tuned to, shared between RF apps while the shell
// runs (Spectrum sets it; Logbook fills a new contact from it).
#pragma once

#include <string>

namespace zs {

inline double g_rf_last_hz = 0;
inline std::string g_rf_last_mode;   // daemon mode name, "" when not listening

}  // namespace zs
