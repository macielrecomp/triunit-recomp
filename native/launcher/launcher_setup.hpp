#pragma once
// Setup records of tools/setup/setup.py: generated/<game>/<program_sha256>/setup.json. The launcher offers a game only
// when its setup finished on this machine, and identifies the game executable by the SHA-256 that record holds; no
// identity of generated code is committed to the repository.
#include "../audio/tiny_json.hpp"
#include <filesystem>
#include <string>
#include <vector>
namespace mklauncher {
// tools/setup/setup.py SETUP_VERSION: a record of another version was built by an older release and must be built again.
inline constexpr const char* kSetupVersion="14";
struct SetupRecord {
 bool finished=false;
 bool outdated=false; // built by another setup version: not offered until it is built again
 std::filesystem::path directory,executable;
 std::string program,executableSha256;
 double seconds=0; // generate + compile + seed seconds of that setup (the launcher's time estimate)
};
inline bool hex64(const std::string& v){return v.size()==64&&v.find_first_not_of("0123456789abcdef")==std::string::npos;}
// The most recent finished record of a game (by file time), or finished=false.
inline SetupRecord readSetup(const std::filesystem::path& root,const wchar_t* game){
 namespace fs=std::filesystem;SetupRecord best;fs::file_time_type newest{};
 std::error_code error;const auto base=root/L"generated"/game;
 if(tinyjson::Linked(base)||!fs::is_directory(base,error))return best;
 for(const auto& entry:fs::directory_iterator(base,error)){
  const auto name=entry.path().filename().string();
  if(!hex64(name)||!entry.is_directory(error))continue;
  const auto file=entry.path()/L"setup.json";const auto text=tinyjson::Read(file,1<<20);if(!text)continue;
  tinyjson::Parser parser(*text);tinyjson::Value value;
  if(!parser.Parse(value)||!parser.End()||value.kind!=tinyjson::Value::Object)continue;
  const auto compile=value.fields.find("compile");
  if(compile==value.fields.end()||compile->second.kind!=tinyjson::Value::Object)continue;
  const auto exe=compile->second.fields.find("executable"),sha=compile->second.fields.find("sha256");
  if(exe==compile->second.fields.end()||sha==compile->second.fields.end()||exe->second.kind!=tinyjson::Value::String||
     sha->second.kind!=tinyjson::Value::String||!hex64(sha->second.text))continue;
  auto path=fs::u8path(exe->second.text);if(path.is_absolute()||path.empty())continue;
  bool traversal=false;for(const auto& part:path)traversal|=part==L".."||part==L".";if(traversal)continue;
  path=(root/path).lexically_normal();
  if(path.parent_path()!=entry.path().lexically_normal()||!fs::is_regular_file(path,error)||tinyjson::Linked(path))continue;
  const auto time=fs::last_write_time(file,error);if(error)continue;
  const auto number=[](const tinyjson::Value& object,const char* key){
   const auto f=object.fields.find(key);return f!=object.fields.end()&&f->second.kind==tinyjson::Value::Number?f->second.number:0.0;};
  double seconds=number(value,"generate_seconds")+number(compile->second,"seconds");
  const auto seed=value.fields.find("seed");if(seed!=value.fields.end()&&seed->second.kind==tinyjson::Value::Object)seconds+=number(seed->second,"seconds");
  const auto version=value.fields.find("setup_version");
  if(version==value.fields.end()||version->second.kind!=tinyjson::Value::String||version->second.text!=kSetupVersion){
   if(!best.finished)best.outdated=true;
   continue;
  }
  if(!best.finished||time>newest){best={true,false,entry.path(),path,name,sha->second.text,seconds};newest=time;}
 }
 return best;
}
}
