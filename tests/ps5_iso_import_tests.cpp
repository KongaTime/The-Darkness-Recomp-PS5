#include "app/ps5/iso_import.h"
#include "runtime/native/xex_crypto_portable.h"
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <cstdlib>
#include <unistd.h>

namespace fs=std::filesystem;
using namespace DarkRecomp::Ps5;
void check(bool ok) {if(!ok)throw std::runtime_error("test assertion failed");}
template<class F> void rejects(F f) {bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed);}
void put32(uint8_t* b,uint32_t v) {for(int i=0;i<4;++i)b[i]=v>>(8*i);}
void put16(uint8_t* b,uint16_t v) {b[0]=v;b[1]=v>>8;}
void writeAt(const fs::path& p,uint64_t at,const void* data,size_t bytes) {
    std::fstream out(p,std::ios::binary|std::ios::in|std::ios::out);
    out.seekp(at);out.write(static_cast<const char*>(data),bytes);check(bool(out));
}
std::string read(const fs::path& p) {std::ifstream in(p,std::ios::binary);return {(std::istreambuf_iterator<char>(in)),{}};}
void entry(std::array<uint8_t,2048>& table,unsigned at,const std::string& name,uint32_t sector,uint32_t bytes,bool directory=false,uint16_t right=0) {
    auto* b=table.data()+at;put16(b+2,right);put32(b+4,sector);put32(b+8,bytes);b[12]=directory?16:0;b[13]=name.size();
    std::copy(name.begin(),name.end(),b+14);
}
fs::path makeIso(const fs::path& root,const std::string& name,uint64_t base=0) {
    auto iso=root/name;
    {std::ofstream out(iso,std::ios::binary);out.seekp(base+80*2048-1);out.put(0);}
    std::array<uint8_t,2048> header{};
    std::string magic="MICROSOFT*XBOX*MEDIA";
    std::copy(magic.begin(),magic.end(),header.begin());std::copy(magic.begin(),magic.end(),header.end()-20);
    put32(header.data()+20,40);put32(header.data()+24,2048);
    writeAt(iso,base+0x10000,header.data(),header.size());
    std::array<uint8_t,2048> table{};
    entry(table,0,"Default.XEX",50,32,false,8);
    entry(table,32,"Content",41,2048,true,16);
    entry(table,64,"$SystemUpdate",0,0,true);
    writeAt(iso,base+40*2048,table.data(),table.size());
    table.fill(0);entry(table,0,"test.bin",51,11);
    writeAt(iso,base+41*2048,table.data(),table.size());
    const std::string exe(32,'X');writeAt(iso,base+50*2048,exe.data(),exe.size());
    writeAt(iso,base+51*2048,"hello world",11);
    return iso;
}
int main() {
    auto root=fs::temp_directory_path()/("darkness-iso-tests-"+std::to_string(getpid()));
    fs::create_directory(root);
    auto verify=[](std::span<const uint8_t> b){check(b.size()==32 && b[0]=='X');};
    try {
        auto iso=makeIso(root,"game.ISO");
        auto plan=inspectGameIso(iso,verify);check(plan.bytes==43 && plan.entries.size()==3);
        auto output=root/"Darkness";
        uint64_t progress=0;
        extractGameIso(plan,output,[&](uint64_t n,uint64_t total,const std::string&){check(n>=progress && n<=total);progress=n;});
        check(progress==43 && read(output/"Content/test.bin")=="hello world" && read(output/"default.xex")==std::string(32,'X'));
        check(!fs::exists(output/"$SystemUpdate"));
        rejects([&]{extractGameIso(plan,output);});check(read(output/"Content/test.bin")=="hello world");
        rejects([&]{inspectGameIso(iso,[](auto){throw std::runtime_error("unsupported XEX");});});
        rejects([&]{inspectGameIso(iso,{});});
        auto interrupted=root/"Interrupted";
        rejects([&]{extractGameIso(plan,interrupted,[](auto,auto,const auto&){throw std::runtime_error("power interrupted");});});
        check(!fs::exists(interrupted) && fs::exists(root/"Interrupted.importing"));
        extractGameIso(plan,interrupted);check(read(interrupted/"Content/test.bin")=="hello world");
        auto collision=root/"Unknown.importing";fs::create_directory(collision);
        {std::ofstream out(collision/"keep.txt");out<<"keep";}
        rejects([&]{extractGameIso(plan,root/"Unknown");});check(read(collision/"keep.txt")=="keep");
        fs::create_directory_symlink(output,root/"Link.importing");
        rejects([&]{extractGameIso(plan,root/"Link");});check(fs::exists(output/"default.xex"));
        auto changed=root/"Changed";writeAt(iso,50*2048,"Y",1);
        rejects([&]{extractGameIso(plan,changed);});check(!fs::exists(changed));
        for(uint64_t base:{0x0fd90000ull,0x02080000ull,0x18300000ull}) {
            auto p=makeIso(root,"offset.iso",base);check(inspectGameIso(p,verify).bytes==43);fs::remove(p);
        }
        auto bad=makeIso(root,"bad.iso");
        const std::string unsafe="../oops.bin";writeAt(bad,40*2048+14,unsafe.data(),unsafe.size());
        rejects([&]{inspectGameIso(bad,verify);});
        bad=makeIso(root,"bad.iso");uint8_t pointer[2];put16(pointer,8);writeAt(bad,40*2048+32+2,pointer,2);
        rejects([&]{inspectGameIso(bad,verify);}); // repeated sibling entry
        bad=makeIso(root,"bad.iso");uint8_t huge[4];put32(huge,0xffffffff);writeAt(bad,40*2048+4,huge,4);
        rejects([&]{inspectGameIso(bad,verify);});
        bad=makeIso(root,"bad.iso");fs::resize_file(bad,0x10010);rejects([&]{inspectGameIso(bad,verify);});
        fs::create_symlink(iso,root/"linked.iso");
        const auto found=findGameIsos({root,root,root/"missing"});check(found.size()==2);
        if(const char* real=std::getenv("DARKNESS_IMPORT_REAL_XEX")) {
            const auto original=read(real);
            check(original.size()>24 && original.size()<64*1024*1024);
            auto realIso=makeIso(root,"real-xex.iso");
            fs::resize_file(realIso,64*2048+original.size());
            uint8_t field[4];put32(field,64);writeAt(realIso,40*2048+4,field,4);
            put32(field,original.size());writeAt(realIso,40*2048+8,field,4);
            writeAt(realIso,64*2048,original.data(),original.size());
            auto realPlan=inspectGameIso(realIso,[](auto bytes) {
                check(DarkRecomp::Native::XexCrypto::Sha256Hex(bytes)=="aace35a8f9bcdc7f28aeab9ff8cf3bdf200353f5c83705f6284487347acb3c5f");
            });
            extractGameIso(realPlan,root/"RealExecutable");
            check(read(root/"RealExecutable/default.xex")==original);
            std::cout<<"User-supplied original XEX: SHA-256 gate and byte-identical ISO extraction pass.\n";
        }
        fs::remove_all(root);
        std::cout<<"ISO import: layouts, nested content, XEX gate, progress, interruption/retry, overwrite/symlink protection, same-size changes, traversal, cycles, bounds and discovery pass.\n";
    }catch(...){fs::remove_all(root);throw;}
}
