#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#include "launcher_renderer.hpp"
#include "launcher_backend.hpp"
#include "launcher_music.hpp"
#include "launcher_legal.hpp"
#include "launcher_setup.hpp"
#include <shobjidl.h>
#include "../include/mk5_video_presets.hpp"
#include <windowsx.h>
#include <shellapi.h>
#include <mmsystem.h>
#include "../include/mk5_gamepad.hpp"
#include "../include/mk5_hid.hpp"
#include "../include/mk5_timing.hpp"
#include <array>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <future>
#include <fstream>
#include <vector>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <cstdio>

namespace fs=std::filesystem;
using namespace mklauncher;
namespace {
constexpr uint64_t ticksPerSecond=10000000;
uint64_t frameTicks(){
    using Tick=std::chrono::duration<uint64_t,std::ratio<1,ticksPerSecond>>;
    return std::chrono::duration_cast<Tick>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool frameDue(mk5timing::Cadence& cadence,uint64_t now,unsigned fps){
    const auto interval=(ticksPerSecond+fps-1)/fps;
    if(cadence.interval!=interval)cadence={0,interval};
    if(!cadence.due(now))return false;
    cadence.presented(now);
    return true;
}
struct FrameTimer {
    HANDLE handle=CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_ALL_ACCESS);
    FrameTimer(){
        if(!handle)handle=CreateWaitableTimerW(nullptr,FALSE,nullptr);
        if(!handle)throw std::runtime_error("launcher frame timer");
    }
    ~FrameTimer(){CloseHandle(handle);}
    void arm(uint64_t ticks)const{
        LARGE_INTEGER due{};
        due.QuadPart=-static_cast<LONGLONG>(ticks);
        if(!SetWaitableTimer(handle,&due,0,nullptr,nullptr,FALSE))throw std::runtime_error("launcher frame timer deadline");
    }
};
constexpr Color red{.88f,.10f,.11f,1},redHot{1,.24f,.20f,1},gold{1,.70f,.28f,1},white{.93f,.93f,.95f,1},muted{.58f,.58f,.63f,1},blue{.10f,.43f,1,1};
constexpr Color ink{.028f,.027f,.032f,1},panelFill{.058f,.056f,.064f,.97f},panelEdge{.20f,.19f,.22f,1},fieldFill{.034f,.033f,.039f,1};
const std::array<std::wstring,3> games={compatibleGames[0].name,compatibleGames[1].name,compatibleGames[2].name};
constexpr std::array<Rect,3> positions={Rect{250,155,330,680},Rect{705,155,330,680},Rect{1160,155,330,680}};
constexpr std::array<float,3> wideCabinetShift{48,142,236};
// English labels of mk5video::presets (same order; the presets themselves are shared with the game renderer).
constexpr std::array<const wchar_t*,10> crtLabels{L"OFF",L"LIGHT / 25%",L"MEDIUM / 50%",L"STRONG / 75%",L"BVM / 1080p",L"BVM / 4K",
                                                 L"BVM / SOFT",L"CRT / ENERGY",L"PVM / SOFT",L"CRT / PHOSPHOR"};
static_assert(crtLabels.size()==mk5video::presets.size(),"one English label per CRT preset");
// Pages of the left navigation; Settings is the existing settings panel (App::settings).
enum Page {pageOverview,pageRoms,pageBuild,pagePlay,pageLegal};
constexpr std::array<int,6> navIds{40,41,42,10,11,43};
constexpr std::array<const wchar_t*,6> navLabels{L"Overview",L"ROM Setup",L"Build",L"Play",L"Settings",L"Legal"};
constexpr std::array<int,6> navIcons{0,2,1,3,5,4};
constexpr std::array<const wchar_t*,6> stepLabels{L"Select ROM Folder",L"Verify Files",L"Generate C++",L"Validate Digests",L"Compile Game",L"Ready"};
constexpr float contentLeft=226,headerBottom=124,footerTop=972;
std::wstring widen(const std::string& text){if(text.empty())return {};int n=MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),nullptr,0);std::wstring w(size_t(n),0);MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),w.data(),n);return w;}
void rejectLinks(fs::path path){while(!path.empty()){DWORD attr=GetFileAttributesW(path.c_str());if(attr!=INVALID_FILE_ATTRIBUTES&&(attr&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("launcher path uses a link/junction");auto parent=path.parent_path();if(parent==path)break;path=parent;}}
void eventLog(const fs::path& root,const std::string& value){
    auto path=root/L"logs/temporaria-8/ui-latest.log";rejectLinks(path);fs::create_directories(path.parent_path());
    if(fs::exists(path)&&fs::file_size(path)>65536){std::ofstream clear(path,std::ios::trunc);}
    std::ofstream out(path,std::ios::app);out<<GetTickCount64()<<" "<<value<<"\n";
}
std::wstring quote(const std::wstring& s){std::wstring o=L"\"";unsigned slashes=0;for(wchar_t c:s){if(c==L'\\'){++slashes;continue;}if(c==L'\"'){o.append(slashes*2+1,L'\\');o+=c;}else{o.append(slashes,L'\\');o+=c;}slashes=0;}o.append(slashes*2,L'\\');return o+L'\"';}
std::wstring clockText(double seconds){const int t=int(seconds+.5);return std::to_wstring(t/60)+(t%60<10?L":0":L":")+std::to_wstring(t%60);}
// Setup step (index into stepLabels) of a setup.py progress step.
int stepOf(const std::string& s){return s=="decoder"||s=="generate"?2:s=="accept"?3:s=="compile"||s=="seed"||s=="install"?4:1;}
std::wstring checkLabel(const std::string& key){return key=="size"?L"Size":key=="crc32"?L"CRC32":key=="sha1"?L"SHA-1":key=="sha256"?L"SHA-256":widen(key);}
// Short metallic coin/chute sound, generated once and kept alive for async WinMM.
void coinSound(){
    static const auto wav=[](){
        constexpr unsigned rate=22050,count=rate*32/100;std::vector<unsigned char> bytes(44+count*2,0);
        auto word=[&](unsigned at,unsigned n,unsigned size){for(unsigned i=0;i<size;++i)bytes[at+i]=static_cast<unsigned char>(n>>(8*i));};
        std::memcpy(bytes.data(),"RIFF",4);word(4,unsigned(bytes.size()-8),4);std::memcpy(bytes.data()+8,"WAVEfmt ",8);
        word(16,16,4);word(20,1,2);word(22,1,2);word(24,rate,4);word(28,rate*2,4);word(32,2,2);word(34,16,2);std::memcpy(bytes.data()+36,"data",4);word(40,count*2,4);
        unsigned noise=17;for(unsigned i=0;i<count;++i){double t=double(i)/rate,value=0;
            for(double strike:{0.,.047,.129})if(t>=strike){double a=t-strike;noise=noise*1664525u+1013904223u;
                value+=.12*(std::sin(6.283185307*2180*a)+.7*std::sin(6.283185307*3517*a)+.4*std::sin(6.283185307*5273*a))*std::exp(-a*34);
                value+=(double(noise>>16)/32768-1)*.22*std::exp(-a*260);}
            word(44+i*2,unsigned(int(std::clamp(value,-1.,1.)*32767)),2);
        }return bytes;
    }();
    PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()),nullptr,SND_MEMORY|SND_ASYNC|SND_NODEFAULT);
}
struct Control{int id;Rect box;bool enabled;};
// A clickable element of the setup pages: geometry, label and state from one place, for drawing, hit-testing and focus.
enum ButtonStyle {styleNormal,stylePrimary,styleField,styleHidden,styleCheck};
struct Button{int id;Rect box;std::wstring label;bool enabled;ButtonStyle style;};
enum VideoControl {videoProfile=274,videoPage,videoScale,videoCas,videoSmaa,videoBilateral,videoFullscreen};
enum MenuAudioControl {menuVolume=281,menuMute};
enum DisplayControl {displayPage=290,hdrMode,hdrPaper,hdrPeak,hdrHighlights};
constexpr int launcherTab=3,controlsTab=6,tabCount=7;
// CONTROLS tab cells: 300 + player*50 + column*25 + action (column 0 keyboard, 1 controller); 390 SOCD.
constexpr int controlCell=300,socdControl=390,runAheadControl=391,trainingControl=392;
constexpr Rect menuVolumeBox{969,700,166,54},menuMuteBox{1145,700,84,54};
struct App {
    HWND window=nullptr;Backend backend;std::unique_ptr<Renderer> graphics;
    std::unique_ptr<MenuMusic> menuMusic;
    std::vector<Control> controls;
    int selected=0,chosen=-1,hover=-1,pressed=-1,focus=0,category=0,page=pageOverview,setupFocus=0,reportGame=0;
    bool settings=false,details=false,closing=false,active=true,fullscreen=false,restoring=false,padReady=false;
    bool requirementsAsked=false,setupBusy=false;std::wstring seenStatus;
    std::array<SetupRecord,3> records{};std::array<std::vector<std::wstring>,3> manifestChecks{};
    mk5pad::Devices pads;ULONGLONG nextPadRepeat=0;WORD padDirections=0;
    Config draft{};GameAdjustments draftAdjustments{};
    LauncherPreferences launcher{},draftLauncher{};
    bool editingGame=false,resetScoresPending=false,videoRemastered=false,displayHdr=false;
    mk5controls::Config draftControls;int capture=-1;uint32_t captureHeld=0;
    RECT placement{};std::wstring notice;
    std::array<float,3> glows{};float mx=-1,my=-1;const uint64_t started=frameTicks();bool receivedInput=false;
    WORD gamepadButtons=0;ULONGLONG nextPad=0,nextPoll=0,availabilityDue=0;
    bool mk1Available=false,mk2Available=false,mk3Available=false,musicAvailable=false,mk1Remastered=false,mk2Remastered=false;
    std::future<std::array<bool,3>> availability;
    int drawnFocus=-1;
    explicit App(fs::path root):backend(root){
        backend.load(false);draft=backend.config;launcher=draftLauncher=backend.loadLauncherPreferences(backend.config);notice=backend.status();mk1Available=backend.setupFinished(0);mk2Available=backend.setupFinished(1);mk3Available=backend.setupFinished(2);musicAvailable=backend.remasteredAvailable();mk1Remastered=backend.config.remastered;Backend mk2(root);mk2.selectGame(1,false);mk2Remastered=mk2.config.remastered;Backend mk3(root);mk3.selectGame(2,false);backend.setMk3Engine(mk3.mk3Engine());availabilityDue=0;   // the hashed check runs right after the first frame (poll)
        refreshRecords();readManifests();seenStatus=backend.status();
        page=std::any_of(records.begin(),records.end(),[](const SetupRecord& r){return r.finished;})?pagePlay:
             std::any_of(records.begin(),records.end(),[](const SetupRecord& r){return r.outdated;})?pageBuild:pageOverview;
    }
    ~App() noexcept {
        menuMusic.reset();
        PlaySoundW(nullptr,nullptr,0);
        // Also runs during an exception: do not release the singleton while a
        // game/logger that we started is still writing its profile.
        if(backend.running()){
            try{eventLog(backend.root(),"exception-or-scope-shutdown");}catch(...){}
            while(backend.running()){
                try{backend.poll();}catch(...){/* Reap an exited session independently of a discovery failure. */}
                if(backend.running()){try{backend.requestStop();}catch(...){/* Retry graceful discovery on the next iteration. */}}
                if(backend.running())Sleep(100);
            }
        }
    }
    void refreshRecords(){for(size_t i=0;i<3;++i){try{records[i]=readSetup(backend.root(),compatibleGames[i].setupId);}catch(...){records[i]={};}}}
    // Digests every file of a manifest pins (the chips of each game card), read from the versioned manifest.
    void readManifests(){
        for(size_t i=0;i<3;++i){
            const auto text=tinyjson::Read(backend.root()/compatibleGames[i].manifest,1<<22);if(!text)continue;
            tinyjson::Parser parser(*text);tinyjson::Value value;if(!parser.Parse(value)||value.kind!=tinyjson::Value::Object)continue;
            const auto roms=value.fields.find("roms");if(roms==value.fields.end()||roms->second.kind!=tinyjson::Value::Array||roms->second.items.empty())continue;
            for(const char* key:{"size","crc32","sha1","sha256"})
                if(std::all_of(roms->second.items.begin(),roms->second.items.end(),[&](const tinyjson::Value& rom){return rom.fields.count(key)>0;}))
                    manifestChecks[i].push_back(checkLabel(key));
        }
    }
    fs::path pickFolder(const std::wstring& title){
        const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        struct Com{HRESULT hr;~Com(){if(SUCCEEDED(hr))CoUninitialize();}} comGuard{com};
        IFileOpenDialog* dialog=nullptr;
        if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))){notice=L"Folder picker unavailable.";return {};}
        DWORD options=0;dialog->GetOptions(&options);dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST);
        dialog->SetTitle(title.c_str());
        fs::path folder;
        if(SUCCEEDED(dialog->Show(window))){IShellItem* item=nullptr;if(SUCCEEDED(dialog->GetResult(&item))){PWSTR path=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))){folder=path;CoTaskMemFree(path);}item->Release();}}
        dialog->Release();
        return folder;
    }
    // ROM folder of one game (or of every game, game<0). Nothing runs until Verify or Build.
    void chooseFolder(int game){
        if(backend.running())return;
        const auto folder=pickFolder(game<0?std::wstring(L"Folder with the ROM files of your own boards"):L"ROM files of your own board: "+std::wstring(compatibleGames[size_t(game)].title));
        if(folder.empty())return;
        bool saved=true;for(int i=game<0?0:game;i<=(game<0?2:game);++i)saved=backend.setRomFolder(i,folder)&&saved;
        notice=saved?L"ROM folder saved. Use Verify to check the files.":backend.status();
        if(game>=0)setupFocus=reportGame=game;
    }
    void openPath(const fs::path& path){
        rejectLinks(path);
        if(!fs::exists(path)){notice=L"Not found: "+path.filename().wstring();return;}
        if(INT_PTR(ShellExecuteW(window,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)notice=L"Could not open "+path.filename().wstring()+L".";
    }
    bool copyText(const std::wstring& text){
        if(!OpenClipboard(window))return false;
        const HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,(text.size()+1)*sizeof(wchar_t));bool copied=false;
        if(memory){
            if(void* bytes=GlobalLock(memory)){std::memcpy(bytes,text.c_str(),(text.size()+1)*sizeof(wchar_t));GlobalUnlock(memory);
                copied=EmptyClipboard()&&SetClipboardData(CF_UNICODETEXT,memory);}
            if(!copied)GlobalFree(memory);   // on success the clipboard owns it
        }
        CloseClipboard();return copied;
    }
    bool settingsEnabled()const{return !closing&&!backend.running();}
    bool ready(int game)const{return game==0?mk1Available:game==1?mk2Available:game==2&&mk3Available;}
    // MK3 card: the two editions of the native package (Original / Remastered video layer); UMK3 is not integrated.
    const wchar_t* mk3Edition()const{return backend.mk3Engine()==2?L"NATIVE  /  REMASTERED":L"NATIVE  /  ORIGINAL";}
    bool gameSelected()const{return settingsEnabled()&&chosen==selected&&ready(selected);}
    void rememberEdition(int game){if(game==0)mk1Remastered=backend.config.remastered;else if(game==1)mk2Remastered=backend.config.remastered;}
    bool tabEnabled(int tab)const{return tab==launcherTab||tab==controlsTab||editingGame;}
    // MK1 package, native MK2 and the MK3 native package share the presenter options (filters, CRT, effects, sync, volume).
    bool nativeVideo()const{return selected==0||(selected==1&&backend.nativeMk2Installed())||selected==2;}
    VideoConfig& video(){return draft.video(videoRemastered);}
    const VideoConfig& video()const{return draft.video(videoRemastered);}
    bool scaleFxActive()const{return nativeVideo()&&video().scalefx;}
    // Widescreen: the Remastered profile of MK1, of the native MK2 and of MK3 (DISPLAY > SCREEN: TV FRAME, 4:3, WIDESCREEN).
    bool widescreenAvailable()const{return (selected==0||(selected==1&&backend.nativeMk2Installed())||selected==2)&&videoRemastered;}
    bool gameVolumeEnabled()const{return nativeVideo();}
    int menuMusicLevel()const{
        const auto& preferences=settings&&category==launcherTab?draftLauncher:launcher;
        return preferences.musicMuted?0:preferences.musicVolume;
    }
    std::array<int,7> videoControls()const{
        return draft.videoEffects?std::array<int,7>{videoProfile,videoPage,255,256,videoCas,videoSmaa,videoBilateral}
                           :std::array<int,7>{videoProfile,videoPage,videoScale,201,200,207,208};
    }
    // HDR on/off is on both pages, so it is visible without paging; its brightness controls are on the HDR page.
    std::vector<int> displayControls()const{
        return displayHdr?std::vector<int>{videoProfile,displayPage,hdrMode,hdrPaper,hdrPeak,hdrHighlights}
                         :std::vector<int>{videoProfile,displayPage,250,251,209,videoFullscreen,hdrMode};
    }
    bool videoControlEnabled(int id)const{
        if(id==256)return scaleFxActive();
        if(id==hdrHighlights)return nativeVideo()&&video().hdr&&videoRemastered;
        if(id==hdrPaper||id==hdrPeak)return nativeVideo()&&video().hdr;
        if(id==displayPage||id==hdrMode)return nativeVideo();
        if(id==videoProfile||id==videoPage||id==videoScale||id==255||id==videoCas||
           id==videoSmaa||id==videoBilateral||id==251||id==videoFullscreen)return nativeVideo();
        return true;
    }
    GameAdjustments effectiveAdjustments()const{
        auto value=draftAdjustments;
        // The legacy MK1 electrical DIP word selects USA 1; only blood/violence bits vary.
        if(selected==0&&!draft.cmosCoinage){value.coinage=1;value.freePlay=false;value.customCoinage=false;}
        return value;
    }
    void selectMachine(int index){
        if(backend.running()||settings||index<0||index>=int(games.size()))return;
        if(chosen==index&&selected==index&&index==2&&mk3Available){
            const int engine=3-backend.mk3Engine();
            if(!backend.chooseMk3Engine(engine)){notice=backend.status();return;}
            draft=backend.config;notice=std::wstring(mk3Edition())+L" selected. Use PLAY to start.";
            eventLog(backend.root(),"mk3-engine "+std::to_string(engine));return;
        }
        if(chosen==index&&selected==index&&index<3){
            if(!gameSelected()){notice=L"Game unavailable: build it on the Build page first.";return;}
            musicAvailable=backend.remasteredAvailable();
            if(!musicAvailable&&!backend.config.remastered){notice=L"Original Arcade: the remastered version is unavailable.";return;}
            const auto before=backend.config;backend.config.remastered=!before.remastered;
            if(!backend.save()){notice=backend.status();backend.config=before;return;}
            draft=backend.config;rememberEdition(index);
            notice=backend.config.remastered?L"Remastered selected. Use PLAY to start.":L"Original Arcade selected. Use PLAY to start.";
            eventLog(backend.root(),backend.config.remastered?"mode-remastered":"mode-original");return;
        }
        if(index<3){
            const auto before=backend.config;const int previous=chosen>=0?chosen:0;
            if(!backend.selectGame(index,false)){notice=backend.status();backend.selectGame(previous,false);backend.config=before;return;}
            draft=backend.config;musicAvailable=backend.remasteredAvailable();rememberEdition(index);
        }
        selected=chosen=index;details=false;focus=index;page=pagePlay;
        if(gameSelected()){
            if(menuMusic)menuMusic->duck();
            coinSound();
        }
        notice=gameSelected()?L"":L"Game unavailable: build it on the Build page first.";
        eventLog(backend.root(),"game-select "+std::to_string(index));
    }
    void openDetails(){selectMachine(selected);}
    void invalidate(){receivedInput=true;}
    float layoutWidth()const{return graphics?graphics->layoutWidth():baseWidth;}
    float extraWidth()const{return layoutWidth()-baseWidth;}
    float contentRight()const{return layoutWidth()-20;}
    float cabinetShift(int i)const{return wideCabinetShift[size_t(i)]*extraWidth()/(wideWidth-baseWidth);}
    Rect rightAligned(Rect box)const{box.x+=extraWidth();return box;}
    // ---- Setup state shown on the pages (backend queue + finished records) ----
    bool setupQueued()const{for(int i=0;i<3;++i){const auto& s=backend.setupGame(i);if(s.queued||s.active)return true;}return false;}
    int activeGame()const{for(int i=0;i<3;++i)if(backend.setupGame(i).active)return i;return -1;}
    bool idle(int i)const{const auto& s=backend.setupGame(i);return !s.queued&&!s.active;}
    bool pythonMissing()const{const auto& r=backend.setupRequirements();return r.checked&&(r.python.empty()||!r.pythonSupported);}
    bool canVerify(int i)const{return backend.romsAcknowledged()&&!backend.setupGame(i).folder.empty()&&idle(i)&&!backend.running()&&!pythonMissing();}
    bool canBuild(int i)const{return canVerify(i)&&!(backend.setupRequirements().checked&&backend.setupRequirements().compiler.empty());}
    std::wstring buildLabel(int i)const{
        const auto& s=backend.setupGame(i);
        return s.active?(s.mode=="verify"?L"Verifying":L"Building"):s.queued?L"Queued":records[size_t(i)].finished?L"Rebuild":records[size_t(i)].outdated?L"Update":L"Build";
    }
    // CONTROLS tab cells (ids, boxes and their player / column / action).
    static std::tuple<int,int,unsigned> cell(int id){const int n=id-controlCell;return {n/50,n%50/25,unsigned(n%25)};}
    static Rect cellBox(int id){const auto [player,column,action]=cell(id);return {560+float(player*2+column)*170,368+float(action)*27,160,26};}
    static std::vector<int> controlCells(){
        std::vector<int> ids;for(int player:{0,1})for(int column:{0,1})for(unsigned a=0;a<mk5controls::Count;++a)
            if(!column||a>=mk5controls::HighPunch)ids.push_back(controlCell+player*50+column*25+int(a));
        return ids;
    }
    // Every controller's buttons (XInput pads and Raw Input HID) as one mk5controls mask.
    uint32_t controllerHeld()const{
        uint32_t held=0;for(unsigned i=0;i<4;++i)if(pads.connected[i])held|=mk5pad::buttons(pads.states[i].Gamepad);
        for(const auto& hid:mk5hid::shared().pads())held|=hid.buttons;
        return held;
    }
    static std::wstring keyName(uint8_t vk){
        if(!vk)return L"-";
        UINT scan=MapVirtualKeyW(vk,MAPVK_VK_TO_VSC);
        const bool extended=(vk>=VK_PRIOR&&vk<=VK_DOWN)||vk==VK_INSERT||vk==VK_DELETE||vk==VK_DIVIDE||vk==VK_NUMLOCK||vk==VK_RCONTROL||vk==VK_RMENU;
        wchar_t name[64]{};if(scan&&GetKeyNameTextW(LONG(scan<<16|(extended?1u<<24:0)),name,64)>0){std::wstring s=name;for(auto& c:s)c=wchar_t(towupper(c));return s;}
        wchar_t hex[16];swprintf(hex,16,L"KEY %02X",vk);return hex;
    }
    static std::wstring padName(uint32_t mask){
        constexpr std::array<std::pair<uint32_t,const wchar_t*>,12> named{{{mk5controls::A,L"A"},{mk5controls::B,L"B"},{mk5controls::X,L"X"},{mk5controls::Y,L"Y"},
            {mk5controls::LB,L"LB"},{mk5controls::RB,L"RB"},{mk5controls::LT,L"LT"},{mk5controls::RT,L"RT"},{mk5controls::BACK,L"BACK"},
            {mk5controls::START,L"START"},{0x40,L"L3"},{0x80,L"R3"}}};
        std::wstring out;
        for(const auto& [bit,name]:named)if(mask&bit)out+=(out.empty()?L"":L" / ")+std::wstring(name);
        for(unsigned b=0;b<32;++b){const uint32_t bit=1u<<b;bool known=false;for(const auto& n:named)known|=n.first==bit;
            if((mask&bit)&&!known)out+=(out.empty()?L"":L" / ")+(b>=18?L"BUTTON "+std::to_wstring(b-7):std::wstring(L"D-PAD"));}
        return out.empty()?L"-":out;
    }
    std::wstring badge(int i)const{
        const auto& s=backend.setupGame(i);
        if(s.active)return s.mode=="verify"?L"Verifying":L"Building";
        if(s.queued)return L"Queued";
        if(s.state=="failed")return L"Failed";
        if(records[size_t(i)].finished)return ready(i)?L"Ready":L"Built";
        if(records[size_t(i)].outdated)return L"Update";
        return s.verified?L"Verified":L"Supported";
    }
    // Step of one game in the six-step strip: folder, verify, generate, validate, compile, ready.
    int setupStep(int i)const{
        const auto& s=backend.setupGame(i);
        if(s.active&&s.mode=="setup")return stepOf(s.step);
        if(records[size_t(i)].finished&&s.state!="failed")return 5;
        if(s.verified)return 2;
        return s.folder.empty()?0:1;
    }
    int stepGame()const{const int a=activeGame();return a>=0?a:setupFocus;}
    // ---- Geometry of the setup pages (shared by drawing, hit-testing and keyboard focus) ----
    Rect navBox(int k)const{return {16,142+float(k)*58,192,48};}
    Rect sidePanel()const{return {contentRight()-340,206,340,464};}
    Rect cardRect(int i)const{const float right=sidePanel().x-16,w=(right-contentLeft-14)/2;return {contentLeft+float(i%2)*(w+14),246+float(i/2)*218,i==2?right-contentLeft:w,206};}
    Rect folderField(Rect card)const{const float x=card.x+218;return {x,card.y+98,card.x+card.w-14-x,28};}
    Rect romRow(int i)const{return {contentLeft,290+float(i)*94,contentRight()-contentLeft,84};}
    Rect buildRow(int i)const{return {contentLeft,228+float(i)*228,contentRight()-contentLeft,214};}
    std::vector<Button> pageButtons()const{
        std::vector<Button> b;const float x1=contentRight();
        if(page==pageOverview){
            for(int i=0;i<3;++i){const Rect c=cardRect(i);
                b.push_back({50+i,folderField(c),backend.setupGame(i).folder.wstring(),idle(i)&&!backend.running(),styleField});
                b.push_back({70+i,{c.x+c.w-118,c.y+c.h-46,104,34},buildLabel(i),canBuild(i),stylePrimary});}
        }else if(page==pageRoms){
            if(!backend.romsAcknowledged())b.push_back({43,{x1-170,238,150,36},L"Open Legal",true,stylePrimary});
            b.push_back({30,{x1-320,140,300,40},L"Use one folder for all games",idle(0)&&idle(1)&&idle(2)&&!backend.running(),styleNormal});
            for(int i=0;i<3;++i){const Rect r=romRow(i);
                b.push_back({130+i,{r.x,r.y,300,r.h},L"",true,styleHidden});
                b.push_back({50+i,{r.x+320,r.y+14,r.w-320-190,32},backend.setupGame(i).folder.wstring(),idle(i)&&!backend.running(),styleField});
                b.push_back({60+i,{x1-170,r.y+20,150,40},backend.setupGame(i).active&&backend.setupGame(i).mode=="verify"?L"Verifying":L"Verify",canVerify(i),stylePrimary});}
        }else if(page==pageBuild){
            b.push_back({124,{x1-410,140,220,40},L"Check requirements",!setupQueued(),styleNormal});
            bool any=false;for(int i=0;i<3;++i)any=any||canBuild(i);
            b.push_back({73,{x1-170,140,150,40},L"Build all",any,stylePrimary});
            for(int i=0;i<3;++i){const Rect r=buildRow(i);const auto& s=backend.setupGame(i);const bool done=records[size_t(i)].finished;
                b.push_back({70+i,{x1-190,r.y+18,170,40},buildLabel(i),canBuild(i),stylePrimary});
                b.push_back({80+i,{x1-190,r.y+66,170,36},L"Cancel",s.queued||s.active,styleNormal});
                b.push_back({90+i,{r.x+20,r.y+r.h-50,130,34},L"Open log",fs::exists(backend.setupLog(i)),styleNormal});
                b.push_back({95+i,{r.x+160,r.y+r.h-50,150,34},L"Open folder",done,styleNormal});
                b.push_back({100+i,{r.x+320,r.y+r.h-50,150,34},L"Copy report",s.state=="failed"&&!s.active,styleNormal});
                b.push_back({110+i,{x1-190,r.y+r.h-52,170,38},L"Play",ready(i)&&idle(i),stylePrimary});}
        }else if(page==pageLegal){
            b.push_back({120,{contentLeft+30,790,x1-contentLeft-60,44},
                         L"I own the original board / ROM files of the games I set up, and I obtained them legally.",true,styleCheck});
            const std::array<const wchar_t*,3> docs{L"DISCLAIMER.md",L"THIRD_PARTY_NOTICES.md",L"LICENSE"};
            for(int k=0;k<3;++k)b.push_back({121+k,{contentLeft+30+float(k)*260,856,240,40},docs[size_t(k)],true,styleNormal});
        }
        return b;
    }
    void shellControls(){
        for(int k=0;k<6;++k)controls.push_back({navIds[size_t(k)],navBox(k),navIds[size_t(k)]!=11||settingsEnabled()});
        const float w=layoutWidth();controls.push_back({12,{w-92,0,44,34},true});controls.push_back({13,{w-46,0,44,34},true});
    }
    void hitTargets(){
        // Input follows current state immediately, including multiple messages
        // delivered before the next GPU frame completes.
        controls.clear();
        if(!settings&&!details&&page==pagePlay){for(int i=0;i<3;++i){auto r=positions[size_t(i)];controls.push_back({i+1,{r.x+cabinetShift(i),r.y+15,r.w,680},!backend.running()});}if(chosen>=0)controls.push_back({20,rightAligned({1256,888,244,52}),gameSelected()});}
        if(!settings&&page!=pagePlay)for(const auto& button:pageButtons())controls.push_back({button.id,button.box,button.enabled});
        if(settings){
            for(int i=0;i<tabCount;++i)controls.push_back({220+i,{300+float(i)*134,294,134,40},tabEnabled(i)});
            if(category==controlsTab){for(int id:controlCells())controls.push_back({id,cellBox(id),true});controls.push_back({socdControl,{969,698,260,28},true});controls.push_back({runAheadControl,{969,730,260,28},true});controls.push_back({trainingControl,{969,762,260,28},true});}
            if(category==0){float y=350;for(int id:videoControls()){controls.push_back({id,{969,y,260,54},videoControlEnabled(id)});y+=60;}}
            if(category==1){controls.push_back({245,{969,350,260,54},musicAvailable||draft.remastered});controls.push_back({205,{969,420,260,54},true});controls.push_back({243,{969,490,260,54},false});controls.push_back({244,{969,560,260,54},false});}
            if(category==2){float y=350;for(int id:displayControls()){controls.push_back({id,{969,y,260,54},videoControlEnabled(id)});y+=60;}}
            if(category==4){for(int i=0;i<5;++i)controls.push_back({260+i,{969,350+float(i)*70,260,54},i<3?backend.adjustmentsAvailable():backend.dipSwitchesAvailable()});}
            if(category==5){controls.push_back({270,{969,350,260,54},gameVolumeEnabled()});controls.push_back({271,{969,420,260,54},backend.adjustmentsAvailable()});if(resetScoresPending){controls.push_back({272,{969,520,125,54},true});controls.push_back({273,{1104,520,125,54},true});}}
            if(category==launcherTab){
                std::array<int,5> ids{202,203,204,206,230};
                for(int i=0;i<5;++i)controls.push_back({ids[size_t(i)],{969,350+float(i)*70,260,54},true});
                controls.push_back({menuVolume,menuVolumeBox,true});controls.push_back({menuMute,menuMuteBox,true});
            }
            controls.push_back({212,{300,810,190,50},true});controls.push_back({211,{510,810,240,50},true});controls.push_back({210,{1000,810,235,50},true});
            const float shift=extraWidth()/2;
            for(auto& control:controls)control.box.x+=shift;
        }
        shellControls();
    }
    void openSettings(){
        if(!settingsEnabled())return;
        editingGame=gameSelected()&&backend.selectGame(selected);
        if(editingGame){
            backend.load();musicAvailable=backend.remasteredAvailable();
            draft=backend.config;draftAdjustments=backend.adjustments;videoRemastered=selected==2?draft.mk3Engine==2:draft.remastered;
        }else category=launcherTab;
        draftLauncher=launcher;draftControls=backend.loadControls();capture=-1;
        resetScoresPending=false;settings=true;details=false;focus=0;notice.clear();
        eventLog(backend.root(),"settings-open");
    }
    void selectTab(int tab){
        if(!settings||!tabEnabled(tab))return;
        category=tab;resetScoresPending=false;focus=0;
    }
    void closeSettings(){settings=false;capture=-1;draft=backend.config;focus=page==pagePlay&&chosen>=0?chosen:0;notice.clear();}
    void showPage(int next){
        if(settings)closeSettings();
        details=false;page=next;notice.clear();focus=page==pagePlay&&chosen>=0?chosen:0;
    }
    // PgUp / PgDn and the shoulder buttons walk the left navigation (Settings included).
    void cyclePage(int step){
        int k=settings?4:page==pageOverview?0:page==pageRoms?1:page==pageBuild?2:page==pagePlay?3:5;
        for(int n=0;n<6;++n){k=(k+step+6)%6;if(navIds[size_t(k)]!=11||settingsEnabled())break;}
        act(navIds[size_t(k)]);
    }
    void close(){if(backend.running()){closing=true;backend.requestStop();notice=L"Closing the game and saving the profile...";ShowWindow(window,SW_RESTORE);}else DestroyWindow(window);}
    void borderless(bool on){
        if(on==fullscreen)return;
        if(on){GetWindowRect(window,&placement);MONITORINFO m{};m.cbSize=sizeof(m);GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&m);SetWindowLongPtrW(window,GWL_STYLE,WS_POPUP|WS_VISIBLE);SetWindowPos(window,HWND_TOP,m.rcMonitor.left,m.rcMonitor.top,m.rcMonitor.right-m.rcMonitor.left,m.rcMonitor.bottom-m.rcMonitor.top,SWP_FRAMECHANGED);}
        else{SetWindowLongPtrW(window,GWL_STYLE,WS_POPUP|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_SYSMENU|WS_VISIBLE);SetWindowPos(window,nullptr,placement.left,placement.top,placement.right-placement.left,placement.bottom-placement.top,SWP_FRAMECHANGED|SWP_NOZORDER);}
        fullscreen=on;
    }
    int hit(float x,float y)const{for(auto i=controls.rbegin();i!=controls.rend();++i)if(i->box.contains(x,y))return i->id;return -1;}
    void move(POINT point){if(!graphics)return;hitTargets();graphics->mouse(point,mx,my);int next=hit(mx,my);dragSlider();if(next!=hover){hover=next;auto c=std::find_if(controls.begin(),controls.end(),[&](const Control& v){return v.id==next;});SetCursor(LoadCursorW(nullptr,c!=controls.end()&&c->enabled?IDC_HAND:IDC_ARROW));}invalidate();}
    int sliderValue(float x,float width=260)const{return std::clamp(int(std::lround((x-969-extraWidth()/2)*100/width)),0,100);}
    void setScaleAmount(float x){video().scalefxAmount=sliderValue(x);}
    void setGameVolume(float x){if(gameVolumeEnabled())draft.gameVolume=sliderValue(x);}
    void dragSlider(){
        if(!settings)return;
        if(pressed==256&&category==0&&draft.videoEffects&&scaleFxActive())setScaleAmount(mx);
        if(pressed==270&&category==5)setGameVolume(mx);
        if(pressed==menuVolume&&category==launcherTab)draftLauncher.musicVolume=sliderValue(mx,menuVolumeBox.w);
    }
    void changeAdjustment(int id,int step){
        if(!(id==263||id==264?backend.dipSwitchesAvailable():backend.adjustmentsAvailable()))return;
        if(id==261||id==262){draftAdjustments=effectiveAdjustments();draft.cmosCoinage=true;}
        if(id==260)draftAdjustments.difficulty=(draftAdjustments.difficulty-1+step+5)%5+1;
        if(id==261){draftAdjustments.coinage=(draftAdjustments.coinage-1+step+58)%58+1;draftAdjustments.customCoinage=false;}
        if(id==262)draftAdjustments.freePlay=!draftAdjustments.freePlay;
        if(id==263)draft.blood=!draft.blood;
        if(id==264)draft.violence=!draft.violence;
    }
    void act(int id){
        hitTargets();
        auto it=std::find_if(controls.begin(),controls.end(),[&](const Control& c){return c.id==id;});if(it!=controls.end()&&!it->enabled)return;
        invalidate();
        if(id>=1&&id<=3){selectMachine(id-1);return;}
        if(id==40||id==41||id==42||id==43){showPage(id==40?pageOverview:id==41?pageRoms:id==42?pageBuild:pageLegal);return;}
        if(id==30){chooseFolder(-1);return;}
        if(id>=50&&id<=52){chooseFolder(id-50);return;}
        if(id>=60&&id<=62){setupFocus=reportGame=id-60;backend.queueSetup(id-60,true);notice=backend.status();return;}
        if(id>=70&&id<=72){setupFocus=id-70;backend.queueSetup(id-70,false);notice=backend.status();return;}
        if(id==73){for(int i=0;i<3;++i)if(canBuild(i))backend.queueSetup(i,false);notice=backend.status();return;}
        if(id>=80&&id<=82){backend.cancelSetup();notice=backend.status();return;}
        if(id>=90&&id<=92){openPath(backend.setupLog(id-90));return;}
        if(id>=95&&id<=97){openPath(backend.setupOutput(id-95));return;}
        if(id>=100&&id<=102){notice=copyText(backend.setupReport(id-100))?L"Report copied. Paste it in your message to the project.":L"Could not copy the report.";return;}
        if(id>=110&&id<=112){if(chosen!=id-110)selectMachine(id-110);page=pagePlay;focus=3;return;}
        if(id==120){backend.acknowledgeRoms(!backend.romsAcknowledged());notice=backend.romsAcknowledged()?L"Confirmation saved. You can verify and build your games.":L"Confirmation withdrawn.";return;}
        if(id>=121&&id<=123){openPath(backend.root()/std::array<const wchar_t*,3>{L"DISCLAIMER.md",L"THIRD_PARTY_NOTICES.md",L"LICENSE"}[size_t(id-121)]);return;}
        if(id==124){backend.checkRequirements();notice=L"Checking Python and the compiler...";return;}
        if(id>=130&&id<=132){setupFocus=reportGame=id-130;return;}
        if(id==10){showPage(pagePlay);draft=backend.config;}
        if(id==11)openSettings();
        if(id==12)ShowWindow(window,SW_MINIMIZE);
        if(id==13)close();
        if(id==20&&!settings&&gameSelected()){
            if(menuMusic)menuMusic->pause();
            PlaySoundW(nullptr,nullptr,0);
            if(backend.launch()){notice=backend.status();eventLog(backend.root(),"game-start "+std::to_string(selected));}
            else{notice=backend.status();if(selected==2)mk3Available=false;eventLog(backend.root(),"game-start-error");}
        }
        if(id==21||id==230){auto logs=backend.root()/(id==230?L"logs":selected==2?L"logs/mk3-native":selected==1?L"logs/mk2-v1":L"logs/temporaria-8");rejectLinks(logs);fs::create_directories(logs);auto result=ShellExecuteW(window,L"open",logs.c_str(),nullptr,nullptr,SW_SHOWNORMAL);if(INT_PTR(result)<=32)notice=L"Could not open the logs folder.";}
        if(id==103){details=false;notice.clear();}
        if(nativeVideo()&&(id==videoProfile||id==videoPage)){
            if(pressed==256){pressed=-1;if(window&&GetCapture()==window)ReleaseCapture();}
            // MK1 and MK2: the profile shown is the edition the game starts in (DEFAULT GAME VERSION), one choice for the three
            // tabs, so the profile edited is the one saved as the game's and shown again when Settings reopen (issue #5).
            if(id==videoProfile){videoRemastered=!videoRemastered;if(selected!=2&&(musicAvailable||!videoRemastered))draft.remastered=videoRemastered;}
            else draft.videoEffects=!draft.videoEffects;
        }
        if(id==videoScale&&nativeVideo())video().scaleMode=(video().scaleMode+1)%6;
        if(id==200)video().scanlines=(video().scanlines+1)%(nativeVideo()?int(mk5video::presets.size()):4);
        if(id==201)video().filter=(video().filter+1)%(nativeVideo()?4:2);
        if(id==202)draftLauncher.fps=draftLauncher.fps==30?60:(draftLauncher.fps==60?120:(draftLauncher.fps==120?240:30));
        if(id==245){draft.remastered=!draft.remastered;if(selected!=2)videoRemastered=draft.remastered;}
        if(id==205)draft.audioLatency=draft.audioLatency==1?2:1;
        if(id==207)video().color=(video().color+1)%3;
        if(id==208)video().brightness=video().brightness==120?80:video().brightness+10;
        if(id==209&&widescreenAvailable()){auto& v=video();if(v.widescreen){v.widescreen=false;v.bezel=true;}else if(v.bezel)v.bezel=false;else v.widescreen=true;}
        else if(id==209)video().bezel=!video().bezel;
        if(id==255&&nativeVideo())video().scalefx=!video().scalefx;
        if(id==videoCas&&nativeVideo())video().cas=video().cas>=100?0:std::min(video().cas+25,100);
        if(id==videoSmaa&&nativeVideo())video().smaa=!video().smaa;
        if(id==videoBilateral&&nativeVideo())video().bilateral=video().bilateral>=100?0:std::min(video().bilateral+25,100);
        if(id==250)video().sync=nativeVideo()?(video().sync+1)%3:(video().sync==0?2:0);
        if(id==251&&nativeVideo())video().highRefresh=!video().highRefresh;
        if(id==videoFullscreen&&nativeVideo())video().fullscreen=!video().fullscreen;
        if(id==displayPage&&nativeVideo())displayHdr=!displayHdr;
        if(id==hdrMode&&nativeVideo())video().hdr=!video().hdr;
        if(id==hdrPaper&&videoControlEnabled(hdrPaper)){auto& n=video().hdrPaper;n=n==0?80:n>=480?0:n<320?n+40:n+80;}
        if(id==hdrPeak&&videoControlEnabled(hdrPeak)){auto& n=video().hdrPeak;n=n==0?400:n<1000?n+200:n<2000?n+500:n<4000?4000:0;}
        if(id==hdrHighlights&&videoControlEnabled(hdrHighlights))video().hdrHighlights=video().hdrHighlights>=100?0:video().hdrHighlights+25;
        if(id==203)draftLauncher.vsync=!draftLauncher.vsync;
        if(id==204)draftLauncher.reducedMotion=!draftLauncher.reducedMotion;
        if(id==206)draftLauncher.fullscreen=!draftLauncher.fullscreen;
        if(id==menuMute&&settings&&category==launcherTab)draftLauncher.musicMuted=!draftLauncher.musicMuted;
        if(id>=260&&id<=264&&settings&&category==4)changeAdjustment(id,1);
        if(id==271&&settings&&category==5&&backend.adjustmentsAvailable()){resetScoresPending=true;notice=L"Reset the saved ranking? This is immediate and cannot be undone.";}
        if(id==272&&settings&&category==5&&resetScoresPending){backend.resetHighScores();resetScoresPending=false;notice=backend.status();}
        if(id==273){resetScoresPending=false;notice.clear();}
        if(id==210&&settings){
            if(category==launcherTab){
                if(backend.saveLauncherPreferences(draftLauncher)){
                    launcher=draftLauncher;borderless(launcher.fullscreen);
                    notice=L"Launcher preferences saved.";
                    eventLog(backend.root(),"launcher-settings-saved");
                }else notice=L"Could not save the launcher preferences.";
            }else if(editingGame){
                if(backend.saveAll(draft,draftAdjustments)){
                    draft=backend.config;draftAdjustments=backend.adjustments;
                    rememberEdition(selected);
                    eventLog(backend.root(),"settings-saved");
                }
                notice=backend.status();
            }
        }
        if(id==211&&settings&&category==controlsTab){draftControls={};capture=-1;notice=L"Original controls restored on screen. Use SAVE to apply.";}
        if(id==210&&settings&&category==controlsTab){
            notice=backend.saveControls(draftControls)?L"Controls saved for every game.":L"Could not save the controls.";capture=-1;
        }
        if(id==211&&settings){
            if(category==launcherTab){
                draftLauncher={};
                notice=L"Launcher defaults restored on screen. Use SAVE to apply.";
            }else if(category==0||category==2){
                video()=selected==2&&videoRemastered?mklauncher::mk3RemasteredDefaults():VideoConfig{};if(!nativeVideo())video().sync=2;
                notice=L"Video profile restored on screen. Use SAVE to apply.";
            }else{
                const auto videos=draft.videos;
                const bool videoEffects=draft.videoEffects;
                const int mk3Engine=draft.mk3Engine;
                draft=Config{};draft.videos=videos;draft.videoEffects=videoEffects;draft.mk3Engine=mk3Engine;
                draftAdjustments=backend.adjustmentsAvailable()?GameAdjustments{}:backend.adjustments;
                resetScoresPending=false;
                notice=L"Defaults restored on screen. Use SAVE to apply.";
            }
        }
        if(id==212)closeSettings();
        if(id>=220&&id<220+tabCount)selectTab(id-220);
        if(settings&&category==controlsTab){
            const auto cells=controlCells();if(std::find(cells.begin(),cells.end(),id)!=cells.end()){
                capture=id;captureHeld=controllerHeld();const auto [player,column,action]=cell(id);
                notice=std::wstring(L"Press ")+(column?L"a controller button":L"a key")+L" for P"+std::to_wstring(player+1)+L" "+mk5controls::names[action]+L". Esc cancels.";
            }
            if(id==socdControl)draftControls.socd=!draftControls.socd;
            if(id==runAheadControl)draftControls.runAhead=!draftControls.runAhead;
            if(id==trainingControl)draftControls.training=!draftControls.training;
        }
    }
    std::vector<int> actions()const{
        if(!settings){
            if(page==pagePlay)return {1,2,3,20,11};
            std::vector<int> a;for(const auto& button:pageButtons())if(button.enabled)a.push_back(button.id);
            if(a.empty())a.push_back(navIds[size_t(page==pageOverview?0:page==pageRoms?1:page==pageBuild?2:5)]);
            return a;
        }
        std::vector<int> a;
        if(category==0)for(int id:videoControls()){if(videoControlEnabled(id))a.push_back(id);}
        else if(category==2)for(int id:displayControls()){if(videoControlEnabled(id))a.push_back(id);}
        else if(category==1){if(musicAvailable||draft.remastered)a.push_back(245);a.push_back(205);}
        else if(category==4){if(backend.adjustmentsAvailable())a={260,261,262};if(backend.dipSwitchesAvailable())a.insert(a.end(),{263,264});}
        else if(category==5){
            if(gameVolumeEnabled())a.push_back(270);
            if(backend.adjustmentsAvailable())a.push_back(271);
            if(resetScoresPending){a.push_back(272);a.push_back(273);}
        }else if(category==controlsTab){a=controlCells();a.push_back(socdControl);a.push_back(runAheadControl);a.push_back(trainingControl);}
        else a={202,203,204,206,230,menuVolume,menuMute};
        a.insert(a.end(),{210,211,212});return a;
    }
    void key(WPARAM key){
        invalidate();
        if(capture>=0){ // the next key binds the cell, Esc cancels; a controller cell takes only Esc here
            const auto [player,column,action]=cell(capture);
            if(key==VK_ESCAPE)notice=L"Unchanged.";
            else if(column)return;
            else if(key==VK_F5||key==VK_F7){notice=L"F5/F7 save and load the game state.";return;}
            else{draftControls.keys[size_t(player)][action]=uint8_t(key);notice=L"Key set. Use SAVE to apply.";}
            capture=-1;return;
        }
        if(key==VK_F11){borderless(!fullscreen);return;}
        if(key==VK_ESCAPE){if(fullscreen){borderless(false);eventLog(backend.root(),"fullscreen-exit-escape");return;}if(details){details=false;notice.clear();}else if(settings)closeSettings();else close();return;}
        if(key==VK_PRIOR||key==VK_NEXT){cyclePage(key==VK_NEXT?1:-1);return;}
        if(key==VK_TAB){auto a=actions();focus=(focus+1)%int(a.size());return;}
        if(!settings&&!details&&page==pagePlay){
            if(key==VK_LEFT||key==VK_RIGHT){focus=focus<3?(focus+(key==VK_LEFT?2:1))%3:(focus==3?4:3);}
            if(key==VK_DOWN)focus=focus<3?3:(focus==3?4:3);
            if(key==VK_UP)focus=focus>=3?(chosen>=0?chosen:0):4;
            if(key==VK_RETURN||key==VK_SPACE){auto a=actions();act(a[size_t(std::clamp(focus,0,int(a.size())-1))]);}
            if(key=='S'){openSettings();}
            if(key=='L'){act(21);}
        }else if(!settings&&!details){
            auto a=actions();const int n=int(a.size());focus=std::clamp(focus,0,n-1);
            if(key==VK_UP||key==VK_LEFT)focus=(focus+n-1)%n;
            if(key==VK_DOWN||key==VK_RIGHT)focus=(focus+1)%n;
            if(key==VK_RETURN||key==VK_SPACE)act(a[size_t(focus)]);
            if(key=='S')openSettings();
        }else{
            auto a=actions();if(key==VK_UP)focus=(focus+int(a.size())-1)%int(a.size());if(key==VK_DOWN)focus=(focus+1)%int(a.size());focus=std::min(focus,int(a.size())-1);
            if(settings&&a[size_t(focus)]==256&&(key==VK_LEFT||key==VK_RIGHT)){video().scalefxAmount=std::clamp(video().scalefxAmount+(key==VK_LEFT?-5:5),0,100);return;}
            if(settings&&a[size_t(focus)]==270&&(key==VK_LEFT||key==VK_RIGHT)){draft.gameVolume=std::clamp(draft.gameVolume+(key==VK_LEFT?-5:5),0,100);return;}
            if(settings&&a[size_t(focus)]==menuVolume&&(key==VK_LEFT||key==VK_RIGHT)){draftLauncher.musicVolume=std::clamp(draftLauncher.musicVolume+(key==VK_LEFT?-5:5),0,100);return;}
            if(settings&&category==4&&(key==VK_LEFT||key==VK_RIGHT)&&a[size_t(focus)]>=260&&a[size_t(focus)]<=264){changeAdjustment(a[size_t(focus)],key==VK_LEFT?-1:1);return;}
            if(details&&(key==VK_LEFT||key==VK_RIGHT)){focus=(focus+1)%int(a.size());}
            const int id=a[size_t(focus)];const bool editable=(id>=200&&id<=209)||id==245||id==250||id==251||id==255||(id>=videoProfile&&id<=videoFullscreen)||(id>=displayPage&&id<=hdrHighlights);
            if(key==VK_RETURN||key==VK_SPACE||((key==VK_LEFT||key==VK_RIGHT)&&editable))act(id);
            if(key>='1'&&key<'1'+tabCount)selectTab(int(key-'1'));
        }
    }
    WORD padEdges(WORD buttons){
        if(!active){padReady=false;gamepadButtons=padDirections=0;return 0;}
        // A button held in the game or another app is not a new launcher action.
        if(!padReady){gamepadButtons=buttons;padDirections=0;padReady=buttons==0;return 0;}
        const WORD edge=buttons&~gamepadButtons;gamepadButtons=buttons;return edge;
    }
    void padActions(WORD edge){
        if(edge&XINPUT_GAMEPAD_B){if(settings)closeSettings();else if(details){details=false;notice.clear();}return;}
        for(auto pair:{std::pair<WORD,int>{XINPUT_GAMEPAD_DPAD_LEFT,VK_LEFT},{XINPUT_GAMEPAD_DPAD_RIGHT,VK_RIGHT},{XINPUT_GAMEPAD_DPAD_UP,VK_UP},{XINPUT_GAMEPAD_DPAD_DOWN,VK_DOWN},{WORD(XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_START),VK_RETURN},{XINPUT_GAMEPAD_Y,'S'}})if(edge&pair.first)key(WPARAM(pair.second));
        if(edge&(XINPUT_GAMEPAD_LEFT_SHOULDER|XINPUT_GAMEPAD_RIGHT_SHOULDER)){const bool right=(edge&XINPUT_GAMEPAD_RIGHT_SHOULDER)!=0;if(settings)selectTab((category+(right?1:tabCount-1))%tabCount);else cyclePage(right?1:-1);}
    }
    void pad(){
        if(!active){padReady=false;gamepadButtons=0;return;}
        const auto now=GetTickCount64();if(now<nextPad)return;nextPad=now+4;pads.poll();XINPUT_STATE state{};bool found=false;
        if(capture>=0&&std::get<1>(cell(capture))){ // binding a controller button: the first newly pressed one
            const uint32_t held=controllerHeld(),fresh=held&~captureHeld;captureHeld=held;
            if(fresh){const auto [player,column,action]=cell(capture);draftControls.pad[size_t(player)][action]=fresh&(~fresh+1);
                capture=-1;notice=L"Button set. Use SAVE to apply.";gamepadButtons=0xffff;}
            return;
        }
        for(unsigned i=0;i<4;++i)if(pads.connected[i]){state=pads.states[i];found=true;break;}
        if(!found){padReady=false;gamepadButtons=0;padDirections=0;return;}
        WORD buttons=state.Gamepad.wButtons;if(state.Gamepad.sThumbLX<-18000)buttons|=XINPUT_GAMEPAD_DPAD_LEFT;if(state.Gamepad.sThumbLX>18000)buttons|=XINPUT_GAMEPAD_DPAD_RIGHT;if(state.Gamepad.sThumbLY>18000)buttons|=XINPUT_GAMEPAD_DPAD_UP;if(state.Gamepad.sThumbLY<-18000)buttons|=XINPUT_GAMEPAD_DPAD_DOWN;
        WORD edge=padEdges(buttons);if(!padReady)return;
        const WORD direction=buttons&0x000f;if(direction!=padDirections){padDirections=direction;nextPadRepeat=now+350;}else if(direction&&now>=nextPadRepeat){edge|=direction;nextPadRepeat=now+90;}
        padActions(edge);
    }
    void poll(){
        const auto now=GetTickCount64();
        if(now<nextPoll)return;
        nextPoll=now+200;bool was=backend.running();backend.poll();
        // Setup queue: advance it, show its status, and re-check the games once a run ends.
        const bool busy=backend.setupRunning();refreshRecords();
        if(backend.status()!=seenStatus){seenStatus=backend.status();if(busy||setupBusy)notice=seenStatus;}
        if(setupBusy&&!busy)availabilityDue=0;
        setupBusy=busy;
        if(availability.valid()&&availability.wait_for(std::chrono::seconds(0))==std::future_status::ready){
            const auto ready=availability.get();
            mk1Available=ready[0];mk2Available=ready[1];mk3Available=ready[2];
        }
        if(!backend.running()&&!availability.valid()&&now>=availabilityDue){
            // Native availability hashes the executable and ROMs.
            availability=std::async(std::launch::async,[root=backend.root()]{
                Backend probe(root);
                return std::array<bool,3>{probe.available(0),probe.available(1),probe.available(2)};
            });
            musicAvailable=backend.remasteredAvailable();
            availabilityDue=now+10000;
        }
        if(was&&!backend.running()){ShowWindow(window,SW_RESTORE);SetForegroundWindow(window);restoring=false;notice=backend.status();eventLog(backend.root(),"game-return exit="+std::to_string(backend.exitCode()));if(closing)DestroyWindow(window);}
        if(backend.running()&&backend.gameWindow()&&!restoring&&!closing){restoring=true;ShowWindow(window,SW_MINIMIZE);SetForegroundWindow(backend.gameWindow());}
        if(closing&&backend.running())backend.requestStop();
    }
    // ---- Drawing helpers (all art is drawn in code; no image file is read) ----
    void center(const std::wstring& text,float x,float y,float size,Color color,float tracking=0){graphics->text(text,x-graphics->textWidth(text,size,tracking)/2,y,size,color,tracking);}
    std::wstring fit(const std::wstring& text,float size,float width,bool keepEnd=false)const{
        if(graphics->textWidth(text,size)<=width)return text;
        std::wstring out=text;
        while(!out.empty()&&graphics->textWidth(keepEnd?L"..."+out:out+L"...",size)>width){if(keepEnd)out.erase(0,1);else out.pop_back();}
        return keepEnd?L"..."+out:out+L"...";
    }
    std::vector<std::wstring> wrap(const std::wstring& text,float size,float width)const{
        std::vector<std::wstring> lines;std::wstring line,word;std::wistringstream words(text);
        while(words>>word){const auto next=line.empty()?word:line+L" "+word;if(!line.empty()&&graphics->textWidth(next,size)>width){lines.push_back(line);line=word;}else line=next;}
        if(!line.empty())lines.push_back(line);
        return lines;
    }
    float paragraph(const std::wstring& text,float x,float y,float size,float width,Color color){for(const auto& line:wrap(text,size,width)){graphics->text(line,x,y,size,color,.1f);y+=size*1.5f;}return y;}
    void panel(Rect r,float radius=10){graphics->rounded(r,radius,panelFill);graphics->rounded(r,radius,panelEdge,1.2f);}
    void control(int id,Rect box,const std::wstring& label,bool enabled=true,bool chosen=false,float size=18){
        controls.push_back({id,box,enabled});bool over=hover==id,down=pressed==id,focused=drawnFocus==id;
        auto& g=*graphics;
        Color fill=chosen?Color{.62f,.05f,.06f,1}:Color{.085f,.082f,.092f,1};
        if(enabled&&(over||focused))fill=chosen?Color{.78f,.09f,.09f,1}:Color{.16f,.07f,.08f,1};
        if(enabled&&down)fill={.42f,.04f,.04f,1};
        if(!enabled)fill={.05f,.05f,.055f,1};
        g.rounded(box,7,fill);
        g.rounded(box,7,enabled&&(chosen||over||focused)?(focused||over?redHot:red):panelEdge,focused?2.2f:1.f);
        center(label,box.x+box.w/2,box.y+(box.h-size*1.15f)/2,size,enabled?white:muted,.4f);
    }
    void icon(int which,float x,float y,Color c){
        auto& g=*graphics;
        if(which==0){g.triangle(x,y+11,x+12,y,x+24,y+11,c);g.fill({x+4,y+10,16,14},c);g.fill({x+10,y+15,5,9},{.04f,.01f,.01f,1});}
        if(which==1){for(int i=0;i<8;++i){float a=float(i)*3.14159265f/4;g.line(x+12+std::cos(a)*6,y+12+std::sin(a)*6,x+12+std::cos(a)*12,y+12+std::sin(a)*12,5,c);}g.fill({x+5,y+5,14,14},c);g.fill({x+9,y+9,6,6},{.015f,.02f,.035f,1});}
        if(which==2){g.line(x+2,y+6,x+9,y+6,2,c);g.line(x+9,y+6,x+11,y+9,2,c);g.rounded({x+1,y+8,22,14},2,c,2);}
        if(which==3){g.rounded({x,y+6,24,13},6,c,2);g.line(x+4.5f,y+12.5f,x+10.5f,y+12.5f,2,c);g.line(x+7.5f,y+9.5f,x+7.5f,y+15.5f,2,c);g.rounded({x+15,y+9,3.5f,3.5f},1.7f,c);g.rounded({x+18,y+12.5f,3.5f,3.5f},1.7f,c);}
        if(which==4){g.line(x+12,y+2,x+12,y+21,2,c);g.line(x+5,y+22,x+19,y+22,2,c);g.line(x+3,y+6,x+21,y+6,2,c);
                     for(float p:{3.f,21.f}){g.line(x+p,y+6,x+p-4,y+14,1.5f,c);g.line(x+p,y+6,x+p+4,y+14,1.5f,c);g.line(x+p-4,y+14,x+p+4,y+14,1.5f,c);}}
        if(which==5){for(int k=0;k<3;++k){const float yy=y+5+float(k)*7;g.line(x+2,yy,x+22,yy,2,c);g.rounded({x+4+float((k*7)%3)*6,yy-3,6,6},3,c);}}
        if(which==6)g.triangle(x+6,y+4,x+20,y+12,x+6,y+20,c);
        if(which==7){g.rounded({x+1,y+3,22,14},2,c,2);g.line(x+12,y+17,x+12,y+21,2,c);g.line(x+7,y+21,x+17,y+21,2,c);}
        if(which==8){g.rounded({x+4,y+1,16,22},2,c,2);g.line(x+8,y+8,x+16,y+8,1.5f,c);g.line(x+8,y+12,x+16,y+12,1.5f,c);g.line(x+8,y+16,x+14,y+16,1.5f,c);}
        if(which==9){g.line(x+9,y+3,x+9,y+18,2,c);g.line(x+9,y+3,x+20,y+1,2,c);g.line(x+20,y+1,x+20,y+15,2,c);g.rounded({x+3,y+15,8,6},3,c);g.rounded({x+14,y+13,8,6},3,c);}
        if(which==10){g.rounded({x+4,y+1,16,22},2,c,2);g.fill({x+7,y+5,10,7},c);g.line(x+4,y+15,x+20,y+15,1.5f,c);}
    }
    // Generic arcade cabinet silhouette drawn in code (no marquee text, logo or artwork).
    void miniCabinet(Rect r,Color hue,float light){
        auto& g=*graphics;const float x=r.x,y=r.y,w=r.w,h=r.h;auto tone=[&](float k){return Color{hue.r*k*light,hue.g*k*light,hue.b*k*light,1};};
        g.quad(0,{x-w*.4f,y+h*.1f,w*1.8f,h*.9f},{0,0,1,1},{hue.r,hue.g,hue.b,.10f*light},5);
        g.fill({x+w*.10f,y+h*.04f,w*.80f,h*.94f},{.030f,.026f,.030f,1});
        g.fill({x+w*.04f,y,w*.08f,h},tone(.42f));g.fill({x+w*.88f,y,w*.08f,h},tone(.42f));
        g.fill({x+w*.12f,y+h*.04f,w*.76f,h*.11f},tone(.55f));
        const Rect screen{x+w*.18f,y+h*.20f,w*.64f,h*.25f};g.fill(screen,{.012f,.010f,.012f,1});g.quad(0,screen,{0,0,1,1},{hue.r,hue.g,hue.b,.35f*light},5);
        g.triangle(x+w*.06f,y+h*.60f,x+w*.18f,y+h*.50f,x+w*.82f,y+h*.50f,tone(.30f));g.triangle(x+w*.06f,y+h*.60f,x+w*.82f,y+h*.50f,x+w*.94f,y+h*.60f,tone(.30f));
        for(int b=0;b<3;++b)g.rounded({x+w*(.30f+.16f*float(b)),y+h*.535f,w*.07f,w*.07f},w*.035f,tone(.9f));
        g.fill({x+w*.12f,y+h*.60f,w*.76f,h*.38f},{.036f,.030f,.034f,1});
        g.fill({x+w*.40f,y+h*.70f,w*.20f,h*.07f},tone(.6f));
    }
    void titleBar(){
        auto& g=*graphics;const float w=layoutWidth();
        g.fill({0,0,w,34},{.018f,.017f,.020f,1});g.line(0,34,w,34,1,{.12f,.11f,.13f,1});
        g.rounded({12,7,20,20},4,red);center(L"T",22,9,13,white);
        g.text(std::wstring(productName)+L" Setup",42,9,14,white,.2f);
        g.line(w-77,17,w-63,17,1.5f,hover==12?white:muted);
        const Color cross=hover==13?redHot:muted;g.line(w-31,10,w-17,24,1.5f,cross);g.line(w-17,10,w-31,24,1.5f,cross);
    }
    void header(){
        auto& g=*graphics;const float w=layoutWidth();
        g.fill({0,34,w,90},{.030f,.026f,.030f,1});
        // Generic cabinet silhouettes and a red glow at the right of the header.
        g.quad(0,{w*.45f,20,w*.6f,140},{0,0,1,1},{.9f,.08f,.06f,.22f},5);
        for(int k=0;k<5;++k){const float x=w*.55f+float(k)*w*.085f;miniCabinet({x,44,38,78},red,.55f+.1f*float(k%2));}
        for(int k=0;k<3;++k){const float x=40+float(k)*20;g.triangle(x+16,52,x+25,52,x+9,106,red);g.triangle(x+9,106,x,106,x+16,52,red);}
        g.text(std::wstring(productName)+L" Setup",128,46,34,white,.6f);
        g.text(L"Visual setup for local static recompilation from your own ROM files",130,94,15,muted,.3f);
        g.line(0,headerBottom,w,headerBottom,1,{.30f,.05f,.06f,1});
    }
    void sidebar(){
        auto& g=*graphics;
        g.fill({0,headerBottom,210,footerTop-headerBottom},{.036f,.034f,.039f,1});g.line(210,headerBottom,210,footerTop,1,{.12f,.11f,.13f,1});
        for(int k=0;k<6;++k){
            const Rect box=navBox(k);const int id=navIds[size_t(k)];
            const bool current=k==4?settings:!settings&&page==(k==0?pageOverview:k==1?pageRoms:k==2?pageBuild:k==3?pagePlay:pageLegal);
            const bool enabled=id!=11||settingsEnabled(),focused=drawnFocus==id;
            controls.push_back({id,box,enabled});
            if(current){g.rounded(box,8,{.22f,.03f,.04f,1});g.rounded(box,8,red,1.6f);}
            else if(hover==id&&enabled)g.rounded(box,8,{.09f,.085f,.095f,1});
            if(focused)g.rounded(box,8,redHot,2);
            icon(navIcons[size_t(k)],box.x+14,box.y+12,current?white:enabled?Color{.80f,.80f,.84f,1}:muted);
            g.text(navLabels[size_t(k)],box.x+52,box.y+14,18,enabled?white:muted,.3f);
        }
        float y=footerTop-118;
        for(const wchar_t* hint:{L"Arrows / D-pad: move",L"Enter / A: select",L"PgUp PgDn / LB RB: pages",L"Esc / B: back"}){g.text(hint,22,y,11,muted,.2f);y+=22;}
    }
    void footer(){
        auto& g=*graphics;const float w=layoutWidth();const Rect f{16,footerTop,w-32,44};
        g.rounded(f,8,{.040f,.034f,.038f,1});g.rounded(f,8,{.38f,.05f,.06f,1},1);
        icon(4,f.x+16,f.y+10,white);g.text(L"Legal",f.x+50,f.y+13,16,white,.3f);g.line(f.x+108,f.y+10,f.x+108,f.y+34,1,{.35f,.33f,.36f,1});
        // Legal notice, always visible (launcher_legal.hpp).
        g.text(fit(std::wstring(legalNotice[0])+L" "+legalNotice[1],12,f.w-140),f.x+122,f.y+7,12,{.80f,.80f,.84f,1},.1f);
        g.text(fit(legalNotice[2],12,f.w-140),f.x+122,f.y+24,12,{.62f,.62f,.67f,1},.1f);
    }
    void noticeBar(){
        if(notice.empty())return;
        auto& g=*graphics;const Rect r{contentLeft,928,contentRight()-contentLeft,32};
        g.rounded(r,7,{.05f,.045f,.05f,.97f});g.rounded(r,7,{.45f,.30f,.10f,1},1);
        center(fit(notice,13,r.w-30),r.x+r.w/2,r.y+9,13,gold,.1f);
    }
    void drawButtons(){
        auto& g=*graphics;
        for(const auto& b:pageButtons()){
            if(b.style==styleHidden){controls.push_back({b.id,b.box,b.enabled});continue;}
            if(b.style==styleField){
                controls.push_back({b.id,b.box,b.enabled});const bool over=hover==b.id&&b.enabled,focused=drawnFocus==b.id;
                g.rounded(b.box,6,fieldFill);g.rounded(b.box,6,focused||over?redHot:panelEdge,focused?2.f:1.f);
                const bool empty=b.label.empty();const float size=b.box.h>30?14.f:13.f;
                g.text(fit(empty?L"Choose folder...":b.label,size,b.box.w-20,!empty),b.box.x+10,b.box.y+(b.box.h-size*1.15f)/2,size,empty?muted:white,.1f);
                continue;
            }
            if(b.style==styleCheck){
                controls.push_back({b.id,b.box,b.enabled});const bool on=backend.romsAcknowledged(),focused=drawnFocus==b.id||hover==b.id;
                g.rounded(b.box,8,on?Color{.16f,.03f,.04f,1}:fieldFill);g.rounded(b.box,8,focused?redHot:on?red:panelEdge,focused?2.f:1.2f);
                const Rect box{b.box.x+14,b.box.y+11,22,22};g.rounded(box,4,on?red:Color{.10f,.10f,.11f,1});g.rounded(box,4,on?redHot:muted,1.5f);
                if(on){g.line(box.x+5,box.y+11,box.x+9,box.y+16,2.5f,white);g.line(box.x+9,box.y+16,box.x+17,box.y+6,2.5f,white);}
                g.text(fit(b.label,16,b.box.w-70),b.box.x+50,b.box.y+13,16,white,.2f);continue;
            }
            const bool primary=b.style==stylePrimary;
            control(b.id,b.box,b.label,b.enabled,primary,b.box.h>=40?16.f:14.f);
            if(primary&&(b.id>=70&&b.id<=72)&&b.enabled)g.triangle(b.box.x+14,b.box.y+b.box.h/2-6,b.box.x+24,b.box.y+b.box.h/2,b.box.x+14,b.box.y+b.box.h/2+6,white);
        }
    }
    void stepper(Rect r,int current){
        auto& g=*graphics;panel(r,8);const float sw=r.w/6;
        for(int k=0;k<6;++k){
            const float sx=r.x+float(k)*sw;const bool done=k<current,now=k==current;
            if(now){const Color fill{.70f,.06f,.07f,1};g.rounded({sx+3,r.y+3,sw-14,r.h-6},6,fill);if(k<5)g.triangle(sx+sw-12,r.y+3,sx+sw+5,r.y+r.h/2,sx+sw-12,r.y+r.h-3,fill);}
            if(k<5&&!now){g.line(sx+sw-8,r.y+7,sx+sw+5,r.y+r.h/2,1.5f,panelEdge);g.line(sx+sw+5,r.y+r.h/2,sx+sw-8,r.y+r.h-7,1.5f,panelEdge);}
            const Rect ring{sx+14,r.y+r.h/2-14,28,28};g.rounded(ring,14,now?white:done?red:muted,2);
            center(std::to_wstring(k+1),ring.x+14,ring.y+6,15,now||done?white:muted);
            g.text(fit(stepLabels[size_t(k)],16,sw-64),sx+50,r.y+r.h/2-9,16,now||done?white:muted,.2f);
        }
    }
    void requirementLine(float x,float y,const std::wstring& text,int state,const std::wstring& note=L""){
        auto& g=*graphics;const Color dot=state==1?red:state==2?gold:muted;
        g.rounded({x,y+5,9,9},4.5f,dot);g.text(text,x+22,y,16,white,.2f);
        if(!note.empty())g.text(note,x+22,y+20,12,state==2?gold:muted,.1f);
    }
    void drawOverview(){
        auto& g=*graphics;const float x1=contentRight();
        stepper({contentLeft,140,x1-contentLeft,48},setupStep(stepGame()));
        icon(10,contentLeft+2,212,white);g.text(L"Supported Games",contentLeft+40,206,26,white,.5f);
        const std::wstring showing=L"Steps shown for: "+std::wstring(compatibleGames[size_t(stepGame())].title);
        g.text(showing,sidePanel().x-16-g.textWidth(showing,13,.1f),218,13,muted,.1f);
        for(int i=0;i<3;++i)gameCard(i);
        // Requirements, output and receipt.
        const Rect s=sidePanel();panel(s);const auto& r=backend.setupRequirements();float y=s.y+18;const float x=s.x+22;
        icon(7,x,y+2,white);g.text(L"Requirements",x+38,y,22,white,.4f);y+=44;
        requirementLine(x,y,L"Windows (10 or 11, 64-bit)",1);y+=30;
        requirementLine(x,y,L"DirectX 12 GPU",1);y+=30;
        const bool exact=!r.compiler.empty()&&r.compiler==r.validated;
        requirementLine(x,y,L"MSYS2 MinGW-w64 g++ 16.1.0",!r.checked?0:exact?1:2,!r.checked?L"checking...":r.compiler.empty()?L"not found at C:/msys64/mingw64/bin":exact?L"":L"found "+widen(r.compiler)+L" (not the validated version)");y+=r.checked&&exact?30:46;
        requirementLine(x,y,L"Python 3.12 or later",!r.checked?0:pythonMissing()?2:1,!r.checked?L"checking...":r.python.empty()?L"not found on PATH":L"found "+widen(r.python)+(r.pythonSupported?L"":L" (too old)"));y+=46;
        g.line(x,y,s.x+s.w-22,y,1,panelEdge);y+=14;
        icon(2,x,y+2,white);g.text(L"Output",x+38,y,22,white,.4f);y+=40;
        requirementLine(x,y,L"generated/<game>/<sha256>/",1);y+=28;requirementLine(x,y,L"roms/<game>/",1);y+=36;
        g.line(x,y,s.x+s.w-22,y,1,panelEdge);y+=14;
        icon(8,x,y+2,white);g.text(L"Receipt",x+38,y,22,white,.4f);y+=40;
        requirementLine(x,y,L"setup.json",1);y+=28;requirementLine(x,y,L"logs/setup-<game>.log",1);
        // Launcher modes and custom audio.
        const float half=(x1-contentLeft-16)/2;const Rect modes{contentLeft,684,half,236},audio{contentLeft+half+16,684,half,236};
        panel(modes);icon(3,modes.x+20,modes.y+20,white);g.text(L"Launcher Modes",modes.x+58,modes.y+16,22,white,.4f);
        const float col=(modes.w-60)/2;
        const std::array<std::pair<const wchar_t*,const wchar_t*>,2> mode{{
            {L"Arcade Original",L"The board's audio and image exactly as the arcade produces them. Video filters stay optional."},
            {L"Remastered",L"Scaling, CRT presets, ScaleFX, CAS, SMAA, bilateral filter, a CRT TV frame drawn in code and your own music (T-Unit games)."}}};
        for(size_t k=0;k<2;++k){const float mx0=modes.x+24+float(k)*(col+20);g.fill({mx0,modes.y+68,3,30},red);g.text(mode[k].first,mx0+16,modes.y+68,20,white,.3f);paragraph(mode[k].second,mx0+16,modes.y+102,14,col-20,muted);}
        panel(audio);icon(9,audio.x+20,audio.y+18,white);g.text(L"Custom Audio & Tests",audio.x+58,audio.y+16,22,white,.4f);
        float ay=audio.y+62;
        for(const wchar_t* line:{L"Your music: custom_audio/<game>/music-NN.wav",L"WAV PCM 16-bit, 48000 Hz, mono or stereo",L"Optional slots.json: cuts, loops and gain"}){
            g.rounded({audio.x+26,ay+6,8,8},4,red);g.text(fit(line,15,audio.w-70),audio.x+46,ay,15,white,.1f);ay+=30;}
        g.rounded({audio.x+26,ay+6,8,8},4,red);g.text(L"Run tests:",audio.x+46,ay,15,white,.1f);
        const Rect code{audio.x+136,ay-6,std::min(audio.w-160,330.f),30};g.rounded(code,5,fieldFill);g.rounded(code,5,panelEdge,1);
        g.text(fit(L"python tools/quality/run.py fast",14,code.w-16),code.x+10,code.y+7,14,white,.1f);
    }
    void gameCard(int i){
        auto& g=*graphics;const Rect r=cardRect(i);
        panel(r);miniCabinet({r.x+14,r.y+16,70,r.h-32},red,1.f);
        g.line(r.x+92,r.y+14,r.x+92,r.y+r.h-14,1,panelEdge);
        const float tx=r.x+104;const auto label=badge(i);const float bw=g.textWidth(label,13,.2f)+26;
        const Rect bb{r.x+r.w-bw-14,r.y+16,bw,28};const bool readyBadge=label==L"Ready",failed=label==L"Failed";
        g.rounded(bb,6,readyBadge?Color{.70f,.06f,.07f,1}:Color{.18f,.03f,.04f,1});
        g.rounded(bb,6,failed?gold:red,1.2f);center(label,bb.x+bw/2,bb.y+7,13,failed?gold:white,.2f);
        g.text(fit(compatibleGames[size_t(i)].title,22,bb.x-tx-10),tx,r.y+16,22,white,.3f);
        g.text(L"Revision:",tx,r.y+50,14,muted);g.text(compatibleGames[size_t(i)].revision,tx+82,r.y+50,14,white);
        g.text(L"Board:",tx,r.y+72,14,muted);g.text(compatibleGames[size_t(i)].board,tx+82,r.y+72,14,white);
        icon(2,tx,r.y+101,muted);g.text(L"ROM Folder:",tx+30,r.y+104,13,muted);
        const Rect manifest{folderField(r).x,r.y+132,folderField(r).w,28};
        icon(8,tx,r.y+134,muted);g.text(L"Manifest:",tx+30,r.y+138,13,muted);
        g.rounded(manifest,6,fieldFill);g.rounded(manifest,6,panelEdge,1);g.text(fit(compatibleGames[size_t(i)].manifest,13,manifest.w-20),manifest.x+10,manifest.y+7,13,white,.1f);
        float cx=tx;const auto& s=backend.setupGame(i);const bool verified=s.verified||records[size_t(i)].finished;
        for(const auto& check:manifestChecks[size_t(i)]){
            const float cw=g.textWidth(check,12,.1f)+16;if(cx+cw>r.x+r.w-122)break;
            const Rect chip{cx,r.y+r.h-44,cw,28};g.rounded(chip,14,verified?Color{.16f,.03f,.04f,1}:fieldFill);g.rounded(chip,14,verified?red:panelEdge,1.2f);
            center(check,chip.x+cw/2,chip.y+8,12,verified?white:muted,.1f);cx+=cw+5;
        }
    }
    void drawRoms(){
        auto& g=*graphics;const float x1=contentRight();
        g.text(L"ROM Setup",contentLeft,140,28,white,.5f);
        paragraph(L"Choose the folder that holds the ROM files of your own board. Every file is checked against the game's manifest by size and digests; nothing is downloaded.",contentLeft,180,15,x1-contentLeft-360,muted);
        if(!backend.romsAcknowledged()){
            const Rect banner{contentLeft,232,x1-contentLeft,48};g.rounded(banner,8,{.10f,.07f,.03f,1});g.rounded(banner,8,gold,1.2f);
            g.text(L"Confirm on the Legal page that you own the original board / ROM files before verifying or building.",banner.x+18,banner.y+16,15,gold,.1f);
        }
        for(int i=0;i<3;++i){
            const Rect r=romRow(i);const auto& s=backend.setupGame(i);panel(r);
            if(reportGame==i)g.rounded(r,10,red,1.6f);
            g.text(fit(compatibleGames[size_t(i)].title,20,280),r.x+20,r.y+14,20,white,.3f);
            g.text(std::wstring(compatibleGames[size_t(i)].revision)+L"  /  "+compatibleGames[size_t(i)].board,r.x+20,r.y+46,13,muted,.1f);
            size_t ok=0;for(const auto& f:s.files)ok+=f.second=="ok";
            const std::wstring summary=s.active&&s.mode=="verify"?L"Checking the files...":s.files.empty()?(records[size_t(i)].finished?L"Verified by the last finished setup.":L"Not verified yet."):
                std::to_wstring(ok)+L" of "+std::to_wstring(s.files.size())+L" files OK  /  program image "+(s.program=="ok"?L"OK":s.program=="not checked"?L"not checked":L"differs");
            g.text(summary,r.x+320,r.y+54,13,s.files.empty()||s.verified?muted:gold,.1f);
        }
        const Rect report{contentLeft,576,x1-contentLeft,344};panel(report);
        const auto& s=backend.setupGame(reportGame);
        g.text(L"Verification report: "+std::wstring(compatibleGames[size_t(reportGame)].title),report.x+20,report.y+16,20,white,.3f);
        std::wstring checks=L"Checks: ";for(size_t k=0;k<manifestChecks[size_t(reportGame)].size();++k)checks+=(k?L", ":L"")+manifestChecks[size_t(reportGame)][k];
        g.text(checks,report.x+report.w-20-g.textWidth(checks,13,.1f),report.y+22,13,muted,.1f);
        if(s.files.empty()){paragraph(L"Run Verify to check each file of this game. The report lists every file of the manifest with its result; nothing is written.",report.x+20,report.y+70,15,report.w-40,muted);return;}
        const float colW=(report.w-40)/3;
        for(size_t k=0;k<s.files.size();++k){
            const float fx=report.x+20+float(k/9)*colW,fy=report.y+56+float(k%9)*26;const bool ok=s.files[k].second=="ok";
            g.rounded({fx,fy+5,9,9},4.5f,ok?red:gold);g.text(fit(widen(s.files[k].first),14,colW-110),fx+20,fy,14,white,.1f);
            const std::wstring status=ok?L"OK":s.files[k].second=="missing"?L"MISSING":s.files[k].second=="link"?L"LINK":checkLabel(s.files[k].second)+L" differs";
            g.text(status,fx+colW-100,fy,13,ok?muted:gold,.1f);
        }
        g.text(L"Program image: "+(s.program=="ok"?std::wstring(L"OK"):widen(s.program)),report.x+20,report.y+report.h-34,14,s.program=="ok"?white:gold,.1f);
    }
    void drawBuild(){
        auto& g=*graphics;const float x1=contentRight();const auto& req=backend.setupRequirements();
        g.text(L"Build",contentLeft,140,28,white,.5f);
        paragraph(L"Generates each game's C++ from your ROM files and compiles it on this machine, one game at a time.",contentLeft,180,15,x1-contentLeft-440,muted);
        const std::wstring tools=!req.checked?L"Checking Python and the compiler...":
            (req.python.empty()?L"Python 3 not found on PATH":L"Python "+widen(req.python))+L"  /  "+
            (req.compiler.empty()?L"g++ not found":L"g++ "+widen(req.compiler)+(req.compiler==req.validated?L" (validated)":L" (not the validated "+widen(req.validated)+L")"));
        g.text(tools,contentLeft,204,13,req.checked&&(req.python.empty()||req.compiler.empty())?gold:muted,.1f);
        const double t=launcher.reducedMotion?0:double(frameTicks()-started)/ticksPerSecond;
        for(int i=0;i<3;++i){
            const Rect r=buildRow(i);const auto& s=backend.setupGame(i);const auto& rec=records[size_t(i)];panel(r);
            if(s.active)g.rounded(r,10,red,1.6f);
            g.text(fit(compatibleGames[size_t(i)].title,22,r.w-460),r.x+20,r.y+16,22,white,.3f);
            const auto label=badge(i);const float bw=g.textWidth(label,13,.2f)+26;const Rect bb{r.x+20+g.textWidth(compatibleGames[size_t(i)].title,22,.3f)+16,r.y+17,bw,28};
            g.rounded(bb,6,label==L"Ready"?Color{.70f,.06f,.07f,1}:Color{.18f,.03f,.04f,1});g.rounded(bb,6,label==L"Failed"?gold:red,1.2f);center(label,bb.x+bw/2,bb.y+7,13,label==L"Failed"?gold:white,.2f);
            std::wstring line;Color lineColor=muted;float fraction=0;bool sweep=false;
            const int step=stepOf(s.step);
            if(s.active&&s.mode=="setup"){
                fraction=float(step-1)/4;if(step==4&&s.unitsTotal>0)fraction+=.25f*float(s.unitsDone)/float(s.unitsTotal)*.8f;fraction=std::max(fraction,.03f);
                line=L"Step: "+std::wstring(stepLabels[size_t(step)])+L" ("+widen(s.step)+L")";
                if(s.unitsTotal>0)line+=L"  /  units "+std::to_wstring(s.unitsDone)+L" of "+std::to_wstring(s.unitsTotal);
                line+=L"  /  elapsed "+clockText(s.elapsed);if(s.estimate>0)line+=L"  /  last run "+clockText(s.estimate);lineColor=white;
            }else if(s.active){sweep=true;line=L"Checking the ROM files...  /  elapsed "+clockText(s.elapsed);lineColor=white;}
            else if(s.queued)line=L"Queued: starts after the current game.";
            else if(s.state=="failed"){fraction=float(step-1)/4;line=L"Failed at "+std::wstring(stepLabels[size_t(step)])+L": "+backend.failureText(i);lineColor=gold;}
            else if(s.state=="cancelled")line=L"Cancelled. Build again to restart from the first step.";
            else if(rec.finished){fraction=1;line=std::wstring(ready(i)?L"Ready to play":L"Built; the launcher package for this build is not installed")+L"  /  executable SHA-256 "+widen(rec.executableSha256.substr(0,16))+L"...  /  setup time "+clockText(rec.seconds);}
            else if(rec.outdated)line=L"Built by an older version of TriUnit Recomp: build it again to update this game.";
            else line=s.folder.empty()?L"Choose the ROM folder on the ROM Setup page first.":L"Not built yet.";
            g.text(fit(line,15,r.w-240),r.x+20,r.y+58,15,lineColor,.1f);
            const Rect bar{r.x+20,r.y+92,r.w-240,14};g.rounded(bar,7,fieldFill);g.rounded(bar,7,panelEdge,1);
            if(sweep){const float p=float(std::fmod(t*.6,1.0));g.rounded({bar.x+(bar.w-bar.w*.25f)*p,bar.y,bar.w*.25f,bar.h},7,red);}
            else if(fraction>0)g.rounded({bar.x,bar.y,bar.w*std::min(fraction,1.f),bar.h},7,s.state=="failed"?gold:red);
            float sx=bar.x;
            for(int k=1;k<6;++k){
                const bool reached=rec.finished&&!s.active&&s.state!="failed"?true:s.active&&s.mode=="setup"?k<=step:false;
                const std::wstring name=stepLabels[size_t(k)];g.text(name,sx,r.y+116,13,reached?white:muted,.1f);sx+=g.textWidth(name,13,.1f)+12;
                if(k<5){g.text(L">",sx,r.y+116,13,muted);sx+=18;}
            }
            if(s.state=="failed"&&!s.active&&!s.queued)g.text(fit(backend.failureHint(i),14,r.w-240),r.x+20,r.y+138,14,white,.1f);
        }
    }
    void drawLegal(){
        auto& g=*graphics;const float x1=contentRight();
        g.text(L"Legal",contentLeft,140,28,white,.5f);
        const Rect p{contentLeft,186,x1-contentLeft,734};panel(p);
        float y=p.y+20;const float x=p.x+30,w=p.w-60;
        auto section=[&](const wchar_t* title,const std::wstring& text){g.text(title,x,y,19,white,.3f);y=paragraph(text,x,y+30,15,w,Color{.78f,.78f,.82f,1})+12;};
        g.text(L"Notice",x,y,19,white,.3f);y+=30;for(const auto* line:legalNotice){g.text(fit(line,15,w),x,y,15,Color{.78f,.78f,.82f,1},.1f);y+=22;}y+=12;
        section(L"Your ROM files",L"The setup needs ROM files of your own board, obtained legally. The project includes no game files and does not provide ROM files, links to them, or instructions for obtaining them. Whether a copy is lawful depends on how it was obtained and on the law where you live.");
        section(L"Method",L"Static recompilation done locally from your ROM: the setup reads your files on this machine, generates C++ with the project's generators and compiles it here. Generated code, game executables and CMOS seeds stay on this machine and are never distributed.");
        section(L"Licenses",L"TriUnit Recomp is released under the BSD 3-Clause license. Third-party components and their licenses are listed in THIRD_PARTY_NOTICES.md. Nothing in this application is legal advice.");
        if(!backend.romsAcknowledged())g.text(L"Verify and Build stay disabled until you confirm:",x,766,14,gold,.1f);
    }
    // Start-screen background drawn in code: night gradient, floor and perspective lines.
    void backdrop(float width){
        auto& g=*graphics;
        for(int n=0;n<32;++n){const float k=float(n)/31;g.fill({0,float(n)*32,width,33},{.010f+.020f*k,.013f+.002f*k,.030f-.008f*k,1});}
        g.fill({0,700,width,324},{.010f,.009f,.016f,1});g.line(0,700,width,700,1,{.30f,.07f,.09f,.8f});
        for(int n=-6;n<=18;++n){const float x=width*float(n)/12;g.line(width/2,560,x,1024,1,{.12f,.16f,.30f,.25f});}
        for(int n=0;n<6;++n){const float y=700+float(n*n)*9;g.line(0,y,width,y,1,{.10f,.13f,.24f,.22f});}
    }
    const wchar_t* missing(int game)const{return backend.setupFinished(game)?L"INSTALL PENDING":L"SETUP PENDING";}
    static Rect cabinetScreen(Rect r){return {r.x+r.w*.14f,r.y+168,r.w*.72f,152};}
    // Arcade cabinet drawn in code: body, side trims, marquee with the board and revision, screen bezel, panel and base.
    void cabinet(int i,Rect r,Color hue,float shade){
        auto& g=*graphics;auto tone=[&](float v){return Color{v*shade,v*shade*1.03f,v*shade*1.15f,1};};
        const float cx=r.x+r.w/2;
        g.fill({r.x+r.w*.06f,r.y+40,r.w*.88f,r.h-82},tone(.060f));
        g.fill({r.x+r.w*.02f,r.y+40,r.w*.05f,r.h-82},tone(.095f));g.fill({r.x+r.w*.93f,r.y+40,r.w*.05f,r.h-82},tone(.095f));
        const Rect marquee{r.x+r.w*.08f,r.y+52,r.w*.84f,72};
        g.fill(marquee,{hue.r*.28f*shade,hue.g*.28f*shade,hue.b*.28f*shade,1});
        g.line(marquee.x,marquee.y+marquee.h,marquee.x+marquee.w,marquee.y+marquee.h,2,{hue.r,hue.g,hue.b,.85f});
        const std::wstring board=compatibleGames[size_t(i)].marquee;
        const float size=std::min(17.f,marquee.w*.9f/std::max(1.f,g.textWidth(board,1,.6f)));
        center(board,cx,marquee.y+(marquee.h-size*1.15f)/2,size,white,.6f);
        g.fill({r.x+r.w*.10f,r.y+150,r.w*.80f,188},tone(.022f));
        // The game's name only as plain compatibility text (nominative use), smaller than the marquee.
        const auto& game=compatibleGames[size_t(i)];
        center(L"COMPATIBLE WITH",cx,r.y+348,9,muted,1);
        center(fit(std::wstring(game.title)+L" (rev. "+game.revision+L")",11,r.w*.84f),cx,r.y+366,11,{.78f,.78f,.82f,1},.2f);
        const Rect panelRect{r.x+r.w*.03f,r.y+398,r.w*.94f,66};
        g.triangle(panelRect.x,panelRect.y+panelRect.h,panelRect.x+panelRect.w*.08f,panelRect.y,panelRect.x+panelRect.w*.92f,panelRect.y,tone(.11f));
        g.triangle(panelRect.x,panelRect.y+panelRect.h,panelRect.x+panelRect.w*.92f,panelRect.y,panelRect.x+panelRect.w,panelRect.y+panelRect.h,tone(.11f));
        for(int b=0;b<6;++b)g.fill({panelRect.x+panelRect.w*(.20f+.11f*float(b)),panelRect.y+28,10,10},{hue.r,hue.g,hue.b,.85f});
        g.fill({r.x+r.w*.08f,r.y+464,r.w*.84f,150},tone(.045f));
        g.fill({cx-26,r.y+512,52,34},tone(.018f));g.fill({cx-4,r.y+520,8,18},{hue.r*.6f,hue.g*.6f,hue.b*.6f,1});
        g.fill({r.x+r.w*.04f,r.y+614,r.w*.92f,18},tone(.03f));
    }
    void scene(float dt,bool deterministic){
        auto& g=*graphics;float t=deterministic||launcher.reducedMotion?0:float(frameTicks()-started)/ticksPerSecond;
        const float width=layoutWidth();
        backdrop(width);
        if(!launcher.reducedMotion){
            for(float anchor:{642.f,1097.f}){float x=anchor*width/baseWidth,y=anchor==1097?250.f:365.f;g.quad(0,{x-35,y+10,150,220},{0,0,1,1},{1,.24f,.02f,.12f+.025f*std::sin(t*2+x)},5,t);g.quad(0,{x,y,95,140},{0,0,1,1},{1,1,1,.38f},1,t+x);}
            g.quad(0,{0,520,width,280},{0,0,1,1},{.17f,.18f,.45f,.18f},2,t);
            for(int i=0;i<22;++i){float x=std::fmod(float(i*173)+t*(3+i%4),width),y=790-std::fmod(float(i*47)+t*(12+i%9),550.f);float a=.12f+.13f*std::sin(float(i)+t);g.fill({x,y,1.5f,1.5f},{1,.35f,.09f,a});}
        }
        for(int i=0;i<3;++i){
            float target=!settings&&!details&&(chosen==i||hover==i+1||focus==i)?1.f:0.f;glows[size_t(i)]+=std::clamp(dt*7,0.f,1.f)*(target-glows[size_t(i)]);float glow=deterministic?0:glows[size_t(i)];
            Rect r=positions[size_t(i)];const float shift=cabinetShift(i);r.x+=shift;float lift=launcher.reducedMotion?0:glow*3;r.y-=lift;
            Color hue=i<2?red:blue;
            g.quad(0,{r.x-55,660,r.w+110,235},{0,0,1,1},{hue.r,hue.g,hue.b,.13f+glow*.12f},5,t);
            // Cabinet drawn in code (no image file): body, board marquee, compatibility line, screen, panel and base.
            float shade=i==0?1.f:.71f;shade+=glow*.17f;cabinet(i,r,hue,shade);
            const Rect screen=cabinetScreen(r);g.fill(screen,{.012f,.018f,.03f,1});g.quad(0,screen,{0,0,1,1},{.5f,.7f,1,1},4,t);
            center(ready(i)?L"READY TO PLAY":backend.setupFinished(i)?L"INSTALL PENDING":L"SETUP PENDING",screen.x+screen.w/2,screen.y+screen.h/2-8,9,ready(i)?gold:muted,1.4f);
            if(glow>.01f){g.line(r.x+20,831-lift,r.x+r.w-15,831-lift,2,{hue.r,hue.g,hue.b,glow*.8f});}
            if(!settings&&!details){controls.push_back({i+1,{r.x,r.y+15,r.w,680},!backend.running()});if(chosen==i)g.line(r.x+20,831-lift,r.x+r.w-15,831-lift,3,gold);}
            Rect badgeRect{r.x+30,848,r.w-60,28};g.fill(badgeRect,{.006f,.009f,.022f,.85f});center(i==0?(mk1Available?(mk1Remastered?L"REMASTERED  /  AVAILABLE":L"ORIGINAL ARCADE  /  AVAILABLE"):missing(0)):i==1?(mk2Available?(mk2Remastered?L"REMASTERED  /  AVAILABLE":L"ORIGINAL ARCADE  /  AVAILABLE"):missing(1)):ready(2)?mk3Edition():missing(2),badgeRect.x+badgeRect.w/2,badgeRect.y+6,10,gold,1.3f);
        }
        if(!deterministic&&!launcher.reducedMotion){float fade=1-t/.65f;if(fade>0&&!receivedInput)g.fill({0,headerBottom,width,footerTop-headerBottom},{0,0,0,std::clamp(fade,0.f,1.f)});}
        // Selected game, its edition and Play.
        const Rect bar{contentLeft,884,contentRight()-contentLeft,62};g.rounded(bar,10,{.03f,.03f,.04f,.92f});g.rounded(bar,10,panelEdge,1);
        if(chosen<0&&notice.empty())g.text(L"Select a cabinet, then press PLAY. Enter / A on a selected cabinet switches Original Arcade and Remastered.",bar.x+20,bar.y+22,15,muted,.1f);
        else if(chosen>=0){
            g.text(std::wstring(compatibleGames[size_t(chosen)].title)+L"  /  ",bar.x+20,bar.y+10,18,white,.3f);
            g.text(chosen==2?mk3Edition():backend.config.remastered?L"REMASTERED":L"ORIGINAL ARCADE",bar.x+28+g.textWidth(std::wstring(compatibleGames[size_t(chosen)].title)+L"  /  ",18,.3f),bar.y+12,15,gold,.8f);
            control(20,rightAligned({1256,888,244,52}),backend.running()?L"RUNNING":gameSelected()?L"PLAY":L"UNAVAILABLE",gameSelected(),true,20);
        }
        if(!notice.empty())g.text(fit(notice,13,bar.w-320),bar.x+20,chosen<0?bar.y+22:bar.y+38,13,gold,.1f);
    }
    void row(int id,float y,const std::wstring& title,const std::wstring& description,const std::wstring& value,bool enabled=true){
        auto& g=*graphics;const float shift=extraWidth()/2;
        g.line(300+shift,y+65,1235+shift,y+65,1,{.13f,.12f,.14f,1});g.text(title,306+shift,y+5,18,enabled?white:muted,.6f);g.text(description,306+shift,y+32,12,muted,.3f);control(id,{969+shift,y,260,54},value,enabled,false,15);
    }
    void settingsPanel(){
        auto& g=*graphics;const float shift=extraWidth()/2;
        const Rect p{240+shift,150,1056,733};g.rounded(p,12,panelFill);g.rounded(p,12,panelEdge,1.2f);
        const auto* videoEdition=videoRemastered?L"REMASTERED":L"ORIGINAL ARCADE";
        const auto* headingEdition=category==0||category==2?videoEdition:(draft.remastered?L"REMASTERED":L"ORIGINAL ARCADE");
        g.text(L"Settings",300+shift,190,31,white,.6f);
        const std::wstring subtitle=category==launcherTab||category==controlsTab
            ?(editingGame?L"COLLECTION PREFERENCES":L"COLLECTION PREFERENCES  /  SELECT A CABINET ON PLAY TO ADJUST A GAME")
            :selected==2?std::wstring(compatibleGames[2].name)+L"  /  "+mk3Edition()
            :std::wstring(compatibleGames[size_t(selected==1)].name)+L"  /  "+headingEdition;
        g.text(subtitle,302+shift,246,12,gold,1.5f);
        for(int i=0;i<tabCount;++i)control(220+i,{300+shift+float(i)*134,294,134,40},std::array<std::wstring,tabCount>{L"VIDEO",L"AUDIO",L"DISPLAY",L"LAUNCHER",L"ADJUSTMENTS",L"UTILITIES",L"CONTROLS"}[size_t(i)],tabEnabled(i),category==i,13);
        if(category==0){
            row(videoProfile,350,L"VIDEO PROFILE",selected==2?L"Choose which profile to edit. The edition is chosen on the cabinet.":L"Choose which profile to edit. The game version is set on the Audio tab.",videoEdition,nativeVideo());
            row(videoPage,410,L"VIDEO CONTROLS",L"Switch between scaling and additional filters.",draft.videoEffects?L"EFFECTS":L"SCALING AND CRT",nativeVideo());
            const auto& v=video();
            if(draft.videoEffects){
                row(255,470,L"SCALEFX",L"Edge reconstruction at 3x.",v.scalefx?L"ON":L"OFF");
                g.text(L"SCALEFX STRENGTH",306+shift,535,18,scaleFxActive()?white:muted,.6f);
                g.text(std::to_wstring(v.scalefxAmount)+L"%",720+shift,535,18,scaleFxActive()?gold:muted,1);
                controls.push_back({256,{969+shift,530,260,54},scaleFxActive()});
                const bool focused=drawnFocus==256;
                const float amount=260.f*v.scalefxAmount/100;
                g.rounded({969+shift,553,260,6},3,{.14f,.13f,.15f,1});
                g.rounded({969+shift,553,amount,6},3,scaleFxActive()?red:muted);
                g.rounded({963+shift+amount,545,12,22},5,scaleFxActive()?(focused?gold:white):muted);
                row(videoCas,590,L"CAS ADAPTIVE SHARPENING",L"Brings out detail in the game image.",v.cas?std::to_wstring(v.cas)+L"%":L"OFF");
                row(videoSmaa,650,L"SMAA 1X",L"Smooths diagonals and high-contrast edges.",v.smaa?L"ON":L"OFF");
                row(videoBilateral,710,L"BILATERAL FILTER",L"Smooths nearby colors in the frame, sprites included.",v.bilateral?std::to_wstring(v.bilateral)+L"%":L"OFF");
            }else{
                constexpr std::array<const wchar_t*,6> scales{L"FIT 4:3",L"INTEGER / AUTO",L"INTEGER / 2X",L"INTEGER / 3X",L"INTEGER / 4X",L"INTEGER LINES 4:3"};
                constexpr std::array<const wchar_t*,4> filters{L"SHARP PIXELS",L"BILINEAR",L"SHARP BILINEAR",L"BICUBIC"};
                row(videoScale,470,L"IMAGE SCALE",L"Integer scaling uses the largest factor that fits when the choice does not.",scales[size_t(v.scaleMode)],nativeVideo());
                row(201,530,L"UPSCALING FILTER",L"How the image is sampled to the output resolution.",filters[size_t(v.filter)]);
                row(200,590,L"CRT AND SCANLINES",L"Choose the beam, mask and lines of the arcade monitor.",crtLabels[size_t(v.scanlines)]);
                row(207,650,L"COLORS",L"Optional output adjustment.",std::array<std::wstring,3>{L"ORIGINAL",L"VIVID",L"WARM"}[size_t(v.color)]);
                row(208,710,L"BRIGHTNESS",L"100% uses the original output gain.",std::to_wstring(v.brightness)+L"%");
            }
        }else if(category==1){
            // D7: the MK3 Remastered edition has no remastered music.
            if(selected==2)row(245,350,L"AUDIO OF THIS GAME",L"Remastered: remastered video, original arcade audio.",L"ORIGINAL ARCADE",false);
            else row(245,350,L"DEFAULT GAME VERSION",L"Choose and SAVE to always start in this version.",draft.remastered?L"REMASTERED":L"ORIGINAL ARCADE",musicAvailable||draft.remastered);
            row(205,420,L"AUDIO QUEUE",selected==2?L"Balanced: 80 ms in both editions. Safe: 140 ms.":L"Balanced: Original 80 ms / Remastered 140 ms. Safe: 140 ms.",std::array<std::wstring,3>{L"LOW / 40 ms",L"BALANCED",L"SAFE / 140 ms"}[size_t(draft.audioLatency)]);
            row(243,490,L"ARCADE VOICES AND EFFECTS",L"Voices, hits and effects preserved in both versions.",L"ORIGINAL",false);
            row(244,560,L"AUDIO RESERVE",L"Margin against system delays; no sample is dropped.",draft.remastered||draft.audioLatency==2?L"140 ms":L"80 ms",false);
            g.text(L"The Windows volume still controls the game output.",306+shift,653,13,muted,.2f);
        }else if(category==2){
            const auto& v=video();
            row(videoProfile,350,L"VIDEO PROFILE",L"Choose which display profile to edit.",videoEdition,nativeVideo());
            row(displayPage,410,L"DISPLAY CONTROLS",L"Switch between sync and window options and the HDR brightness.",displayHdr?L"HDR":L"SYNC AND WINDOW",nativeVideo());
            const auto nits=[](int n){return n?std::to_wstring(n)+L" NITS":std::wstring(L"AUTO");};
            if(displayHdr){
                row(hdrMode,470,L"HDR",L"HDR10 when Windows HDR is on for this monitor; SDR otherwise.",v.hdr?L"AUTO":L"OFF",nativeVideo());
                row(hdrPaper,530,L"PAPER WHITE",L"Brightness of the game's white. Auto follows Windows SDR content brightness.",nits(v.hdrPaper),nativeVideo()&&v.hdr);
                row(hdrPeak,590,L"PEAK BRIGHTNESS",L"Brightest level of the highlights. Auto reads the monitor.",nits(v.hdrPeak),nativeVideo()&&v.hdr);
                row(hdrHighlights,650,L"HDR HIGHLIGHTS",videoRemastered?L"Expands bright colors toward the peak; dark and mid tones stay.":L"Remastered profile only. The Original keeps the SDR image.",
                    v.hdrHighlights?std::to_wstring(v.hdrHighlights)+L"%":L"OFF",nativeVideo()&&v.hdr&&videoRemastered);
            }else{
                row(250,470,L"GAME SYNC",L"VRR needs G-Sync / FreeSync on in the monitor and the driver.",std::array<std::wstring,3>{L"V-SYNC",L"VRR / ADAPTIVE",L"IMMEDIATE"}[size_t(v.sync)]);
                row(251,530,L"HIGH REFRESH",L"Shows the original frames again at the desktop refresh rate.",nativeVideo()?(v.highRefresh?L"ON":L"OFF"):L"UNAVAILABLE",nativeVideo());
                if(widescreenAvailable())row(209,590,L"SCREEN",L"Widescreen shows the arena beyond the arcade's 4:3, without the TV frame.",
                                             v.widescreen?L"WIDESCREEN":v.bezel?L"TV FRAME":L"4:3");
                else row(209,590,L"TV FRAME",L"CRT television around the game. Your own: custom_bezel/<game>/bezel.png.",v.bezel?L"ON":L"OFF");
                row(videoFullscreen,650,L"GAME FULLSCREEN",L"Window mode when this version starts.",v.fullscreen?L"ON":L"WINDOW",nativeVideo());
                row(hdrMode,710,L"HDR",L"HDR10 when Windows HDR is on; brightness on DISPLAY CONTROLS > HDR.",v.hdr?L"AUTO":L"OFF",nativeVideo());
            }
            g.text(L"XInput: "+std::to_wstring(pads.count())+L" connected. Arcade: 54.7069 frames per second.",306+shift,displayHdr?723:783,13,muted,.2f);
        }else if(category==4){
            const bool ready=backend.adjustmentsAvailable(),dips=backend.dipSwitchesAvailable();const auto effective=effectiveAdjustments();auto priced=effective;priced.freePlay=false;
            row(260,350,L"COMPUTER DIFFICULTY",L"The arcade's native AI level.",std::array<std::wstring,5>{L"VERY EASY",L"EASY",L"MEDIUM",L"HARD",L"VERY HARD"}[size_t(std::clamp(draftAdjustments.difficulty,1,5)-1)],ready);
            row(261,420,L"COINAGE",L"Preset / credits to start / continue. Original bonuses and minimums.",backend.coinageLabel(priced),ready);
            row(262,490,L"FREE PLAY",L"Start without coins through the game's own setting.",effective.freePlay?L"ON":L"OFF",ready);
            row(263,560,L"BLOOD",L"The cabinet's original blood DIP switch.",draft.blood?L"ON":L"OFF",dips);
            row(264,630,L"VIOLENCE",L"The cabinet's original violence DIP switch.",draft.violence?L"ON":L"OFF",dips);
            g.text(ready||selected==2?L"SAVE applies the adjustments the next time this game starts.":L"Start the game once to create its profile; CMOS and ROMs must be valid.",306+shift,723,13,muted,.2f);
        }else if(category==5){
            row(270,350,L"VOLUME ADJUST",gameVolumeEnabled()?L"Software volume of this game's music, voices and effects.":L"Per-game control available on the native engine.",gameVolumeEnabled()?std::to_wstring(draft.gameVolume)+L"%":L"UNAVAILABLE",gameVolumeEnabled());
            g.rounded({969+shift,408,260,4},2,{.14f,.13f,.15f,1});g.rounded({969+shift,408,260.f*draft.gameVolume/100,4},2,red);
            row(271,420,L"RESET HIGH SCORES",L"Resets the records through the original routine; keeps the adjustments.",L"RESET RANKING",backend.adjustmentsAvailable());
            if(resetScoresPending){g.text(L"Erase this game's saved ranking?",306+shift,535,16,gold,.2f);control(272,{969+shift,520,125,54},L"CONFIRM",true,false,12);control(273,{1104+shift,520,125,54},L"CANCEL",true,false,12);}
            g.text(L"Clearing the ranking is separate from SAVE and RESTORE DEFAULTS.",306+shift,723,13,muted,.2f);
        }else if(category==controlsTab){
            // Every game: keyboard keys and controller buttons per player. Controller directions: D-pad, left stick or hat.
            g.text(L"ACTION",306+shift,346,12,muted,1);
            for(int k=0;k<4;++k)g.text(std::array<const wchar_t*,4>{L"P1 KEYBOARD",L"P1 CONTROLLER",L"P2 KEYBOARD",L"P2 CONTROLLER"}[size_t(k)],560+shift+float(k)*170,346,12,muted,1);
            for(unsigned a=0;a<mk5controls::Count;++a){
                const float y=370+float(a)*27;g.text(mk5controls::names[a],306+shift,y+5,13,white,.4f);
                if(a<mk5controls::HighPunch)for(int player:{0,1})g.text(L"D-PAD / STICK",730+shift+float(player)*340+10,y+7,11,muted,.6f);
            }
            for(int id:controlCells()){auto box=cellBox(id);box.x+=shift;const auto [player,column,action]=cell(id);
                control(id,box,capture==id?L"PRESS...":column?padName(draftControls.pad[size_t(player)][action]):keyName(draftControls.keys[size_t(player)][action]),true,capture==id,11);}
            g.text(L"OPPOSITE DIRECTIONS (SOCD)",306+shift,703,13,white,.4f);
            g.text(L"An arcade lever cannot hold both: neutral is the original.",560+shift,705,11,muted,.2f);
            control(socdControl,{969+shift,698,260,28},draftControls.socd?L"LAST INPUT WINS":L"NEUTRAL",true,false,12);
            g.text(L"RUN-AHEAD",306+shift,735,13,white,.4f);
            g.text(L"Shows the next frame: one frame less input lag. Off is the arcade.",560+shift,737,11,muted,.2f);
            control(runAheadControl,{969+shift,730,260,28},draftControls.runAhead?L"1 FRAME":L"OFF",true,false,12);
            g.text(L"TRAINING MODE",306+shift,767,13,white,.4f);
            g.text(L"Shows each player's inputs and the game's hit boxes over the image.",560+shift,769,11,muted,.2f);
            control(trainingControl,{969+shift,762,260,28},draftControls.training?L"ON":L"OFF",true,false,12);
            g.text(L"SAVE STATES  In the game: F5 saves, F7 loads. Every game and edition; one slot each, in saves.",306+shift,795,11,muted,.2f);
        }else{
            row(202,350,L"LAUNCHER FPS LIMIT",L"Unfocused: 2 FPS (10 while building). Minimized: paused.",std::to_wstring(draftLauncher.fps)+L" FPS");
            row(203,420,L"LAUNCHER V-SYNC",L"Syncs the launcher with the monitor.",draftLauncher.vsync?L"ON":L"OFF");
            row(204,490,L"REDUCE MOTION",L"Turns off fire, fog, particles and the opening fade.",draftLauncher.reducedMotion?L"ON":L"OFF");
            row(206,560,L"LAUNCHER FULLSCREEN",L"Starts fullscreen. Esc returns to a window; F11 toggles.",draftLauncher.fullscreen?L"ON":L"WINDOW");
            row(230,630,L"TEST LOGS",L"Opens the collection's logs folder.",L"OPEN LOGS");
            g.text(L"MENU MUSIC VOLUME",306+shift,705,18,white,.6f);
            g.text(L"Adjusts only the launcher track (custom_audio/launcher/menu.wav). SAVE to keep it.",306+shift,732,12,muted,.3f);
            auto volumeBox=menuVolumeBox,muteBox=menuMuteBox;volumeBox.x+=shift;muteBox.x+=shift;
            control(menuVolume,volumeBox,std::to_wstring(draftLauncher.musicVolume)+L"%",true,false,15);
            g.rounded({volumeBox.x,746,volumeBox.w,4},2,{.14f,.13f,.15f,1});
            g.rounded({volumeBox.x,746,volumeBox.w*draftLauncher.musicVolume/100,4},2,draftLauncher.musicMuted?muted:red);
            control(menuMute,muteBox,draftLauncher.musicMuted?L"UNMUTE":L"MUTE",true,draftLauncher.musicMuted,12);
        }
        control(212,{300+shift,810,190,50},L"BACK",true,false,14);control(211,{510+shift,810,240,50},L"RESTORE DEFAULTS",true,false,13);control(210,{1000+shift,810,235,50},L"SAVE",true,true,16);
        if(!notice.empty())g.text(notice.substr(0,105),306+shift,769,12,gold,.1f);
    }
    void draw(float dt,bool deterministic=false){
        const auto a=actions();
        drawnFocus=size_t(focus)<a.size()?a[size_t(focus)]:-1;
        auto& g=*graphics;const float width=layoutWidth();
        controls.clear();g.bezel(settings?video().bezel:backend.config.video().bezel);g.begin();
        g.fill({0,0,width,1024},ink);
        if(!settings&&page==pagePlay)scene(dt,deterministic);
        else{
            g.quad(0,{width*.2f,headerBottom,width,900},{0,0,1,1},{.6f,.04f,.05f,.06f},5);
            if(settings)settingsPanel();
            else{
                if(!requirementsAsked&&!deterministic){requirementsAsked=true;backend.checkRequirements();}
                if(page==pageOverview)drawOverview();else if(page==pageRoms)drawRoms();else if(page==pageBuild)drawBuild();else drawLegal();
                drawButtons();noticeBar();
            }
        }
        titleBar();header();sidebar();footer();
        controls.push_back({12,{width-92,0,44,34},true});controls.push_back({13,{width-46,0,44,34},true});
        if(closing){g.fill({0,headerBottom,width,footerTop-headerBottom},{0,0,0,.65f});center(L"CLOSING THE GAME AND SAVING THE PROFILE...",width/2,480,22,white,1);}
        g.end(launcher.vsync);
    }
    static LRESULT CALLBACK procedure(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp){
        auto* self=reinterpret_cast<App*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));if(msg==WM_NCCREATE){self=static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->window=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(hwnd,msg,wp,lp);
        try{
            switch(msg){
            case WM_CLOSE:self->close();return 0;
            case WM_DESTROY:PostQuitMessage(0);return 0;
            case WM_ERASEBKGND:return 1;
            case WM_PAINT:{PAINTSTRUCT ps{};BeginPaint(hwnd,&ps);EndPaint(hwnd,&ps);return 0;}
            case WM_GETMINMAXINFO:{auto* m=reinterpret_cast<MINMAXINFO*>(lp);m->ptMinTrackSize={768,512};return 0;}
            case WM_ACTIVATEAPP:self->active=wp!=0;self->pressed=-1;self->gamepadButtons=0;self->padReady=false;return 0;
            case WM_CAPTURECHANGED:self->pressed=-1;return 0;
            case WM_KILLFOCUS:self->pressed=-1;self->gamepadButtons=0;self->padReady=false;return 0;
            case WM_DPICHANGED:{auto* r=reinterpret_cast<RECT*>(lp);SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
            case WM_NCCALCSIZE:return 0;
            case WM_NCHITTEST:{LRESULT hit=DefWindowProcW(hwnd,msg,wp,lp);if(hit!=HTCLIENT)return hit;POINT pt{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(hwnd,&pt);if(!self->fullscreen){RECT r{};GetClientRect(hwnd,&r);int b=6;if(pt.x<b)return pt.y<b?HTTOPLEFT:(pt.y>=r.bottom-b?HTBOTTOMLEFT:HTLEFT);if(pt.x>=r.right-b)return pt.y<b?HTTOPRIGHT:(pt.y>=r.bottom-b?HTBOTTOMRIGHT:HTRIGHT);if(pt.y<b)return HTTOP;if(pt.y>=r.bottom-b)return HTBOTTOM;}
                if(self->graphics){float x=0,y=0;self->graphics->mouse(pt,x,y);if(y<headerBottom&&self->hit(x,y)<0)return HTCAPTION;}return HTCLIENT;}
            case WM_MOUSEMOVE:self->move({GET_X_LPARAM(lp),GET_Y_LPARAM(lp)});return 0;
            case WM_LBUTTONDOWN:self->move({GET_X_LPARAM(lp),GET_Y_LPARAM(lp)});self->pressed=self->hit(self->mx,self->my);self->dragSlider();SetCapture(hwnd);return 0;
            case WM_LBUTTONUP:{self->move({GET_X_LPARAM(lp),GET_Y_LPARAM(lp)});int action=self->hit(self->mx,self->my),down=self->pressed;self->pressed=-1;ReleaseCapture();if(action==down&&action>=0)self->act(action);return 0;}
            case WM_KEYDOWN:if(!(lp&(1LL<<30)))self->key(wp);return 0;
            case WM_INPUT:mk5hid::shared().input(lp);break;
            case WM_INPUT_DEVICE_CHANGE:if(wp==GIDC_REMOVAL)mk5hid::shared().removed(reinterpret_cast<HANDLE>(lp));return 0;
            case WM_SYSCOMMAND:if((wp&0xfff0)==SC_KEYMENU)return 0;break;
            }
        }catch(const std::exception& e){self->notice=L"Error: "+widen(e.what());try{eventLog(self->backend.root(),"ui-error "+std::string(e.what()));}catch(...){}}
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
};

int diagnostics(const fs::path& root,const std::vector<std::wstring>& args){
    wchar_t system[MAX_PATH];if(!GetSystemDirectoryW(system,MAX_PATH))throw std::runtime_error("system path");auto powershell=fs::path(system)/L"WindowsPowerShell/v1.0/powershell.exe";
    std::vector<std::wstring> command={powershell.wstring(),L"-NoLogo",L"-NoProfile",L"-NonInteractive",L"-ExecutionPolicy",L"Bypass",L"-WindowStyle",L"Hidden",L"-File",(root/L"build/native/manual-temporary-8-20261025/product/session.ps1").wstring(),L"-ProjectRoot",root.wstring()};
    fs::path log=root/L"logs/temporaria-8/launcher-diagnostic.log";
    for(size_t i=0;i<args.size();++i){std::wstring flag;if(args[i]==L"--game-mode")flag=L"-GameMode";else if(args[i]==L"--smoke-frames")flag=L"-WindowFrames";else if(args[i]==L"--data-root")flag=L"-DataRoot";else if(args[i]==L"--plan-output")flag=L"-PlanOutput";else if(args[i]==L"--visual-mode")flag=L"-VisualMode";else if(args[i]==L"--scanlines")flag=L"-Scanlines";else if(args[i]==L"--filter")flag=L"-Filter";else if(args[i]==L"--scalefx")flag=L"-ScaleFX";else if(args[i]==L"--scale-mode")flag=L"-ScaleMode";else if(args[i]==L"--cas")flag=L"-Cas";else if(args[i]==L"--bilateral")flag=L"-Bilateral";else if(args[i]==L"--smaa")flag=L"-Smaa";else if(args[i]==L"--game-fullscreen")flag=L"-GameFullscreen";else if(args[i]==L"--crt-preset")flag=L"-CrtPreset";else if(args[i]==L"--sync")flag=L"-SyncMode";else if(args[i]==L"--color")flag=L"-ColorMode";else if(args[i]==L"--brightness")flag=L"-Brightness";else if(args[i]==L"--bezel")flag=L"-Bezel";else if(args[i]==L"--high-refresh")flag=L"-HighRefresh";else if(args[i]==L"--audio-latency")flag=L"-AudioLatency";else throw std::runtime_error("unknown diagnostic argument");if(i+1>=args.size())throw std::runtime_error("missing diagnostic argument");command.push_back(flag);command.push_back(args[++i]);if(flag==L"-PlanOutput")log=args[i]+L".launcher.log";}
    std::wstring line;for(auto& s:command){if(!line.empty())line+=L' ';line+=quote(s);}rejectLinks(log);fs::create_directories(log.parent_path());SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE output=CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);HANDLE input=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(output==INVALID_HANDLE_VALUE||input==INVALID_HANDLE_VALUE){if(output!=INVALID_HANDLE_VALUE)CloseHandle(output);if(input!=INVALID_HANDLE_VALUE)CloseHandle(input);throw std::runtime_error("diagnostic log");}
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW|STARTF_USESTDHANDLES;startup.wShowWindow=SW_HIDE;startup.hStdInput=input;startup.hStdOutput=output;startup.hStdError=output;PROCESS_INFORMATION info{};BOOL ok=CreateProcessW(powershell.c_str(),line.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,root.c_str(),&startup,&info);CloseHandle(input);CloseHandle(output);if(!ok)throw std::runtime_error("diagnostic process");CloseHandle(info.hThread);WaitForSingleObject(info.hProcess,INFINITE);DWORD code=4;GetExitCodeProcess(info.hProcess,&code);CloseHandle(info.hProcess);return int(code);
}
// --prepare --roms <folder> [--confirm-roms]: the setup without a window, for other launchers (RecompLauncher). The same queue
// as the ROM Setup and Build pages: every game is verified against the folder, then the games it holds are built. Prints
// "progress <0..1> <text>" lines on stdout. --confirm-roms is the Legal page confirmation, ticked by the player there.
// Exit: 0 built, 1 a build failed (logs/setup-<game>.log), 2 no supported ROM set in the folder, 3 ownership not confirmed,
// 5 Python or the compiler not found, 6 the app is open.
int prepare(const fs::path& root,const std::vector<std::wstring>& args){
    fs::path roms;bool confirm=false;
    for(size_t i=1;i<args.size();++i){if(args[i]==L"--roms"&&i+1<args.size())roms=args[++i];else if(args[i]==L"--confirm-roms")confirm=true;else throw std::runtime_error("unknown prepare argument");}
    if(roms.empty())throw std::runtime_error("--prepare needs --roms <folder>");
    const HANDLE out=GetStdHandle(STD_OUTPUT_HANDLE);std::string last;
    const auto say=[&](double done,const std::wstring& text){
        char number[16];std::snprintf(number,sizeof(number),"%.3f",std::clamp(done,0.,1.));
        const int n=WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);std::string utf8(size_t(std::max(n,0)),'\0');
        if(n>0)WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),utf8.data(),n,nullptr,nullptr);
        const auto line=std::string("progress ")+number+" "+utf8+"\n";if(line==last)return;last=line;
        DWORD written=0;if(out&&out!=INVALID_HANDLE_VALUE)WriteFile(out,line.data(),DWORD(line.size()),&written,nullptr);};
    // A failure also goes to stderr as plain text: RecompLauncher shows the step's last words, not its progress lines.
    const auto fail=[&](int code,double done,const std::wstring& text){say(done,text);const HANDLE err=GetStdHandle(STD_ERROR_HANDLE);
        const int n=WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);std::string utf8(size_t(std::max(n,0)),'\0');
        if(n>0)WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),utf8.data(),n,nullptr,nullptr);
        utf8+="\n";
        DWORD written=0;if(err&&err!=INVALID_HANDLE_VALUE)WriteFile(err,utf8.data(),DWORD(utf8.size()),&written,nullptr);return code;};
    const HANDLE singleton=CreateMutexW(nullptr,TRUE,L"Local\\TriUnitRecompLauncher");if(!singleton)throw std::runtime_error("launcher mutex");
    const bool open=GetLastError()==ERROR_ALREADY_EXISTS;struct MutexGuard{HANDLE h;bool owned;~MutexGuard(){if(owned)ReleaseMutex(h);CloseHandle(h);}} guard{singleton,!open};
    if(open)return fail(6,0,L"Close the TriUnit Recomp window first, then run the setup again.");
    Backend backend(root);
    if(confirm&&!backend.romsAcknowledged()&&!backend.acknowledgeRoms(true))throw std::runtime_error("ROM ownership confirmation not saved");
    if(!backend.romsAcknowledged())return fail(3,0,L"Confirm that you own the original ROM files.");
    const auto wait=[&](const auto& report){while(backend.setupRunning()){report();Sleep(250);}report();};
    say(0,L"Checking Python and the compiler");backend.checkRequirements();wait([]{});
    const auto& r=backend.setupRequirements();
    if(r.python.empty()||!r.pythonSupported||r.compiler.empty())return fail(5,0,L"Python 3.12+ and MSYS2 g++ were not found (README, Requirements).");
    std::array<fs::path,3> before;for(int i=0;i<3;++i)before[size_t(i)]=backend.setupGame(i).folder;
    for(int i=0;i<3;++i){if(!backend.setRomFolder(i,roms))return fail(2,0,L"This ROM folder can't be used.");backend.queueSetup(i,true);}
    wait([&]{say(.02,L"Checking your ROM files");});
    std::vector<int> found;
    for(int i=0;i<3;++i){if(backend.setupGame(i).state=="done")found.push_back(i);else if(!before[size_t(i)].empty())backend.setRomFolder(i,before[size_t(i)]);}
    if(found.empty())return fail(2,.05,L"No supported ROM set in this folder or in its mk1, mk2 and mk3 subfolders (README, Compatibility).");
    const std::array<std::pair<const char*,const wchar_t*>,8> steps{{{"verify",L"checking the ROM files"},{"stage",L"staging the ROM files"},
        {"decoder",L"building the decoder"},{"generate",L"generating the C++"},{"accept",L"checking the generated C++"},{"compile",L"compiling"},
        {"seed",L"making the factory settings"},{"install",L"installing"}}};
    std::wstring failed;   // the first game that did not build, in words
    for(size_t k=0;k<found.size();++k){
        const int i=found[k];const std::wstring title=compatibleGames[size_t(i)].title;
        if(!backend.queueSetup(i,false))throw std::runtime_error("setup not queued");
        wait([&]{
            const auto& g=backend.setupGame(i);size_t s=0;while(s<steps.size()&&g.step!=steps[s].first)++s;if(s==steps.size())s=0;
            const double part=s<5?.04*double(s):s==5?.2+.7*(g.unitsTotal?double(g.unitsDone)/g.unitsTotal:0):.9+.05*double(s-5);
            auto text=title+L": "+steps[s].second;if(s==5&&g.unitsTotal)text+=L" "+std::to_wstring(g.unitsDone)+L"/"+std::to_wstring(g.unitsTotal);
            say(.05+.95*(double(k)+part)/double(found.size()),text);});
        if(backend.setupGame(i).state!="done"){const auto why=title+L": "+backend.failureText(i);if(failed.empty())failed=why+L" "+backend.failureHint(i);say(.05+.95*double(k+1)/double(found.size()),why);}
    }
    if(!failed.empty())return fail(1,1,failed);
    say(1,L"Ready to play");return 0;
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int){
    const uint64_t launched=frameTicks();   // start-to-first-frame in the UI log (launcher-ready)
    fs::path root;bool unattended=false;
    try{
        std::vector<wchar_t> filename(32768);DWORD n=GetModuleFileNameW(nullptr,filename.data(),DWORD(filename.size()));if(!n||n>=filename.size())throw std::runtime_error("launcher executable path");root=fs::path(filename.data()).parent_path();
        int count=0;auto raw=CommandLineToArgvW(GetCommandLineW(),&count);if(!raw)throw std::runtime_error("launcher arguments");std::vector<std::wstring> args;for(int i=1;i<count;++i)args.emplace_back(raw[i]);LocalFree(raw);
        fs::path capture;bool captureSettings=false;int captureTab=launcherTab;int capturePage=-1;
        // --ui-capture <png> [overview|roms|build|play|legal] / --settings-capture <png> [controls|display]: one deterministic frame (layout checks).
        if(!args.empty()){unattended=true;if(args[0]==L"--prepare")return prepare(root,args);if((args[0]==L"--ui-capture"||args[0]==L"--settings-capture")&&(args.size()==2||args.size()==3)){capture=args[1];captureSettings=args[0]==L"--settings-capture";rejectLinks(capture);
                if(args.size()==3){if(captureSettings&&args[2]==L"controls")captureTab=controlsTab;else if(captureSettings&&args[2]==L"display")captureTab=2;else{const std::array<const wchar_t*,5> names{L"overview",L"roms",L"build",L"play",L"legal"};for(int k=0;k<5;++k)if(args[2]==names[size_t(k)])capturePage=k;if(capturePage<0)throw std::runtime_error("unknown capture page");}}}
            else return diagnostics(root,args);}
        HANDLE singleton=nullptr;if(capture.empty()){singleton=CreateMutexW(nullptr,TRUE,L"Local\\TriUnitRecompLauncher");if(!singleton)throw std::runtime_error("launcher mutex");if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(singleton);auto existing=FindWindowW(L"MkArcadeCollection",nullptr);if(existing){ShowWindow(existing,SW_RESTORE);SetForegroundWindow(existing);}return 0;}}
        struct MutexGuard{HANDLE h;~MutexGuard(){if(h){ReleaseMutex(h);CloseHandle(h);}}} mutexGuard{singleton};
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(com))throw std::runtime_error("launcher COM");struct ComGuard{~ComGuard(){CoUninitialize();}} comGuard;
        App app(root);WNDCLASSW cls{};cls.lpfnWndProc=App::procedure;cls.hInstance=instance;cls.lpszClassName=L"MkArcadeCollection";cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(nullptr,IDI_APPLICATION);if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("launcher class");
        RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);int w=capture.empty()?std::min(1382,int(work.right-work.left-60)):1536,h=capture.empty()?std::min(922,int(work.bottom-work.top-60)):1024;
        HWND window=CreateWindowExW(WS_EX_APPWINDOW,cls.lpszClassName,productName,WS_POPUP|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_SYSMENU,(work.right-w)/2,(work.bottom-h)/2,w,h,nullptr,nullptr,instance,&app);if(!window)throw std::runtime_error("launcher window");mk5hid::shared().listen(window);
        struct WindowGuard{HWND h;~WindowGuard(){if(IsWindow(h))DestroyWindow(h);}} windowGuard{window};
        const uint64_t built=frameTicks();app.graphics=std::make_unique<Renderer>(window);const uint64_t rendered=frameTicks();// Art is drawn in code; the launcher reads no image file.
        if(!capture.empty()){
            SetWindowPos(window,nullptr,0,0,1536,1024,SWP_NOZORDER|SWP_NOACTIVATE);app.launcher.reducedMotion=true;app.settings=captureSettings;app.category=captureTab;app.notice.clear();if(capturePage>=0)app.page=capturePage;app.draw(1,true);eventLog(root,"capture-ready ms="+std::to_string((frameTicks()-launched)/10000)+" app+window="+std::to_string((built-launched)/10000)+" renderer="+std::to_string((rendered-built)/10000));app.graphics->saveCapture(capture);return 0;
        }
        eventLog(root,"launcher-start DX12");app.borderless(app.launcher.fullscreen);ShowWindow(window,SW_SHOW);UpdateWindow(window);
        try{
            const auto track=root/L"custom_audio/launcher/menu.wav";rejectLinks(track); // optional user file; silence without it
            app.menuMusic=std::make_unique<MenuMusic>(track);
        }catch(const std::exception& e){eventLog(root,"menu-music unavailable: "+std::string(e.what()));}
        FrameTimer frameTimer;
        mk5timing::Cadence cadence;
        bool quit=false,ready=false;uint64_t last=frameTicks();
        while(!quit){
            MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message==WM_QUIT){quit=true;break;}TranslateMessage(&msg);DispatchMessageW(&msg);}if(quit)break;
            app.poll();if(!IsWindow(window))break;app.pad();
            const bool minimized=IsIconic(window);
            const bool inLauncher=!app.backend.running()&&!app.closing&&!minimized&&GetForegroundWindow()==window;
            const bool fadingMusic=app.menuMusic&&app.menuMusic->update(inLauncher?app.menuMusicLevel():0);
            const auto now=frameTicks();
            if(minimized)cadence.next=0;
            else if(frameDue(cadence,now,app.active?app.launcher.fps:app.setupBusy?10:2)){   // behind other windows: 10 fps while a build shows progress, else 2
                app.draw(std::min(.1f,float(now-last)/ticksPerSecond));
                if(!ready){ready=true;eventLog(root,"launcher-ready ms="+std::to_string((frameTicks()-launched)/10000)+" app+window="+std::to_string((built-launched)/10000)+" renderer="+std::to_string((rendered-built)/10000));}
                last=now;
            }
            // Input may wake this wait without forcing another GPU frame.
            const auto after=frameTicks();
            if(!minimized){
                if(cadence.due(after))continue;
                frameTimer.arm(cadence.next-after);
            }
            const DWORD delay=fadingMusic||(!minimized&&app.active)?4:(minimized?200:100);
            if(MsgWaitForMultipleObjectsEx(minimized?0:1,&frameTimer.handle,delay,QS_ALLINPUT,MWMO_INPUTAVAILABLE)==WAIT_FAILED)
                throw std::runtime_error("launcher message wait");
        }
        eventLog(root,"launcher-close frames="+std::to_string(app.graphics->frames()));return 0;
    }catch(const std::exception& e){try{if(!root.empty())eventLog(root,"fatal "+std::string(e.what()));}catch(...){}if(!unattended)MessageBoxW(nullptr,(L"The launcher could not start.\n\n"+widen(e.what())+L"\n\nKeep the EXE at the project root, next to native, build and roms.").c_str(),productName,MB_OK|MB_ICONERROR);return 4;}
}
