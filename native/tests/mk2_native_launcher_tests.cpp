#include "launcher_backend.hpp"
#define wWinMain StagedNativeStarterEntry
#include "mk2_native_starter.cpp"
#undef wWinMain
#include "mk2_profile.hpp"
#include "../mk2/mk2_factory_seed.hpp"
#include "mk5_program.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>
namespace fs=std::filesystem;
namespace {
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
void write(const fs::path& path,const std::string& value){fs::create_directories(path.parent_path());std::ofstream stream(path,std::ios::binary);stream<<value;require(bool(stream),"fixture write");}
std::vector<uint8_t> bytes(const fs::path& path){std::ifstream stream(path,std::ios::binary);return {std::istreambuf_iterator<char>(stream),{}};}
std::wstring env(const wchar_t* name){std::vector<wchar_t> value(32768);auto size=GetEnvironmentVariableW(name,value.data(),DWORD(value.size()));return size?std::wstring(value.data(),size):L"";}
struct Environment {std::wstring key,old;Environment(const wchar_t* name,const wchar_t* value):key(name),old(env(name)){SetEnvironmentVariableW(name,value);}~Environment(){SetEnvironmentVariableW(key.c_str(),old.empty()?nullptr:old.c_str());}};
LRESULT CALLBACK proc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp){if(message==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProcW(hwnd,message,wp,lp);}
int child(int argc,wchar_t** argv){
 require(argc==9,"native argc");require(std::wstring(argv[2])==L"--window"&&std::wstring(argv[3])==L"--audio"&&
  std::wstring(argv[4])==L"--host-controls"&&std::wstring(argv[5])==L"--through"&&
  (std::wstring(argv[6])==L"cmos-host"||std::wstring(argv[6])==L"cmos-live")&&std::wstring(argv[7])==L"--profile","native argv");
 const fs::path profile=argv[8];const auto loaded=mk2::LoadCmosProfile(profile);
 const bool publicNative=std::wstring(argv[6])==L"cmos-live";
 if(publicNative){
  require(env(L"MK1_SCALE_MODE")==L"5"&&env(L"MK1_CAS")==L"40"&&env(L"MK1_BILATERAL")==L"70"&&env(L"MK1_SMAA")==L"1","public child did not receive the original video profile options");
 }
 for(auto pair:{std::pair<const wchar_t*,const wchar_t*>{L"MK1_CRT_PRESET",publicNative?L"phosphor":L"energy"},{L"MK1_FILTER",publicNative?L"bicubic":L"nearest"},
  {L"MK1_SCALEFX",L"37"},{L"MK1_SYNC",L"vrr"},{L"MK1_COLOR",L"warm"},{L"MK1_BRIGHTNESS",L"120"},{L"MK1_BEZEL",L"0"},
  {L"MK1_HIGH_REFRESH",L"1"},{L"MK1_FULLSCREEN",L"0"},{L"MK1_AUDIO_LATENCY",L"safe"},
  {L"MK1_MUSIC_MODE",L"original"},{L"MK1_GRAPHICS_VALIDATE",publicNative?L"0":L"1"},{L"PATH",L"Z:/no-python-or-compiler"}})
  require(env(pair.first)==pair.second,"native presentation environment");
 require(env(L"MK1_CRT_GAIN").empty()&&env(L"MK1_DIAGNOSTIC_LEGACY_PACING").empty()&&env(L"MK2_WIDESCREEN").empty(),"ambient override survived");
 WNDCLASSW cls{};cls.lpfnWndProc=proc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"Mk5TemporalScanout";
 require(RegisterClassW(&cls)!=0,"fake hidden class");
 const auto window=CreateWindowW(cls.lpszClassName,L"Hidden native adapter fixture",WS_POPUP,0,0,1,1,nullptr,nullptr,cls.hInstance,nullptr);
 require(window!=nullptr&&!IsWindowVisible(window),"fixture must never show UI");write(profile/L"child-ready.txt","argv/env/profile checked; no guest/GPU/audio execution");
 MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
 if(env(L"MK2_ADAPTER_TEST_CONFLICT")==L"1"){
  auto competitor=loaded.image;competitor[127]^=0x5a;
  mk2::CompareAndSwapCmosProfile(profile,competitor,loaded.sha256);
 }
 mk2::CompareAndSwapCmosProfile(profile,loaded.image,loaded.sha256);return 0;
}
void stop(mklauncher::Backend& backend,const fs::path& profile){
 const auto until=GetTickCount64()+10000;
 while(GetTickCount64()<until&&backend.running()&&(!backend.gameWindow()||!fs::exists(profile/L"child-ready.txt"))){backend.poll();Sleep(20);}
 if(!backend.running()||!backend.gameWindow()){
  const auto output=bytes(profile.parent_path().parent_path()/L"logs/native-experiment-latest.log");
  throw std::runtime_error("owned direct child window was not discovered: "+std::string(output.begin(),output.end()));
 }
 backend.requestStop();
 while(GetTickCount64()<until&&backend.running()){backend.poll();Sleep(20);}require(!backend.running(),"owned direct child did not stop");
}
}
int wmain(int argc,wchar_t** argv){try {
 if(argc>2)return child(argc,argv);
 require(argc==2,"scratch root and project root arguments required");
 const auto root=fs::absolute(argv[1]);const auto project=fs::path(env(L"MK2_ADAPTER_PROJECT_ROOT"));require(!project.empty(),"project fixture context");
 fs::create_directories(root);std::vector<wchar_t> self(32768);require(GetModuleFileNameW(nullptr,self.data(),DWORD(self.size()))!=0,"self path");
 const auto executable=root/L"native prebuilt.exe";fs::copy_file(self.data(),executable);
 const auto seed=mk2::FactorySeedPath(project);fs::copy_file(seed,root/L"factory-seed.bin");
 auto fields=std::map<std::string,std::string>{{"format","1"},{"kind","mk2-native-static-cpp17"},{"readiness","finite-experiment"},
  {"source","native/mk2/mk2_native_check.cpp"},{"source_sha256","c99f2b8260c8f0cfbdc1cee1ab3594f446931b492462542ec1b1f68bdaacaa58"},
  {"rom_profile_sha256","229541f3f6208448af404c5f7cf5e640bfb0200ea5fa188fa40325775feace09"},
  {"first_fight_pcs","9152"},{"maximum_cycle","150023965"},{"executable","native prebuilt.exe"},
  {"executable_sha256",Sha256(bytes(executable))},{"rom_directory",(project/L"roms/mk2").u8string()},
  {"profile_directory","profiles/private native"},{"factory_seed","factory-seed.bin"}};
 const auto manifest=root/L"native-experiment.ini";
 auto manifestWrite=[&]{std::string text;for(const auto& row:fields)text+=row.first+"="+row.second+"\n";write(manifest,text);};manifestWrite();
 const auto settings=root/L"settings.ini";write(settings,"[Launcher]\nFullscreen=0\n[MK2]\nScaleFX=1\nScaleFXAmount=37\nScanlines=7\nFilter=1\nSync=1\nColor=2\nBrightness=120\nBezel=0\nHighRefresh=1\nAudioLatency=2\nRemastered=0\n");
 const auto originalSettings=bytes(settings);const auto profile=root/L"profiles/private native";mklauncher::Backend backend(root);
 require(!backend.available(0)&&!backend.available(1)&&!backend.launch(),"native package implicitly became public JOGAR");
 for(const auto& invalid:std::map<std::string,std::string>{{"kind","mame"},{"readiness","v1-ready"},{"first_fight_pcs","9151"},{"source_sha256",std::string(64,'0')},{"rom_profile_sha256",std::string(64,'0')},{"profile_directory","../legacy"},{"executable_sha256",std::string(64,'0')}}){
  const auto original=fields[invalid.first];fields[invalid.first]=invalid.second;manifestWrite();
  require(!backend.launchNativeExperiment(manifest,settings)&&!backend.running(),"invalid native spec accepted");
  require(!fs::exists(profile),"invalid native spec mutated profile");fields[invalid.first]=original;
 }
 manifestWrite();auto cleanManifest=bytes(manifest);
 write(manifest,std::string(cleanManifest.begin(),cleanManifest.end())+"kind=mk2-native-static-cpp17\n");
 require(!backend.launchNativeExperiment(manifest,settings)&&!fs::exists(profile),"duplicate manifest field accepted");
 write(manifest,std::string(cleanManifest.begin(),cleanManifest.end())+"unexpected=1\n");
 require(!backend.launchNativeExperiment(manifest,settings)&&!fs::exists(profile),"unknown manifest field accepted");
 manifestWrite();auto remastered=std::string(originalSettings.begin(),originalSettings.end());remastered.replace(remastered.find("Remastered=0"),12,"Remastered=1");
 write(settings,remastered);require(!backend.launchNativeExperiment(manifest,settings)&&!fs::exists(profile),"unsupported native soundtrack accepted");
 require(!backend.config.remastered&&backend.config.fullscreen,"failed native Settings leaked into collection");write(settings,std::string(originalSettings.begin(),originalSettings.end()));
 manifestWrite();Environment path(L"PATH",L"Z:/no-python-or-compiler"),gain(L"MK1_CRT_GAIN",L"4"),pacing(L"MK1_DIAGNOSTIC_LEGACY_PACING",L"1"),wide(L"MK2_WIDESCREEN",L"1");
 require(backend.launchNativeExperiment(manifest,settings),"native prebuilt start");require(backend.config.fullscreen&&backend.config.video().scanlines==0,"native Settings leaked into collection state");fs::remove(manifest);stop(backend,profile);
 require(backend.exitCode()==0,"native completion propagation");require(mk2::LoadCmosProfile(profile).image==bytes(seed),"unchanged fake profile bytes");
 require(bytes(settings)==originalSettings,"Settings file was modified");
 manifestWrite();const auto otherRoot=root/L"other portable package";const auto otherManifest=otherRoot/L"native-experiment.ini";
 auto otherFields=fields;otherFields["executable"]=executable.u8string();otherFields["factory_seed"]=(root/L"factory-seed.bin").u8string();
 std::string otherText;for(const auto& row:otherFields)otherText+=row.first+"="+row.second+"\n";write(otherManifest,otherText);
 mklauncher::Backend survivor(otherRoot);require(survivor.launchNativeExperiment(otherManifest,settings),"unrelated native fixture start");
 mklauncher::Backend exceptional(root);bool injected=false;
 try {CloseOwnedSession guard{exceptional};require(exceptional.launchNativeExperiment(manifest,settings),"exception fixture start");throw std::runtime_error("expected caller exception after CreateProcess");}
 catch(const std::runtime_error& error){injected=std::string(error.what())=="expected caller exception after CreateProcess";}
 require(injected&&!exceptional.running()&&exceptional.exitCode()==0,"starter scope did not gracefully close owned native session");
 survivor.poll();require(survivor.running(),"starter guard stopped another native session");stop(survivor,otherRoot/L"profiles/private native");require(survivor.exitCode()==0,"unrelated native fixture completion");
 {
  const auto product=root/L"build/native/mk2-native-v1/product";
  auto playable=fields;playable.erase("maximum_cycle");playable["readiness"]="native-playable";
  playable["source_sha256"]="17c6afbfd9bee9b50bdf4377e12919b05348afb79e5570163c0198562666b345";
  playable["core_overlays_sha256"]="48e76f51909a12fd091c3f19ee19c63710968df8f14caaaced7ce2327eca7c90";
  const std::string runtimeDigest="0c975d4baf08d7fa949406c5a4d42bed12851a5d4608dbd778303cced551bb64";
  playable["runtime_sources_sha256"]=runtimeDigest;
  playable["first_fight_pcs"]="63870";playable["chunk_request_cycles"]="2000000";
  playable["cooperative_slack_cycles"]="2000000";playable["chunk_wall_timeout_ms"]="10000";playable["bounded_observations"]="true";
  playable["executable"]=executable.u8string();playable["factory_seed"]=(root/L"factory-seed.bin").u8string();
  auto publish=[&]{std::string text;for(const auto& row:playable)text+=row.first+"="+row.second+"\n";write(product/L"native-package.ini",text);};
  auto publicSettings=std::string(originalSettings.begin(),originalSettings.end());
  publicSettings.replace(publicSettings.find("Fullscreen=0"),12,"Fullscreen=1");
  publish();write(product.parent_path()/L"profile/launcher.ini","[Video.Remastered]\nScaleMode=2\nFullscreen=1\n[Video.Original]\nScanlines=9\nFilter=3\nScaleMode=5\nCas=40\nBilateral=70\nSmaa=1\nFullscreen=0\n"+publicSettings);
  mklauncher::Backend publicNative(root);require(publicNative.selectGame(1),"native selection/settings");
  require(!publicNative.available(1),"public native package offered before its setup finished");
  const auto record=root/L"generated/mk2"/std::wstring(64,L'c');fs::create_directories(record);fs::copy_file(executable,record/L"mk2-native.exe");
  write(record/L"setup.json","{\"game\":\"mk2\",\"setup_version\":\"14\",\"compile\":{\"executable\":\"generated/mk2/"+std::string(64,'c')+"/mk2-native.exe\",\"sha256\":\""+Sha256(bytes(executable))+"\"}}");
  require(publicNative.available(1),"verified native package unavailable in public collection");
  playable["runtime_sources_sha256"]="f33c2fe77c0ccc6a90cec0178db4a8d794754ed4672fbc9a11c6c4cfdfbefd67";
  publish();require(publicNative.available(1),"published video runtime must stay available");
  playable["runtime_sources_sha256"]="c003710a923b182c512448c5a3e11d2f666adb824ac4f859777f478972578a77";
  publish();require(publicNative.available(1),"previous video runtime must stay available");
  playable["runtime_sources_sha256"]=runtimeDigest;
  for(const auto& invalid:std::map<std::string,std::string>{{"readiness","live-experiment"},{"first_fight_pcs","0"},{"runtime_sources_sha256","not-a-digest"},{"chunk_request_cycles","2000001"},{"executable_sha256",std::string(64,'0')}}){
   const auto original=playable[invalid.first];playable[invalid.first]=invalid.second;publish();
   require(!publicNative.available(1)&&!publicNative.launch()&&!publicNative.running(),"invalid public native package accepted or fallback launched");
   playable[invalid.first]=original;
  }
  publish();require(publicNative.launch(),"public JOGAR did not start verified native directly");
  fs::remove(product/L"native-package.ini");stop(publicNative,product/L"profiles/private native");
  require(publicNative.exitCode()==0,"public native owned close/CAS failed");
  require(mk2::LoadCmosProfile(product/L"profiles/private native").image==bytes(seed),"public native profile changed");
 }
 manifestWrite();fs::remove(profile/L"child-ready.txt");Environment conflict(L"MK2_ADAPTER_TEST_CONFLICT",L"1");
 require(backend.launchNativeExperiment(manifest,settings),"native CAS conflict start");fs::remove(manifest);stop(backend,profile);
 require(backend.exitCode()==1,"native CAS error exit was hidden");auto competing=bytes(seed);competing[127]^=0x5a;
 require(mk2::LoadCmosProfile(profile).image==competing,"CAS conflict overwrote other writer");
 require(bytes(profile/L"factory-seed.bin")==bytes(seed),"immutable factory seed changed");
 manifestWrite();write(profile/L".cmos.lock","foreign");require(!backend.launchNativeExperiment(manifest,settings),"foreign profile lock ignored");
 require(mk2::LoadCmosProfile(profile).image==competing&&bytes(profile/L".cmos.lock")==std::vector<uint8_t>({'f','o','r','e','i','g','n'}),"foreign lock or profile overwritten");fs::remove(profile/L".cmos.lock");
 manifestWrite();write(profile/L"cmos.bin","broken");require(!backend.launchNativeExperiment(manifest,settings),"broken native profile repaired or launched");
 require(bytes(profile/L"cmos.bin")==std::vector<uint8_t>({'b','r','o','k','e','n'}),"broken profile normalized");
 std::cout<<"PASS: strict native manifest, no public JOGAR, prebuilt direct argv/env, C++ prepare/CAS, hidden owned shutdown, removed metadata, profile/CAS errors\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
