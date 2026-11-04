#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace DarkRecomp::Ps5 {
class VertexPreparationCapture {
public:
    struct Counters {
        uint64_t real{},generated{},draws{},rejected{};
        bool timers{},sampling{};
    };
    enum class Result { idle,armed,completed,failed };
private:
    struct Sample { int64_t elapsed,interval;Counters counters; };
    std::filesystem::path root_;
    std::string id_;
    std::vector<Sample> samples_;
    Counters initial_{};
    int64_t nextPoll_{},warmupEnd_{},start_{},previous_{},duration_{};
    bool active_{},measuring_{},mode_{},previousMode_{};
    void error(const char* message) const {
        std::ofstream file(root_/"vertex-preparation-error.txt",std::ios::app);
        file<<id_<<' '<<message<<char(10);
    }
    void write(int64_t boundaryNs) {
        const auto final=root_/("vertex-preparation-"+id_+".csv");
        const auto temporary=std::filesystem::path(final.string()+".part");
        if(std::filesystem::exists(final) || std::filesystem::exists(temporary))
            throw std::runtime_error("capture output already exists");
        std::ofstream file(temporary);
        file<<"# mode="<<unsigned(mode_)<<" duration_ns="<<duration_
            <<" boundary_elapsed_ns="<<boundaryNs-start_<<" initial_real="<<initial_.real
            <<" initial_generated="<<initial_.generated<<" initial_draws="<<initial_.draws
            <<" initial_rejected="<<initial_.rejected<<char(10);
        file<<"elapsed_ns,interval_ns,real_frames,generated_frames,world_draws,rejected,timers,sampling"<<char(10);
        for(const auto& sample:samples_) {
            const auto& c=sample.counters;
            file<<sample.elapsed<<','<<sample.interval<<','<<c.real<<','<<c.generated<<','<<c.draws
                <<','<<c.rejected<<','<<unsigned(c.timers)<<','<<unsigned(c.sampling)<<char(10);
        }
        file.close();
        if(!file)throw std::runtime_error("capture write failed");
        std::filesystem::rename(temporary,final);
    }
public:
    explicit VertexPreparationCapture(std::filesystem::path root):root_(std::move(root)) {}
    template<class Apply>
    Result frame(int64_t now,bool currentMode,const Counters& counters,Apply apply) noexcept {
        try {
            if(!active_) {
                if(now<nextPoll_)return Result::idle;
                nextPoll_=now+1000000000;
                std::ifstream request(root_/"vertex-preparation.request");
                if(!request)return Result::idle;
                unsigned mode,durationMs,warmupMs;
                std::string extra;
                const bool parsed=bool(request>>id_>>mode>>durationMs>>warmupMs) && !(request>>extra);
                request.close();
                std::filesystem::remove(root_/"vertex-preparation.request");
                bool validId=!id_.empty() && id_.size()<=40;
                for(const unsigned char ch:id_)
                    validId=validId && ((ch>='a' && ch<='z') || (ch>='A' && ch<='Z') ||
                        (ch>='0' && ch<='9') || ch=='-' || ch=='_');
                if(!parsed || !validId || mode>1 || durationMs<1000 || durationMs>120000 ||
                   warmupMs<1000 || warmupMs>30000) {
                    error("invalid request");return Result::failed;
                }
                if(std::filesystem::exists(root_/("vertex-preparation-"+id_+".csv"))) {
                    error("duplicate run id");return Result::failed;
                }
                samples_.clear();samples_.reserve(120000);
                previousMode_=currentMode;mode_=mode!=0;duration_=int64_t(durationMs)*1000000;
                warmupEnd_=now+int64_t(warmupMs)*1000000;measuring_=false;active_=true;
                apply(mode_);return Result::armed;
            }
            if(currentMode!=mode_)throw std::runtime_error("capture mode changed externally");
            if(!measuring_) {
                if(now<warmupEnd_)return Result::idle;
                start_=previous_=now;initial_=counters;measuring_=true;return Result::idle;
            }
            if(now<=previous_)throw std::runtime_error("non-monotonic frame timestamp");
            if(now-start_<=duration_) {
                if(samples_.size()==samples_.capacity())throw std::runtime_error("capture sample limit exceeded");
                samples_.push_back({now-start_,now-previous_,counters});
            }
            previous_=now;
            if(now-start_<duration_)return Result::idle;
            write(now);apply(previousMode_);active_=false;measuring_=false;
            return Result::completed;
        } catch(const std::exception& failure) {
            if(active_)apply(previousMode_);
            active_=false;measuring_=false;
            try {error(failure.what());}catch(...) {}
            return Result::failed;
        } catch(...) {
            if(active_)apply(previousMode_);
            active_=false;measuring_=false;
            return Result::failed;
        }
    }
};
}
