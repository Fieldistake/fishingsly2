// FIELD's FISHING - compact Win32 build. Windows 10/11, MSVC or MinGW-w64.
#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commctrl.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cmath>
#include <algorithm>
#ifdef _MSC_VER
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"winmm.lib")
#endif

// ---------- config ----------
struct Cfg { POINT cast{-1,-1}; RECT bar{0,0,0,0}, ex{0,0,0,0}; int waitSec=20; UINT vkStart=VK_F6, vkExit=VK_F8; } C;
static wchar_t gIni[MAX_PATH];
static bool hasCast(){ return C.cast.x>=0; }
static bool hasRect(const RECT&r){ return r.right-r.left>2 && r.bottom-r.top>2; }
static bool calibrated(){ return hasCast()&&hasRect(C.bar)&&hasRect(C.ex); }
static int  ri(const wchar_t*k,int d){ return (int)GetPrivateProfileIntW(L"Fish",k,d,gIni); }
static void wi(const wchar_t*k,int v){ wchar_t b[32]; _snwprintf(b,31,L"%d",v); WritePrivateProfileStringW(L"Fish",k,b,gIni); }
static void saveAll(){
  wi(L"CastX",C.cast.x); wi(L"CastY",C.cast.y);
  wi(L"BarL",C.bar.left); wi(L"BarT",C.bar.top); wi(L"BarR",C.bar.right); wi(L"BarB",C.bar.bottom);
  wi(L"ExL",C.ex.left); wi(L"ExT",C.ex.top); wi(L"ExR",C.ex.right); wi(L"ExB",C.ex.bottom);
  wi(L"Wait",C.waitSec); wi(L"VkStart",C.vkStart); wi(L"VkExit",C.vkExit);
}
static void loadAll(){
  GetModuleFileNameW(0,gIni,MAX_PATH); wchar_t*d=wcsrchr(gIni,L'\\'); if(d) wcscpy(d+1,L"Fishingtrack.ini");
  C.cast={ri(L"CastX",-1),ri(L"CastY",-1)};
  C.bar={ri(L"BarL",0),ri(L"BarT",0),ri(L"BarR",0),ri(L"BarB",0)};
  C.ex={ri(L"ExL",0),ri(L"ExT",0),ri(L"ExR",0),ri(L"ExB",0)};
  C.waitSec=std::min(60,std::max(0,ri(L"Wait",20))); C.vkStart=ri(L"VkStart",VK_F6); C.vkExit=ri(L"VkExit",VK_F8);
}

// ---------- shared state ----------
static std::atomic<bool> gRun{false}, gQuit{false};
static std::atomic<int> gScans{0};
static std::mutex gM; static std::wstring gPhase=L"Idle", gStats=L"";
static HWND hMain;
static void setPhase(const wchar_t*f,...){ wchar_t b[128]; va_list a; va_start(a,f); _vsnwprintf(b,127,f,a); va_end(a); b[127]=0; std::lock_guard<std::mutex> l(gM); gPhase=b; }
static void setStats(const wchar_t*f,...){ wchar_t b[128]; va_list a; va_start(a,f); _vsnwprintf(b,127,f,a); va_end(a); b[127]=0; std::lock_guard<std::mutex> l(gM); gStats=b; }

// ---------- input ----------
static bool gDown=false, gKey=false;
static void mouseSet(bool d){ if(d==gDown) return; gDown=d; INPUT i{}; i.type=INPUT_MOUSE; i.mi.dwFlags=d?MOUSEEVENTF_LEFTDOWN:MOUSEEVENTF_LEFTUP; SendInput(1,&i,sizeof i); }
static void keyT(bool d){ if(d==gKey) return; gKey=d; INPUT i{}; i.type=INPUT_KEYBOARD; i.ki.wScan=(WORD)MapVirtualKeyW('T',MAPVK_VK_TO_VSC); i.ki.dwFlags=KEYEVENTF_SCANCODE|(d?0:KEYEVENTF_KEYUP); SendInput(1,&i,sizeof i); }
static double now(){ static LARGE_INTEGER f; LARGE_INTEGER t; if(!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return t.QuadPart*1000.0/f.QuadPart; }
static bool nap(int ms){ double e=now()+ms; while(now()<e){ if(!gRun||gQuit) return false; Sleep(5);} return gRun&&!gQuit; }
static HWND gameWnd(){ HWND w=WindowFromPoint(C.cast); if(!w) return 0; w=GetAncestor(w,GA_ROOT); return w==hMain?0:w; }
static void forceFg(HWND g){
  if(!g||g==GetForegroundWindow()) return;
  DWORD ft=GetWindowThreadProcessId(GetForegroundWindow(),0), me=GetCurrentThreadId();
  AttachThreadInput(me,ft,TRUE); ShowWindow(g,SW_SHOWNA); SetForegroundWindow(g); AttachThreadInput(me,ft,FALSE); Sleep(60);
}
static void moveTo(POINT p){ POINT s; GetCursorPos(&s); for(int i=1;i<=20;i++){ SetCursorPos(s.x+(p.x-s.x)*i/20, s.y+(p.y-s.y)*i/20); Sleep(8);} SetCursorPos(p.x,p.y); }

// ---------- screen capture + color detection ----------
struct Cap {
  HDC mem=0; HBITMAP bmp=0; uint32_t*px=0; int w=0,h=0;
  bool grab(const RECT&r){
    int W=r.right-r.left,H=r.bottom-r.top; if(W<=0||H<=0) return false;
    HDC s=GetDC(0);
    if(!mem) mem=CreateCompatibleDC(s);
    if(W!=w||H!=h){
      BITMAPINFO bi{}; bi.bmiHeader={sizeof(BITMAPINFOHEADER),W,-H,1,32,BI_RGB};
      void*bits=0; HBITMAP nb=CreateDIBSection(s,&bi,DIB_RGB_COLORS,&bits,0,0); if(!nb){ReleaseDC(0,s);return false;}
      HBITMAP old=(HBITMAP)SelectObject(mem,nb); if(bmp) DeleteObject(old?old:bmp);
      bmp=nb; px=(uint32_t*)bits; w=W; h=H;
    }
    BOOL ok=BitBlt(mem,0,0,W,H,s,r.left,r.top,SRCCOPY|CAPTUREBLT); ReleaseDC(0,s); return ok!=0;
  }
};
static bool isGreen(int r,int g,int b){ return abs(r-60)<50&&abs(g-200)<55&&abs(b-80)<55; }
static bool isWarn(int r,int g,int b){ // gold border / olive fill, by hue
  int mx=std::max({r,g,b}),mn=std::min({r,g,b}); if(mx<40) return false;
  float d=(float)(mx-mn),s=d/mx; if(s<0.35f) return false; float h;
  if(mx==r) h=60.f*std::fmod((g-b)/d+6.f,6.f); else if(mx==g) h=60.f*((b-r)/d+2.f); else return false;
  return h>=45.f&&h<=85.f;
}
static int countRed(Cap&c){ int n=0; for(int i=0;i<c.w*c.h;i++){ uint32_t p=c.px[i]; int r=(p>>16)&255,g=(p>>8)&255,b=p&255; if(abs(r-190)<60&&g<75&&b<75) n++; } return n; }
struct Scan{ bool hasT=false,hasP=false; int tmin=0,tmax=0,pmin=0,pmax=0; };
static Scan scanBar(Cap&c){
  Scan s;
  for(int y=0;y<c.h;y++){
    const uint32_t*row=c.px+(size_t)y*c.w; int g=0,wh=0;
    for(int x=0;x<c.w;x++){ uint32_t p=row[x]; int r=(p>>16)&255,gg=(p>>8)&255,b=p&255;
      if(std::min({r,gg,b})>=225) wh++; else if(isGreen(r,gg,b)||isWarn(r,gg,b)) g++; }
    if(g>=3){ if(!s.hasT){s.hasT=true;s.tmin=y;} s.tmax=y; }
    if(wh>=3){ if(!s.hasP){s.hasP=true;s.pmin=y;} s.pmax=y; }
  }
  return s;
}

// ---------- fishing cycle ----------
static Cap barC, exC; static double lastCast=0;
static bool exitSeen(){ return exC.grab(C.ex) && countRed(exC)>=15; }

static void hookLoop(){
  int gone=0,scans=0; double last=now(),sec=now(),pyP=-1,tyP=-1,vp=0,vt=0,ty=0,th=12,py=0,acc=0,slot=0,holdSince=0; bool pulse=false;
  while(gRun&&!gQuit){
    if(exitSeen()) gone=0; else if(++gone>=8) break;
    if(!barC.grab(C.bar)){ Sleep(5); continue; }
    Scan s=scanBar(barC); double t=now(), dt=std::max(1.0,t-last)/1000.0; last=t;
    if(s.hasT){ ty=(s.tmin+s.tmax)/2.0; th=std::max(4.0,(s.tmax-s.tmin)/2.0); if(tyP>=0) vt=0.6*vt+0.4*(ty-tyP)/dt; tyP=ty; }
    if(s.hasP){ py=(s.pmin+s.pmax)/2.0; if(pyP>=0) vp=0.6*vp+0.4*(py-pyP)/dt; pyP=py; }
    double H=barC.h; bool want=false;
    if(s.hasP){
      double db=std::max(3.0,th*0.3), pe=(py+vp*0.05)-(ty+vt*0.05); // +: player below target -> rise
      if(py>H*0.93) want=true; else if(py<H*0.07) want=false;
      else if(pe>db*2.5) want=true; else if(pe<-db*2.5) want=false;
      else { if(t>=slot){ slot=t+18; acc+=std::min(0.85,std::max(0.15,0.5+pe/(db*6))); if(acc>=1){acc-=1;pulse=true;} else pulse=false; } want=pulse; }
    }
    if(want&&!gDown) holdSince=t;
    if(want&&gDown&&t-holdSince>700){ mouseSet(false); Sleep(30); want=false; }  // safety release
    mouseSet(want);
    scans++; if(t-sec>=1000){ gScans=scans; setStats(L"%d scans/s  target %.0f  player %.0f",scans,ty,py); scans=0; sec=t; }
    Sleep(2);
  }
  mouseSet(false);
}

static void cycle(){
  while(lastCast>0 && now()-lastCast<10000){ setPhase(L"Post-cast failsafe: %ds left before next click",(int)((10000-(now()-lastCast))/1000)+1); if(!nap(100)) return; }
  setPhase(L"CASTING");
  HWND g=gameWnd(), prev=GetForegroundWindow(); forceFg(g);
  moveTo(C.cast); if(!nap(450)) return;
  mouseSet(true); Sleep(70); mouseSet(false); if(!nap(1200)) return;
  double w0=now(); bool seen=false;
  while(now()-w0<C.waitSec*1000.0){ setPhase(L"WAIT FOR FISH  %ds",(int)(C.waitSec-(now()-w0)/1000)); if(exitSeen()){seen=true;break;} if(!nap(20)) return; }
  if(!seen){ setPhase(L"Cast not registered - retrying"); return; }
  lastCast=now();
  setPhase(L"HOOKING"); forceFg(g); hookLoop(); if(!gRun) return;
  for(int i=4;i>0;i--){ setPhase(L"COLLECTING  Waiting to collect %ds",i); if(!nap(1000)) return; }
  forceFg(g); keyT(true);
  for(int i=4;i>0;i--){ setPhase(L"HOLDING T  %ds",i); if(!nap(1000)){ keyT(false); return; } }
  keyT(false);
  if(prev&&IsWindow(prev)&&prev!=g) SetForegroundWindow(prev);
}
static void worker(){
  timeBeginPeriod(1);
  while(!gQuit){
    if(!gRun||!calibrated()){ mouseSet(false); keyT(false); setPhase(!calibrated()?L"NOT CALIBRATED - use Calibration tab":L"Paused"); if(!calibrated()) gRun=false; Sleep(40); continue; }
    cycle(); mouseSet(false); keyT(false);
  }
  mouseSet(false); keyT(false); timeEndPeriod(1);
}

// ---------- UI ----------
enum{ ID_START=101,ID_EXIT,ID_WAIT,ID_RB1=111,ID_RB2,ID_CAL=121 };
static HWND hTab,hStatus,hPhase,hStats,hCastL,hWait,hStartB,hKeyL,hRb[2],hCalB[3],hCalL[3];
static std::vector<HWND> pages[3];
static int rebind=0;
static HWND mk(const wchar_t*cls,const wchar_t*txt,DWORD st,int x,int y,int w,int h,int id,int pg){
  HWND c=CreateWindowW(cls,txt,WS_CHILD|(pg==0?WS_VISIBLE:0)|st,x,y,w,h,hMain,(HMENU)(INT_PTR)id,0,0); pages[pg].push_back(c); return c; }
static void showPage(int p){ for(int i=0;i<3;i++) for(HWND c:pages[i]) ShowWindow(c,i==p?SW_SHOW:SW_HIDE); }
static std::wstring keyName(UINT vk){ wchar_t b[64]=L"?"; LONG sc=MapVirtualKeyW(vk,MAPVK_VK_TO_VSC)<<16; if(vk>=VK_PRIOR&&vk<=VK_DOWN) sc|=1<<24; GetKeyNameTextW(sc,b,63); return b; }
static void refreshText(){
  wchar_t b[200];
  _snwprintf(b,199,L"%s",calibrated()?(gRun?L"RUNNING":L"CALIBRATED - ready"):L"NOT CALIBRATED"); SetWindowTextW(hStatus,b);
  if(hasCast()) _snwprintf(b,199,L"Cast point: (%ld, %ld)",C.cast.x,C.cast.y); else wcscpy(b,L"Cast point: NOT SET"); SetWindowTextW(hCastL,b);
  _snwprintf(b,199,L"Start/Pause: %s     Exit: %s",keyName(C.vkStart).c_str(),keyName(C.vkExit).c_str()); SetWindowTextW(hKeyL,b);
  _snwprintf(b,199,L"%s (%s)",gRun?L"Pause":L"Start",keyName(C.vkStart).c_str()); SetWindowTextW(hStartB,b);
  if(hasCast()) _snwprintf(b,199,L"Cast: (%ld,%ld)",C.cast.x,C.cast.y); else wcscpy(b,L"Cast: NOT SET"); SetWindowTextW(hCalL[0],b);
  const RECT*rs[2]={&C.bar,&C.ex}; const wchar_t*nm[2]={L"Bar",L"Exit"};
  for(int i=0;i<2;i++){ if(hasRect(*rs[i])) _snwprintf(b,199,L"%s: (%ld,%ld) %ldx%ld",nm[i],rs[i]->left,rs[i]->top,rs[i]->right-rs[i]->left,rs[i]->bottom-rs[i]->top); else _snwprintf(b,199,L"%s: NOT SET",nm[i]); SetWindowTextW(hCalL[i+1],b); }
}
static void regHotkeys(){ UnregisterHotKey(hMain,1); UnregisterHotKey(hMain,2); RegisterHotKey(hMain,1,MOD_NOREPEAT,C.vkStart); RegisterHotKey(hMain,2,MOD_NOREPEAT,C.vkExit); }
static void toggle(){ if(!gRun&&!calibrated()){ refreshText(); return; } gRun=!gRun; if(!gRun){ mouseSet(false); keyT(false);} refreshText(); InvalidateRect(hMain,0,FALSE); }

// calibration overlay
static int calMode=-1; static bool drag=false; static POINT pa,pb; static int vx,vy;
static void endCal(HWND h){ DestroyWindow(h); drag=false; calMode=-1; }
static LRESULT CALLBACK OvProc(HWND h,UINT m,WPARAM w,LPARAM l){
  switch(m){
  case WM_SETCURSOR: SetCursor(LoadCursor(0,IDC_CROSS)); return TRUE;
  case WM_KEYDOWN: if(w==VK_ESCAPE) endCal(h); return 0;
  case WM_LBUTTONDOWN: pa={(short)LOWORD(l),(short)HIWORD(l)}; pb=pa; drag=true; SetCapture(h);
    if(calMode==0){ ReleaseCapture(); C.cast={pa.x+vx,pa.y+vy}; saveAll(); endCal(h); PostMessage(hMain,WM_APP+1,0,0); } return 0;
  case WM_MOUSEMOVE: if(drag){ pb={(short)LOWORD(l),(short)HIWORD(l)}; InvalidateRect(h,0,TRUE);} return 0;
  case WM_LBUTTONUP: if(drag&&calMode>0){ ReleaseCapture();
      RECT r{std::min(pa.x,pb.x)+vx,std::min(pa.y,pb.y)+vy,std::max(pa.x,pb.x)+vx,std::max(pa.y,pb.y)+vy};
      if(hasRect(r)){ (calMode==1?C.bar:C.ex)=r; saveAll(); } endCal(h); PostMessage(hMain,WM_APP+1,0,0); } return 0;
  case WM_PAINT: { PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
      if(drag&&calMode>0){ HPEN p=CreatePen(PS_SOLID,2,RGB(255,60,60)); HGDIOBJ o1=SelectObject(dc,p),o2=SelectObject(dc,GetStockObject(NULL_BRUSH)); Rectangle(dc,pa.x,pa.y,pb.x,pb.y); SelectObject(dc,o1); SelectObject(dc,o2); DeleteObject(p);}
      EndPaint(h,&ps); return 0; }
  }
  return DefWindowProc(h,m,w,l);
}
static void startCal(int mode){
  calMode=mode; vx=GetSystemMetrics(SM_XVIRTUALSCREEN); vy=GetSystemMetrics(SM_YVIRTUALSCREEN);
  HWND o=CreateWindowExW(WS_EX_TOPMOST|WS_EX_LAYERED|WS_EX_TOOLWINDOW,L"FFOv",L"",WS_POPUP|WS_VISIBLE,vx,vy,GetSystemMetrics(SM_CXVIRTUALSCREEN),GetSystemMetrics(SM_CYVIRTUALSCREEN),0,0,0,0);
  SetLayeredWindowAttributes(o,0,90,LWA_ALPHA); SetForegroundWindow(o); SetFocus(o);
}

static std::wstring lastP,lastS,lastSt;
static LRESULT CALLBACK MainProc(HWND h,UINT m,WPARAM w,LPARAM l){
  switch(m){
  case WM_CREATE: SetTimer(h,1,90,0); return 0;
  case WM_NCHITTEST: { LRESULT r=DefWindowProc(h,m,w,l); if(r==HTCLIENT){ POINT p={(short)LOWORD(l),(short)HIWORD(l)}; ScreenToClient(h,&p); if(p.y<30) return HTCAPTION; } return r; }
  case WM_NOTIFY: if(((NMHDR*)l)->hwndFrom==hTab&&((NMHDR*)l)->code==TCN_SELCHANGE) showPage(TabCtrl_GetCurSel(hTab)); return 0;
  case WM_COMMAND: {
    int id=LOWORD(w);
    if(id==ID_START) toggle();
    else if(id==ID_EXIT) DestroyWindow(h);
    else if(id==ID_WAIT&&HIWORD(w)==EN_CHANGE){ wchar_t b[16]; GetWindowTextW(hWait,b,15); C.waitSec=std::min(60,std::max(0,_wtoi(b))); saveAll(); }
    else if(id==ID_RB1||id==ID_RB2){ rebind=id-ID_RB1+1; SetWindowTextW(hRb[rebind-1],L"Press a key... (Esc cancels)"); SetTimer(h,2,30,0); }
    else if(id>=ID_CAL&&id<ID_CAL+3) startCal(id-ID_CAL);
    return 0; }
  case WM_APP+1: refreshText(); return 0;
  case WM_HOTKEY: if(w==1) toggle(); else DestroyWindow(h); return 0;
  case WM_TIMER:
    if(w==1){ std::wstring p,s; { std::lock_guard<std::mutex> l2(gM); p=gPhase; s=gStats; }
      if(p!=lastP){ SetWindowTextW(hPhase,p.c_str()); lastP=p; } if(s!=lastS){ SetWindowTextW(hStats,s.c_str()); lastS=s; }
      std::wstring st=gRun?L"1":L"0"; if(st!=lastSt){ lastSt=st; InvalidateRect(h,0,FALSE); refreshText(); } }
    else if(w==2){
      for(UINT vk=8;vk<255;vk++){ if(vk<7||vk==VK_SHIFT||vk==VK_CONTROL||vk==VK_MENU||vk==VK_LSHIFT||vk==VK_RSHIFT||vk==VK_LCONTROL||vk==VK_RCONTROL||vk==VK_LMENU||vk==VK_RMENU) continue;
        if(GetAsyncKeyState(vk)&0x8000){ KillTimer(h,2);
          if(vk!=VK_ESCAPE){ (rebind==1?C.vkStart:C.vkExit)=vk; saveAll(); regHotkeys(); }
          SetWindowTextW(hRb[0],L"Rebind Start/Pause"); SetWindowTextW(hRb[1],L"Rebind Exit"); rebind=0; refreshText(); break; } } }
    return 0;
  case WM_PAINT: { PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); RECT rc; GetClientRect(h,&rc);
      HBRUSH bg=CreateSolidBrush(RGB(30,34,40)); RECT hd={0,0,rc.right,30}; FillRect(dc,&hd,bg); DeleteObject(bg);
      SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(235,235,235)); TextOutW(dc,10,7,L"FIELD's FISHING",15);
      HBRUSH f=CreateSolidBrush(gRun?RGB(60,200,80):RGB(200,50,50)); HGDIOBJ o=SelectObject(dc,f); Ellipse(dc,rc.right-28,6,rc.right-10,24); SelectObject(dc,o); DeleteObject(f);
      EndPaint(h,&ps); return 0; }
  case WM_DESTROY: gRun=false; gQuit=true; mouseSet(false); keyT(false); PostQuitMessage(0); return 0;
  }
  return DefWindowProc(h,m,w,l);
}

int WINAPI WinMain(HINSTANCE hi,HINSTANCE,LPSTR,int){
  SetProcessDPIAware(); loadAll();
  INITCOMMONCONTROLSEX ic{sizeof ic,ICC_TAB_CLASSES}; InitCommonControlsEx(&ic);
  WNDCLASSW wc{}; wc.lpfnWndProc=MainProc; wc.hInstance=hi; wc.hCursor=LoadCursor(0,IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1); wc.lpszClassName=L"FFMain"; RegisterClassW(&wc);
  WNDCLASSW ow{}; ow.lpfnWndProc=OvProc; ow.hInstance=hi; ow.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH); ow.lpszClassName=L"FFOv"; RegisterClassW(&ow);
  hMain=CreateWindowExW(WS_EX_TOPMOST|WS_EX_LAYERED,L"FFMain",L"FIELD's FISHING",WS_POPUP|WS_BORDER|WS_VISIBLE,80,80,350,270,0,0,hi,0);
  SetLayeredWindowAttributes(hMain,0,235,LWA_ALPHA);
  hTab=CreateWindowW(WC_TABCONTROL,L"",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,5,32,338,228,hMain,0,hi,0);
  const wchar_t*tn[3]={L"Fishing",L"Keystrokes",L"Calibration"}; for(int i=0;i<3;i++){ TCITEMW t{}; t.mask=TCIF_TEXT; t.pszText=(LPWSTR)tn[i]; TabCtrl_InsertItem(hTab,i,&t); }
  hStatus=mk(L"STATIC",L"",0,20,68,300,18,0,0); hPhase=mk(L"STATIC",L"Idle",0,20,92,310,18,0,0); hStats=mk(L"STATIC",L"",0,20,116,310,18,0,0);
  hCastL=mk(L"STATIC",L"",0,20,140,300,18,0,0);
  mk(L"STATIC",L"Wait for fish (0-60 s):",0,20,166,150,18,0,0);
  hWait=mk(L"EDIT",L"",WS_BORDER|ES_NUMBER,175,163,50,22,ID_WAIT,0); { wchar_t b[8]; _snwprintf(b,7,L"%d",C.waitSec); SetWindowTextW(hWait,b); }
  hStartB=mk(L"BUTTON",L"Start",BS_PUSHBUTTON,20,200,150,30,ID_START,0); mk(L"BUTTON",L"Exit",BS_PUSHBUTTON,180,200,140,30,ID_EXIT,0);
  hKeyL=mk(L"STATIC",L"",0,20,70,310,20,0,1);
  hRb[0]=mk(L"BUTTON",L"Rebind Start/Pause",BS_PUSHBUTTON,20,100,230,28,ID_RB1,1); hRb[1]=mk(L"BUTTON",L"Rebind Exit",BS_PUSHBUTTON,20,136,230,28,ID_RB2,1);
  mk(L"STATIC",L"Held key during Holding phase is fixed: T",0,20,176,310,20,0,1);
  const wchar_t*cn[3]={L"Calibrate Cast Point",L"Calibrate Fishing Bar",L"Calibrate Exit Button"};
  for(int i=0;i<3;i++){ hCalB[i]=mk(L"BUTTON",cn[i],BS_PUSHBUTTON,20,66+i*54,160,28,ID_CAL+i,2); hCalL[i]=mk(L"STATIC",L"",0,20,96+i*54,300,18,0,2); }
  HFONT f=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
  for(int i=0;i<3;i++) for(HWND c:pages[i]) SendMessage(c,WM_SETFONT,(WPARAM)f,0);
  SendMessage(hTab,WM_SETFONT,(WPARAM)f,0);
  refreshText(); regHotkeys();
  std::thread t(worker);
  MSG msg; while(GetMessage(&msg,0,0,0)){ TranslateMessage(&msg); DispatchMessage(&msg); }
  gQuit=true; t.join(); return 0;
}
