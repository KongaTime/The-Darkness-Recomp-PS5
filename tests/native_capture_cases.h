#include <random>
namespace DarkRecomp::Native {
void retainCompletedVertexBinding(uint8_t*,uint32_t) noexcept;
bool testPreparedBindingValidation(uint8_t*,bool) noexcept;
bool testVertexSourceReadMatches(uint8_t*) noexcept;
}
namespace {
constexpr uint32_t nativeDevice=0x06000000,nativeProgram=0x06100000,nativeName=0x06100100;
thread_local unsigned nativeMutationRow=0;
void nativeScene(uint8_t* base,unsigned scene) {
    std::memset(base+kAttributes,0,160);base[kAttributes+97]=8;
    put32(base,kContext+17152,0);put32(base,kContext+17156,0);
    put32(base,kContext+17160,1280);put32(base,kContext+17164,720);
    putFloat(base,kContext+17168,0);putFloat(base,kContext+17172,1);
    put32(base,kContext+17964,0);
    std::memset(base+nativeDevice+12432,0,20);
    put32(base,nativeDevice+10564,0);
    if(scene==1 || scene==2) {
        put32(base,kAttributes+92,0x00100000);
        for(unsigned i=0;i<scene;++i)put16(base,kAttributes+8+i*2,uint16_t(i+1));
    } else if(scene>=3) {
        const char* name=scene==3?"XRShader_MotionMap":scene==4?"XRShader_FP20_NDSP":"XRShader_FP20_NDS";
        put32(base,kAttributes,nativeProgram);put32(base,nativeProgram,5);
        put32(base,nativeProgram+4,nativeName);put32(base,nativeProgram+16,scene==3?4:34);
        std::memset(base+nativeName,0,96);std::memcpy(base+nativeName,name,std::strlen(name));
        put16(base,kAttributes+8,1);put16(base,kAttributes+10,2);
    }
    for(unsigned s=0;s<16;++s) {
        std::memset(base+nativeDevice+1152+s*24,0,24);
        put32(base,nativeDevice+1152+s*24,2);
        put32(base,nativeDevice+1152+s*24+12,(1u<<19)|(1u<<21)|(1u<<23));
        put32(base,nativeDevice+1152+s*24+16,5u<<6);
        const auto object=0x06200000+s*128;
        put32(base,nativeDevice+12536+s*4,s<2?object:0);
        std::memset(base+object,0,64);
        put32(base,object+32,0x07000000+s*4096+6);put32(base,object+36,31|(31<<13));
        put32(base,object+44,5u<<6);
    }
    for(unsigned row=0;row<64;++row)for(unsigned lane=0;lane<4;++lane)
        put32(base,nativeDevice+6016+row*16+lane*4,row%5==0?0x7fc01234u:std::bit_cast<uint32_t>(float(row+lane)));
    if(scene==1 || scene==2)for(unsigned lane=0;lane<4;++lane)putFloat(base,nativeDevice+6016+lane*4,1);
}
StoredDraw nativeGeometry() {
    auto geometry=std::make_shared<StoredGeometry>();geometry->id=1;
    geometry->vertexCount=3;geometry->stride=12;geometry->formats[0]=3;
    geometry->vertices.resize(36);geometry->indices={0,1,2};geometry->maximumIndex=2;
    return {geometry,geometry,0,3};
}
void nativeKey(EngineVertexBindingSnapshot& b) {
    const auto encoded=encodeEngineVertexDescriptor(b.descriptor);
    for(unsigned i=0;i<5;++i)b.key[i+1]=uint32_t(encoded[i*4])<<24|uint32_t(encoded[i*4+1])<<16|
        uint32_t(encoded[i*4+2])<<8|encoded[i*4+3];
}
void retainNativeFixture(uint8_t* base,unsigned scene=0,bool weighted=false,bool second=false) {
    constexpr uint32_t stack=0x06501000,model=0x06300000,record=0x06400000,shader=0x06410000;
    nativeScene(base,scene);
    const auto& modes=engineVertexModes();
    for(unsigned i=0;i<modes.size();++i) {put32(base,0x82A5CD88+i*4,modes[i].flags);base[0x82A5CDF4+i]=modes[i].reservation;}
    EngineVertexProgramInput input;input.declarationFlags=0x400;input.rendererFlags=0x100000;input.modes.fill(4);
    input.materialFlags=(scene==1 || scene==2)?0x00100000:0;
    input.palette=weighted;input.declarationAddress=weighted?0x06601000:0;input.secondBlendIndices=weighted && second;
    put32(base,kContext+8224,model);put32(base,kContext+8232,0);
    put32(base,model+16+640,weighted?0x06600000:0);put32(base,model+16+644,0);
    put32(base,kContext+16420,input.declarationFlags);put32(base,kContext+16428,0);
    put32(base,kContext+16528,input.declarationAddress);put32(base,kContext+17180,input.rendererFlags);
    if(weighted)base[input.declarationAddress+24]=second?17:0;
    put32(base,kContext+17184,0);put32(base,kContext+17256,0);
    for(unsigned i=0;i<8;++i) {base[kContext+16944+i]=4;base[kContext+16952+i]=0;}
    EngineVertexProgramKey key;require(buildEngineVertexProgramKey(input,key),"Native fixture key refused");
    EngineVertexDescriptor descriptor;descriptor.coordinateMapping=key[1];descriptor.flags=key[2];
    descriptor.modes.fill(4);descriptor.declarationFlags=input.declarationFlags;
    const auto encoded=encodeEngineVertexDescriptor(descriptor);
    std::memcpy(base+stack-192,encoded.data(),encoded.size());
    std::memset(base+record,0,48);put32(base,record+8,shader);
    for(unsigned i=0;i<6;++i) {put32(base,stack-224+i*4,key[i]);put32(base,record+16+i*4,key[i]);}
    put32(base,kContext+15748,nativeDevice);put32(base,kContext+17188,record);
    put32(base,nativeDevice+12688,shader);
    for(unsigned row=0;row<256;++row)for(unsigned lane=0;lane<4;++lane)putFloat(base,nativeDevice+1920+row*16+lane*4,float(row+lane));
    retainCompletedVertexBinding(base,stack);
    EngineVertexBindingSnapshot retained;
    require(snapshotEngineVertexBindings(base,retained),"Native fixture did not retain a valid original binding");
}
void testPreparedSourceValidation(uint8_t* base) {
    unsigned checks=0,accepted=0,rejected=0;
    for(unsigned variant=0;variant<3;++variant) {
        retainNativeFixture(base,0,variant!=0,variant==2);
        require(testPreparedBindingValidation(base,true) && testPreparedBindingValidation(base,false),
                "Source validation fixture did not retain a binding");
        const auto check=[&](uint32_t address,unsigned size) {
            for(unsigned byte=0;byte<size;++byte)for(unsigned bit=0;bit<8;++bit) {
                base[address+byte]^=uint8_t(1u<<bit);
                const bool canonical=testPreparedBindingValidation(base,true);
                const bool compact=testPreparedBindingValidation(base,false);
                const bool sameSource=testVertexSourceReadMatches(base);
                base[address+byte]^=uint8_t(1u<<bit);
                require(sameSource,"Compact source acquisition changed canonical fields or rejection output");
                require(canonical==compact,"Compact source validation changed a source-bit acceptance decision");
                ++checks;(canonical?accepted:rejected)++;
                require(testPreparedBindingValidation(base,false),"Source validation did not recover after restoration");
            }
        };
        check(kContext+8224,12);check(kContext+16420,840);
        check(0x06300000+16+640,8);
        check(kContext+15748,4);check(kContext+17188,4);check(nativeDevice+12688,4);
        if(variant)check(0x06601000+24,1);
    }
    require(accepted>10000 && rejected>1000,"Source oracle did not cover ignored bits and stale state");
    require(!testPreparedBindingValidation(nullptr,false),"Null mapping accepted a prepared binding");
    compactVertexSourcesEnabled=false;
    require(testPreparedBindingValidation(base,false) && testVertexSourceReadMatches(base),
            "Original-source switch did not retain the completed binding");
    compactVertexSourcesEnabled=true;
    require(testPreparedBindingValidation(base,false),"Compact-source switch did not recover the completed binding");
    std::printf("Compact source validation: %u bit mutations, %u unchanged/%u rejected match canonical reconstruction.%c",
                checks,accepted,rejected,10);
}
void testNativeCapture(uint8_t* base) {
    testPreparedSourceValidation(base);
    const auto geometry=nativeGeometry();std::mt19937 random(0x9831b);
    unsigned accepted=0,refused=0;
    EngineVertexBindingSnapshot randomBinding;randomBinding.deviceAddress=nativeDevice;
    for(unsigned trial=0;trial<8000;++trial) {
        auto& d=randomBinding.descriptor;
        if(trial%4==0) {
        d.flags=0x01000000|((random()%2?4:0)<<16);
        d.palette=uint8_t(random()%105);d.color=uint8_t(random());d.positionConversion=uint8_t(random());
        if(random()&1)d.flags|=0x04000000;
        for(unsigned s=0;s<8;++s) {
            constexpr uint8_t choices[]{0,1,4};d.modes[s]=choices[random()%3];
            d.parameters[s][0]=uint8_t(random());d.matrices[s]=uint8_t(random());d.conversions[s]=uint8_t(random());
        }
        }
        nativeKey(randomBinding);nativeScene(base,trial%6);
        for(unsigned row=0;row<256;++row)for(unsigned lane=0;lane<4;++lane)
            put32(base,nativeDevice+1920+row*16+lane*4,random()&0xfeffffffu);
        if(trial%19==0)put32(base,nativeDevice+1920,0x7fc01234u);
        if(trial%23==0)randomBinding.key[1]^=1;
        std::memcpy(randomBinding.constantBytes.data(),base+nativeDevice+1920,4096);
        auto originalGeometry=geometry;originalGeometry.vertexBindings=randomBinding;
        vertexProgramPlansEnabled=false;
        const auto original=captureWorldDraw(base,originalGeometry);
        vertexProgramPlansEnabled=true;
        const auto native=testCaptureNativeBinding(base,randomBinding,geometry);
        require(bool(original)==bool(native),"Native capture acceptance differs from original strict capture");
        if(original) {++accepted;require(equivalentWorldCaptures(*original,*native),"Native capture changed owned world state");}
        else ++refused;
    }
    vertexProgramPlansEnabled=false;
    retainNativeFixture(base,2,true);
    auto weightedOriginal=geometry;
    require(captureEngineVertexBindings(base,weightedOriginal.vertexBindings),"Weighted retained binding lost");
    const auto weightedReference=captureWorldDraw(base,weightedOriginal);
    auto weightedInput=geometry;const auto weightedNative=captureNativeWorldDraw(base,weightedInput);
    require(weightedReference && weightedNative && equivalentWorldCaptures(*weightedReference,*weightedNative),
            "Live weighted capture changed the 156-row palette");
    // Disturb the last byte of each row after preparation: every marked row
    // must reject, while every unused row is irrelevant to this draw.
    auto rowBinding=*weightedOriginal.vertexBindings;rowBinding.descriptor.color=255;
    WorldVertexOptions rowOptions;WorldVertexConstants rowConstants;
    require(prepareWorldVertexProgram(rowBinding,rowOptions,rowConstants),"Row guard fixture refused");
    nativeCaptureBeforeValidation=[](uint8_t* b){b[nativeDevice+1920+nativeMutationRow*16+15]^=1;};
    for(nativeMutationRow=0;nativeMutationRow<256;++nativeMutationRow) {
        std::memcpy(base+nativeDevice+1920,rowBinding.constantBytes.data(),4096);
        const bool used=(rowConstants.written[nativeMutationRow/64]>>(nativeMutationRow%64))&1;
        require(bool(testCaptureNativeBinding(base,rowBinding,geometry))==!used,"Native row guard missed a consumed row or checked an unused row");
    }
    nativeCaptureBeforeValidation=nullptr;
    retainNativeFixture(base);
    EngineVertexBindingSnapshot binding;require(snapshotEngineVertexBindings(base,binding),"Retained binding lost");
    nativeCaptureBeforeValidation=[](uint8_t* b){putFloat(b,nativeDevice+1920,123);};
    auto input=geometry;const auto* owner=input.vertices.get();
    require(!captureNativeWorldDraw(base,input) && input.vertices.get()==owner,"Changed used row was published or consumed geometry");
    nativeCaptureBeforeValidation=nullptr;
    // Used rows are reread; writes outside the prepared mask have no effect.
    nativeCaptureBeforeValidation=[](uint8_t* b){putFloat(b,nativeDevice+1920+255*16,789);};
    input=geometry;require(bool(captureNativeWorldDraw(base,input)) && !input.vertices,"Unused row prevented direct capture or owners were not transferred");
    nativeCaptureBeforeValidation=nullptr;
    nativeCaptureBeforeValidation=[](uint8_t* b){put32(b,kContext+17188,0);};
    input=geometry;require(!captureNativeWorldDraw(base,input) && input.vertices.get()==owner,"Changed selected binding was published");
    nativeCaptureBeforeValidation=nullptr;retainNativeFixture(base);
    // Reject after writing several rows, then force pool reuse and compare all
    // 256 output rows to the original. This catches dirty partial-bank leakage.
    nativeCaptureBeforeValidation=[](uint8_t* b){putFloat(b,nativeDevice+1920,999);};
    input=geometry;require(!captureNativeWorldDraw(base,input),"Partial capture rejection fixture accepted");
    nativeCaptureBeforeValidation=nullptr;
    auto originalInput=geometry;require(captureEngineVertexBindings(base,originalInput.vertexBindings),"Partial capture fallback binding lost");
    const auto original=captureWorldDraw(base,originalInput);input=geometry;
    const auto recycled=captureNativeWorldDraw(base,input);
    require(original && recycled && equivalentWorldCaptures(*original,*recycled),"Rejected native draw leaked dirty rows into pool reuse");
    // The reference must bypass recipe lookups even while the optimization is
    // enabled; otherwise comparison would test the cache against itself.
    vertexProgramPlansEnabled=true;
    const auto lookupsBefore=vertexProgramPlanLookups.load();
    for(unsigned i=0;i<2048;++i)require(bool(captureWorldDrawCanonical(base,originalInput)),"Canonical reference refused valid geometry");
    require(vertexProgramPlanLookups.load()==lookupsBefore,"Canonical reference consulted the recipe cache");
    // Exercise queue publication, original fallback, and the live comparison
    // safety latch through the real unbound producer entry.
    enableEnginePreview();nativeWorldCaptureEnabled=true;nativeWorldCaptureCompare=true;nativeWorldCaptureSafe=true;
    previewObserveWorldUnbound(base,StoredDraw(geometry));previewEndFrame();
    std::vector<SimpleMesh> commands;
    require(takePreviewFrame(commands) && commands.size()==1 && commands[0].world && nativeWorldCaptureSafe,
            "Native comparison producer failed queue publication");commands.clear();
    nativeCaptureAfterValidation=[](WorldDraw& draw){draw.fragmentFlags^=1;};
    previewObserveWorldUnbound(base,StoredDraw(geometry));previewEndFrame();
    require(takePreviewFrame(commands) && commands.size()==1 && !nativeWorldCaptureSafe && commands[0].world->fragmentFlags==0,
            "Native mismatch did not publish original and latch safety off");commands.clear();nativeCaptureAfterValidation=nullptr;
    require(!vertexProgramPlansSafe,"Native mismatch left recipe optimization active in original fallback");
    const auto before=nativeWorldCaptureStats();
    previewObserveWorldUnbound(base,StoredDraw(geometry));previewEndFrame();
    require(takePreviewFrame(commands) && commands.size()==1 && nativeWorldCaptureStats().attempts==before.attempts,
            "Latched native path retried instead of original fallback");commands.clear();
    nativeWorldCaptureEnabled=false;nativeWorldCaptureCompare=false;nativeWorldCaptureSafe=true;vertexProgramPlansEnabled=false;vertexProgramPlansSafe=true;
    std::printf("Native capture: %u exact owned world-state matches/%u refusals; used rows/source races, pool cleanup, ownership, queue comparison and sticky fallback pass.\n",accepted,refused);
}
}
