#pragma once

#include <string>
#include <vector>

#include "runtime.h"

int game_launcher_preboot(std::vector<std::string>& args,
                          const gbarecomp::RunOptions& opts);
