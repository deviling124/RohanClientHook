#pragma once
#include <string_view>

// Private server -> client signal: "another player hit you, stop the game's Hunter Kit". It travels as a Shout
// (0x1F04) to the victim only, with sender id 0 and this reserved sender name; the client consumes it.
// Mirrored in RohanClient_Hook/shared; keep both copies identical.
namespace HunterKitStopWire {
constexpr std::string_view Source = "[HunterKit]";
constexpr std::string_view Stop = "HKS1 stop";
}
