// User music slots (custom_audio/<game>/): empty, partial and corrupted folders, slots.json cuts and fallback.
#include "../audio/custom_audio.hpp"
#include "mk5_remastered_music.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
namespace fs=std::filesystem;
namespace {
void need(bool v,const char* m){if(!v)throw std::runtime_error(m);}
void put(std::string& b,uint32_t v,unsigned n){for(unsigned i=0;i<n;++i)b+=char((v>>(8*i))&255);}
std::string wav(unsigned channels,uint32_t rate,unsigned bits,uint32_t frames,unsigned tag=1,bool extra=false){
    std::string fmt;put(fmt,tag,2);put(fmt,channels,2);put(fmt,rate,4);put(fmt,rate*channels*bits/8,4);put(fmt,channels*bits/8,2);put(fmt,bits,2);
    if(tag==0xfffe){put(fmt,22,2);put(fmt,bits,2);put(fmt,channels==2?3:4,4);put(fmt,1,2);fmt.append(14,'\x01');}
    std::string data;for(uint32_t n=0;n<frames*channels;++n)put(data,uint16_t(int16_t(n*7-300)),2);
    std::string body="WAVE";
    if(extra){body+="LIST";put(body,3,4);body+="abc";body+='\0';}
    body+="fmt ";put(body,uint32_t(fmt.size()),4);body+=fmt;body+="data";put(body,uint32_t(data.size()),4);body+=data;
    std::string out="RIFF";put(out,uint32_t(body.size()),4);return out+body;
}
void write(const fs::path& p,const std::string& b){std::ofstream f(p,std::ios::binary);f.write(b.data(),std::streamsize(b.size()));need(bool(f),"fixture write");}
const std::vector<std::string> names{"music-01","music-02","music-03","music-04"};
}
int main(int argc,char** argv){try{
    need(argc==2,"scratch directory argument");
    const fs::path root=fs::absolute(argv[1]);fs::create_directories(root);
    customaudio::Report r;
    // No folder at all and an empty folder: every slot falls back, nothing throws.
    auto t=customaudio::LoadSlots(root/"absent",names,{true,true,true,true},&r);
    need(t.size()==4&&!t[0]&&!t[3]&&r.loaded==0&&r.missing==4,"absent folder must fall back per slot");
    t=customaudio::LoadSlots({},names,{true,true,true,true},&r);need(!t[0]&&r.loaded==0,"no folder must fall back");
    const auto empty=root/"empty";fs::create_directories(empty);
    t=customaudio::LoadSlots(empty,names,{true,true,true,true},&r);need(r.missing==4&&r.rejected==0,"empty folder must fall back");
    // Partial folder: stereo, mono and extensible PCM load; the others fall back.
    const auto partial=root/"partial";fs::create_directories(partial);
    write(partial/"music-01.wav",wav(2,48000,16,4800));
    write(partial/"music-03.wav",wav(1,48000,16,480,1,true));
    write(partial/"music-04.wav",wav(2,48000,16,96,0xfffe));
    t=customaudio::LoadSlots(partial,names,{true,false,true,true},&r);
    need(r.loaded==3&&r.missing==1&&t[0]&&!t[1]&&t[2]&&t[3],"partial folder slots differ");
    need(t[0]->Frames()==4800&&t[0]->begin==0&&t[0]->end==4800&&t[0]->loop&&!t[0]->repeat_end&&t[0]->gain==1.f,"default cue differs");
    need(t[2]->Frames()==480&&t[2]->pcm[0]==t[2]->pcm[1]&&t[2]->pcm[2]==int16_t(-293),"mono must be duplicated to both channels");
    need(t[3]->Frames()==96,"extensible PCM rejected");
    // Corrupted or unsupported files: each falls back alone.
    const auto bad=root/"corrupt";fs::create_directories(bad);
    auto truncated=wav(2,48000,16,4800);truncated.resize(truncated.size()-10);write(bad/"music-01.wav",truncated);
    write(bad/"music-02.wav",wav(2,44100,16,100));
    write(bad/"music-03.wav",wav(2,48000,24,100));
    write(bad/"music-04.wav",std::string("ID3\x03\x00 not a wave file",22));
    t=customaudio::LoadSlots(bad,names,{true,true,true,true},&r);need(r.loaded==0&&r.rejected==4,"corrupted files must fall back");
    fs::create_directories(bad/"dir.wav");need(!customaudio::ReadWav(bad/"dir.wav"),"directory read as WAV");
    // slots.json: cuts, loop window, gain, file reuse; an invalid entry rejects only its slot.
    write(partial/"slots.json",R"({"slots":{
      "music-01":{"begin":0.01,"end":0.09,"loop":false,"gain_db":-6.0206},
      "music-02":{"file":"music-01.wav","loop_begin":0.02,"loop_end":0.08},
      "music-03":{"end":99},
      "music-04":{"file":"../music-01.wav"}}})");
    t=customaudio::LoadSlots(partial,names,{true,true,true,true},&r);
    need(r.slots_json&&r.loaded==2&&r.rejected==2,"slots.json entries differ");
    need(t[0]->begin==480&&t[0]->end==4320&&!t[0]->loop&&std::abs(t[0]->gain-.5f)<1e-4f,"slot cut or gain differs");
    need(t[1]&&t[1]->Frames()==4800&&t[1]->repeat_begin==960&&t[1]->repeat_end==3840&&t[1]->end==4800,"loop window or reuse differs");
    need(!t[2]&&!t[3],"out-of-range cut or path traversal accepted");
    write(partial/"slots.json","{\"slots\":{\"music-01\":{\"begin\":0.01,}}");
    t=customaudio::LoadSlots(partial,names,{true,true,true,true},&r);
    need(r.slots_json_rejected&&r.loaded==3&&t[0]->begin==0,"invalid slots.json must be ignored as a whole");
    // MK1 Remastered: a loaded slot replaces its selector's music, a missing or corrupt one keeps the board's output.
    {
        mk5audio::RemasteredMusic none(root/"absent");
        none.Command(0x100|19);
        const auto out=none.Render({1234.f,-77.f});
        need(none.LoadedSlots()==0&&!none.Replaces()&&out[0]==1234&&out[1]==-77&&none.Starts()==0&&none.ActiveCommand()==0,"MK1 empty folder must keep the board's master");
        const auto mk1=root/"mk1";fs::create_directories(mk1);
        write(mk1/"music-01.wav",wav(2,48000,16,3));   // selector 19, plays once
        write(mk1/"music-02.wav",truncated);            // selector 20, corrupt
        write(mk1/"music-03.wav",wav(1,48000,16,2));    // selector 1, loops
        mk5audio::RemasteredMusic m(mk1);
        need(m.LoadedSlots()==2,"MK1 partial folder slots differ");
        m.Command(0x100|20);need(!m.Replaces()&&m.Render({777.f,777.f})==std::array<int16_t,2>{777,777}&&m.ActiveCommand()==0,"MK1 corrupt slot must keep the original music");
        m.Command(0x100|19);
        need(m.Replaces(),"MK1 loaded slot must replace the board's music");
        const auto first=m.Render({100.f,40.f});
        need(m.ActiveCommand()==19&&first[0]==int16_t(int32_t(100.f-300*.125f))&&first[1]==int16_t(int32_t(40.f-293*.125f)),"MK1 slot music or effects mix differs");
        m.Render({0.f,0.f});m.Render({0.f,0.f});need(m.Finishes()==1&&m.Render({5.f,5.f})==std::array<int16_t,2>{5,5},"MK1 one-shot slot must end in silence over effects");
        m.Command(0x100|1);for(int n=0;n<5;++n)m.Render({0.f,0.f});need(m.Loops()==2,"MK1 looping slot must repeat");
        m.Command(0x100|0x21);need(m.ActiveCommand()==1,"MK1 effect command interrupted the music");
        m.Command(0x100|0x05);need(m.ActiveCommand()==0,"MK1 unmapped music selector must return to the board's music");
    }
    std::cout<<"PASS custom audio slots: absent, empty, partial, corrupted, slots.json cuts/loops/gain, MK1 fallback per selector\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
