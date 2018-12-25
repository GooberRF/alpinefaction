#include "stdafx.h"
#include "graphics.h"
#include "utils.h"
#include "rf.h"
#include "main.h"
#include "gr_color.h"
#include "inline_asm.h"

using namespace rf;

static float g_GrClippedGeomOffsetX = -0.5;
static float g_GrClippedGeomOffsetY = -0.5;

auto GrInitBuffers_AfterReset_Hook = makeCallHook(GrInitBuffers);

static void SetTextureMinMagFilterInCode(D3DTEXTUREFILTERTYPE FilterType)
{
    uintptr_t Addresses[] = {
        // GrInitD3D
        0x00546283, 0x00546295,
        0x005462A9, 0x005462BD,
        // GrD3DSetMaterialFlags
        0x0054F2E8, 0x0054F2FC,
        0x0054F438, 0x0054F44C,
        0x0054F567, 0x0054F57B,
        0x0054F62D, 0x0054F641,
        0x0054F709, 0x0054F71D,
        0x0054F800, 0x0054F814,
        0x0054F909, 0x0054F91D,
        0x0054FA1A, 0x0054FA2E,
        0x0054FB22, 0x0054FB36,
        0x0054FBFE, 0x0054FC12,
        0x0054FD06, 0x0054FD1A,
        0x0054FD90, 0x0054FDA4,
        0x0054FE98, 0x0054FEAC,
        0x0054FF74, 0x0054FF88,
        0x0055007C, 0x00550090,
        0x005500C6, 0x005500DA,
        0x005501A2, 0x005501B6,
    };
    unsigned i;
    for (i = 0; i < sizeof(Addresses) / sizeof(Addresses[0]); ++i)
        WriteMemUInt8(Addresses[i] + 1, (uint8_t)FilterType);
}

extern "C" void GrSetViewMatrix_FovFix(float fFovScale, float fWFarFactor)
{ 
    constexpr float fRefAspectRatio = 4.0f / 3.0f;
    constexpr float fMaxWideAspectRatio = 21.0f / 9.0f; // biggest aspect ratio currently in market

    // g_GrScreen.fAspect == ScrW / ScrH * 0.75 (1.0 for 4:3 monitors, 1.2 for 16:10) - looks like Pixel Aspect Ratio
    // We use here MaxWidth and MaxHeight to calculate proper FOV for windowed mode

    float fViewportAspectRatio = (float)g_GrScreen.ViewportWidth / (float)g_GrScreen.ViewportHeight;
    float fAspectRatio = (float)g_GrScreen.MaxWidth / (float)g_GrScreen.MaxHeight;
    float fScaleX = 1.0f;
    float fScaleY = fRefAspectRatio * fViewportAspectRatio / fAspectRatio; // this is how RF does it and is needed for working scanner

    if (fAspectRatio <= fMaxWideAspectRatio) // never make X scale too high in windowed mode
        fScaleX *= fRefAspectRatio / fAspectRatio;
    else
    {
        fScaleX *= fRefAspectRatio / fMaxWideAspectRatio;
        fScaleY *= fAspectRatio / fMaxWideAspectRatio;
    }

    g_GrScaleVec.x = fWFarFactor / fFovScale * fScaleX;
    g_GrScaleVec.y = fWFarFactor / fFovScale * fScaleY;
    g_GrScaleVec.z = fWFarFactor;

    g_GrClippedGeomOffsetX = g_GrScreen.OffsetX - 0.5f;
    g_GrClippedGeomOffsetY = g_GrScreen.OffsetY - 0.5f;
    static auto &GrViewportCenterX = *(float*)0x01818B54;
    static auto &GrViewportCenterY = *(float*)0x01818B5C;
    GrViewportCenterX -= 0.5f; // viewport center x
    GrViewportCenterY -= 0.5f; // viewport center y
}

ASM_FUNC(GrSetViewMatrix_00547344,
    ASM_I  mov eax, [esp + 0x10 + 0x14]
    ASM_I  sub esp, 8
    ASM_I  fstp dword ptr [esp + 4]
    ASM_I  mov [esp + 0], eax
    ASM_I  call ASM_SYM(GrSetViewMatrix_FovFix)
    ASM_I  fld dword ptr [esp + 4]
    ASM_I  add esp, 8
    ASM_I  mov eax, 0x00547366
    ASM_I  jmp eax
)

extern "C" void DisplayD3DDeviceError(HRESULT hr)
{
    ERR("D3D CreateDevice failed (hr 0x%X - %s)", hr, getDxErrorStr(hr));

    char buf[1024];
    sprintf(buf, "Failed to create Direct3D device object - error 0x%X (%s).\n"
        "A critical error has occurred and the program cannot continue.\n"
        "Press OK to exit the program", hr, getDxErrorStr(hr));
    MessageBoxA(nullptr, buf, "Error!", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TASKMODAL);
    ExitProcess(-1);
}

ASM_FUNC(GrCreateD3DDeviceError_00545BEF,
    ASM_I  push eax
    ASM_I  call ASM_SYM(DisplayD3DDeviceError)
    ASM_I  add esp, 4
)

extern "C" void GrClearZBuffer_SetRect(D3DRECT *pClearRect)
{
    pClearRect->x1 = g_GrScreen.OffsetX + g_GrScreen.ClipLeft;
    pClearRect->y1 = g_GrScreen.OffsetY + g_GrScreen.ClipTop;
    pClearRect->x2 = g_GrScreen.OffsetX + g_GrScreen.ClipRight + 1;
    pClearRect->y2 = g_GrScreen.OffsetY + g_GrScreen.ClipBottom + 1;
}

ASM_FUNC(GrClearZBuffer_005509C4,
    ASM_I  lea eax, [esp + 0x14 - 0x10] // Rect
    ASM_I  push eax
    ASM_I  call ASM_SYM(GrClearZBuffer_SetRect)
    ASM_I  add esp, 4
    ASM_I  mov eax, 0x005509F0
    ASM_I  jmp eax
)

static void SetupPP(void)
{
    memset(&g_GrPP, 0, sizeof(g_GrPP));

    static auto &Format = *(D3DFORMAT*)0x005A135C;
    INFO("D3D Format: %ld", Format);

    // Note: in MSAA mode we don't lock back buffer
#if !D3D_SWAP_DISCARD
    g_pGrPP.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
#endif

#if MULTISAMPLING_SUPPORT
    if (g_gameConfig.msaa && Format > 0)
    {
        // Make sure selected MSAA mode is available
        HRESULT hr = (g_pDirect3D)->CheckDeviceMultiSampleType(g_AdapterIdx, D3DDEVTYPE_HAL, Format,
            g_gameConfig.wndMode != GameConfig::FULLSCREEN, (D3DMULTISAMPLE_TYPE)g_gameConfig.msaa);
        if (SUCCEEDED(hr))
        {
            INFO("Enabling Anti-Aliasing (%ux MSAA)...", g_gameConfig.msaa);
            g_GrPP.MultiSampleType = (D3DMULTISAMPLE_TYPE)g_gameConfig.msaa;
        }
        else
        {
            WARN("MSAA not supported (0x%x)...", hr);
            g_gameConfig.msaa = D3DMULTISAMPLE_NONE;
        }
    }
#endif

    // Make sure stretched window is always full screen
    if (g_gameConfig.wndMode == GameConfig::STRETCHED)
        SetWindowPos(g_hWnd, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_NOZORDER);
}

void GrDrawRect_GrDrawPolyHook(int Num, GrVertex **ppVertices, int Flags, int iMat)
{
    for (int i = 0; i < Num; ++i)
    {
        ppVertices[i]->vScreenPos.x -= 0.5f;
        ppVertices[i]->vScreenPos.y -= 0.5f;
    }
    GrDrawPoly(Num, ppVertices, Flags, iMat);
}

DWORD SetupMaxAnisotropy()
{
    DWORD AnisotropyLevel = std::min(g_GrDeviceCaps.MaxAnisotropy, 16ul);
    (g_pGrDevice)->SetTextureStageState(0, D3DTSS_MAXANISOTROPY, AnisotropyLevel);
    (g_pGrDevice)->SetTextureStageState(1, D3DTSS_MAXANISOTROPY, AnisotropyLevel);
    return AnisotropyLevel;
}

void GrInitBuffers_AfterReset_New()
{
    GrInitBuffers_AfterReset_Hook.callParent();

    // Apply state change after reset
    // Note: we dont have to set min/mag filtering because its set when selecting material

    (g_pGrDevice)->SetRenderState(D3DRS_CULLMODE, 1);
    (g_pGrDevice)->SetRenderState(D3DRS_SHADEMODE, 2);
    (g_pGrDevice)->SetRenderState(D3DRS_SPECULARENABLE, 0);
    (g_pGrDevice)->SetRenderState(D3DRS_AMBIENT, 0xFF545454);
    (g_pGrDevice)->SetRenderState(D3DRS_CLIPPING, 0);

    if (g_pLocalPlayer)
        GrSetTextureMipFilter((g_pLocalPlayer)->Config.FilteringLevel == 0);

    if (g_GrDeviceCaps.MaxAnisotropy > 0 && g_gameConfig.anisotropicFiltering)
        SetupMaxAnisotropy();
}

void GraphicsInit()
{
    // Fix for "At least 8 MB of available video memory"
    WriteMemUInt8(0x005460CD, ASM_JAE_SHORT);

#if WINDOWED_MODE_SUPPORT
    if (g_gameConfig.wndMode != GameConfig::FULLSCREEN)
    {
        /* Enable windowed mode */
        WriteMemUInt32(0x004B29A5 + 6, 0xC8);
        if (g_gameConfig.wndMode == GameConfig::STRETCHED)
        {
            uint32_t WndStyle = WS_POPUP | WS_SYSMENU;
            WriteMemUInt32(0x0050C474 + 1, WndStyle);
            WriteMemUInt32(0x0050C4E3 + 1, WndStyle);
        }
    }
#endif

    //WriteMemUInt8(0x00524C98, ASM_SHORT_JMP_REL); // disable window hooks

#if D3D_SWAP_DISCARD
    // Use Discard Swap Mode
    WriteMemUInt32(0x00545B4D + 6, D3DSWAPEFFECT_DISCARD); // full screen
    WriteMemUInt32(0x00545AE3 + 6, D3DSWAPEFFECT_DISCARD); // windowed
#endif

    // Replace memset call to SetupPP
    WriteMemUInt8(0x00545AA7, ASM_NOP, 0x00545AB5 - 0x00545AA7);
    WriteMemUInt8(0x00545AA7, ASM_LONG_CALL_REL);
    WriteMemInt32(0x00545AA7 + 1, (uintptr_t)SetupPP - (0x00545AA7 + 0x5));
    WriteMemUInt8(0x00545BA5, ASM_NOP, 6); // dont set PP.Flags

    // nVidia issue fix (make sure D3Dsc is enabled)
    WriteMemUInt8(0x00546154, 1);

    // Properly restore state after device reset
    GrInitBuffers_AfterReset_Hook.hook(0x00545045, GrInitBuffers_AfterReset_New);

#if WIDESCREEN_FIX
    // Fix FOV for widescreen
    WriteMemUInt8(0x00547344, ASM_LONG_JMP_REL);
    WriteMemInt32(0x00547344 + 1, (uintptr_t)GrSetViewMatrix_00547344 - (0x00547344 + 0x5));
    WriteMemFloat(0x0058A29C, 0.0003f); // factor related to near plane, default is 0.000588f
#endif

    // Don't use LOD models
    if (g_gameConfig.disableLodModels)
    {
        //WriteMemUInt8(0x00421A40, ASM_SHORT_JMP_REL);
        WriteMemUInt8(0x0052FACC, ASM_SHORT_JMP_REL);
    }

    // Better error message in case of device creation error
    WriteMemUInt8(0x00545BEF, ASM_LONG_JMP_REL);
    WriteMemInt32(0x00545BEF + 1, (uintptr_t)GrCreateD3DDeviceError_00545BEF - (0x00545BEF + 0x5));

    // Optimization - remove unused back buffer locking/unlocking in GrSwapBuffers
    AsmWritter(0x0054504A).jmpNear(0x0054508B);

#if 1
    // Fix rendering of right and bottom edges of viewport
    WriteMemUInt8(0x00431D9F, ASM_SHORT_JMP_REL);
    WriteMemUInt8(0x00431F6B, ASM_SHORT_JMP_REL);
    WriteMemUInt8(0x004328CF, ASM_SHORT_JMP_REL);
    WriteMemUInt8(0x0043298F, ASM_LONG_JMP_REL);
    WriteMemInt32(0x0043298F + 1, (uintptr_t)0x004329DC - (0x0043298F + 0x5));
    WriteMemUInt8(0x005509C4, ASM_LONG_JMP_REL);
    WriteMemInt32(0x005509C4 + 1, (uintptr_t)GrClearZBuffer_005509C4 - (0x005509C4 + 0x5));

    // Left and top viewport edge fix for MSAA (RF does similar thing in GrDrawTextureD3D)
    WriteMemUInt8(0x005478C6, ASM_FADD);
    WriteMemUInt8(0x005478D7, ASM_FADD);
    WriteMemPtr(0x005478C6 + 2, &g_GrClippedGeomOffsetX);
    WriteMemPtr(0x005478D7 + 2, &g_GrClippedGeomOffsetY);
    WriteMemInt32(0x0050DD69 + 1, (uintptr_t)GrDrawRect_GrDrawPolyHook - (0x0050DD69 + 0x5));
#endif

    if (g_gameConfig.highScannerRes)
    {
        // Improved Railgun Scanner resolution
        constexpr int8_t ScannerResolution = 120; // default is 64, max is 127 (signed byte)
        WriteMemUInt8(0x004325E6 + 1, ScannerResolution); // RenderInGame
        WriteMemUInt8(0x004325E8 + 1, ScannerResolution);
        WriteMemUInt8(0x004A34BB + 1, ScannerResolution); // PlayerCreate
        WriteMemUInt8(0x004A34BD + 1, ScannerResolution);
        WriteMemUInt8(0x004ADD70 + 1, ScannerResolution); // PlayerRenderRailgunScannerViewToTexture
        WriteMemUInt8(0x004ADD72 + 1, ScannerResolution);
        WriteMemUInt8(0x004AE0B7 + 1, ScannerResolution);
        WriteMemUInt8(0x004AE0B9 + 1, ScannerResolution);
        WriteMemUInt8(0x004AF0B0 + 1, ScannerResolution); // PlayerRenderScannerView
        WriteMemUInt8(0x004AF0B4 + 1, ScannerResolution * 3 / 4);
        WriteMemUInt8(0x004AF0B6 + 1, ScannerResolution);
        WriteMemUInt8(0x004AF7B0 + 1, ScannerResolution);
        WriteMemUInt8(0x004AF7B2 + 1, ScannerResolution);
        WriteMemUInt8(0x004AF7CF + 1, ScannerResolution);
        WriteMemUInt8(0x004AF7D1 + 1, ScannerResolution);
        WriteMemUInt8(0x004AF818 + 1, ScannerResolution);
        WriteMemUInt8(0x004AF81A + 1, ScannerResolution);
        WriteMemUInt8(0x004AF820 + 1, ScannerResolution);
        WriteMemUInt8(0x004AF822 + 1, ScannerResolution);
        WriteMemUInt8(0x004AF855 + 1, ScannerResolution);
        WriteMemUInt8(0x004AF860 + 1, ScannerResolution * 3 / 4);
        WriteMemUInt8(0x004AF862 + 1, ScannerResolution);
    }

    // Always transfer entire framebuffer to entire window in Present call
    WriteMemUInt8(0x00544FE6, ASM_SHORT_JMP_REL);

    // Init True Color improvements
    GrColorInit();

    // Enable mip-mapping for textures bigger than 256x256
    AsmWritter(0x0050FEDA, 0x0050FEE9).nop();
    WriteMemUInt8(0x0055B739, ASM_SHORT_JMP_REL);
}

void GraphicsAfterGameInit()
{
#if ANISOTROPIC_FILTERING
    // Anisotropic texture filtering
    if (g_GrDeviceCaps.MaxAnisotropy > 0 && g_gameConfig.anisotropicFiltering && !g_bDedicatedServer)
    {
        SetTextureMinMagFilterInCode(D3DTEXF_ANISOTROPIC);
        DWORD AnisotropyLevel = SetupMaxAnisotropy();
        INFO("Anisotropic Filtering enabled (level: %d)", AnisotropyLevel);
    }
#endif

    // Change font for Time Left text
    static int TimeLeftFont = GrLoadFont("rfpc-large.vf", -1);
    if (TimeLeftFont >= 0)
    {
        WriteMemInt8(0x00477157 + 1, TimeLeftFont);
        WriteMemInt8(0x0047715F + 2, 21);
        WriteMemInt32(0x00477168 + 1, 154);
    }
}

void GraphicsDrawFpsCounter()
{
    if (g_gameConfig.fpsCounter)
    {
        char szBuf[32];
        sprintf(szBuf, "FPS: %.1f", g_fFps);
        GrSetColor(0, 255, 0, 255);
        GrDrawAlignedText(GR_ALIGN_RIGHT, GrGetMaxWidth() - 10, 60, szBuf, -1, g_GrTextMaterial);
    }
}