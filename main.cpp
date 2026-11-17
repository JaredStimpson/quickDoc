#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <d2d1.h>
#include <wrl/client.h>
#include <limits>
#include <pdfium/public/fpdfview.h>
#include <pdfium/public/fpdf_doc.h>
#include <pdfium/public/fpdf_text.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib,"d2d1.lib")
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"comdlg32.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"uxtheme.lib")
#pragma comment(lib,"dwmapi.lib")
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace {
constexpr UINT WM_READY = WM_APP+1;
constexpr UINT WM_PIXELS = WM_APP+2;
constexpr UINT WM_SEARCH_RESULT = WM_APP+3;
constexpr int GAP = 18, MARGIN = 16, SIDEBAR = 250;
int toolbarHeight = 76; // Responsive toolbar: may use one or more rows.
constexpr int THUMB_ROW = 178, THUMB_MAX_W = 150, THUMB_MAX_H = 143;
constexpr int ID_OPEN=101,ID_SIDEBAR=102,ID_PREV=103,ID_NEXT=104,ID_PAGE=105,ID_GO=106,ID_MINUS=107,ID_PLUS=108,ID_FIT=109,ID_TREE=110,ID_CHAPTER_PREV=111,ID_CHAPTER_NEXT=112,ID_PAGE_TOTAL=113,ID_BOOK_TOGGLE=114,ID_BOOK_PDF=115,ID_BOOK_INPUT=116,ID_BOOK_SET=117,ID_BOOK_OFFSET=118,ID_FIT_PAGE=119,ID_ROTATE_LEFT=120,ID_ROTATE_RIGHT=121,ID_TWO_PAGE=122,ID_FULLSCREEN=123,ID_SCROLL_UP=124,ID_SCROLL_DOWN=125,ID_SEARCH_FOCUS=126,ID_SEARCH_PREV=127,ID_SEARCH_NEXT=128,ID_SEARCH_CLOSE=129,ID_SEARCH_FIELD=130,ID_SEARCH_STATUS=131,ID_THEME=132;
struct Bookmark { std::wstring name; int page=-1; int parent=-1; };
struct Pixels { int width=0,height=0; std::vector<unsigned char> data; };
struct Entry { int page=-1, width=0; double zoom=0; std::shared_ptr<Pixels> pixels; ComPtr<ID2D1Bitmap> bitmap; uint64_t age=0; };
struct Request { int page; int width; unsigned generation; int rotation; };
HWND win=nullptr,toolbar=nullptr,tree=nullptr,pageBox=nullptr,thumbnailPane=nullptr,bookBox=nullptr,splitter=nullptr,searchBox=nullptr;
HWND ToolbarControl(int id) { return toolbar ? GetDlgItem(toolbar,id) : nullptr; }

WNDPROC originalPageEditProc=nullptr;
ComPtr<ID2D1Factory> factory;
ComPtr<ID2D1HwndRenderTarget> target;
std::thread worker;
std::mutex mx;
std::condition_variable cv;
bool stop=false,loaded=false,loading=false,sidebar=true,chaptersMode=false,fitWidth=false,fitPage=true;
bool bookMode=false, bookOffsetSet=false, twoPage=false, fullScreen=false, draggingSplitter=false, darkMode=true;
int sidebarWidth=250;
std::vector<int> rotations; // 0, 1, 2, 3 quarter-turns; never written into the PDF
WINDOWPLACEMENT previousPlacement{sizeof(WINDOWPLACEMENT)};
DWORD previousStyle=0;
COLORREF ThemeBG() {return darkMode?RGB(30,32,37):RGB(245,246,248);}
COLORREF ThemePanel() {return darkMode?RGB(38,41,47):RGB(232,234,238);}
COLORREF ThemeText() {return darkMode?RGB(230,233,240):RGB(29,32,38);}
D2D1::ColorF ThemeD2D() {return darkMode?D2D1::ColorF(0x26292f):D2D1::ColorF(0xe8eaee);}
void RefreshTheme();
HBRUSH darkPanelBrush=nullptr, darkEditBrush=nullptr;
int bookOffset=0; // printed book page = PDF page (1-based) + bookOffset
std::wstring filename,errorText;
std::wstring pendingFile;
unsigned generation=0;
int pageCount=0;
std::vector<std::pair<double,double>> dimensions;
std::vector<Bookmark> bookmarks;
std::vector<Entry> cache;
std::deque<Request> jobs;
std::deque<int> thumbnailJobs;
struct ThumbEntry { int page=-1; std::shared_ptr<Pixels> pixels; uint64_t age=0; };
std::vector<ThumbEntry> thumbnailCache;
int thumbnailScroll=0;
bool thumbnailFollow=true;
uint64_t clockTick=0;
double zoom=1.0,scrollY=0,contentHeight=0;
std::vector<double> tops,heights,lefts,widths;
struct SearchHit { int page=0; int charIndex=0; };
std::vector<SearchHit> searchHits;
std::wstring searchQuery;
unsigned searchRevision=0;
int searchScanPage=0, searchSelected=-1;
bool searching=false, searchVisible=false, searchStatusDirty=false;


int DocWidth() { RECT r{}; GetClientRect(win,&r); return std::max(1,static_cast<int>(r.right-r.left)-(sidebar?sidebarWidth:0)); }
int DocHeight() { RECT r{}; GetClientRect(win,&r); return std::max(1,static_cast<int>(r.bottom)-toolbarHeight); }
// Only invalidate the document viewport. Invalidation of the whole parent
// window made native toolbar buttons and the bookmark tree repaint on every
// wheel tick, producing the visible dim/blink reported in v2.2.
void RequestPaint() {
  if(!win) return;
  RECT r{}; GetClientRect(win,&r);
  r.left=sidebar?sidebarWidth:0;
  r.top=toolbarHeight;
  if(r.right>r.left && r.bottom>r.top) InvalidateRect(win,&r,FALSE);
}
// Document positions are virtual: in two-page mode each pair shares a row.
// Page rotation changes the displayed dimensions, but never modifies the file.
std::pair<double,double> OrientedSize(size_t i) {
  auto d=dimensions[i];
  if(i<rotations.size() && (rotations[i]&1)) std::swap(d.first,d.second);
  return d;
}
void RebuildLayout() {
  std::lock_guard<std::mutex> lock(mx);
  tops.resize(dimensions.size()); heights.resize(dimensions.size());
  lefts.resize(dimensions.size()); widths.resize(dimensions.size());
  const int viewW=DocWidth();
  double useZoom=zoom;
  if((fitWidth || fitPage) && !dimensions.empty()) {
    double maxWidth=1, maxHeight=1;
    for(size_t i=0;i<dimensions.size();i+=(twoPage?2:1)) {
      auto a=OrientedSize(i);
      double rowW=a.first, rowH=a.second;
      if(twoPage && i+1<dimensions.size()) {
        auto b=OrientedSize(i+1);rowW+=b.first+GAP;rowH=std::max(rowH,b.second);
      }
      maxWidth=std::max(maxWidth,rowW);maxHeight=std::max(maxHeight,rowH);
    }
    const double widthScale=std::max(1,viewW-2*MARGIN-16)/maxWidth;
    const double heightScale=std::max(1,DocHeight()-2*GAP-8)/maxHeight;
    useZoom=std::clamp(fitPage?std::min(widthScale,heightScale):widthScale,0.15,5.0);
  }
  zoom=useZoom;
  double y=GAP;
  for(size_t i=0;i<dimensions.size();i+=(twoPage?2:1)) {
    auto a=OrientedSize(i); double aw=a.first*zoom, ah=a.second*zoom;
    double bw=0,bh=0;
    if(twoPage && i+1<dimensions.size()) {
      auto b=OrientedSize(i+1);bw=b.first*zoom;bh=b.second*zoom;
    }
    const double rowWidth=aw+(bw?GAP+bw:0);
    const double left=std::max(0.0,(viewW-rowWidth)*0.5);
    tops[i]=y;heights[i]=ah;lefts[i]=left;widths[i]=aw;
    if(bw) { tops[i+1]=y;heights[i+1]=bh;lefts[i+1]=left+aw+GAP;widths[i+1]=bw; }
    y+=std::max(ah,bh)+GAP;
  }
  contentHeight=y;
  scrollY=std::clamp(scrollY,0.0,std::max(0.0,contentHeight-DocHeight()));
}
void RefreshScroll() {
  SCROLLINFO si{sizeof(si),SIF_RANGE|SIF_PAGE|SIF_POS};
  si.nMin=0; si.nMax=1000000; si.nPage=static_cast<UINT>(std::clamp(DocHeight()/std::max(contentHeight,1.0)*1000000.0,1.0,1000000.0));
  si.nPos=static_cast<int>(scrollY/std::max(1.0,contentHeight)*1000000.0);
  SetScrollInfo(win,SB_VERT,&si,TRUE);
}
void ScrollTo(double y) { scrollY=std::clamp(y,0.0,std::max(0.0,contentHeight-DocHeight())); RefreshScroll(); RequestPaint(); }
// Page jumps must explicitly schedule the destination bitmap. Mouse-wheel and
// scrollbar handlers already did this, but Go/Prev/Next could leave an uncached
// page blank until another scroll or zoom event queued the render.
void QueueVisible();
void Jump(int page) {
  if(page<0 || page>=static_cast<int>(tops.size())) return;
  ScrollTo(tops[page]-GAP);
  QueueVisible();
}
int VisiblePage() {
  if(tops.empty()) return 0;
  auto it=std::upper_bound(tops.begin(),tops.end(),scrollY+DocHeight()*0.30);
  int p=std::clamp(static_cast<int>(it-tops.begin())-1,0,static_cast<int>(tops.size())-1);
  return twoPage ? p-(p%2) : p;
}
// All locations within the engine remain zero-based PDF page indices.
// The optional printed-page mapping is a constant offset at the UI boundary.
int ShownPage(int pdfIndex) { return pdfIndex+1+(bookMode&&bookOffsetSet?bookOffset:0); }
void UpdateBookControls() {
  if(!win) return;
  const bool enabled=bookMode&&pageCount>0;
  for(int id : {ID_BOOK_PDF, ID_BOOK_INPUT, ID_BOOK_SET, ID_BOOK_OFFSET})
    if(HWND control=ToolbarControl(id)) {
      if((IsWindowEnabled(control)!=FALSE)!=enabled) EnableWindow(control,enabled);
    }
  if(HWND toggle=ToolbarControl(ID_BOOK_TOGGLE)) {
    static bool lastDrawnMode=false;
    if(lastDrawnMode!=bookMode) {lastDrawnMode=bookMode;InvalidateRect(toggle,nullptr,FALSE);}
  }
  wchar_t b[100]{};
  swprintf_s(b,L"PDF %d",pageCount?VisiblePage()+1:0);
  if(HWND label=ToolbarControl(ID_BOOK_PDF)) {wchar_t old[100]{};GetWindowTextW(label,old,100);if(wcscmp(old,b)) {SetWindowTextW(label,b);InvalidateRect(label,nullptr,TRUE);}}
  if(HWND label=ToolbarControl(ID_BOOK_OFFSET)) {
    if(bookOffsetSet) swprintf_s(b,L"Offset %+d",bookOffset);
    else wcscpy_s(b,L"Offset unset");
    wchar_t old[100]{};GetWindowTextW(label,old,100);if(wcscmp(old,b)) {SetWindowTextW(label,b);InvalidateRect(label,nullptr,TRUE);}
  }
}
void UpdatePageField() {
  if(!pageBox) return;
  wchar_t b[100]{};
  // The total is always an actual PDF-page count, not a fictitious book end.
  swprintf_s(b,bookMode&&bookOffsetSet?L" / %d PDF":L" / %d",pageCount);
  if(HWND label=ToolbarControl(ID_PAGE_TOTAL)) {wchar_t old[100]{};GetWindowTextW(label,old,100);if(wcscmp(old,b)) {SetWindowTextW(label,b);InvalidateRect(label,nullptr,TRUE);}}
  // Never overwrite user input while they are typing a destination page.
  if(GetFocus()!=pageBox) {
    swprintf_s(b,L"%d",pageCount?ShownPage(VisiblePage()):0);
    wchar_t old[100]{};GetWindowTextW(pageBox,old,100);if(wcscmp(old,b))SetWindowTextW(pageBox,b);
  }
  UpdateBookControls();
}
bool ReadInteger(HWND control,int &result) {
  if(!control) return false;
  wchar_t text[128]{};
  GetWindowTextW(control,text,128);
  wchar_t *end=nullptr;
  const long long parsed=wcstoll(text,&end,10);
  if(end==text || *end!=L'\0' || parsed<-100000000 || parsed>100000000) return false;
  result=(int)parsed;
  return true;
}
void ApplyBookMapping() {
  if(!bookMode || pageCount<1) return;
  int printed=0;
  if(!ReadInteger(bookBox,printed)) {
    MessageBoxW(win,L"Enter a valid printed book page number.",L"Book page offset",MB_OK|MB_ICONWARNING);
    SetFocus(bookBox);SendMessageW(bookBox,EM_SETSEL,0,-1);return;
  }
  // Capture the PDF page at the instant the user applies the mapping.
  // For instance PDF 190 -> book 185 gives offset -5.
  bookOffset=printed-(VisiblePage()+1);
  bookOffsetSet=true;
  SetFocus(win);
  UpdatePageField();
}
// An ordinary Win32 EDIT places the caret where the mouse is clicked. Select
// the whole number AFTER the click completes so it can be replaced directly.
LRESULT CALLBACK PageEditProc(HWND h,UINT m,WPARAM w,LPARAM l) {
  LRESULT result=CallWindowProcW(originalPageEditProc,h,m,w,l);
  if(m==WM_LBUTTONUP || m==WM_LBUTTONDBLCLK || m==WM_SETFOCUS)
    SendMessageW(h,EM_SETSEL,0,-1);
  return result;
}
void QueueVisible() {
  std::lock_guard<std::mutex> lock(mx);
  if(dimensions.empty()) return;
  // Always queue visible pages first. The previous implementation queued two
  // off-screen pages before the viewport, causing white flashes on fast scrolls.
  const double viewTop=scrollY, viewBottom=scrollY+DocHeight();
  const double ahead=DocHeight()*1.25;
  const double behind=DocHeight()*0.65;
  const auto firstIt=std::lower_bound(tops.begin(),tops.end(),viewTop-behind,
    [&](double top,double value) { return top+GAP<value; });
  const int first=std::max(0,(int)(firstIt-tops.begin())-1);
  std::vector<std::pair<double,Request>> wanted;
  const double center=(viewTop+viewBottom)*0.5;
  for(int i=first;i<(int)tops.size() && tops[i]<viewBottom+ahead;i++) {
    if(tops[i]+heights[i]<viewTop-behind) continue;
    const int w=std::clamp((int)std::round(widths[i]),1,12000);
    const bool cached=std::any_of(cache.begin(),cache.end(),[&](const Entry &e) {
      return e.page==i && e.width==w && e.pixels;
    });
    if(cached) continue;
    const double lower=std::max(tops[i],viewTop);
    const double upper=std::min(tops[i]+heights[i],viewBottom);
    const bool onScreen=upper>lower;
    const double pageCenter=tops[i]+heights[i]*0.5;
    double priority=std::abs(pageCenter-center);
    if(!onScreen) priority+=DocHeight()*8;
    wanted.push_back({priority,{i,w,generation,i<(int)rotations.size()?rotations[i]:0}});
  }
  std::sort(wanted.begin(),wanted.end(),[](const auto &a,const auto &b){return a.first<b.first;});
  jobs.clear();
  for(const auto &item:wanted) jobs.push_back(item.second);
  cv.notify_one();
}
// Thumbnail requests are independent of the full-resolution render queue.
// Main document page renders are always processed before thumbnail requests.
int ThumbnailHeight() { RECT r{}; if(thumbnailPane) GetClientRect(thumbnailPane,&r); return std::max(1,(int)r.bottom); }
int ThumbnailMaxScroll() { return std::max(0, pageCount*THUMB_ROW-ThumbnailHeight()); }
void QueueThumbnails() {
  if(!thumbnailPane || !sidebar || chaptersMode || pageCount<=0) return;
  const int first=std::max(0,thumbnailScroll/THUMB_ROW-2);
  const int last=std::min(pageCount-1,(thumbnailScroll+ThumbnailHeight())/THUMB_ROW+2);
  {
    std::lock_guard<std::mutex> l(mx);
    thumbnailJobs.erase(std::remove_if(thumbnailJobs.begin(),thumbnailJobs.end(),[&](int p){return p<first || p>last;}),thumbnailJobs.end());
    for(int p=first;p<=last;p++) {
      const bool cached=std::any_of(thumbnailCache.begin(),thumbnailCache.end(),[&](const ThumbEntry &e){return e.page==p;});
      const bool queued=std::find(thumbnailJobs.begin(),thumbnailJobs.end(),p)!=thumbnailJobs.end();
      if(!cached && !queued) thumbnailJobs.push_back(p);
    }
  }
  cv.notify_one();
}
void ScrollThumbnail(int newY, bool userAction=false) {
  thumbnailScroll=std::clamp(newY,0,ThumbnailMaxScroll());
  if(userAction) thumbnailFollow=false;
  if(thumbnailPane && sidebar && !chaptersMode) InvalidateRect(thumbnailPane,nullptr,FALSE);
  QueueThumbnails();
}
void FollowThumbnailPage() {
  if(!thumbnailPane || chaptersMode || !sidebar || pageCount<=0 || !thumbnailFollow) return;
  int current=VisiblePage();
  int y=current*THUMB_ROW;
  if(y<thumbnailScroll+THUMB_ROW || y+THUMB_ROW>thumbnailScroll+ThumbnailHeight()-THUMB_ROW)
    ScrollThumbnail(y-ThumbnailHeight()/2+THUMB_ROW/2);
  else if(thumbnailPane) InvalidateRect(thumbnailPane,nullptr,FALSE);
}
void PaintThumbnails(HWND h) {
  PAINTSTRUCT ps{};HDC windowDC=BeginPaint(h,&ps);
  RECT r{};GetClientRect(h,&r);
  // Double buffer the native thumbnail pane to avoid introducing sidebar flicker.
  HDC bufferDC=CreateCompatibleDC(windowDC);
  HBITMAP bufferBitmap=bufferDC?CreateCompatibleBitmap(windowDC,std::max(1,(int)r.right),std::max(1,(int)r.bottom)):nullptr;
  HGDIOBJ oldBitmap=bufferBitmap?SelectObject(bufferDC,bufferBitmap):nullptr;
  HDC dc=bufferBitmap?bufferDC:windowDC;
  HBRUSH background=CreateSolidBrush(ThemePanel());FillRect(dc,&r,background);DeleteObject(background);
  SetBkMode(dc,TRANSPARENT);
  const int first=std::max(0,thumbnailScroll/THUMB_ROW);
  const int last=std::min(pageCount-1,(thumbnailScroll+std::max(0,(int)r.bottom))/THUMB_ROW+1);
  const int current=VisiblePage();
  for(int i=first;i<=last;i++) {
    int top=i*THUMB_ROW-thumbnailScroll;
    int pageW=120,pageH=135;
    std::shared_ptr<Pixels> pixels;
    {
      std::lock_guard<std::mutex> l(mx);
      for(auto &e:thumbnailCache) if(e.page==i) {pixels=e.pixels;e.age=++clockTick;break;}
    }
    if(pixels) {pageW=pixels->width;pageH=pixels->height;}
    else if(i<(int)dimensions.size()) {
      auto [pw,ph]=OrientedSize(i);
      double scale=std::min((double)THUMB_MAX_W/std::max(1.0,pw),(double)THUMB_MAX_H/std::max(1.0,ph));
      pageW=std::max(1,(int)std::round(pw*scale));pageH=std::max(1,(int)std::round(ph*scale));
    }
    int px=(r.right-pageW)/2,py=top+8+(THUMB_MAX_H-pageH)/2;
    RECT paper{px,py,px+pageW,py+pageH};
    HBRUSH white=CreateSolidBrush(RGB(255,255,255));FillRect(dc,&paper,white);DeleteObject(white);
    if(pixels) {
      BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
      bi.bmiHeader.biWidth=pixels->width;bi.bmiHeader.biHeight=-pixels->height;
      bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
      StretchDIBits(dc,px,py,pageW,pageH,0,0,pixels->width,pixels->height,pixels->data.data(),&bi,DIB_RGB_COLORS,SRCCOPY);
    }
    HPEN pen=CreatePen(PS_SOLID,i==current?2:1,i==current?RGB(50,115,205):RGB(94,99,110));
    HGDIOBJ old=SelectObject(dc,pen);HGDIOBJ brush=SelectObject(dc,GetStockObject(NULL_BRUSH));
    Rectangle(dc,px-1,py-1,px+pageW+1,py+pageH+1);
    SelectObject(dc,brush);SelectObject(dc,old);DeleteObject(pen);
    wchar_t number[32];swprintf_s(number,L"%d",i+1);
    RECT label{0,top+153,r.right,top+THUMB_ROW};
    SetTextColor(dc,i==current?RGB(50,115,205):ThemeText());
    DrawTextW(dc,number,-1,&label,DT_CENTER|DT_TOP|DT_SINGLELINE);
  }
  if(bufferBitmap) {
    BitBlt(windowDC,0,0,r.right,r.bottom,bufferDC,0,0,SRCCOPY);
    SelectObject(bufferDC,oldBitmap);DeleteObject(bufferBitmap);
  }
  if(bufferDC)DeleteDC(bufferDC);
  EndPaint(h,&ps);
}
LRESULT CALLBACK ThumbnailProc(HWND h,UINT m,WPARAM w,LPARAM l) {
  switch(m) {
    case WM_PAINT:PaintThumbnails(h);return 0;
    case WM_ERASEBKGND:return 1;
    case WM_MOUSEWHEEL:ScrollThumbnail(thumbnailScroll-GET_WHEEL_DELTA_WPARAM(w)/120*90,true);return 0;
    case WM_LBUTTONDOWN: {
      int row=(thumbnailScroll+GET_Y_LPARAM(l))/THUMB_ROW;
      if(row>=0 && row<pageCount) {thumbnailFollow=true;Jump(row);UpdatePageField();FollowThumbnailPage();}
      return 0;
    }
    case WM_SIZE:ScrollThumbnail(thumbnailScroll);return 0;
  }
  return DefWindowProcW(h,m,w,l);
}
void ShowError(std::wstring message) { errorText=std::move(message); RequestPaint(); }

void ReadBookmarks(FPDF_DOCUMENT doc,FPDF_BOOKMARK parent,int parentIndex,std::vector<Bookmark> &out,int depth=0) {
  if(depth>48 || out.size()>20000) return;
  auto current=FPDFBookmark_GetFirstChild(doc,parent);
  while(current && out.size()<20000) {
    unsigned long len=FPDFBookmark_GetTitle(current,nullptr,0);
    std::wstring title;
    if(len>=2 && len<16384) { std::vector<unsigned short> bytes((len+1)/2); FPDFBookmark_GetTitle(current,bytes.data(),len); title.assign(reinterpret_cast<wchar_t*>(bytes.data()), (len/2)>0?(len/2)-1:0); }
    FPDF_DEST dest=FPDFBookmark_GetDest(doc,current);
    if(!dest) { auto action=FPDFBookmark_GetAction(current); if(action) dest=FPDFAction_GetDest(doc,action); }
    int page=dest?FPDFDest_GetDestPageIndex(doc,dest):-1;
    int index=static_cast<int>(out.size()); out.push_back({title.empty()?L"(untitled)":title,page,parentIndex});
    ReadBookmarks(doc,current,index,out,depth+1);
    current=FPDFBookmark_GetNextSibling(doc,current);
  }
}
[[maybe_unused]] std::string Utf8(const std::wstring &s) {
  if(s.empty()) return {};
  int n=WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),nullptr,0,nullptr,nullptr);
  std::string out(n,'\0'); WideCharToMultiByte(CP_UTF8,0,s.data(),(int)s.size(),out.data(),n,nullptr,nullptr); return out;
}
void WorkerProc() {
  FPDF_InitLibrary(); FPDF_DOCUMENT doc=nullptr;
  for(;;) {
    Request req{}; std::wstring newFile; bool work=false; int thumbPage=-1, scanPage=-1; unsigned openGeneration=0, scanRevision=0; std::wstring scanText;
    {
      std::unique_lock<std::mutex> l(mx);
      cv.wait(l,[]{return stop||!pendingFile.empty()||!jobs.empty()||searching||!thumbnailJobs.empty();});
      if(stop) break;
      if(!pendingFile.empty()) { newFile=std::move(pendingFile); pendingFile.clear(); jobs.clear(); thumbnailJobs.clear(); openGeneration=generation; }
      else if(!jobs.empty()) {req=jobs.front(); jobs.pop_front();work=true;}
      else if(searching && searchScanPage<pageCount && loaded && !searchQuery.empty()) {
        scanPage=searchScanPage++;scanRevision=searchRevision;openGeneration=generation;scanText=searchQuery;
      }
      else if(searching) { searching=false;searchStatusDirty=true;if(win)PostMessageW(win,WM_SEARCH_RESULT,0,0); }
      else if(!thumbnailJobs.empty()) {thumbPage=thumbnailJobs.front();thumbnailJobs.pop_front();openGeneration=generation;}
    }
    if(!newFile.empty()) {
      if(doc) { FPDF_CloseDocument(doc); doc=nullptr; }
      // FPDF_LoadDocument expects an 8-bit local-codepage path on Windows.
      // Use FPDF_LoadMemDocument64 for full Unicode path support via Windows file I/O.
      std::vector<unsigned char> fileBytes;
      HANDLE f=CreateFileW(newFile.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
      if(f!=INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if(GetFileSizeEx(f,&size) && size.QuadPart>0 && size.QuadPart<INT_MAX) {
          fileBytes.resize((size_t)size.QuadPart);
          DWORD got=0;
          if(!ReadFile(f,fileBytes.data(),(DWORD)fileBytes.size(),&got,nullptr)||got!=fileBytes.size()) fileBytes.clear();
        }
        CloseHandle(f);
      }
      if(!fileBytes.empty()) doc=FPDF_LoadMemDocument64(fileBytes.data(),fileBytes.size(),nullptr);
      std::vector<std::pair<double,double>> dims; std::vector<Bookmark> bm;
      if(doc) {
        int count=std::max(0,FPDF_GetPageCount(doc)); dims.reserve(count);
        for(int i=0;i<count;i++) { double w=612,h=792; if(!FPDF_GetPageSizeByIndex(doc,i,&w,&h)||w<=0||h<=0) {w=612;h=792;} dims.push_back({w,h}); }
        ReadBookmarks(doc,nullptr,-1,bm);
      }
      // PDFium retains memory document bytes: keep fileBytes alive until document closes.
      static std::vector<unsigned char> keptBytes;
      keptBytes=std::move(fileBytes);
      {
        std::lock_guard<std::mutex> l(mx);
        if(openGeneration!=generation) continue;
        dimensions=std::move(dims); bookmarks=std::move(bm); rotations.assign(dimensions.size(),0);cache.clear(); jobs.clear(); thumbnailCache.clear(); thumbnailJobs.clear();
        loaded=doc!=nullptr; loading=false;
        searching=false;searchScanPage=0;searchQuery.clear();searchHits.clear();searchSelected=-1;++searchRevision;
        errorText=doc?L"":L"Cannot open PDF (unsupported, damaged, protected, or too large).";
        pageCount=(int)dimensions.size();
      }
      if(win) PostMessageW(win,WM_READY,0,0);
      continue;
    }
    if(scanPage>=0 && doc) {
      std::vector<SearchHit> found;
      FPDF_PAGE pg=FPDF_LoadPage(doc,scanPage);
      if(pg) {
        FPDF_TEXTPAGE tp=FPDFText_LoadPage(pg);
        if(tp) {
          // Match case-insensitively, without changing or indexing the PDF on disk.
          FPDF_SCHHANDLE sh=FPDFText_FindStart(tp,reinterpret_cast<FPDF_WIDESTRING>(scanText.c_str()),0,0);
          if(sh) {
            while(FPDFText_FindNext(sh)) {
              found.push_back({scanPage,FPDFText_GetSchResultIndex(sh)});
              if(found.size()>=512)break; // keep pathological pages bounded
            }
            FPDFText_FindClose(sh);
          }
          FPDFText_ClosePage(tp);
        }
        FPDF_ClosePage(pg);
      }
      {
        std::lock_guard<std::mutex> l(mx);
        if(scanRevision==searchRevision && openGeneration==generation) {
          for(const auto &hit:found) if(searchHits.size()<100000)searchHits.push_back(hit);
          searchStatusDirty=true;
          if(win && (scanPage%8==0 || !found.empty() || scanPage+1>=pageCount))PostMessageW(win,WM_SEARCH_RESULT,0,0);
        }
      }
      continue;
    }
    if(thumbPage>=0 && doc) {
      // Render the thumbnail in the PDFium worker, never on the UI thread.
      FPDF_PAGE pg=FPDF_LoadPage(doc,thumbPage);
      if(pg) {
        double pw=FPDF_GetPageWidth(pg),ph=FPDF_GetPageHeight(pg);
        double scale=std::min((double)THUMB_MAX_W/std::max(1.0,pw),(double)THUMB_MAX_H/std::max(1.0,ph));
        int tw=std::max(1,(int)std::round(pw*scale));
        int th=std::max(1,(int)std::round(ph*scale));
        int rotation=0;{std::lock_guard<std::mutex> l(mx);if(thumbPage<(int)rotations.size())rotation=rotations[thumbPage];}
        if(rotation&1) {std::swap(pw,ph);scale=std::min((double)THUMB_MAX_W/std::max(1.0,pw),(double)THUMB_MAX_H/std::max(1.0,ph));tw=std::max(1,(int)std::round(pw*scale));th=std::max(1,(int)std::round(ph*scale));}
        auto pix=std::make_shared<Pixels>();pix->width=tw;pix->height=th;pix->data.resize((size_t)tw*th*4);
        FPDF_BITMAP bmp=FPDFBitmap_CreateEx(tw,th,FPDFBitmap_BGRA,pix->data.data(),tw*4);
        if(bmp) {
          FPDFBitmap_FillRect(bmp,0,0,tw,th,0xffffffff);
          FPDF_RenderPageBitmap(bmp,pg,0,0,tw,th,rotation,FPDF_ANNOT);
          FPDFBitmap_Destroy(bmp);
          std::lock_guard<std::mutex> lock(mx);
          if(openGeneration==generation) {
            thumbnailCache.push_back({thumbPage,pix,++clockTick});
            size_t total=0;for(const auto &e:thumbnailCache)total+=e.pixels->data.size();
            while(total>24*1024*1024 && thumbnailCache.size()>1) {
              auto it=std::min_element(thumbnailCache.begin(),thumbnailCache.end(),[](const ThumbEntry&a,const ThumbEntry&b){return a.age<b.age;});
              total-=it->pixels->data.size();thumbnailCache.erase(it);
            }
            if(win) PostMessageW(win,WM_PIXELS,1,0);
          }
        }
        FPDF_ClosePage(pg);
      }
      continue;
    }
    if(work && doc) {
      { std::lock_guard<std::mutex> l(mx); if(req.generation!=generation) continue; }
      FPDF_PAGE page=FPDF_LoadPage(doc,req.page); if(!page) continue;
      double pw=FPDF_GetPageWidth(page),ph=FPDF_GetPageHeight(page);
      if(req.rotation&1)std::swap(pw,ph);
      int w=req.width,h=std::clamp((int)std::round(req.width*ph/std::max(1.0,pw)),1,16000);
      if((uint64_t)w*h>50000000ULL) {FPDF_ClosePage(page);continue;}
      auto pixels=std::make_shared<Pixels>();pixels->width=w;pixels->height=h;pixels->data.resize((size_t)w*h*4);
      FPDF_BITMAP bmp=FPDFBitmap_CreateEx(w,h,FPDFBitmap_BGRA,pixels->data.data(),w*4);
      if(bmp) {FPDFBitmap_FillRect(bmp,0,0,w,h,0xffffffff);FPDF_RenderPageBitmap(bmp,page,0,0,w,h,req.rotation,FPDF_ANNOT);FPDFBitmap_Destroy(bmp);}
      FPDF_ClosePage(page);
      if(bmp) {
        std::lock_guard<std::mutex> l(mx);
        if(req.generation==generation) {
          cache.erase(std::remove_if(cache.begin(),cache.end(),[&](const Entry &e){return e.page==req.page;}),cache.end());
          cache.push_back({req.page,w,0,pixels,nullptr,++clockTick});
          size_t total=0;for(auto &e:cache) if(e.pixels) total+=e.pixels->data.size();
          while(total>128*1024*1024 && cache.size()>1) {
            auto it=std::min_element(cache.begin(),cache.end(),[](const Entry&a,const Entry&b){return a.age<b.age;});
            total-=it->pixels?it->pixels->data.size():0;cache.erase(it);
          }
          if(win) PostMessageW(win,WM_PIXELS,0,0);
        }
      }
    }
  }
  if(doc) FPDF_CloseDocument(doc);
  FPDF_DestroyLibrary();
}
void OpenPDF(const std::wstring &file) {
  if(file.empty()) return;
  {
    std::lock_guard<std::mutex> l(mx);
    ++generation;pendingFile=file; filename=file;loading=true;loaded=false;pageCount=0;
    dimensions.clear();bookmarks.clear();rotations.clear();cache.clear();jobs.clear();thumbnailCache.clear();thumbnailJobs.clear();
  }
  {std::lock_guard<std::mutex> l(mx);searching=false;searchQuery.clear();searchHits.clear();searchSelected=-1;searchScanPage=0;++searchRevision;}
  if(searchBox)SetWindowTextW(searchBox,L"");
  fitPage=true;fitWidth=false;zoom=1.0;scrollY=0;thumbnailScroll=0;thumbnailFollow=true; bookMode=false;bookOffsetSet=false;bookOffset=0;
  if(bookBox) SetWindowTextW(bookBox,L"");
  tops.clear();heights.clear();lefts.clear();widths.clear();contentHeight=0;
  TreeView_DeleteAllItems(tree);if(thumbnailPane)InvalidateRect(thumbnailPane,nullptr,FALSE);SetWindowTextW(win,(L"quickDoc - "+fs::path(file).filename().wstring()).c_str());
  cv.notify_one();UpdatePageField();RequestPaint();
}
void ChoosePDF() {
  wchar_t path[32768]{}; OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=win;ofn.lpstrFilter=L"PDF files\0*.pdf\0All files\0*.*\0";
  ofn.lpstrFile=path;ofn.nMaxFile=32768;ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;
  if(GetOpenFileNameW(&ofn)) OpenPDF(path);
}
void LoadTree() {
  TreeView_DeleteAllItems(tree);
  std::vector<Bookmark> bm;{std::lock_guard<std::mutex> l(mx);bm=bookmarks;}
  std::vector<HTREEITEM> parents(bm.size(),nullptr);
  for(size_t i=0;i<bm.size();i++) {
    TVINSERTSTRUCTW t{};t.hParent=(bm[i].parent>=0 && bm[i].parent<(int)i)?parents[bm[i].parent]:TVI_ROOT;
    t.hInsertAfter=TVI_LAST;t.item.mask=TVIF_TEXT|TVIF_PARAM;t.item.pszText=bm[i].name.data();t.item.lParam=(LPARAM)i;
    parents[i]=TreeView_InsertItem(tree,&t);
    if(bm[i].parent<0) TreeView_Expand(tree,parents[i],TVE_EXPAND);
  }
}
void PaintControl(const DRAWITEMSTRUCT *d);

// The toolbar is a separate opaque child HWND. It never shares the Direct2D
// render target, so resizing and mode changes cannot leave native text/edge
// ghosts in the document's paint buffer.
LRESULT CALLBACK ToolbarProc(HWND h,UINT m,WPARAM w,LPARAM l) {
  switch(m) {
    case WM_ERASEBKGND: {
      RECT area{};GetClientRect(h,&area);
      FillRect((HDC)w,&area,darkPanelBrush);
      return 1;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);
      FillRect(dc,&ps.rcPaint,darkPanelBrush);
      EndPaint(h,&ps);
      return 0;
    }
    case WM_DRAWITEM:PaintControl((const DRAWITEMSTRUCT*)l);return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
      HDC dc=(HDC)w;SetTextColor(dc,ThemeText());SetBkColor(dc,ThemePanel());
      return (LRESULT)darkPanelBrush;
    }
    case WM_CTLCOLOREDIT: {
      HDC dc=(HDC)w;SetTextColor(dc,ThemeText());SetBkColor(dc,ThemeBG());
      return (LRESULT)darkEditBrush;
    }
    case WM_COMMAND:
      // Preserve the existing navigation code without changing command IDs.
      return SendMessageW(GetParent(h),WM_COMMAND,w,l);
  }
  return DefWindowProcW(h,m,w,l);
}
// Toolbar groups are indivisible, wrapping as a whole rather than overflowing
// onto each other. All placement is calculated from the current client width.
void ResizeChildren() {
  if(!win || !toolbar || !tree || !thumbnailPane || !splitter) return;
  RECT r{};GetClientRect(win,&r);
  const int clientWidth=std::max(1,static_cast<int>(r.right-r.left));
  struct Control {int id;int width;};
  using Group=std::vector<Control>;
  const std::vector<Group> groups={
    {{ID_OPEN,58}},
    {{ID_SIDEBAR,76}},
    {{ID_PREV,29},{ID_NEXT,29},{ID_PAGE,52},{ID_PAGE_TOTAL,76},{ID_GO,37}},
    {{ID_MINUS,30},{ID_PLUS,30},{ID_FIT,68},{ID_FIT_PAGE,66}},
    {{ID_ROTATE_LEFT,76},{ID_ROTATE_RIGHT,76}},
    {{ID_TWO_PAGE,91}},
    {{ID_THEME,102}},
    {{ID_BOOK_TOGGLE,86},{ID_BOOK_PDF,74},{ID_BOOK_INPUT,62},{ID_BOOK_SET,44},{ID_BOOK_OFFSET,110}},
    {{ID_SEARCH_FIELD,155},{ID_SEARCH_PREV,30},{ID_SEARCH_NEXT,30},{ID_SEARCH_STATUS,126},{ID_SEARCH_CLOSE,28}}
  };
  constexpr int innerGap=4, groupGap=14, margin=6, rowH=36, controlH=27;
  auto widthOf=[&](const Group &g) {
    int w=innerGap*std::max(0,(int)g.size()-1);
    for(const auto &c:g)w+=c.width;
    return w;
  };
  const int available=std::max(1,clientWidth-2*margin);
  std::vector<std::vector<int>> rows(1);
  std::vector<int> rowWidths(1,0);
  for(int gi=0;gi<(int)groups.size();++gi) {
    if(gi==8 && !searchVisible) continue;
    const int width=widthOf(groups[gi]);
    auto &row=rows.back();
    const int added=width+(row.empty()?0:groupGap);
    if(!row.empty() && rowWidths.back()+added>available) {
      rows.emplace_back();rowWidths.push_back(0);
    }
    const int gap=rows.back().empty()?0:groupGap;
    rows.back().push_back(gi);
    rowWidths.back()+=width+gap;
  }
  const int oldHeight=toolbarHeight;
  toolbarHeight=std::max(42,(int)rows.size()*rowH+6);

  // Compute the entire layout before moving a single child HWND.
  struct Placement { HWND hwnd; int x,y,width,height; };
  std::vector<Placement> positions;
  for(size_t ri=0;ri<rows.size();++ri) {
    const auto &row=rows[ri];
    // Left-align the row: controls keep their natural widths and fixed gaps.
    // Any unused horizontal space stays to the right, even after resizing.
    int x=margin;
    for(int gi:row) {
      for(const auto &c:groups[gi]) {
        HWND child=ToolbarControl(c.id);
        const int height=(c.id==ID_PAGE||c.id==ID_BOOK_INPUT||c.id==ID_SEARCH_FIELD)?25:controlH;
        if(child)positions.push_back({child,x,5+(int)ri*rowH,c.width,height});
        x+=c.width+innerGap;
      }
      x-=innerGap;
      x+=groupGap;
    }
  }
  // Deferred window positions prevent intermediate paints during resize.
  // If batching fails, retry every position individually instead of leaving
  // a partially applied layout. No intermediate repaint is requested.
  bool deferred=false;
  if(HDWP batch=BeginDeferWindowPos(std::max(1,(int)positions.size()))) {
    bool ok=true;
    for(const auto &p:positions) {
      HDWP next=DeferWindowPos(batch,p.hwnd,nullptr,p.x,p.y,p.width,p.height,
                               SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
      if(!next) {ok=false;break;}
      batch=next;
    }
    if(ok) deferred=EndDeferWindowPos(batch)!=FALSE;
    // A failed DeferWindowPos invalidates its HDWP handle. Do not reuse it.
  }
  if(!deferred)
    for(const auto &p:positions)
      SetWindowPos(p.hwnd,nullptr,p.x,p.y,p.width,p.height,
                   SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
  for(int id:{ID_SEARCH_FIELD,ID_SEARCH_PREV,ID_SEARCH_NEXT,ID_SEARCH_STATUS,ID_SEARCH_CLOSE})
    if(HWND c=ToolbarControl(id))ShowWindow(c,searchVisible?SW_SHOWNA:SW_HIDE);
  // Do not redraw at an intermediate size. The control area is painted in an
  // opaque HWND, rather than the Direct2D page target.
  SetWindowPos(toolbar,nullptr,0,0,clientWidth,toolbarHeight,
               SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW);
  RedrawWindow(toolbar,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);

  const int paneHeight=std::max(0,(int)r.bottom-toolbarHeight);
  const int maxSidebar=std::max(140,clientWidth-220);
  sidebarWidth=std::clamp(sidebarWidth,140,maxSidebar);
  MoveWindow(tree,0,toolbarHeight,sidebarWidth-5,paneHeight,TRUE);
  MoveWindow(thumbnailPane,0,toolbarHeight,sidebarWidth-5,paneHeight,TRUE);
  MoveWindow(splitter,sidebarWidth-5,toolbarHeight,5,paneHeight,TRUE);
  ShowWindow(splitter,sidebar?SW_SHOW:SW_HIDE);
  ShowWindow(tree,sidebar&&chaptersMode?SW_SHOW:SW_HIDE);
  ShowWindow(thumbnailPane,sidebar&&!chaptersMode?SW_SHOW:SW_HIDE);
  if(HWND toggle=ToolbarControl(ID_SIDEBAR)) {
    const wchar_t *label=chaptersMode?L"Pages":L"Chapters";
    wchar_t old[32]{};GetWindowTextW(toggle,old,32);
    if(wcscmp(old,label))SetWindowTextW(toggle,label);
  }
  if(oldHeight!=toolbarHeight) {
    RECT repaint{0,std::min(oldHeight,toolbarHeight),r.right,r.bottom};
    InvalidateRect(win,&repaint,FALSE);
  }
  RebuildLayout();RefreshScroll();QueueVisible();QueueThumbnails();RequestPaint();
}
void SetZoom(double value) {
  double old=zoom; double center=scrollY+DocHeight()*0.5;
  fitWidth=false;fitPage=false;zoom=std::clamp(value,0.2,5.0);RebuildLayout();
  ScrollTo(center*(zoom/old)-DocHeight()*0.5);QueueVisible();
}
void Paint() {
  PAINTSTRUCT ps{}; BeginPaint(win,&ps);
  RECT r{};GetClientRect(win,&r);
  int x=sidebar?sidebarWidth:0, top=toolbarHeight;
  if(!factory) {EndPaint(win,&ps);return;}
  if(!target) {
    D2D1_RENDER_TARGET_PROPERTIES props=D2D1::RenderTargetProperties();
    auto size=D2D1::SizeU(std::max(1,static_cast<int>(r.right)),std::max(1,static_cast<int>(r.bottom)));
    factory->CreateHwndRenderTarget(props,D2D1::HwndRenderTargetProperties(win,size),&target);
  }
  if(!target) {EndPaint(win,&ps);return;}
  const auto wantedSize=D2D1::SizeU(std::max(1,static_cast<int>(r.right)),std::max(1,static_cast<int>(r.bottom)));
  const auto currentSize=target->GetPixelSize();
  if(currentSize.width!=wantedSize.width || currentSize.height!=wantedSize.height)
    target->Resize(wantedSize);
  target->BeginDraw();
  // Paint static chrome only when its region has been invalidated (window
  // creation/resize/sidebar toggle), not on each scrolling frame.
  ComPtr<ID2D1SolidColorBrush> chrome;
  target->CreateSolidColorBrush(ThemeD2D(),&chrome);
  if(chrome && x>0 && ps.rcPaint.left<x)
    target->FillRectangle(D2D1::RectF(0,(FLOAT)toolbarHeight,(FLOAT)x,(FLOAT)r.bottom),chrome.Get());
  // Never clear over the native controls or the chapter tree.
  target->PushAxisAlignedClip(D2D1::RectF((FLOAT)x,(FLOAT)top,(FLOAT)r.right,(FLOAT)r.bottom),D2D1_ANTIALIAS_MODE_ALIASED);
  target->Clear(ThemeD2D());
  {
    std::lock_guard<std::mutex> lock(mx);
    for(int i=0;i<(int)tops.size();i++) {
      double y=tops[i]-scrollY+top;
      if(y+heights[i]<top || y>r.bottom) continue;
      double w=widths[i];
      double left=x+lefts[i];
      auto rect=D2D1::RectF((FLOAT)left,(FLOAT)y,(FLOAT)(left+w),(FLOAT)(y+heights[i]));
      const int expected=(int)std::round(w);
      Entry *chosen=nullptr;
      for(auto &e:cache) {
        if(e.page!=i || !e.pixels) continue;
        if(e.width==expected) { chosen=&e; break; }
        if(!chosen) chosen=&e; // Display scaled cached page until replacement is ready.
      }
      if(chosen) {
        chosen->age=++clockTick;
        if(!chosen->bitmap) {
          auto &p=*chosen->pixels;
          D2D1_BITMAP_PROPERTIES bp=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE));
          target->CreateBitmap(D2D1::SizeU(p.width,p.height),p.data.data(),p.width*4,bp,&chosen->bitmap);
        }
        if(chosen->bitmap) target->DrawBitmap(chosen->bitmap.Get(),rect,1.0f,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
      } else {
        // Neutral placeholder instead of bright white frames on uncached pages.
        ComPtr<ID2D1SolidColorBrush> placeholder;
        target->CreateSolidColorBrush(D2D1::ColorF(0xf4f4f4),&placeholder);
        if(placeholder) target->FillRectangle(rect,placeholder.Get());
      }
    }
  }
  target->PopAxisAlignedClip();
  HRESULT result=target->EndDraw();
  if(result==D2DERR_RECREATE_TARGET) {target.Reset();std::lock_guard<std::mutex> l(mx);for(auto &e:cache)e.bitmap.Reset();}
  EndPaint(win,&ps);
  if(tops.empty()) {
    HDC dc=GetDC(win);RECT textRect{x+24,top+50,r.right-24,r.bottom};
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,ThemeText());
    std::wstring msg;
    {std::lock_guard<std::mutex> l(mx);msg=loading?L"Opening PDF...":(errorText.empty()?L"Ctrl+O to open a PDF\nOr drag a PDF into this window":errorText);}
    DrawTextW(dc,msg.c_str(),-1,&textRect,DT_CENTER|DT_WORDBREAK);ReleaseDC(win,dc);
  }
}
void ChapterStep(int direction) {
  int page=VisiblePage();int selected=-1;
  {
    std::lock_guard<std::mutex> l(mx);
    if(direction>0) {
      int best=INT_MAX;
      for(const auto &b:bookmarks) if(b.page>page && b.page<best)best=b.page;
      if(best!=INT_MAX)selected=best;
    } else {
      int best=-1;
      for(const auto &b:bookmarks) if(b.page<page && b.page>best)best=b.page;
      selected=best;
    }
  }
  if(selected>=0) {Jump(selected);UpdatePageField();}
}

void RotateCurrent(int step) {
  if(pageCount<=0) return;
  const int current=VisiblePage();
  {
    std::lock_guard<std::mutex> l(mx);
    if(current<0 || current>=(int)rotations.size())return;
    rotations[current]=(rotations[current]+step+4)%4;
    ++generation; jobs.clear();thumbnailJobs.clear(); cache.clear();thumbnailCache.clear();
  }
  const double anchor=scrollY;
  RebuildLayout();ScrollTo(anchor);QueueVisible();QueueThumbnails();
  if(thumbnailPane)InvalidateRect(thumbnailPane,nullptr,FALSE);
  RequestPaint();
}
void ToggleFullscreen() {
  if(!fullScreen) {
    previousStyle=(DWORD)GetWindowLongPtrW(win,GWL_STYLE);
    previousPlacement.length=sizeof(previousPlacement);GetWindowPlacement(win,&previousPlacement);
    MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(win,MONITOR_DEFAULTTONEAREST),&mi);
    SetWindowLongPtrW(win,GWL_STYLE,(LONG_PTR)(previousStyle&~WS_OVERLAPPEDWINDOW));
    const RECT &r=mi.rcMonitor;
    SetWindowPos(win,HWND_TOP,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_FRAMECHANGED|SWP_NOOWNERZORDER);
    fullScreen=true;
  } else {
    SetWindowLongPtrW(win,GWL_STYLE,(LONG_PTR)previousStyle);
    SetWindowPlacement(win,&previousPlacement);
    SetWindowPos(win,nullptr,0,0,0,0,SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER);
    fullScreen=false;
  }
  ResizeChildren();
}
LRESULT CALLBACK SplitterProc(HWND h,UINT m,WPARAM w,LPARAM l) {
  switch(m) {
    case WM_SETCURSOR:SetCursor(LoadCursorW(nullptr,IDC_SIZEWE));return TRUE;
    case WM_LBUTTONDOWN:SetCapture(h);draggingSplitter=true;return 0;
    case WM_MOUSEMOVE:if(draggingSplitter && (w&MK_LBUTTON)) {
      POINT pt{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ClientToScreen(h,&pt);ScreenToClient(win,&pt);
      RECT r{};GetClientRect(win,&r);
      const int oldWidth=sidebarWidth;
      sidebarWidth=std::clamp(static_cast<int>(pt.x),140,std::max(140,static_cast<int>(r.right)-220));
      if(sidebarWidth!=oldWidth) {
        ResizeChildren();
        // Invalidate the swept strip, including the old splitter location.
        // Without this, a 5px ghost line remains on the D2D parent canvas.
        RECT exposed{std::max(0,std::min(oldWidth,sidebarWidth)-8),toolbarHeight,r.right,r.bottom};
        InvalidateRect(win,&exposed,FALSE);
        RedrawWindow(win,&exposed,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN);
      }
      return 0;
    }break;
    case WM_LBUTTONUP:if(draggingSplitter){draggingSplitter=false;ReleaseCapture();
      RECT r{};GetClientRect(win,&r);r.top=toolbarHeight;
      RedrawWindow(win,&r,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);
    }return 0;
    case WM_CAPTURECHANGED:draggingSplitter=false;return 0;
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);RECT r{};GetClientRect(h,&r);FillRect(dc,&r,darkPanelBrush);EndPaint(h,&ps);return 0;}
  }
  return DefWindowProcW(h,m,w,l);
}
void RefreshTheme() {
  if(darkPanelBrush) DeleteObject(darkPanelBrush);
  if(darkEditBrush) DeleteObject(darkEditBrush);
  darkPanelBrush=CreateSolidBrush(ThemePanel());
  darkEditBrush=CreateSolidBrush(ThemeBG());
  if(tree) {
    TreeView_SetBkColor(tree,ThemePanel());
    TreeView_SetTextColor(tree,ThemeText());
    TreeView_SetLineColor(tree,darkMode?RGB(100,105,115):RGB(175,180,188));
    SetWindowTheme(tree,darkMode?L"DarkMode_Explorer":L"Explorer",nullptr);
  }
  if(HWND b=ToolbarControl(ID_THEME)) {SetWindowTextW(b,darkMode?L"Dark mode: On":L"Dark mode: Off");InvalidateRect(b,nullptr,FALSE);}
  const BOOL title=darkMode?TRUE:FALSE;
  DwmSetWindowAttribute(win,20,&title,sizeof(title));
  if(thumbnailPane) InvalidateRect(thumbnailPane,nullptr,FALSE);
  // Theme changes are rare: invalidate toolbar, sidebar, and document together.
  RedrawWindow(win,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_FRAME);
}
void PaintControl(const DRAWITEMSTRUCT *d) {
  if(!d || !d->hDC || !d->hwndItem) return;
  HDC dc=d->hDC;RECT r=d->rcItem;
  const int saved=SaveDC(dc);IntersectClipRect(dc,r.left,r.top,r.right,r.bottom);
  // Opaque owner-draw static labels prevent old glyphs from accumulating when
  // their text changes (PDF page, offset, search status, page total).
  if(d->CtlType==ODT_STATIC) {
    FillRect(dc,&r,darkPanelBrush);
    wchar_t text[160]{};GetWindowTextW(d->hwndItem,text,160);
    SetBkMode(dc,TRANSPARENT);
    SetTextColor(dc,IsWindowEnabled(d->hwndItem)?ThemeText():(darkMode?RGB(116,120,128):RGB(150,155,160)));
    r.left+=3;
    DrawTextW(dc,text,-1,&r,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);
    RestoreDC(dc,saved);
    return;
  }
  bool checked=d->CtlID==ID_BOOK_TOGGLE && bookMode;
  HBRUSH bg=CreateSolidBrush((d->itemState&ODS_SELECTED)?(darkMode?RGB(65,72,84):RGB(214,220,230)):ThemePanel());
  FillRect(dc,&r,bg);DeleteObject(bg);
  HPEN pen=CreatePen(PS_SOLID,1,(d->itemState&ODS_FOCUS)?RGB(110,160,230):(darkMode?RGB(91,96,108):RGB(151,157,166)));
  HGDIOBJ old=SelectObject(dc,pen);HGDIOBJ nullbrush=SelectObject(dc,GetStockObject(NULL_BRUSH));
  Rectangle(dc,r.left,r.top,r.right,r.bottom);SelectObject(dc,nullbrush);SelectObject(dc,old);DeleteObject(pen);
  wchar_t label[80]{};GetWindowTextW(d->hwndItem,label,80);
  if(d->CtlID==ID_BOOK_TOGGLE) {
    wcscpy_s(label,L"Book #");
    RECT boxRect{r.left+7,r.top+(r.bottom-r.top-14)/2,r.left+21,r.top+(r.bottom-r.top-14)/2+14};
    DrawFrameControl(dc,&boxRect,DFC_BUTTON,DFCS_BUTTONCHECK | (checked?DFCS_CHECKED:0) | ((d->itemState&ODS_DISABLED)?DFCS_INACTIVE:0));
    r.left+=20;
  }
  SetBkMode(dc,TRANSPARENT);SetTextColor(dc,(d->itemState&ODS_DISABLED)?(darkMode?RGB(116,120,128):RGB(150,155,160)):ThemeText());
  DrawTextW(dc,label,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
  RestoreDC(dc,saved);
}

void UpdateSearchStatus() {
  if(!win)return;
  size_t count=0;int pos=-1, scanned=0;bool active=false;
  {std::lock_guard<std::mutex> l(mx);count=searchHits.size();pos=searchSelected;scanned=searchScanPage;active=searching;}
  std::wstring status;
  if(active)status=L""+std::to_wstring(count)+L" found ("+std::to_wstring(scanned)+L"/"+std::to_wstring(pageCount)+L")";
  else if(count)status=std::to_wstring(pos>=0?pos+1:0)+L" / "+std::to_wstring(count);
  else status=L"No matches";
  if(HWND c=ToolbarControl(ID_SEARCH_STATUS))SetWindowTextW(c,status.c_str());
}
void StartSearch() {
  if(!searchBox)return;
  int len=GetWindowTextLengthW(searchBox);
  std::wstring term((size_t)len+1,L'\0');GetWindowTextW(searchBox,term.data(),len+1);term.resize((size_t)len);
  {std::lock_guard<std::mutex> l(mx);
    ++searchRevision;searchQuery=std::move(term);searchScanPage=0;searchHits.clear();searchSelected=-1;
    searching=loaded&&!searchQuery.empty();
  }
  UpdateSearchStatus();cv.notify_one();
}
void StepSearch(int direction) {
  SearchHit result{};bool found=false;
  {std::lock_guard<std::mutex> l(mx);
    if(!searchHits.empty()) {
      int n=(int)searchHits.size();
      if(searchSelected<0) {
        const int current=VisiblePage();
        if(direction>0) {
          auto it=std::lower_bound(searchHits.begin(),searchHits.end(),current,[](const SearchHit &h,int page){return h.page<page;});
          searchSelected=(it==searchHits.end())?0:(int)(it-searchHits.begin());
        } else {
          auto it=std::upper_bound(searchHits.begin(),searchHits.end(),current,[](int page,const SearchHit &h){return page<h.page;});
          searchSelected=(it==searchHits.begin())?n-1:(int)(it-searchHits.begin())-1;
        }
      } else searchSelected=(searchSelected+direction+n)%n;
      result=searchHits[searchSelected];found=true;
    }
  }
  if(found)Jump(result.page);
  UpdateSearchStatus();
}
void SetSearchVisible(bool visible) {
  searchVisible=visible;
  ResizeChildren();
  if(visible && searchBox) {SetFocus(searchBox);SendMessageW(searchBox,EM_SETSEL,0,-1);}
  else SetFocus(win);
}
void Command(int id) {
  switch(id) {
    case ID_OPEN:ChoosePDF();break;
    case ID_SEARCH_FOCUS:SetSearchVisible(true);break;
    case ID_SEARCH_CLOSE:SetSearchVisible(false);break;
    case ID_SEARCH_NEXT:StepSearch(1);break;
    case ID_SEARCH_PREV:StepSearch(-1);break;
    case ID_SIDEBAR:chaptersMode=!chaptersMode;thumbnailFollow=true;ResizeChildren();FollowThumbnailPage();break;
    case ID_PREV:Jump(VisiblePage()-1);break;
    case ID_NEXT:Jump(VisiblePage()+1);break;
    case ID_GO:{
      int entered=0;
      if(ReadInteger(pageBox,entered)) {
        const long long pdf=(long long)entered-(bookMode&&bookOffsetSet?bookOffset:0);
        if(pdf>=1 && pdf<=pageCount) Jump((int)pdf-1);
        else MessageBeep(MB_ICONWARNING);
      } else MessageBeep(MB_ICONWARNING);
      SetFocus(win);break;
    }
    case ID_BOOK_TOGGLE:
      bookMode=!bookMode;
      // Retain mapping only while this document is open, even while toggled off.
      UpdateBookControls();
      if(HWND checkbox=ToolbarControl(ID_BOOK_TOGGLE))InvalidateRect(checkbox,nullptr,FALSE);
      if(bookMode&&!bookOffsetSet&&pageCount>0) { SetFocus(bookBox);SendMessageW(bookBox,EM_SETSEL,0,-1); }
      break;
    case ID_BOOK_SET:ApplyBookMapping();break;
    case ID_MINUS:SetZoom(zoom/1.2);break;
    case ID_PLUS:SetZoom(zoom*1.2);break;
    case ID_FIT:fitWidth=true;fitPage=false;RebuildLayout();RefreshScroll();QueueVisible();RequestPaint();break;
    case ID_FIT_PAGE:fitPage=true;fitWidth=false;RebuildLayout();RefreshScroll();QueueVisible();RequestPaint();break;
    case ID_PAGE:SetFocus(pageBox);SendMessageW(pageBox,EM_SETSEL,0,-1);break;
    case ID_CHAPTER_PREV:ChapterStep(-1);break;
    case ID_CHAPTER_NEXT:ChapterStep(1);break;
    case ID_ROTATE_LEFT:RotateCurrent(3);break;
    case ID_ROTATE_RIGHT:RotateCurrent(1);break;
    case ID_TWO_PAGE:twoPage=!twoPage;{HWND b=ToolbarControl(ID_TWO_PAGE);if(b)SetWindowTextW(b,twoPage?L"Two pages":L"Single page");}RebuildLayout();RefreshScroll();QueueVisible();RequestPaint();break;
    case ID_FULLSCREEN:ToggleFullscreen();break;
    case ID_THEME:darkMode=!darkMode;RefreshTheme();break;
    case ID_SCROLL_UP:Jump(VisiblePage()-(twoPage?2:1));thumbnailFollow=true;FollowThumbnailPage();break;
    case ID_SCROLL_DOWN:Jump(VisiblePage()+(twoPage?2:1));thumbnailFollow=true;FollowThumbnailPage();break;
  }
  UpdatePageField();
}
LRESULT CALLBACK Proc(HWND h,UINT m,WPARAM w,LPARAM l) {
  switch(m) {
  case WM_CREATE: {
    win=h;
    toolbar=CreateWindowExW(0,L"quickDoc.Toolbar",L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN,
                            0,0,100,toolbarHeight,h,nullptr,nullptr,nullptr);
    if(!toolbar)return -1;
    auto button=[&](int id,const wchar_t* text){CreateWindowW(L"BUTTON",text,WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,0,0,30,28,toolbar,(HMENU)(INT_PTR)id,nullptr,nullptr);};
    button(ID_OPEN,L"Open");button(ID_SIDEBAR,L"Chapters");button(ID_PREV,L"<");button(ID_NEXT,L">");
    pageBox=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"0",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL|ES_NUMBER,0,0,57,26,toolbar,(HMENU)(INT_PTR)ID_PAGE,nullptr,nullptr);
    originalPageEditProc=(WNDPROC)SetWindowLongPtrW(pageBox,GWLP_WNDPROC,(LONG_PTR)PageEditProc);
    CreateWindowW(L"STATIC",L" / 0",WS_CHILD|WS_VISIBLE|SS_OWNERDRAW,0,0,68,28,toolbar,(HMENU)(INT_PTR)ID_PAGE_TOTAL,nullptr,nullptr);
    button(ID_GO,L"Go");button(ID_MINUS,L"-");button(ID_PLUS,L"+");button(ID_FIT,L"Fit width");button(ID_FIT_PAGE,L"Fit page");button(ID_ROTATE_LEFT,L"Rotate <");button(ID_ROTATE_RIGHT,L"Rotate >");button(ID_TWO_PAGE,L"Single page");button(ID_THEME,L"Dark mode: On");
    CreateWindowW(L"BUTTON",L"Book #",WS_CHILD|WS_VISIBLE|BS_OWNERDRAW,0,0,74,28,toolbar,(HMENU)(INT_PTR)ID_BOOK_TOGGLE,nullptr,nullptr);
    CreateWindowW(L"STATIC",L"PDF 0",WS_CHILD|WS_VISIBLE|SS_OWNERDRAW,0,0,71,28,toolbar,(HMENU)(INT_PTR)ID_BOOK_PDF,nullptr,nullptr);
    bookBox=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,0,0,62,26,toolbar,(HMENU)(INT_PTR)ID_BOOK_INPUT,nullptr,nullptr);
    SetWindowLongPtrW(bookBox,GWLP_WNDPROC,(LONG_PTR)PageEditProc);
    button(ID_BOOK_SET,L"Set");
    searchBox=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|ES_AUTOHSCROLL,0,0,156,26,toolbar,(HMENU)(INT_PTR)ID_SEARCH_FIELD,nullptr,nullptr);
    button(ID_SEARCH_PREV,L"<");button(ID_SEARCH_NEXT,L">");
    CreateWindowW(L"STATIC",L"No matches",WS_CHILD|SS_OWNERDRAW,0,0,130,28,toolbar,(HMENU)(INT_PTR)ID_SEARCH_STATUS,nullptr,nullptr);
    button(ID_SEARCH_CLOSE,L"X");
    CreateWindowW(L"STATIC",L"Offset unset",WS_CHILD|WS_VISIBLE|SS_OWNERDRAW,0,0,110,28,toolbar,(HMENU)(INT_PTR)ID_BOOK_OFFSET,nullptr,nullptr);
    splitter=CreateWindowW(L"quickDoc.Splitter",L"",WS_CHILD|WS_VISIBLE,sidebarWidth-5,toolbarHeight,5,300,h,nullptr,nullptr,nullptr);
    thumbnailPane=CreateWindowExW(WS_EX_CLIENTEDGE,L"quickDoc.ThumbnailPane",L"",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,0,toolbarHeight,SIDEBAR,300,h,nullptr,nullptr,nullptr);
    tree=CreateWindowExW(WS_EX_CLIENTEDGE,WC_TREEVIEWW,L"",WS_CHILD|WS_VISIBLE|TVS_HASBUTTONS|TVS_HASLINES|TVS_LINESATROOT,0,40,250,300,h,(HMENU)(INT_PTR)ID_TREE,nullptr,nullptr);
    HFONT font=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
    for(int i=ID_OPEN;i<=ID_THEME;i++) if(HWND c=ToolbarControl(i))SendMessageW(c,WM_SETFONT,(WPARAM)font,FALSE);
    TreeView_SetBkColor(tree,ThemePanel());TreeView_SetTextColor(tree,ThemeText());TreeView_SetLineColor(tree,RGB(100,105,115));
    SetWindowTheme(tree,L"DarkMode_Explorer",nullptr);
    UpdateBookControls();DragAcceptFiles(h,TRUE);return 0;
  }
  case WM_GETMINMAXINFO: {
    auto* info=reinterpret_cast<MINMAXINFO*>(l);
    info->ptMinTrackSize.x=520;
    info->ptMinTrackSize.y=240;
    return 0;
  }
  case WM_SIZE:ResizeChildren();return 0;
  case WM_DPICHANGED: {
    RECT *suggested=reinterpret_cast<RECT*>(l);
    SetWindowPos(h,nullptr,suggested->left,suggested->top,suggested->right-suggested->left,suggested->bottom-suggested->top,SWP_NOZORDER|SWP_NOACTIVATE);
    return 0;
  }
  case WM_COMMAND:if(HIWORD(w)==BN_CLICKED || HIWORD(w)==1)Command(LOWORD(w));return 0;
  case WM_SEARCH_RESULT:UpdateSearchStatus();return 0;
  case WM_NOTIFY: {
    auto n=(NMHDR*)l;
    if(n->idFrom==ID_TREE && n->code==TVN_SELCHANGEDW) {
      auto tv=(NMTREEVIEWW*)l;int index=(int)tv->itemNew.lParam;int page=-1;
      {std::lock_guard<std::mutex> lock(mx);if(index>=0&&index<(int)bookmarks.size())page=bookmarks[index].page;}
      Jump(page);UpdatePageField();
    } return 0;
  }
  case WM_READY:{
    {std::lock_guard<std::mutex> guard(mx);pageCount=(int)dimensions.size();}
    RebuildLayout();RefreshScroll();LoadTree();QueueVisible();UpdatePageField();QueueThumbnails();FollowThumbnailPage();RequestPaint();return 0;
  }
  case WM_PIXELS:if(w==1) {if(thumbnailPane)InvalidateRect(thumbnailPane,nullptr,FALSE);} else RequestPaint();return 0;
  case WM_MOUSEWHEEL: {
    int delta=GET_WHEEL_DELTA_WPARAM(w);
    if(GET_KEYSTATE_WPARAM(w)&MK_CONTROL)SetZoom(zoom*std::pow(1.15,delta/120.0));
    else {ScrollTo(scrollY-delta/120.0*90);QueueVisible();UpdatePageField();thumbnailFollow=true;FollowThumbnailPage();}
    return 0;
  }
  case WM_VSCROLL: {
    SCROLLINFO si{sizeof(si),SIF_ALL};GetScrollInfo(h,SB_VERT,&si);
    if(LOWORD(w)==SB_THUMBTRACK || LOWORD(w)==SB_THUMBPOSITION) ScrollTo((double)si.nTrackPos/1000000.0*contentHeight);
    else if(LOWORD(w)==SB_PAGEUP)ScrollTo(scrollY-DocHeight());
    else if(LOWORD(w)==SB_PAGEDOWN)ScrollTo(scrollY+DocHeight());
    else if(LOWORD(w)==SB_LINEUP)ScrollTo(scrollY-70);
    else if(LOWORD(w)==SB_LINEDOWN)ScrollTo(scrollY+70);
    QueueVisible();UpdatePageField();thumbnailFollow=true;FollowThumbnailPage();return 0;
  }
  case WM_DROPFILES: {
    HDROP drop=(HDROP)w;wchar_t p[32768]{};DragQueryFileW(drop,0,p,32768);DragFinish(drop);OpenPDF(p);return 0;
  }
  case WM_PAINT:Paint();return 0;
  case WM_ERASEBKGND:return 1;
  case WM_DESTROY:{
    if(darkPanelBrush){DeleteObject(darkPanelBrush);darkPanelBrush=nullptr;}
    if(darkEditBrush){DeleteObject(darkEditBrush);darkEditBrush=nullptr;}
    {std::lock_guard<std::mutex> lock(mx);stop=true;}
    cv.notify_all();if(worker.joinable())worker.join();
    target.Reset();factory.Reset();PostQuitMessage(0);return 0;
  }
  } return DefWindowProcW(h,m,w,l);
}
} // namespace
#ifdef EMBED_PDFIUM
// The distributed EXE carries PDFium as a Win32 resource. On first use, a
// version-specific cache directory is created; future starts reuse it.
static bool InstallEmbeddedPdfium(HINSTANCE instance) {
  HRSRC res=FindResourceW(instance,MAKEINTRESOURCEW(201),RT_RCDATA);
  if(!res) return false;
  HGLOBAL hg=LoadResource(instance,res);
  DWORD len=SizeofResource(instance,res);
  auto bytes=reinterpret_cast<const unsigned char*>(LockResource(hg));
  if(!bytes||len<1000) return false;
  uint64_t hash=1469598103934665603ULL;
  for(DWORD i=0;i<len;i++) { hash^=bytes[i];hash*=1099511628211ULL; }
  wchar_t local[MAX_PATH]{};
  DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH);
  if(!n || n>=MAX_PATH) return false;
  wchar_t suffix[64]{};swprintf_s(suffix,L"\\quickDoc\\engine-%016llx",(unsigned long long)hash);
  std::wstring root=std::wstring(local)+L"\\quickDoc";
  CreateDirectoryW(root.c_str(),nullptr);
  std::wstring dir=std::wstring(local)+suffix;
  CreateDirectoryW(dir.c_str(),nullptr);
  std::wstring destination=dir+L"\\pdfium.dll";
  WIN32_FILE_ATTRIBUTE_DATA existing{};
  bool valid=GetFileAttributesExW(destination.c_str(),GetFileExInfoStandard,&existing) &&
    existing.nFileSizeHigh==0 && existing.nFileSizeLow==len;
  if(!valid) {
    std::wstring temp=destination+L".tmp";
    HANDLE file=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    DWORD wrote=0;BOOL ok=WriteFile(file,bytes,len,&wrote,nullptr);
    CloseHandle(file);
    if(!ok || wrote!=len || !MoveFileExW(temp.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) return false;
  }
  // Delay-loaded pdfium.dll resolves after this call, from the per-user cache.
  return SetDllDirectoryW(dir.c_str())!=0;
}
#endif
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR cmd,int show) {
#ifdef EMBED_PDFIUM
  if(!InstallEmbeddedPdfium(instance)) {
    MessageBoxW(nullptr,L"Could not initialize the bundled PDF engine.",L"quickDoc",MB_OK|MB_ICONERROR);
    return 3;
  }
#endif
  darkPanelBrush=CreateSolidBrush(ThemePanel());darkEditBrush=CreateSolidBrush(ThemeBG());
  INITCOMMONCONTROLSEX cc{sizeof(cc),ICC_TREEVIEW_CLASSES};InitCommonControlsEx(&cc);
  CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
  // Avoid DPI bitmap scaling by Windows on high-density displays.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf());
  WNDCLASSEXW thumbsClass{sizeof(thumbsClass)};thumbsClass.hInstance=instance;thumbsClass.lpszClassName=L"quickDoc.ThumbnailPane";thumbsClass.lpfnWndProc=ThumbnailProc;
  thumbsClass.hCursor=LoadCursorW(nullptr,IDC_ARROW);thumbsClass.hbrBackground=nullptr;RegisterClassExW(&thumbsClass);
  WNDCLASSEXW splitClass{sizeof(splitClass)};splitClass.hInstance=instance;splitClass.lpszClassName=L"quickDoc.Splitter";splitClass.lpfnWndProc=SplitterProc;splitClass.hCursor=LoadCursorW(nullptr,IDC_SIZEWE);RegisterClassExW(&splitClass);
  WNDCLASSEXW toolbarClass{sizeof(toolbarClass)};
  toolbarClass.hInstance=instance;
  toolbarClass.lpszClassName=L"quickDoc.Toolbar";
  toolbarClass.lpfnWndProc=ToolbarProc;
  toolbarClass.hCursor=LoadCursorW(nullptr,IDC_ARROW);
  toolbarClass.hbrBackground=nullptr;
  RegisterClassExW(&toolbarClass);
  WNDCLASSEXW c{sizeof(c)};c.hInstance=instance;c.lpszClassName=L"quickDoc.Window";c.lpfnWndProc=Proc;
  c.hCursor=LoadCursorW(nullptr,IDC_ARROW);c.hbrBackground=nullptr;
  c.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(101));
  c.hIconSm=(HICON)LoadImageW(instance,MAKEINTRESOURCEW(101),IMAGE_ICON,16,16,LR_DEFAULTCOLOR);
  RegisterClassExW(&c);
  win=CreateWindowExW(0,c.lpszClassName,L"quickDoc",WS_OVERLAPPEDWINDOW|WS_VSCROLL|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,1060,820,nullptr,nullptr,instance,nullptr);
  if(!win)return 2;
  const BOOL darkTitle=TRUE;DwmSetWindowAttribute(win,20,&darkTitle,sizeof(darkTitle));
  worker=std::thread(WorkerProc);
  ShowWindow(win,show);UpdateWindow(win);
  if(cmd && *cmd) {std::wstring path=cmd;if(path.front()==L'"'&&path.back()==L'"')path=path.substr(1,path.size()-2);OpenPDF(path);}
  ACCEL accelerators[]={{FVIRTKEY|FCONTROL,'O',ID_OPEN},{FVIRTKEY|FCONTROL,'G',ID_PAGE},{FVIRTKEY,VK_PRIOR,ID_PREV},{FVIRTKEY,VK_NEXT,ID_NEXT},{FVIRTKEY,VK_F12,ID_SIDEBAR},{FVIRTKEY|FCONTROL,'1',ID_FIT},{FVIRTKEY|FCONTROL,'2',ID_FIT_PAGE},{FVIRTKEY|FCONTROL,VK_PRIOR,ID_CHAPTER_PREV},{FVIRTKEY|FCONTROL,VK_NEXT,ID_CHAPTER_NEXT},{FVIRTKEY,VK_F11,ID_FULLSCREEN},{FVIRTKEY|FCONTROL,'R',ID_ROTATE_RIGHT},{FVIRTKEY|FCONTROL,'F',ID_SEARCH_FOCUS}};
  HACCEL accel=CreateAcceleratorTableW(accelerators,(int)std::size(accelerators));
  MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0) {
    if(msg.message==WM_KEYDOWN && (msg.wParam==VK_UP || msg.wParam==VK_DOWN)) {
      HWND focused=GetFocus();
      wchar_t klass[32]{};
      if(focused) GetClassNameW(focused,klass,32);
      // Do not consume arrows while editing numbers/text or browsing bookmarks.
      if(wcscmp(klass,L"Edit")!=0 && focused!=tree) {
        Command(msg.wParam==VK_UP?ID_SCROLL_UP:ID_SCROLL_DOWN);
        continue;
      }
    }
    if(msg.message==WM_KEYDOWN && msg.wParam==VK_RETURN) {
      if(GetFocus()==pageBox) {Command(ID_GO);continue;}
      if(GetFocus()==bookBox && bookMode) {ApplyBookMapping();continue;}
      if(GetFocus()==searchBox) {StartSearch();StepSearch(1);continue;}
    }
    if(msg.message==WM_KEYDOWN && msg.wParam==VK_ESCAPE && GetFocus()==searchBox) {SetSearchVisible(false);continue;}
    if(!TranslateAcceleratorW(win,accel,&msg)) {TranslateMessage(&msg);DispatchMessageW(&msg);}
  }
  DestroyAcceleratorTable(accel);CoUninitialize();return (int)msg.wParam;
}
