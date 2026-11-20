// Single-player cheats (launcher Settings > Cheats). Off unless enabled, and never applied in an online room.
//   DCP_CHEAT_GOD=1      enemy hits do no damage to a local human player's character
//   DCP_CHEAT_GOLD=1     each hero's gold is kept at 99,999,999
//   DCP_CHEAT_DAMAGE=x   damage to enemies times x (0.1..1000); a scaled hit never kills on its own (see cheats.cpp)
//   DCP_CHEAT_XP=x       XP from the results-screen tally times x (1..100), through the game's own level-ups
#pragma once
#include <cstdint>

namespace dcp::cheats {

void install(uint8_t* eboot_base);

}  // namespace dcp::cheats
