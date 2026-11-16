// SPDX-License-Identifier: GPL-3.0-only
// Bounded, synthetic host replay. This does not consume device evidence.
#ifndef MGM_EXAMPLES_TRACE_REPLAY_HPP
#define MGM_EXAMPLES_TRACE_REPLAY_HPP
#include <meguviz/media_scheduler.h>
#include <meguviz/timeline.h>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace mgm_trace {
constexpr size_t max_input = 8u * 1024u * 1024u;
constexpr size_t max_output = 16u * 1024u * 1024u;
constexpr size_t max_line = 8192, max_events = 32768;

// Deliberately small JSON subset: flat objects, ASCII identifiers, int64
// values, and no duplicate keys. No string escapes, floats, booleans or null.
struct Value { bool text=false; std::string s; int64_t n=0; };
using Object = std::map<std::string, Value>;
struct Parser {
    const std::string &s; size_t i=0;
    void ws(){while(i<s.size()&&(s[i]==' '||s[i]=='\t'||s[i]=='\r'))++i;}
    bool take(char c){ws();if(i<s.size()&&s[i]==c){++i;return true;}return false;}
    std::string str(){
        if(!take('"'))throw std::runtime_error("expected identifier");
        const size_t begin=i;
        while(i<s.size()&&s[i]!='"'){
            const unsigned char c=static_cast<unsigned char>(s[i]);
            if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'))throw std::runtime_error("invalid identifier");
            if(i-begin>=64)throw std::runtime_error("identifier too long");++i;
        }
        if(i==begin||i==s.size())throw std::runtime_error("invalid identifier");
        auto v=s.substr(begin,i-begin);++i;return v;
    }
    int64_t integer(){
        ws();const size_t begin=i;if(i<s.size()&&s[i]=='-')++i;
        if(i==s.size()||s[i]<'0'||s[i]>'9')throw std::runtime_error("expected integer");
        if(s[i]=='0'&&i+1<s.size()&&s[i+1]>='0'&&s[i+1]<='9')throw std::runtime_error("leading zero");
        while(i<s.size()&&s[i]>='0'&&s[i]<='9')++i;
        int64_t n=0;auto result=std::from_chars(s.data()+begin,s.data()+i,n);
        if(result.ec!=std::errc{}||result.ptr!=s.data()+i)throw std::runtime_error("integer out of range");return n;
    }
    Object parse(){
        Object o;if(!take('{'))throw std::runtime_error("expected object");
        if(take('}'))throw std::runtime_error("empty object");
        do {auto key=str();if(!take(':'))throw std::runtime_error("expected colon");ws();Value v;
            if(i<s.size()&&s[i]=='"'){v.text=true;v.s=str();}else v.n=integer();
            if(!o.emplace(key,std::move(v)).second)throw std::runtime_error("duplicate key");
        } while(take(','));
        if(!take('}'))throw std::runtime_error("expected object end");ws();if(i!=s.size())throw std::runtime_error("trailing data");return o;
    }
};
inline void fields(const Object &o,std::initializer_list<const char*> names){
    if(o.size()!=names.size()+2)throw std::runtime_error("unexpected or missing fields");
    for(const char *name:names)if(!o.count(name))throw std::runtime_error("missing field");
    if(!o.count("seq")||!o.count("op"))throw std::runtime_error("missing envelope");
}
inline int64_t number(const Object &o,const char *key){auto it=o.find(key);if(it==o.end()||it->second.text)throw std::runtime_error("expected numeric field");return it->second.n;}
inline uint64_t nonnegative(const Object &o,const char *key){auto n=number(o,key);if(n<0)throw std::runtime_error("negative field");return static_cast<uint64_t>(n);}
inline uint32_t u32(const Object &o,const char *key){auto n=nonnegative(o,key);if(n>UINT32_MAX)throw std::runtime_error("field out of range");return static_cast<uint32_t>(n);}
inline std::string op_name(const Object &o){auto it=o.find("op");if(it==o.end()||!it->second.text)throw std::runtime_error("expected op");return it->second.s;}
inline void success(MgcStatus s){if(s!=MGC_SUCCESS)throw std::runtime_error("planner operation failed: "+std::to_string(s));}
inline void discarded(std::ostream &out,const MgmOfferResult &o){out<<",\"accepted\":"<<o.accepted<<",\"discarded\":[";for(uint32_t i=0;i<o.discarded_count;++i){if(i)out<<',';out<<o.discarded_ids[i];}out<<']';}

// Exclusively create a new file; never replace an existing output.
inline void write_exclusive(const std::filesystem::path &path,const std::string &data){
    int fd=-1;
#ifdef _WIN32
    if(_wsopen_s(&fd,path.c_str(),_O_WRONLY|_O_CREAT|_O_EXCL|_O_BINARY|_O_NOINHERIT,_SH_DENYRW,_S_IREAD|_S_IWRITE)!=0)throw std::runtime_error("cannot create output exclusively");
#else
    fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)throw std::runtime_error("cannot create output exclusively");
#endif
    bool ok=true;size_t offset=0;
    while(offset<data.size()){
        const auto amount=std::min<size_t>(data.size()-offset,1024u*1024u);
#ifdef _WIN32
        int n=_write(fd,data.data()+offset,static_cast<unsigned>(amount));
#else
        auto n=::write(fd,data.data()+offset,amount);
#endif
        if(n<=0){ok=false;break;}offset+=static_cast<size_t>(n);
    }
#ifdef _WIN32
    if(_close(fd)!=0)ok=false;
#else
    if(::close(fd)!=0)ok=false;
#endif
    if(!ok)throw std::runtime_error("output write failed; partial file retained");
}

struct Job { bool cancelled=false; };
inline int run(const char *input,const char *output){
    try {
        const std::filesystem::path path(input);
        if(std::filesystem::is_symlink(path)||!std::filesystem::is_regular_file(path)||std::filesystem::file_size(path)>max_input)throw std::runtime_error("invalid or oversized input file");
        std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("cannot open input");
        std::unique_ptr<MgmPlanner,decltype(&mgm_planner_release)> planner(nullptr,mgm_planner_release);
        std::unique_ptr<MgmAdmission,decltype(&mgm_admission_release)> admission(nullptr,mgm_admission_release);
        std::unordered_map<uint64_t,Job> jobs;std::unordered_set<uint64_t> accepted_ids;std::ostringstream out;out<<std::setprecision(17);
        std::string line;size_t lines=0,bytes=0;uint64_t generation=0;MgmProposal proposal{};bool has_proposal=false,epoch_active=false;
        while(std::getline(in,line)){
            bytes+=line.size()+1;if(bytes>max_input+1||line.empty()||line.size()>max_line||++lines>max_events)throw std::runtime_error("input limits exceeded");
            Object o=Parser{line}.parse();const auto seq=nonnegative(o,"seq");const auto op=op_name(o);
            if(seq!=lines)throw std::runtime_error("sequence must start at one and be contiguous");
            if((lines==1)!=(op=="config"))throw std::runtime_error("config must be first and unique");
            out<<"{\"seq\":"<<seq<<",\"op\":\""<<op<<"\"";
            if(op=="config"){
                fields(o,{"target_fps","frame_window","max_clock_age_ns","presentation_delay_ns","max_lead_ns","min_lead_ns","original_tolerance_us","max_clock_uncertainty_us","phase_lock","cold_cost_ns","safety_margin_ns","sample_window","percentile","max_in_flight","enabled"});
                MgmPlannerConfig c{};c.header=MGC_HEADER_INIT(MgmPlannerConfig);c.target_fps=u32(o,"target_fps");c.frame_window=u32(o,"frame_window");c.max_clock_age_ns=number(o,"max_clock_age_ns");c.presentation_delay_ns=number(o,"presentation_delay_ns");c.max_lead_ns=number(o,"max_lead_ns");c.min_lead_ns=number(o,"min_lead_ns");c.original_tolerance_us=number(o,"original_tolerance_us");c.max_clock_uncertainty_us=number(o,"max_clock_uncertainty_us");c.phase_lock=u32(o,"phase_lock");MgmPlanner *p=nullptr;success(mgm_planner_create(&c,&p));planner.reset(p);
                MgmAdmissionConfig a{};a.header=MGC_HEADER_INIT(MgmAdmissionConfig);a.cold_cost_ns=nonnegative(o,"cold_cost_ns");a.safety_margin_ns=nonnegative(o,"safety_margin_ns");a.sample_window=u32(o,"sample_window");a.percentile=u32(o,"percentile");a.max_in_flight=u32(o,"max_in_flight");a.enabled=u32(o,"enabled");MgmAdmission *q=nullptr;success(mgm_admission_create(&a,&q));admission.reset(q);
                out<<",\"status\":0,\"scope\":\"SYNTHETIC_PLANNER_REPLAY\"";
            }else if(op=="begin"){
                fields(o,{"media","decoder","surface","format","discontinuity"});MgmEpoch e{nonnegative(o,"media"),nonnegative(o,"decoder"),nonnegative(o,"surface"),nonnegative(o,"format"),nonnegative(o,"discontinuity")};MgmOfferResult v{};v.header=MGC_HEADER_INIT(MgmOfferResult);success(mgm_planner_begin(planner.get(),&e,&generation,&v));has_proposal=false;epoch_active=true;out<<",\"status\":0,\"generation\":"<<generation;discarded(out,v);
            }else if(op=="clock"){
                fields(o,{"media_us","monotonic_ns","speed_milli","discontinuity","uncertainty_us","paused"});if(!generation)throw std::runtime_error("epoch not started");MgmClockSnapshot c{};c.header=MGC_HEADER_INIT(MgmClockSnapshot);c.media_us=number(o,"media_us");c.monotonic_ns=number(o,"monotonic_ns");c.speed=number(o,"speed_milli")/1000.0;c.discontinuity=nonnegative(o,"discontinuity");c.uncertainty_us=number(o,"uncertainty_us");c.paused=u32(o,"paused");if(c.paused>1)throw std::runtime_error("invalid paused flag");MgmOfferResult v{};v.header=MGC_HEADER_INIT(MgmOfferResult);uint32_t state=0;success(mgm_planner_clock(planner.get(),generation,&c,&state,&v));if(state==MGM_CLOCK_RESTART_REQUIRED){has_proposal=false;epoch_active=false;}out<<",\"status\":0,\"clock_state\":"<<state;discarded(out,v);
            }else if(op=="offer"){
                fields(o,{"frame_id","pts_us"});if(!generation)throw std::runtime_error("epoch not started");MgmOfferResult v{};v.header=MGC_HEADER_INIT(MgmOfferResult);success(mgm_planner_offer(planner.get(),generation,nonnegative(o,"frame_id"),number(o,"pts_us"),&v));out<<",\"status\":0";discarded(out,v);
            }else if(op=="plan"){
                fields(o,{"now_ns"});if(!generation)throw std::runtime_error("epoch not started");MgmProposal p{};p.header=MGC_HEADER_INIT(MgmProposal);success(mgm_planner_plan(planner.get(),generation,number(o,"now_ns"),&p));if(!p.waiting_reason){proposal=p;has_proposal=true;}out<<",\"status\":0,\"waiting_reason\":"<<p.waiting_reason<<",\"due_ns\":"<<p.due_ns<<",\"media_us\":"<<p.media_us<<",\"before_id\":"<<p.before_id<<",\"after_id\":"<<p.after_id<<",\"alpha\":"<<p.alpha<<",\"mode\":"<<p.mode;
            }else if(op=="commit"){
                fields(o,{"now_ns"});if(!has_proposal)throw std::runtime_error("no proposal to commit");const auto s=mgm_planner_commit(planner.get(),&proposal,number(o,"now_ns"));has_proposal=false;out<<",\"status\":"<<s;
            }else if(op=="reserve"){
                fields(o,{"job_id","now_ns","deadline_ns"});if(!epoch_active)throw std::runtime_error("reserve requires active epoch");const auto id=nonnegative(o,"job_id");if(!id||accepted_ids.count(id))throw std::runtime_error("invalid or reused accepted job");MgmAdmissionDecision d{};d.header=MGC_HEADER_INIT(MgmAdmissionDecision);auto s=mgm_admission_reserve(admission.get(),number(o,"now_ns"),number(o,"deadline_ns"),&d);if(s!=MGC_ACCEPTED&&s!=MGC_BACKPRESSURE&&s!=MGC_DEADLINE_REJECTED)throw std::runtime_error("admission failed");if(s==MGC_ACCEPTED){jobs.emplace(id,Job{});accepted_ids.insert(id);}out<<",\"status\":"<<s<<",\"predicted_joint_ns\":"<<d.predicted_joint_ns<<",\"in_flight\":"<<d.in_flight<<",\"sample_count\":"<<d.sample_count;
            }else if(op=="cancel"){
                fields(o,{"job_id"});auto it=jobs.find(nonnegative(o,"job_id"));if(it==jobs.end()||it->second.cancelled)throw std::runtime_error("unknown or already cancelled job");it->second.cancelled=true;out<<",\"status\":0,\"effect\":\"publication_suppressed_retirement_pending\"";
            }else if(op=="retire"){
                fields(o,{"job_id","joint_ns","succeeded"});auto it=jobs.find(nonnegative(o,"job_id"));if(it==jobs.end())throw std::runtime_error("unknown job");const auto succeeded=u32(o,"succeeded");if(succeeded>1||(it->second.cancelled&&succeeded))throw std::runtime_error("invalid successful sample");success(mgm_admission_retire(admission.get(),nonnegative(o,"joint_ns"),succeeded));jobs.erase(it);out<<",\"status\":0";
            }else if(op=="counters"){
                fields(o,{});MgmPlannerCounters c{};c.header=MGC_HEADER_INIT(MgmPlannerCounters);success(mgm_planner_counters(planner.get(),&c));out<<",\"status\":0,\"original\":"<<c.original<<",\"interpolate\":"<<c.interpolate<<",\"snapped\":"<<c.snapped<<",\"no_future\":"<<c.no_future<<",\"no_previous\":"<<c.no_previous<<",\"skipped_lead\":"<<c.skipped_lead<<",\"skipped_behind\":"<<c.skipped_behind;
            }else if(op=="close"){
                fields(o,{});if(!generation)throw std::runtime_error("epoch not started");MgmOfferResult v{};v.header=MGC_HEADER_INIT(MgmOfferResult);success(mgm_planner_close_epoch(planner.get(),generation,&v));generation=0;has_proposal=false;epoch_active=false;out<<",\"status\":0,\"outstanding_jobs\":"<<jobs.size();discarded(out,v);
            }else throw std::runtime_error("unknown operation");
            out<<"}\n";if(out.tellp()<0||static_cast<uint64_t>(out.tellp())>max_output)throw std::runtime_error("output limit exceeded");
        }
        if(in.bad()||!lines)throw std::runtime_error("input read failed or empty");
        if(generation||!jobs.empty())throw std::runtime_error("trace ended with active epoch or unretired jobs");
        write_exclusive(std::filesystem::path(output),out.str());return 0;
    }catch(const std::exception &e){std::cerr<<"trace replay: "<<e.what()<<"\n";return 2;}
}
}
#endif
