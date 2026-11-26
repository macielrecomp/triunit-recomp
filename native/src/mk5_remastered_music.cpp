#include "mk5_remastered_music.hpp"
#include "../runtime/remastered_music_manifest.hpp"
#include "../audio/custom_audio.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
namespace mk5audio {
RemasteredMusic::RemasteredMusic(const std::filesystem::path& directory){
 std::vector<std::string> names;std::vector<bool> loops;
 for(const auto& slot:remastered_slots){names.emplace_back(slot.slot);loops.push_back(slot.loop);}
 // A slot without a usable user file keeps the board's own music for that selector.
 for(auto& track:customaudio::LoadSlots(directory,names,loops)){
  auto& t=tracks.emplace_back();
  if(track){t.pcm=std::move(track->pcm);t.begin=track->begin;t.end=track->end;t.loop=track->loop;
   t.repeat_begin=track->repeat_end?track->repeat_begin:track->begin;t.repeat_end=track->repeat_end?track->repeat_end:track->end;t.gain=.125f*track->gain;++loaded;}
 }
 // 64-bit FNV-1a over every slot's presence, window, loop, gain and PCM (four samples per word), once at the load.
 uint64_t hash=0xcbf29ce484222325ull;
 const auto mix=[&hash](uint64_t value){hash=(hash^value)*0x100000001b3ull;};
 for(const auto& t:tracks){
  uint32_t gain;std::memcpy(&gain,&t.gain,sizeof gain);
  for(const uint64_t value:{uint64_t(t.pcm.size()),t.begin,t.end,t.repeat_begin,t.repeat_end,uint64_t(t.loop),uint64_t(gain)})mix(value);
  size_t n=0;
  for(uint64_t word;n+4<=t.pcm.size();n+=4){std::memcpy(&word,t.pcm.data()+n,sizeof word);mix(word);}
  for(;n<t.pcm.size();++n)mix(uint16_t(t.pcm[n]));
 }
 char text[17];std::snprintf(text,sizeof text,"%016llx",static_cast<unsigned long long>(hash));fingerprint=text;
}
void RemasteredMusic::Command(uint16_t value){
 if(!(value&0x100)){active=-1;position=0;return;}
 const unsigned id=value&255;
 for(size_t i=0;i<remastered_slots.size();++i)if(id==remastered_slots[i].command){
  ++commands[id];
  if(tracks[i].pcm.empty()){active=-1;position=0;return;}
  active=int(i);position=tracks[i].begin;limit=tracks[i].end;++starts;return;
 }
 // 00..1f are the original music selectors. Other commands are effects,
 // voices or sound-board controls and must not interrupt the music.
 if(id<32){active=-1;position=0;}
}
unsigned RemasteredMusic::ActiveCommand()const{return active<0?0:remastered_slots[size_t(active)].command;}
std::string RemasteredMusic::CommandsJson()const{std::ostringstream s;s<<'{';bool comma=false;for(size_t i=0;i<commands.size();++i)if(commands[i]){if(comma)s<<',';s<<'"'<<i<<"\":"<<commands[i];comma=true;}s<<'}';return s.str();}
std::array<int16_t,2> RemasteredMusic::Render(std::array<float,2> board){
 if(active<0)return {int16_t(std::clamp(int32_t(board[0]),-32768,32767)),int16_t(std::clamp(int32_t(board[1]),-32768,32767))};
 const auto& t=tracks[size_t(active)];std::array<int16_t,2> out{};
 const bool playing=position<limit;
 // Fixed presentation gain provides room for original effects; the user's file itself is not normalized.
 for(unsigned c=0;c<2;++c){const auto v=int32_t(board[c]+(playing?float(t.pcm[size_t(position)*2+c])*t.gain:0.f));clipped+=v<-32768||v>32767;out[c]=int16_t(std::clamp(v,-32768,32767));}
 ++rendered;
 if(playing&&++position==limit){if(t.loop){position=t.repeat_begin;limit=t.repeat_end;++loops;}else ++finishes;}
 return out;
}
}
