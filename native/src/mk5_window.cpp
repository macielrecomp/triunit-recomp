#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include "mk5_window.hpp"
#include "mk5_crt.hpp"
#include "mk5_crt_shader.hpp"
#include "mk5_stage_bicubic.hpp"
#include "mk5_scalefx.hpp"
#include "mk5_smaa.hpp"
#include "mk5_video.hpp"
#include "mk5_gamepad.hpp"
#include "mk5_hid.hpp"
#include "mk5_timing.hpp"
#include "mk5_color.hpp"
#include "mk5_bezel.hpp"
#include "mk5_hdr.hpp"
#include "mk5_audio_link.hpp"
#include "mk5_audio_output.hpp"
#include "mk5_training.hpp"
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <unordered_map>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

using Microsoft::WRL::ComPtr;
namespace {
void checked(HRESULT hr,const char* operation){
    if(FAILED(hr))throw std::runtime_error(std::string(operation)+" HRESULT="+std::to_string(uint32_t(hr)));
}
HANDLE make_pace_timer(){
    const auto timer=CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_ALL_ACCESS);
    return timer?timer:CreateWaitableTimerW(nullptr,FALSE,nullptr);
}
void arm_pace_timer(HANDLE timer,LONGLONG ticks,LONGLONG frequency){
    LARGE_INTEGER relative{};
    relative.QuadPart=-(std::max<LONGLONG>(1,ticks)*10000000+frequency-1)/frequency;
    if(!SetWaitableTimer(timer,&relative,0,nullptr,nullptr,FALSE))
        throw std::runtime_error("MK5 presentation timer failed");
}
constexpr UINT kFrontier=WM_APP+2;
constexpr DWORD kOccludedRetryMs=100;
constexpr UINT kSourceSrv=0,kBezelSrv=1,kChainSrv=2,kPrefilterSrv=3,kStageSrv=5,kSceneSrv=6,kOverlaySrv=7;
constexpr auto kHdrSpace=DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
constexpr UINT kShaderConstants=29;
// The game's area in the television frame: the 4:3 glass, or the Phosphor raster that fits inside it.
mk5crt::Viewport bezel_area(const mk5bezel::Tv& tv,unsigned source_height,const mk5crt::Settings& crt){
    const float w=float(tv.right-tv.left),h=float(tv.bottom-tv.top);
    if(!crt.phosphor())return {float(tv.left),float(tv.top),w,h};
    auto area=mk5crt::phosphor_viewport(unsigned(w),unsigned(h),source_height);
    area.x+=float(tv.left);area.y+=float(tv.top);return area;
}
constexpr char bezel_shader[]=R"HLSL(
Texture2D artwork : register(t0);
// The frame is drawn at the output size; alpha 0 marks the glass, where the game shows.
float4 ps(float4 position:SV_Position):SV_Target {
    float4 c=artwork.Load(int3(position.xy,0));
    if(c.a==0)discard;
    return float4(c.rgb,1);
}
)HLSL";
// Training overlay (mk5_training.hpp): source pixels over the game area, point sampled, blended by alpha.
// shortcut: flat over the CRT curvature (the boxes drift a few pixels near the edges), until a curved preset needs it.
constexpr char overlay_shader[]=R"HLSL(
Texture2D overlay : register(t0);
cbuffer Settings : register(b0) {float4 s[6];};   // s[0].x source height, s[0].z source width, s[5] game viewport
float4 ps(float4 position:SV_Position):SV_Target {
    const int2 size=int2(s[0].z,s[0].x);
    const int2 p=clamp(int2((position.xy-s[5].xy)/s[5].zw*float2(size)),int2(0,0),size-1);
    float4 c=overlay.Load(int3(p,0));
    if(c.a==0)discard;
    return c;
}
)HLSL";
// The television frame at the output size, drawn in code (mk5_bezel.hpp) on every core; no image file is read.
std::vector<uint32_t> load_bezel(const mk5bezel::Tv& tv){
    std::vector<uint32_t> pixels(size_t(tv.width)*size_t(tv.height));
    const int workers=int(std::max(1u,std::thread::hardware_concurrency()));std::vector<std::thread> threads;
    for(int k=0;k<workers;++k)threads.emplace_back([&,k]{
        for(int y=k;y<tv.height;y+=workers)for(int x=0;x<tv.width;++x)pixels[size_t(y)*size_t(tv.width)+size_t(x)]=mk5bezel::tv_pixel(tv,x,y);});
    for(auto& thread:threads)thread.join();
    return pixels;
}
// The user's own frame (custom_bezel/<game>/bezel.png, never shipped): scaled to fit the output, transparent where the
// screen is. The game takes the largest centred 4:3 rectangle inside the transparent area; `tv` receives that rectangle.
std::vector<uint32_t> load_user_bezel(const std::wstring& path,mk5bezel::Tv& tv){
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    struct Uninit{bool on;~Uninit(){if(on)CoUninitialize();}} uninit{SUCCEEDED(com)};
    ComPtr<IWICImagingFactory> factory;checked(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"WIC factory");
    ComPtr<IWICBitmapDecoder> decoder;checked(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder),"custom frame image");
    ComPtr<IWICBitmapFrameDecode> frame;checked(decoder->GetFrame(0,&frame),"custom frame image frame");
    UINT iw=0,ih=0;checked(frame->GetSize(&iw,&ih),"custom frame size");
    if(!iw||!ih||iw>16384||ih>16384)throw std::runtime_error("custom frame image must be 1 to 16384 pixels per side");
    const double s=std::min(double(tv.width)/iw,double(tv.height)/ih);
    const UINT sw=std::max(1u,UINT(std::lround(iw*s))),sh=std::max(1u,UINT(std::lround(ih*s)));
    ComPtr<IWICFormatConverter> convert;checked(factory->CreateFormatConverter(&convert),"WIC converter");
    checked(convert->Initialize(frame.Get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"custom frame BGRA");
    ComPtr<IWICBitmapScaler> scaler;checked(factory->CreateBitmapScaler(&scaler),"WIC scaler");
    checked(scaler->Initialize(convert.Get(),sw,sh,WICBitmapInterpolationModeFant),"custom frame scale");
    std::vector<uint32_t> art(size_t(sw)*sh);checked(scaler->CopyPixels(nullptr,sw*4,UINT(art.size()*4),reinterpret_cast<BYTE*>(art.data())),"custom frame pixels");
    std::vector<uint32_t> pixels(size_t(tv.width)*size_t(tv.height),0xff000000u);
    const int ox=(tv.width-int(sw))/2,oy=(tv.height-int(sh))/2;int l=tv.width,t=tv.height,r=0,b=0;
    for(UINT y=0;y<sh;++y)for(UINT x=0;x<sw;++x){
        const uint32_t p=art[size_t(y)*sw+x];auto& out=pixels[size_t(oy+int(y))*size_t(tv.width)+size_t(ox+int(x))];
        if((p>>24)<128){out=0;l=std::min(l,ox+int(x));t=std::min(t,oy+int(y));r=std::max(r,ox+int(x)+1);b=std::max(b,oy+int(y)+1);}
        else out=0xff000000u|(p&0xffffff); // premultiplied: the frame over black
    }
    if(r<=l||b<=t)throw std::runtime_error("custom frame image has no transparent screen area");
    const int h=std::min(b-t,(r-l)*3/4),w=h*4/3;
    tv.left=l+(r-l-w)/2;tv.right=tv.left+w;tv.top=t+(b-t-h)/2;tv.bottom=tv.top+h;tv.radius=0;
    return pixels;
}
}
BootDiInputs Mk5KeyboardInputs(const std::array<bool,256>& keys,BootDiInputs base){
    struct Mapping{unsigned key,bit;};
    constexpr Mapping map[]={{VK_UP,0},{VK_DOWN,1},{VK_LEFT,2},{VK_RIGHT,3},{'A',4},{'S',5},{'D',6},
        {'I',8},{'K',9},{'J',10},{'L',11},{'F',12},{'G',13},{'H',14},
        {'5',16},{'6',17},{'1',18},{'2',21},{'V',25},{'B',26},{'Z',28},{'X',29}};
    for(auto m:map)if(keys[m.key])base.in01&=~(1u<<m.bit);
    return base;
}
struct Mk5Window::Impl {
    // The PCM/game producer never services modal loops or waits on GPU Present.
    std::thread ui,compositor;
    std::atomic<HWND> hwnd{nullptr};
    std::atomic<bool> running{true},exclusive{false};
    std::atomic<bool> focused{false};
    mutable mk5pad::Devices pads;
    mk5controls::Config controls;std::array<mk5controls::Socd,2> socd{};
    mutable std::atomic<unsigned> controller_count{0};
    std::mutex stats_mutex;mk5timing::Stats producer_timing,display_timing,prepare_timing;
    // The producer's own work per frame (from one Present's end to the next one's start: the game, its audio, its inputs)
    // and the frames that reached Present after their deadline. A frame's work over the native 18.28 ms is a late frame
    // whatever the display does.
    mk5timing::Stats work_timing;LONGLONG work_from=0;uint64_t late_frames=0;bool producer_raised=false;
    bool bezel=true,fullscreen=true,legacy_pacing=false,high_refresh=false,tearing=false,immediate_headroom=false;
    unsigned across=4,down=3;   // the frame's physical aspect (Mk5Window)
    int sync_mode=1,color_mode=0,brightness=100;
    unsigned scalefx_amount=0,scale_mode=0,cas_amount=0,bilateral_amount=0;
    bool smaa_enabled=false;
    // HDR: 0 off, 1 when Windows has HDR on for this monitor, 2 always (validation). The frame is drawn to `scene` exactly as
    // in SDR, then one pass encodes it to HDR10 in the back buffer.
    // shortcut: decided when the window opens; turning Windows HDR on or off mid-game takes effect on the next start.
    unsigned hdr_mode=0,paper_setting=0,peak_setting=0,highlight_setting=0;
    bool hdr=false;mk5hdr::Settings hdr_settings;
    std::atomic<int> hdr_state{0}; // 0 off, 1 HDR10, 2 Windows SDR, 3 unsupported
    ComPtr<ID3D12Resource> scene,hdr_readback;ComPtr<ID3D12PipelineState> hdr_pipeline;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT hdr_readback_layout{};
    std::atomic<uint64_t> hdr_validation_frames{0},hdr_validation_mismatches{0};
    mk5video::Sampling filter=mk5video::Sampling::nearest;
    std::shared_ptr<mk5sfx::Pipeline> scalefx;
    std::shared_ptr<mk5smaa::Pipeline> smaa;
    mk5sfx::Capture source_capture;
    UINT swap_flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    HANDLE frame_ready=nullptr,refresh_timer=nullptr;bool present_slot=false;mk5timing::Cadence repeat_clock;
    std::atomic<unsigned> frame_queue_limit{0};
    std::atomic<uint64_t> repeats{0},occluded_presents{0};
    std::array<std::atomic<bool>,256> keys{};
    std::mutex request_mutex;std::vector<Mk5Window::StateRequest> state_requests;
    std::atomic<uint64_t> frames{0},events{0},displayed{0},coalesced{0},toggles{0},mode_errors{0};
    std::atomic<uint32_t> last_mode_error{0};
    std::atomic<uint32_t> last_present_result{0};
    std::atomic<uint64_t> move_enter{0},move_exit{0},max_present_ms{0};
    std::mutex state_mutex,frame_mutex;
    std::condition_variable ready_cv;
    bool ready=false;std::string failure;
    struct Frame {
        std::vector<uint32_t> pixels,backdrop,scaled,overlay;
        mk5presentation::FighterFrame fighters;
        unsigned width=0,height=0;
        bool ready=false,scenery=false,actors=false;
    } pending,prepared,staged;
    HANDLE wake=nullptr,pace_timer=nullptr,compose_wake=nullptr;
    LONGLONG frequency=0,interval=0,next_due=0;
    unsigned ahead_low=0,ahead_late=0;bool ahead_stopped=false;   // run-ahead guard (Present)
    // One UI-owned submission at a time. The producer never waits on a GPU fence.
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swap;
    ComPtr<ID3D12DescriptorHeap> rtv_heap,srv_heap;
    std::array<ComPtr<ID3D12Resource>,2> targets;
    ComPtr<ID3D12Resource> texture,upload_buffer,readback;
    ComPtr<ID3D12Resource> stage_texture,stage_upload;
    std::array<ComPtr<ID3D12Resource>,2> prefiltered;
    ComPtr<ID3D12Resource> bezel_texture,bezel_upload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT bezel_upload_layout{};
    std::vector<uint32_t> bezel_pixels;mk5bezel::Tv tv;bool bezel_dirty=false;std::wstring bezel_image;
    ComPtr<ID3D12Resource> overlay_texture,overlay_upload;D3D12_PLACED_SUBRESOURCE_FOOTPRINT overlay_layout{};
    bool overlay_ready=false,overlay_pending=false,overlay_shown=false;mk5training::History history;
    std::vector<uint32_t> validation_overlay;   // the training overlay of the validated frame, blended by the reference
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12PipelineState> bezel_pipeline,prefilter_pipeline,overlay_pipeline;
    ComPtr<ID3D12Fence> fence;HANDLE fence_event=nullptr;
    UINT rtv_stride=0;UINT64 fence_value=0;bool in_flight=false,texture_ready=false,upload_pending=false,scaled_ready=false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT upload_layout{},readback_layout{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT stage_layout{};
    bool validate_graphics=false;float scanlines=0;
    bool stage_scaled=false,scenery_scaled=false,fighters_scaled=false,stage_texture_ready=false;
    bool plain_validated=false;
    std::atomic<uint64_t> stage_frames{0},stage_validation_frames{0},fighter_frames{0},fighter_validation_frames{0};
    mk5crt::Settings crt;
    std::atomic<unsigned> reported_source_width{0},reported_source_height{0};
    std::atomic<unsigned> viewport_width{0},viewport_height{0};
    std::atomic<bool> source_aspect{false};
    std::atomic<bool> crt_integer_vertical{false};
    std::atomic<uint64_t> draw_fence_us{0},draw_fence_max_us{0};
    std::atomic<uint64_t> gpu_reuse_wait_max_us{0};
    std::vector<uint32_t> validation_source,validation_stage;
    std::atomic<uint64_t> validation_frames{0},validation_mismatches{0};
    std::atomic<uint64_t> validation_bezel_pixels{0},validation_image_hash{0},validation_subtexel_pixels{0};
    std::atomic<unsigned> validation_max_error{0};
    void settings(){
        auto read=[](const char* name){char value[64]{};SetLastError(ERROR_SUCCESS);DWORD n=GetEnvironmentVariableA(name,value,sizeof(value));
            if(!n){if(GetLastError()==ERROR_ENVVAR_NOT_FOUND)return std::string();throw std::runtime_error(std::string(name)+" must not be empty");}
            if(n>=sizeof(value))throw std::runtime_error(std::string(name)+" is too long");
            return std::string(value,n);};
        const auto sampling=read("MK1_FILTER");
        const auto& names=mk5video::sampling_names;
        const auto found=std::find(names.begin(),names.end(),sampling);
        if(!sampling.empty()&&found==names.end())throw std::runtime_error("MK1_FILTER must be nearest, linear, sharp-bilinear or bicubic");
        if(!sampling.empty())filter=mk5video::Sampling(found-names.begin());
        const auto percent=[&](const char* key){
            const auto value=read(key);
            if(value.empty())return 0u;
            if(value.size()>3||value.find_first_not_of("0123456789")!=std::string::npos||std::stoul(value)>100)
                throw std::runtime_error(std::string(key)+" must be an integer from 0 to 100");
            return unsigned(std::stoul(value));
        };
        scalefx_amount=percent("MK1_SCALEFX");cas_amount=percent("MK1_CAS");bilateral_amount=percent("MK1_BILATERAL");
        const auto scale=read("MK1_SCALE_MODE");
        if(!scale.empty()){
            if(scale.size()!=1||scale.front()<'0'||scale.front()>'5')throw std::runtime_error("MK1_SCALE_MODE must be an integer from 0 to 5");
            scale_mode=unsigned(scale.front()-'0');
        }
        const auto amount=read("MK1_SCANLINES");
        if(!amount.empty()){
            // Locale-independent decimal grammar, without whitespace, signs, exponents or NaN.
            bool digit=false,dot=false;for(char c:amount){if(c>='0' && c<='9')digit=true;else if(c=='.' && !dot)dot=true;else throw std::runtime_error("MK1_SCANLINES must be a decimal from 0 to 1");}
            if(!digit)throw std::runtime_error("MK1_SCANLINES must be a decimal from 0 to 1");
            double value=0,scale=1;bool fractional=false;for(char c:amount){if(c=='.'){fractional=true;continue;}if(fractional){scale*=0.1;value+=(c-'0')*scale;}else value=value*10+c-'0';}
            if(!std::isfinite(value) || value<0 || value>1)throw std::runtime_error("MK1_SCANLINES must be from 0 to 1");
            scanlines=float(value);
        }
        crt.load(read);
        auto option=[&](const char* name,const std::string& fallback){auto v=read(name);return v.empty()?fallback:v;};
        const auto sync=option("MK1_SYNC","vrr");
        if(sync!="vrr"&&sync!="vsync"&&sync!="immediate")throw std::runtime_error("MK1_SYNC must be vrr, vsync or immediate");
        sync_mode=sync=="vsync"?0:(sync=="vrr"?1:2);
        auto boolean=[&](const char* name,bool fallback){const auto v=option(name,fallback?"1":"0");if(v!="0"&&v!="1")throw std::runtime_error(std::string(name)+" must be 0 or 1");return v=="1";};
        smaa_enabled=boolean("MK1_SMAA",false);
        bezel=boolean("MK1_BEZEL",true);fullscreen=boolean("MK1_FULLSCREEN",true);high_refresh=boolean("MK1_HIGH_REFRESH",false);
        legacy_pacing=boolean("MK1_DIAGNOSTIC_LEGACY_PACING",false);
        const auto palette=option("MK1_COLOR","original");if(palette!="original"&&palette!="vivid"&&palette!="warm")throw std::runtime_error("MK1_COLOR must be original, vivid or warm");color_mode=palette=="vivid"?1:(palette=="warm"?2:0);
        const auto light=option("MK1_BRIGHTNESS","100");if(light!="80"&&light!="90"&&light!="100"&&light!="110"&&light!="120")throw std::runtime_error("MK1_BRIGHTNESS must be 80, 90, 100, 110 or 120");brightness=std::stoi(light);
        const auto validation=read("MK1_GRAPHICS_VALIDATE");
        if(!validation.empty() && validation!="0" && validation!="1")throw std::runtime_error("MK1_GRAPHICS_VALIDATE must be 0 or 1");
        validate_graphics=validation=="1";
        std::wstring image(32768,L'\0');image.resize(GetEnvironmentVariableW(L"MK1_BEZEL_IMAGE",image.data(),DWORD(image.size())));
        bezel_image=image.size()<32768?image:std::wstring();
        // MK1_CONTROLS: the launcher's player controls (mk5controls::format); absent: the original layout.
        std::string text(4096,'\0');text.resize(GetEnvironmentVariableA("MK1_CONTROLS",text.data(),DWORD(text.size())));
        if(!text.empty()&&(text.size()>=4096||!mk5controls::parse(text,controls)))throw std::runtime_error("MK1_CONTROLS is invalid");
        const auto mode=option("MK1_HDR","off");
        if(mode!="off"&&mode!="auto"&&mode!="force")throw std::runtime_error("MK1_HDR must be off, auto or force");
        hdr_mode=mode=="off"?0:(mode=="auto"?1:2);
        const auto nits=[&](const char* key,unsigned low,unsigned high){
            const auto v=option(key,"auto");if(v=="auto")return 0u;
            if(v.size()>5||v.find_first_not_of("0123456789")!=std::string::npos||std::stoul(v)<low||std::stoul(v)>high)
                throw std::runtime_error(std::string(key)+" must be auto or "+std::to_string(low)+" to "+std::to_string(high));
            return unsigned(std::stoul(v));
        };
        paper_setting=nits("MK1_HDR_PAPER_WHITE",80,480);peak_setting=nits("MK1_HDR_PEAK",400,10000);
        highlight_setting=percent("MK1_HDR_HIGHLIGHTS");
    }
    unsigned image_width=0,image_height=0;UINT target_width=0,target_height=0;
    bool resize=true,repaint=false,toggle=false,leave_exclusive=false,switching_mode=false,placement_saved=false,close_requested=false;
    WINDOWPLACEMENT placement{};
    LONG_PTR window_style=WS_OVERLAPPEDWINDOW;
    ULONGLONG exit_deadline=0,occluded_retry=0;
    ~Impl(){if(wake)CloseHandle(wake);if(pace_timer)CloseHandle(pace_timer);if(compose_wake)CloseHandle(compose_wake);if(refresh_timer)CloseHandle(refresh_timer);if(fence_event)CloseHandle(fence_event);if(frame_ready)CloseHandle(frame_ready);}
    void clear_keys(){for(auto& key:keys)key=false;++events;}
    static LRESULT CALLBACK proc(HWND window,UINT message,WPARAM key,LPARAM value){
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(value)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(window,message,key,value);
        if(message==WM_CLOSE){self->close_requested=true;PostMessageW(window,WM_CANCELMODE,0,0);return 0;}
        if(message==WM_DESTROY){self->hwnd=nullptr;self->running=false;return 0;}
        if(message==WM_INPUT){mk5hid::shared().input(value);return DefWindowProcW(window,message,key,value);}
        if(message==WM_INPUT_DEVICE_CHANGE){if(key==GIDC_REMOVAL)mk5hid::shared().removed(reinterpret_cast<HANDLE>(value));return 0;}
        if(message==WM_ACTIVATEAPP)self->focused=key!=0;
        if(message==WM_SETFOCUS)self->focused=true;
        if(message==WM_KILLFOCUS || (message==WM_ACTIVATEAPP && !key)){self->focused=false;self->clear_keys();if(!self->switching_mode)self->leave_exclusive=true;return 0;}
        if(message==WM_SYSKEYDOWN && key==VK_RETURN && (value&(1LL<<29))){if(!(value&(1LL<<30)))self->toggle=true;return 0;}
        if(message==WM_SYSCOMMAND && (key&0xfff0)==SC_KEYMENU)return 0;
        if(message==WM_KEYDOWN || message==WM_KEYUP || message==WM_SYSKEYUP){
            if(key==VK_ESCAPE && message==WM_KEYDOWN){SendMessageW(window,WM_CLOSE,0,0);return 0;}
            if((key==VK_F5 || key==VK_F7) && message==WM_KEYDOWN){
                std::lock_guard<std::mutex> lock(self->request_mutex);
                if(!(value&(1LL<<30)) && self->state_requests.size()<16)
                    self->state_requests.push_back(key==VK_F5?Mk5Window::StateRequest::Save:Mk5Window::StateRequest::Load);
                return 0;
            }
            if(key<256){bool down=message==WM_KEYDOWN;if(self->keys[key].exchange(down)!=down)++self->events;}
            return message==WM_SYSKEYUP?DefWindowProcW(window,message,key,value):0;
        }
        // Retain a complete front buffer; no visible black GDI erase/copy gap.
        if(message==WM_ERASEBKGND)return 1;
        if(message==WM_PAINT){PAINTSTRUCT paint{};BeginPaint(window,&paint);EndPaint(window,&paint);self->repaint=true;return 0;}
        if(message==WM_SIZE){self->resize=true;self->repaint=true;if(key==SIZE_MINIMIZED && !self->switching_mode)self->leave_exclusive=true;return 0;}
        if(message==WM_DISPLAYCHANGE){self->resize=true;self->repaint=true;return 0;}
        if(message==WM_ENTERSIZEMOVE){++self->move_enter;return 0;}
        if(message==WM_EXITSIZEMOVE){++self->move_exit;self->repaint=true;return 0;}
        if(message==kFrontier){wchar_t title[128];swprintf(title,128,L"TriUnit Recomp | fronteira 0x%08x | Esc para fechar",unsigned(key));SetWindowTextW(window,title);return 0;}
        return DefWindowProcW(window,message,key,value);
    }
    static D3D12_RESOURCE_DESC buffer_desc(UINT64 bytes){
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;
        d.DepthOrArraySize=1;d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;return d;
    }
    ComPtr<ID3D12Resource> make_resource(D3D12_HEAP_TYPE heap,const D3D12_RESOURCE_DESC& desc,D3D12_RESOURCE_STATES state){
        D3D12_HEAP_PROPERTIES properties{};properties.Type=heap;properties.CreationNodeMask=properties.VisibleNodeMask=1;
        ComPtr<ID3D12Resource> resource;
        checked(device->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&resource)),"D3D12 resource");return resource;
    }
    void barrier(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=resource;
        b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=before;b.Transition.StateAfter=after;commands->ResourceBarrier(1,&b);
    }
    bool await_gpu(DWORD timeout){
        if(!in_flight)return true;
        if(fence->GetCompletedValue()>=fence_value){in_flight=false;return true;}
        if(FAILED(fence->SetEventOnCompletion(fence_value,fence_event)))return false;
        if(WaitForSingleObject(fence_event,timeout)!=WAIT_OBJECT_0)return false;
        if(fence->GetCompletedValue()<fence_value)return false;
        in_flight=false;return true;
    }
    void wait_gpu(){
        const bool measure=in_flight&&!validate_graphics;
        LARGE_INTEGER before{},after{};
        if(measure)QueryPerformanceCounter(&before);
        if(!await_gpu(5000)){checked(device->GetDeviceRemovedReason(),"D3D12 device removed");throw std::runtime_error("D3D12 GPU fence timed out after 5 seconds");}
        checked(device->GetDeviceRemovedReason(),"D3D12 device removed");
        if(measure){
            QueryPerformanceCounter(&after);
            const auto us=uint64_t((after.QuadPart-before.QuadPart)*1000000/frequency);
            gpu_reuse_wait_max_us=std::max(gpu_reuse_wait_max_us.load(),us);
        }
    }
    void retire_gpu() noexcept {
        if(!in_flight)return;
        // Signal once more after any failed original signal. Never release memory
        // still used by a live GPU. A stalled device is retired off the UI thread.
        const UINT64 completion=++fence_value;const HRESULT signal=queue->Signal(fence.Get(),completion);
        if(SUCCEEDED(signal) && await_gpu(5000))return;
        if(FAILED(device->GetDeviceRemovedReason()))return;
        try{
            std::vector<ComPtr<IUnknown>> keep;
            auto hold=[&keep](IUnknown* object){if(object)keep.emplace_back(object);};
            hold(queue.Get());hold(swap.Get());hold(rtv_heap.Get());hold(srv_heap.Get());
            for(auto& target:targets)hold(target.Get());
            for(auto& target:prefiltered)hold(target.Get());
            hold(texture.Get());hold(upload_buffer.Get());hold(readback.Get());hold(allocator.Get());hold(commands.Get());hold(root.Get());hold(pipeline.Get());
            hold(stage_texture.Get());hold(stage_upload.Get());
            hold(bezel_texture.Get());hold(bezel_upload.Get());hold(bezel_pipeline.Get());hold(overlay_texture.Get());hold(overlay_upload.Get());hold(overlay_pipeline.Get());hold(prefilter_pipeline.Get());hold(source_capture.memory.Get());
            auto retained_device=device;auto retained_queue=queue;auto retained_fence=fence;
            auto retained_scalefx=scalefx;auto retained_smaa=smaa;
            std::thread([keep=std::move(keep),retained_device,retained_queue,retained_fence,retained_scalefx,retained_smaa,completion,signal]() mutable {
                HRESULT result=signal;while(SUCCEEDED(retained_device->GetDeviceRemovedReason())){
                    if(FAILED(result))result=retained_queue->Signal(retained_fence.Get(),completion);
                    if(SUCCEEDED(result) && retained_fence->GetCompletedValue()>=completion)break;
                    Sleep(100);
                }
                keep.clear();retained_scalefx.reset();retained_smaa.reset();
            }).detach();
        }catch(...){
            // If even retirement allocation fails, retain references until process
            // teardown rather than free command memory while the GPU may use it.
            if(scalefx){auto* retained=new(std::nothrow) std::shared_ptr<mk5sfx::Pipeline>(std::move(scalefx));if(!retained)std::terminate();}
            if(smaa){auto* retained=new(std::nothrow) std::shared_ptr<mk5smaa::Pipeline>(std::move(smaa));if(!retained)std::terminate();}
            source_capture.memory.Detach();
            queue.Detach();swap.Detach();rtv_heap.Detach();srv_heap.Detach();
            for(auto& target:targets)target.Detach();
            for(auto& target:prefiltered)target.Detach();
            texture.Detach();upload_buffer.Detach();readback.Detach();bezel_texture.Detach();bezel_upload.Detach();overlay_texture.Detach();overlay_upload.Detach();overlay_pipeline.Detach();
            stage_texture.Detach();stage_upload.Detach();
            allocator.Detach();commands.Detach();root.Detach();pipeline.Detach();bezel_pipeline.Detach();prefilter_pipeline.Detach();fence.Detach();device.Detach();
        }
    }
    void configure_frame_queue(){
        const UINT limit=immediate_headroom&&sync_mode==2&&tearing&&!exclusive&&!high_refresh&&!validate_graphics?2:1;
        if(frame_queue_limit==limit)return;
        checked(swap->SetMaximumFrameLatency(limit),"DXGI maximum frame latency");
        frame_queue_limit=limit;
    }
    void graphics(){
        ComPtr<IDXGIFactory4> factory;checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");
        ComPtr<IDXGIFactory5> capable;BOOL supported=FALSE;
        if(SUCCEEDED(factory.As(&capable))&&SUCCEEDED(capable->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&supported,sizeof(supported))))tearing=supported!=FALSE;
        if(tearing)swap_flags|=DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        checked(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"D3D12 hardware device");
        D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        checked(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"D3D12 direct queue");
        checked(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"D3D12 fence");
        fence_event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!fence_event)throw std::runtime_error("D3D12 fence event failed");
        checked(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"D3D12 command allocator");
        checked(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"D3D12 command list");checked(commands->Close(),"D3D12 initial close");
        DXGI_SWAP_CHAIN_DESC1 desc{};desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
        desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.Scaling=physical_pixels()?DXGI_SCALING_NONE:DXGI_SCALING_STRETCH;desc.AlphaMode=DXGI_ALPHA_MODE_IGNORE;desc.Flags=swap_flags;
        ComPtr<IDXGISwapChain1> created;
        checked(factory->CreateSwapChainForHwnd(queue.Get(),hwnd.load(),&desc,nullptr,nullptr,&created),"D3D12 DXGI flip swap chain");
        checked(created.As(&swap),"DXGI swap chain 3");
        enable_hdr();
        configure_frame_queue();
        frame_ready=swap->GetFrameLatencyWaitableObject();if(!frame_ready)throw std::runtime_error("DXGI frame latency handle missing");
        DEVMODEW desktop{};desktop.dmSize=sizeof(desktop);MONITORINFOEXW monitor{};monitor.cbSize=sizeof(monitor);GetMonitorInfoW(MonitorFromWindow(hwnd.load(),MONITOR_DEFAULTTONEAREST),&monitor);
        const unsigned hz=EnumDisplaySettingsW(monitor.szDevice,ENUM_CURRENT_SETTINGS,&desktop)&&desktop.dmDisplayFrequency>1?desktop.dmDisplayFrequency:60;
        repeat_clock.interval=frequency/std::min(360u,hz); // presentation only; never guest rate
        checked(factory->MakeWindowAssociation(hwnd.load(),DXGI_MWA_NO_ALT_ENTER|DXGI_MWA_NO_WINDOW_CHANGES),"DXGI window association");
        D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=UINT(targets.size())+prefilter_passes()+1;
        checked(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&rtv_heap)),"D3D12 RTV heap");rtv_stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=kOverlaySrv+1;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        checked(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&srv_heap)),"D3D12 SRV heap");
        D3D12_DESCRIPTOR_RANGE ranges[2]{};ranges[0].RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ranges[0].NumDescriptors=1;
        ranges[1]=ranges[0];ranges[1].BaseShaderRegister=1;
        D3D12_ROOT_PARAMETER params[3]{};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable.NumDescriptorRanges=1;params[0].DescriptorTable.pDescriptorRanges=&ranges[0];params[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants.Num32BitValues=kShaderConstants;params[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        params[2]=params[0];params[2].DescriptorTable.pDescriptorRanges=&ranges[1];
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC signature{};signature.NumParameters=3;signature.pParameters=params;signature.NumStaticSamplers=1;signature.pStaticSamplers=&sampler;
        ComPtr<ID3DBlob> serialized,errors;checked(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors),"D3D12 root signature serialization");
        checked(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)),"D3D12 root signature");
        ComPtr<ID3DBlob> vs,ps;
        const auto crt_define=std::to_string(int(crt.mode)),sample_define=std::to_string(int(filter));
        const D3D_SHADER_MACRO shader_defines[]={{"MK5_CRT_MODE",crt_define.c_str()},{"MK5_SAMPLE_MODE",sample_define.c_str()},{nullptr,nullptr}};
        auto compile_pixel=[&](const char* source,size_t size,const char* entry){
            const HRESULT hr=D3DCompile(source,size,nullptr,shader_defines,nullptr,entry,"ps_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&ps,&errors);
            if(FAILED(hr)&&errors)throw std::runtime_error(std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()));
            checked(hr,"D3D12 pixel shader compilation");
        };
        checked(D3DCompile(mk5crt::shader,sizeof(mk5crt::shader),nullptr,shader_defines,nullptr,"vs","vs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vs,&errors),"D3D12 vertex shader compilation");
        compile_pixel(mk5crt::shader,sizeof(mk5crt::shader),"ps");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};state.pRootSignature=root.Get();state.VS={vs->GetBufferPointer(),vs->GetBufferSize()};state.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        state.BlendState.RenderTarget[0].SrcBlend=D3D12_BLEND_ONE;state.BlendState.RenderTarget[0].DestBlend=D3D12_BLEND_ZERO;state.BlendState.RenderTarget[0].BlendOp=D3D12_BLEND_OP_ADD;
        state.BlendState.RenderTarget[0].SrcBlendAlpha=D3D12_BLEND_ONE;state.BlendState.RenderTarget[0].DestBlendAlpha=D3D12_BLEND_ZERO;state.BlendState.RenderTarget[0].BlendOpAlpha=D3D12_BLEND_OP_ADD;state.BlendState.RenderTarget[0].LogicOp=D3D12_LOGIC_OP_NOOP;state.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        state.SampleMask=UINT_MAX;state.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;state.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;state.RasterizerState.DepthClipEnable=TRUE;
        state.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;state.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;state.NumRenderTargets=1;state.RTVFormats[0]=desc.Format;state.SampleDesc.Count=1;
        checked(device->CreateGraphicsPipelineState(&state,IID_PPV_ARGS(&pipeline)),"D3D12 presentation pipeline");
        if(bezel){
            compile_pixel(bezel_shader,sizeof(bezel_shader),"ps");
            state.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
            checked(device->CreateGraphicsPipelineState(&state,IID_PPV_ARGS(&bezel_pipeline)),"D3D12 bezel pipeline");
        }
        if(controls.training){
            compile_pixel(overlay_shader,sizeof(overlay_shader),"ps");
            auto blend=state.BlendState.RenderTarget[0];
            auto& b=state.BlendState.RenderTarget[0];b.BlendEnable=TRUE;b.SrcBlend=D3D12_BLEND_SRC_ALPHA;b.DestBlend=D3D12_BLEND_INV_SRC_ALPHA;
            b.SrcBlendAlpha=D3D12_BLEND_ZERO;b.DestBlendAlpha=D3D12_BLEND_ONE;
            state.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
            checked(device->CreateGraphicsPipelineState(&state,IID_PPV_ARGS(&overlay_pipeline)),"D3D12 training overlay pipeline");
            b=blend;
        }
        if(prefilter_passes()){
            compile_pixel(mk5crt::shader,sizeof(mk5crt::shader),"psPrefilter");
            state.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
            checked(device->CreateGraphicsPipelineState(&state,IID_PPV_ARGS(&prefilter_pipeline)),"D3D12 video prefilter pipeline");
        }
        if(hdr){
            compile_pixel(mk5hdr::shader,sizeof(mk5hdr::shader),"ps");
            state.PS={ps->GetBufferPointer(),ps->GetBufferSize()};state.RTVFormats[0]=DXGI_FORMAT_R10G10B10A2_UNORM;
            checked(device->CreateGraphicsPipelineState(&state,IID_PPV_ARGS(&hdr_pipeline)),"D3D12 HDR10 pipeline");
        }
        if(scalefx_amount)scalefx=std::make_shared<mk5sfx::Pipeline>(device.Get());
        if(smaa_enabled)smaa=std::make_shared<mk5smaa::Pipeline>(device.Get());
    }
    // Windows "SDR content brightness" of the monitor, in nits (1000 = 80 nits).
    static float sdr_white(HMONITOR monitor){
        MONITORINFOEXW info{};info.cbSize=sizeof(info);UINT32 paths=0,modes=0;
        if(!GetMonitorInfoW(monitor,&info)||GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&paths,&modes)!=ERROR_SUCCESS)return 203;
        std::vector<DISPLAYCONFIG_PATH_INFO> path(paths);std::vector<DISPLAYCONFIG_MODE_INFO> mode(modes);
        if(QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&paths,path.data(),&modes,mode.data(),nullptr)!=ERROR_SUCCESS)return 203;
        for(UINT32 i=0;i<paths;++i){
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header={DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,sizeof(source),path[i].sourceInfo.adapterId,path[i].sourceInfo.id};
            if(DisplayConfigGetDeviceInfo(&source.header)!=ERROR_SUCCESS||std::wcscmp(source.viewGdiDeviceName,info.szDevice))continue;
            DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
            white.header={DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL,sizeof(white),path[i].targetInfo.adapterId,path[i].targetInfo.id};
            if(DisplayConfigGetDeviceInfo(&white.header)==ERROR_SUCCESS&&white.SDRWhiteLevel)return float(white.SDRWhiteLevel)*80/1000;
        }
        return 203;
    }
    void enable_hdr(){
        if(!hdr_mode)return;
        ComPtr<IDXGIOutput> output;ComPtr<IDXGIOutput6> output6;DXGI_OUTPUT_DESC1 desc{};
        const bool display=SUCCEEDED(swap->GetContainingOutput(&output))&&SUCCEEDED(output.As(&output6))&&
            SUCCEEDED(output6->GetDesc1(&desc))&&desc.ColorSpace==kHdrSpace;
        if(!display&&hdr_mode==1){hdr_state=2;return;}
        checked(swap->ResizeBuffers(0,0,0,DXGI_FORMAT_R10G10B10A2_UNORM,swap_flags),"DXGI HDR10 buffers");
        UINT support=0;
        if(FAILED(swap->CheckColorSpaceSupport(kHdrSpace,&support))||!(support&DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)||
           FAILED(swap->SetColorSpace1(kHdrSpace))){
            checked(swap->ResizeBuffers(0,0,0,DXGI_FORMAT_B8G8R8A8_UNORM,swap_flags),"DXGI SDR buffers");hdr_state=3;return;
        }
        hdr=true;hdr_state=1;
        hdr_settings.paper=paper_setting?float(paper_setting):sdr_white(display?desc.Monitor:MonitorFromWindow(hwnd.load(),MONITOR_DEFAULTTONEAREST));
        hdr_settings.peak=peak_setting?float(peak_setting):(display&&desc.MaxLuminance>0?desc.MaxLuminance:1000.f);
        hdr_settings.highlights=float(highlight_setting)/100;
    }
    UINT scene_rtv()const{return UINT(targets.size())+prefilter_passes();}
    bool physical_pixels()const{return crt.phosphor()||scale_mode!=0;}
    unsigned prefilter_passes()const{return unsigned(bilateral_amount!=0)+unsigned(cas_amount!=0);}
    unsigned source_scale()const{return scalefx_amount?3:1;}
    bool decode_phosphor()const{
        return crt.phosphor()&&(scalefx_amount||smaa_enabled||prefilter_passes()||filter!=mk5video::Sampling::nearest);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE srv_at(UINT slot)const{
        auto handle=srv_heap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr+=size_t(slot)*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return handle;
    }
    void set_srv(UINT slot,ID3D12Resource* image,DXGI_FORMAT format){
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=1;
        auto handle=srv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr+=size_t(slot)*device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device->CreateShaderResourceView(image,&view,handle);
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_at(UINT slot)const{
        auto handle=rtv_heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr+=size_t(slot)*rtv_stride;
        return handle;
    }
    // Called for each back-buffer size: the frame is drawn 1:1 with the output.
    void prepare_bezel(){
        tv=mk5bezel::tv_layout(int(target_width),int(target_height));
        auto pixels=bezel_image.empty()?load_bezel(tv):load_user_bezel(bezel_image,tv);
        const UINT w=target_width,h=target_height;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;
        desc.DepthOrArraySize=1;desc.MipLevels=1;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
        bezel_texture=make_resource(D3D12_HEAP_TYPE_DEFAULT,desc,D3D12_RESOURCE_STATE_COPY_DEST);
        UINT64 bytes=0;
        device->GetCopyableFootprints(&desc,0,1,0,&bezel_upload_layout,nullptr,nullptr,&bytes);
        bezel_upload=make_resource(D3D12_HEAP_TYPE_UPLOAD,buffer_desc(bytes),D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped=nullptr;D3D12_RANGE none{0,0};
        checked(bezel_upload->Map(0,&none,&mapped),"D3D12 bezel upload map");
        for(UINT y=0;y<h;++y)std::memcpy(static_cast<BYTE*>(mapped)+bezel_upload_layout.Offset+size_t(y)*bezel_upload_layout.Footprint.RowPitch,pixels.data()+size_t(y)*w,size_t(w)*4);
        bezel_upload->Unmap(0,nullptr);
        set_srv(kBezelSrv,bezel_texture.Get(),desc.Format);bezel_dirty=true;
        if(validate_graphics)bezel_pixels=std::move(pixels);
    }
    void draw_bezel(){
        if(bezel_dirty){
            D3D12_TEXTURE_COPY_LOCATION dest{};dest.pResource=bezel_texture.Get();dest.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=bezel_upload.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint=bezel_upload_layout;
            commands->CopyTextureRegion(&dest,0,0,0,&source,nullptr);
            barrier(bezel_texture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);bezel_dirty=false;
        }
        const D3D12_VIEWPORT viewport{0,0,float(target_width),float(target_height),0,1};
        const D3D12_RECT clip{0,0,LONG(target_width),LONG(target_height)};
        commands->RSSetViewports(1,&viewport);commands->RSSetScissorRects(1,&clip);
        commands->SetPipelineState(bezel_pipeline.Get());
        auto* heap=srv_heap.Get();commands->SetDescriptorHeaps(1,&heap);
        commands->SetGraphicsRootDescriptorTable(0,srv_at(kBezelSrv));
        commands->DrawInstanced(3,1,0,0);
        commands->SetPipelineState(pipeline.Get());
    }
    void fit_borderless(){
        if(!fullscreen || exclusive || switching_mode)return;
        const auto window=hwnd.load();
        if(!(GetWindowLongPtrW(window,GWL_STYLE)&WS_POPUP))return;
        MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);RECT current{};
        if(!GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor) || !GetWindowRect(window,&current))
            throw std::runtime_error("MK5 borderless monitor geometry failed");
        const auto& r=monitor.rcMonitor;
        if(!EqualRect(&current,&r) && !SetWindowPos(window,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE))
            throw std::runtime_error("MK5 borderless monitor resize failed");
    }
    void resize_buffers(){
        // The desktop mode can change while the initially visible window opens.
        // Fit again before publishing readiness or reallocating the swap chain.
        fit_borderless();
        RECT client{};GetClientRect(hwnd.load(),&client);
        if(client.right<=0 || client.bottom<=0 || IsIconic(hwnd.load()))return;
        wait_gpu();for(auto& target:targets)target.Reset();
        checked(swap->ResizeBuffers(0,UINT(client.right),UINT(client.bottom),DXGI_FORMAT_UNKNOWN,swap_flags),"DXGI ResizeBuffers");
        configure_frame_queue();
        auto handle=rtv_heap->GetCPUDescriptorHandleForHeapStart();
        for(UINT i=0;i<targets.size();++i){checked(swap->GetBuffer(i,IID_PPV_ARGS(&targets[i])),"D3D12 DXGI back buffer");device->CreateRenderTargetView(targets[i].Get(),nullptr,handle);handle.ptr+=rtv_stride;}
        auto d=targets[0]->GetDesc();target_width=UINT(d.Width);target_height=d.Height;resize=false;repaint=true;occluded_retry=0;
        if(hdr){
            checked(swap->SetColorSpace1(kHdrSpace),"DXGI HDR10 color space");
            d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            scene=make_resource(D3D12_HEAP_TYPE_DEFAULT,d,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            device->CreateRenderTargetView(scene.Get(),nullptr,rtv_at(scene_rtv()));set_srv(kSceneSrv,scene.Get(),d.Format);
        }
        if(bezel)prepare_bezel();
        if(compose_wake)SetEvent(compose_wake);
    }
    void change_mode(bool want){
        struct Guard {bool& flag;Guard(bool& f):flag(f){flag=true;}~Guard(){flag=false;}} guard(switching_mode);
        BOOL current=FALSE;checked(swap->GetFullscreenState(&current,nullptr),"DXGI fullscreen query");
        if(bool(current)==want && (want || !placement_saved)){exclusive=want;return;}
        auto window=hwnd.load();
        if(want){
            placement.length=sizeof(placement);GetWindowPlacement(window,&placement);window_style=GetWindowLongPtrW(window,GWL_STYLE);placement_saved=true;
            ComPtr<IDXGIOutput> output;checked(swap->GetContainingOutput(&output),"DXGI containing output");
            DXGI_OUTPUT_DESC desc{};checked(output->GetDesc(&desc),"DXGI output description");
            SetWindowLongPtrW(window,GWL_STYLE,WS_POPUP|WS_VISIBLE);
            const auto& r=desc.DesktopCoordinates;
            SetWindowPos(window,HWND_TOP,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_FRAMECHANGED|SWP_NOACTIVATE);
            HRESULT hr=swap->SetFullscreenState(TRUE,output.Get());
            checked(swap->GetFullscreenState(&current,nullptr),"DXGI fullscreen confirmation");
            if(hr!=S_OK || !current){
                last_mode_error=uint32_t(hr==S_OK?E_FAIL:hr);++mode_errors;
                exclusive=current!=FALSE;leave_exclusive=true;return;
            }
        }else{
            HRESULT hr=swap->SetFullscreenState(FALSE,nullptr);checked(hr,"DXGI exclusive exit");
            checked(swap->GetFullscreenState(&current,nullptr),"DXGI exclusive exit confirmation");
            if(current){
                // DXGI may return a positive transition-in-progress status.
                // Keep the saved window until it really relinquishes exclusive.
                if(!exit_deadline)exit_deadline=GetTickCount64()+3000;
                if(GetTickCount64()>=exit_deadline)throw std::runtime_error("DXGI exclusive exit was not confirmed within 3 seconds");
                last_mode_error=uint32_t(hr==S_OK?DXGI_STATUS_MODE_CHANGE_IN_PROGRESS:hr);
                exclusive=true;leave_exclusive=true;return;
            }
            exit_deadline=0;
            SetWindowLongPtrW(window,GWL_STYLE,window_style);SetWindowPlacement(window,&placement);
            SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
            placement_saved=false;
        }
        checked(swap->GetFullscreenState(&current,nullptr),"DXGI fullscreen confirmation");exclusive=current!=FALSE;
        if(exclusive==want)++toggles;
        resize=true;repaint=true;resize_buffers();
    }
    bool take_frame(Frame& source,Frame& target){
        std::lock_guard<std::mutex> lock(frame_mutex);
        if(!source.ready)return false;
        std::swap(source,target);
        source.ready=false;
        return true;
    }
    void prepare_frame(Frame& frame){
        LARGE_INTEGER before{};QueryPerformanceCounter(&before);
        frame.actors=!frame.fighters.layers.empty();
        frame.scenery=frame.actors?!frame.fighters.backdrop.empty():!frame.backdrop.empty();
        if(frame.actors)mk5presentation::ComposeFighterFrame4x(frame.fighters,frame.width,frame.height,frame.scaled);
        else if(frame.scenery)mk5stage::Bicubic4x(frame.pixels,frame.backdrop,frame.width,frame.height,frame.scaled);
        else frame.scaled.clear();
        LARGE_INTEGER after{};QueryPerformanceCounter(&after);
        std::lock_guard<std::mutex> lock(stats_mutex);
        if(frames>=60)prepare_timing.duration(after.QuadPart-before.QuadPart,frequency);
    }
    void compose_frames(){
        try{
            Frame work;
            while(running){
                if(WaitForSingleObject(compose_wake,INFINITE)!=WAIT_OBJECT_0)
                    throw std::runtime_error("MK5 compositor event wait failed");
                if(!running)break;
                if(IsIconic(hwnd.load())||!take_frame(pending,work))continue;
                prepare_frame(work);
                if(!running)break;
                {
                    std::lock_guard<std::mutex> lock(frame_mutex);
                    if(prepared.ready)++coalesced;
                    std::swap(prepared,work);
                }
                SetEvent(wake);
            }
        }catch(const std::exception& e){
            {std::lock_guard<std::mutex> lock(state_mutex);if(!running)return;failure=e.what();running=false;}
            SetEvent(wake);
        }
    }
    bool upload(){
        if(!take_frame(high_refresh?prepared:pending,staged))return false;
        if(!high_refresh)prepare_frame(staged);
        const unsigned w=staged.width,h=staged.height;
        fighters_scaled=staged.actors;
        scenery_scaled=staged.scenery;
        stage_scaled=scenery_scaled || fighters_scaled;
        // The native filter chain uses the reconstructed base to avoid drawing a second actor edge.
        const auto& image=fighters_scaled?staged.fighters.underlay:staged.pixels;
        wait_gpu();
        const bool hardware_srgb=crt.phosphor()&&!decode_phosphor();
        if(w!=image_width || h!=image_height){
            texture.Reset();upload_buffer.Reset();texture_ready=false;scaled_ready=false;
            stage_texture.Reset();stage_upload.Reset();stage_texture_ready=false;
            D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=1;d.MipLevels=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;
            if(hardware_srgb)d.Format=DXGI_FORMAT_B8G8R8A8_TYPELESS;
            texture=make_resource(D3D12_HEAP_TYPE_DEFAULT,d,D3D12_RESOURCE_STATE_COPY_DEST);
            UINT64 bytes=0;device->GetCopyableFootprints(&d,0,1,0,&upload_layout,nullptr,nullptr,&bytes);
            upload_buffer=make_resource(D3D12_HEAP_TYPE_UPLOAD,buffer_desc(bytes),D3D12_RESOURCE_STATE_GENERIC_READ);
            set_srv(kSourceSrv,texture.Get(),hardware_srgb?DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:d.Format);
            image_width=w;image_height=h;reported_source_width=w;reported_source_height=h;
            auto* input=texture.Get();
            if(scalefx){scalefx->prepare(input,w,h);input=scalefx->resource_output();}
            if(smaa){smaa->prepare(input,w*source_scale(),h*source_scale());input=smaa->resource_output();}
            if(input!=texture.Get())set_srv(kChainSrv,input,DXGI_FORMAT_B8G8R8A8_UNORM);
            d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            d.Width=w*source_scale();d.Height=h*source_scale();
            for(unsigned i=0;i<prefilter_passes();++i){
                prefiltered[i]=make_resource(D3D12_HEAP_TYPE_DEFAULT,d,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                device->CreateRenderTargetView(prefiltered[i].Get(),nullptr,rtv_at(UINT(targets.size())+i));
                set_srv(kPrefilterSrv+i,prefiltered[i].Get(),d.Format);
            }
        }
        write_upload(upload_buffer.Get(),upload_layout,image,w,h);
        if(stage_scaled){
            const unsigned stage_w=w*mk5stage::scale,stage_h=h*mk5stage::scale;
            if(!stage_texture){
                auto d=texture->GetDesc();
                d.Width=stage_w;d.Height=stage_h;
                stage_texture=make_resource(D3D12_HEAP_TYPE_DEFAULT,d,D3D12_RESOURCE_STATE_COPY_DEST);
                UINT64 bytes=0;device->GetCopyableFootprints(&d,0,1,0,&stage_layout,nullptr,nullptr,&bytes);
                stage_upload=make_resource(D3D12_HEAP_TYPE_UPLOAD,buffer_desc(bytes),D3D12_RESOURCE_STATE_GENERIC_READ);
                set_srv(kStageSrv,stage_texture.Get(),hardware_srgb?DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:d.Format);
            }
            write_upload(stage_upload.Get(),stage_layout,staged.scaled,stage_w,stage_h);
        }
        overlay_shown=!staged.overlay.empty();
        if(overlay_shown){
            if(!overlay_texture || overlay_layout.Footprint.Width!=w || overlay_layout.Footprint.Height!=h){
                D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=w;d.Height=h;d.DepthOrArraySize=1;d.MipLevels=1;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.SampleDesc.Count=1;
                overlay_texture=make_resource(D3D12_HEAP_TYPE_DEFAULT,d,D3D12_RESOURCE_STATE_COPY_DEST);overlay_ready=false;
                UINT64 bytes=0;device->GetCopyableFootprints(&d,0,1,0,&overlay_layout,nullptr,nullptr,&bytes);
                overlay_upload=make_resource(D3D12_HEAP_TYPE_UPLOAD,buffer_desc(bytes),D3D12_RESOURCE_STATE_GENERIC_READ);
                set_srv(kOverlaySrv,overlay_texture.Get(),d.Format);
            }
            write_upload(overlay_upload.Get(),overlay_layout,staged.overlay,w,h);overlay_pending=true;
        }
        upload_pending=true;
        if(validate_graphics && (fighters_scaled?!fighter_validation_frames:
                                 scenery_scaled?!stage_validation_frames:!plain_validated)){
            validation_source=image;validation_overlay=overlay_shown?staged.overlay:std::vector<uint32_t>{};
            validation_stage.swap(staged.scaled);
        }
        return true;
    }
    static void write_upload(ID3D12Resource* buffer,const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout,
                             const std::vector<uint32_t>& image,unsigned w,unsigned h){
        void* mapped=nullptr;D3D12_RANGE none{0,0};checked(buffer->Map(0,&none,&mapped),"D3D12 upload map");
        for(unsigned y=0;y<h;++y)std::memcpy(static_cast<unsigned char*>(mapped)+layout.Offset+size_t(y)*layout.Footprint.RowPitch,image.data()+size_t(y)*w,size_t(w)*4);
        buffer->Unmap(0,nullptr);
    }
    void copy_upload(ID3D12Resource* image,ID3D12Resource* buffer,
                     const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout,bool& ready){
        if(ready)barrier(image,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dest{};dest.pResource=image;dest.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=buffer;source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint=layout;
        commands->CopyTextureRegion(&dest,0,0,0,&source,nullptr);
        barrier(image,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);ready=true;
    }
    void draw_prefilters(UINT input){
        const unsigned w=image_width*source_scale(),h=image_height*source_scale();
        const D3D12_VIEWPORT viewport{0,0,float(w),float(h),0,1};
        const D3D12_RECT clip{0,0,LONG(w),LONG(h)};
        auto* heap=srv_heap.Get();commands->SetDescriptorHeaps(1,&heap);
        commands->SetGraphicsRootSignature(root.Get());
        commands->SetPipelineState(prefilter_pipeline.Get());
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->RSSetViewports(1,&viewport);commands->RSSetScissorRects(1,&clip);
        float settings[kShaderConstants]{};
        settings[0]=float(h);settings[2]=float(w);
        for(unsigned i=0;i<prefilter_passes();++i){
            const bool bilateral=bilateral_amount&&i==0;
            settings[25]=bilateral?0:cas_amount/100.f;
            settings[26]=bilateral?bilateral_amount/100.f:0;
            auto* target=prefiltered[i].Get();
            barrier(target,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
            const auto rtv=rtv_at(UINT(targets.size())+i);
            commands->OMSetRenderTargets(1,&rtv,FALSE,nullptr);
            commands->SetGraphicsRootDescriptorTable(0,srv_at(input));
            commands->SetGraphicsRoot32BitConstants(1,kShaderConstants,settings,0);
            commands->DrawInstanced(3,1,0,0);
            barrier(target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            input=kPrefilterSrv+i;
        }
    }
    unsigned validation_tolerance()const{
        if(prefilter_passes())return crt.enabled()?5:3;
        if(crt.enabled() || filter==mk5video::Sampling::bicubic)return 3;
        if(filter!=mk5video::Sampling::nearest)return 2;
        return scanlines>0?1:0;
    }
    bool color_adjusted()const{return color_mode!=0||brightness!=100;}
    // Copies a render target to a new readback buffer and leaves the target in `after`.
    ComPtr<ID3D12Resource> copy_back(ID3D12Resource* image,D3D12_RESOURCE_STATES after,D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout){
        auto desc=image->GetDesc();UINT64 bytes=0;device->GetCopyableFootprints(&desc,0,1,0,&layout,nullptr,nullptr,&bytes);
        auto buffer=make_resource(D3D12_HEAP_TYPE_READBACK,buffer_desc(bytes),D3D12_RESOURCE_STATE_COPY_DEST);
        barrier(image,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=image;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dest{};dest.pResource=buffer.Get();dest.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dest.PlacedFootprint=layout;
        commands->CopyTextureRegion(&dest,0,0,0,&source,nullptr);barrier(image,D3D12_RESOURCE_STATE_COPY_SOURCE,after);
        return buffer;
    }
    // Every HDR10 pixel against the CPU encoding of the SDR scene pixel (1 code of 1023 for GPU pow rounding).
    void validate_hdr(){
        void* sdr=nullptr;void* wide=nullptr;D3D12_RANGE none{0,0};
        D3D12_RANGE all{0,size_t(readback->GetDesc().Width)},all_wide{0,size_t(hdr_readback->GetDesc().Width)};
        checked(readback->Map(0,&all,&sdr),"D3D12 scene readback map");checked(hdr_readback->Map(0,&all_wide,&wide),"D3D12 HDR10 readback map");
        uint64_t mismatches=0;std::ostringstream details;std::unordered_map<uint32_t,uint32_t> encoded; // a frame repeats colors
        for(UINT y=0;y<target_height;++y){
            const auto* in=reinterpret_cast<const uint32_t*>(static_cast<const BYTE*>(sdr)+readback_layout.Offset+size_t(y)*readback_layout.Footprint.RowPitch);
            const auto* out=reinterpret_cast<const uint32_t*>(static_cast<const BYTE*>(wide)+hdr_readback_layout.Offset+size_t(y)*hdr_readback_layout.Footprint.RowPitch);
            for(UINT x=0;x<target_width;++x){
                const auto found=encoded.try_emplace(in[x]&0xffffff,0);
                if(found.second)found.first->second=mk5hdr::encode(in[x]&0xffffff,hdr_settings);
                const uint32_t want=found.first->second;bool wrong=false;
                for(int c=0;c<3;++c)wrong|=std::abs(int((want>>(10*c))&1023)-int((out[x]>>(10*c))&1023))>1;
                if(wrong&&mismatches++<8)details<<" ["<<x<<","<<y<<" got="<<std::hex<<out[x]<<" expected="<<want<<std::dec<<"]";
            }
        }
        readback->Unmap(0,&none);hdr_readback->Unmap(0,&none);hdr_readback.Reset();
        hdr_validation_mismatches=mismatches;++hdr_validation_frames;
        if(mismatches)throw std::runtime_error("D3D12 HDR10 validation mismatches="+std::to_string(mismatches)+details.str());
    }
    void validate_pixels(const D3D12_VIEWPORT& viewport){
        void* mapped=nullptr;D3D12_RANGE range{0,size_t(readback->GetDesc().Width)};
        checked(readback->Map(0,&range,&mapped),"D3D12 readback map");
        uint64_t mismatches=0,visible_bezel=0,subtexel_pixels=0,hash=14695981039346656037ull;unsigned max_error=0;std::ostringstream details;
        const unsigned scale=source_scale(),sample_width=image_width*scale,sample_height=image_height*scale;
        const bool reconstruction=filter!=mk5video::Sampling::nearest;
        if(source_capture.memory){validation_source=source_capture.pixels();source_capture.memory.Reset();}
        if(bilateral_amount)mk5video::reference_prefilter(validation_source,sample_width,sample_height,0,bilateral_amount);
        if(cas_amount)mk5video::reference_prefilter(validation_source,sample_width,sample_height,cas_amount,0);
        std::vector<float> linear_source;
        if(crt.enabled()&&!reconstruction){
            std::array<float,256> decode{};
            for(unsigned i=0;i<decode.size();++i)decode[i]=crt.phosphor()?mk5crt::srgb_decode(float(i)/255):std::pow(float(i)/255,crt.gamma_in);
            linear_source.resize(validation_source.size()*3);
            for(size_t i=0;i<validation_source.size();++i)for(unsigned c=0;c<3;++c)linear_source[i*3+c]=decode[(validation_source[i]>>(c*8))&255];
        }
        const auto pixel_index=[&](int x,int y){
            return size_t(std::clamp(y,0,int(sample_height)-1))*sample_width+std::clamp(x,0,int(sample_width)-1);
        };
        const auto linear_channel=[&](int x,int y,unsigned shift){return linear_source[pixel_index(x,y)*3+shift/8];};
        const auto encoded_pixel=[&](int x,int y){
            const uint32_t pixel=validation_source[pixel_index(x,y)];
            return std::array<float,3>{float((pixel>>16)&255)/255,float((pixel>>8)&255)/255,float(pixel&255)/255};
        };
        const auto sample=[&](float u,float v){
            return mk5video::reconstruct(filter,u,v,sample_width,sample_height,viewport.Width,viewport.Height,encoded_pixel);
        };
        const auto stage_texel=[&](float u,float v)->uint32_t{
            if(validation_stage.empty())return 0;
            const int w=int(image_width*mk5stage::scale),h=int(image_height*mk5stage::scale);
            const int x=std::clamp(mk5crt::sample_index(u*w),0,w-1);
            const int y=std::clamp(mk5crt::sample_index(v*h),0,h-1);
            return validation_stage[size_t(y)*w+x];
        };
        const auto stage_rgb=[](uint32_t pixel,std::array<float,3>& rgb){
            if(!(pixel&mk5stage::scenery))return false;
            rgb={float((pixel>>16)&255)/255,float((pixel>>8)&255)/255,float(pixel&255)/255};
            return true;
        };
        const auto stage_sample=[&](float u,float v,std::array<float,3>& rgb){
            return stage_rgb(stage_texel(u,v),rgb);
        };
        const auto linear_stage=[&](float u,float v,float fine_v,std::array<float,3>& rgb){
            auto pixel=stage_texel(u,v);
            if(fighters_scaled){
                const auto fine=stage_texel(u,fine_v);
                if((pixel>>24)>191 || (fine>>24)>191)pixel=fine;
            }
            if(!stage_rgb(pixel,rgb))return false;
            for(float& value:rgb)value=crt.phosphor()?mk5crt::srgb_decode(value):std::pow(value,crt.gamma_in);
            return true;
        };
        // Without curvature, the CRT beam samples share each column's cubic taps.
        const int left=std::max(0,int(std::ceil(viewport.TopLeftX-.5f)));
        const int right=std::min(int(target_width),int(std::floor(viewport.TopLeftX+viewport.Width)));
        const int columns=std::max(0,right-left);
        std::vector<std::array<float,3>> horizontal;
        if(crt.enabled()&&crt.curvature==0&&filter==mk5video::Sampling::bicubic&&columns){
            std::vector<mk5video::CubicAxis> axes;
            axes.reserve(columns);
            for(int x=left;x<right;++x)axes.push_back(mk5video::cubic_axis((float(x)+.5f-viewport.TopLeftX)/viewport.Width,sample_width));
            horizontal.resize(size_t(columns)*sample_height);
            for(unsigned y=0;y<sample_height;++y)for(int x=0;x<columns;++x)
                horizontal[size_t(y)*columns+x]=mk5video::cubic_axis_sample(axes[x],[&](int sx){return encoded_pixel(sx,y);});
        }
        const auto linear_sample=[&](float u,float v){
            std::array<float,3> rgb;
            if(horizontal.empty())rgb=sample(u,v);
            else{
                const int x=std::clamp(int(std::lround(viewport.TopLeftX+u*viewport.Width-.5f))-left,0,columns-1);
                rgb=mk5video::cubic_axis_sample(mk5video::cubic_axis(v,sample_height),[&](int sy){
                    return horizontal[size_t(std::clamp(sy,0,int(sample_height)-1))*columns+x];
                });
                for(float& value:rgb)value=std::clamp(value,0.f,1.f);
            }
            for(float& value:rgb)value=crt.phosphor()?mk5crt::srgb_decode(value):std::pow(value,crt.gamma_in);
            return rgb;
        };
        const unsigned tolerance=validation_tolerance();
        const bool color_rounding=tolerance==0&&color_adjusted();
        std::vector<std::array<float,3>> phosphor_row;
        for(UINT y=0;y<target_height;++y){auto* row=reinterpret_cast<const uint32_t*>(static_cast<const unsigned char*>(mapped)+readback_layout.Offset+size_t(y)*readback_layout.Footprint.RowPitch);
            if(crt.phosphor()&&y>=viewport.TopLeftY&&y<viewport.TopLeftY+viewport.Height){
                const int left=int(viewport.TopLeftX),relative_y=int(y)-int(viewport.TopLeftY),w=int(viewport.Width),h=int(viewport.Height);
                if(reconstruction||scale>1)mk5crt::phosphor_reference_row(crt,left,relative_y,w,h,image_width,image_height,linear_channel,phosphor_row,linear_sample,linear_stage);
                else mk5crt::phosphor_reference_row(crt,left,relative_y,w,h,image_width,image_height,linear_channel,phosphor_row,nullptr,linear_stage);
            }
            for(UINT x=0;x<target_width;++x){
                const float u=(float(x)+0.5f-viewport.TopLeftX)/viewport.Width,v=(float(y)+0.5f-viewport.TopLeftY)/viewport.Height;
                // Direct3D implicit viewport scissor floors each fractional extent.
                const bool inside=u>=0 && v>=0 && float(x)<std::floor(viewport.TopLeftX+viewport.Width) && float(y)<std::floor(viewport.TopLeftY+viewport.Height);bool wrong=(row[x]>>24)!=255;
                // The frame covers everything but the glass, the game's corners included.
                const uint32_t framed=bezel&&bezel_pixels.size()==size_t(target_width)*target_height?bezel_pixels[size_t(y)*target_width+x]:0;
                if(!inside||framed>>24){
                    const uint32_t expected=framed>>24?framed:0xff000000;
                    if((framed>>24)&&row[x]==expected)++visible_bezel;
                    if(row[x]!=expected){
                        if(mismatches<16)details<<" ["<<x<<","<<y<<" frame="<<std::hex<<row[x]<<" expected="<<expected<<std::dec<<"]";
                        ++mismatches;
                    }
                    continue;
                }
                hash=(hash^(row[x]&0xffffff))*1099511628211ull;
                // Blending is within one code on the GPU (D3D12 output merger precision): only where the overlay is translucent.
                const auto overlay_alpha=[&]{
                    if(validation_overlay.empty())return 0u;
                    const unsigned ox=std::min(unsigned(std::max(u,0.f)*float(image_width)),image_width-1),oy=std::min(unsigned(std::max(v,0.f)*float(image_height)),image_height-1);
                    return unsigned(validation_overlay[size_t(oy)*image_width+ox]>>24);
                }();
                const bool blended=overlay_alpha>0&&overlay_alpha<255;
                const auto expected_at=[&](float su,float sv){
                    std::array<float,3> rgb{};
                    if(crt.phosphor())rgb=phosphor_row[x-unsigned(viewport.TopLeftX)];
                    else if(crt.enabled()){
                        if(reconstruction)rgb=mk5crt::reference_rgb(crt,su,sv,float(x)+.5f,viewport.Height/image_height,image_width,image_height,linear_channel,scale,linear_sample,linear_stage);
                        else rgb=mk5crt::reference_rgb(crt,su,sv,float(x)+.5f,viewport.Height/image_height,image_width,image_height,linear_channel,scale,nullptr,linear_stage);
                    }else{
                        if(!stage_sample(su,sv,rgb))rgb=sample(su,sv);
                        if(mk5crt::sample_index(sv*image_height)&1)for(float& value:rgb)value*=1-scanlines;
                    }
                    rgb=mk5color::apply(rgb,color_mode,float(brightness)/100);
                    if(!validation_overlay.empty()){   // the overlay pass: its point-sampled texel over the stored 8-bit colour
                        const unsigned ox=std::min(unsigned(std::max(u,0.f)*float(image_width)),image_width-1),oy=std::min(unsigned(std::max(v,0.f)*float(image_height)),image_height-1);
                        const uint32_t o=validation_overlay[size_t(oy)*image_width+ox];const float a=float(o>>24)/255;
                        if(a>0)for(unsigned c=0;c<3;++c)rgb[c]=float(o>>(16-8*c)&255)/255*a+std::round(std::clamp(rgb[c],0.f,1.f)*255)/255*(1-a);
                    }
                    return rgb;
                };
                // Returns the largest channel error and whether every channel is within its limit.
                const auto compare=[&](const std::array<float,3>& expected_rgb,unsigned& worst){
                    bool ok=true;worst=0;
                    for(unsigned shift=0;shift<24;shift+=8){const float expected=expected_rgb[2-shift/8]*255;
                        const int actual=int((row[x]>>shift)&255);
                        const unsigned error=unsigned(std::abs(actual-int(std::lround(expected))));
                        worst=std::max(worst,error);
                        const unsigned limit=(crt.enabled()&&expected==0&&!prefilter_passes()&&!reconstruction?0u:tolerance)+(blended?1u:0u);
                        // Compare against the unrounded color so a whole-code error still fails.
                        if(error>limit&&!(color_rounding&&std::abs(actual-expected)<=D3D12_FLOAT32_TO_INTEGER_TOLERANCE_IN_ULP))ok=false;
                    }
                    return ok;
                };
                const auto primary=expected_at(u,v);unsigned error=0;bool ok=compare(primary,error);
                if(!ok&&!crt.phosphor()){
                    // Direct3D converts texture coordinates to fixed point with at least 8 fractional bits, so within 1/256 of
                    // a texel edge either neighbour is a conforming point sample. There only, try the reference with the
                    // position moved 1/256 texel to each side of that edge, on the textures the GPU samples (the source at the
                    // sample scale and the stage); shader arithmetic such as the scanline row is exact and is not moved.
                    const auto nudges=[&](float t,std::initializer_list<unsigned> grids){
                        std::vector<float> out{t};
                        for(unsigned n:grids){const float at=t*float(n),edge=std::round(at);
                            if(n&&std::abs(at-edge)<1.f/256){out.push_back((edge-1.f/256)/float(n));out.push_back((edge+1.f/256)/float(n));}}
                        return out;
                    };
                    const auto us=nudges(u,{sample_width,image_width*mk5stage::scale});
                    auto vs=nudges(v,{sample_height,image_height*mk5stage::scale});
                    // The scanline row is shader arithmetic, and GPU division is within 2.5 ulp: a row edge closer than 1e-4
                    // (a viewport height near a whole multiple of the source) may round to either row.
                    const float row_at=v*float(image_height)+1.f/512,row_edge=std::round(row_at);
                    if(scanlines>0&&std::abs(row_at-row_edge)<1e-4f)
                        for(const float side:{-2e-4f,2e-4f})vs.push_back((row_edge-1.f/512+side)/float(image_height));
                    for(size_t i=0;i<us.size()&&!ok;++i)for(size_t j=0;j<vs.size()&&!ok;++j){
                        if(!i&&!j)continue;
                        unsigned e=0;if(compare(expected_at(us[i],vs[j]),e)){ok=true;error=e;++subtexel_pixels;}
                    }
                }else if(!ok&&(reconstruction||scale>1)&&int(viewport.Height)/int(image_height)>=2){
                    // Phosphor: each beam of the triad is a filtered source sample, which the hardware takes at 1/256 texel
                    // precision; the energy one saturated lane spills into a dark one, sRGB encoded near black, turns that
                    // sub-code difference into several codes. Accept only a value between the references with every beam
                    // moved independently by -1/256, 0 and +1/256 texel (27 triads), within the usual tolerance.
                    const int left=int(viewport.TopLeftX),w=int(viewport.Width),h=int(viewport.Height),ry=int(y)-int(viewport.TopLeftY);
                    const int px=int(x)-left,lane=int(x)%3;const float step=1.f/(256.f*float(sample_width));
                    std::array<std::array<std::array<float,3>,3>,3> beams{};
                    for(int i=0;i<3;++i){const int bx=std::clamp(px-lane+i,0,w-1);
                        for(int k=0;k<3;++k)beams[i][k]=mk5crt::phosphor_beam(crt,bx,ry,w,h,image_width,image_height,linear_channel,linear_sample,linear_stage,float(k-1)*step);}
                    std::array<float,3> low{1.f,1.f,1.f},high{0.f,0.f,0.f};
                    for(int a=0;a<3;++a)for(int b=0;b<3;++b)for(int c=0;c<3;++c){
                        const auto rgb=mk5color::apply(mk5crt::phosphor_mask(crt,{beams[0][a],beams[1][b],beams[2][c]},lane),color_mode,float(brightness)/100);
                        for(unsigned ch=0;ch<3;++ch){low[ch]=std::min(low[ch],rgb[ch]);high[ch]=std::max(high[ch],rgb[ch]);}
                    }
                    bool inside=(row[x]>>24)==255;unsigned outside=0;
                    for(unsigned shift=0;shift<24;shift+=8){const unsigned ch=2-shift/8;const long actual=long((row[x]>>shift)&255);
                        const long lo=std::lround(low[ch]*255),hi=std::lround(high[ch]*255);
                        const unsigned beyond=unsigned(std::max({0l,lo-actual,actual-hi}));
                        outside=std::max(outside,beyond);if(beyond>tolerance)inside=false;
                    }
                    if(inside){ok=true;error=outside;++subtexel_pixels;}
                }
                max_error=std::max(max_error,error);
                if(!ok)wrong=true;
                if(wrong){if(mismatches<16){
                    unsigned want=0;for(unsigned c=0;c<3;++c)want=want<<8|unsigned(std::clamp(std::lround(primary[c]*255),0l,255l));
                    details<<" ["<<x<<","<<y<<" rgb="<<std::hex<<row[x]<<" expected="<<want<<std::dec<<" uv="<<u<<","<<v<<"]";}++mismatches;}
            }
        }
        D3D12_RANGE none{0,0};readback->Unmap(0,&none);readback.Reset();validation_source.clear();validation_stage.clear();validation_overlay.clear();
        validation_mismatches=mismatches;validation_max_error=max_error;validation_bezel_pixels=visible_bezel;validation_subtexel_pixels=subtexel_pixels;validation_image_hash=hash;++validation_frames;
        if(mismatches)throw std::runtime_error("D3D12 GPU pixel validation mismatches="+std::to_string(mismatches)+" max_error="+std::to_string(max_error)+details.str());
    }
    void draw(bool repeat){
        if(!targets[0] || !texture)return;
        wait_gpu();
        if(!bezel_dirty)bezel_upload.Reset();
        checked(allocator->Reset(),"D3D12 allocator reset");checked(commands->Reset(allocator.Get(),pipeline.Get()),"D3D12 command reset");
        const bool source_changed=upload_pending;
        if(upload_pending){
            copy_upload(texture.Get(),upload_buffer.Get(),upload_layout,texture_ready);
            if(stage_scaled)copy_upload(stage_texture.Get(),stage_upload.Get(),stage_layout,stage_texture_ready);
            if(overlay_pending){copy_upload(overlay_texture.Get(),overlay_upload.Get(),overlay_layout,overlay_ready);overlay_pending=false;}
            upload_pending=false;
        }
        const bool validation=validate_graphics && !validation_source.empty();
        const UINT input=scalefx||smaa?kChainSrv:kSourceSrv;
        if(source_changed||!scaled_ready){
            if(scalefx)scalefx->draw(commands.Get(),scalefx_amount);
            if(smaa)smaa->draw(commands.Get());
            if(prefilter_passes())draw_prefilters(input);
            scaled_ready=true;
        }
        if(validation){
            if(smaa)source_capture=smaa->capture(commands.Get());
            else if(scalefx)source_capture=scalefx->capture(commands.Get());
        }
        commands->SetPipelineState(pipeline.Get());
        const UINT index=swap->GetCurrentBackBufferIndex();auto* target=targets[index].Get();
        auto* drawn=hdr?scene.Get():target;const auto rest=hdr?D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_PRESENT;
        barrier(drawn,rest,D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto rtv=rtv_at(hdr?scene_rtv():index);
        const float black[4]={0,0,0,1};commands->OMSetRenderTargets(1,&rtv,FALSE,nullptr);commands->ClearRenderTargetView(rtv,black,0,nullptr);
        // A widescreen frame (16:9) fills the screen: no whole-line CRT height and no integer scale, which would leave a border.
        // Phosphor keeps whole lines (its raster draws each source line on height/lines rows: a height between whole lines
        // read past the last line, issue #6).
        const bool wide=across*3!=down*4;
        auto area=crt.phosphor()?mk5crt::phosphor_viewport(target_width,target_height,image_height,across,down):mk5crt::viewport(target_width,target_height,image_height,crt.enabled()&&!wide,across,down);
        mk5crt::Viewport bounds{0,0,float(target_width),float(target_height)};
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->SetGraphicsRootSignature(root.Get());
        if(bezel){
            area=bezel_area(tv,image_height,crt);bounds=area;
        }
        area=mk5video::scaled_viewport(bounds,area,image_width,image_height,wide?0:scale_mode,across,down);
        viewport_width=unsigned(area.width);viewport_height=unsigned(area.height);
        source_aspect=scale_mode>=1&&scale_mode<=4&&area.width>=image_width&&area.width*image_height==area.height*image_width;
        crt_integer_vertical=crt.enabled()&&std::fmod(area.height,float(image_height))==0;
        D3D12_VIEWPORT viewport{area.x,area.y,area.width,area.height,0,1};D3D12_RECT clip{0,0,LONG(target_width),LONG(target_height)};
        commands->RSSetViewports(1,&viewport);commands->RSSetScissorRects(1,&clip);
        auto* heap=srv_heap.Get();commands->SetDescriptorHeaps(1,&heap);
        commands->SetGraphicsRootDescriptorTable(0,srv_at(prefilter_passes()?kPrefilterSrv+prefilter_passes()-1:input));
        commands->SetGraphicsRootDescriptorTable(2,srv_at(stage_scaled?kStageSrv:kSourceSrv));
        const float settings[kShaderConstants]={float(image_height),scanlines,float(image_width),0,
            crt.scanline_strength,crt.beam_min,crt.beam_max,crt.mask_strength,crt.mask_scale,crt.horizontal_blend,crt.halation,crt.gamma_in,
             crt.gamma_out,crt.gain,crt.curvature,viewport.Height/image_height,crt.horizontal_sharpness,float(color_mode),float(brightness)/100,scalefx?3.f:0.f,
             viewport.TopLeftX,viewport.TopLeftY,viewport.Width,viewport.Height,float(decode_phosphor()),0,0,float(stage_scaled),float(fighters_scaled)};
        commands->SetGraphicsRoot32BitConstants(1,kShaderConstants,settings,0);commands->DrawInstanced(3,1,0,0);
        if(overlay_shown && overlay_pipeline){
            commands->SetPipelineState(overlay_pipeline.Get());commands->SetGraphicsRootDescriptorTable(0,srv_at(kOverlaySrv));
            commands->DrawInstanced(3,1,0,0);commands->SetPipelineState(pipeline.Get());
        }
        if(bezel)draw_bezel(); // after the game: the rounded glass corners cover the image corners
        if(validation)readback=copy_back(drawn,rest,readback_layout);
        else barrier(drawn,D3D12_RESOURCE_STATE_RENDER_TARGET,rest);
        if(hdr){
            barrier(target,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
            const auto output=rtv_at(index);commands->OMSetRenderTargets(1,&output,FALSE,nullptr);
            const D3D12_VIEWPORT full{0,0,float(target_width),float(target_height),0,1};commands->RSSetViewports(1,&full);
            commands->SetPipelineState(hdr_pipeline.Get());commands->SetGraphicsRootDescriptorTable(0,srv_at(kSceneSrv));
            const float levels[3]={hdr_settings.paper,hdr_settings.peak,hdr_settings.highlights};
            commands->SetGraphicsRoot32BitConstants(1,3,levels,0);commands->DrawInstanced(3,1,0,0);
            if(validation)hdr_readback=copy_back(target,D3D12_RESOURCE_STATE_PRESENT,hdr_readback_layout);
            else barrier(target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
        }
        LARGE_INTEGER draw_before{},draw_after{};
        if(validate_graphics)QueryPerformanceCounter(&draw_before);
        checked(commands->Close(),"D3D12 command close");ID3D12CommandList* list=commands.Get();in_flight=true;++fence_value;queue->ExecuteCommandLists(1,&list);checked(queue->Signal(fence.Get(),fence_value),"D3D12 queue fence signal");
        // Queue Present as soon as the GPU has work. Wait before reusing the allocator or upload buffers.
        if(validate_graphics){
            wait_gpu();QueryPerformanceCounter(&draw_after);
            draw_fence_us=uint64_t((draw_after.QuadPart-draw_before.QuadPart)*1000000/frequency);
            draw_fence_max_us=std::max(draw_fence_max_us.load(),draw_fence_us.load());
        }
        if(validation){
            if(hdr)validate_hdr();
            validate_pixels(viewport);
            if(scenery_scaled)++stage_validation_frames;
            if(fighters_scaled)++fighter_validation_frames;
            if(!stage_scaled)plain_validated=true;
        }
        const bool adaptive=sync_mode!=0&&tearing&&!exclusive;
        const auto before=GetTickCount64();HRESULT hr=swap->Present(adaptive?0:1,adaptive?DXGI_PRESENT_ALLOW_TEARING:0);last_present_result=uint32_t(hr);
        max_present_ms=std::max<uint64_t>(max_present_ms.load(),GetTickCount64()-before);
        if(hr==DXGI_STATUS_OCCLUDED){++occluded_presents;occluded_retry=GetTickCount64()+kOccludedRetryMs;repaint=true;return;}
        checked(hr,"DXGI Present");++displayed;if(repeat)++repeats;
        if(scenery_scaled&&source_changed)++stage_frames;
        if(fighters_scaled&&source_changed)++fighter_frames;
        repaint=false;occluded_retry=0;
        LARGE_INTEGER stamp{};QueryPerformanceCounter(&stamp);{std::lock_guard<std::mutex> lock(stats_mutex);if(displayed>=60)display_timing.observe(stamp.QuadPart,frequency);}repeat_clock.presented(stamp.QuadPart);
    }
    void run(){
        try{
            if(physical_pixels()&&!SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
                throw std::runtime_error("D3D12 pixel-aligned presentation requires per-monitor DPI awareness");
            WNDCLASSW c{};c.lpfnWndProc=proc;c.hInstance=GetModuleHandleW(nullptr);c.lpszClassName=L"Mk5TemporalScanout";c.hCursor=LoadCursorW(nullptr,IDC_ARROW);
            if(!RegisterClassW(&c) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("MK5 window registration failed");
            RECT rect{0,0,LONG(800*across*3/(down*4)),508};AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);
            hwnd=CreateWindowExW(0,c.lpszClassName,L"TriUnit Recomp | Alt+Enter fullscreen exclusivo | Esc",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,c.hInstance,this);
            if(!hwnd)throw std::runtime_error("MK5 window creation failed");
            if(fullscreen){MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);GetMonitorInfoW(MonitorFromWindow(hwnd.load(),MONITOR_DEFAULTTONEAREST),&monitor);const auto& r=monitor.rcMonitor;SetWindowLongPtrW(hwnd.load(),GWL_STYLE,WS_POPUP|WS_VISIBLE);SetWindowPos(hwnd.load(),HWND_TOP,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_FRAMECHANGED);}
            mk5hid::shared().listen(hwnd.load());graphics();ShowWindow(hwnd.load(),SW_SHOW);UpdateWindow(hwnd.load());resize_buffers();
            {std::lock_guard<std::mutex> lock(state_mutex);ready=true;}ready_cv.notify_one();
            while(running){
                MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
                if(!running)break;
                if(close_requested){
                    if(in_flight)wait_gpu();
                    running=false;break;
                }
                if(leave_exclusive){leave_exclusive=false;if(exclusive || placement_saved)change_mode(false);}
                if(toggle){toggle=false;if(!leave_exclusive)change_mode(!exclusive.load());}
                if(!leave_exclusive){
                    if(resize)resize_buffers();
                    if(!IsIconic(hwnd.load())&&(!occluded_retry||GetTickCount64()>=occluded_retry)){
                        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
                        const bool due=high_refresh&&texture&&repeat_clock.due(now.QuadPart);
                        // Never block the producer on DXGI. Acquire a present slot
                        // before taking the newest mailbox frame, not after it.
                        if(!present_slot)present_slot=WaitForSingleObject(frame_ready,0)==WAIT_OBJECT_0;
                        if(present_slot&&(!high_refresh||due||!texture||repaint)){bool fresh=upload();if(texture&&(fresh||repaint||due)){draw(!fresh&&due);present_slot=false;}}
                    }
                }
                const auto now=GetTickCount64();
                const DWORD cooldown=occluded_retry>now?DWORD(occluded_retry-now):0;
                const bool visible=!leave_exclusive&&!IsIconic(hwnd.load())&&!cooldown;
                const bool wait_slot=visible&&!present_slot;
                const bool wait_refresh=visible&&present_slot&&high_refresh&&texture;
                if(wait_refresh){
                    LARGE_INTEGER stamp{};QueryPerformanceCounter(&stamp);
                    arm_pace_timer(refresh_timer,LONGLONG(repeat_clock.next)-stamp.QuadPart,frequency);
                }
                // An occluded swap chain can signal immediately even when it cannot display.
                HANDLE waits[2]={wake,wait_slot?frame_ready:refresh_timer};
                const DWORD count=wait_slot||wait_refresh?2:1;
                const DWORD timeout=cooldown?cooldown:leave_exclusive?4:INFINITE;
                const DWORD waited=MsgWaitForMultipleObjectsEx(count,waits,timeout,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
                if(waited==WAIT_FAILED)throw std::runtime_error("MK5 presentation event wait failed");
                if(wait_slot&&waited==WAIT_OBJECT_0+1)present_slot=true;
            }
        }catch(const std::exception& e){
            {std::lock_guard<std::mutex> lock(state_mutex);failure=e.what();ready=true;}running=false;ready_cv.notify_one();
        }
        if(compose_wake)SetEvent(compose_wake);
        retire_gpu();
        if(swap){swap->SetFullscreenState(FALSE,nullptr);exclusive=false;}
        if(hwnd)DestroyWindow(hwnd.load());
    }
    void check_failure(){std::lock_guard<std::mutex> lock(state_mutex);if(!failure.empty())throw std::runtime_error(failure);}
};
Mk5Window::Mk5Window(bool immediate_headroom,unsigned across,unsigned down):impl(std::make_unique<Impl>()){
    impl->immediate_headroom=immediate_headroom;impl->across=across;impl->down=down;
    impl->settings();
    if(across*3!=down*4)impl->bezel=false;   // the TV frame's glass is 4:3
    LARGE_INTEGER frequency{};if(!QueryPerformanceFrequency(&frequency)||frequency.QuadPart<=0)throw std::runtime_error("MK5 presentation clock failed");
    impl->frequency=frequency.QuadPart;impl->interval=(impl->frequency*146234+4000000)/8000000;
    impl->pace_timer=make_pace_timer();
    if(impl->high_refresh){
        impl->refresh_timer=make_pace_timer();
        impl->compose_wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    }
    impl->wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!impl->pace_timer || !impl->wake || (impl->high_refresh&&(!impl->refresh_timer||!impl->compose_wake)))throw std::runtime_error("MK5 presentation events failed");
    impl->ui=std::thread([this]{impl->run();});
    {std::unique_lock<std::mutex> lock(impl->state_mutex);impl->ready_cv.wait(lock,[this]{return impl->ready;});}
    try{
        impl->check_failure();
        if(impl->high_refresh)impl->compositor=std::thread([this]{impl->compose_frames();});
    }catch(...){impl->running=false;SetEvent(impl->wake);impl->ui.join();throw;}
}
Mk5Window::~Mk5Window(){
    impl->running=false;SetEvent(impl->wake);if(impl->hwnd)PostMessageW(impl->hwnd.load(),WM_CANCELMODE,0,0);
    if(impl->ui.joinable())impl->ui.join();
    if(impl->compositor.joinable())impl->compositor.join();
}
bool Mk5Window::Pump(){impl->check_failure();return impl->running;}
std::vector<Mk5Window::StateRequest> Mk5Window::TakeStateRequests(){
    std::lock_guard<std::mutex> lock(impl->request_mutex);return std::exchange(impl->state_requests,{});
}
BootDiInputs Mk5Window::Inputs(BootDiInputs base)const{
    std::array<bool,256> keys{};for(size_t i=0;i<keys.size();++i)keys[i]=impl->keys[i].load();
    const auto& c=impl->controls;std::array<uint32_t,2> held{{mk5controls::from_keys(c,0,keys),mk5controls::from_keys(c,1,keys)}};
    if(impl->focused){
        // Players without an XInput pad take the HID controllers in arrival order.
        impl->pads.poll();const auto hid=mk5hid::shared().pads();size_t next=0;
        for(unsigned p=0;p<2;++p){
            if(const auto* g=impl->pads.player(p))held[p]|=mk5controls::from_pad(c,p,mk5pad::buttons(*g),mk5pad::directions(*g));
            else if(next<hid.size()){held[p]|=mk5controls::from_pad(c,p,hid[next].buttons,hid[next].directions);++next;}
        }
        impl->controller_count=impl->pads.count()+unsigned(hid.size());
    }else impl->controller_count=0;
    for(unsigned p=0;p<2;++p){held[p]=mk5controls::clean(held[p],c.socd,impl->socd[p]);mk5training::held[p]=held[p];}
    base.in01=mk5controls::shared(held[0],held[1],base.in01,true);
    return base;
}
void Mk5Window::Present(const std::vector<uint32_t>& pixels,unsigned width,unsigned height,
                        const std::vector<uint32_t>& backdrop,
                        const mk5presentation::FighterFrame& fighters,const std::vector<mk5training::Box>& boxes){
    impl->check_failure();if(!impl->running)return;
    if(!width || !height || pixels.size()!=size_t(width)*height)throw std::runtime_error("MK5 window requires complete RGB frame");
    if(!backdrop.empty()&&backdrop.size()!=pixels.size())throw std::runtime_error("MK5 window requires complete backdrop frame");
    // The game's thread runs a step above normal: the picture's workers and the rest of the desktop do not hold a frame.
    if(!impl->producer_raised){impl->producer_raised=true;SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);}
    {LARGE_INTEGER entry{};QueryPerformanceCounter(&entry);std::lock_guard<std::mutex> lock(impl->stats_mutex);
        if(impl->work_from&&impl->frames>=60){impl->work_timing.duration(uint64_t(entry.QuadPart-impl->work_from),uint64_t(impl->frequency));
            impl->late_frames+=impl->next_due&&entry.QuadPart>impl->next_due;}}
    if(!(impl->legacy_pacing&&mk5audio::GameAudioUsesOutputClock())){
    // Native raster clock, independent of audio packet boundaries. Never skip guest work.
    LARGE_INTEGER now{};QueryPerformanceCounter(&now);
    // Run-ahead guard: the game keeps pace while its audio queue holds the startup reserve. 4 frames in a row late with
    // the queue under 75%, or 8 frames in a row under half of it, mean the speculative frame does not fit: run-ahead is
    // off for this session, before any underrun.
    if(impl->controls.runAhead && !impl->ahead_stopped && impl->frames>=60){
        const int reserve=mk5audio::output_reserve_percent;
        impl->ahead_late=impl->next_due>now.QuadPart?0:impl->ahead_late+1;
        impl->ahead_low=reserve>=0 && reserve<50?impl->ahead_low+1:0;
        impl->ahead_stopped=impl->ahead_low>=8 || (impl->ahead_late>=4 && reserve>=0 && reserve<75);
    }
    if(impl->next_due>now.QuadPart){
        arm_pace_timer(impl->pace_timer,impl->next_due-now.QuadPart,impl->frequency);
        if(WaitForSingleObject(impl->pace_timer,INFINITE)!=WAIT_OBJECT_0)throw std::runtime_error("MK5 presentation wait failed");
        QueryPerformanceCounter(&now);
    }
    const bool output_clock=mk5audio::GameAudioUsesOutputClock();const int reserve=mk5audio::output_reserve_percent;
    impl->next_due=LONGLONG(mk5timing::ProducerDeadline(uint64_t(impl->next_due),uint64_t(now.QuadPart),
                                                        mk5timing::ProducerInterval(uint64_t(impl->interval),output_clock,reserve),output_clock,reserve>=100));
    }
    {std::lock_guard<std::mutex> lock(impl->frame_mutex);
        auto& pending=impl->pending;
        if(pending.ready)++impl->coalesced;
        if(fighters.layers.empty()){
            pending.pixels=pixels;pending.backdrop=backdrop;
            pending.fighters.layers.clear();
        }
        else pending.fighters=fighters;
        pending.width=width;pending.height=height;pending.ready=true;
        if(impl->controls.training){
            for(unsigned p=0;p<2;++p)impl->history.Push(p,mk5training::held[p]);
            mk5training::Draw(pending.overlay,width,height,impl->history,boxes);
        }
    }
    LARGE_INTEGER stamp{};QueryPerformanceCounter(&stamp);{std::lock_guard<std::mutex> lock(impl->stats_mutex);if(impl->frames>=60)impl->producer_timing.observe(stamp.QuadPart,impl->frequency);}
    impl->work_from=stamp.QuadPart;
    ++impl->frames;SetEvent(impl->compose_wake?impl->compose_wake:impl->wake);
}
void Mk5Window::Frontier(uint32_t pc){if(impl->hwnd)PostMessageW(impl->hwnd.load(),kFrontier,pc,0);}
uint64_t Mk5Window::frames()const{return impl->frames;}
bool Mk5Window::RunAhead()const{return impl->controls.runAhead && !impl->ahead_stopped;}
bool Mk5Window::Training()const{return impl->controls.training;}
uint64_t Mk5Window::input_events()const{return impl->events;}
bool Mk5Window::GraphicsValidationEnabled()const{return impl->validate_graphics;}
std::string Mk5Window::presentation_json()const{
    std::lock_guard<std::mutex> lock(impl->stats_mutex);
    std::ostringstream s;s<<"{\"renderer\":\"D3D12-DXGI-flip\",\"vsync\":"<<((impl->sync_mode==0||!impl->tearing||impl->exclusive)?"true":"false")<<",\"sync_requested\":"<<impl->sync_mode<<",\"tearing_supported\":"<<(impl->tearing?"true":"false")<<",\"vrr_driver_monitor_verified\":false,\"fullscreen_start\":"<<(impl->fullscreen?"true":"false")<<",\"bezel\":"<<(impl->bezel?"true":"false")<<",\"color_mode\":"<<impl->color_mode<<",\"brightness\":"<<impl->brightness<<",\"gamepads_connected\":"<<impl->controller_count<<",\"gamepad_poll_ms\":4,\"native_frame_ms\":18.27925,\"high_refresh\":"<<(impl->high_refresh?"true":"false")<<",\"run_ahead\":\""<<(!impl->controls.runAhead?"off":impl->ahead_stopped?"stopped":"on")<<"\""<<",\"audio_reserve_percent\":"<<mk5audio::output_reserve_percent.load()<<",\"repeated_frames\":"<<impl->repeats<<",\"producer_intervals\":"<<impl->producer_timing.json()<<",\"producer_work_ms\":"<<impl->work_timing.json()<<",\"late_frames\":"<<impl->late_frames<<",\"present_call_intervals\":"<<impl->display_timing.json()<<",\"ui_thread_isolated\":true,\"exclusive\":"<<(impl->exclusive?"true":"false")
      <<",\"submitted_frames\":"<<impl->frames<<",\"displayed_frames\":"<<impl->displayed<<",\"coalesced_host_frames\":"<<impl->coalesced
      <<",\"frame_queue_limit\":"<<impl->frame_queue_limit
      <<",\"frame_preparation_ms\":"<<impl->prepare_timing.json()
      <<",\"mode_transitions\":"<<impl->toggles<<",\"mode_errors\":"<<impl->mode_errors<<",\"last_mode_hresult\":"<<impl->last_mode_error
      <<",\"occluded_present_attempts\":"<<impl->occluded_presents<<",\"last_present_hresult\":"<<impl->last_present_result<<",\"max_present_ms\":"<<impl->max_present_ms
      <<",\"filter\":\""<<mk5video::sampling_names[size_t(impl->filter)]<<"\",\"scanlines\":"<<impl->scanlines
      <<",\"scale_mode\":"<<impl->scale_mode<<",\"viewport_width\":"<<impl->viewport_width<<",\"viewport_height\":"<<impl->viewport_height
      <<",\"cas_amount\":"<<impl->cas_amount<<",\"bilateral_amount\":"<<impl->bilateral_amount<<",\"smaa\":"<<(impl->smaa_enabled?"true":"false")
      <<","<<impl->crt.json(impl->crt_integer_vertical)<<",\"source_width\":"<<impl->reported_source_width<<",\"source_height\":"<<impl->reported_source_height
      <<",\"draw_submit_fence_us\":"<<impl->draw_fence_us<<",\"draw_submit_fence_max_us\":"<<impl->draw_fence_max_us
      <<",\"gpu_reuse_wait_max_us\":"<<impl->gpu_reuse_wait_max_us
      <<",\"scalefx_amount\":"<<impl->scalefx_amount<<",\"scalefx_passes\":"<<(impl->scalefx_amount?5:0)
      // The summary is in whole codes. Color rounding is checked at fractional precision.
      <<",\"gpu_validation_tolerance\":"<<std::max(impl->validation_tolerance(),unsigned(impl->color_adjusted()))
      <<",\"gpu_validation_frames\":"<<impl->validation_frames<<",\"gpu_validation_mismatches\":"<<impl->validation_mismatches<<",\"gpu_validation_max_error\":"<<impl->validation_max_error<<",\"gpu_validation_bezel_pixels\":"<<impl->validation_bezel_pixels<<",\"gpu_validation_subtexel_pixels\":"<<impl->validation_subtexel_pixels
      <<",\"bicubic_stage_frames\":"<<impl->stage_frames<<",\"bicubic_stage_validation_frames\":"<<impl->stage_validation_frames
      <<",\"bicubic_fighter_frames\":"<<impl->fighter_frames
      <<",\"bicubic_fighter_validation_frames\":"<<impl->fighter_validation_frames
      <<",\"gpu_validation_image_hash\":"<<impl->validation_image_hash
      <<",\"hdr\":\""<<std::array<const char*,4>{"off","hdr10","windows-sdr","unsupported"}[size_t(impl->hdr_state.load())]<<"\""
      <<",\"hdr_paper_nits\":"<<impl->hdr_settings.paper<<",\"hdr_peak_nits\":"<<impl->hdr_settings.peak<<",\"hdr_highlights\":"<<impl->hdr_settings.highlights
      <<",\"hdr_validation_frames\":"<<impl->hdr_validation_frames<<",\"hdr_validation_mismatches\":"<<impl->hdr_validation_mismatches
      <<",\"move_size_enter\":"<<impl->move_enter<<",\"move_size_exit\":"<<impl->move_exit<<",\"physical_aspect\":\""<<(impl->source_aspect?"source":impl->across*3==impl->down*4?"4:3":"wide")<<"\"}";return s.str();
}
