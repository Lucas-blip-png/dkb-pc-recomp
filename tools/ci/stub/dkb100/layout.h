// Stub layout for CI builds without a game dump. Neutral placeholders; not shipped.
#pragma once

#define MOD_MAIN_BASE 0x10000000ull
#define MOD_MAIN_SIZE 0x100000ull
#define MOD_SDK_BASE 0x10100000ull
#define MOD_SDK_SIZE 0x100000ull

#define MOD_COUNT 2
#define MOD_LIST(X) X(main, MAIN) X(sdk, SDK)