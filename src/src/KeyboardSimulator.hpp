#include <memory>

struct KeyboardSimulator
{
    enum KeyboardSimulatorLayout
    {
        LAYOUT_MANIA
    };

    struct Impl;
    std::unique_ptr<Impl> m_impl;

    KeyboardSimulator(KeyboardSimulatorLayout layout = LAYOUT_MANIA);
    ~KeyboardSimulator();

    void send(uint64_t keys);
    void delay(int millis);
};
