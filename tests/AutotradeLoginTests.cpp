#include "../src/Auth/AutotradeLogin.cpp"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int failures = 0, lastResult = 0;
bool nested = false, shouldThrow = false;
std::string title, firstLine, secondLine, nestedLine;

void Check(bool condition, const char* message) {
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}

const char* __fastcall FakeString(void* self, void*, int) {
    Check(self == reinterpret_cast<void*>(0x1234), "native string owner forwarded");
    return "native text";
}

unsigned __fastcall FakeLoginResult(void* self, void*, int result) {
    Check(self == reinterpret_cast<void*>(0x1234), "native login owner forwarded");
    lastResult = result;
    if (shouldThrow) throw std::runtime_error("fixture");
    if (result == -2) {
        nestedLine = AutotradeLogin::LookupString(self, nullptr, 0x3a);
        return 88;
    }
    title = AutotradeLogin::LookupString(self, nullptr, 0x39);
    if (nested) {
        AutotradeLogin::LoginResult(self, nullptr, -2);
        lastResult = result;
    }
    firstLine = AutotradeLogin::LookupString(self, nullptr, 0x3a);
    secondLine = AutotradeLogin::LookupString(self, nullptr, 0x3b);
    Check(!std::strcmp(AutotradeLogin::LookupString(self, nullptr, 0x4e4a), "native text"),
        "unrelated resource IDs use native text");
    return 77;
}

void TestFingerprint() {
    std::ifstream file("00_reference/rohanclient_unpacked.exe", std::ios::binary);
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    Check(bytes.size() > 0x1000, "reference client available");
    if (bytes.size() <= 0x1000) return;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(bytes.data() + dos->e_lfanew);
    std::vector<unsigned char> image(nt->OptionalHeader.SizeOfImage);
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (section[i].PointerToRawData + section[i].SizeOfRawData <= bytes.size() &&
            section[i].VirtualAddress + section[i].SizeOfRawData <= image.size()) {
            std::memcpy(image.data() + section[i].VirtualAddress,
                bytes.data() + section[i].PointerToRawData, section[i].SizeOfRawData);
        }
    }
    Check(AutotradeLogin::MatchesReference(image.data()), "popup hooks match the supplied client binary");
    image[0x871a70 - 0x400000] ^= 1;
    Check(!AutotradeLogin::MatchesReference(image.data()), "modified login dispatcher is rejected");
}
}

int main() {
    AutotradeLogin::originalLoginResult = reinterpret_cast<AutotradeLogin::LoginResultFn>(FakeLoginResult);
    AutotradeLogin::originalString = reinterpret_cast<AutotradeLogin::StringFn>(FakeString);
    auto* owner = reinterpret_cast<void*>(0x1234);
    Check(AutotradeLogin::LoginResult(owner, nullptr, AutotradeLogin::Result) == 77 && lastResult == -23,
        "autotrade result enters the existing native confirmation");
    Check(title == "Autotrade" && firstLine == "Your character is currently in autotrade." &&
        secondLine == "Close your offline Kiosk and log in?", "autotrade popup explains the action");
    Check(!std::strcmp(AutotradeLogin::LookupString(owner, nullptr, 0x3a), "native text"),
        "custom text expires after dialog construction");
    AutotradeLogin::LoginResult(owner, nullptr, -23);
    Check(lastResult == -23 && title == "native text" && firstLine == "native text",
        "ordinary already-logged-in confirmation keeps native wording");
    nested = true;
    AutotradeLogin::LoginResult(owner, nullptr, AutotradeLogin::Result);
    Check(nestedLine == "native text" && firstLine == "Your character is currently in autotrade.",
        "nested native errors restore the outer popup scope");
    shouldThrow = true;
    try { AutotradeLogin::LoginResult(owner, nullptr, AutotradeLogin::Result); }
    catch (const std::runtime_error&) {}
    Check(!std::strcmp(AutotradeLogin::LookupString(owner, nullptr, 0x3a), "native text"),
        "exception unwinding restores native text");
    TestFingerprint();
    std::printf("Autotrade login tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
