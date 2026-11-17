#pragma once
#include <windows.h>

namespace PriceInput {
bool MatchesReference(const unsigned char* base) noexcept;
LONG Attach(); // Uses the caller's startup Detours transaction.
#ifdef PRICE_INPUT_TESTING
using ConvertFn=int(__thiscall*)(void*,char*,int);
void TestConverter(ConvertFn converter);
int TestRead(void* edit,char* output);
LONG TestAttach(const unsigned char* base);
#endif
}
