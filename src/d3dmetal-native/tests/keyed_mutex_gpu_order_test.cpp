// Isolated keyed-mutex ordering probe, derived from MIT UTM shared_texture_test.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include "d3dmetal_native.h"
#include "common/com.h"
#include "common/ipc.h"
#define T_TAG "KEYEDGPU"
#include "common/check.h"
namespace {
constexpr unsigned W=1664,H=928,TICKS=32;
int device(Com<ID3D11Device>& d, Com<ID3D11DeviceContext>& c) {
 EXPECT(dmn_init(nullptr)==DMN_SUCCESS,"init");
 D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_11_0,actual;
 CK(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,&level,1,D3D11_SDK_VERSION,&d,&actual,&c),"device");
 return 0;
}
void colour(unsigned tick, float f[4], unsigned char b[4]) {
 b[0]=(tick*43)%256;b[1]=(tick*79)%256;b[2]=(tick*131)%256;b[3]=255;
 for(unsigned i=0;i<4;i++)f[i]=float(b[i])/255;
}
int producer(int sock) {
 Com<ID3D11Device>d;Com<ID3D11DeviceContext>c;EXPECT(device(d,c)==0,"producer device");
 D3D11_TEXTURE2D_DESC td{};td.Width=W;td.Height=H;td.MipLevels=1;td.ArraySize=1;
 td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;
 td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;td.MiscFlags=D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
 Com<ID3D11Texture2D>tex;CK(d->CreateTexture2D(&td,nullptr,&tex),"shared texture");
 Com<IDXGIResource>res;CK(tex->QueryInterface(__uuidof(IDXGIResource),(void**)&res),"resource");
 HANDLE handle=nullptr;CK(res->GetSharedHandle(&handle),"export");
 dmn_shared_texture_handle pod{};memcpy(&pod,handle,sizeof(pod));
 Com<IDXGIKeyedMutex>km;CK(tex->QueryInterface(__uuidof(IDXGIKeyedMutex),(void**)&km),"producer mutex");
 Com<ID3D11RenderTargetView>rt;CK(d->CreateRenderTargetView(tex.ptr(),nullptr,&rt),"rtv");
 EXPECT(send_with_fd(sock,&pod,sizeof(pod),pod.fd),"send allocation");
 char ack=0;EXPECT(read(sock,&ack,1)==1,"consumer ready");
 for(unsigned tick=1;tick<=TICKS;tick++) {
  CK_OK(km->AcquireSync(0,5000),"producer acquire");
  float f[4];unsigned char bytes[4];colour(tick,f,bytes);
  c->ClearRenderTargetView(rt.ptr(),f);
  c->Flush(); // Shared writes must Flush; deliberately no precompletion fence.
  CK_OK(km->ReleaseSync(tick),"producer release");
  EXPECT(write(sock,&tick,sizeof(tick))==sizeof(tick),"announce tick");
  EXPECT(read(sock,&ack,1)==1,"consumer ack");
 }
 return 0;
}
int consumer(int sock) {
 dmn_shared_texture_handle pod{};int fd=-1;EXPECT(recv_with_fd(sock,&pod,sizeof(pod),&fd),"receive");pod.fd=fd;
 Com<ID3D11Device>d;Com<ID3D11DeviceContext>c;EXPECT(device(d,c)==0,"consumer device");
 Com<ID3D11Texture2D>tex;CK(d->OpenSharedResource(&pod,__uuidof(ID3D11Texture2D),(void**)&tex),"import");
 Com<IDXGIKeyedMutex>km;CK(tex->QueryInterface(__uuidof(IDXGIKeyedMutex),(void**)&km),"consumer mutex");
 D3D11_TEXTURE2D_DESC td{};tex->GetDesc(&td);td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;td.BindFlags=td.MiscFlags=0;
 Com<ID3D11Texture2D>staging;CK(d->CreateTexture2D(&td,nullptr,&staging),"staging");
 const size_t mapsize=(pod.size+16383)&~size_t(16383);const unsigned char*cpu=(const unsigned char*)mmap(nullptr,mapsize,PROT_READ,MAP_SHARED,fd,0);EXPECT(cpu!=MAP_FAILED,"CPU map");
 const size_t at=(H/2)*pod.stride+(W/2)*4;unsigned cpuBad=0,gpuBad=0;
 char ack='R';EXPECT(write(sock,&ack,1)==1,"ready");
 for(unsigned want=1;want<=TICKS;want++) {
  unsigned tick=0;EXPECT(read(sock,&tick,sizeof(tick))==sizeof(tick)&&tick==want,"tick");
  CK_OK(km->AcquireSync(tick,5000),"consumer acquire");
  float f[4];unsigned char bytes[4],early[4];colour(tick,f,bytes);memcpy(early,cpu+at,4);
  bool earlymatch=memcmp(early,bytes,4)==0;if(!earlymatch)cpuBad++;
  c->CopyResource(staging.ptr(),tex.ptr());
  D3D11_MAPPED_SUBRESOURCE mapped{};CK(c->Map(staging.ptr(),0,D3D11_MAP_READ,0,&mapped),"readback");
  unsigned char got[4];memcpy(got,(unsigned char*)mapped.pData+(H/2)*mapped.RowPitch+(W/2)*4,4);
  c->Unmap(staging.ptr(),0);bool match=memcmp(got,bytes,4)==0;if(!match)gpuBad++;
  printf(T_TAG ": tick=%u expected=%02x%02x%02x%02x cpuEarly=%02x%02x%02x%02x GPU=%02x%02x%02x%02x %s\n",tick,bytes[0],bytes[1],bytes[2],bytes[3],early[0],early[1],early[2],early[3],got[0],got[1],got[2],got[3],match?"OK":"MISMATCH");
  CK_OK(km->ReleaseSync(0),"consumer release");ack='A';EXPECT(write(sock,&ack,1)==1,"ack");
 }
 munmap((void*)cpu,mapsize);close(fd);
 printf(T_TAG ": CPU early mismatches=%u/%u GPU readback mismatches=%u/%u\n",cpuBad,TICKS,gpuBad,TICKS);
 EXPECT(gpuBad==0,"keyed mutex GPU ordering failed");T_PASS();return 0;
}
}
int main(int argc,char**argv) {
 if(argc==3&&strcmp(argv[1],"--consumer")==0)return consumer(atoi(argv[2]));
 return run_exec_pair(T_TAG,argv[0],"--consumer",producer);
}
