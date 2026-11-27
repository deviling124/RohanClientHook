#include "../src/EventGate/GateWall.h"
#include "../src/EventGate/GateWallStore.h"
#include <cstdio>
#include <vector>

namespace {
int checks = 0, failures = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", message); }
}
// The reference client's .text bytes at an address, read from the file on disk.
bool ReferenceMatches(const char* path) {
    FILE* file = nullptr;
    if (fopen_s(&file, path, "rb")) return false;
    std::vector<unsigned char> data;
    unsigned char chunk[65536];
    for (std::size_t read; (read = std::fread(chunk, 1, sizeof chunk, file)) != 0;)
        data.insert(data.end(), chunk, chunk + read);
    std::fclose(file);
    if (data.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(data.data());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(data.data() + dos->e_lfanew);
    std::vector<unsigned char> image(nt->OptionalHeader.SizeOfImage);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        if (section->PointerToRawData + section->SizeOfRawData <= data.size() &&
            section->VirtualAddress + section->SizeOfRawData <= image.size())
            std::memcpy(image.data() + section->VirtualAddress, data.data() + section->PointerToRawData,
                        section->SizeOfRawData);
    return EventGate::MatchesReference(image.data());
}
}

int main(int argc, char** argv) {
    using EventGate::Walls;
    Check(!Walls.Blocked(1000, 1000, 0), "no walls blocks nothing");
    Walls.Apply({"door", true, 1000, 0, 1000, 2000}, 100);
    Check(Walls.Blocked(1000, 1000, 100), "on the line");
    Check(Walls.Blocked(901, 1000, 100) && Walls.Blocked(1099, 1000, 100), "inside the band on both sides");
    Check(!Walls.Blocked(899, 1000, 100) && !Walls.Blocked(1101, 1000, 100), "outside the band");
    Check(Walls.Blocked(1000, 2050, 100) && !Walls.Blocked(1000, 2101, 100), "rounded end");
    Check(Walls.Blocked(1000, 1000, 100 + EventGateWire::ExpiryMs - 1) &&
          !Walls.Blocked(1000, 1000, 100 + EventGateWire::ExpiryMs), "a wall that is not repeated expires");
    Walls.Apply({"door", true, 5000, 0, 5000, 2000}, 200);
    Check(Walls.Blocked(5000, 10, 200) && !Walls.Blocked(1000, 10, 200), "same name moves the wall");
    Walls.Apply({"door", false}, 300);
    Check(!Walls.Blocked(5000, 10, 300), "open removes it");
    Walls.Apply({"unknown", false}, 300);
    for (int i = 0; i < 20; ++i)
        Walls.Apply({"g" + std::to_string(i), true, i * 1000.0f, 0, i * 1000.0f, 500}, 400);
    Check(Walls.Blocked(15000, 10, 400) && !Walls.Blocked(16000, 10, 400), "holds 16 walls, ignores more");
    Walls.Apply({"late", true, 90000, 0, 90000, 500}, 400 + EventGateWire::ExpiryMs);
    Check(Walls.Blocked(90000, 10, 400 + EventGateWire::ExpiryMs), "expired slots are reused");
    Check(argc == 2 && ReferenceMatches(argv[1]), "reference client has the walk check at the expected address");
    std::printf("EventGate walls: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
