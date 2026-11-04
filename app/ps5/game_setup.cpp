#include "game_setup.h"
#include "iso_import.h"
#include "display.h"
#include "graphics_backend.h"
#include "renderer/engine/simple_mesh.h"
#include "runtime/native/input_ps5.h"
#include "runtime/native/xex_crypto_portable.h"
#include "ppc_image_metadata.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>

extern "C" int sceSystemServiceHideSplashScreen(void);

namespace DarkRecomp::Ps5 {
namespace {
namespace fs = std::filesystem;
namespace Pad = Native::Ps5Pad;
// Small original bitmap alphabet for setup; no game font or system font required.
constexpr uint8_t glyphs[][7] = {
 {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
 {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
 {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
 {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
 {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
 {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
 {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
 {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
 {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
 {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
 {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
 {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
 {14,17,17,15,1,1,14},
 {0,0,0,31,0,0,0},{0,0,0,0,0,0,31},{0,0,0,0,0,12,12},
 {0,12,12,0,12,12,0},{1,2,2,4,8,8,16},{17,2,4,8,17,0,0},
 {14,17,1,2,4,0,4},{0,4,2,31,2,4,0}
};
const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.:/%?>";
static_assert(sizeof(glyphs)/sizeof(glyphs[0]) == 44);
SimpleMesh mesh() {
    SimpleMesh m;
    m.projection = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    return m;
}
void quad(SimpleMesh& m,float x,float y,float w,float h,float u=0,float v=0,float uw=1,float vh=1,
          float r=1,float g=1,float b=1,float a=1) {
    uint16_t i = static_cast<uint16_t>(m.vertices.size());
    const float l=x/960-1, top=1-y/540, right=(x+w)/960-1, bottom=1-(y+h)/540;
    m.vertices.insert(m.vertices.end(),{
        {{l,bottom,0},{u,v+vh},{r,g,b,a}},{{right,bottom,0},{u+uw,v+vh},{r,g,b,a}},
        {{right,top,0},{u+uw,v},{r,g,b,a}},{{l,top,0},{u,v},{r,g,b,a}}});
    for(uint16_t k:{0,1,2,0,2,3}) m.indices.push_back(i+k);
}
class Screen {
    Display& display;
    GraphicsBackend& graphics;
    std::shared_ptr<ColorImage> art, white;
    std::shared_ptr<AlphaImage> font;
    std::shared_ptr<AlphaImage> playLabel, exitLabel;
    uint16_t previous=0;
    bool hidden=false;
public:
    Screen(Display& d,GraphicsBackend& g,const fs::path& artwork="/app0/setup-menu.rgba"):display(d),graphics(g) {
        white=std::make_shared<ColorImage>();white->width=white->height=1;white->pixels={255,255,255,255};
        font=std::make_shared<AlphaImage>();font->width=512;font->height=16;font->pixels.resize(512*16);
        for(size_t c=0;c<alphabet.size();++c)
            for(unsigned y=0;y<7;++y)for(unsigned x=0;x<5;++x)
                if(glyphs[c][y]&(16>>x)) font->pixels[y*512+c*6+x]=255;
        std::ifstream in(artwork,std::ios::binary);
        uint32_t dims[2]{};
        if(in.read(reinterpret_cast<char*>(dims),sizeof(dims)) && dims[0] && dims[1] && dims[0]<=2048 && dims[1]<=2048) {
            auto image=std::make_shared<ColorImage>();image->width=dims[0];image->height=dims[1];
            image->pixels.resize(size_t(dims[0])*dims[1]*4);
            if(in.read(reinterpret_cast<char*>(image->pixels.data()),image->pixels.size())) {
                art=image;
                // Lift only the lettering into UI masks so the original type
                // stays intact when the selection moves off the baked Play row.
                auto label=[&](unsigned x,unsigned y,unsigned width,unsigned height) {
                    auto mask=std::make_shared<AlphaImage>();mask->width=width;mask->height=height;
                    mask->pixels.resize(width*height);
                    if(x+width>art->width || y+height>art->height)return mask;
                    for(unsigned row=0;row<height;++row)for(unsigned col=0;col<width;++col) {
                        const auto* p=&art->pixels[((y+row)*art->width+x+col)*4];
                        const int light=std::min({int(p[0]),int(p[1]),int(p[2])});
                        mask->pixels[row*width+col]=uint8_t(std::clamp((light-100)*255/120,0,255));
                    }
                    return mask;
                };
                playLabel=label(140,446,140,34);
                exitLabel=label(140,511,120,38);
            }
        }
    }
    uint16_t pressed() {
        Pad::PadState p{};
        if(!Pad::readPad(p))return 0;
        uint16_t result=p.buttons&~previous;previous=p.buttons;return result;
    }
    void text(std::vector<SimpleMesh>& frame,const std::string& line,float x,float y,float scale=4) {
        auto m=mesh();m.texture=font;
        for(unsigned char c:line.substr(0,96)) {
            if(c>='a' && c<='z')c-='a'-'A';
            auto index=alphabet.find(c);
            if(index!=std::string::npos)quad(m,x,y,5*scale,7*scale,float(index*6)/512,0,5.0f/512,7.0f/16);
            x+=6*scale;
        }
        if(!m.indices.empty())frame.push_back(std::move(m));
    }
    void finish(std::vector<SimpleMesh>& frame) {
        graphics.render(frame,{true,true});
        if(!hidden){sceSystemServiceHideSplashScreen();hidden=true;}
        if(graphics.error())throw std::runtime_error(graphics.error());
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    void menu(bool exitSelected) {
        std::vector<SimpleMesh> frame;
        auto bg=mesh();bg.colorTexture=art?art:white;
        quad(bg,0,0,1920,1080,0,0,1,1,art?1:.025f,art?1:.04f,art?1:.06f);
        frame.push_back(std::move(bg));
        if(art && exitSelected) {
            const float sx=1920.0f/art->width,sy=1080.0f/art->height;
            // Quiet the baked Play highlight, then draw the live gold band at Exit.
            auto inactive=mesh();inactive.colorTexture=white;
            quad(inactive,0,429*sy,300*sx,65*sy,0,0,1,1,.015f,.02f,.025f,1);
            const size_t fade=inactive.vertices.size();
            quad(inactive,300*sx,429*sy,350*sx,65*sy,0,0,1,1,.015f,.02f,.025f,1);
            inactive.vertices[fade+1].color[3]=inactive.vertices[fade+2].color[3]=0;
            frame.push_back(std::move(inactive));
            auto band=mesh();band.colorTexture=white;
            quad(band,0,499*sy,540*sx,65*sy,0,0,1,1,.65f,.42f,.12f,.42f);
            // Smooth fade into the artwork instead of a rectangular panel.
            band.vertices[1].color[3]=band.vertices[2].color[3]=0;
            frame.push_back(std::move(band));
            auto rules=mesh();rules.colorTexture=white;
            quad(rules,0,499*sy,540*sx,sy,0,0,1,1,.88f,.63f,.25f,.65f);
            rules.vertices[1].color[3]=rules.vertices[2].color[3]=0;
            const size_t next=rules.vertices.size();
            quad(rules,0,563*sy,540*sx,sy,0,0,1,1,.88f,.63f,.25f,.65f);
            rules.vertices[next+1].color[3]=rules.vertices[next+2].color[3]=0;
            frame.push_back(std::move(rules));
            auto play=mesh();play.texture=playLabel;
            quad(play,140*sx,446*sy,140*sx,34*sy,0,0,1,1,.85f,.85f,.85f);
            frame.push_back(std::move(play));
            auto exit=mesh();exit.texture=exitLabel;
            quad(exit,140*sx,511*sy,120*sx,38*sy,0,0,1,1,1,.92f,.75f);
            frame.push_back(std::move(exit));
            auto arrow=mesh();arrow.colorTexture=white;
            // Six points form the same narrow chevron as the artwork.
            for(const auto& point:std::array<std::array<float,2>,6>{{{93,518},{99,518},{109,530},{99,542},{93,542},{103,530}}})
                arrow.vertices.push_back({{point[0]*sx/960-1,1-point[1]*sy/540,0},{0,0},{1,.86f,.57f,1}});
            arrow.indices={0,1,5,1,2,5,2,3,5,3,4,5};
            frame.push_back(std::move(arrow));
        } else if(!art) {
            text(frame,"THE DARKNESS RECOMP",140,300,6);
            text(frame,exitSelected?"  PLAY":"> PLAY",140,500,5);
            text(frame,exitSelected?"> EXIT":"  EXIT",140,580,5);
        }
        finish(frame);
    }
    void present(const std::string& title,const std::vector<std::string>& lines,double progress=-1) {
        std::vector<SimpleMesh> frame;
        auto bg=mesh();bg.colorTexture=art?art:white;
        quad(bg,0,0,1920,1080,0,0,1,1,art?1:.025f,art?1:.04f,art?1:.06f);
        frame.push_back(std::move(bg));
        auto panel=mesh();panel.colorTexture=white;
        quad(panel,60,690,1800,330,0,0,1,1,.015f,.02f,.03f,.94f);frame.push_back(std::move(panel));
        text(frame,title,90,715);
        float y=765;
        for(const auto& line:lines){text(frame,line,90,y,3);y+=35;}
        if(progress>=0) {
            auto bar=mesh();bar.colorTexture=white;
            quad(bar,90,965,1740,12,0,0,1,1,.12f,.12f,.12f);
            quad(bar,90,965,float(1740*std::clamp(progress,0.0,1.0)),12,0,0,1,1,.85f,.65f,.25f);
            frame.push_back(std::move(bar));
        }
        finish(frame);
    }
};
void verify(std::span<const uint8_t> bytes) {
    const auto hash=Native::XexCrypto::Sha256Hex(bytes);
    for(const auto& revision:kGameRevisions) if(hash==revision.xexSha256)return;
    throw std::runtime_error("Unsupported game version. Use a supported Xbox 360 disc dump.");
}
bool installed(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path/"default.xex",ec);
}
}

bool runGameSetup(Display& display,GraphicsBackend& graphics,fs::path& gameDirectory) {
    Screen screen(display,graphics);
    const fs::path persistent="/data/homebrew/PPSA99301/Darkness";
    const fs::path fallback="/download0/darkness-ps5/Darkness";
    if(!installed(gameDirectory)) {
        if(installed(persistent))gameDirectory=persistent;
        else if(installed(fallback))gameDirectory=fallback;
    }
    std::vector<fs::path> roots{"/app0","/data/homebrew/PPSA99301","/mnt/usb0","/mnt/usb1"};
    std::vector<fs::path> isos;
    size_t choice=0;
    bool exitSelected=false, scan=true;
    std::string error;
    for(;;) {
        if(scan){isos=findGameIsos(roots);choice=0;scan=false;}
        const bool ready=installed(gameDirectory);
        std::vector<std::string> lines;
        if(ready)lines={exitSelected?"  PLAY     > EXIT":"> PLAY       EXIT","X: CONFIRM    UP/DOWN: SELECT    CIRCLE: EXIT"};
        else if(isos.empty())lines={"PLACE YOUR XBOX 360 ISO BESIDE EBOOT.BIN OR ON USB.","X: SCAN AGAIN    CIRCLE: EXIT","NO GAME FILES ARE INCLUDED WITH THIS APP."};
        else lines={isos[choice].filename().string(),"X: EXTRACT AND PLAY    UP/DOWN: CHOOSE ISO    CIRCLE: EXIT","KEEP THE ISO IN PLACE UNTIL EXTRACTION FINISHES."};
        if(!error.empty()) {
            lines.push_back(error.substr(0,96));
            if(error.size()>96)lines.push_back(error.substr(96,96));
        }
        if(ready)screen.menu(exitSelected);
        else screen.present("GAME SETUP",lines);
        const auto buttons=screen.pressed();
        if(buttons&Pad::kB)return false;
        if(buttons&(Pad::kDpadUp|Pad::kDpadDown)) {
            if(ready)exitSelected=!exitSelected;
            else if(!isos.empty())choice=(choice+isos.size()+((buttons&Pad::kDpadUp)?-1:1))%isos.size();
        }
        if(!(buttons&Pad::kA))continue;
        if(ready)return !exitSelected;
        if(isos.empty()){scan=true;continue;}
        error.clear();
        struct State {std::mutex mutex;uint64_t done=0,total=0;std::string file="Checking ISO and game version",error;bool finished=false;fs::path output; } state;
        const auto source=isos[choice];
        std::thread worker([&] {
            try {
                const auto plan=inspectGameIso(source,verify);
                fs::path output=persistent;
                // /app0 is read-only. Prefer the app's actual /data folder; use writable app data otherwise.
                std::error_code ec;
                fs::create_directories(output.parent_path(),ec);
                const auto probe=output.parent_path()/".darkness-import-write-probe";
                // Only remove a probe we created. Never truncate an existing file.
                const bool created=fs::create_directory(probe,ec);
                if(created)fs::remove(probe,ec);
                else output=fallback;
                fs::create_directories(output.parent_path());
                extractGameIso(plan,output,[&](uint64_t done,uint64_t total,const std::string& file) {
                    std::lock_guard lock(state.mutex);state.done=done;state.total=total;state.file=file;
                });
                std::lock_guard lock(state.mutex);state.output=output;
            } catch(const std::exception& e) {std::lock_guard lock(state.mutex);state.error=e.what();}
            std::lock_guard lock(state.mutex);state.finished=true;
        });
        try {
            for(;;) {
                uint64_t done,total;std::string file;bool finished;
                {std::lock_guard lock(state.mutex);done=state.done;total=state.total;file=state.file;finished=state.finished;}
                if(finished)break;
                std::string amount=std::to_string(done/(1024*1024))+" / "+std::to_string(total/(1024*1024))+" MB";
                screen.present("IMPORTING YOUR GAME",{file,amount,"PLEASE WAIT. YOUR ORIGINAL ISO WILL BE KEPT."},total?double(done)/total:0);
            }
        } catch(...) {worker.join();throw;}
        worker.join();
        if(!state.error.empty()) {error=state.error;std::fprintf(stderr,"[Import] %s\n",error.c_str());}
        else {gameDirectory=state.output;return true;}
    }
}
}
