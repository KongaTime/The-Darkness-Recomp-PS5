#include "vertex_preparation_capture.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
using Capture=DarkRecomp::Ps5::VertexPreparationCapture;
static void require(bool value,const char* message) {
    if(!value)throw std::runtime_error(message);
}
int main(int argc,char** argv) {
    try {
        require(argc==2,"temporary directory required");
        const std::filesystem::path root=argv[1];
        Capture capture(root);bool mode=true;
        const auto apply=[&](bool enabled){mode=enabled;};
        Capture::Counters counters;
        const auto command=[&](const std::string& text){std::ofstream(root/"vertex-preparation.request")<<text;};
        int64_t now=0;
        for(unsigned run=0;run<2;++run) {
            const auto id=std::string("run")+std::to_string(run);
            command(id+" "+std::to_string(run)+" 1000 1000");
            require(capture.frame(now,mode,counters,apply)==Capture::Result::armed,"request did not arm");
            require(mode==bool(run),"mode not applied");
            now+=1000000000;capture.frame(now,mode,counters,apply);
            for(unsigned frame=0;frame<50;++frame) {
                now+=20000000;++counters.real;counters.draws+=13;
                const auto result=capture.frame(now,mode,counters,apply);
                require(result==(frame==49?Capture::Result::completed:Capture::Result::idle),"capture boundary changed");
            }
            require(mode,"previous mode not restored");
            std::ifstream csv(root/("vertex-preparation-"+id+".csv"));std::string line;
            require(bool(std::getline(csv,line)) && line.find("duration_ns=1000000000")!=std::string::npos,"metadata missing");
            std::getline(csv,line);unsigned rows=0;
            while(std::getline(csv,line)) {
                require(line.find(",20000000,")!=std::string::npos,"raw frame interval changed");++rows;
            }
            require(rows==50,"fixed duration captured the wrong frames");
            now+=1000000000;
        }
        command("../escape 0 1000 1000");
        require(capture.frame(now,mode,counters,apply)==Capture::Result::failed && mode,"unsafe run id changed mode");
        now+=1000000000;command("bad 0 999999 1000");
        require(capture.frame(now,mode,counters,apply)==Capture::Result::failed && mode,"unbounded duration accepted");
        now+=1000000000;command("interrupted 0 1000 1000");
        require(capture.frame(now,mode,counters,apply)==Capture::Result::armed && !mode,"interruption fixture failed");
        mode=true;now+=1000000000;
        require(capture.frame(now,mode,counters,apply)==Capture::Result::failed && mode,"external mode change was not refused");
        require(!std::filesystem::exists(root/"vertex-preparation-interrupted.csv"),"mixed-mode result published");
        now+=1000000000;command("run0 0 1000 1000");
        require(capture.frame(now,mode,counters,apply)==Capture::Result::failed && mode,"existing capture overwritten");
        std::puts("Vertex capture: equal durations, raw intervals, restoration, bounds, interruption and output preservation pass.");
    }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}
}
