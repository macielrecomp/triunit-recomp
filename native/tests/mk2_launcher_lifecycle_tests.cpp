#include "launcher_backend.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace fs=std::filesystem;
namespace {
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void write(const fs::path& path,const std::string& text){fs::create_directories(path.parent_path());std::ofstream out(path);out<<text;check(bool(out),"fixture write");}
LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wp,LPARAM lp){if(message==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProcW(window,message,wp,lp);}
int gameWindow(){
 WNDCLASSW cls{};cls.lpfnWndProc=procedure;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"MkLauncherLifecycleFixture";check(RegisterClassW(&cls)!=0,"window class");
 const auto window=CreateWindowW(cls.lpszClassName,L"Lifecycle fixture",WS_OVERLAPPEDWINDOW,-32000,-32000,100,100,nullptr,nullptr,cls.hInstance,nullptr);check(window!=nullptr,"window create");
 ShowWindow(window,SW_SHOW);ShowWindow(window,SW_SHOWNOACTIVATE);
 MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}return 0;
}
void package(const fs::path& root,const fs::path& stage,const fs::path& self,bool mk2){
 const auto product=root/stage/L"product";fs::create_directories(product);
 const auto executable=product/(mk2?L"engine.exe":L"mk5-native-audio.exe");fs::copy_file(self,executable);
 write(product/L"session.ps1","param([string]$ProjectRoot,[Parameter(ValueFromRemainingArguments=$true)][object[]]$Rest)\n"
  "$ErrorActionPreference='Stop'\n$p=Start-Process -FilePath (Join-Path $PSScriptRoot '"+executable.filename().string()+"') -ArgumentList '--game-window' -WorkingDirectory $ProjectRoot -WindowStyle Hidden -PassThru -Wait\nexit $p.ExitCode\n");
 if(mk2)write(product/L"engine-path.txt",executable.u8string());
 else{write(product/L"factory-default-control-input.bin","");write(product/L"LogStore.cs","");}
 std::string manifest="[";for(unsigned i=0;i<(mk2?20u:17u);++i){
  const auto name=std::to_string(i)+".rom";write((mk2?product/L"roms":root/L"roms")/name,"x");
  if(i)manifest+=",";
  manifest+="{\"path\":\""+name+"\",\"size\":1}";
 }write(product/L"manifest.json",manifest+"]");
}
void stop(mklauncher::Backend& backend){
 backend.requestStop();const auto deadline=GetTickCount64()+15000;
 while(backend.running()&&GetTickCount64()<deadline){backend.poll();if(backend.running())Sleep(20);}
 check(!backend.running(),"owned game/session did not close");check(backend.exitCode()==0,"owned session failed");
}
}
int wmain(int argc,wchar_t** argv){
 try{
  if(argc==2&&std::wstring(argv[1])==L"--game-window")return gameWindow();
  check(argc==2,"isolated root required");const auto root=fs::absolute(argv[1]);
  std::vector<wchar_t> self(32768);check(GetModuleFileNameW(nullptr,self.data(),DWORD(self.size()))!=0,"self path");
  const fs::path mk2=L"build/native/mk2-arcade-v1",mk1=L"build/native/manual-temporary-8-20261025";
  package(root,mk2,self.data(),true);package(root,mk1,self.data(),false);
  // The launcher offers a game only after its local setup finished (generated/<game>/<sha>/setup.json).
  for(const char* game:{"mk1","mk2"}){
   const auto folder=root/L"generated"/fs::u8path(game)/std::wstring(64,L'd');fs::create_directories(folder);write(folder/L"game.exe","x");
   write(folder/L"setup.json",std::string("{\"setup_version\":\"13\",\"compile\":{\"executable\":\"generated/")+game+"/"+std::string(64,'d')+"/game.exe\",\"sha256\":\""+std::string(64,'e')+"\"}}");
  }
  mklauncher::Backend backend(root);check(backend.selectGame(1),"select MK2");check(backend.launch(),"launch MK2");
  const auto identity=root/mk2/L"product/engine-path.txt";fs::remove(identity);
  backend.poll();stop(backend);
  check(backend.selectGame(0)&&backend.launch(),"switch to MK1");stop(backend);
  check(backend.selectGame(1),"switch back to MK2");write(identity,"missing.exe");
  check(!backend.launch()&&!backend.running(),"invalid engine must reject before process creation");
  write(identity,(root/mk2/L"product/engine.exe").u8string());check(backend.launch(),"retry MK2 after failed launch");stop(backend);
  std::cout<<"PASS: MK2 deleted metadata shutdown, MK1 switch, failed launch, MK2 retry\n";return 0;
 }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
