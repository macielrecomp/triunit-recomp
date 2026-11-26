// Launcher drawn in code: no image file is read, the legal notice and setup state are on the start screen,
// and the game names live only in launcher_legal.hpp. With "render" it also draws the home screen (DX12).
#include "../launcher/launcher_main.cpp"
#include <iostream>
namespace {
void need(bool v,const char* m){if(!v)throw std::runtime_error(m);}
std::string slurp(const fs::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};}
std::string upper(std::string v){for(auto& c:v)if(c>='a'&&c<='z')c=char(c-'a'+'A');return v;}
}
int wmain(int argc,wchar_t** argv){try{
    need(argc>=3,"project root and scratch root required");
    const fs::path project=fs::absolute(argv[1]),scratch=fs::absolute(argv[2]);
    // Static contract over the launcher sources.
    for(const auto& entry:fs::directory_iterator(project/L"native/launcher")){
        const auto name=entry.path().filename().wstring();auto text=upper(slurp(entry.path()));
        // custom_bezel/<game>/bezel.png is the user's own file (like custom_audio/), never shipped artwork.
        for(auto at=text.find("\"BEZEL.PNG\"");at!=std::string::npos;at=text.find("\"BEZEL.PNG\""))text.erase(at,11);
        // renderer_contract.cpp round-trips its own capture through a temporary file; it loads no artwork.
        if(name!=L"renderer_contract.cpp")for(const char* image:{".PNG\"",".JPG\"",".JPEG\"",".BMP\"",".DDS\"","LAUNCHER/ASSETS"})
            need(text.find(image)==std::string::npos,"launcher source references an image file");
        if(name!=L"launcher_legal.hpp")for(const char* mark:{"MORTAL KOMBAT","KOMBAT ARCADE","MK ARCADE LABS","MIDWAY","NETHERREALM","WARNER BROS"})
            need(text.find(mark)==std::string::npos,"game name or mark outside launcher_legal.hpp");
    }
    need(!fs::exists(project/L"native/runtime/arcade-bezel.png")&&!fs::exists(project/L"native/launcher/assets"),"image assets still present");
    const auto legal=upper(slurp(project/L"native/launcher/launcher_legal.hpp"));
    for(const char* term:{"WARNER BROS. ENTERTAINMENT INC.","NETHERREALM STUDIOS","NO GAME FILES ARE INCLUDED","NOT AFFILIATED"})
        need(legal.find(term)!=std::string::npos,"legal notice incomplete");
    // Setup state: without a finished setup record no game is offered.
    fs::create_directories(scratch);
    {
        Backend b(scratch);
        for(int i=0;i<3;++i)need(!b.setupFinished(i)&&!b.available(i),"game offered before its setup finished");
        const auto record=scratch/L"generated/mk3"/std::wstring(64,L'a');fs::create_directories(record);
        {std::ofstream(record/L"mk3.exe")<<"fixture";}
        {std::ofstream(record/L"setup.json")<<"{\"game\":\"mk3\",\"setup_version\":\"14\",\"compile\":{\"executable\":\"generated/mk3/"+std::string(64,'a')+"/mk3.exe\",\"sha256\":\""+std::string(64,'b')+"\"}}";}
        need(b.setupFinished(2)&&!b.setupFinished(1),"finished setup record not recognized");
        need(!b.available(2),"setup record alone must not stand in for the game package");
        // A build of an older release (another or no setup_version) asks for a new build.
        {std::ofstream(record/L"setup.json")<<"{\"game\":\"mk3\",\"setup_version\":\"13\",\"compile\":{\"executable\":\"generated/mk3/"+std::string(64,'a')+"/mk3.exe\",\"sha256\":\""+std::string(64,'b')+"\"}}";}
        need(!b.setupFinished(2)&&readSetup(scratch,L"mk3").outdated,"setup record of an older release offered as built");
        {std::ofstream(record/L"setup.json")<<"{\"setup_version\":\"14\",\"compile\":{\"executable\":\"../../x.exe\",\"sha256\":\""+std::string(64,'b')+"\"}}";}
        need(!b.setupFinished(2),"setup record outside its folder accepted");
    }
    if(argc>=4&&std::wstring(argv[3])==L"render"){
        WNDCLASSW cls{};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"TriUnitArtContract";RegisterClassW(&cls);
        HWND window=CreateWindowExW(0,cls.lpszClassName,productName,WS_POPUP,0,0,1536,1024,nullptr,nullptr,cls.hInstance,nullptr);need(window,"window");
        {
            App app(scratch);app.window=window;app.graphics=std::make_unique<Renderer>(window);app.launcher.reducedMotion=true;
            app.draw(1,true);unsigned w=0,h=0;const auto pixels=app.graphics->capture(w,h);
            // Legal footer: y 972..1016 of the 1024-unit layout, left part.
            size_t lit=0,legalBand=0;for(size_t n=0;n<pixels.size();++n){const unsigned v=(pixels[n]>>16&255)+(pixels[n]>>8&255)+(pixels[n]&255);lit+=v>60;if(n/w>=size_t(h*974/1024)&&n/w<size_t(h*1014/1024)&&n%w<size_t(w*480/1536))legalBand+=v>150;}
            need(lit>pixels.size()/50,"start screen did not draw");
            need(legalBand>200,"legal notice not drawn on the start screen");
        }
        DestroyWindow(window);
    }
    std::cout<<"PASS launcher drawn in code, legal notice, setup gate\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
