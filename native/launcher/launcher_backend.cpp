#include "launcher_backend.hpp"
#include "mk2_profile.hpp"
#include "mk5_program.hpp"
#include "mk5_video_presets.hpp"
#include "launcher_setup.hpp"
#include "launcher_legal.hpp"
#include <tlhelp32.h>
#include <atomic>
#include <array>
#include <algorithm>
#include <climits>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>
namespace fs=std::filesystem;
namespace {
const fs::path stage=L"build/native/manual-temporary-8-20261025";
const fs::path mk2Stage=L"build/native/mk2-arcade-v1";
const fs::path mk2NativeStage=L"build/native/mk2-native-v1";
// MK3 native package (tools/mk3/preview_install.py) and its launcher profile, as mk2-native-v1.
const fs::path mk3Stage=L"build/native/mk3-native";
// Profile stage of MK2 v1 (authenticated core) and MK3 (native package).
const fs::path& coreStage(int game){return game==2?mk3Stage:mk2Stage;}
const wchar_t* coreLogs(int game){return game==2?L"logs/mk3-native":L"logs/mk2-v1";}
const fs::path menuAudioFile=L"build/native/launcher-profile/menu-audio.cfg";
const fs::path launcherPreferencesFile=L"build/native/launcher-profile/launcher.cfg";
const fs::path controlsFile=L"build/native/launcher-profile/controls.cfg";
fs::path publicNativeManifest(const fs::path& root){return root/mk2NativeStage/L"product/native-package.ini";}
bool publicNativePresent(const fs::path& root){
 const auto attributes=GetFileAttributesW(publicNativeManifest(root).c_str());
 return attributes!=INVALID_FILE_ATTRIBUTES||(GetLastError()!=ERROR_FILE_NOT_FOUND&&GetLastError()!=ERROR_PATH_NOT_FOUND);
}
void unlinked(fs::path path){
 for(auto cursor=fs::absolute(path).lexically_normal();!cursor.empty();){
  const DWORD a=GetFileAttributesW(cursor.c_str());
  if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("linked path");
  if(a==INVALID_FILE_ATTRIBUTES&&GetLastError()!=ERROR_FILE_NOT_FOUND&&GetLastError()!=ERROR_PATH_NOT_FOUND)throw std::runtime_error("inaccessible path");
  const auto parent=cursor.parent_path();if(parent==cursor)break;cursor=parent;
 }
}
std::string trim(const std::string& v){const auto first=v.find_first_not_of(" \t\r\n");if(first==std::string::npos)return {};return v.substr(first,v.find_last_not_of(" \t\r\n")-first+1);}
std::string lower(std::string v){for(auto& c:v)if(c>='A'&&c<='Z')c=char(c-'A'+'a');return v;}
bool integer(const std::string& v,int& out){try{size_t end=0;const auto n=std::stol(v,&end,10);if(end!=v.size()||n<INT_MIN||n>INT_MAX)return false;out=int(n);return true;}catch(...){return false;}}
bool validFps(int fps){return fps==30||fps==60||fps==120||fps==240;}
int preference(std::istream& data,const std::string& prefix,int minimum,int maximum){
 std::string token;int value=0;
 if(!(data>>token)||token.compare(0,prefix.size(),prefix)!=0||
    !integer(token.substr(prefix.size()),value)||value<minimum||value>maximum)
  throw std::runtime_error("invalid launcher preference: "+prefix);
 return value;
}
using mklauncher::VideoConfig;
struct VideoNumber {const char* key;int VideoConfig::*member;int minimum,maximum;};
struct VideoToggle {const char* key;bool VideoConfig::*member;};
constexpr VideoNumber videoNumbers[]={
 {"scanlines",&VideoConfig::scanlines,0,int(mk5video::presets.size())-1},
 {"filter",&VideoConfig::filter,0,3},{"sync",&VideoConfig::sync,0,2},{"color",&VideoConfig::color,0,2},
 {"brightness",&VideoConfig::brightness,80,120},{"scalefxamount",&VideoConfig::scalefxAmount,0,100},
 {"scalemode",&VideoConfig::scaleMode,0,5},{"cas",&VideoConfig::cas,0,100},{"bilateral",&VideoConfig::bilateral,0,100},
 {"hdr",&VideoConfig::hdr,0,1},{"hdrpaperwhite",&VideoConfig::hdrPaper,0,480},{"hdrpeak",&VideoConfig::hdrPeak,0,10000},
 {"hdrhighlights",&VideoConfig::hdrHighlights,0,100}
};
constexpr VideoToggle videoToggles[]={
 {"scalefx",&VideoConfig::scalefx},{"bezel",&VideoConfig::bezel},{"highrefresh",&VideoConfig::highRefresh},
 {"smaa",&VideoConfig::smaa},{"fullscreen",&VideoConfig::fullscreen},{"widescreen",&VideoConfig::widescreen}
};
constexpr VideoConfig videoDefaults;
constexpr const wchar_t* filters[]={L"nearest",L"linear",L"sharp-bilinear",L"bicubic"};
bool needsModernVideo(const VideoConfig& v){return v.filter>1||v.scaleMode||v.cas||v.bilateral||v.smaa;}
bool validVideoNumber(const VideoNumber& field,int value){
 return value>=field.minimum&&value<=field.maximum&&(field.member!=&VideoConfig::brightness||value%10==0)&&
  (field.member!=&VideoConfig::hdrPaper||value==0||value>=80)&&(field.member!=&VideoConfig::hdrPeak||value==0||value>=400);
}
bool readVideo(VideoConfig& video,const std::string& key,const std::string& value,const VideoConfig& defaults=videoDefaults){
 for(const auto& field:videoNumbers)if(key==field.key){
  int n=0;const bool valid=integer(value,n)&&validVideoNumber(field,n);
  video.*field.member=valid?n:defaults.*field.member;return valid;
 }
 for(const auto& field:videoToggles)if(key==field.key){
  int n=0;const bool valid=integer(value,n)&&(n==0||n==1);
  video.*field.member=valid?n!=0:defaults.*field.member;return valid;
 }
 return true;
}
void writeVideo(std::ostream& text,const VideoConfig& video){
 for(const auto& field:videoNumbers)text<<field.key<<'='<<video.*field.member<<"\r\n";
 for(const auto& field:videoToggles)text<<field.key<<'='<<int(video.*field.member)<<"\r\n";
}
// Factory video profiles of a game (capabilities as readSettings): MK3 Remastered on BVM / 1080p and ScaleFX 100.
mklauncher::Config factory(int game){mklauncher::Config c;if(game==2)c.videos[1]=mklauncher::mk3RemasteredDefaults();return c;}
void validate(mklauncher::Config& c,int game=0){
 if(c.gameVolume<0||c.gameVolume>100)c.gameVolume=100;
 if(!validFps(c.fps))c.fps=60;
 c.videos[0].hdrHighlights=0; // the Original edition keeps the SDR image at paper white
 c.videos[0].widescreen=false; // and the arcade's 4:3
 if(c.audioLatency<1||c.audioLatency>2)c.audioLatency=1;
 c.uiVolume=0; // Legacy key; coin feedback follows the Windows output volume.
 // MK2 v1 (authenticated core) has neither the remastered music nor the native presenter options; MK3 has the native
 // presenter (all options, as native MK2) and no remastered music (D7: its Remastered edition is video only).
 if(game==1||game==2)c.remastered=false;
 if(game==1)c.videoEffects=false;
 if(game!=2||c.mk3Engine!=2)c.mk3Engine=1; // 0 (the removed temporary core) and invalid values migrate to the native Original
 const auto defaults=factory(game).videos;
 for(size_t i=0;i<c.videos.size();++i){auto& video=c.videos[i];
  for(const auto& field:videoNumbers)
   if(!validVideoNumber(field,video.*field.member))video.*field.member=defaults[i].*field.member;
  if(game==1){video.scalefx=false;video.highRefresh=false;if(video.scanlines>3)video.scanlines=0;if(video.filter>1)video.filter=0;if(video.sync==1)video.sync=2;}
 }
}
std::wstring quote(const std::wstring& s){std::wstring out=L"\"";unsigned slashes=0;
 for(wchar_t ch:s){if(ch==L'\\'){++slashes;continue;}if(ch==L'\"'){out.append(slashes*2+1,L'\\');out+=ch;}else{out.append(slashes,L'\\');out+=ch;}slashes=0;}
 out.append(slashes*2,L'\\');out+=L'\"';return out;
}
struct Handle {HANDLE h=INVALID_HANDLE_VALUE;~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}Handle()=default;Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;};
bool ordinary(const fs::path& p){unlinked(p);return fs::is_regular_file(p);}
std::string smallFile(const fs::path& p,size_t max){unlinked(p);if(fs::file_size(p)>max)throw std::runtime_error("oversized file");std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("read file");return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};}
void writePreferences(const fs::path& path,const std::string& bytes){
 unlinked(path);fs::create_directories(path.parent_path());unlinked(path);
 static std::atomic<unsigned> serial{0};
 auto temporary=path;temporary+=L".tmp-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++serial);unlinked(temporary);
 bool created=false;
 try{
  {Handle file;file.h=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
   if(file.h==INVALID_HANDLE_VALUE)throw std::runtime_error("create config");
   created=true;
   DWORD written=0;
   if(!WriteFile(file.h,bytes.data(),DWORD(bytes.size()),&written,nullptr)||written!=bytes.size()||!FlushFileBuffers(file.h))throw std::runtime_error("write config");}
  unlinked(path);unlinked(temporary);
  if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("replace config Win32 "+std::to_string(GetLastError()));
 }catch(...){
  if(created){try{unlinked(temporary);DeleteFileW(temporary.c_str());}catch(...){}}
  throw;
 }
}
}
namespace {
struct NativeRom {const char* path;uintmax_t size;const char* sha;};
const NativeRom nativeRoms[]={
#include "native_roms.inc"
};
// SHA-256 of package files already read, kept while size and write time are unchanged: the launcher's periodic
// availability check reads tens of MB otherwise. Starting a game forgets them (forgetFileHashes), so a launch always
// hashes what it runs.
struct HashedFile {uintmax_t size;fs::file_time_type written;std::string sha;};
std::mutex hashedLock;std::map<fs::path,HashedFile> hashed;
std::string fileHash(const fs::path& path,uintmax_t maximum,uintmax_t exact=0){
 if(!ordinary(path))throw std::runtime_error("native package file is not ordinary");
 const auto size=fs::file_size(path);if(size>maximum||(exact&&size!=exact))throw std::runtime_error("native package file size differs");
 const auto written=fs::last_write_time(path);const auto key=fs::absolute(path).lexically_normal();
 {std::lock_guard<std::mutex> hold(hashedLock);const auto it=hashed.find(key);
  if(it!=hashed.end()&&it->second.size==size&&it->second.written==written)return it->second.sha;}
 std::vector<uint8_t> data(size);std::ifstream input(path,std::ios::binary);
 if(!input.read(reinterpret_cast<char*>(data.data()),std::streamsize(size))||input.peek()!=EOF)throw std::runtime_error("native package file read differs");
 auto sha=Sha256(data);
 std::lock_guard<std::mutex> hold(hashedLock);hashed[key]={size,written,sha};return sha;
}
void forgetFileHashes(){std::lock_guard<std::mutex> hold(hashedLock);hashed.clear();}
struct NativeSpec {fs::path root,executable,roms,profile,seed;bool live=false,playable=false,modernVideo=false,liveWithoutPixelCheck=false;};
// Remastered music is the user's own (custom_audio/<game>/, optional per slot); the Arcade Original never receives it.
fs::path customAudio(const fs::path& root,const wchar_t* game){return root/L"custom_audio"/game;}
fs::path nativePath(const fs::path& root,const std::string& text,bool profile=false){
 auto path=fs::u8path(text);if(path.empty()||(!path.is_absolute()&&path.has_root_path())||(profile&&path.is_absolute()))throw std::runtime_error("native package path must be explicit; profile must be relative");
 for(const auto& part:path)if(part==L".."||part==L".")throw std::runtime_error("native package traversal rejected");
 path=(path.is_absolute()?path:root/path).lexically_normal();unlinked(path);return path;
}
NativeSpec nativeSpec(const fs::path& root,const fs::path& manifest,bool verifyPayload=true){
 const auto absolute=fs::absolute(manifest).lexically_normal();const auto raw=smallFile(absolute,8192);
 if(raw.find('\0')!=std::string::npos)throw std::runtime_error("native manifest is binary");
 std::map<std::string,std::string> fields;std::istringstream lines(raw);std::string line;
 while(std::getline(lines,line)){
  line=trim(line);if(line.empty()||line[0]=='#'||line[0]==';')continue;
  const auto eq=line.find('=');if(eq==std::string::npos)throw std::runtime_error("native manifest syntax");
  const auto key=lower(trim(line.substr(0,eq))),value=trim(line.substr(eq+1));
  if(value.empty()||!fields.emplace(key,value).second)throw std::runtime_error("native manifest empty or duplicate field");
 }
 const std::set<std::string> names={"format","kind","readiness","source","source_sha256","rom_profile_sha256","first_fight_pcs","maximum_cycle","executable","executable_sha256","rom_directory","profile_directory","factory_seed"};
 const bool playable=fields.count("readiness")&&fields.at("readiness")=="native-playable";
 const bool live=playable||(fields.count("readiness")&&fields.at("readiness")=="live-experiment");
 auto expected=names;
 if(live){expected.erase("maximum_cycle");expected.insert({"core_overlays_sha256","chunk_request_cycles","cooperative_slack_cycles","chunk_wall_timeout_ms","bounded_observations"});}
 if(playable)expected.insert("runtime_sources_sha256");
 if(fields.size()!=expected.size())throw std::runtime_error("native manifest fields differ");
 for(const auto& field:fields)if(!expected.count(field.first))throw std::runtime_error("unknown native manifest field");
 if(!live){
 if(fields.at("format")!="1"||fields.at("kind")!="mk2-native-static-cpp17"||fields.at("readiness")!="finite-experiment"||
    fields.at("source")!="native/mk2/mk2_native_check.cpp"||
    fields.at("source_sha256")!="c99f2b8260c8f0cfbdc1cee1ab3594f446931b492462542ec1b1f68bdaacaa58"||
    fields.at("rom_profile_sha256")!="229541f3f6208448af404c5f7cf5e640bfb0200ea5fa188fa40325775feace09"||
    fields.at("first_fight_pcs")!="9152"||fields.at("maximum_cycle")!="150023965")
  throw std::runtime_error("native manifest kind/source/ROM/readiness is not this finite experiment");
 }else if(playable){
  // The public game is identified by the executable that the setup built on this machine
  // (generated/mk2/<sha>/setup.json); no hash of generated code is committed for it.
  int pcs=0;
  if(fields.at("format")!="1"||fields.at("kind")!="mk2-native-static-cpp17"||fields.at("source")!="native/mk2/mk2_native_check.cpp"||
     !mklauncher::hex64(fields.at("source_sha256"))||!mklauncher::hex64(fields.at("core_overlays_sha256"))||!mklauncher::hex64(fields.at("runtime_sources_sha256"))||
     fields.at("rom_profile_sha256")!="229541f3f6208448af404c5f7cf5e640bfb0200ea5fa188fa40325775feace09"||
     !integer(fields.at("first_fight_pcs"),pcs)||pcs<=0||fields.at("chunk_request_cycles")!="2000000"||
     fields.at("cooperative_slack_cycles")!="2000000"||fields.at("chunk_wall_timeout_ms")!="10000"||fields.at("bounded_observations")!="true")
   throw std::runtime_error("native playable package source/ROM/execution contract differs");
  const auto record=mklauncher::readSetup(root,L"mk2");
  if(!record.finished)throw std::runtime_error("MK2 setup has not finished on this machine");
  if(fields.at("executable_sha256")!=record.executableSha256)throw std::runtime_error("native executable is not the one built by the local setup");
 }else{
  if(fields.at("format")!="1"||fields.at("kind")!="mk2-native-static-cpp17"||
     fields.at("source")!="native/mk2/mk2_native_check.cpp"||
     fields.at("source_sha256")!="d5d156a8f43c9893235a60b6ac88faa35c9bb185ceb457b785ca1400a5316951"||
     fields.at("core_overlays_sha256")!="d4dd43e6a7603287115b9dc1baa6c26af04c3245016fd03041f98a65188373c6"||
     fields.at("rom_profile_sha256")!="229541f3f6208448af404c5f7cf5e640bfb0200ea5fa188fa40325775feace09"||
     fields.at("first_fight_pcs")!="10356"||fields.at("chunk_request_cycles")!="2000000"||
     fields.at("cooperative_slack_cycles")!="2000000"||fields.at("chunk_wall_timeout_ms")!="10000"||
     fields.at("bounded_observations")!="true")
   throw std::runtime_error("native manifest is not this live experiment with bounded observations");
 }
 const auto& digest=fields.at("executable_sha256");if(digest.size()!=64||digest.find_first_not_of("0123456789abcdef")!=std::string::npos)throw std::runtime_error("native executable SHA-256 malformed");
 NativeSpec spec;spec.live=live;spec.playable=playable;spec.modernVideo=playable;
 spec.liveWithoutPixelCheck=playable;
 spec.root=absolute.parent_path();spec.executable=nativePath(spec.root,fields.at("executable"));
 spec.roms=nativePath(spec.root,fields.at("rom_directory"));spec.profile=nativePath(spec.root,fields.at("profile_directory"),true);spec.seed=nativePath(spec.root,fields.at("factory_seed"));
 if(verifyPayload){
  if(fileHash(spec.executable,128*1024*1024)!=digest)throw std::runtime_error("native executable SHA-256 differs");
  for(const auto& rom:nativeRoms)if(fileHash(spec.roms/rom.path,rom.size,rom.size)!=rom.sha)throw std::runtime_error("native ROM SHA-256 differs");
 }
 return spec;
}
struct EnvironmentLess {bool operator()(const std::wstring& a,const std::wstring& b)const{return _wcsicmp(a.c_str(),b.c_str())<0;}};
// Native MK2 and MK3: the presenter (Mk5Window) and the Windows PCM sink (GameVolume, AudioLatency) read these MK1_* values.
// The user's own game frame (custom_bezel/<game>/bezel.png, local and ignored by Git); empty when absent.
fs::path customBezel(const fs::path& root,size_t game){
 std::error_code error;const auto image=root/L"custom_bezel"/mklauncher::compatibleGames.at(game).setupId/L"bezel.png";
 return fs::is_regular_file(image,error)?image:fs::path{};
}
std::vector<wchar_t> nativeEnvironment(const mklauncher::Config& c,const VideoConfig& v,const fs::path& music,bool modernVideo,bool validateGraphics,
                                      const fs::path& bezelImage,const mk5controls::Config& controls,const wchar_t* widescreen=nullptr){
 std::map<std::wstring,std::wstring,EnvironmentLess> values;auto* original=GetEnvironmentStringsW();if(!original)throw std::runtime_error("native process environment");
 for(const auto* cursor=original;*cursor;cursor+=wcslen(cursor)+1){
  const std::wstring row=cursor;const auto eq=row.find(L'=',row[0]==L'='?1:0);
  if(eq!=std::wstring::npos&&_wcsnicmp(row.c_str(),L"MK1_",4)!=0&&_wcsnicmp(row.c_str(),L"MK2_WIDESCREEN=",15)!=0&&_wcsnicmp(row.c_str(),L"MK3_WIDESCREEN=",15)!=0)values[row.substr(0,eq)]=row.substr(eq+1);
 }FreeEnvironmentStringsW(original);
 const auto& preset=mk5video::presets[size_t(v.scanlines)];
 values[L"MK1_SCANLINES"]=preset.scanlines;values[L"MK1_CRT_PRESET"]=preset.crt;
 // The previously installed runtime and experiment binaries have the older filter contract.
 values[L"MK1_FILTER"]=modernVideo?filters[v.filter]:v.scanlines<4&&v.filter==1&&!(v.scalefx&&v.scalefxAmount)?L"linear":L"nearest";
 values[L"MK1_SCALEFX"]=std::to_wstring(v.scalefx&&(modernVideo||v.scanlines!=mk5video::phosphor_index)?v.scalefxAmount:0);
 if(modernVideo){
  values[L"MK1_SCALE_MODE"]=std::to_wstring(v.scaleMode);values[L"MK1_CAS"]=std::to_wstring(v.cas);
  values[L"MK1_BILATERAL"]=std::to_wstring(v.bilateral);values[L"MK1_SMAA"]=v.smaa?L"1":L"0";
 }
 values[L"MK1_GAME_VOLUME"]=std::to_wstring(c.gameVolume);
 values[L"MK1_SYNC"]=std::array<const wchar_t*,3>{L"vsync",L"vrr",L"immediate"}[v.sync];values[L"MK1_COLOR"]=std::array<const wchar_t*,3>{L"original",L"vivid",L"warm"}[v.color];
 values[L"MK1_BRIGHTNESS"]=std::to_wstring(v.brightness);values[L"MK1_BEZEL"]=v.bezel?L"1":L"0";values[L"MK1_HIGH_REFRESH"]=v.highRefresh?L"1":L"0";values[L"MK1_FULLSCREEN"]=v.fullscreen?L"1":L"0";
 values[L"MK1_HDR"]=v.hdr?L"auto":L"off";values[L"MK1_HDR_PAPER_WHITE"]=v.hdrPaper?std::to_wstring(v.hdrPaper):L"auto";
 values[L"MK1_HDR_PEAK"]=v.hdrPeak?std::to_wstring(v.hdrPeak):L"auto";values[L"MK1_HDR_HIGHLIGHTS"]=std::to_wstring(v.hdrHighlights);
 if(!bezelImage.empty())values[L"MK1_BEZEL_IMAGE"]=bezelImage.wstring();
 {const auto text=mk5controls::format(controls);values[L"MK1_CONTROLS"]=std::wstring(text.begin(),text.end());}
 values[L"MK1_AUDIO_LATENCY"]=c.audioLatency==2?L"safe":L"balanced";values[L"MK1_MUSIC_MODE"]=c.remastered?L"remastered":L"original";values[L"MK1_GRAPHICS_VALIDATE"]=validateGraphics?L"1":L"0";
 if(c.remastered)values[L"MK1_MUSIC_DIRECTORY"]=music.wstring();
 if(widescreen)values[widescreen]=L"1";   // MK1_WIDESCREEN, MK2_WIDESCREEN or MK3_WIDESCREEN: the game's Remastered launch only
 std::vector<wchar_t> block;for(const auto& value:values){const auto row=value.first+L"="+value.second;block.insert(block.end(),row.begin(),row.end());block.push_back(0);}block.push_back(0);return block;
}
std::wstring nativeError(const char* error){
 const int size=MultiByteToWideChar(CP_UTF8,0,error,-1,nullptr,0);if(size<=1)return L"Native error.";
 std::wstring text(size,0);MultiByteToWideChar(CP_UTF8,0,error,-1,text.data(),size);text.pop_back();return text;
}
// MK3 native package (tools/mk3/preview_install.py): Profile::Game build, fidelity claimed only on compared routes.
// SHA-256 of roms.txt ("sha256 size path" per authenticated MK3 rev 2.1 ROM, tools/mk3/roms manifest order).
constexpr const char* mk3RomList="e755b48202380163da989a195fc65924bdf5924a087823060e95c6c699db20e9";
struct Mk3Spec {fs::path root,executable,roms,graphics,seed,profile;};
Mk3Spec mk3Spec(const fs::path& root,bool verifyPayload){
 const auto product=root/mk3Stage/L"product";const auto raw=smallFile(product/L"package.ini",8192);
 if(raw.find('\0')!=std::string::npos)throw std::runtime_error("MK3 native manifest is binary");
 std::map<std::string,std::string> fields;std::istringstream lines(raw);std::string line;
 while(std::getline(lines,line)){
  line=trim(line);if(line.empty()||line[0]=='#'||line[0]==';')continue;
  const auto eq=line.find('=');if(eq==std::string::npos)throw std::runtime_error("MK3 native manifest syntax");
  const auto key=lower(trim(line.substr(0,eq))),value=trim(line.substr(eq+1));
  if(value.empty()||!fields.emplace(key,value).second)throw std::runtime_error("MK3 native manifest empty or duplicate field");
 }
 const std::set<std::string> names={"format","kind","readiness","label","game","revision","program_sha256","executable","executable_sha256",
  "rom_directory","rom_list","graphics_list","factory_seed","profile_directory","editions","fidelity"};
 if(fields.size()!=names.size())throw std::runtime_error("MK3 native manifest fields differ");
 for(const auto& field:fields)if(!names.count(field.first))throw std::runtime_error("unknown MK3 native manifest field");
 if(fields.at("format")!="1"||fields.at("kind")!="mk3-native"||fields.at("readiness")!="product"||fields.at("game")!="mk3"||
    fields.at("revision")!="2.1"||fields.at("editions")!="original,remastered")throw std::runtime_error("MK3 native manifest kind differs");
 Mk3Spec spec;spec.root=product;spec.executable=nativePath(product,fields.at("executable"),true);
 spec.roms=nativePath(product,fields.at("rom_directory"),true);spec.graphics=nativePath(product,fields.at("graphics_list"),true);
 spec.seed=nativePath(product,fields.at("factory_seed"),true);spec.profile=nativePath(product,fields.at("profile_directory"),true);
 const auto list=nativePath(product,fields.at("rom_list"),true);
 if(fileHash(list,8192)!=mk3RomList)throw std::runtime_error("MK3 native ROM list is not the authenticated rev 2.1 set");
 const auto& digest=fields.at("executable_sha256");if(digest.size()!=64||digest.find_first_not_of("0123456789abcdef")!=std::string::npos)throw std::runtime_error("MK3 native executable SHA-256 malformed");
 if(!ordinary(spec.executable)||!ordinary(spec.graphics)||!ordinary(spec.seed)||fs::file_size(spec.seed)!=49152)throw std::runtime_error("MK3 native package incomplete");
 std::istringstream roms(smallFile(list,8192));std::string sha,path;uintmax_t size=0;size_t count=0;
 while(roms>>sha>>size>>path){
  const auto file=nativePath(spec.roms,path,true);if(!ordinary(file)||fs::file_size(file)!=size)throw std::runtime_error("MK3 native ROM missing");
  if(verifyPayload&&fileHash(file,size,size)!=sha)throw std::runtime_error("MK3 native ROM SHA-256 differs");
  ++count;
 }
 if(count!=26)throw std::runtime_error("MK3 native ROM count differs");
 const auto record=mklauncher::readSetup(root,L"mk3");
 if(!record.finished||digest!=record.executableSha256)throw std::runtime_error("MK3 native executable is not the one built by the local setup");
 if(verifyPayload&&fileHash(spec.executable,256*1024*1024)!=digest)throw std::runtime_error("MK3 native executable SHA-256 differs");
 return spec;
}
}
namespace {
const fs::path setupFoldersFile=L"build/native/launcher-profile/setup.cfg";
std::string setupId(size_t game){const std::wstring id=mklauncher::compatibleGames.at(game).setupId;return {id.begin(),id.end()};}
fs::path setupProgress(const fs::path& root,size_t game){return root/L"logs"/(L"setup-"+std::wstring(mklauncher::compatibleGames.at(game).setupId)+L".progress.json");}
// The user's Python, looked up on PATH only (never the launcher or working folder): python.exe, then the py launcher.
std::vector<std::wstring> interpreters(){
 std::vector<std::wstring> out;const DWORD need=GetEnvironmentVariableW(L"PATH",nullptr,0);if(!need)return out;
 std::wstring path(need,L'\0');path.resize(GetEnvironmentVariableW(L"PATH",path.data(),need));
 for(const auto& [name,arguments]:{std::pair{L"python.exe",L""},std::pair{L"py.exe",L" -3"}}){
  std::vector<wchar_t> found(32768);const DWORD n=SearchPathW(path.c_str(),name,nullptr,DWORD(found.size()),found.data(),nullptr);
  if(n&&n<found.size())out.push_back(quote(found.data())+arguments);
 }
 return out;
}
// Progress and requirement files: shared for delete, so setup.py can replace them atomically while the launcher reads.
bool readObject(const fs::path& path,tinyjson::Value& value){
 if(tinyjson::Linked(path))return false;
 Handle file;file.h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file.h==INVALID_HANDLE_VALUE)return false;
 std::string bytes(65536,'\0');DWORD n=0;
 if(!ReadFile(file.h,bytes.data(),DWORD(bytes.size()),&n,nullptr)||n==bytes.size())return false;
 bytes.resize(n);tinyjson::Parser parser(bytes);
 return parser.Parse(value)&&parser.End()&&value.kind==tinyjson::Value::Object;
}
const tinyjson::Value* member(const tinyjson::Value& object,const char* key,tinyjson::Value::Kind kind){
 const auto f=object.fields.find(key);return f!=object.fields.end()&&f->second.kind==kind?&f->second:nullptr;
}
std::string text(const tinyjson::Value& object,const char* key){const auto* v=member(object,key,tinyjson::Value::String);return v?v->text:std::string();}
double number(const tinyjson::Value& object,const char* key){const auto* v=member(object,key,tinyjson::Value::Number);return v?v->number:0;}
// One setup.py run in a kill-on-close Job Object: Cancel, or closing the launcher, ends its whole process tree.
HANDLE spawnSetup(const fs::path& root,std::wstring command,const fs::path& out,const fs::path& err,HANDLE& job){
 for(const auto& path:{out,err}){unlinked(path);fs::create_directories(path.parent_path());unlinked(path);}
 SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};Handle output,error,input,owned;
 output.h=CreateFileW(out.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(err!=out)error.h=CreateFileW(err.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
 input.h=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
 const HANDLE errors=err==out?output.h:error.h;
 if(output.h==INVALID_HANDLE_VALUE||errors==INVALID_HANDLE_VALUE||input.h==INVALID_HANDLE_VALUE)throw std::runtime_error("setup log");
 owned.h=CreateJobObjectW(nullptr,nullptr);if(!owned.h)throw std::runtime_error("setup job");
 JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
 if(!SetInformationJobObject(owned.h,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))throw std::runtime_error("setup job limits");
 STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdInput=input.h;si.hStdOutput=output.h;si.hStdError=errors;
 PROCESS_INFORMATION pi{};
 if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,root.c_str(),&si,&pi))
  throw std::runtime_error("setup CreateProcess");
 if(!AssignProcessToJobObject(owned.h,pi.hProcess)){TerminateProcess(pi.hProcess,ERROR_CANCELLED);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
  throw std::runtime_error("setup job assignment");}
 ResumeThread(pi.hThread);CloseHandle(pi.hThread);job=owned.h;owned.h=nullptr;return pi.hProcess;
}
}
namespace mklauncher {
Backend::Backend(const fs::path& root):root_(fs::absolute(root).lexically_normal()),status_(L"Choose the folder with your ROM files to prepare the games."){
 try{loadSetupFolders();}catch(...){/* unreadable setup.cfg: folders are chosen again */}
 for(int i=0;i<3;++i)refreshSetupEstimate(i);
}
Backend::~Backend(){if(process_)CloseHandle(process_);if(setupProcess_)CloseHandle(setupProcess_);if(setupJob_)CloseHandle(setupJob_);}
bool Backend::setupFinished(int index)const{
 if(index<0||index>2)return false;
 try{return readSetup(root_,compatibleGames[size_t(index)].setupId).finished;}catch(...){return false;}
}
bool Backend::setupRunning(){pollSetup();return setupProcess_||!setupQueue_.empty();}
// Runs tools/setup/setup.py for every game the setup supports, on the folder chosen by the user. Nothing is downloaded.
bool Backend::startSetup(const fs::path& roms){
 if(running()||setupRunning()){status_=L"Wait for the running game or setup to finish.";return false;}
 if(!acknowledged_){status_=L"Confirm on the Legal page that you own the original ROM files.";return false;}
 for(int i=0;i<3;++i)if(!setRomFolder(i,roms))return false;
 for(int i=0;i<3;++i)queueSetup(i,false);
 status_=L"Setup running: checking your ROM files and generating the games on this machine...";return true;
}
bool Backend::acknowledgeRoms(bool value){
 const bool before=acknowledged_;acknowledged_=value;
 if(saveSetupFolders())return true;
 acknowledged_=before;status_=L"Could not save the confirmation.";return false;
}
fs::path Backend::setupLog(int index)const{return root_/L"logs"/(L"setup-"+std::wstring(compatibleGames.at(size_t(index)).setupId)+L".log");}
namespace {
// Text of setup.py shown as is only when it is one short plain sentence (never a JSON dump, traceback or path list).
bool plain(const std::string& text){return !text.empty()&&text.size()<=200&&text.find_first_of("{}[]\r\n")==std::string::npos;}
std::wstring hideProfile(std::wstring text){
 wchar_t profile[MAX_PATH]{};if(!GetEnvironmentVariableW(L"USERPROFILE",profile,MAX_PATH)||!profile[0])return text;
 const auto lower=[](std::wstring s){for(auto& c:s)c=wchar_t(towlower(c));return s;};
 std::wstring slash=profile;std::replace(slash.begin(),slash.end(),L'\\',L'/');
 for(const auto& key:{lower(profile),lower(slash)})
  for(size_t at;(at=lower(text).find(key))!=std::wstring::npos;)text.replace(at,key.size(),L"%USERPROFILE%");
 return text;
}
std::wstring logTail(const fs::path& log,size_t lines){
 std::ifstream in(log,std::ios::binary);if(!in)return L"(no log)";
 in.seekg(0,std::ios::end);const std::streamoff size=in.tellg(),take=std::min<std::streamoff>(size,32768);
 std::string bytes(size_t(take),'\0');in.seekg(size-take);in.read(bytes.data(),take);
 size_t at=bytes.size(),n=0;while(at&&n<=lines)if(bytes[--at]=='\n')++n;
 if(n>lines)bytes.erase(0,at+1);
 return nativeError(bytes.c_str());
}
}
std::wstring Backend::failureText(int index)const{
 const auto& g=setupGames_.at(size_t(index));
 return !g.code.empty()&&plain(g.message)?nativeError(g.message.c_str()):L"Something went wrong at this step.";
}
std::wstring Backend::failureHint(int index)const{
 const auto& hint=setupGames_.at(size_t(index)).hint;
 return plain(hint)?nativeError(hint.c_str()):L"Open the log for details, or use Copy report and send it to the project.";
}
std::wstring Backend::setupReport(int index)const{
 const auto& g=setupGames_.at(size_t(index));const auto& r=requirements_;const auto& game=compatibleGames.at(size_t(index));
 const auto w=[](const std::string& s){return s.empty()?std::wstring(L"-"):nativeError(s.c_str());};
 std::wstring text=L"TriUnit Recomp setup report\nGame: "+std::wstring(game.title)+L" ("+game.setupId+L")\nSetup version: "+w(kSetupVersion)+
  L"\nPython: "+w(r.python)+L"\ng++: "+w(r.compiler)+L" (tested: "+w(r.validated)+L")\nStep: "+w(g.step)+L"\nResult: "+w(g.state)+
  L" ["+w(g.code)+L"]\nMessage: "+w(g.message)+L"\nDetail: "+w(g.detail)+L"\n\nLast lines of "+setupLog(index).filename().wstring()+L":\n"+
  logTail(setupLog(index),40);
 std::wstring crlf;for(const wchar_t c:text){if(c==L'\n'&&(crlf.empty()||crlf.back()!=L'\r'))crlf+=L'\r';crlf+=c;}
 return hideProfile(crlf);
}
fs::path Backend::setupOutput(int index)const{
 if(index<0||index>2)return {};
 try{return readSetup(root_,compatibleGames[size_t(index)].setupId).directory;}catch(...){return {};}
}
void Backend::refreshSetupEstimate(int index){
 try{const auto record=readSetup(root_,compatibleGames[size_t(index)].setupId);setupGames_[size_t(index)].estimate=record.finished?record.seconds:0;}
 catch(...){setupGames_[size_t(index)].estimate=0;}
}
void Backend::loadSetupFolders(){
 const auto file=root_/setupFoldersFile;if(!ordinary(file))return;
 std::istringstream lines(smallFile(file,65536));std::string line;
 while(std::getline(lines,line)){
  line=trim(line);const auto equals=line.find('=');if(equals==std::string::npos)continue;
  if(line=="acknowledged=1")acknowledged_=true;
  const auto folder=fs::u8path(line.substr(equals+1));
  for(size_t i=0;i<3;++i)if(line.compare(0,equals,setupId(i))==0&&folder.is_absolute())setupGames_[i].folder=folder;
 }
}
bool Backend::saveSetupFolders()const{
 std::string bytes=acknowledged_?"acknowledged=1\n":"";
 for(size_t i=0;i<3;++i)if(!setupGames_[i].folder.empty())bytes+=setupId(i)+"="+setupGames_[i].folder.u8string()+"\n";
 try{writePreferences(root_/setupFoldersFile,bytes);return true;}catch(...){return false;}
}
bool Backend::setRomFolder(int index,const fs::path& folder){
 if(index<0||index>2)return false;
 auto& game=setupGames_[size_t(index)];
 if(game.queued||game.active){status_=L"Wait for this game's setup to finish.";return false;}
 try{
  const auto chosen=fs::absolute(folder).lexically_normal();unlinked(chosen);if(!fs::is_directory(chosen))throw std::runtime_error("ROM folder");
  if(chosen!=game.folder){game.folder=chosen;game.verified=false;game.files.clear();game.checks.clear();game.program.clear();}
  if(!saveSetupFolders())throw std::runtime_error("setup folders not saved");
  return true;
 }catch(const std::exception& error){status_=L"Folder refused: "+nativeError(error.what());return false;}
}
bool Backend::queueSetup(int index,bool verifyOnly){
 if(index<0||index>2)return false;
 auto& game=setupGames_[size_t(index)];
 if(!acknowledged_){status_=L"Confirm on the Legal page that you own the original ROM files.";return false;}
 if(running()){status_=L"Close the game before preparing files.";return false;}
 if(game.folder.empty()){status_=L"Choose the ROM folder of this game.";return false;}
 if(game.queued||game.active){status_=L"This game is already in the setup; wait or cancel.";return false;}
 game.queued=true;game.mode=verifyOnly?"verify":"setup";game.state="queued";game.step.clear();game.message.clear();
 setupQueue_.push_back({index,verifyOnly});pollSetup();return true;
}
void Backend::checkRequirements(){
 if(activeSetup_.game==-1&&setupProcess_)return;
 for(const auto& job:setupQueue_)if(job.game==-1)return;
 requirements_={};interpreter_.clear();interpreterTry_=0;setupQueue_.push_front({-1,false});pollSetup();
}
void Backend::cancelSetup(){
 for(const auto& job:setupQueue_)if(job.game>=0){auto& game=setupGames_[size_t(job.game)];game.queued=false;game.state="cancelled";}
 setupQueue_.clear();
 if(setupJob_){setupCancelled_=true;TerminateJobObject(setupJob_,ERROR_CANCELLED);}
 status_=L"Setup cancelled.";
}
void Backend::pollSetup(){
 if(setupProcess_){
  const bool exited=WaitForSingleObject(setupProcess_,0)==WAIT_OBJECT_0;
  if(activeSetup_.game>=0&&(exited||GetTickCount64()>=nextSetupRead_)){nextSetupRead_=GetTickCount64()+250;readSetupProgress(activeSetup_.game);}
  if(!exited)return;
  DWORD code=ERROR_GEN_FAILURE;if(!GetExitCodeProcess(setupProcess_,&code))code=ERROR_GEN_FAILURE;
  CloseHandle(setupProcess_);setupProcess_=nullptr;CloseHandle(setupJob_);setupJob_=nullptr;
  finishSetup(code);
 }
 while(!setupProcess_&&!setupQueue_.empty()){
  auto job=setupQueue_.front();setupQueue_.pop_front();
  if(job.game>=0&&!requirements_.checked){setupQueue_.push_front(job);job={-1,false};}
  try{startSetupJob(job);}
  catch(const std::exception& error){
   if(job.game<0){requirements_={};requirements_.checked=true;}
   else{auto& game=setupGames_[size_t(job.game)];game.queued=game.active=false;game.state="failed";game.code="launcher";game.message=error.what();game.hint.clear();game.detail.clear();}
   status_=L"Setup refused: "+nativeError(error.what());
  }
 }
}
void Backend::startSetupJob(SetupJob job){
 const auto script=quote((root_/L"tools/setup/setup.py").wstring());
 if(!ordinary(root_/L"tools/setup/setup.py"))throw std::runtime_error("tools/setup/setup.py missing");
 setupCancelled_=false;
 if(job.game<0){
  const auto found=interpreters();
  if(interpreterTry_>=found.size()){requirements_={};requirements_.checked=true;status_=L"Python 3 was not found on PATH.";return;}
  setupProcess_=spawnSetup(root_,found[interpreterTry_]+L" -I "+script+L" --requirements",root_/L"logs/setup-requirements.json",
                           root_/L"logs/setup-requirements.log",setupJob_);
  activeSetup_=job;return;
 }
 auto& game=setupGames_[size_t(job.game)];game.queued=false;
 if(interpreter_.empty())throw std::runtime_error("Python 3 was not found on PATH");
 unlinked(game.folder);if(!fs::is_directory(game.folder))throw std::runtime_error("ROM folder not found");
 const auto progress=setupProgress(root_,size_t(job.game));unlinked(progress);DeleteFileW(progress.c_str());
 std::wstring command=interpreter_+L" -I "+script+L" --game "+compatibleGames[size_t(job.game)].setupId+L" --roms "+
                      quote(game.folder.wstring())+L" --progress "+quote(progress.wstring())+(job.verify?L" --verify-only":L"");
 setupProcess_=spawnSetup(root_,command,setupLog(job.game),setupLog(job.game),setupJob_);
 activeSetup_=job;nextSetupRead_=0;
 game.active=true;game.state="running";game.step="verify";game.message.clear();game.code.clear();game.hint.clear();game.detail.clear();game.unitsDone=game.unitsTotal=0;game.elapsed=0;
 status_=job.verify?L"Checking the ROM files...":L"Setup running: checking the ROM files and generating the game on this machine...";
}
void Backend::finishSetup(DWORD code){
 const auto job=activeSetup_;activeSetup_={-1,false};
 if(job.game<0){
  tinyjson::Value value;
  if(!readObject(root_/L"logs/setup-requirements.json",value)){
   if(++interpreterTry_<interpreters().size())setupQueue_.push_front({-1,false});
   else{requirements_={};requirements_.checked=true;status_=L"Python 3 was not found on PATH.";}
   return;
  }
  interpreter_=interpreters().at(interpreterTry_);
  requirements_.checked=true;requirements_.python=text(value,"python");
  if(const auto* windows=member(value,"windows_64bit",tinyjson::Value::Bool))requirements_.windows64=windows->boolean;
  if(const auto* supported=member(value,"python_supported",tinyjson::Value::Bool))requirements_.pythonSupported=supported->boolean;
  if(const auto* ok=member(value,"ok",tinyjson::Value::Bool))requirements_.ok=ok->boolean;
  if(const auto* compiler=member(value,"compiler",tinyjson::Value::Object)){
   requirements_.compiler=text(*compiler,"version");requirements_.validated=text(*compiler,"validated");}
  return;
 }
 auto& game=setupGames_[size_t(job.game)];game.active=false;
 if(setupCancelled_){game.state="cancelled";game.message="Cancelled";}
 else if(code==0){game.state="done";if(game.mode=="setup")game.verified=true;}
 else if(game.state!="failed"||game.code.empty()){
  game.state="failed";game.code="internal";game.detail=(game.message.empty()?"":game.message+"; ")+"setup.py exit code "+std::to_string(code);
  game.message="The setup stopped unexpectedly.";game.hint.clear();}
 refreshSetupEstimate(job.game);
 const std::wstring name=compatibleGames[size_t(job.game)].title;
 status_=game.state=="done"?(game.mode=="verify"?name+L": ROM files verified.":name+L": ready to play."):
         game.state=="cancelled"?name+L": setup cancelled.":name+L": "+failureText(job.game);
}
void Backend::readSetupProgress(int index){
 tinyjson::Value value;if(!readObject(setupProgress(root_,size_t(index)),value))return;
 auto& game=setupGames_[size_t(index)];
 game.mode=text(value,"mode");game.step=text(value,"step");game.state=text(value,"state");game.message=text(value,"message");
 game.code=text(value,"code");game.hint=text(value,"hint");game.detail=text(value,"detail");
 game.elapsed=number(value,"elapsed");game.unitsDone=game.unitsTotal=0;
 if(const auto* units=member(value,"units",tinyjson::Value::Object)){game.unitsDone=int(number(*units,"done"));game.unitsTotal=int(number(*units,"total"));}
 if(const auto* report=member(value,"report",tinyjson::Value::Object)){
  game.files.clear();game.checks.clear();game.program=text(*report,"program");
  if(const auto* files=member(*report,"files",tinyjson::Value::Array))
   for(const auto& file:files->items)if(file.kind==tinyjson::Value::Object)game.files.emplace_back(text(file,"name"),text(file,"status"));
  if(const auto* checks=member(*report,"checks",tinyjson::Value::Array))
   for(const auto& check:checks->items)if(check.kind==tinyjson::Value::String)game.checks.push_back(check.text);
  const auto* ok=member(*report,"ok",tinyjson::Value::Bool);game.verified=ok&&ok->boolean;
 }
}
bool Backend::selectGame(int index,bool adjustments){
 if(running()||index<0||index>2)return false;
 if(index==gameIndex_)return true;
 gameIndex_=index;return load(adjustments);
}
fs::path Backend::operatorPath(bool verifyPayload)const{
 if(gameIndex_==0)return root_/stage/L"profile/cmos.bin";
 if(gameIndex_==1&&publicNativePresent(root_))return nativeSpec(root_,publicNativeManifest(root_),verifyPayload).profile/L"cmos.bin";
 return {};
}
fs::path Backend::operatorRoms()const{
 if(gameIndex_==0)return root_/L"roms/mk";
 return nativeSpec(root_,publicNativeManifest(root_),false).roms;
}
bool Backend::load(bool adjustments){const bool native=gameIndex_==1&&publicNativePresent(root_);selectedNative_=native;
 auto profile=root_/(native?mk2NativeStage:gameIndex_?coreStage(gameIndex_):stage)/L"profile/launcher.ini";
 const bool valid=readSettings(profile,std::array<const char*,3>{"mk1","mk2","mk3"}[size_t(gameIndex_)],native?0:gameIndex_);refreshAdjustments(adjustments);
 if(gameIndex_==2&&!running())mk3Engine_=config.mk3Engine;
 return valid;}
LauncherPreferences Backend::loadLauncherPreferences(const Config& legacy)const{
 LauncherPreferences defaults;
 defaults.fps=legacy.fps;defaults.vsync=legacy.vsync;
 defaults.reducedMotion=legacy.reducedMotion;defaults.fullscreen=legacy.fullscreen;
 for(bool audioOnly:{false,true}){
  try{
   const auto path=root_/(audioOnly?menuAudioFile:launcherPreferencesFile);unlinked(path);
   if(!fs::exists(path))continue;
   std::istringstream data(smallFile(path,256));
   auto result=defaults;
   result.musicVolume=preference(data,"Volume=",0,100);
   result.musicMuted=preference(data,"Muted=",0,1)!=0;
   if(!audioOnly){
    result.fps=preference(data,"FPS=",30,240);
    if(!validFps(result.fps))throw std::runtime_error("invalid launcher FPS");
    result.vsync=preference(data,"VSync=",0,1)!=0;
    result.reducedMotion=preference(data,"ReducedMotion=",0,1)!=0;
    result.fullscreen=preference(data,"Fullscreen=",0,1)!=0;
   }
   std::string extra;if(data>>extra)throw std::runtime_error("unexpected launcher preference");
   return result;
  }catch(const std::exception& error){std::clog<<"launcher preferences load failed: "<<error.what()<<'\n';}
 }
 return defaults;
}
mk5controls::Config Backend::loadControls()const{
 mk5controls::Config controls;
 try{
  const auto path=root_/controlsFile;unlinked(path);
  if(fs::exists(path)){
   auto text=smallFile(path,4096);while(!text.empty()&&(text.back()=='\n'||text.back()=='\r'))text.pop_back();
   if(text.rfind("Controls=",0)!=0||!mk5controls::parse(text.substr(9),controls))throw std::runtime_error("invalid controls");
  }
 }catch(const std::exception& error){std::clog<<"controls load failed: "<<error.what()<<'\n';controls={};}
 return controls;
}
bool Backend::saveControls(const mk5controls::Config& controls)const{
 try{writePreferences(root_/controlsFile,"Controls="+mk5controls::format(controls)+"\r\n");return true;}
 catch(const std::exception& error){std::clog<<"controls save failed: "<<error.what()<<'\n';return false;}
}
bool Backend::saveLauncherPreferences(const LauncherPreferences& preferences)const{
 if(!validFps(preferences.fps)||preferences.musicVolume<0||preferences.musicVolume>100)return false;
 try{
  std::ostringstream text;
  text<<"Volume="<<preferences.musicVolume<<"\r\nMuted="<<int(preferences.musicMuted)
      <<"\r\nFPS="<<preferences.fps<<"\r\nVSync="<<int(preferences.vsync)
      <<"\r\nReducedMotion="<<int(preferences.reducedMotion)<<"\r\nFullscreen="<<int(preferences.fullscreen)<<"\r\n";
  writePreferences(root_/launcherPreferencesFile,text.str());
  return true;
 }catch(const std::exception& error){std::clog<<"launcher preferences save failed: "<<error.what()<<'\n';return false;}
}
bool Backend::readSettings(const fs::path& path,const std::string& gameSection,int capabilities,bool mk3Legacy){
 config=factory(capabilities);validate(config,capabilities);
 try{
  unlinked(path);
  if(!fs::exists(path)){status_=L"Default preferences loaded.";return true;}
  auto text=smallFile(path,8192);if(text.find('\0')!=std::string::npos)throw std::runtime_error("binary config");
  if(text.compare(0,3,"\xef\xbb\xbf")==0)text.erase(0,3);
  auto parsed=factory(capabilities);VideoConfig legacy;
  std::array<std::map<std::string,std::string>,2> overrides;
  // A bad MK3 value falls back to its default without blocking the MK3 card.
  std::set<std::string> seen;std::string section,line;bool valid=true,mk3Defaults=false;
  std::map<std::string,std::string> mk3Preview; // [Video.Preview.Remastered], the MK3 Remastered profile until F3 (14/11/2026)
  std::istringstream lines(text);
  while(std::getline(lines,line)){
   line=trim(line);if(line.empty()||line[0]==';'||line[0]=='#')continue;
   if(line[0]=='['){if(line.back()!=']')throw std::runtime_error("section syntax");section=lower(trim(line.substr(1,line.size()-2)));
    mk3Legacy=mk3Legacy||(gameSection=="mk3"&&section=="video.preview.remastered");continue;}
   const auto eq=line.find('=');if(eq==std::string::npos||section.empty())throw std::runtime_error("key syntax");
   const auto key=lower(trim(line.substr(0,eq))),value=trim(line.substr(eq+1));
   if(!seen.insert(section+"/"+key).second)throw std::runtime_error("duplicate key");
   int* target=nullptr;bool* boolean=nullptr;int minimum=0,maximum=0,fallback=0;
   if(section==gameSection&&key=="gamevolume"){target=&parsed.gameVolume;maximum=100;fallback=100;}
   else if(section==gameSection&&key=="blood")boolean=&parsed.blood;
   else if(section==gameSection&&key=="violence")boolean=&parsed.violence;
   else if(section==gameSection&&key=="cmoscoinage")boolean=&parsed.cmosCoinage;
   else if(section==gameSection&&key=="audiolatency"){target=&parsed.audioLatency;minimum=1;maximum=2;fallback=1;}
   else if(section==gameSection&&key=="remastered")boolean=&parsed.remastered;
   else if(section==gameSection&&gameSection=="mk3"&&key=="engine"){
    int n=0;if(!integer(value,n)||n<0||n>2){n=1;mk3Defaults=true;}
    parsed.mk3Engine=n;continue;
   }
   else if(section=="launcher"&&key=="fps"){target=&parsed.fps;minimum=30;maximum=240;fallback=60;}
   else if(section=="launcher"&&key=="uivolume"){target=&parsed.uiVolume;maximum=100;fallback=0;}
   else if(section=="launcher"&&key=="vsync")boolean=&parsed.vsync;
   else if(section=="launcher"&&key=="reducedmotion")boolean=&parsed.reducedMotion;
   else if(section=="launcher"&&key=="fullscreen")boolean=&parsed.fullscreen;
   else if(section=="launcher"&&key=="videoeffects")boolean=&parsed.videoEffects;
   else{
    if(section==gameSection&&key!="fullscreen"&&!readVideo(legacy,key,value))valid=false;
    if(section=="video.original"||section=="video.remastered")overrides[section=="video.remastered"].emplace(key,value);
    if(gameSection=="mk3"&&section=="video.preview.remastered")mk3Preview.emplace(key,value);
    continue;
   }
   int n=0;bool ok=integer(value,n);
   if(boolean){ok=ok&&(n==0||n==1);if(ok)*boolean=n!=0;}
   else{ok=ok&&n>=minimum&&n<=maximum;if(key=="fps")ok=ok&&validFps(n);*target=ok?n:fallback;}
   if(!ok)valid=false;
  }
  // Only native MK2 used the launcher fullscreen preference for its game window.
  if(gameSection=="mk2"&&capabilities==0)legacy.fullscreen=parsed.fullscreen;
  const bool mk3=gameSection=="mk3";
  if(!mk3)parsed.videos.fill(legacy);
  else if(mk3Legacy){
   // Profile written before F3: the MK3 applied only [Video.Preview.Remastered] and the fullscreen key of its edition's section.
   for(auto& keys:overrides){const auto fullscreen=keys.find("fullscreen");std::map<std::string,std::string> kept;if(fullscreen!=keys.end())kept.insert(*fullscreen);keys.swap(kept);}
   for(const auto& [key,value]:mk3Preview)overrides[1][key]=value;
  }
  const auto defaults=factory(capabilities).videos;
  for(size_t i=0;i<overrides.size();++i)for(const auto& [key,value]:overrides[i])
   if(!readVideo(parsed.videos[i],key,value,defaults[i])){if(mk3)mk3Defaults=true;else valid=false;}
  validate(parsed,capabilities);config=parsed;
  status_=!valid?L"Invalid values were replaced by the defaults.":mk3Defaults?L"Preferences loaded; invalid MK3 values were replaced by the defaults.":L"Preferences loaded.";return valid;
 }catch(...){config=factory(capabilities);validate(config,capabilities);status_=L"The settings file is invalid or unreadable; using safe defaults.";return false;}
}
bool Backend::save(){
 if(running()){status_=L"Close the game before saving preferences.";return false;}
 try{
  const bool native=gameIndex_==1&&publicNativePresent(root_);
  if(native!=selectedNative_)throw std::runtime_error("selected installation changed; reload Settings");
  validate(config,native?0:gameIndex_);const auto path=root_/(native?mk2NativeStage:gameIndex_?coreStage(gameIndex_):stage)/L"profile/launcher.ini";
  std::ostringstream text;text<<"[Launcher]\r\nFPS="<<config.fps<<"\r\nVSync="<<int(config.vsync)<<"\r\nReducedMotion="<<int(config.reducedMotion)<<"\r\nUIVolume="<<config.uiVolume<<"\r\nFullscreen="<<int(config.fullscreen)<<"\r\nVideoEffects="<<int(config.videoEffects)<<"\r\n["<<std::array<const char*,3>{"MK1","MK2","MK3"}[size_t(gameIndex_)]<<"]\r\n";
  text<<"Remastered="<<int(config.remastered)<<"\r\n";
  if(gameIndex_==2)text<<"Engine="<<config.mk3Engine<<"\r\n";
  text<<"GameVolume="<<config.gameVolume<<"\r\n";
  text<<"Blood="<<int(config.blood)<<"\r\nViolence="<<int(config.violence)<<"\r\nCmosCoinage="<<int(config.cmosCoinage)<<"\r\n";
  text<<"AudioLatency="<<config.audioLatency<<"\r\n\r\n[Video.Original]\r\n";writeVideo(text,config.video(false));
  text<<"\r\n[Video.Remastered]\r\n";writeVideo(text,config.video(true));
  writePreferences(path,text.str());
  status_=L"Preferences saved.";return true;
 }catch(const std::exception& error){
  std::clog<<"preferences save failed: "<<error.what()<<'\n';
  status_=L"Could not save the preferences; check access to the profile folder.";return false;
 }
}
void Backend::reset(){config=factory(gameIndex_);save();}
bool Backend::available(int index)const{
 // Only games whose setup finished on this machine are offered.
 if(!setupFinished(index))return false;
 if(index==1||index==2){
  try{
   if(index==1&&publicNativePresent(root_))return nativeSpec(root_,publicNativeManifest(root_)).playable;
   if(index==2){mk3Spec(root_,true);return true;}
   const auto product=root_/mk2Stage/L"product";
   for(const auto* name:{L"session.ps1",L"manifest.json",L"engine-path.txt"})if(!ordinary(product/name))return false;
   const auto core=fs::u8path(trim(smallFile(product/L"engine-path.txt",4096)));if(!core.is_absolute()||!ordinary(core))return false;
   const auto manifest=smallFile(product/L"manifest.json",65536);
   static const std::regex rom("\"path\"\\s*:\\s*\"([^\"]+)\"\\s*,\\s*\"size\"\\s*:\\s*([0-9]+)");
   size_t count=0;
   for(auto it=std::sregex_iterator(manifest.begin(),manifest.end(),rom);it!=std::sregex_iterator();++it){
    const fs::path rel=(*it)[1].str();if(rel.is_absolute())return false;for(const auto& part:rel)if(part==L"..")return false;
    const auto file=product/L"roms"/rel;if(!ordinary(file)||fs::file_size(file)!=std::stoull((*it)[2].str()))return false;++count;
   }
   return count==20;
  }catch(...){return false;}
 }
 if(index!=0)return false;
 try{
  const auto product=root_/stage/L"product";
  for(const auto* name:{L"mk5-native-audio.exe",L"session.ps1",L"manifest.json",L"factory-default-control-input.bin",L"LogStore.cs"})if(!ordinary(product/name))return false;
  const auto manifest=smallFile(product/L"manifest.json",65536);
  static const std::regex rom("\"path\"\\s*:\\s*\"([^\"]+)\"\\s*,\\s*\"size\"\\s*:\\s*([0-9]+)");
  size_t count=0;
  for(auto it=std::sregex_iterator(manifest.begin(),manifest.end(),rom);it!=std::sregex_iterator();++it){
   const fs::path rel=(*it)[1].str();if(rel.is_absolute())return false;for(const auto& part:rel)if(part==L"..")return false;
   const auto file=root_/L"roms"/rel;if(!ordinary(file)||fs::file_size(file)!=std::stoull((*it)[2].str()))return false;++count;
  }
  return count==17;
 }catch(...){return false;}
}
bool Backend::nativeMk2Installed()const{return publicNativePresent(root_);}
bool Backend::setMk3Engine(int engine){
 if(running()||engine<1||engine>2)return false;
 mk3Engine_=engine;return true;
}
bool Backend::chooseMk3Engine(int engine){
 if(gameIndex_!=2||running())return false;
 const int effective=mk3Engine_;
 if(!setMk3Engine(engine)){status_=L"MK3 is unavailable: build it again on the Build page.";return false;}
 const auto before=config;config.mk3Engine=engine;
 if(save())return true;
 config=before;mk3Engine_=effective;return false;
}
std::vector<std::wstring> mk3Arguments(const fs::path& executable,const fs::path& roms,const fs::path& seed,const fs::path& graphics,
                                       const fs::path& profile,bool remastered,const Config& c){
 std::vector<std::wstring> args={executable.wstring(),roms.wstring(),seed.wstring(),graphics.wstring(),L"--window",L"--audio",
  L"--edition",remastered?L"remastered":L"original",L"--cmos-profile",profile.wstring()};
 if(const auto dsw=mk3DswWord(c);dsw!=mk3DefaultDsw)args.insert(args.end(),{L"--dsw-word",std::to_wstring(dsw)});
 return args;
}
bool Backend::launchMk3(){
 forgetFileHashes();
 try{
  const auto spec=mk3Spec(root_,true);const bool remastered=mk3Engine_==2;auto selected=config;validate(selected,2);
  // The edition's video profile (presentation only: the canonical state is the same in both), game volume and audio queue.
  auto environment=nativeEnvironment(selected,selected.video(remastered),{},true,false,customBezel(root_,2),loadControls(),remastered&&selected.video(remastered).widescreen?L"MK3_WIDESCREEN":nullptr);auto expectedPath=spec.executable;expectedPath.make_preferred();
  fs::create_directories(spec.profile);unlinked(spec.profile);
  const auto log=root_/coreLogs(2)/L"launcher-latest.log";unlinked(log);fs::create_directories(log.parent_path());unlinked(log);
  SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};Handle output,input;
  output.h=CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  input.h=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(output.h==INVALID_HANDLE_VALUE||input.h==INVALID_HANDLE_VALUE)throw std::runtime_error("MK3 native log");
  const auto args=mk3Arguments(spec.executable,spec.roms,spec.seed,spec.graphics,spec.profile,remastered,selected);
  std::wstring command;for(const auto& arg:args){if(!command.empty())command+=L' ';command+=quote(arg);}
  STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdInput=input.h;si.hStdOutput=output.h;si.hStdError=output.h;
  PROCESS_INFORMATION pi{};
  if(!CreateProcessW(spec.executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,environment.data(),spec.root.c_str(),&si,&pi))throw std::runtime_error("MK3 native CreateProcess failed: "+std::to_string(GetLastError()));
  CloseHandle(pi.hThread);process_=pi.hProcess;processId_=pi.dwProcessId;gameExecutable_=std::move(expectedPath);
  nativeExperiment_=true;mk3Native_=true;exitCode_=STILL_ACTIVE;stopping_=false;gameWindow_=nullptr;gamePid_=0;nextDiscovery_=0;
  status_=remastered?L"MK3 started: Remastered.":L"MK3 started: Original.";return true;
 }catch(const std::exception& error){exitCode_=ERROR_INVALID_DATA;status_=L"MK3 refused: "+nativeError(error.what())+L". Build MK3 again on the Build page.";return false;}
}
bool Backend::remasteredAvailable()const{
 // D7: the MK3 Remastered edition is video only. MK1 and MK2 always offer Remastered: a missing or unreadable
 // music slot plays the arcade's original music, so the edition needs no files beyond the game itself.
 if(gameIndex_==2)return false;
 if(gameIndex_==1){
  try{return publicNativePresent(root_)&&nativeSpec(root_,publicNativeManifest(root_),false).playable;}catch(...){return false;}
 }
 return true; // MK1: the edition is part of the game package; launch() still requires available(0)
}
bool Backend::launchNativeExperiment(const fs::path& manifest,const fs::path& settings){
 return launchNative(manifest,settings,true);
}
bool Backend::launchNative(const fs::path& manifest,const fs::path& settings,bool diagnostics){
 forgetFileHashes();
 poll();if(process_){status_=L"A game is still running.";return false;}
 const auto collectionConfig=config;gameExecutable_.clear();nativeExperiment_=false;nativeLiveExperiment_=false;nativePlayable_=false;mk3Native_=false;
 try{
  const auto spec=nativeSpec(root_,manifest);auto selected=config;
  if(!diagnostics&&!spec.playable)throw std::runtime_error("Pacote nativo invalido; verifique a instalacao.");
  if(!settings.empty()){if(!readSettings(fs::absolute(settings),"mk2",0))throw std::runtime_error("native Settings invalid");selected=config;config=collectionConfig;}
  validate(selected);
  if(!spec.modernVideo&&needsModernVideo(selected.video()))throw std::runtime_error("Atualize o pacote nativo do MK2 para usar os novos filtros e modos de escala.");
  const auto music=customAudio(root_,L"mk2");
  if(selected.remastered&&!spec.playable)throw std::runtime_error("native experiment supports original audio only");
  auto environment=nativeEnvironment(selected,selected.video(),music,spec.modernVideo,diagnostics||!spec.liveWithoutPixelCheck,customBezel(root_,1),loadControls(),
                                     selected.remastered&&selected.video().widescreen?L"MK2_WIDESCREEN":nullptr);auto expectedPath=spec.executable;expectedPath.make_preferred();
  std::wstring started=std::wstring(compatibleGames[1].name)+L" nativo iniciado.";
  mk2::PrepareCmosProfile(spec.profile,spec.seed);
  const auto log=spec.root/L"logs/native-experiment-latest.log";unlinked(log);fs::create_directories(log.parent_path());unlinked(log);
  SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};Handle output,input;
  output.h=CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  input.h=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(output.h==INVALID_HANDLE_VALUE||input.h==INVALID_HANDLE_VALUE)throw std::runtime_error("native session log");
  std::vector<std::wstring> args={spec.executable.wstring(),spec.roms.wstring(),L"--window",L"--audio",L"--host-controls",L"--through",spec.live?L"cmos-live":L"cmos-host",L"--profile",spec.profile.wstring()};
  const auto dsw=(0xfc7d&~0xc000)|(selected.blood?0x4000:0)|(selected.violence?0x8000:0);
  if(dsw!=0xfc7d)args.insert(args.end(),{L"--dsw-word",std::to_wstring(dsw)});
  std::wstring command;for(const auto& arg:args){if(!command.empty())command+=L' ';command+=quote(arg);}
  STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdInput=input.h;si.hStdOutput=output.h;si.hStdError=output.h;
  PROCESS_INFORMATION pi{};
  if(!CreateProcessW(spec.executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,environment.data(),spec.root.c_str(),&si,&pi))throw std::runtime_error("native prebuilt CreateProcess failed: "+std::to_string(GetLastError()));
  CloseHandle(pi.hThread);process_=pi.hProcess;processId_=pi.dwProcessId;gameExecutable_=std::move(expectedPath);
  nativeExperiment_=true;nativeLiveExperiment_=spec.live;nativePlayable_=spec.playable;exitCode_=STILL_ACTIVE;stopping_=false;gameWindow_=nullptr;gamePid_=0;nextDiscovery_=0;
  status_=std::move(started);return true;
 }catch(const std::exception& error){config=collectionConfig;exitCode_=ERROR_INVALID_DATA;status_=L"Native test refused: "+nativeError(error.what());return false;}
}
bool Backend::launch(unsigned frames){
 forgetFileHashes();
 poll();if(process_){status_=L"A game is still running.";return false;}
 if(setupRunning()){status_=L"Wait for the build to finish before playing.";return false;}
 if(gameIndex_==1&&publicNativePresent(root_)!=selectedNative_){status_=L"The installation changed. Reopen Settings before playing.";return false;}
 gameExecutable_.clear();nativeExperiment_=false;nativeLiveExperiment_=false;nativePlayable_=false;mk3Native_=false;
 if(frames>500000){status_=L"Invalid diagnostic frame limit.";return false;}
 if(gameIndex_==2){
  if(frames){status_=L"Close MK3 from its own window.";return false;}
  return launchMk3();
 }
 if(gameIndex_==1&&publicNativePresent(root_)){
  if(frames){status_=L"Close the game from its own window.";return false;}
  return launchNative(publicNativeManifest(root_),{},false);
 }
 if(!available(gameIndex_)){status_=L"Game unavailable: build it again on the Build page.";exitCode_=ERROR_FILE_NOT_FOUND;return false;}
 if(config.remastered&&!remasteredAvailable()){status_=L"The Remastered edition is not available for this game.";exitCode_=ERROR_FILE_NOT_FOUND;return false;}
 try{
  validate(config,gameIndex_);unlinked(root_);const auto script=root_/(gameIndex_?coreStage(gameIndex_):stage)/L"product/session.ps1";unlinked(script);
  auto expectedPath=(gameIndex_?fs::u8path(trim(smallFile(root_/coreStage(gameIndex_)/L"product/engine-path.txt",4096))):root_/stage/L"product/mk5-native-audio.exe").lexically_normal();
  if(!expectedPath.is_absolute()||!ordinary(expectedPath))throw std::runtime_error("game executable");
  expectedPath.make_preferred();
  wchar_t system[32768];const UINT n=GetSystemDirectoryW(system,32768);if(!n||n>=32768)throw std::runtime_error("system directory");
  const auto powershell=fs::path(system)/L"WindowsPowerShell/v1.0/powershell.exe";
  const auto& v=config.video();const auto& preset=mk5video::presets[size_t(v.scanlines)];
  static const std::regex videoProfiles("\"video_profiles\"\\s*:\\s*1\\s*[,}]");
  const bool modernVideo=gameIndex_==0&&std::regex_search(smallFile(script.parent_path()/L"manifest.json",65536),videoProfiles);
  if(gameIndex_==0&&!modernVideo&&(needsModernVideo(v)||!v.fullscreen)){
   status_=L"Build MK1 again to use this video profile.";exitCode_=ERROR_INVALID_DATA;return false;
  }
  const bool presentation=v.scanlines!=0||v.filter!=0||(v.scalefx&&v.scalefxAmount)||needsModernVideo(v);
  std::vector<std::wstring> args={powershell.wstring(),L"-NoLogo",L"-NoProfile",L"-NonInteractive",L"-ExecutionPolicy",L"Bypass",L"-WindowStyle",L"Hidden",L"-File",script.wstring(),L"-ProjectRoot",root_.wstring(),L"-WindowFrames",std::to_wstring(frames),L"-GameMode",config.remastered?L"remastered":L"original",L"-VisualMode",presentation?L"presentation":L"original",L"-Scanlines",preset.scanlines,L"-Filter",filters[v.filter],L"-CrtPreset",preset.crt,L"-SyncMode",std::array<const wchar_t*,3>{L"vsync",L"vrr",L"immediate"}[v.sync],L"-ColorMode",std::array<const wchar_t*,3>{L"original",L"vivid",L"warm"}[v.color],L"-Brightness",std::to_wstring(v.brightness),L"-Bezel",v.bezel?L"1":L"0",L"-HighRefresh",v.highRefresh?L"1":L"0",L"-AudioLatency",std::array<const wchar_t*,3>{L"low",L"balanced",L"safe"}[config.audioLatency]};
  if(gameIndex_==0)args.insert(args.end(),{L"-ScaleFX",std::to_wstring(v.scalefx?v.scalefxAmount:0),
   L"-GameVolume",std::to_wstring(config.gameVolume),L"-DswWord",std::to_wstring((0xfeff&~0xc000)|(config.blood?0x4000:0)|(config.violence?0x8000:0)),L"-CmosCoinage",config.cmosCoinage?L"1":L"0"});
  if(modernVideo)args.insert(args.end(),{
   L"-ScaleMode",std::to_wstring(v.scaleMode),L"-Cas",std::to_wstring(v.cas),L"-Bilateral",std::to_wstring(v.bilateral),
   L"-Smaa",v.smaa?L"1":L"0",L"-GameFullscreen",v.fullscreen?L"1":L"0"});
  std::wstring command;for(const auto& arg:args){if(!command.empty())command+=L' ';command+=quote(arg);}
  const auto log=root_/(gameIndex_?fs::path(coreLogs(gameIndex_))/L"launcher-latest.log":fs::path(L"logs/temporaria-8/launcher-latest.log"));unlinked(log);fs::create_directories(log.parent_path());unlinked(log);
  SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};Handle output,input;
  output.h=CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
  input.h=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(output.h==INVALID_HANDLE_VALUE||input.h==INVALID_HANDLE_VALUE)throw std::runtime_error("startup log");
  STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESHOWWINDOW|STARTF_USESTDHANDLES;si.wShowWindow=SW_HIDE;si.hStdInput=input.h;si.hStdOutput=output.h;si.hStdError=output.h;
  PROCESS_INFORMATION pi{};
  // MK1: session.ps1 sets the other MK1_* values itself; HDR and the custom frame reach the game only through this environment.
  auto environment=gameIndex_==0?nativeEnvironment(config,v,{},modernVideo,false,customBezel(root_,0),loadControls(),
                                                   config.remastered&&v.widescreen?L"MK1_WIDESCREEN":nullptr):std::vector<wchar_t>{};
  if(!CreateProcessW(powershell.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|(environment.empty()?0:CREATE_UNICODE_ENVIRONMENT),
                     environment.empty()?nullptr:environment.data(),root_.c_str(),&si,&pi))throw std::runtime_error("start session");
  CloseHandle(pi.hThread);process_=pi.hProcess;processId_=pi.dwProcessId;exitCode_=STILL_ACTIVE;stopping_=false;gameWindow_=nullptr;gamePid_=0;nextDiscovery_=0;
  gameExecutable_=std::move(expectedPath);
  status_=L"Starting the game; checking its files...";return true;
 }catch(...){exitCode_=GetLastError();if(!exitCode_)exitCode_=ERROR_GEN_FAILURE;status_=gameIndex_?L"The game could not start; details in "+std::wstring(coreLogs(gameIndex_))+L"/launcher-latest.log.":std::wstring(L"The game could not start; the Logs button opens its log.");return false;}
}
void Backend::inspectGame(){
 if(!process_)return;
 DWORD owner=0;if(gameWindow_&&IsWindow(gameWindow_)&&GetWindowThreadProcessId(gameWindow_,&owner)&&owner==gamePid_){if(stopping_)PostMessageW(gameWindow_,WM_CLOSE,0,0);return;}
 gameWindow_=nullptr;gamePid_=0;
 const auto now=GetTickCount64();if(now<nextDiscovery_)return;nextDiscovery_=now+200;
 Handle snapshot;snapshot.h=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot.h==INVALID_HANDLE_VALUE)return;
 std::map<DWORD,DWORD> parents;PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
 if(Process32FirstW(snapshot.h,&entry)){do{parents[entry.th32ProcessID]=entry.th32ParentProcessID;}while(Process32NextW(snapshot.h,&entry));}
 // Keep shutdown independent of package metadata changed after this launch.
 const auto expected=gameExecutable_.wstring();
 for(const auto& row:parents){
  DWORD pid=row.first,cursor=pid;std::set<DWORD> visited;bool owned=nativeExperiment_&&pid==processId_;
  while(visited.insert(cursor).second){auto it=parents.find(cursor);if(it==parents.end())break;if(it->second==processId_){owned=true;break;}cursor=it->second;}
  if(!owned)continue;
  Handle child;child.h=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!child.h)continue;
  std::vector<wchar_t> path(32768);DWORD size=DWORD(path.size());if(!QueryFullProcessImageNameW(child.h,0,path.data(),&size)||_wcsicmp(expected.c_str(),path.data())!=0)continue;
  struct FindWindow {DWORD pid;HWND result;bool native;};FindWindow find{pid,nullptr,nativeExperiment_};
  EnumWindows([](HWND window,LPARAM opaque)->BOOL{auto* f=reinterpret_cast<FindWindow*>(opaque);DWORD owner=0;GetWindowThreadProcessId(window,&owner);wchar_t name[64]{};if(f->native)GetClassNameW(window,name,64);if(owner==f->pid&&(f->native?std::wstring(name)==L"Mk5TemporalScanout":IsWindowVisible(window))&&GetWindow(window,GW_OWNER)==nullptr){f->result=window;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&find));
  if(find.result){gameWindow_=find.result;gamePid_=pid;if(stopping_)PostMessageW(gameWindow_,WM_CLOSE,0,0);return;}
 }
}
void Backend::poll(){
 if(!process_)return;
 const DWORD wait=WaitForSingleObject(process_,0);
 if(wait==WAIT_OBJECT_0){DWORD code=ERROR_GEN_FAILURE;if(!GetExitCodeProcess(process_,&code))code=ERROR_GEN_FAILURE;CloseHandle(process_);process_=nullptr;processId_=0;gameWindow_=nullptr;gameExecutable_.clear();stopping_=false;exitCode_=code;
  status_=mk3Native_?(code==0?L"MK3 closed.":L"MK3 closed with an error; the Logs button opens its log."):nativePlayable_?(code==0?L"Game closed.":L"The game closed with an error; the Logs button opens its log."):nativeExperiment_?(code==0?(nativeLiveExperiment_?L"Native live test closed.":L"Native test closed."):L"Native test closed with an error; see logs/native-experiment-latest.log."):code==0?L"Game closed.":gameIndex_?L"The game closed with an error; details in "+std::wstring(coreLogs(gameIndex_))+L"/launcher-latest.log.":std::wstring(L"The game closed with an error; the Logs button opens its log.");return;}
 inspectGame();if(stopping_)status_=L"Closing the game and saving its records and CMOS...";else if(gameWindow_)status_=mk3Native_?(mk3Engine_==2?L"MK3 (Remastered) running.":L"MK3 (Original) running."):nativePlayable_?std::wstring(compatibleGames[1].name)+L" running.":nativeExperiment_?(nativeLiveExperiment_?L"Native live test running.":L"Native test running."):std::wstring(compatibleGames[size_t(gameIndex_)].name)+L" running.";
}
bool Backend::running()const{return process_!=nullptr;}
void Backend::requestStop(){if(!process_)return;stopping_=true;inspectGame();status_=L"Closing the game and saving its records and CMOS...";}
const std::wstring& Backend::status()const{return status_;}
const fs::path& Backend::root()const{return root_;}
DWORD Backend::exitCode()const{return exitCode_;}
HWND Backend::gameWindow()const{return gameWindow_;}
}
