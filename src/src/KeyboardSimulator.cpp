#include "KeyboardSimulator.hpp"

#include "spdlog/spdlog.h"

#include <Windows.h>

// https://docs.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes

static constexpr int N_BUTTONS = 4;

WORD MANIA_BTN_MAP[N_BUTTONS] = {
    'A', 'D', 'J', 'L'};

struct KeyboardSimulator::Impl
{
    WORD *m_layout;
    INPUT m_input_buffer[N_BUTTONS];

    uint64_t m_last_keys;
    int m_buffered_keys;

    Impl(KeyboardSimulator::KeyboardSimulatorLayout layout);

    void start();
    void key_down(int i);
    void key_up(int i);
    void end();

    void send(uint64_t keys);
};

KeyboardSimulator::KeyboardSimulator(KeyboardSimulatorLayout layout)
    : m_impl(std::make_unique<Impl>(layout)) {}

KeyboardSimulator::~KeyboardSimulator() = default;

void KeyboardSimulator::send(uint64_t keys)
{
    m_impl->send(keys);
}

void KeyboardSimulator::delay(int millis)
{
    Sleep(millis);
}

KeyboardSimulator::Impl::Impl(KeyboardSimulator::KeyboardSimulatorLayout layout)
    : m_layout(nullptr),
      m_last_keys(0),
      m_buffered_keys(0)
{
    switch (layout)
    {
    case LAYOUT_MANIA:
        m_layout = MANIA_BTN_MAP;
        break;
    default:
        m_layout = MANIA_BTN_MAP;
        break;
    }

    // Zero out buffer
    for (int i = 0; i < N_BUTTONS; i++)
    {
        m_input_buffer[i].type = INPUT_KEYBOARD;
        m_input_buffer[i].ki.wVk = 0;
        m_input_buffer[i].ki.wScan = 0;
        m_input_buffer[i].ki.dwFlags = 0;
        m_input_buffer[i].ki.time = 0;
        m_input_buffer[i].ki.dwExtraInfo = 0;
    }

    // Send one blank keydown
    start();
    end();
};

void KeyboardSimulator::Impl::send(uint64_t keys)
{
    uint64_t keys_changed = keys ^ m_last_keys;
    uint64_t keys_to_down = keys_changed & keys;
    uint64_t keys_to_up = keys_changed & m_last_keys;

    m_last_keys = keys;

    start();
    for (int i = 0; i < N_BUTTONS; i++)
    {
        if (keys_to_down & 1)
        {
            key_down(i);
        }
        else if (keys_to_up & 1)
        {
            key_up(i);
        }

        keys_to_down >>= 1;
        keys_to_up >>= 1;
    }
    end();
}

void KeyboardSimulator::Impl::start()
{
    m_buffered_keys = 0;
}

void KeyboardSimulator::Impl::key_down(int i)
{
    spdlog::debug("{} Down", m_layout[i]);
    m_input_buffer[m_buffered_keys].ki.wVk = m_layout[i];
    m_input_buffer[m_buffered_keys].ki.dwFlags = 0;
    m_buffered_keys++;
}

void KeyboardSimulator::Impl::key_up(int i)
{
    spdlog::debug("{} Up", m_layout[i]);
    m_input_buffer[m_buffered_keys].ki.wVk = m_layout[i];
    m_input_buffer[m_buffered_keys].ki.dwFlags = KEYEVENTF_KEYUP;
    m_buffered_keys++;
}

using SendInputHandleType = UINT(WINAPI *)(UINT, LPINPUT, int);
static SendInputHandleType SendInputHandle = reinterpret_cast<SendInputHandleType>(GetProcAddress(GetModuleHandleW((L"user32")), "SendInput"));

void KeyboardSimulator::Impl::end()
{
    if (m_buffered_keys)
    {
        SendInputHandle(m_buffered_keys, m_input_buffer, sizeof(INPUT));
    }
}
