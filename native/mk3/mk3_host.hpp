// MK3 native host helpers kept out of the emulation headers (windows.h stays in mk3_host.cpp).
// Inputs: the Wolf-Unit ports of INPUT_PORTS(mk3) (docs/investigation/sources/midwunit.cpp:136-232), active low.
//   Keyboard: the shared MK1/MK2 map (Mk5KeyboardInputs: arrows/A S D/Z X for P1, I J K L/F G H/V B for P2, 5 6 coins,
//   1 2 starts) plus the MK3-only switches: Run C (P1) / N (P2), and the pinned MAME 0.280 defaults of the cabinet
//   switches the shared map lacks: T Tilt, F2 Service Mode, 9 Service 1, 7 / 8 Coin 3 / 4, - / = Volume, Backspace
//   Banknote 1. Door Interlock has no MAME default and stays released.
//   XInput pads (players 1 and 2, the shared slot assignment of mk5_gamepad.hpp): the shared MK2 layout (D-pad/stick,
//   X HP, Y HK, A LP, B LK, Back coin, Start start) except the shoulders: RB/RT Block and LB/LT Run, as the MK3
//   temporary core's controls (tools/mk3_arcade/build.py: RB blocks, LB runs).
//   These are the defaults of mk5controls::Config; the launcher's CONTROLS tab remaps the player actions (MK1_CONTROLS),
//   adds Raw Input (HID) controllers and SOCD cleaning. Map keeps the defaults; Input::Poll uses the player's controls.
#pragma once
#include <filesystem>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mk3host {
// D5/F4: master gain of both editions (D7: same audio), in the host only, after the DCS output and before the int16
// conversion and the user volume. +16.6 dB lifts the loudest of the 108 verified routes (r2e11, -18.60 dBTP at unity)
// to -2.0 dBTP, 1 dB under D3 (build/native/mk3-product/audio-measure.json).
constexpr double kMasterDb = 16.6;
// Remastered since 26/11/2026: the same DCS output through mk5audio::StudioMaster, whose equalizer, stereo side and
// room change the level; this gain is set so that its loudness matches the Original's (see docs/DISPLAY_INPUT_SETUP.md).
constexpr double kRemasteredMasterDb = 15.6;   // -0.97 dB against the Original on 30 s of the fight route
struct Ports { uint16_t in0 = 0xffff, in1 = 0xffff, in2 = 0xffff; };
struct Pad { uint16_t buttons = 0; uint8_t left_trigger = 0, right_trigger = 0; int16_t lx = 0, ly = 0; };   // XINPUT_GAMEPAD
Ports Map(const std::array<bool, 256>& keys, const Pad* p1, const Pad* p2);   // keys: virtual-key state; pad nullptr: absent
std::string InputMapJson();   // each key and each pad control alone -> ports (mk3 --input-map)

class Input {   // live keyboard and pads, read only while the game window is in the foreground
public:
    Input();
    ~Input();
    Ports Poll();
    unsigned pads() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Persistent NVRAM (MAME nvram image of the Wolf-Unit CMOS, 24576 LE words = 49152 bytes) in DIRECTORY/cmos.bin, held
// under DIRECTORY/.cmos.lock (exclusive, removed on close) for the session. Load returns the profile image, or `seed`
// when there is none yet; Save replaces cmos.bin atomically. Linked (reparse-point) paths are rejected.
class CmosProfile {
public:
    explicit CmosProfile(const std::wstring& directory);
    ~CmosProfile();
    std::vector<uint8_t> Load(const std::vector<uint8_t>& seed, bool& from_profile) const;
    void Save(const std::vector<uint16_t>& words) const;
private:
    std::wstring dir_;
    void* lock_;
};

// The MAME nvram image (24576 LE words = 49152 bytes) of the CMOS words.
std::vector<uint8_t> CmosImage(const std::vector<uint16_t>& words);
// Writes `bytes` beside `file` (write-through), then replaces `file` atomically; false (and nothing replaced) on failure.
bool ReplaceFileBytes(const std::filesystem::path& file, const std::vector<uint8_t>& bytes);
void SetTitle(const std::wstring& title);   // the Mk5Window of this process (class Mk5TemporalScanout)
std::wstring Widen(const std::string& text);
std::string Narrow(const std::wstring& text);
}
