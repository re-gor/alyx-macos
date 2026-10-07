/* MIT-derived regression: creator makes an undefined shared texture, peer
 * fills the imported view and completes GPU work, creator's FIRST GPU use
 * must preserve and sample those bytes. A late creator zero-fill used to erase
 * the peer's complete image. Separate fresh runs sample first or blit first.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <sys/mman.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <windows.h>

#include "d3dmetal_native.h"
#include "common/com.h"

#define T_TAG "SHAREDPRODUCER"
#include "common/check.h"
#include "common/skip.h"
#include "common/ipc.h"

namespace {

constexpr uint32_t kW = 256, kH = 256;

/* Per-texel gradient (RGBA8): B = x scaled, G = y scaled, R = (x+y) scaled.
 * A flat/box sample zeroes the colour channels, which this catches. */
void gradient_texel(uint32_t x, uint32_t y, uint8_t out[4]) {
    out[0] = (uint8_t)(x * 255 / (kW - 1));        /* R */
    out[1] = (uint8_t)(y * 255 / (kH - 1));        /* G */
    out[2] = (uint8_t)((x + y) * 255 / (kW + kH)); /* B */
    out[3] = 0xff;                                 /* A */
}

const char* kVS =
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut main(uint vid : SV_VertexID) {\n"
    "    VSOut o;\n"
    "    float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "    o.uv = p;\n"
    "    o.pos = float4(p * 2.0 - 1.0, 0, 1);\n"
    "    o.pos.y = -o.pos.y;\n"
    "    return o;\n"
    "}\n";

/* Normalised-coordinate sample (the natural D2D/DWM path); this is the access
 * that flattens for a 2D-as-array binding. */
const char* kPS =
    "Texture2D tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
    "    return tex.Sample(smp, uv);\n"
    "}\n";

HRESULT make_device(Com<ID3D11Device>& dev, Com<ID3D11DeviceContext>& ctx) {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1, flo;
    return D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                             D3D11_SDK_VERSION, &dev, &flo, &ctx);
}

/* CopyResource a texture into a fresh CPU-readable staging copy and read texel
 * (x,y). Also forces the source GPU work to complete (Map blocks). */
int read_texel(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* src,
               uint32_t x, uint32_t y, uint8_t out[4]) {
    D3D11_TEXTURE2D_DESC d = {};
    src->GetDesc(&d);
    d.BindFlags = 0;
    d.MiscFlags = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Com<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &staging)))
        return 1;
    ctx->CopyResource(staging.ptr(), src);
    ctx->Flush();
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(staging.ptr(), 0, D3D11_MAP_READ, 0, &m)))
        return 1;
    const uint8_t* p = (const uint8_t*)m.pData + (size_t)y * m.RowPitch + (size_t)x * 4;
    memcpy(out, p, 4);
    ctx->Unmap(staging.ptr(), 0);
    return 0;
}

int producer(int sock) {
    EXPECT(dmn_init(nullptr) == DMN_SUCCESS, "producer init");
    T_SKIP_WITHOUT("D3DCompile");
    Com<ID3D11Device> cdev; Com<ID3D11DeviceContext> cctx;
    CK(make_device(cdev,cctx), "owner device");
    D3D11_TEXTURE2D_DESC td{};
    td.Width=kW;td.Height=kH;td.MipLevels=1;td.ArraySize=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;td.SampleDesc.Count=1;
    td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;td.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
    // No local upload, clear or rendering of the original producer texture.
    Com<ID3D11Texture2D> opened;
    CK(cdev->CreateTexture2D(&td,nullptr,&opened), "owner original texture");
    Com<IDXGIResource> resource; CK(opened->QueryInterface(__uuidof(IDXGIResource),(void**)&resource), "owner resource");
    HANDLE handle=nullptr; CK(resource->GetSharedHandle(&handle), "owner export");
    dmn_shared_texture_handle wire{};memcpy(&wire,handle,sizeof(wire));
    EXPECT(send_with_fd(sock,&wire,sizeof(wire),wire.fd), "send allocation");
    char ready=0;EXPECT(read(sock,&ready,1)==1 && ready=='R', "child fill completed");
    uint8_t want[4];gradient_texel(kW*3/4,kH/4,want);
    const size_t mapped_length=(wire.size+16383)&~size_t(16383);
    const auto* cpu=static_cast<const uint8_t*>(mmap(nullptr,mapped_length,PROT_READ,MAP_SHARED,wire.fd,0));
    EXPECT(cpu!=MAP_FAILED,"owner CPU backing");
    const size_t pixel=(kH/4)*wire.stride+(kW*3/4)*4;
    printf(T_TAG ": CPU before owner GPU read=%02x%02x%02x%02x expected=%02x%02x%02x%02x\n",cpu[pixel],cpu[pixel+1],cpu[pixel+2],cpu[pixel+3],want[0],want[1],want[2],want[3]);
    EXPECT(memcmp(cpu+pixel,want,4)==0,"peer data missing before owner read");
    const bool blit_first=getenv("PRODUCERSAMPLE_BLIT_FIRST")!=nullptr;
    printf(T_TAG ": order=%s\n",blit_first ? "blit before sample" : "sample before blit");
    if(blit_first) {
    /* Control: blit readback of the opened surface — the always-correct path. */
    uint8_t bc[4];
    EXPECT(read_texel(cdev.ptr(), cctx.ptr(), opened.ptr(), kW * 3 / 4, kH / 4, bc) == 0,
           "blit readback failed");
    printf(T_TAG ": blit  readback RGBA=%02x%02x%02x%02x (expect %02x%02x%02x%02x)\n",
           bc[0], bc[1], bc[2], bc[3], want[0], want[1], want[2], want[3]);
    printf(T_TAG ": CPU after owner blit=%02x%02x%02x%02x\n",cpu[pixel],cpu[pixel+1],cpu[pixel+2],cpu[pixel+3]);
    EXPECT(memcmp(cpu+pixel,want,4)==0,"owner blit erased peer data");
    EXPECT(bc[0] == want[0] && bc[1] == want[1] && bc[2] == want[2],
           "blit readback did not match gradient (producer/copy path broken)");

    }
    /* Sample the opened SRV through a pixel shader into a private RT. */
    Com<ID3D11ShaderResourceView> srv;
    CK(cdev->CreateShaderResourceView(opened.ptr(), nullptr, &srv), "CreateSRV");

    D3D11_TEXTURE2D_DESC rd = td;
    rd.BindFlags = D3D11_BIND_RENDER_TARGET;
    rd.MiscFlags = 0;
    Com<ID3D11Texture2D> rt;
    CK(cdev->CreateTexture2D(&rd, nullptr, &rt), "CreateTexture2D(rt)");
    Com<ID3D11RenderTargetView> rtv;
    CK(cdev->CreateRenderTargetView(rt.ptr(), nullptr, &rtv), "CreateRTV");

    Com<ID3DBlob> vsb, psb, errb;
    CK(D3DCompile(kVS, strlen(kVS), "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0,
                  &vsb, &errb), "VS compile");
    CK(D3DCompile(kPS, strlen(kPS), "ps", nullptr, nullptr, "main", "ps_5_0", 0, 0,
                  &psb, &errb), "PS compile");
    Com<ID3D11VertexShader> vs;
    Com<ID3D11PixelShader> ps;
    CK(cdev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr,
                                &vs), "CreateVS");
    CK(cdev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr,
                               &ps), "CreatePS");
    D3D11_SAMPLER_DESC smpd = {};
    smpd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smpd.AddressU = smpd.AddressV = smpd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    Com<ID3D11SamplerState> samp;
    CK(cdev->CreateSamplerState(&smpd, &samp), "CreateSampler");

    FLOAT clear[4] = {0, 0, 0, 1};
    cctx->ClearRenderTargetView(rtv.ptr(), clear);
    cctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp = {0, 0, (float)kW, (float)kH, 0, 1};
    cctx->RSSetViewports(1, &vp);
    cctx->IASetInputLayout(nullptr);
    cctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cctx->VSSetShader(vs.ptr(), nullptr, 0);
    cctx->PSSetShader(ps.ptr(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {srv.ptr()};
    cctx->PSSetShaderResources(0, 1, srvs);
    ID3D11SamplerState* samps[] = {samp.ptr()};
    cctx->PSSetSamplers(0, 1, samps);
    cctx->Draw(3, 0);
    cctx->Flush();

    /* Read the shaded texel that sampled source (48,16). */
    uint8_t sc[4];
    EXPECT(read_texel(cdev.ptr(), cctx.ptr(), rt.ptr(), kW * 3 / 4, kH / 4, sc) == 0,
           "sampled readback failed");
    printf(T_TAG ": shader sample RGBA=%02x%02x%02x%02x (expect %02x%02x%02x%02x)\n",
           sc[0], sc[1], sc[2], sc[3], want[0], want[1], want[2], want[3]);

    /* Tolerate filter/sRGB rounding; the bug returns flat zero colour (box). */
    printf(T_TAG ": CPU after owner sample=%02x%02x%02x%02x\n",cpu[pixel],cpu[pixel+1],cpu[pixel+2],cpu[pixel+3]);
    EXPECT(memcmp(cpu+pixel,want,4)==0,"owner shader sample erased peer data");
    auto near = [](uint8_t a, uint8_t b) { return (a > b ? a - b : b - a) <= 8; };
    EXPECT(near(sc[0], want[0]) && near(sc[1], want[1]) && near(sc[2], want[2]),
           "shader-sampled shared SRV colour flattened (glyph-box regression)");

    if(!blit_first) {
    /* Control: blit readback of the opened surface — the always-correct path. */
    uint8_t bc[4];
    EXPECT(read_texel(cdev.ptr(), cctx.ptr(), opened.ptr(), kW * 3 / 4, kH / 4, bc) == 0,
           "blit readback failed");
    printf(T_TAG ": blit  readback RGBA=%02x%02x%02x%02x (expect %02x%02x%02x%02x)\n",
           bc[0], bc[1], bc[2], bc[3], want[0], want[1], want[2], want[3]);
    printf(T_TAG ": CPU after owner blit=%02x%02x%02x%02x\n",cpu[pixel],cpu[pixel+1],cpu[pixel+2],cpu[pixel+3]);
    EXPECT(memcmp(cpu+pixel,want,4)==0,"owner blit erased peer data");
    EXPECT(bc[0] == want[0] && bc[1] == want[1] && bc[2] == want[2],
           "blit readback did not match gradient (producer/copy path broken)");

    }
    char done='D';EXPECT(write(sock,&done,1)==1,"owner finished");
    munmap(const_cast<uint8_t*>(cpu),mapped_length);
    T_PASS(); return 0;
}
int consumer(int sock) {
    EXPECT(dmn_init(nullptr)==DMN_SUCCESS,"child init");
    dmn_shared_texture_handle wire{};int received=-1;EXPECT(recv_with_fd(sock,&wire,sizeof(wire),&received),"receive allocation");wire.fd=received;
    Com<ID3D11Device> dev;Com<ID3D11DeviceContext> ctx;CK(make_device(dev,ctx),"child device");
    Com<ID3D11Texture2D> tex;CK(dev->OpenSharedResource(&wire,__uuidof(ID3D11Texture2D),(void**)&tex),"child import");
    D3D11_TEXTURE2D_DESC td{};tex->GetDesc(&td);td.Usage=D3D11_USAGE_STAGING;td.BindFlags=td.MiscFlags=0;td.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    Com<ID3D11Texture2D> upload;CK(dev->CreateTexture2D(&td,nullptr,&upload),"child upload");
    D3D11_MAPPED_SUBRESOURCE mapped{};CK(ctx->Map(upload.ptr(),0,D3D11_MAP_WRITE,0,&mapped),"child fill map");
    for(uint32_t y=0;y<kH;y++)for(uint32_t x=0;x<kW;x++)gradient_texel(x,y,(uint8_t*)mapped.pData+y*mapped.RowPitch+x*4);
    ctx->Unmap(upload.ptr(),0);ctx->CopyResource(tex.ptr(),upload.ptr());
    uint8_t got[4],want[4];gradient_texel(kW*3/4,kH/4,want);
    EXPECT(read_texel(dev.ptr(),ctx.ptr(),tex.ptr(),kW*3/4,kH/4,got)==0,"child completion readback");
    EXPECT(memcmp(got,want,4)==0,"child gradient mismatch");
    char ready='R';EXPECT(write(sock,&ready,1)==1,"child ready");
    char done=0;EXPECT(read(sock,&done,1)==1&&done=='D',"owner acknowledgment");close(received);return 0;
}
} // namespace
int main(int argc,char**argv) {
    if(argc==3&&strcmp(argv[1],"--consumer")==0)return consumer(atoi(argv[2]));
    return run_exec_pair(T_TAG,argv[0],"--consumer",producer);
}
