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
static std::atomic<float> gTy{-1.f}, gTh{0.f}, gPy{-1.f};  // normalized 0..1 for the HUD gauge
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
    gTy=(float)(ty/H); gTh=(float)(th/H); gPy=s.hasP?(float)(py/H):-1.f;
    scans++; if(t-sec>=1000){ gScans=scans; setStats(L"%d scans/s  target %.0f  player %.0f",scans,ty,py); scans=0; sec=t; }
    Sleep(2);
  }
  mouseSet(false); gTy=-1.f; gPy=-1.f;
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

// ---------- UI (dark theme, left sidebar tabs, live bar gauge) ----------
enum{ ID_START=101,ID_EXIT,ID_WAIT,ID_RB1=111,ID_RB2,ID_CAL=121 };
static const int WW=520,HH=340;
static const COLORREF cBG=RGB(16,20,28),cPanel=RGB(25,31,43),cSide=RGB(11,14,20),cAcc=RGB(0,214,170),cTxt=RGB(228,234,242),
  cDim=RGB(128,142,162),cGood=RGB(70,220,120),cBad=RGB(240,84,84),cWarn=RGB(255,184,64),cField=RGB(36,43,58),cEdge=RGB(58,70,92);
static HBRUSH bPanel,bField; static HFONT fUI,fBig,fTitle,fSmall;
static HWND hStatus,hPhase,hStats,hCastL,hWait,hStartB,hKeyL,hRb[2],hCalL[3];
static std::vector<HWND> pages[3],dimL;
static int curPage=0,hover=-1,rebind=0;
static const wchar_t*pageName[3]={L"Fishing",L"Keystrokes",L"Calibration"};
static RECT itemRc(int i){ return RECT{10,64+i*46,130,104+i*46}; }
static RECT gaugeRc(){ return RECT{WW-70,72,WW-36,262}; }
static bool isSet(int i){ return i==0?hasCast():hasRect(i==1?C.bar:C.ex); }
static HWND mk(const wchar_t*cls,const wchar_t*txt,DWORD st,int x,int y,int w,int h,int id,int pg){
  HWND c=CreateWindowW(cls,txt,WS_CHILD|(pg==0?WS_VISIBLE:0)|st,x,y,w,h,hMain,(HMENU)(INT_PTR)id,0,0);
  SendMessage(c,WM_SETFONT,(WPARAM)fUI,0); pages[pg].push_back(c); return c; }
static void showPage(int p){ curPage=p; for(int i=0;i<3;i++) for(HWND c:pages[i]) ShowWindow(c,i==p?SW_SHOW:SW_HIDE); InvalidateRect(hMain,0,FALSE); }
static std::wstring keyName(UINT vk){ wchar_t b[64]=L"?"; LONG sc=MapVirtualKeyW(vk,MAPVK_VK_TO_VSC)<<16; if(vk>=VK_PRIOR&&vk<=VK_DOWN) sc|=1<<24; GetKeyNameTextW(sc,b,63); return b; }
static void refreshText(){
  wchar_t b[200];
  SetWindowTextW(hStatus,calibrated()?(gRun?L"RUNNING":L"READY"):L"NOT CALIBRATED");
  if(hasCast()) _snwprintf(b,199,L"Cast point: (%ld, %ld)",C.cast.x,C.cast.y); else wcscpy(b,L"Cast point: NOT SET"); SetWindowTextW(hCastL,b);
  _snwprintf(b,199,L"Start/Pause:  %s        Exit:  %s",keyName(C.vkStart).c_str(),keyName(C.vkExit).c_str()); SetWindowTextW(hKeyL,b);
  _snwprintf(b,199,L"%s  (%s)",gRun?L"PAUSE":L"START",keyName(C.vkStart).c_str()); SetWindowTextW(hStartB,b);
  if(hasCast()) _snwprintf(b,199,L"(%ld, %ld)",C.cast.x,C.cast.y); else wcscpy(b,L"NOT SET"); SetWindowTextW(hCalL[0],b);
  const RECT*rs[2]={&C.bar,&C.ex};
  for(int i=0;i<2;i++){ if(hasRect(*rs[i])) _snwprintf(b,199,L"(%ld,%ld)  %ldx%ld",rs[i]->left,rs[i]->top,rs[i]->right-rs[i]->left,rs[i]->bottom-rs[i]->top); else wcscpy(b,L"NOT SET"); SetWindowTextW(hCalL[i+1],b); }
  InvalidateRect(hMain,0,FALSE);
}
static void regHotkeys(){ UnregisterHotKey(hMain,1); UnregisterHotKey(hMain,2); RegisterHotKey(hMain,1,MOD_NOREPEAT,C.vkStart); RegisterHotKey(hMain,2,MOD_NOREPEAT,C.vkExit); }
static void toggle(){ if(!gRun&&!calibrated()){ refreshText(); return; } gRun=!gRun; if(!gRun){ mouseSet(false); keyT(false);} refreshText(); }

// ----- drawing helpers -----
static void rr(HDC dc,RECT r,int rad,COLORREF f,COLORREF e){
  bool ne=(e==(COLORREF)-1); HBRUSH b=CreateSolidBrush(f); HPEN p=ne?(HPEN)GetStockObject(NULL_PEN):CreatePen(PS_SOLID,1,e);
  HGDIOBJ o1=SelectObject(dc,b),o2=SelectObject(dc,p); RoundRect(dc,r.left,r.top,r.right,r.bottom,rad,rad);
  SelectObject(dc,o1); SelectObject(dc,o2); DeleteObject(b); if(!ne) DeleteObject(p); }
static void fillR(HDC dc,RECT r,COLORREF c){ HBRUSH b=CreateSolidBrush(c); FillRect(dc,&r,b); DeleteObject(b); }
static void tx(HDC dc,HFONT f,COLORREF c,const wchar_t*s,RECT r,UINT fmt){ SelectObject(dc,f); SetTextColor(dc,c); DrawTextW(dc,s,-1,&r,fmt|DT_SINGLELINE|DT_NOPREFIX); }
static COLORREF lighten(COLORREF c,int a){ return RGB(std::min(255,GetRValue(c)+a),std::min(255,GetGValue(c)+a),std::min(255,GetBValue(c)+a)); }

static void paintAll(HDC dc,int W,int H){
  fillR(dc,RECT{0,0,W,H},cBG); fillR(dc,RECT{0,0,140,H},cSide); SetBkMode(dc,TRANSPARENT);
  // brand
  tx(dc,fTitle,cAcc,L"FIELD'S",RECT{16,8,140,28},0); tx(dc,fTitle,cTxt,L"FISHING",RECT{16,26,140,46},0);
  // sidebar tabs
  for(int i=0;i<3;i++){ RECT r=itemRc(i); bool sel=i==curPage;
    if(sel||i==hover) rr(dc,r,10,sel?RGB(24,34,46):RGB(17,22,31),-1);
    if(sel) fillR(dc,RECT{0,r.top+8,4,r.bottom-8},cAcc);
    RECT t=r; t.left+=18; tx(dc,fUI,sel?cAcc:(i==hover?cTxt:cDim),pageName[i],t,DT_VCENTER); }
  wchar_t b[96]; _snwprintf(b,95,L"%s  start / pause",keyName(C.vkStart).c_str()); tx(dc,fSmall,cDim,b,RECT{16,H-52,140,H-36},0);
  _snwprintf(b,95,L"%s  exit",keyName(C.vkExit).c_str()); tx(dc,fSmall,cDim,b,RECT{16,H-34,140,H-18},0);
  // header
  tx(dc,fBig,cTxt,pageName[curPage],RECT{160,14,340,38},DT_VCENTER);
  tx(dc,fSmall,gRun?cGood:cDim,gRun?L"LIVE":L"IDLE",RECT{W-130,14,W-46,40},DT_RIGHT|DT_VCENTER);
  int cx=W-28,cy=27; if(gRun){ int ph=(int)(GetTickCount()/90%8); double k=1.0-ph/8.0; HPEN p=CreatePen(PS_SOLID,2,RGB((int)(70*k),(int)(220*k),(int)(120*k)));
    HGDIOBJ o1=SelectObject(dc,p),o2=SelectObject(dc,GetStockObject(NULL_BRUSH)); int r=8+ph; Ellipse(dc,cx-r,cy-r,cx+r,cy+r); SelectObject(dc,o1); SelectObject(dc,o2); DeleteObject(p); }
  { HBRUSH f=CreateSolidBrush(gRun?cGood:cBad); HGDIOBJ o=SelectObject(dc,f),o2=SelectObject(dc,GetStockObject(NULL_PEN)); Ellipse(dc,cx-6,cy-6,cx+6,cy+6); SelectObject(dc,o); SelectObject(dc,o2); DeleteObject(f); }
  // content card
  rr(dc,RECT{150,54,W-10,H-10},16,cPanel,cEdge);
  // live gauge
  if(curPage==0){ RECT g=gaugeRc(); int gh=g.bottom-g.top; rr(dc,RECT{g.left-4,g.top-4,g.right+4,g.bottom+4},8,cField,cEdge);
    float ty=gTy,th=gTh,py=gPy;
    if(ty>=0){ int y0=g.top+(int)std::max(0.f,(ty-th)*gh),y1=g.top+(int)std::min((float)gh,(ty+th)*gh); fillR(dc,RECT{g.left,y0,g.right,std::max(y0+2,y1)},RGB(60,200,80)); }
    if(py>=0){ int y=g.top+(int)(py*gh); fillR(dc,RECT{g.left+3,y-5,g.right-3,y+5},RGB(250,250,250)); }
    tx(dc,fSmall,cDim,L"LIVE BAR",RECT{g.left-14,g.bottom+8,g.right+14,g.bottom+24},DT_CENTER); }
  // window outline
  { HPEN p=CreatePen(PS_SOLID,1,cEdge); HGDIOBJ o1=SelectObject(dc,p),o2=SelectObject(dc,GetStockObject(NULL_BRUSH)); RoundRect(dc,0,0,W,H,16,16); SelectObject(dc,o1); SelectObject(dc,o2); DeleteObject(p); }
}

// ----- calibration overlay -----
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

static std::wstring lastP,lastS; static bool lastRun=false;
static int hitItem(int x,int y){ for(int i=0;i<3;i++){ RECT r=itemRc(i); if(x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom) return i; } return -1; }
static LRESULT CALLBACK MainProc(HWND h,UINT m,WPARAM w,LPARAM l){
  switch(m){
  case WM_CREATE: SetTimer(h,1,90,0); return 0;
  case WM_ERASEBKGND: return 1;
  case WM_NCHITTEST: { LRESULT r=DefWindowProc(h,m,w,l); if(r==HTCLIENT){ POINT p={(short)LOWORD(l),(short)HIWORD(l)}; ScreenToClient(h,&p); if(p.y<50&&p.x>140) return HTCAPTION; } return r; }
  case WM_MOUSEMOVE: { int i=hitItem((short)LOWORD(l),(short)HIWORD(l)); if(i!=hover){ hover=i; RECT s{0,50,140,HH}; InvalidateRect(h,&s,FALSE); }
      TRACKMOUSEEVENT te{sizeof te,TME_LEAVE,h,0}; TrackMouseEvent(&te); return 0; }
  case WM_MOUSELEAVE: hover=-1; { RECT s{0,50,140,HH}; InvalidateRect(h,&s,FALSE); } return 0;
  case WM_LBUTTONDOWN: { int i=hitItem((short)LOWORD(l),(short)HIWORD(l)); if(i>=0&&i!=curPage) showPage(i); return 0; }
  case WM_CTLCOLORSTATIC: { HDC dc=(HDC)w; HWND c=(HWND)l; COLORREF col=cTxt;
      if(c==hPhase) col=cAcc; else if(c==hStatus) col=(gRun||calibrated())?cGood:cBad;
      else if(std::find(dimL.begin(),dimL.end(),c)!=dimL.end()) col=cDim;
      for(int i=0;i<3;i++) if(c==hCalL[i]) col=isSet(i)?cGood:cBad;
      SetBkColor(dc,cPanel); SetTextColor(dc,col); return (LRESULT)bPanel; }
  case WM_CTLCOLOREDIT: { HDC dc=(HDC)w; SetBkColor(dc,cField); SetTextColor(dc,cTxt); return (LRESULT)bField; }
  case WM_DRAWITEM: { DRAWITEMSTRUCT*d=(DRAWITEMSTRUCT*)l; if(d->CtlType!=ODT_BUTTON) break;
      int id=d->CtlID; bool pr=(d->itemState&ODS_SELECTED)!=0; COLORREF fill=cField,txt=cTxt,edge=cEdge;
      if(id==ID_START){ fill=gRun?cWarn:cAcc; txt=RGB(10,14,20); edge=(COLORREF)-1; } else if(id==ID_EXIT){ txt=cBad; }
      if(pr) fill=lighten(fill,28);
      fillR(d->hDC,d->rcItem,cPanel); rr(d->hDC,d->rcItem,12,fill,edge);
      wchar_t b[96]; GetWindowTextW(d->hwndItem,b,95); SetBkMode(d->hDC,TRANSPARENT); tx(d->hDC,fUI,txt,b,d->rcItem,DT_CENTER|DT_VCENTER); return TRUE; }
  case WM_COMMAND: {
    int id=LOWORD(w);
    if(id==ID_START) toggle();
    else if(id==ID_EXIT) DestroyWindow(h);
    else if(id==ID_WAIT&&HIWORD(w)==EN_CHANGE){ wchar_t b[16]; GetWindowTextW(hWait,b,15); C.waitSec=std::min(60,std::max(0,_wtoi(b))); saveAll(); }
    else if(id==ID_RB1||id==ID_RB2){ rebind=id-ID_RB1+1; SetWindowTextW(hRb[rebind-1],L"Press a key...  (Esc cancels)"); SetTimer(h,2,30,0); }
    else if(id>=ID_CAL&&id<ID_CAL+3) startCal(id-ID_CAL);
    return 0; }
  case WM_APP+1: refreshText(); return 0;
  case WM_HOTKEY: if(w==1) toggle(); else DestroyWindow(h); return 0;
  case WM_TIMER:
    if(w==1){ std::wstring p,s; { std::lock_guard<std::mutex> l2(gM); p=gPhase; s=gStats; }
      if(p!=lastP){ SetWindowTextW(hPhase,p.c_str()); lastP=p; } if(s!=lastS){ SetWindowTextW(hStats,s.c_str()); lastS=s; }
      if(gRun!=lastRun){ lastRun=gRun; refreshText(); }
      RECT d{WW-140,0,WW,50}; InvalidateRect(h,&d,FALSE);
      if(curPage==0){ RECT g=gaugeRc(); g.left-=16; g.right+=16; g.top-=6; g.bottom+=28; InvalidateRect(h,&g,FALSE); } }
    else if(w==2){
      for(UINT vk=8;vk<255;vk++){ if(vk==VK_SHIFT||vk==VK_CONTROL||vk==VK_MENU||vk==VK_LSHIFT||vk==VK_RSHIFT||vk==VK_LCONTROL||vk==VK_RCONTROL||vk==VK_LMENU||vk==VK_RMENU) continue;
        if(GetAsyncKeyState(vk)&0x8000){ KillTimer(h,2);
          if(vk!=VK_ESCAPE){ (rebind==1?C.vkStart:C.vkExit)=vk; saveAll(); regHotkeys(); }
          SetWindowTextW(hRb[0],L"Rebind Start / Pause"); SetWindowTextW(hRb[1],L"Rebind Exit"); rebind=0; refreshText(); break; } } }
    return 0;
  case WM_PAINT: { PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps); RECT rc; GetClientRect(h,&rc);
      HDC mdc=CreateCompatibleDC(dc); HBITMAP bm=CreateCompatibleBitmap(dc,rc.right,rc.bottom); HGDIOBJ old=SelectObject(mdc,bm);
      paintAll(mdc,rc.right,rc.bottom);
      BitBlt(dc,ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right-ps.rcPaint.left,ps.rcPaint.bottom-ps.rcPaint.top,mdc,ps.rcPaint.left,ps.rcPaint.top,SRCCOPY);
      SelectObject(mdc,old); DeleteObject(bm); DeleteDC(mdc); EndPaint(h,&ps); return 0; }
  case WM_DESTROY: gRun=false; gQuit=true; mouseSet(false); keyT(false); PostQuitMessage(0); return 0;
  }
  return DefWindowProc(h,m,w,l);
}

int WINAPI WinMain(HINSTANCE hi,HINSTANCE,LPSTR,int){
  SetProcessDPIAware(); loadAll();
  INITCOMMONCONTROLSEX ic{sizeof ic,ICC_STANDARD_CLASSES}; InitCommonControlsEx(&ic);
  bPanel=CreateSolidBrush(cPanel); bField=CreateSolidBrush(cField);
  fUI=CreateFontW(-14,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
  fBig=CreateFontW(-18,0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
  fTitle=CreateFontW(-18,0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
  fSmall=CreateFontW(-12,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
  WNDCLASSW wc{}; wc.lpfnWndProc=MainProc; wc.hInstance=hi; wc.hCursor=LoadCursor(0,IDC_ARROW); wc.hbrBackground=0; wc.lpszClassName=L"FFMain"; RegisterClassW(&wc);
  WNDCLASSW ow{}; ow.lpfnWndProc=OvProc; ow.hInstance=hi; ow.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH); ow.lpszClassName=L"FFOv"; RegisterClassW(&ow);
  hMain=CreateWindowExW(WS_EX_TOPMOST|WS_EX_LAYERED,L"FFMain",L"FIELD's FISHING",WS_POPUP|WS_CLIPCHILDREN|WS_VISIBLE,80,80,WW,HH,0,0,hi,0);
  SetWindowRgn(hMain,CreateRoundRectRgn(0,0,WW+1,HH+1,16,16),TRUE);
  SetLayeredWindowAttributes(hMain,0,238,LWA_ALPHA);
  // Fishing page
  hStatus=mk(L"STATIC",L"",0,168,66,260,20,0,0);
  hPhase =mk(L"STATIC",L"Idle",0,168,90,268,48,0,0); SendMessage(hPhase,WM_SETFONT,(WPARAM)fBig,0);
  hStats =mk(L"STATIC",L"",0,168,146,268,18,0,0); hCastL=mk(L"STATIC",L"",0,168,170,268,18,0,0);
  dimL.push_back(hStats); dimL.push_back(hCastL);
  mk(L"STATIC",L"Wait for fish (0-60 s)",0,168,204,150,20,0,0);
  hWait=mk(L"EDIT",L"",ES_NUMBER|ES_CENTER,322,200,56,26,ID_WAIT,0); { wchar_t b[8]; _snwprintf(b,7,L"%d",C.waitSec); SetWindowTextW(hWait,b); }
  hStartB=mk(L"BUTTON",L"START",BS_OWNERDRAW,168,262,150,36,ID_START,0); mk(L"BUTTON",L"EXIT",BS_OWNERDRAW,328,262,100,36,ID_EXIT,0);
  // Keystrokes page
  hKeyL=mk(L"STATIC",L"",0,168,74,330,22,0,1);
  hRb[0]=mk(L"BUTTON",L"Rebind Start / Pause",BS_OWNERDRAW,168,112,250,38,ID_RB1,1); hRb[1]=mk(L"BUTTON",L"Rebind Exit",BS_OWNERDRAW,168,160,250,38,ID_RB2,1);
  dimL.push_back(mk(L"STATIC",L"The key held during the Holding phase is fixed: T",0,168,218,320,36,0,1));
  // Calibration page
  const wchar_t*cn[3]={L"Calibrate Cast Point",L"Calibrate Fishing Bar",L"Calibrate Exit Button"};
  for(int i=0;i<3;i++){ mk(L"BUTTON",cn[i],BS_OWNERDRAW,168,74+i*62,170,38,ID_CAL+i,2); hCalL[i]=mk(L"STATIC",L"",0,352,84+i*62,140,20,0,2); }
  dimL.push_back(mk(L"STATIC",L"Click the button, then click the spot / drag a box in-game. Esc cancels.",0,168,266,320,36,0,2));
  for(int i=1;i<3;i++) for(HWND c:pages[i]) ShowWindow(c,SW_HIDE);
  refreshText(); regHotkeys();
  std::thread t(worker);
  MSG msg; while(GetMessage(&msg,0,0,0)){ TranslateMessage(&msg); DispatchMessage(&msg); }
  gQuit=true; t.join(); return 0;
}
