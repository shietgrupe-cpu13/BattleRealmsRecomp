__declspec(dllexport) __declspec(noinline) int __cdecl fixture_leaf(int value)
{
    return ((value + 0x17) ^ 0x35) - 9;
}

__declspec(noinline) int __cdecl fixture_entry(void)
{
    volatile int seed = 0x1234;
    return fixture_leaf(seed);
}
