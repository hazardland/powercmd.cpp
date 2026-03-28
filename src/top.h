// MODULE: top
// Purpose : interactive process viewer — system stats, colors, filter, full navigation
// Exports : top_cmd()
// Depends : common.h, terminal.h

#include <psapi.h>
#include <winternl.h>
#include <cstdint>

struct proc_entry {
    DWORD       pid;
    std::string name;
    std::string path;
    std::string cmdline;
    double      cpu;
    SIZE_T      mem;
};

static std::string get_cmdline(HANDLE h) {
    typedef NTSTATUS (WINAPI *PFN)(HANDLE,PROCESSINFOCLASS,PVOID,ULONG,PULONG);
    static PFN fn = (PFN)GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtQueryInformationProcess");
    if (!fn) return "";
    PROCESS_BASIC_INFORMATION pbi={};
    if (fn(h,ProcessBasicInformation,&pbi,sizeof(pbi),nullptr)!=0) return "";
    PEB peb={};
    SIZE_T rd=0;
    if (!ReadProcessMemory(h,pbi.PebBaseAddress,&peb,sizeof(peb),&rd)) return "";
    RTL_USER_PROCESS_PARAMETERS params={};
    if (!ReadProcessMemory(h,peb.ProcessParameters,&params,sizeof(params),&rd)) return "";
    if (!params.CommandLine.Length||!params.CommandLine.Buffer) return "";
    std::wstring wbuf(params.CommandLine.Length/sizeof(wchar_t),L'\0');
    if (!ReadProcessMemory(h,params.CommandLine.Buffer,&wbuf[0],params.CommandLine.Length,&rd)) return "";
    // convert to narrow ASCII, replace non-ASCII with '?'
    std::string out;
    out.reserve(wbuf.size());
    for (wchar_t wc:wbuf) out+=(wc<128?(char)wc:'?');
    return out;
}

static std::string fmt_mem(SIZE_T bytes) {
    char buf[32];
    if      (bytes >= (SIZE_T)1024*1024*1024) snprintf(buf,sizeof(buf),"%.1f GB",bytes/(1024.0*1024*1024));
    else if (bytes >= 1024*1024)              snprintf(buf,sizeof(buf),"%.1f MB",bytes/(1024.0*1024));
    else                                      snprintf(buf,sizeof(buf),"%.0f KB",bytes/1024.0);
    return buf;
}

// 10-char bar: "[########]"
static std::string make_bar(double pct) {
    int f = (int)(pct/100.0*8+0.5);
    if (f>8) f=8; if (f<0) f=0;
    std::string s="[";
    for (int i=0;i<8;i++) s+=(i<f?'#':' ');
    return s+"]";
}

static const char* cpu_col(double pct) {
    return BLUE;
}

static const char* mem_col(SIZE_T b) {
    if (b>=(SIZE_T)100*1024*1024) return "\x1b[1;35m"; // bold magenta, same as ls images
    return GRAY;
}

static const char* name_col(const std::string& n) {
    if (n=="svchost.exe"||n=="lsass.exe"||n=="csrss.exe"||n=="winlogon.exe"||
        n=="services.exe"||n=="smss.exe"||n=="wininit.exe"||n=="dwm.exe"||
        n=="System"||n=="Registry"||n=="Idle"||n=="spoolsv.exe"||
        n=="fontdrvhost.exe"||n=="audiodg.exe"||n=="taskhostw.exe"||
        n=="SearchIndexer.exe"||n=="RuntimeBroker.exe"||n=="sihost.exe")
        return GRAY;
    return BLUE;
}

static void top_cmd() {
    SYSTEM_INFO si; GetSystemInfo(&si);
    int ncpu = (int)si.dwNumberOfProcessors;

    auto ft2u64 = [](FILETIME ft)->uint64_t {
        return ((uint64_t)ft.dwHighDateTime<<32)|ft.dwLowDateTime;
    };

    char sort_by    = 'm';  // 'm'=mem 'c'=cpu
    int  sel        = 0;
    int  scroll_top = 0;
    bool fmode      = false;
    std::string fstr;
    bool needs_clear      = true;
    bool prev_show_filter = false;
    bool kill_pending     = false;

    std::unordered_map<DWORD,uint64_t> prev_times;
    ULONGLONG prev_wall = GetTickCount64();
    std::vector<proc_entry> procs;

    FILETIME prev_idle={},prev_kern={},prev_user={};
    GetSystemTimes(&prev_idle,&prev_kern,&prev_user);
    double sys_cpu=0.0;
    double ram_used=0.0, ram_total=0.0;

    ULONGLONG last_fetch = 0; // force immediate fetch on first iteration
    DWORD     sticky_pid = 0; // keep selection on same process across refreshes

    out("\x1b[?25l\x1b[2J");

    while (true) {
        // ── Fetch data only when 1 s has elapsed ──
        bool just_fetched = false;
        if (GetTickCount64()-last_fetch >= 1000) {
            just_fetched = true;
            // System CPU
            {
                FILETIME idle,kern,user;
                GetSystemTimes(&idle,&kern,&user);
                uint64_t di=ft2u64(idle)-ft2u64(prev_idle);
                uint64_t dk=ft2u64(kern)-ft2u64(prev_kern);
                uint64_t du=ft2u64(user)-ft2u64(prev_user);
                uint64_t dt=dk+du;
                if (dt>0) sys_cpu=(double)(dt-di)/dt*100.0;
                prev_idle=idle; prev_kern=kern; prev_user=user;
            }

            // RAM
            MEMORYSTATUSEX ms={}; ms.dwLength=sizeof(ms);
            GlobalMemoryStatusEx(&ms);
            ram_used  = (ms.ullTotalPhys-ms.ullAvailPhys)/(1024.0*1024*1024);
            ram_total = ms.ullTotalPhys/(1024.0*1024*1024);

            // Processes
            DWORD pids[4096]; DWORD cb=0;
            EnumProcesses(pids,sizeof(pids),&cb);
            int np=(int)(cb/sizeof(DWORD));

            std::unordered_map<DWORD,uint64_t> curr_times;
            std::vector<proc_entry> new_procs;
            ULONGLONG curr_wall=GetTickCount64();
            uint64_t wall_delta=(curr_wall-prev_wall)*10000ULL;
            if (!wall_delta) wall_delta=1;

            for (int i=0;i<np;i++) {
                DWORD pid=pids[i];
                HANDLE h=OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid);
                if (!h) continue;
                char name[MAX_PATH]={};
                GetModuleBaseNameA(h,nullptr,name,sizeof(name));
                FILETIME ct,et,kt,ut; uint64_t ptime=0;
                if (GetProcessTimes(h,&ct,&et,&kt,&ut)) ptime=ft2u64(kt)+ft2u64(ut);
                curr_times[pid]=ptime;
                double cpu=0.0;
                if (prev_times.count(pid)) {
                    uint64_t d=ptime-prev_times[pid];
                    cpu=(double)d/wall_delta/ncpu*100.0;
                    if (cpu<0)cpu=0; if (cpu>100)cpu=100;
                }
                PROCESS_MEMORY_COUNTERS pmc={}; pmc.cb=sizeof(pmc);
                SIZE_T mem=0;
                if (GetProcessMemoryInfo(h,&pmc,sizeof(pmc))) mem=pmc.WorkingSetSize;
                char fullpath[MAX_PATH]={};
                DWORD pathlen=MAX_PATH;
                if (!QueryFullProcessImageNameA(h,0,fullpath,&pathlen)) fullpath[0]=0;
                std::string cmdline=get_cmdline(h);
                CloseHandle(h);
                if (strlen(name) < 3) continue;
                new_procs.push_back({pid,name,fullpath,cmdline,cpu,mem});
            }
            prev_times=curr_times; prev_wall=curr_wall; procs=new_procs;
            last_fetch=GetTickCount64();
        }

        // stable_sort: ties keep relative order, preventing list churn on CPU sort
        if (sort_by=='m')
            std::stable_sort(procs.begin(),procs.end(),[](const proc_entry&a,const proc_entry&b){return a.mem>b.mem;});
        else
            std::stable_sort(procs.begin(),procs.end(),[](const proc_entry&a,const proc_entry&b){return a.cpu>b.cpu;});

        // ── Filter ──
        std::vector<proc_entry*> filtered;
        for (auto& p:procs) {
            if (fstr.empty()) { filtered.push_back(&p); continue; }
            std::string ln=p.name, lf=fstr;
            for (auto& c:ln) c=(char)tolower((unsigned char)c);
            for (auto& c:lf) c=(char)tolower((unsigned char)c);
            if (ln.find(lf)!=std::string::npos) filtered.push_back(&p);
        }

        // Restore sel to wherever sticky_pid ended up after a refresh/reorder
        if (just_fetched && sticky_pid != 0) {
            for (int i=0;i<(int)filtered.size();i++) {
                if (filtered[i]->pid==sticky_pid) { sel=i; break; }
            }
        }

        if (!filtered.empty()) {
            if (sel>=(int)filtered.size()) sel=(int)filtered.size()-1;
            if (sel<0) sel=0;
            sticky_pid=filtered[sel]->pid;
        } else { sel=0; sticky_pid=0; }

        int cols       = term_width();
        int rows       = term_height();
        bool show_filter = fmode || !fstr.empty();
        if (show_filter != prev_show_filter) { needs_clear=true; prev_show_filter=show_filter; }
        int visible    = rows-(show_filter?5:4); // summary + sep + [filter] + sep + pathbar
        if (visible<1) visible=1;

        if (sel<scroll_top) scroll_top=sel;
        if (sel>=scroll_top+visible) scroll_top=sel-visible+1;
        if (scroll_top<0) scroll_top=0;

        // ── Build frame ──────────────────────────────────────────────────────
        // Column layout (all widths are display/plain chars):
        //   PID   : " %6lu"       =  7  (" " + right-align-6)
        //   CPU%  : "  %5.1f%%"   =  8  ("  " + min-5-float + "%")
        //   MEM   : "  %9s"       = 11  ("  " + right-align-9)
        //   "  "  before name     =  2
        //   FIXED total           = 28
        static const int FIXED = 28;

        std::string F;
        if (needs_clear) { F+="\x1b[2J"; needs_clear=false; }
        F+="\x1b[H";

        // Row 1 — Summary (dark bg)
        {
            char s[256];
            snprintf(s,sizeof(s),"%.1f%%",sys_cpu);
            char ru[32],rt[32];
            snprintf(ru,sizeof(ru),"%.1f",ram_used);
            snprintf(rt,sizeof(rt),"%.1f",ram_total);
            char p[256];
            snprintf(p,sizeof(p),"%d",(int)procs.size());
            std::string line="  "+std::string(GRAY)+"CPU: "+BLUE+s+GRAY+"  RAM: "+BLUE+ru+GRAY+" / "+BLUE+rt+GRAY+" GB  "+GRAY+"PROCS: "+BLUE+p;
            // pad
            int plain_len=2+5+(int)strlen(s)+7+(int)strlen(ru)+3+(int)strlen(rt)+5+7+(int)strlen(p);
            while (plain_len<cols) { line+=' '; plain_len++; }
            F+=line+"\x1b[0m\r\n";
        }

        // Row 2 (conditional) — Filter bar
        if (show_filter) {
            std::string line="  "+std::string(GRAY)+"[";
            if (fmode) {
                line+=YELLOW+fstr+"_\x1b[39m";
            } else {
                line+=YELLOW+fstr+"\x1b[39m";
            }
            line+=std::string(GRAY)+"]\x1b[39m"+RESET;
            F+=line+"\x1b[K\x1b[0m\r\n";
        }

        // Row 3 — Separator (box-drawing ─)
        {
            F+=GRAY;
            for (int i=0;i<cols;i++) F+="\xe2\x94\x80"; // U+2500 ─
            F+="\x1b[0m\r\n";
        }

        // Rows — Process list
        for (int i=scroll_top;i<scroll_top+visible;i++) {
            if (i>=(int)filtered.size()) {
                F+="\x1b[2K\r\n";
                continue;
            }
            auto& p=*filtered[i];
            std::string mems=fmt_mem(p.mem);

            int name_w=cols-1-FIXED;
            if (name_w<0) name_w=0;
            std::string sname=(p.name.size()>(size_t)name_w)?p.name.substr(0,name_w):p.name;

            if (i==sel) {
                // Plain reverse-video highlight
                char plain[512];
                snprintf(plain,sizeof(plain)," %6lu  %5.1f%%  %9s  %-*s",
                    (unsigned long)p.pid,p.cpu,mems.c_str(),name_w,sname.c_str());
                std::string row(plain);
                if ((int)row.size()>cols-1) row=row.substr(0,cols-1);
                while ((int)row.size()<cols-1) row+=' ';
                F+="\x1b[48;5;75m\x1b[30m"+row+RESET+"\r\n";
            } else {
                // Colored row — build piece by piece, track plain length for padding
                char pid_s[12]; snprintf(pid_s,sizeof(pid_s)," %6lu",(unsigned long)p.pid);
                char cpu_s[12]; snprintf(cpu_s,sizeof(cpu_s),"  %5.1f%%",p.cpu);
                char mem_s[16]; snprintf(mem_s,sizeof(mem_s),"  %9s",mems.c_str());

                std::string row;
                row+=GRAY+std::string(pid_s)+RESET;
                row+=cpu_col(p.cpu)+std::string(cpu_s)+RESET;
                row+=mem_col(p.mem)+std::string(mem_s)+RESET;
                row+="  "+std::string(name_col(p.name))+sname+RESET;

                int plain_len=FIXED+(int)sname.size();
                while (plain_len<cols-1) { row+=' '; plain_len++; }
                F+=row+"\r\n";
            }
        }
        F+="\x1b[J"; // clear any leftover rows

        // Row n-1 / n — Path bar or kill confirm
        {
            F+=GRAY;
            for (int i=0;i<cols;i++) F+="\xe2\x94\x80";
            F+="\x1b[0m\r\n";
            if (kill_pending && !filtered.empty()) {
                const proc_entry& sel_p=*filtered[sel];
                char conf[512];
                snprintf(conf,sizeof(conf),
                    "\x1b[48;5;170m\x1b[30m  Kill %s (PID %lu)? [Y/N]  " RESET,
                    sel_p.name.c_str(),(unsigned long)sel_p.pid);
                F+=std::string(conf);
            } else {
                std::string pathline;
                if (!filtered.empty()) {
                    const proc_entry& sel_p=*filtered[sel];
                    std::string p=!sel_p.cmdline.empty()?sel_p.cmdline:(!sel_p.path.empty()?sel_p.path:sel_p.name);
                    for (auto& c:p) if (c=='\\') c='/';
                    if ((int)p.size()>cols-2) p=p.substr(0,cols-2);
                    pathline=" "+std::string(GRAY)+p+RESET;
                }
                F+=pathline;
            }
            F+="\x1b[K\x1b[0m";
        }

        out(F);

        // ── Input loop — wait until next fetch, redraw immediately on any key ─
        {
            bool redraw=false;
            ULONGLONG deadline=last_fetch+1000;

            while (!redraw) {
                ULONGLONG now=GetTickCount64();
                if (now>=deadline) break;
                // Block until input arrives or refresh deadline
                if (WaitForSingleObject(in_h,(DWORD)(deadline-now))!=WAIT_OBJECT_0) break;

                DWORD n=0;
                GetNumberOfConsoleInputEvents(in_h,&n);
                while (n-->0) {
                    INPUT_RECORD ir; DWORD rd;
                    ReadConsoleInputW(in_h,&ir,1,&rd);

                    if (ir.EventType==WINDOW_BUFFER_SIZE_EVENT) {
                        needs_clear=true; redraw=true; continue;
                    }
                    if (ir.EventType!=KEY_EVENT||!ir.Event.KeyEvent.bKeyDown) continue;

                    WORD    vk  = ir.Event.KeyEvent.wVirtualKeyCode;
                    wchar_t wch = ir.Event.KeyEvent.uChar.UnicodeChar;
                    wchar_t ch  = towlower(wch);

                    // Filter input mode
                    if (fmode) {
                        if (vk==VK_ESCAPE)         { fmode=false; fstr.clear(); sel=0; scroll_top=0; }
                        else if (vk==VK_BACK)       { if (!fstr.empty()) fstr.pop_back(); sel=0; scroll_top=0; }
                        else if (vk==VK_RETURN)     { fmode=false; }
                        else if (wch>=32&&wch<127)  { fstr+=(char)wch; sel=0; scroll_top=0; }
                        redraw=true; continue;
                    }

                    // Kill confirm mode
                    if (kill_pending) {
                        wchar_t kch=towlower(wch);
                        if (kch=='y'&&!filtered.empty()) {
                            HANDLE kh=OpenProcess(PROCESS_TERMINATE,FALSE,filtered[sel]->pid);
                            if (kh){TerminateProcess(kh,1);CloseHandle(kh);}
                        }
                        kill_pending=false; redraw=true; continue;
                    }

                    // Normal mode
                    if (ch=='q'||vk==VK_ESCAPE) goto done;
                    if (ch=='m') { sort_by='m'; redraw=true; }
                    if (ch=='c') { sort_by='c'; redraw=true; }
                    if (ch=='/'||ch=='f') { fmode=true; fstr.clear(); sel=0; scroll_top=0; redraw=true; }

                    if (vk==VK_UP)    { if (sel>0) sel--; redraw=true; }
                    if (vk==VK_DOWN)  { if (sel<(int)filtered.size()-1) sel++; redraw=true; }
                    if (vk==VK_HOME)  { sel=0; scroll_top=0; redraw=true; }
                    if (vk==VK_END)   { sel=(int)filtered.size()-1; if (sel<0)sel=0; redraw=true; }
                    if (vk==VK_PRIOR) { sel-=visible; if (sel<0)sel=0; redraw=true; }
                    if (vk==VK_NEXT)  {
                        sel+=visible;
                        int last=(int)filtered.size()-1;
                        if (sel>last) sel=last;
                        if (sel<0)   sel=0;
                        redraw=true;
                    }

                    if (ch=='k'&&!filtered.empty()) { kill_pending=true; redraw=true; }
                }
            }
        }
    }
done:
    out("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
}
