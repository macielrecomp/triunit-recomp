#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <string>
namespace mk5audio {
// Presentation only. Never sends commands to or modifies the original board.
// Music comes from the user's custom_audio/mk1 slots; a missing or unreadable slot keeps the board's music.
class RemasteredMusic {
 struct Slot {std::vector<int16_t> pcm;uint64_t begin=0,end=0,repeat_begin=0,repeat_end=0;bool loop=true;float gain=.125f;};
 std::vector<Slot> tracks;
 std::array<uint64_t,32> commands{};
 int active=-1;uint64_t position=0,limit=0,starts=0,rendered=0,clipped=0,loops=0,finishes=0;unsigned loaded=0;
 std::string fingerprint;   // the loaded slots (presence, window, loop, gain, PCM): binds a save state to them
 // Save states: the tracks are reloaded from the user's files; `active` indexes them.
 template<class A> friend void Fields(A& a,RemasteredMusic& m){
  a(m.commands,m.active,m.position,m.limit,m.starts,m.rendered,m.clipped,m.loops,m.finishes);
  if(m.active<-1 || m.active>=int(m.tracks.size()) || (m.active>=0 && m.limit>m.tracks[size_t(m.active)].pcm.size()/2))
   throw std::runtime_error("MK1 save state music names an absent track position");
 }
public:
 explicit RemasteredMusic(const std::filesystem::path& directory);
 void Command(uint16_t value);
 // True while a user track replaces the board's music: the caller then passes the board without its music.
 bool Replaces()const{return active>=0;}
 // The board's stereo master, plus the user's track when one replaces the music.
 std::array<int16_t,2> Render(std::array<float,2> board);
 unsigned ActiveCommand()const;
 std::string CommandsJson()const;
 unsigned LoadedSlots()const{return loaded;}
 uint64_t Loops()const{return loops;}uint64_t Finishes()const{return finishes;}
 uint64_t Starts()const{return starts;}uint64_t Rendered()const{return rendered;}
 uint64_t Clipped()const{return clipped;}
 const std::string& Fingerprint()const{return fingerprint;}
};
}
