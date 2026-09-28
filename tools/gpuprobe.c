/* gpuprobe.c - what the real graphics card in this machine actually reports.
 *
 * Everything the operating system's graphics driver is checked against is
 * either a model of a card or, under VMware, a virtual adapter that forwards
 * to the host.  Neither is the card on the user's desk, and the difference
 * matters most for exactly the numbers a system monitor shows: a virtual
 * adapter reports its own invented memory size, its own driver version, and no
 * per-engine utilisation at all.
 *
 * So this asks Windows, on the real machine, about the real card - through the
 * same interfaces Task Manager uses - and prints what a correct driver would
 * have to agree with.  It is ground truth to check the operating system's own
 * reporting against, gathered on the hardware rather than assumed.
 *
 *   cl gpuprobe.c /link dxgi.lib d3d12.lib pdh.lib
 *   clang -o gpuprobe.exe gpuprobe.c -ldxgi -ld3d12 -lpdh
 */
#define COBJMACROS
#define INITGUID
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static void line(void) { printf("  ----------------------------------------------------------\n"); }

/* Bytes, the way somebody reading a system monitor expects to see them. */
static void human(char *out, size_t cap, unsigned long long bytes) {
    static const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    int u = 0;
    unsigned long long whole = bytes, frac = 0;
    while (whole >= 1024 && u < 4) {
        frac = ((whole % 1024) * 10) / 1024;
        whole /= 1024;
        u++;
    }
    snprintf(out, cap, "%llu.%llu %s", whole, frac, unit[u]);
}

/* ------------------------------------------------------------------ engines
 *
 * Per-engine utilisation is not a DXGI or D3D12 idea: Windows publishes it as
 * performance counters, one instance per engine per process, and Task Manager
 * sums them.  "GPU Engine(*)\\Utilization Percentage" is the same source, so
 * the numbers here are the ones the user sees in Task Manager - which is what
 * makes them useful as something to check against.
 *
 * The instance names carry the engine type, e.g.
 *   pid_1234_luid_0x00000000_0x0000C3A1_phys_0_eng_0_engtype_3D
 */
static void engines(void) {
    PDH_HQUERY q = NULL;
    PDH_HCOUNTER c = NULL;

    if (PdhOpenQueryW(NULL, 0, &q) != ERROR_SUCCESS) {
        printf("  (performance counters unavailable)\n");
        return;
    }
    if (PdhAddEnglishCounterW(q, L"\\GPU Engine(*)\\Utilization Percentage",
                              0, &c) != ERROR_SUCCESS) {
        printf("  (no GPU Engine counters - the display driver does not publish them)\n");
        PdhCloseQuery(q);
        return;
    }

    /* Two samples: a rate counter has no value until it has been read twice. */
    PdhCollectQueryData(q);
    Sleep(1000);
    PdhCollectQueryData(q);

    DWORD size = 0, count = 0;
    PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &count, NULL);
    if (!size) { printf("  (no engine instances)\n"); PdhCloseQuery(q); return; }

    PDH_FMT_COUNTERVALUE_ITEM_W *items = malloc(size);
    if (!items) { PdhCloseQuery(q); return; }

    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &count, items)
        == ERROR_SUCCESS) {
        /* Sum per engine type, the way Task Manager's rows are built. */
        struct { const wchar_t *tag; const char *name; double total; } kind[] = {
            { L"engtype_3D",             "3D",             0 },
            { L"engtype_Copy",           "Copy",           0 },
            { L"engtype_VideoDecode",    "Video decode",   0 },
            { L"engtype_VideoEncode",    "Video encode",   0 },
            { L"engtype_VideoProcessing","Video processing",0 },
            { L"engtype_Compute",        "Compute",        0 },
        };
        int kinds = (int)(sizeof kind / sizeof kind[0]);

        for (DWORD i = 0; i < count; i++) {
            if (!items[i].szName) continue;
            for (int k = 0; k < kinds; k++)
                if (wcsstr(items[i].szName, kind[k].tag))
                    kind[k].total += items[i].FmtValue.doubleValue;
        }

        for (int k = 0; k < kinds; k++)
            printf("    %-18s %6.1f %%\n", kind[k].name, kind[k].total);
    }
    free(items);
    PdhCloseQuery(q);
}

/* -------------------------------------------------------------- one adapter */

static void report(IDXGIAdapter1 *a1, int index) {
    DXGI_ADAPTER_DESC1 d;
    if (FAILED(IDXGIAdapter1_GetDesc1(a1, &d))) return;

    /* Software adapters are not what this is for, and reporting one as the
     * machine's card is exactly the confusion this tool exists to avoid. */
    int software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

    char vram[32], shared[32];
    human(vram, sizeof vram, d.DedicatedVideoMemory);
    human(shared, sizeof shared, d.SharedSystemMemory);

    printf("\nadapter %d: %ls%s\n", index, d.Description, software ? "  (software)" : "");
    line();
    printf("    PCI               %04x:%04x  subsys %08x  rev %u\n",
           d.VendorId, d.DeviceId, d.SubSysId, d.Revision);
    printf("    Dedicated memory  %s\n", vram);
    printf("    Shared memory     %s\n", shared);

    /* The driver's own version, which DXGI exposes as the user-mode driver
     * version rather than as a string. */
    LARGE_INTEGER umd;
    if (SUCCEEDED(IDXGIAdapter1_CheckInterfaceSupport(a1, &IID_IDXGIDevice, &umd))) {
        printf("    Driver version    %u.%u.%u.%u\n",
               (unsigned)((umd.QuadPart >> 48) & 0xFFFF),
               (unsigned)((umd.QuadPart >> 32) & 0xFFFF),
               (unsigned)((umd.QuadPart >> 16) & 0xFFFF),
               (unsigned)(umd.QuadPart & 0xFFFF));
    }

    /* How much of the card's memory is in use right now, which is the figure a
     * system monitor shows and the one a driver cannot work out for itself
     * without knowing what the firmware reserved. */
    IDXGIAdapter3 *a3 = NULL;
    if (SUCCEEDED(IDXGIAdapter1_QueryInterface(a1, &IID_IDXGIAdapter3, (void **)&a3))) {
        DXGI_QUERY_VIDEO_MEMORY_INFO vm;
        if (SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(
                a3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vm))) {
            char used[32], budget[32];
            human(used, sizeof used, vm.CurrentUsage);
            human(budget, sizeof budget, vm.Budget);
            printf("    Memory in use     %s of %s available to this process\n",
                   used, budget);
        }
        IDXGIAdapter3_Release(a3);
    }

    if (software) return;

    /* The highest Direct3D 12 feature level the card actually accepts.  This
     * is the number that says what a driver has to implement to drive it, and
     * it is asked by trying rather than by looking up a table. */
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };
    static const char *names[] = { "12_2", "12_1", "12_0", "11_1", "11_0" };

    ID3D12Device *dev = NULL;
    const char *best = NULL;
    for (int i = 0; i < (int)(sizeof levels / sizeof levels[0]); i++) {
        if (SUCCEEDED(D3D12CreateDevice((IUnknown *)a1, levels[i],
                                        &IID_ID3D12Device, (void **)&dev))) {
            best = names[i];
            break;
        }
    }

    if (!best) {
        printf("    Direct3D 12       not supported\n");
        return;
    }
    printf("    Direct3D 12       feature level %s\n", best);

    /* What that level brings with it, asked of the device rather than assumed
     * from the level, because a card may support a feature above its level. */
    D3D12_FEATURE_DATA_D3D12_OPTIONS o;
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS,
                                                   &o, sizeof o))) {
        printf("    Resource binding  tier %u\n", (unsigned)o.ResourceBindingTier);
        printf("    Tiled resources   tier %u\n", (unsigned)o.TiledResourcesTier);
        printf("    Conservative rast tier %u\n", (unsigned)o.ConservativeRasterizationTier);
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5;
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS5,
                                                   &o5, sizeof o5))) {
        printf("    Raytracing        tier %u\n", (unsigned)o5.RaytracingTier);
    }

    D3D12_FEATURE_DATA_SHADER_MODEL sm;
    sm.HighestShaderModel = D3D_SHADER_MODEL_6_7;
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL,
                                                   &sm, sizeof sm))) {
        printf("    Shader model      %u.%u\n",
               (unsigned)(sm.HighestShaderModel >> 4),
               (unsigned)(sm.HighestShaderModel & 0xF));
    }

    /* The command queues the card can be given work on.  These are the engines
     * a system monitor shows a row for, and asking the device which it accepts
     * is how to find out that a card has a copy engine and a video engine
     * rather than only a drawing one. */
    static const struct { D3D12_COMMAND_LIST_TYPE type; const char *name; } queues[] = {
        { D3D12_COMMAND_LIST_TYPE_DIRECT,        "3D / direct" },
        { D3D12_COMMAND_LIST_TYPE_COMPUTE,       "Compute" },
        { D3D12_COMMAND_LIST_TYPE_COPY,          "Copy" },
        { D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE,  "Video decode" },
        { D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE,  "Video encode" },
        { D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS, "Video process" },
    };

    printf("    Engines the card accepts work on:\n");
    for (int i = 0; i < (int)(sizeof queues / sizeof queues[0]); i++) {
        D3D12_COMMAND_QUEUE_DESC qd;
        memset(&qd, 0, sizeof qd);
        qd.Type = queues[i].type;

        ID3D12CommandQueue *q = NULL;
        HRESULT hr = ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue,
                                                     (void **)&q);
        printf("      %-16s %s\n", queues[i].name, SUCCEEDED(hr) ? "yes" : "no");
        if (q) ID3D12CommandQueue_Release(q);
    }

    ID3D12Device_Release(dev);
}


/* ---------------------------------------------------------------- NVML ----
 *
 * DXGI knows what the card IS.  It does not know what the card is DOING: no
 * temperature, no clocks, no fan, no power, and no encoder or decoder load.
 * Those come from NVML, the library nvidia-smi is built on, which ships with
 * every NVIDIA driver as nvml.dll.
 *
 * It is loaded by name at run time instead of being linked, for a reason that
 * matters to this tool's job: this is the program that establishes ground
 * truth, so it has to run and print what it can on a machine with no NVIDIA
 * driver at all rather than refusing to start.
 *
 * These are exactly the fields the operating system's own system monitor is
 * required to show, so anything printed here is something KestrelOS must
 * eventually agree with - measured on the card, not assumed about it.
 */
typedef struct { unsigned long long total, free, used; } nvml_memory_t;
typedef struct { unsigned int gpu, memory; } nvml_util_t;

typedef int (*fn_init)(void);
typedef int (*fn_shutdown)(void);
typedef int (*fn_count)(unsigned int *);
typedef int (*fn_handle)(unsigned int, void **);
typedef int (*fn_name)(void *, char *, unsigned int);
typedef int (*fn_temp)(void *, int, unsigned int *);
typedef int (*fn_util)(void *, nvml_util_t *);
typedef int (*fn_mem)(void *, nvml_memory_t *);
typedef int (*fn_uint)(void *, unsigned int *);
typedef int (*fn_clock)(void *, int, unsigned int *);
typedef int (*fn_codec)(void *, unsigned int *, unsigned int *);
typedef int (*fn_str)(char *, unsigned int);
typedef int (*fn_devstr)(void *, char *, unsigned int);

static void nvidia_detail(void) {
    HMODULE lib = LoadLibraryA("nvml.dll");
    if (!lib) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (n && n < MAX_PATH - 16) {
            strcat(path, "\\nvml.dll");
            lib = LoadLibraryA(path);
        }
    }
    if (!lib) {
        printf("\nNVML is not on this machine, so temperature, clocks, power\n"
               "and encoder load are unavailable here (no NVIDIA driver).\n");
        return;
    }

    fn_init      init     = (fn_init)     (void *)GetProcAddress(lib, "nvmlInit_v2");
    fn_shutdown  shutdown = (fn_shutdown) (void *)GetProcAddress(lib, "nvmlShutdown");
    fn_count     count    = (fn_count)    (void *)GetProcAddress(lib, "nvmlDeviceGetCount_v2");
    fn_handle    handle   = (fn_handle)   (void *)GetProcAddress(lib, "nvmlDeviceGetHandleByIndex_v2");
    fn_name      name     = (fn_name)     (void *)GetProcAddress(lib, "nvmlDeviceGetName");
    fn_temp      temp     = (fn_temp)     (void *)GetProcAddress(lib, "nvmlDeviceGetTemperature");
    fn_util      util     = (fn_util)     (void *)GetProcAddress(lib, "nvmlDeviceGetUtilizationRates");
    fn_mem       mem      = (fn_mem)      (void *)GetProcAddress(lib, "nvmlDeviceGetMemoryInfo");
    fn_uint      fan      = (fn_uint)     (void *)GetProcAddress(lib, "nvmlDeviceGetFanSpeed");
    fn_uint      power    = (fn_uint)     (void *)GetProcAddress(lib, "nvmlDeviceGetPowerUsage");
    fn_clock     clock    = (fn_clock)    (void *)GetProcAddress(lib, "nvmlDeviceGetClockInfo");
    fn_codec     encoder  = (fn_codec)    (void *)GetProcAddress(lib, "nvmlDeviceGetEncoderUtilization");
    fn_codec     decoder  = (fn_codec)    (void *)GetProcAddress(lib, "nvmlDeviceGetDecoderUtilization");
    fn_str       driver   = (fn_str)      (void *)GetProcAddress(lib, "nvmlSystemGetDriverVersion");
    fn_devstr    vbios    = (fn_devstr)   (void *)GetProcAddress(lib, "nvmlDeviceGetVbiosVersion");
    fn_uint      pciegen  = (fn_uint)     (void *)GetProcAddress(lib, "nvmlDeviceGetCurrPcieLinkGeneration");
    fn_uint      pciewid  = (fn_uint)     (void *)GetProcAddress(lib, "nvmlDeviceGetCurrPcieLinkWidth");

    if (!init || init() != 0) {
        printf("\nNVML is present but would not start.\n");
        FreeLibrary(lib);
        return;
    }

    unsigned int devices = 0;
    if (!count || count(&devices) != 0) devices = 0;

    printf("\nwhat the NVIDIA card reports about itself (NVML)\n");

    char buf[128];
    if (driver && driver(buf, sizeof buf) == 0)
        printf("  Driver version      %s\n", buf);

    for (unsigned int i = 0; i < devices; i++) {
        void *dev = NULL;
        if (!handle || handle(i, &dev) != 0) continue;

        printf("\n  device %u\n", i);
        line();

        if (name && name(dev, buf, sizeof buf) == 0)
            printf("    Name              %s\n", buf);
        if (vbios && vbios(dev, buf, sizeof buf) == 0)
            printf("    VBIOS             %s\n", buf);

        unsigned int v = 0, v2 = 0;

        /* The one the ask names first and DXGI cannot answer at all. */
        if (temp && temp(dev, 0 /* NVML_TEMPERATURE_GPU */, &v) == 0)
            printf("    Temperature       %u C\n", v);
        if (fan && fan(dev, &v) == 0)
            printf("    Fan               %u %%\n", v);
        if (power && power(dev, &v) == 0)
            printf("    Power draw        %u.%03u W\n", v / 1000, v % 1000);

        nvml_util_t u;
        if (util && util(dev, &u) == 0) {
            printf("    GPU utilisation   %u %%\n", u.gpu);
            printf("    Memory bandwidth  %u %% of the time busy\n", u.memory);
        }

        /* Encode and decode are reported separately from the 3D load, which is
         * why Task Manager shows them as their own graphs. */
        if (encoder && encoder(dev, &v, &v2) == 0)
            printf("    Video encode      %u %%\n", v);
        if (decoder && decoder(dev, &v, &v2) == 0)
            printf("    Video decode      %u %%\n", v);

        nvml_memory_t m;
        if (mem && mem(dev, &m) == 0) {
            char a[32], b[32], c[32];
            human(a, sizeof a, m.used);
            human(b, sizeof b, m.total);
            human(c, sizeof c, m.free);
            printf("    Video memory      %s used of %s  (%s free)\n", a, b, c);
        }

        /* 0 graphics, 1 SM, 2 memory, 3 video. */
        static const struct { int id; const char *what; } clocks[] = {
            { 0, "Graphics clock" }, { 2, "Memory clock" }, { 3, "Video clock" },
        };
        for (unsigned k = 0; k < sizeof clocks / sizeof clocks[0]; k++)
            if (clock && clock(dev, clocks[k].id, &v) == 0)
                printf("    %-17s %u MHz\n", clocks[k].what, v);

        if (pciegen && pciewid && pciegen(dev, &v) == 0 && pciewid(dev, &v2) == 0)
            printf("    PCIe link         gen %u x%u\n", v, v2);
    }

    if (shutdown) shutdown();
    FreeLibrary(lib);
}

int main(void) {
    printf("the graphics hardware in this machine, as Windows reports it\n");

    IDXGIFactory1 *f = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f))) {
        printf("could not open DXGI\n");
        return 1;
    }

    IDXGIAdapter1 *a = NULL;
    for (UINT i = 0; IDXGIFactory1_EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        report(a, (int)i);
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);

    nvidia_detail();

    printf("\nper-engine utilisation over one second (what Task Manager shows)\n");
    line();
    engines();

    return 0;
}
