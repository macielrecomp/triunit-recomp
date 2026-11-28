#pragma once
// MK2 save states at a live chunk boundary: CPU34, board, memories, DCS/ADSP, host audio/raster continuity and, in
// the Remastered edition, its video layers and music mix, as value fields of the hardware model. References, ROMs,
// the user's music tracks, observers and providers stay bound to the live session, so a load restores state inside
// the same machine and never rebinds a callback.
#include "mk2_live_host.hpp"
#include "mk2_scanout.hpp"
#include "mk2_music.hpp"
#include "mk2_state_io.hpp"
#include "mk5_program.hpp"

namespace mk2 {
inline constexpr uint32_t kSaveStateVersion=24;   // 23: widescreen far planes at an even pace, far objects flying out of the margins; 22: studio master DC blocker; 21: Remastered studio master; 20: Remastered sinc reconstruction streams; 3: the floor blood's fade references (mk5_blood_shade.hpp); 4: widescreen; 5: 16:9 layer; 6: mirrored planes; 7: backdrop margins; 8: filled edge columns; 9: booked palettes; 10: margin columns of new objects; 11: far-plane lag; 12: parked objects; 13: centred window, reflected world ends; 14: margin flights; 15: margins from draws only, masks; 16: limited wide camera (layer fields); 17: soft menu sides; 18: attract demos; 19: sky, measured plane bounds
inline constexpr const char* kSaveStateRevision="mk2 L3.1";
inline constexpr const char* kSaveStateMagic="TRIUNIT-MK2-STATE";

template<class A> void Fields(A& a,ResetAccess& x){a(x.pc,x.address_bit,x.value,x.cycle);}
template<class A> void Fields(A& a,PixbltAccess& x){a(x.pc,x.address_bit,x.value,x.cycle);}
template<class A> void Fields(A& a,ResetTrace& s){
    a(s.a,s.b,s.status,s.next_pc,s.cycles,s.instructions,s.delay_iterations,s.writes,s.cmos_reads,s.cmos_writes,
      s.board,s.board_writes,s.cmos_write_enabled,s.cmos,s.ram);
}
template<class A> void Fields(A& a,BootCheckpoint& p){a(p.name,p.cpu,p.dsp);}
template<class A> void Fields(A& a,BootExecution& e){a(e.fill_remaining,e.fill_b,e.last_instruction_pc,e.last_instruction_cycles,e.shift_loaded,e.bitmap);}
template<class A> void Fields(A& a,TaskBatchInit& t){
    a(t.ready,t.video,t.main_ram,t.bus,t.points,t.observe_history,t.execution,t.host_origin,t.cpu_trace,t.executed_instructions);
}

// The session's host audio: none, the Original DCS stream or the Remastered music mix.
struct LiveAudio { OriginalAudio* original=nullptr; RemasteredAudio* music=nullptr; };

namespace state {
// What a state must match besides the program ROM: the edition's layers, the audio stream and the user's tracks.
struct Identity { std::string edition,music; uint8_t audio=0; };
inline Identity LiveIdentity(const LiveHostContinuation& continuation,const Scanout& scanout,LiveAudio audio){
    const auto& video=continuation.State().video;
    const bool layers[]={scanout.remastered(),video.BackdropEnabled(),video.FightersEnabled(),video.BloodEnabled()};
    const auto count=std::count(std::begin(layers),std::end(layers),true);
    if(count!=0 && count!=4)throw std::logic_error("MK2 save state requires one whole edition");
    if(audio.original && audio.music)throw std::logic_error("MK2 save state requires one audio stream");
    return {count?"remastered":"original",audio.music?audio.music->MusicFingerprint():std::string{},
            uint8_t(audio.music?2:audio.original?1:0)};
}
struct Header { std::string magic,program,revision,payload; uint32_t version=0; Identity identity; };
template<class A> void Fields(A& a,Header& h){
    a(h.magic,h.version,h.program,h.revision,h.identity.edition,h.identity.audio,h.identity.music,h.payload);
}
// Identity and integrity before anything is applied; a refused file leaves the session untouched.
inline Reader CheckLiveState(const Program& program,const std::vector<uint8_t>& bytes,const Identity& identity){
    Reader reader{bytes.data(),bytes.data()+bytes.size()};Header header;
    reader(header);
    if(header.magic!=kSaveStateMagic || header.version!=kSaveStateVersion || header.revision!=kSaveStateRevision)
        throw std::invalid_argument("MK2 save state format or revision differs");
    if(header.program!=program.sha256)throw std::invalid_argument("MK2 save state belongs to another program ROM");
    if(header.identity.edition!=identity.edition)throw std::invalid_argument("MK2 save state belongs to another edition");
    if(header.identity.audio!=identity.audio)throw std::invalid_argument("MK2 save state audio stream differs from this session");
    if(header.identity.music!=identity.music)throw std::invalid_argument("MK2 save state was made with other music tracks");
    if(header.payload!=Sha256(std::vector<uint8_t>(reader.at,reader.end)))throw std::invalid_argument("MK2 save state payload is damaged");
    return reader;
}
template<class A> void Session(A& a,LiveHostContinuation& continuation,DcsBootMachine& machine,Scanout& scanout,LiveAudio audio){
    a(continuation,machine,scanout);
    if(audio.original)a(*audio.original);
    if(audio.music)a(*audio.music);
}
}

// Only between ResumeValidLiveHost chunks, inside the live domain.
inline std::vector<uint8_t> SaveLiveState(const Program& program,const LiveHostContinuation& continuation,
    const DcsBootMachine& machine,const Scanout& scanout,LiveAudio audio) {
    continuation.ValidateBoundary(program,machine);
    const auto identity=state::LiveIdentity(continuation,scanout,audio);
    state::Writer payload; // The writer only reads its operands.
    state::Session(payload,const_cast<LiveHostContinuation&>(continuation),const_cast<DcsBootMachine&>(machine),
                   const_cast<Scanout&>(scanout),audio);
    state::Header header{kSaveStateMagic,program.sha256,kSaveStateRevision,Sha256(payload.bytes),kSaveStateVersion,identity};
    state::Writer out;out(header);
    out.bytes.insert(out.bytes.end(),payload.bytes.begin(),payload.bytes.end());
    return std::move(out.bytes);
}
// Call CheckLiveState first to refuse a file without side effects; a failure here leaves partial state and is fatal.
inline void LoadLiveState(const Program& program,const std::vector<uint8_t>& bytes,LiveHostContinuation& continuation,
    DcsBootMachine& machine,Scanout& scanout,LiveAudio audio) {
    auto reader=state::CheckLiveState(program,bytes,state::LiveIdentity(continuation,scanout,audio));
    continuation.ValidateBoundary(program,machine);
    state::Session(reader,continuation,machine,scanout,audio);
    if(reader.at!=reader.end)throw std::runtime_error("MK2 save state has trailing data");
    continuation.ValidateBoundary(program,machine);
}
}
