#include "../launcher/launcher_main.cpp"
#include <iostream>
#include <tuple>

void verify(bool value,const char* why){if(!value)throw std::runtime_error(why);}
void verifyFrameCadence(){
 for(unsigned fps:{30u,60u,120u,240u}){
  mk5timing::Cadence cadence;
  constexpr uint64_t start=1000000;
  const auto interval=(ticksPerSecond+fps-1)/fps;
  verify(frameDue(cadence,start,fps),"launcher draws the first frame immediately");
  verify(!frameDue(cadence,start+interval-1,fps),"launcher enforces its fractional frame period");
  verify(frameDue(cadence,start+2*interval+7000,fps),"launcher draws once after missing a slot");
  verify(cadence.next==start+3*interval,"late launcher frames retain phase");
  verify(frameDue(cadence,start+2*interval+8000,10),"launcher changes cadence immediately on losing focus");
 }
}
auto videoValues(const VideoConfig& v){
 return std::tie(v.scanlines,v.filter,v.sync,v.color,v.brightness,v.scalefxAmount,v.scaleMode,
                 v.cas,v.bilateral,v.scalefx,v.bezel,v.highRefresh,v.smaa,v.fullscreen,
                 v.hdr,v.hdrPaper,v.hdrPeak,v.hdrHighlights,v.widescreen);
}
template<class T>
void cycleVideo(App& app,int id,T VideoConfig::*field,std::initializer_list<T> values){
 const auto actions=app.actions();const auto control=std::find(actions.begin(),actions.end(),id);
 verify(control!=actions.end(),"video control must be keyboard reachable");
 app.focus=int(control-actions.begin());
 for(auto expected:values){
  if(app.video().*field!=expected)throw std::runtime_error("unexpected value at video control "+std::to_string(id));
  app.key(VK_RETURN);
 }
 verify(app.video().*field==*values.begin(),"video control must wrap to its first setting");
}
void videoMatrix(App& app,int game){
 app.openSettings();app.key('1');
 for(bool edition:{false,true}){
  app.key('1');if(app.draft.videoEffects)app.act(videoPage);
  if(app.videoRemastered!=edition)app.act(videoProfile);
  const auto inactive=app.draft.video(!edition);
  app.act(211);
  cycleVideo(app,videoScale,&VideoConfig::scaleMode,{0,1,2,3,4,5});
  cycleVideo(app,201,&VideoConfig::filter,{0,1,2,3});
  cycleVideo(app,200,&VideoConfig::scanlines,{0,1,2,3,4,5,6,7,8,9});
  cycleVideo(app,207,&VideoConfig::color,{0,1,2});
  cycleVideo(app,208,&VideoConfig::brightness,{100,110,120,80,90});
  app.key('3');
  cycleVideo(app,250,&VideoConfig::sync,{1,2,0});
  cycleVideo(app,251,&VideoConfig::highRefresh,{false,true});
  if(app.widescreenAvailable()){   // MK1, native MK2 and MK3, Remastered profile: SCREEN cycles TV FRAME, 4:3, WIDESCREEN
   const auto actions=app.actions();app.focus=int(std::find(actions.begin(),actions.end(),209)-actions.begin());
   for(auto [bezel,wide]:{std::pair{true,false},{false,false},{false,true}}){
    verify(app.video().bezel==bezel&&app.video().widescreen==wide,"SCREEN must cycle TV FRAME, 4:3 and WIDESCREEN");app.key(VK_RETURN);}
   verify(app.video().bezel&&!app.video().widescreen,"SCREEN must wrap to the TV frame");
  }else{
   cycleVideo(app,209,&VideoConfig::bezel,{true,false});
   verify(!app.video().widescreen,"widescreen stays off outside the Remastered profiles");
  }
  cycleVideo(app,videoFullscreen,&VideoConfig::fullscreen,{true,false});
  app.act(displayPage);
  cycleVideo(app,hdrMode,&VideoConfig::hdr,{1,0});
  cycleVideo(app,hdrPaper,&VideoConfig::hdrPaper,{0,80,120,160,200,240,280,320,400,480});
  cycleVideo(app,hdrPeak,&VideoConfig::hdrPeak,{0,400,600,800,1000,1500,2000,4000});
  if(edition)cycleVideo(app,hdrHighlights,&VideoConfig::hdrHighlights,{0,25,50,75,100});
  else verify(!app.videoControlEnabled(hdrHighlights),"HDR highlights stay off in the Original profile");
  app.act(displayPage);
  app.key('1');app.act(videoPage);
  cycleVideo(app,255,&VideoConfig::scalefx,{false,true});
  cycleVideo(app,videoCas,&VideoConfig::cas,{0,25,50,75,100});
  cycleVideo(app,videoSmaa,&VideoConfig::smaa,{false,true});
  cycleVideo(app,videoBilateral,&VideoConfig::bilateral,{0,25,50,75,100});
  app.act(255);
  for(int amount=0;amount<=100;++amount){
   app.setScaleAmount(969.f+2.6f*amount);
   verify(app.video().scalefxAmount==amount,"ScaleFX slider must reach every integer percent");
   app.act(videoCas);app.act(videoSmaa);app.act(videoBilateral);
   app.act(videoPage);for(int id:{int(videoScale),201,200,207,208})app.act(id);
   app.key('3');for(int id:{250,251,209,int(videoFullscreen)})app.act(id);
   app.key('1');app.act(videoPage);
   const auto expected=app.video();
   app.backend.config=app.draft;
   verify(app.backend.save()&&app.backend.load(),"save and reload a combined video profile");
   verify(videoValues(app.backend.config.video(edition))==videoValues(expected),"combined values survive the INI round trip");
   verify(videoValues(app.backend.config.video(!edition))==videoValues(inactive),"combined edits must preserve inactive profile");
   verify(app.backend.config.remastered==(app.musicAvailable?edition:app.draft.remastered),"the profile edited is the edition the game starts in (issue #5)");
  }
  app.act(255);
  verify(app.video().scalefxAmount==100&&!app.video().scalefx,"disabling ScaleFX preserves its strength");
 }
 app.act(212);
 verify(app.selected==game,"matrix must keep the chosen game");
}
int wmain(int argc,wchar_t** argv){
 try{
  if(argc!=2)throw std::runtime_error("isolated root required");
  verifyFrameCadence();
  const auto root=fs::absolute(argv[1]);
  const auto stage=root/L"build/native/manual-temporary-8-20261025";
  {
   App controller(root);controller.mk1Available=true;controller.active=false;controller.act(10);
   verify(controller.page==pagePlay&&!controller.settings,"Play in the navigation opens the cabinets");
   verify(controller.padEdges(XINPUT_GAMEPAD_A)==0,"inactive controller cannot activate UI");
   controller.active=true;verify(controller.padEdges(XINPUT_GAMEPAD_A)==0,"held A after focus return must wait for release");
   verify(controller.padEdges(XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_START)==0,"held action buttons remain blocked");
   verify(controller.padEdges(0)==0,"neutral state rearms controller without action");
   controller.padActions(controller.padEdges(XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_START));
   verify(controller.chosen==0&&!controller.backend.config.remastered,"simultaneous A and Start produce one cabinet selection");
   controller.padActions(XINPUT_GAMEPAD_A|XINPUT_GAMEPAD_B);
   verify(!controller.backend.config.remastered,"cancel consumes simultaneous confirmation");
  }
  App app(root);app.mk1Available=app.mk2Available=true;
  auto enabled=[&](int id){app.hitTargets();auto it=std::find_if(app.controls.begin(),app.controls.end(),[&](const Control& c){return c.id==id;});return it!=app.controls.end()&&it->enabled;};
  verify(app.page==pageOverview,"without a finished setup the launcher opens on Overview");
  for(int id:{40,41,42,10,11,43})verify(enabled(id),"every page of the navigation is reachable");
  app.key(VK_NEXT);verify(app.page==pageRoms,"PgDn walks the navigation");app.key(VK_PRIOR);verify(app.page==pageOverview,"PgUp walks back");
  app.act(43);verify(app.page==pageLegal&&!app.backend.romsAcknowledged(),"Legal page without confirmation");
  app.act(41);verify(!enabled(60)&&!enabled(70),"no verify or build before the confirmation");
  app.act(43);app.act(120);verify(app.backend.romsAcknowledged(),"confirmation from the Legal page");app.act(120);verify(!app.backend.romsAcknowledged(),"confirmation withdrawn");
  app.act(10);
  verify(!enabled(20),"Play disabled before explicit selection");
  app.act(1);
  verify(app.chosen==0&&app.selected==0&&!app.details&&!app.backend.running(),"first click selects without popup or launch");
  verify(!app.backend.config.remastered&&enabled(20)&&enabled(11),"first click preserves original and enables Play/Settings");
  app.act(1);verify(app.backend.config.remastered&&app.draft.remastered&&app.mk1Remastered,"second click selects remastered");
  Backend saved(root);verify(saved.load()&&saved.config.remastered,"version persisted");
  app.key(VK_RIGHT);verify(app.selected==0&&app.chosen==0,"keyboard navigation cannot silently switch selected game");
  app.key(VK_RETURN);verify(app.selected==1&&app.chosen==1&&!app.details&&!app.backend.config.remastered,"Enter selects keyboard focused MK2 without launch");
  app.act(2);verify(!app.backend.config.remastered&&!app.backend.running(),"unsupported remaster cannot toggle or start");
  app.act(1);verify(app.backend.config.remastered,"returning to MK1 loads saved version without toggling");
  app.act(1);verify(!app.backend.config.remastered,"next activation returns original");
  app.act(3);verify(app.chosen==2&&!enabled(20)&&enabled(11)&&!app.details,"unavailable cabinet keeps collection Settings but cannot play");
  app.mk3Available=true;verify(app.chosen==2&&enabled(20)&&!app.backend.config.remastered&&!app.backend.running(),"available MK3 native cabinet can play");
  app.act(4);verify(app.chosen==2,"only the three supported cabinets exist");app.mk3Available=false;
  app.act(1);verify(app.chosen==0&&!app.backend.config.remastered,"reselection keeps current saved version");
  app.key(VK_DOWN);verify(app.actions().at(size_t(app.focus))==20,"Down focuses separate Play action");
  app.key(VK_UP);verify(app.actions().at(size_t(app.focus))==1,"Up returns to cabinet activation");
  const auto ini=stage/L"profile/launcher.ini";
  fs::remove(ini);fs::create_directory(ini);
  app.act(1);verify(!app.backend.config.remastered&&!app.draft.remastered&&!app.mk1Remastered,"failed version save rolls back all visible selection state");
  fs::remove(ini);
  app.act(1);verify(app.backend.config.remastered,"retry can save after failure");
  app.backend.config.video().scanlines=3;verify(app.backend.save(),"save MK1 custom settings");
  app.act(2);app.backend.config.video().filter=1;verify(app.backend.save(),"save MK2 custom settings");
  app.act(1);verify(app.backend.config.video().scanlines==3&&app.backend.config.remastered,"MK1 preferences isolated from MK2");
  app.act(2);verify(app.backend.config.video().filter==1&&!app.backend.config.remastered,"MK2 preferences isolated from MK1");
  const auto nativeManifest=root/L"build/native/mk2-native-v1/product/native-package.ini";
  fs::create_directories(nativeManifest.parent_path());std::ofstream(nativeManifest)<<"fixture";
  for(int game:{0,1}){
   app.act(3);app.act(game+1);app.openSettings();
   verify(app.settings&&enabled(274)&&enabled(275)&&enabled(276),"each native game must expose both video profiles and scaling");
   const bool launchedVersion=app.draft.remastered;
   // Issue #5: the video profile is the edition the game starts in (one choice for the Video, Display and Audio tabs).
   // Without the Remastered edition installed the profile can still be edited, but the game keeps starting in the Original.
   const bool follows=app.musicAvailable||launchedVersion;
   app.act(274);verify(app.draft.remastered==(follows?!launchedVersion:launchedVersion),"video profile selector chooses the launch version");
   const auto geometry=app.actions();
   app.act(274);verify(app.actions()==geometry,"Original and Remastered must offer the same scaling choices");
   app.act(275);verify(enabled(255)&&enabled(277)&&enabled(278)&&enabled(279),"effects page must expose ScaleFX, CAS, SMAA and bilateral");
   const auto effects=app.actions();
   app.pressed=256;
   app.act(274);verify(app.actions()==effects,"Original and Remastered must offer the same effects");
   verify(app.pressed==-1,"switching profiles must cancel a ScaleFX drag");
   app.act(277);app.act(278);app.act(279);
   if(game==0)app.act(210);
   else{
    // A placeholder manifest exercises controls without the native ROM package.
    app.backend.config=app.draft;verify(app.backend.save(),"save native MK2 fixture preferences");
   }
   Backend persisted(root);verify(persisted.selectGame(game)&&persisted.load(),"reopen video settings");
   const auto& edited=persisted.config.video(!launchedVersion);
   const auto& other=persisted.config.video(launchedVersion);
   verify(edited.cas>0&&edited.smaa&&edited.bilateral>0&&!other.cas&&!other.smaa&&!other.bilateral,
          "saving one version's video draft must leave the other version's effects independent");
   verify(persisted.config.remastered==(follows?!launchedVersion:launchedVersion),"saving video edits keeps the edited profile as the launch version (issue #5)");
   app.act(274);app.act(211);app.act(274);
   verify(app.video().smaa,"restoring one video profile must keep the other profile's draft");
   app.act(videoPage);
   app.act(212);app.openSettings();app.key('1');
   const auto reopenedControls=app.actions();
   verify(std::find(reopenedControls.begin(),reopenedControls.end(),videoCas)!=reopenedControls.end(),
          "reopening Settings must show the saved Effects page after discarding an unsaved page change");
   app.act(212);
   videoMatrix(app,game);
  }
  // CONTROLS (every game): capture a key, cancel with Esc, toggle SOCD, save, reload, restore the original layout.
  app.openSettings();app.key('7');verify(app.category==controlsTab,"CONTROLS tab reachable by key 7");
  const int highPunch=controlCell+mk5controls::HighPunch;
  app.act(highPunch);verify(app.capture==highPunch,"cell starts a capture");app.key('Q');
  verify(app.capture<0&&app.draftControls.keys[0][mk5controls::HighPunch]=='Q',"captured key bound to P1 high punch");
  app.act(highPunch);app.key(VK_ESCAPE);verify(app.capture<0&&app.settings&&app.draftControls.keys[0][mk5controls::HighPunch]=='Q',"Esc cancels the capture only");
  app.act(controlCell+25+mk5controls::Up);verify(app.capture<0,"controller directions are not mappable");
  app.act(socdControl);verify(app.draftControls.socd==1,"SOCD toggles to last input");
  app.act(runAheadControl);verify(app.draftControls.runAhead==1,"run-ahead toggles to 1 frame");
  app.act(trainingControl);verify(app.draftControls.training==1,"training mode toggles on");
  app.act(210);const auto stored=app.backend.loadControls();
  verify(stored.keys[0][mk5controls::HighPunch]==81&&stored.socd==1&&stored.runAhead==1&&stored.training==1,"controls saved and reloaded");
  app.act(211);app.act(210);verify(mk5controls::format(app.backend.loadControls())==mk5controls::format(mk5controls::Config{}),"original controls restored");
  app.act(212);
  std::cout<<"PASS: cabinet selection and rollback; 608 individual video values and 404 combined INI round trips across both games and editions; controls tab\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
