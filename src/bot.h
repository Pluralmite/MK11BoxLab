// MK11BoxLab - scripted test inputs ("bot"): lets the tool drive P1 with XInput so moves can be
// tested without a person at the controller. Commands are read from MK11BoxLab_cmd.txt next to the mod.
#pragma once
#include <cstdint>
#include <string>
#include "mk11.h"

namespace bot {
void PollCommandFile(const std::string& dir);   // render thread, throttled
void OnSimFrame(const mk::Snapshot& sn);       // once per simulation frame
void OnRenderFrame();                           // every rendered frame (real-time steps)
bool Running();
std::string Status();
// read by the XInput hook
bool Override(uint16_t& buttons);
}
