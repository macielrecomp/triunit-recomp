#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "mk5_controls.hpp"
#include <array>
#include <deque>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
namespace mklauncher {
// One game of the local setup queue (tools/setup/setup.py run by the user's Python, see Backend::queueSetup).
struct SetupGame {
 std::filesystem::path folder;                 // ROM folder chosen for this game (build/native/launcher-profile/setup.cfg)
 bool queued=false,active=false,verified=false; // verified: the last --verify-only report or a finished setup was ok
 std::string mode,step,state,message,program;  // last progress file; state: queued, running, done, failed, cancelled
 std::string code,hint,detail;                 // failed: setup.py SetupError code, plain hint, one technical line
 int unitsDone=0,unitsTotal=0;double elapsed=0,estimate=0; // estimate: seconds of the last finished setup here (0 = none)
 std::vector<std::pair<std::string,std::string>> files; // last --verify-only report: file name, status
 std::vector<std::string> checks;              // digests the manifest pins (size, crc32, sha1, sha256)
};
struct SetupRequirements {bool checked=false,ok=false,windows64=false,pythonSupported=false;std::string python,compiler,validated;};
struct VideoConfig {
 int scanlines=0,filter=0,sync=1,color=0,brightness=100,scalefxAmount=100,scaleMode=0,cas=0,bilateral=0;
 // HDR: 1 when Windows has HDR on; paper white and peak in nits (0 = from Windows); highlights only in [Video.Remastered].
 int hdr=1,hdrPaper=0,hdrPeak=0,hdrHighlights=0;
 bool scalefx=false,bezel=true,highRefresh=false,smaa=false,fullscreen=true;
 bool widescreen=false; // [Video.Remastered] only: the arena beyond the arcade's 4:3 (MK1_/MK2_/MK3_WIDESCREEN), no TV frame
};
// MK3 native Remastered defaults ([Video.Remastered] of the MK3 profile): BVM / 1080p (preset 4) and ScaleFX 100. The MK3
// Original defaults are VideoConfig{}: no effect, the arcade image.
constexpr VideoConfig mk3RemasteredDefaults(){VideoConfig v;v.scanlines=4;v.scalefx=true;v.scalefxAmount=100;return v;}
struct Config {
 int fps=60,uiVolume=0,gameVolume=100,audioLatency=1;
 int mk3Engine=1; // [MK3] Engine: saved MK3 card choice (see Backend::mk3Engine); absent, 0 or invalid -> 1, native Original
 bool blood=true,violence=true,cmosCoinage=false,remastered=false,reducedMotion=false,vsync=true,fullscreen=true,videoEffects=false;
 std::array<VideoConfig,2> videos{}; // [Video.Original] / [Video.Remastered]; for MK3 the profile of each edition (mk3Engine 1 / 2)
 VideoConfig& video(bool remastered){return videos[remastered];}
 const VideoConfig& video(bool remastered)const{return videos[remastered];}
 VideoConfig& video(){return video(remastered);}
 const VideoConfig& video()const{return video(remastered);}
};
// MK3 command line (Backend::launch): the native package's files, the edition, the CMOS profile and the cabinet's DSW
// (--dsw-word, INPUT_PORTS(mk3) default FD7Dh; Blood 4000h and Violence 8000h from Config, the operator's DIP switches),
// passed only when a switch differs from the default.
constexpr unsigned mk3DefaultDsw=0xfd7d;
constexpr unsigned mk3DswWord(const Config& c){return (mk3DefaultDsw&~0xc000u)|(c.blood?0x4000u:0)|(c.violence?0x8000u:0);}
std::vector<std::wstring> mk3Arguments(const std::filesystem::path& executable,const std::filesystem::path& roms,const std::filesystem::path& seed,
                                       const std::filesystem::path& graphics,const std::filesystem::path& profile,bool remastered,const Config& c);
struct GameAdjustments {int difficulty=3,coinage=1;bool freePlay=false,customCoinage=false;};
struct LauncherPreferences {
 int fps=60,musicVolume=100;
 bool vsync=true,reducedMotion=false,fullscreen=true,musicMuted=false;
};
class Backend {
public:
 explicit Backend(const std::filesystem::path& root);
 ~Backend();
 Backend(const Backend&)=delete;
 Backend& operator=(const Backend&)=delete;
 Config config;
 // adjustments=false skips the operator adjustments (they read and verify the program ROM): the startup and a card
 // selection need only the preferences; Settings loads them again.
 bool load(bool adjustments=true);bool save();void reset();
 LauncherPreferences loadLauncherPreferences(const Config& legacy)const;
 bool saveLauncherPreferences(const LauncherPreferences&)const;
 // Player controls of every game (build/native/launcher-profile/controls.cfg); absent or invalid: the original layout.
 mk5controls::Config loadControls()const;bool saveControls(const mk5controls::Config&)const;
 GameAdjustments adjustments;
 bool refreshAdjustments(bool read=true);bool adjustmentsAvailable()const;
 // Blood and Violence: the cabinet's DIP switches, offered with the CMOS adjustments (MK1, native MK2) and always for MK3,
 // whose launch passes them as its DSW (mk3Arguments) without a CMOS profile.
 bool dipSwitchesAvailable()const;
 std::wstring coinageLabel(const GameAdjustments&)const;
 bool saveAll(const Config&,const GameAdjustments&);bool resetHighScores();
 bool selectGame(int index,bool adjustments=true);
 bool available(int index) const;
 bool nativeMk2Installed() const;
 // MK3 edition of the native package (D1, 14/11/2026: no temporary core): 1 native Original, 2 native Remastered (video layers
 // 1 and 2; arcade audio in both, D7). chooseMk3Engine saves the choice as [MK3] Engine and load() restores it; available(2) is
 // the native package. A refused launch (manifest, executable or ROM check) reports the error and leaves MK3 unavailable;
 // nothing else is started. The launch passes the edition's video profile, GameVolume and AudioLatency as native MK2 does.
 int mk3Engine() const{return mk3Engine_;}
 bool setMk3Engine(int engine); // effective engine of this session only
 bool chooseMk3Engine(int engine);
 bool remasteredAvailable() const;
 // Local setup (tools/setup/setup.py): a game is offered only after its record in generated/<game>/<sha>/ exists.
 bool setupFinished(int index) const;
 bool startSetup(const std::filesystem::path& roms); // one folder for every game: sets the three folders, queues three setups
 bool setupRunning();                                // also advances the queue
 // Setup queue: one setup.py at a time in a kill-on-close Job Object (Cancel or closing the launcher ends its whole tree).
 // Python is looked up on PATH only (python.exe, then py.exe -3) by a --requirements run queued before the first job.
 const SetupGame& setupGame(int index)const{return setupGames_.at(size_t(index));}
 const SetupRequirements& setupRequirements()const{return requirements_;}
 bool setRomFolder(int index,const std::filesystem::path& folder);
 // The user confirms owning the original board/ROM files before any verify or build (saved in setup.cfg).
 bool romsAcknowledged()const{return acknowledged_;}
 bool acknowledgeRoms(bool value);
 bool queueSetup(int index,bool verifyOnly);
 void checkRequirements();
 void cancelSetup();
 std::filesystem::path setupLog(int index)const;    // logs/setup-<game>.log
 // A failed setup in words for the player, never the raw error (that stays in the log and in the report).
 std::wstring failureText(int index)const;
 std::wstring failureHint(int index)const;
 std::wstring setupReport(int index)const;         // Copy report: versions, step, code, detail, log tail; %USERPROFILE% hidden
 std::filesystem::path setupOutput(int index)const; // generated/<game>/<sha>/ of the finished record, empty without one
 bool launchNativeExperiment(const std::filesystem::path& manifest,const std::filesystem::path& settings={});
 bool launch(unsigned frames=0);void poll();bool running() const;
 void requestStop();
 const std::wstring& status() const;const std::filesystem::path& root() const;DWORD exitCode() const;
 HWND gameWindow() const;
private:
 bool launchNative(const std::filesystem::path& manifest,const std::filesystem::path& settings,bool diagnostics);
 bool launchMk3();
 int mk3Engine_=1;bool mk3Native_=false;
 std::filesystem::path root_;
 std::filesystem::path gameExecutable_;
 std::wstring status_;
 HANDLE process_=nullptr,setupProcess_=nullptr,setupJob_=nullptr;
 struct SetupJob {int game;bool verify;}; // game -1: --requirements
 std::deque<SetupJob> setupQueue_;SetupJob activeSetup_{-1,false};
 std::array<SetupGame,3> setupGames_{};SetupRequirements requirements_;
 std::wstring interpreter_;size_t interpreterTry_=0;bool setupCancelled_=false,acknowledged_=false;ULONGLONG nextSetupRead_=0;
 void pollSetup();void startSetupJob(SetupJob);void finishSetup(DWORD code);void readSetupProgress(int game);
 void loadSetupFolders();bool saveSetupFolders()const;void refreshSetupEstimate(int game);
 DWORD processId_=0,exitCode_=0,gamePid_=0;
 ULONGLONG nextDiscovery_=0;
 bool stopping_=false,nativeExperiment_=false,nativeLiveExperiment_=false,nativePlayable_=false;
 int gameIndex_=0;
 bool adjustmentsAvailable_=false,selectedNative_=false;
 std::filesystem::path adjustmentsPath_;
 std::vector<uint8_t> adjustmentsImage_,adjustmentsProgram_;
 std::vector<std::wstring> coinageLabels_;
 std::string adjustmentsSha_;
 HWND gameWindow_=nullptr;
 bool readSettings(const std::filesystem::path&,const std::string& section,int capabilities,bool mk3Legacy=false);
 std::filesystem::path operatorPath(bool verifyPayload=true)const;
 std::filesystem::path operatorRoms()const;
 void inspectGame();
};
}
