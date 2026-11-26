#pragma once
// MK1 save states between two ContinueMain calls of the live loop, after the diagnostic trim (no unpublished PCM):
// the TMS34010 and every memory, the board, DI and IRQ state, the DMA video and its Remastered layers, the scanout
// (the partial frame included), the Williams sound board (6809, YM2151, OKI, DAC, schedulers), the host resamplers,
// music mix and gain. ROMs, the user's music tracks, observers and the window stay with the live session.
#include "mk5_task_body.hpp"
#include "mk5_scanout.hpp"
#include "mk5_live_video.hpp"
#include "mk5_audio.hpp"
#include "../audio/host_gain.hpp"
#include "../mk2/mk2_state_io.hpp"

inline constexpr uint32_t kMk5SaveStateVersion=18;   // 18: studio master DC blocker; 17: Remastered studio master; 16: Remastered sinc reconstruction streams; 2: the floor blood's fade references (mk5_blood_shade.hpp); 3: widescreen; 4: 16:9 layer; 5: mirrored planes; 6: backdrop margins; 7: wrapping planes; 8: booked palettes; 9: far-plane lag; 10: centred window, reflected world ends; 11: margins from draws only, masks; 12: limited wide camera; 13: soft menu sides; 14: attract demos; 15: sky, measured plane bounds
inline constexpr const char* kMk5SaveStateRevision="mk1 5.0";
inline constexpr const char* kMk5SaveStateMagic="TRIUNIT-MK1-STATE";

template<class A> void Fields(A& a,Write16& w){a(w.source_pc,w.address_bit,w.value);}
template<class A> void Fields(A& a,ResetTrace& s){a(s.a,s.b,s.status,s.next_original_pc,s.reference_cycles,s.writes);}
template<class A> void Fields(A& a,Mk5MemoryBuffers& m){a(m.ram,m.vram,m.palette);}
template<class A> void Fields(A& a,Mk5CpuHistory& h){a(h.cpu,h.stack_read_count,h.lowest_sp,h.returns);}
template<class A> void Fields(A& a,CmosBusWrite8& w){a(w.source_pc,w.address_bit,w.value,w.applied_to_cmos,w.before_word,w.after_word,w.scalar_writes_before);}
template<class A> void Fields(A& a,Mk5BoardState& f){a(f.check,f.cmos,f.byte_writes,f.memory_write_count,f.unique_words_written);}
template<class A> void Fields(A& a,Mk5IrqEntry& e){a(e.source_pc,e.return_pc,e.saved_status,e.vector_bit,e.target_pc,e.source_mask,e.external_line,e.stacked,e.at_quanta);}
template<class A> void Fields(A& a,Mk5ScanlineEvent& e){a(e.at_quanta,e.vcount,e.dpyadr,e.display_pending);}
template<class A> void Fields(A& a,Mk5DmaCommand& c){a(c.pc,c.start_ns,c.due_ns,c.registers);}

struct Mk5LiveSession {Mk5TaskBodyRunner& runner;Mk5Scanout& screen;mk5audio::WilliamsAudio& audio;mk5audio::HostGain& gain;};

namespace mk5state {
// The edition is the Remastered presentation (scanout layers and music) or neither; music binds the user's tracks.
struct Identity {std::string edition,music;};
inline Identity LiveIdentity(const Mk5LiveSession& s){
    const auto music=s.audio.MusicFingerprint();
    if(s.screen.Remastered()!=!music.empty())throw std::logic_error("MK1 save state requires one whole edition");
    return {music.empty()?"original":"remastered",music};
}
struct Header {std::string magic,program,revision,edition,music,payload;uint32_t version=0;};
template<class A> void Fields(A& a,Header& h){a(h.magic,h.version,h.program,h.revision,h.edition,h.music,h.payload);}
// Identity and integrity before anything is applied; a refused file leaves the session untouched.
inline mk2::state::Reader CheckLiveState(const std::vector<uint8_t>& bytes,const Identity& identity){
    mk2::state::Reader reader{bytes.data(),bytes.data()+bytes.size()};Header header;
    reader(header);
    if(header.magic!=kMk5SaveStateMagic || header.version!=kMk5SaveStateVersion || header.revision!=kMk5SaveStateRevision)
        throw std::invalid_argument("MK1 save state format or revision differs");
    if(header.program!=kMk5ProgramSha256)throw std::invalid_argument("MK1 save state belongs to another program ROM");
    if(header.edition!=identity.edition)throw std::invalid_argument("MK1 save state belongs to another edition");
    if(header.music!=identity.music)throw std::invalid_argument("MK1 save state was made with other music tracks");
    if(header.payload!=Sha256(std::vector<uint8_t>(reader.at,reader.end)))throw std::invalid_argument("MK1 save state payload is damaged");
    return reader;
}
inline void RequireBoundary(const Mk5LiveSession& s){
    if(!s.audio.Pcm().empty())throw std::logic_error("MK1 save state requires the PCM published and trimmed");
}
}

inline std::vector<uint8_t> Mk5SaveLiveState(const Mk5LiveSession& s){
    mk5state::RequireBoundary(s);
    const auto identity=mk5state::LiveIdentity(s);
    mk2::state::Writer payload;payload(s.runner,s.screen,s.audio,s.gain);
    mk5state::Header header{kMk5SaveStateMagic,kMk5ProgramSha256,kMk5SaveStateRevision,identity.edition,identity.music,
                            Sha256(payload.bytes),kMk5SaveStateVersion};
    mk2::state::Writer out;out(header);
    out.bytes.insert(out.bytes.end(),payload.bytes.begin(),payload.bytes.end());
    return std::move(out.bytes);
}
// Call CheckLiveState first to refuse a file without side effects; a failure here leaves partial state and is fatal.
inline void Mk5LoadLiveState(const std::vector<uint8_t>& bytes,const Mk5LiveSession& s){
    auto reader=mk5state::CheckLiveState(bytes,mk5state::LiveIdentity(s));
    mk5state::RequireBoundary(s);
    reader(s.runner,s.screen,s.audio,s.gain);
    if(reader.at!=reader.end)throw std::runtime_error("MK1 save state has trailing data");
    mk5state::RequireBoundary(s);
}
