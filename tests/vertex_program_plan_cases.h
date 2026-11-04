#pragma once
#include "renderer/engine/world_mesh.h"
#include <bit>
#include <cstring>
#include <random>
#include <thread>

static void testVertexProgramPlans() {
    using namespace DarkRecomp::Native;
    auto key=[](EngineVertexBindingSnapshot& b) {
        const auto bytes=encodeEngineVertexDescriptor(b.descriptor);
        for(unsigned i=0;i<5;++i)b.key[i+1]=uint32_t(bytes[i*4])<<24|uint32_t(bytes[i*4+1])<<16|
            uint32_t(bytes[i*4+2])<<8|bytes[i*4+3];
    };
    auto fill=[](EngineVertexBindingSnapshot& b,unsigned seed) {
        for(unsigned n=0;n<1024;++n) {
            const uint32_t bits=std::bit_cast<uint32_t>(float(int(seed%100)-50)+float(n%19)*0.25f);
            for(unsigned byte=0;byte<4;++byte)b.constantBytes[n*4+byte]=uint8_t(bits>>(24-byte*8));
        }
    };
    auto same=[](const WorldVertexConstants& a,const WorldVertexConstants& b) {
        return std::memcmp(a.vectors.data(),b.vectors.data(),sizeof(a.vectors))==0 &&
            a.references==b.references && a.written==b.written;
    };
    std::mt19937 random(98177);
    unsigned accepted=0,refused=0;
    for(unsigned descriptor=0;descriptor<2500;++descriptor) {
        EngineVertexBindingSnapshot binding;
        auto& d=binding.descriptor;
        d.coordinateMapping=random();d.flags=(random()&0x00f0ffffu)|((random()%9)<<16)|0x01000000;
        if(random()%2)d.flags|=0x04000000;
        d.palette=uint8_t(random()%105);d.color=uint8_t(random());d.positionConversion=uint8_t(random());
        constexpr uint8_t modes[]{0,1,4};
        for(unsigned s=0;s<8;++s) {
            d.modes[s]=modes[random()%3];d.conversions[s]=uint8_t(random());d.matrices[s]=uint8_t(random());
            for(auto& parameter:d.parameters[s])parameter=uint8_t(random());
        }
        for(unsigned repeat=0;repeat<4;++repeat) {
            key(binding);fill(binding,descriptor*4+repeat);
            if(repeat==2 && descriptor%3==0) {binding.constantBytes[0]=0x7f;binding.constantBytes[1]=0x80;binding.constantBytes[2]=binding.constantBytes[3]=0;}
            if(repeat==3 && descriptor%11==0)binding.key[1]^=1;
            WorldVertexOptions expectedOptions,actualOptions;
            WorldVertexConstants expected,actual;
            expected.vectors[255][0]=actual.vectors[255][0]=-0.0f;
            vertexProgramPlansEnabled=false;
            const bool oldResult=prepareWorldVertexProgram(binding,expectedOptions,expected);
            vertexProgramPlansEnabled=true;
            const bool newResult=prepareWorldVertexProgram(binding,actualOptions,actual);
            require(oldResult==newResult && expectedOptions==actualOptions && same(expected,actual),
                "Vertex plan changed constants, failure output, validation or written mask");
            (newResult?accepted:refused)++;
        }
    }
    require(accepted>1000 && refused>1000 && vertexProgramPlanHits.load()>0,"Vertex plan oracle did not cover hits and failures");
    for(unsigned stage=0;stage<8;++stage)for(unsigned mode=0;mode<27;++mode) {
        EngineVertexBindingSnapshot binding;auto& d=binding.descriptor;
        d.flags=0x01e200ff;d.modes.fill(4);d.modes[stage]=uint8_t(mode);
        d.conversions.fill(76);d.matrices.fill(40);
        for(auto& parameters:d.parameters)parameters.fill(20);
        key(binding);
        for(unsigned repeat=0;repeat<3;++repeat) {
            fill(binding,stage*27+mode+repeat);
            WorldVertexOptions oldOptions,newOptions;WorldVertexConstants oldConstants,newConstants;
            vertexProgramPlansEnabled=false;
            const bool oldResult=prepareWorldVertexProgram(binding,oldOptions,oldConstants);
            vertexProgramPlansEnabled=true;
            const bool newResult=prepareWorldVertexProgram(binding,newOptions,newConstants);
            require(oldResult==newResult && oldOptions==newOptions && same(oldConstants,newConstants),
                "Vertex plan changed a mode/stage-specific constant range or rejection");
        }
    }
    // Distinct threads keep distinct plans. Each iteration changes the actual
    // constants; a plan must never return cached transform/color values.
    auto worker=[&](unsigned id) {
        EngineVertexBindingSnapshot binding;binding.descriptor.modes.fill(4);
        binding.descriptor.color=uint8_t(10+id);key(binding);
        for(unsigned n=0;n<500;++n) {
            fill(binding,n+id);WorldVertexOptions options;WorldVertexConstants constants;
            require(prepareWorldVertexProgram(binding,options,constants),"Thread-local plan refused valid constants");
            const float expected=float(int((n+id)%100)-50);
            require(constants.vectors[0][0]==expected,"Vertex plan reused another draw or thread's values");
        }
    };
    std::thread first(worker,1),second(worker,2);first.join();second.join();
    vertexProgramPlansEnabled=true;
    std::printf("Vertex plans: %u accepted/%u refused match original path; changed banks, keys, descriptors, finite validation, masks and thread isolation pass.\n",accepted,refused);
}
