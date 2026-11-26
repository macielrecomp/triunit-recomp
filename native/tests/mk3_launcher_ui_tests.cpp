#include "../launcher/launcher_main.cpp"
#include <cstdlib>
#include <iostream>
// MK3 card on a staged native package (tools/launcher_mk3_engine_contract.py): two native editions, choice saved in
// build/native/mk3-native/profile/launcher.ini and restored after a restart, Engine=0 and the old temporary-core profile
// migrated, a refused package reported with MK3 unavailable and nothing else started. F3: per-edition [Video.Original] /
// [Video.Remastered] profiles (Original without effect, Remastered BVM / 1080p and ScaleFX 100), [Video.Preview.Remastered]
// migrated on read, the whole native MK2 variable set (video, GameVolume, AudioLatency) delivered, and the Settings video,
// display and volume controls enabled for MK3.
// This binary is also the staged mk3.exe: started with --edition it only prints its edition and MK1_* environment
// (";" ends each value: stdout is a text-mode log).
void need(bool value,const char* why){if(!value)throw std::runtime_error(why);}
std::string text(const fs::path& path){std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
void put(const fs::path& path,const std::string& bytes){std::ofstream file(path,std::ios::binary|std::ios::trunc);file<<bytes;need(bool(file),"fixture write");}
bool has(const std::string& haystack,const std::string& needle){return haystack.find(needle)!=std::string::npos;}
// No preview or temporary-core wording in what the launcher shows for MK3.
bool clean(std::wstring s){
 for(auto& c:s)if(c>=L'a'&&c<=L'z')c=wchar_t(c-L'a'+L'A');
 for(const wchar_t* word:{L"PREVI",L"PRÉVI",L"PRéVI",L"TEMPORARI"})if(s.find(word)!=std::wstring::npos)return false;
 return true;
}
constexpr const char* keys[]={"MK1_CRT_PRESET","MK1_SCANLINES","MK1_FILTER","MK1_SCALEFX","MK1_SCALE_MODE","MK1_CAS","MK1_BILATERAL",
 "MK1_SMAA","MK1_SYNC","MK1_COLOR","MK1_BRIGHTNESS","MK1_BEZEL","MK1_HIGH_REFRESH","MK1_FULLSCREEN","MK1_GAME_VOLUME",
 "MK1_AUDIO_LATENCY","MK1_MUSIC_MODE","MK1_MUSIC_DIRECTORY","MK1_GRAPHICS_VALIDATE","MK1_STRAY"};
int probe(int argc,wchar_t** argv){
 for(int i=1;i+1<argc;++i)if(std::wstring(argv[i])==L"--edition")std::cout<<"edition="<<(std::wstring(argv[i+1])==L"remastered"?"remastered":"original")<<";\n";
 for(const char* key:keys){const char* value=std::getenv(key);std::cout<<key<<'='<<(value?value:"<unset>")<<";\n";}
 return 0;
}
// Every MK1_* value the probe received, as "KEY=value;" rows in `log`.
bool environment(const std::string& log,std::initializer_list<std::pair<const char*,const char*>> want){
 for(const auto& [key,value]:want)if(!has(log,std::string(key)+"="+value+";"))return false;
 return true;
}
const wchar_t* label(int engine){return engine==2?L"NATIVE  /  REMASTERED":L"NATIVE  /  ORIGINAL";}
// A new launcher session with the MK3 card chosen.
struct Session {
 App app;
 explicit Session(const fs::path& root):app(root){app.act(3);need(app.selected==2&&app.chosen==2,"MK3 card chosen");}
 int engine()const{return app.backend.mk3Engine();}
 // The hashed check of the packages, which the app runs right after its first frame (App::poll); at its start a game
 // is offered on its setup record alone.
 void checked(){app.poll();app.availability.wait();app.nextPoll=0;app.poll();}
 std::string launch(const fs::path& root){
  app.act(20);need(app.backend.running(),"MK3 probe launched");
  for(const auto start=GetTickCount64();app.backend.running()&&GetTickCount64()-start<20000;Sleep(10))app.backend.poll();
  need(!app.backend.running()&&app.backend.exitCode()==0,"MK3 probe exit 0");
  return text(root/L"logs/mk3-native/launcher-latest.log");
 }
 const VideoConfig& video(bool remastered)const{return app.backend.config.video(remastered);}
};
int wmain(int argc,wchar_t** argv){
 for(int i=1;i<argc;++i)if(std::wstring(argv[i])==L"--edition")return probe(argc,argv);
 if(argc!=2)return 2;
 try{
  const fs::path root=argv[1];
  const auto product=root/L"build/native/mk3-native/product",ini=root/L"build/native/mk3-native/profile/launcher.ini",
             legacy=root/L"build/native/mk3-arcade-v1/profile/launcher.ini";
  SetEnvironmentVariableW(L"MK1_STRAY",L"inherited");SetEnvironmentVariableW(L"MK1_GAME_VOLUME",L"7");
  {Session s(root);
   need(s.app.mk3Available&&s.engine()==1&&s.app.mk3Edition()==std::wstring(label(1))&&!fs::exists(ini),"fresh profile starts on the native Original");
   for(int want:{2,1,2}){
    s.app.act(3);need(s.engine()==want&&s.app.mk3Edition()==std::wstring(label(want))&&clean(s.app.notice),"card click toggles the two native editions");
    need(has(text(ini),"[MK3]\r\nRemastered=0\r\nEngine="+std::to_string(want)+"\r\n")&&s.app.draft.mk3Engine==want,"choice saved in [MK3] Engine");
   }
   const auto saved=text(ini);
   need(has(saved,"[Video.Original]\r\nscanlines=0\r\nfilter=0\r\nsync=1\r\n")&&has(saved,"[Video.Remastered]\r\nscanlines=4\r\nfilter=0\r\nsync=1\r\n")&&
        has(saved,"scalefx=1\r\nbezel=1\r\nhighrefresh=0\r\nsmaa=0\r\nfullscreen=1\r\n")&&!has(saved,"Video.Preview"),"edition defaults saved in [Video.Original] / [Video.Remastered] only");}
  {App app(root);need(app.backend.mk3Engine()==2&&app.mk3Edition()==std::wstring(label(2)),"saved choice shown on the card at start");}
  {Session s(root);need(s.engine()==2,"saved choice restored after a restart");
   // Factory profiles reach the presenter: Original without effect, Remastered BVM / 1080p and ScaleFX 100; the inherited
   // MK1_* values are dropped and replaced by the launcher's.
   const auto remastered=s.launch(root);
   need(environment(remastered,{{"edition","remastered"},{"MK1_CRT_PRESET","bvm-1080"},{"MK1_SCANLINES","0"},{"MK1_FILTER","nearest"},{"MK1_SCALEFX","100"},
        {"MK1_SCALE_MODE","0"},{"MK1_CAS","0"},{"MK1_BILATERAL","0"},{"MK1_SMAA","0"},{"MK1_SYNC","vrr"},{"MK1_COLOR","original"},{"MK1_BRIGHTNESS","100"},
        {"MK1_BEZEL","1"},{"MK1_HIGH_REFRESH","0"},{"MK1_FULLSCREEN","1"},{"MK1_GAME_VOLUME","100"},{"MK1_AUDIO_LATENCY","balanced"},
        {"MK1_MUSIC_MODE","original"},{"MK1_MUSIC_DIRECTORY","<unset>"},{"MK1_GRAPHICS_VALIDATE","0"},{"MK1_STRAY","<unset>"}}),"Remastered factory profile delivered");
   s.app.act(3);need(s.engine()==1,"toggle to the Original");
   const auto original=s.launch(root);need(clean(s.app.notice),"start and exit notices");
   need(environment(original,{{"edition","original"},{"MK1_CRT_PRESET","off"},{"MK1_SCANLINES","0"},{"MK1_FILTER","nearest"},{"MK1_SCALEFX","0"},
        {"MK1_CAS","0"},{"MK1_BILATERAL","0"},{"MK1_SMAA","0"},{"MK1_COLOR","original"},{"MK1_BRIGHTNESS","100"},{"MK1_SYNC","vrr"}}),"Original factory profile without effect");}
  // Settings: the video, display and volume controls are enabled for MK3 and the profile toggle starts on the chosen edition.
  {Session s(root);need(s.engine()==1,"Original chosen");s.app.openSettings();
   need(s.app.settings&&s.app.editingGame&&s.app.nativeVideo()&&s.app.gameVolumeEnabled()&&!s.app.videoRemastered,"MK3 Settings open on the Original video profile");
   s.app.hitTargets();
   for(int id:{int(videoProfile),int(videoPage),int(videoScale),201,200,207,208}){
    const auto c=std::find_if(s.app.controls.begin(),s.app.controls.end(),[&](const Control& v){return v.id==id;});
    need(c!=s.app.controls.end()&&c->enabled,"MK3 video control enabled");
   }
   s.app.act(201);s.app.act(201);s.app.act(201);need(s.app.video().filter==3,"four sampling filters");
   s.app.act(videoScale);s.app.act(videoScale);need(s.app.video().scaleMode==2,"scale mode");
   for(int n=0;n<9;++n){s.app.act(200);}
   need(s.app.video().scanlines==9,"all CRT presets");
   s.app.act(207);s.app.act(207);s.app.act(208);need(s.app.video().color==2&&s.app.video().brightness==110,"color and brightness");
   s.app.act(videoPage);need(s.app.draft.videoEffects,"effects page");
   s.app.act(255);s.app.act(videoCas);s.app.act(videoSmaa);s.app.act(videoBilateral);s.app.act(videoBilateral);
   need(s.app.video().scalefx&&s.app.video().cas==25&&s.app.video().smaa&&s.app.video().bilateral==50,"ScaleFX, CAS, SMAA and bilateral");
   s.app.selectTab(2);s.app.act(250);s.app.act(250);s.app.act(251);s.app.act(209);s.app.act(videoFullscreen);
   need(s.app.video().sync==0&&s.app.video().highRefresh&&!s.app.video().bezel&&!s.app.video().fullscreen,"V-Sync, high refresh, bezel and fullscreen");
   s.app.selectTab(1);s.app.act(205);need(s.app.draft.audioLatency==2,"audio queue");
   s.app.selectTab(5);s.app.hitTargets();
   const auto volume=std::find_if(s.app.controls.begin(),s.app.controls.end(),[](const Control& v){return v.id==270;});
   need(volume!=s.app.controls.end()&&volume->enabled,"MK3 game volume enabled");
   s.app.focus=0;s.app.key(VK_LEFT);s.app.key(VK_LEFT);need(s.app.draft.gameVolume==90,"game volume slider");
   s.app.act(210);need(s.app.backend.config.video(false).filter==3&&s.app.backend.config.gameVolume==90,"Settings saved");
   const auto saved=text(ini);
   need(has(saved,"GameVolume=90\r\n")&&has(saved,"AudioLatency=2\r\n")&&has(saved,"[Video.Original]\r\nscanlines=9\r\nfilter=3\r\nsync=0\r\ncolor=2\r\nbrightness=110\r\n")&&
        has(saved,"[Video.Remastered]\r\nscanlines=4\r\n")&&!has(saved,"Video.Preview"),"MK3 Settings saved per edition");
   s.app.selectTab(0);s.app.act(videoProfile);need(s.app.videoRemastered,"profile toggle to the Remastered");
   s.app.video().scanlines=2;s.app.video().scalefx=false;s.app.act(211);need(s.app.video().scanlines==4&&s.app.video().scalefx&&s.app.video().scalefxAmount==100,"Remastered profile reset to its own defaults");
   s.app.act(212);
   const auto original=s.launch(root);
   need(environment(original,{{"edition","original"},{"MK1_CRT_PRESET","phosphor"},{"MK1_FILTER","bicubic"},{"MK1_SCALEFX","100"},{"MK1_SCALE_MODE","2"},
        {"MK1_CAS","25"},{"MK1_BILATERAL","50"},{"MK1_SMAA","1"},{"MK1_SYNC","vsync"},{"MK1_COLOR","warm"},{"MK1_BRIGHTNESS","110"},{"MK1_BEZEL","0"},
        {"MK1_HIGH_REFRESH","1"},{"MK1_FULLSCREEN","0"},{"MK1_GAME_VOLUME","90"},{"MK1_AUDIO_LATENCY","safe"},{"MK1_MUSIC_MODE","original"}}),"Settings reach the MK3 Original");}
  // Profiles written by hand: each edition reads its own section; immediate sync and the Remastered edition's options.
  put(ini,"[MK3]\r\nEngine=2\r\nGameVolume=35\r\nAudioLatency=1\r\n[Video.Original]\r\nscanlines=4\r\n[Video.Remastered]\r\nscanlines=5\r\nscalefx=1\r\nscalefxamount=60\r\n"
          "sync=2\r\nfilter=1\r\nhighrefresh=1\r\n");
  {Session s(root);need(s.engine()==2&&s.video(false).scanlines==4&&s.video(true).scanlines==5,"ini sections per edition");
   const auto remastered=s.launch(root);
   need(environment(remastered,{{"edition","remastered"},{"MK1_CRT_PRESET","bvm-4k"},{"MK1_SCALEFX","60"},{"MK1_SYNC","immediate"},{"MK1_FILTER","linear"},
        {"MK1_HIGH_REFRESH","1"},{"MK1_GAME_VOLUME","35"},{"MK1_AUDIO_LATENCY","balanced"}}),"Remastered profile read from the ini");}
  put(ini,"[MK3]\r\nEngine=2\r\n[Video.Remastered]\r\nscanlines=99\r\nscalefx=2\r\nscalefxamount=abc\r\nbrightness=95\r\n[Video.Original]\r\nfilter=9\r\n");
  {Session s(root);const auto& p=s.video(true);
   need(s.engine()==2&&p.scanlines==4&&p.scalefx&&p.scalefxAmount==100&&p.brightness==100&&s.video(false).filter==0,"invalid values fall back to the edition defaults");
   need(s.app.backend.status().find(L"invalid MK3 values")!=std::wstring::npos,"fallback reported without blocking the MK3 card");
   const auto log=s.launch(root);need(environment(log,{{"MK1_CRT_PRESET","bvm-1080"},{"MK1_SCALEFX","100"}}),"Remastered defaults BVM 1080p and ScaleFX 100");}
  // Migration: a profile with [Video.Preview.Remastered] (written before F3) keeps only that section and the edition sections'
  // fullscreen key; the next save writes [Video.Remastered] and drops the old section.
  put(ini,"[MK3]\r\nEngine=2\r\n[Video.Original]\r\nscanlines=4\r\nsync=2\r\nfullscreen=1\r\n[Video.Remastered]\r\nscanlines=0\r\nsync=2\r\nfullscreen=0\r\n"
          "[Video.Preview.Remastered]\r\nscalefx=1\r\nscalefxamount=60\r\n");
  {Session s(root);
   need(s.video(false).scanlines==0&&s.video(false).sync==1&&s.video(true).scanlines==4&&s.video(true).scalefxAmount==60&&s.video(true).sync==1&&!s.video(true).fullscreen,
        "[Video.Preview.Remastered] migrated; the unused edition sections ignored but fullscreen");
   s.app.act(3);s.app.act(3);need(s.engine()==2,"save through the card");
   const auto saved=text(ini);
   need(has(saved,"[Video.Remastered]\r\nscanlines=4\r\nfilter=0\r\nsync=1\r\ncolor=0\r\nbrightness=100\r\nscalefxamount=60\r\n")&&has(saved,"[Video.Original]\r\nscanlines=0\r\n")&&
        !has(saved,"Video.Preview"),"migrated profile saved in [Video.Remastered] only");}
  {Session s(root);need(s.video(true).scalefxAmount==60&&s.video(true).scanlines==4&&!s.video(true).fullscreen,"migrated profile reads back the same");}
  // Engine=0 (the removed temporary core) and invalid values read as the native Original.
  for(const char* engine:{"0","7"}){
   put(ini,std::string("[MK3]\r\nEngine=")+engine+"\r\n");
   Session s(root);need(s.engine()==1&&s.app.backend.config.mk3Engine==1&&s.app.mk3Edition()==std::wstring(label(1)),"Engine=0 or invalid reads as the native Original");
  }
  // The temporary core profile is no longer read (core removed after F6); a profile in the pre-F3 format (copied from it by
  // tools/mk3/preview_install.py) is still read as such, and the first save rewrites it in the new format.
  need(fs::remove(ini),"remove the new profile");fs::create_directories(legacy.parent_path());
  const std::string old="[MK3]\r\nEngine=0\r\n[Video.Remastered]\r\nscanlines=0\r\nscalefx=0\r\n[Video.Preview.Remastered]\r\nscalefxamount=60\r\n";put(legacy,old);
  {Session s(root);need(s.engine()==1&&s.video(true).scalefxAmount!=60&&!fs::exists(ini),"old temporary core profile ignored");}
  put(ini,old);
  {Session s(root);need(s.engine()==1&&s.video(true).scalefxAmount==60&&s.video(true).scanlines==4&&s.video(true).scalefx,"pre-F3 profile read, its Engine=0 as 1");
   s.app.act(3);const auto saved=text(ini);
   need(s.engine()==2&&has(saved,"Engine=2\r\n")&&has(saved,"scalefxamount=60\r\n")&&!has(saved,"Video.Preview")&&text(legacy)==old,"first save rewrites it in the new format; the old file is never written");}
  {Session s(root);need(s.engine()==2,"the rewritten profile is kept");}
  // Executable digest differs: the launch is refused, MK3 becomes unavailable, nothing else starts; the choice is kept.
  const auto exe=product/L"mk3.exe";const auto good=text(exe);
  {Session s(root);need(s.engine()==2,"Remastered restored");put(exe,good+'\0');
   s.app.act(20);need(!s.app.backend.running()&&s.engine()==2&&!s.app.mk3Available&&!s.app.gameSelected(),"hash mismatch: refused, MK3 unavailable, nothing started");
   need(s.app.notice.find(L"refused")!=std::wstring::npos&&clean(s.app.notice),"refusal reported without any fallback");
   need(has(text(ini),"Engine=2\r\n"),"saved choice kept after the refusal");put(exe,good);}
  // Package missing: MK3 unavailable, no selection and no launch, the saved choice survives.
  const auto away=product.parent_path()/L"product-away";fs::rename(product,away);
  {Session s(root);s.checked();need(!s.app.mk3Available&&!s.app.gameSelected()&&s.engine()==2,"missing package: MK3 unavailable");
   s.app.act(3);need(s.engine()==2&&has(text(ini),"Engine=2\r\n"),"missing package: no selection, choice kept");
   need(!s.app.backend.launch()&&!s.app.backend.running()&&s.app.backend.status().find(L"refused")!=std::wstring::npos&&clean(s.app.backend.status()),"missing package: launch refused, nothing else started");}
  fs::rename(away,product);
  {Session s(root);s.checked();need(s.app.mk3Available&&s.engine()==2,"package back: saved choice restored");}
  // One ROM missing (a link of the staged package; the source ROM stays): MK3 is not offered.
  std::istringstream roms(text(product/L"roms.txt"));std::string sha,size,path;need(bool(roms>>sha>>size>>path),"staged ROM list");
  need(fs::remove(product/L"roms/mk3"/fs::u8path(path)),"remove one staged ROM link");
  {Session s(root);s.checked();need(!s.app.mk3Available&&!s.app.gameSelected()&&s.engine()==2,"missing ROM: MK3 unavailable");}
  std::cout<<"PASS: MK3 card with the two native editions, saved and restored, Engine=0 and old profile migrated, refusal on hash/package/ROM without fallback, "
             "per-edition [Video.Original]/[Video.Remastered] with [Video.Preview.Remastered] migrated, all native video variables, GameVolume and AudioLatency delivered, MK3 Settings enabled\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
