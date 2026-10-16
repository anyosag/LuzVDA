/*++

Copyright (c) Microsoft Corporation

Abstract:

	This module contains a sample implementation of an indirect display driver. See the included README.md file and the
	various TODO blocks throughout this file and all accompanying files for information on building a production driver.

	MSDN documentation on indirect displays can be found at https://msdn.microsoft.com/en-us/library/windows/hardware/mt761968(v=vs.85).aspx.

	LuzVDA: virtual monitors created on request, each describing the screen it stands in for. Changed
	by SudoVDA's contributors and then for LuzVDA; see NOTICE.md.

Environment:

	User Mode, UMDF

--*/

#include "Driver.h"

#include <TraceLoggingProvider.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

#include "AdapterOption.h"
#include "common/validate.h"
#include "edid/edid_builder.h"

using namespace LuzVDA;
using namespace Microsoft::WRL;

// {fdbe20c6-12e0-4cf1-907d-e0b99b08b5ce}
TRACELOGGING_DEFINE_PROVIDER(
	g_Provider,
	"LuzVDA",
	(0xfdbe20c6, 0x12e0, 0x4cf1, 0x90, 0x7d, 0xe0, 0xb9, 0x9b, 0x08, 0xb5, 0xce));

#ifndef LUZVDA_VERSION_MAJOR
#define LUZVDA_VERSION_MAJOR 0
#define LUZVDA_VERSION_MINOR 0
#define LUZVDA_VERSION_PATCH 0
#define LUZVDA_VERSION_BUILD 0
#endif

#pragma region Settings

// Read from HKLM\SOFTWARE\LuzVDA when the driver loads
static DWORD g_MaxMonitors = 10;
static std::atomic<DWORD> g_WatchdogTimeout{3};
static IDDCX_BITS_PER_COMPONENT g_SdrBits = IDDCX_BITS_PER_COMPONENT_8;
static IDDCX_BITS_PER_COMPONENT g_HdrBits = IDDCX_BITS_PER_COMPONENT_10;
// An experiment: a name longer than 13 characters over two EDID name descriptors
static bool g_EdidSplitName = false;

// The GPU every monitor renders on, from the registry or a request
static std::mutex g_AdapterLock;
static LUID g_PreferredAdapter{};
static bool g_PreferredAdapterPending = false;
static int g_AbandonedSwapChains = 0;

// The IddCx adapter, once initialized
static std::atomic<IDDCX_ADAPTER> g_Adapter{nullptr};

static bool SameLuid(const LUID& a, const LUID& b)
{
	return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

static bool ReadDword(HKEY Key, const wchar_t* Name, DWORD& Value)
{
	DWORD size = sizeof(Value);
	return RegGetValueW(Key, nullptr, Name, RRF_RT_REG_DWORD, nullptr, &Value, &size) == ERROR_SUCCESS;
}

static void LoadSettings()
{
	HKEY key;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\LuzVDA", 0, KEY_READ, &key) != ERROR_SUCCESS)
	{
		return;
	}

	// RRF_RT_REG_SZ checks the type and guarantees the terminating NUL
	wchar_t gpuName[128];
	DWORD size = sizeof(gpuName);
	if (RegGetValueW(key, nullptr, L"gpuName", RRF_RT_REG_SZ, nullptr, gpuName, &size) == ERROR_SUCCESS)
	{
		AdapterOption adapterOpt;
		adapterOpt.selectGPU(gpuName);
		if (adapterOpt.hasTargetAdapter)
		{
			g_PreferredAdapter = adapterOpt.adapterLuid;
			g_PreferredAdapterPending = true;
		}
	}

	DWORD value;
	if (ReadDword(key, L"watchdog", value))
	{
		g_WatchdogTimeout = std::min<DWORD>(value, 60);
	}
	if (ReadDword(key, L"maxMonitors", value))
	{
		g_MaxMonitors = std::clamp<DWORD>(value, 1, LUZVDA_MAX_DISPLAYS);
	}
	if (ReadDword(key, L"sdrBits", value) && value == 10)
	{
		g_SdrBits = IDDCX_BITS_PER_COMPONENT_10;
	}
	if (ReadDword(key, L"hdrBits", value) && value == 12)
	{
		g_HdrBits = IDDCX_BITS_PER_COMPONENT_12;
	}
	if (ReadDword(key, L"edidSplitName", value))
	{
		g_EdidSplitName = value != 0;
	}

	RegCloseKey(key);
}

#pragma endregion

#pragma region Registry

// Every monitor from the add request until it is removed. IddCx calls back into the driver while
// creating a monitor, and those callbacks take this lock, so it is never held across an IddCx call.
struct MonitorRegistry
{
	std::mutex lock;
	std::vector<IndirectMonitorContext*> monitors;
	std::vector<bool> slotUsed;

	IndirectMonitorContext* FindByGuid(const LUZVDA_GUID& guid)
	{
		for (auto* m : monitors)
		{
			if (std::memcmp(m->desc.monitor_guid.bytes, guid.bytes, sizeof(guid.bytes)) == 0)
			{
				return m;
			}
		}
		return nullptr;
	}

	IndirectMonitorContext* FindBySerial(uint32_t serial)
	{
		for (auto* m : monitors)
		{
			if (m->serial == serial)
			{
				return m;
			}
		}
		return nullptr;
	}

	IndirectMonitorContext* FindByConnector(UINT connector)
	{
		for (auto* m : monitors)
		{
			if (m->connector == connector)
			{
				return m;
			}
		}
		return nullptr;
	}

	// Takes the monitor out, so nothing finds it any more; returns whether it was in
	bool Unregister(IndirectMonitorContext* m)
	{
		auto it = std::find(monitors.begin(), monitors.end(), m);
		if (it == monitors.end())
		{
			return false;
		}
		monitors.erase(it);
		m->registered = false;
		if (m->connector < slotUsed.size())
		{
			slotUsed[m->connector] = false;
		}
		return true;
	}
};

static MonitorRegistry g_Registry;

// Removes every monitor, for the watchdog and when the driver unloads
static void DisconnectAllMonitors()
{
	std::vector<IDDCX_MONITOR> departing;
	{
		std::lock_guard<std::mutex> lg(g_Registry.lock);
		for (auto* m : std::vector<IndirectMonitorContext*>(g_Registry.monitors))
		{
			if (m->arrived)
			{
				departing.push_back(m->monitor);
				g_Registry.Unregister(m);
			}
		}
	}

	for (auto monitor : departing)
	{
		IddCxMonitorDeparture(monitor);
	}

	if (!departing.empty())
	{
		TraceLoggingWrite(g_Provider, "AllDisplaysRemoved", TraceLoggingUInt32((UINT32)departing.size(), "Count"));
	}
}

#pragma endregion

#pragma region Watchdog

// Monitors are removed when no request has come for this many seconds, so a crashed caller doesn't
// leave them behind
static std::atomic<DWORD> g_WatchdogCountdown{0};
static std::atomic<bool> g_WatchdogStop{false};
static std::thread g_WatchdogThread;

static void RunWatchdog()
{
	if (!g_WatchdogTimeout)
	{
		return;
	}

	g_WatchdogCountdown = g_WatchdogTimeout.load();
	g_WatchdogThread = std::thread([] {
		while (!g_WatchdogStop)
		{
			Sleep(1000);

			bool empty;
			{
				std::lock_guard<std::mutex> lg(g_Registry.lock);
				empty = g_Registry.monitors.empty();
			}

			DWORD countdown = g_WatchdogCountdown;
			if (empty || countdown == 0)
			{
				continue;
			}

			if (g_WatchdogCountdown.compare_exchange_strong(countdown, countdown - 1) && countdown == 1)
			{
				TraceLoggingWrite(g_Provider, "WatchdogExpired");
				DisconnectAllMonitors();
			}
		}

		DisconnectAllMonitors();
	});
}

static void ResetWatchdog()
{
	g_WatchdogCountdown = g_WatchdogTimeout.load();
}

#pragma endregion

#pragma region helpers

static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, const LUZVDA_MODE& M, bool bMonitorMode)
{
	Mode.totalSize.cx = Mode.activeSize.cx = M.width;
	Mode.totalSize.cy = Mode.activeSize.cy = M.height;

	// See https://docs.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_video_signal_info
	Mode.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;
	Mode.AdditionalSignalInfo.videoStandard = 255;

	// Rates are in millihertz, which keeps 59.94 Hz exact
	Mode.vSyncFreq.Numerator = M.refresh_mhz;
	Mode.vSyncFreq.Denominator = 1000;
	Mode.hSyncFreq.Numerator = (UINT32)((UINT64)M.refresh_mhz * M.height);
	Mode.hSyncFreq.Denominator = 1000;

	Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

	Mode.pixelRate = (UINT64)M.refresh_mhz * M.width * M.height / 1000;
}

static IDDCX_MONITOR_MODE2 CreateIddCxMonitorMode2(const LUZVDA_MODE& M, IDDCX_MONITOR_MODE_ORIGIN Origin)
{
	IDDCX_MONITOR_MODE2 Mode = {};

	Mode.Size = sizeof(Mode);
	Mode.Origin = Origin;
	Mode.BitsPerComponent.Rgb = g_SdrBits | g_HdrBits;
	FillSignalInfo(Mode.MonitorVideoSignalInfo, M, true);

	return Mode;
}

static IDDCX_TARGET_MODE2 CreateIddCxTargetMode2(const LUZVDA_MODE& M)
{
	IDDCX_TARGET_MODE2 Mode = {};

	Mode.Size = sizeof(Mode);
	Mode.BitsPerComponent.Rgb = g_SdrBits | g_HdrBits;
	FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, M, false);

	return Mode;
}

static LUZVDA_LUID ToProtocol(const LUID& luid)
{
	return {luid.LowPart, luid.HighPart};
}

#pragma endregion

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_UNLOAD LuzVDADriverUnload;
EVT_WDF_DRIVER_DEVICE_ADD LuzVDADeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY LuzVDADeviceD0Entry;

EVT_IDD_CX_DEVICE_IO_CONTROL LuzVDAIoDeviceControl;
EVT_IDD_CX_ADAPTER_INIT_FINISHED LuzVDAAdapterInitFinished;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES LuzVDAMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN LuzVDAMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN LuzVDAMonitorUnassignSwapChain;

EVT_IDD_CX_ADAPTER_QUERY_TARGET_INFO LuzVDAAdapterQueryTargetInfo;
EVT_IDD_CX_MONITOR_SET_DEFAULT_HDR_METADATA LuzVDAMonitorSetDefaultHdrMetadata;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION2 LuzVDAParseMonitorDescription2;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES2 LuzVDAMonitorQueryModes2;
EVT_IDD_CX_ADAPTER_COMMIT_MODES2 LuzVDAAdapterCommitModes2;
EVT_IDD_CX_MONITOR_SET_GAMMA_RAMP LuzVDAMonitorSetGammaRamp;

struct IndirectDeviceContextWrapper
{
	IndirectDeviceContext* pContext;

	void Cleanup()
	{
		delete pContext;
		pContext = nullptr;
	}
};

struct IndirectMonitorContextWrapper
{
	IndirectMonitorContext* pContext;

	void Cleanup()
	{
		delete pContext;
		pContext = nullptr;
	}
};

// This macro creates the methods for accessing an IndirectDeviceContextWrapper as a context for a WDF object
WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);
WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContextWrapper);

extern "C" BOOL WINAPI DllMain(
	_In_ HINSTANCE hInstance,
	_In_ UINT dwReason,
	_In_opt_ LPVOID lpReserved)
{
	UNREFERENCED_PARAMETER(hInstance);
	UNREFERENCED_PARAMETER(lpReserved);
	UNREFERENCED_PARAMETER(dwReason);

	return TRUE;
}

static void SetHighPriority()
{
	// Kept from SudoVDA: the whole WUDFHost process, so frames are consumed promptly under load
	SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(
	PDRIVER_OBJECT  pDriverObject,
	PUNICODE_STRING pRegistryPath
)
{
	TraceLoggingRegister(g_Provider);

	LoadSettings();

	WDF_DRIVER_CONFIG Config;
	NTSTATUS Status;

	WDF_OBJECT_ATTRIBUTES Attributes;
	WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

	WDF_DRIVER_CONFIG_INIT(&Config,
		LuzVDADeviceAdd
	);

	Config.EvtDriverUnload = LuzVDADriverUnload;

	Status = WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
	if (!NT_SUCCESS(Status))
	{
		TraceLoggingWrite(g_Provider, "DriverCreateFailed", TraceLoggingHexInt32(Status, "Status"));
		TraceLoggingUnregister(g_Provider);
		return Status;
	}

	RunWatchdog();

	SetHighPriority();

	TraceLoggingWrite(g_Provider, "DriverStarted",
		TraceLoggingUInt32(g_WatchdogTimeout.load(), "Watchdog"),
		TraceLoggingUInt32(g_MaxMonitors, "MaxMonitors"));

	return Status;
}

_Use_decl_annotations_
void LuzVDADriverUnload(_In_ WDFDRIVER)
{
	if (g_WatchdogThread.joinable())
	{
		// The thread removes the monitors on its way out
		g_WatchdogStop = true;
		g_WatchdogThread.join();
	}
	else
	{
		DisconnectAllMonitors();
	}

	TraceLoggingUnregister(g_Provider);
}

_Use_decl_annotations_
NTSTATUS LuzVDADeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
	NTSTATUS Status = STATUS_SUCCESS;
	WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;

	UNREFERENCED_PARAMETER(Driver);

	// The callbacks HDR and exact refresh rates need came with IddCx 1.10 (Windows 11 24H2). The INF
	// only installs there, so this is a second line of defense.
	if (!IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo))
	{
		TraceLoggingWrite(g_Provider, "IddCxTooOld");
		return STATUS_NOT_SUPPORTED;
	}

	// Register for power callbacks - in this sample only power-on is needed
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
	PnpPowerCallbacks.EvtDeviceD0Entry = LuzVDADeviceD0Entry;
	WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

	IDD_CX_CLIENT_CONFIG IddConfig;
	IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);

	// If the driver wishes to handle custom IoDeviceControl requests, it's necessary to use this callback since IddCx
	// redirects IoDeviceControl requests to an internal queue.
	IddConfig.EvtIddCxDeviceIoControl = LuzVDAIoDeviceControl;

	IddConfig.EvtIddCxAdapterInitFinished = LuzVDAAdapterInitFinished;

	IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = LuzVDAMonitorGetDefaultModes;
	IddConfig.EvtIddCxMonitorAssignSwapChain = LuzVDAMonitorAssignSwapChain;
	IddConfig.EvtIddCxMonitorUnassignSwapChain = LuzVDAMonitorUnassignSwapChain;

	IddConfig.EvtIddCxAdapterQueryTargetInfo = LuzVDAAdapterQueryTargetInfo;
	IddConfig.EvtIddCxMonitorSetDefaultHdrMetaData = LuzVDAMonitorSetDefaultHdrMetadata;
	IddConfig.EvtIddCxParseMonitorDescription2 = LuzVDAParseMonitorDescription2;
	IddConfig.EvtIddCxMonitorQueryTargetModes2 = LuzVDAMonitorQueryModes2;
	IddConfig.EvtIddCxAdapterCommitModes2 = LuzVDAAdapterCommitModes2;
	IddConfig.EvtIddCxMonitorSetGammaRamp = LuzVDAMonitorSetGammaRamp;

	Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
	Attr.EvtCleanupCallback = [](WDFOBJECT Object)
	{
		// Automatically cleanup the context when the WDF object is about to be deleted
		auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
		if (pContext)
		{
			pContext->Cleanup();
		}
	};

	WDFDEVICE Device = nullptr;
	Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	static const GUID InterfaceGuid = LUZVDA_INTERFACE_GUID_INIT;
	Status = WdfDeviceCreateDeviceInterface(
		Device,
		&InterfaceGuid,
		NULL
	);

	if (!NT_SUCCESS(Status)) {
		return Status;
	}

	Status = IddCxDeviceInitialize(Device);
	if (!NT_SUCCESS(Status))
	{
		TraceLoggingWrite(g_Provider, "DeviceInitializeFailed", TraceLoggingHexInt32(Status, "Status"));
		return Status;
	}

	// Create a new device context object and attach it to the WDF device object
	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	pContext->pContext = new IndirectDeviceContext(Device);

	return Status;
}

_Use_decl_annotations_
NTSTATUS LuzVDADeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
	UNREFERENCED_PARAMETER(PreviousState);

	// This function is called by WDF to start the device in the fully-on power state.

	auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	pContext->pContext->InitAdapter();

	return STATUS_SUCCESS;
}

#pragma region Direct3DDevice

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{
}

HRESULT Direct3DDevice::Init()
{
	// The DXGI factory could be cached, but if a new render adapter appears on the system, a new factory needs to be
	// created. If caching is desired, check DxgiFactory->IsCurrent() each time and recreate the factory if !IsCurrent.
	HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
	if (FAILED(hr))
	{
		return hr;
	}

	// Find the specified render adapter
	hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
	if (FAILED(hr))
	{
		return hr;
	}

	// Create a D3D device using the render adapter. BGRA support is required by the WHQL test suite.
	const D3D_FEATURE_LEVEL FeatureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	hr = D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, FeatureLevels, ARRAYSIZE(FeatureLevels), D3D11_SDK_VERSION, &Device, nullptr, &DeviceContext);
	if (FAILED(hr))
	{
		// If creating the D3D device failed, it's possible the render GPU was lost (e.g. detachable GPU) or else the
		// system is in a transient state.
		return hr;
	}

	return S_OK;
}

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent)
	: m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent)
{
	m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));

	// Immediately create and run the swap-chain processing thread, passing 'this' as the thread parameter
	m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor()
{
	// Alert the swap-chain processing thread to terminate
	SetEvent(m_hTerminateEvent.Get());

	if (m_hThread.Get())
	{
		// Wait for the thread to terminate
		WaitForSingleObject(m_hThread.Get(), INFINITE);
	}
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
	reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
	return 0;
}

void SwapChainProcessor::Run()
{
	// For improved performance, make use of the Multimedia Class Scheduler Service, which will intelligently
	// prioritize this thread for improved throughput in high CPU-load scenarios.
	DWORD AvTask = 0;
	HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"DisplayPostProcessing", &AvTask);

	RunCore();

	// Always delete the swap-chain object when swap-chain processing loop terminates in order to kick the system to
	// provide a new swap-chain if necessary.
	WdfObjectDelete((WDFOBJECT)m_hSwapChain);
	m_hSwapChain = nullptr;

	if (AvTaskHandle)
	{
		AvRevertMmThreadCharacteristics(AvTaskHandle);
	}
}

void SwapChainProcessor::RunCore()
{
	// Get the DXGI device interface
	ComPtr<IDXGIDevice> DxgiDevice;
	HRESULT hr = m_Device->Device.As(&DxgiDevice);
	if (FAILED(hr))
	{
		return;
	}

	IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
	SetDevice.pDevice = DxgiDevice.Get();

	hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
	if (FAILED(hr))
	{
		return;
	}

	// Acquire and release buffers in a loop
	for (;;)
	{
		ComPtr<IDXGIResource> AcquiredBuffer;

		IDARG_IN_RELEASEANDACQUIREBUFFER2 BufferInArgs = {};
		BufferInArgs.Size = sizeof(BufferInArgs);
		IDARG_OUT_RELEASEANDACQUIREBUFFER2 Buffer = {};
		hr = IddCxSwapChainReleaseAndAcquireBuffer2(m_hSwapChain, &BufferInArgs, &Buffer);

		// AcquireBuffer immediately returns STATUS_PENDING if no buffer is yet available
		if (hr == E_PENDING)
		{
			// We must wait for a new buffer
			HANDLE WaitHandles [] =
			{
				m_hAvailableBufferEvent,
				m_hTerminateEvent.Get()
			};
			DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, 16);
			if (WaitResult == WAIT_OBJECT_0 || WaitResult == WAIT_TIMEOUT)
			{
				// We have a new buffer, so try the AcquireBuffer again
				continue;
			}
			else if (WaitResult == WAIT_OBJECT_0 + 1)
			{
				// We need to terminate
				break;
			}
			else
			{
				// The wait was cancelled or something unexpected happened
				hr = HRESULT_FROM_WIN32(WaitResult);
				break;
			}
		}
		else if (SUCCEEDED(hr))
		{
			// We have new frame to process, the surface has a reference on it that the driver has to release
			AcquiredBuffer.Attach(Buffer.MetaData.pSurface);

			// The frame isn't used: Apollo captures the display through Windows. Frames go straight to
			// Apollo only in a later version.

			// We have finished processing this frame hence we release the reference on it.
			// If the driver forgets to release the reference to the surface, it will be leaked which results in the
			// surfaces being left around after swapchain is destroyed.
			AcquiredBuffer.Reset();

			// Indicate to OS that we have finished inital processing of the frame, it is a hint that
			// OS could start preparing another frame
			hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
			if (FAILED(hr))
			{
				break;
			}
		}
		else
		{
			// The swap-chain was likely abandoned (e.g. DXGI_ERROR_ACCESS_LOST), so exit the processing loop
			break;
		}
	}
}

#pragma endregion

#pragma region IndirectDeviceContext

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) :
	m_WdfDevice(WdfDevice)
{
	// One root device per driver: start with every connector free
	std::lock_guard<std::mutex> lg(g_Registry.lock);
	g_Registry.slotUsed.assign(g_MaxMonitors, false);
}

IndirectDeviceContext::~IndirectDeviceContext()
{
}

void IndirectDeviceContext::InitAdapter()
{
	IDDCX_ADAPTER_CAPS AdapterCaps = {};
	AdapterCaps.Size = sizeof(AdapterCaps);

	// Frames can arrive in FP16 for HDR
	AdapterCaps.Flags = IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16;

	// Declare basic feature support for the adapter (required)
	AdapterCaps.MaxMonitorsSupported = g_MaxMonitors;
	AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
	AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
	AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

	// Declare your device strings for telemetry (required)
	AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"LuzVDA Virtual Display Adapter";
	AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"LuzVDA";
	AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"LuzVDA";

	// Declare your hardware and firmware versions (required)
	IDDCX_ENDPOINT_VERSION Version = {};
	Version.Size = sizeof(Version);
	Version.MajorVer = LUZVDA_PROTOCOL_MAJOR;
	Version.MinorVer = LUZVDA_PROTOCOL_MINOR;
	AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
	AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

	// Initialize a WDF context that can store a pointer to the device context object
	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

	IDARG_IN_ADAPTER_INIT AdapterInit = {};
	AdapterInit.WdfDevice = m_WdfDevice;
	AdapterInit.pCaps = &AdapterCaps;
	AdapterInit.ObjectAttributes = &Attr;

	// Start the initialization of the adapter, which will trigger the AdapterFinishInit callback later
	IDARG_OUT_ADAPTER_INIT AdapterInitOut;
	NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);

	if (NT_SUCCESS(Status))
	{
		// Store a reference to the WDF adapter handle
		m_Adapter = AdapterInitOut.AdapterObject;
		g_Adapter = m_Adapter;

		// Store the device context object into the WDF object context
		auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
		pContext->pContext = this;
	}
	else
	{
		TraceLoggingWrite(g_Provider, "AdapterInitFailed", TraceLoggingHexInt32(Status, "Status"));
	}
}

NTSTATUS IndirectDeviceContext::SetRenderAdapter(const LUID& AdapterLuid)
{
	if (!m_Adapter)
	{
		return STATUS_DEVICE_NOT_READY;
	}

	// Refuse a GPU that isn't there, which would have every swap-chain abandoned
	ComPtr<IDXGIFactory4> Factory;
	ComPtr<IDXGIAdapter1> Adapter;
	if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&Factory))) || FAILED(Factory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter))))
	{
		return STATUS_NOT_FOUND;
	}

	{
		std::lock_guard<std::mutex> lg(g_AdapterLock);
		g_PreferredAdapter = AdapterLuid;
		g_PreferredAdapterPending = true;
		g_AbandonedSwapChains = 0;
	}

	IDARG_IN_ADAPTERSETRENDERADAPTER inArgs{AdapterLuid};
	IddCxAdapterSetRenderAdapter(m_Adapter, &inArgs);

	TraceLoggingWrite(g_Provider, "RenderAdapterSet",
		TraceLoggingUInt32(AdapterLuid.LowPart, "LuidLow"),
		TraceLoggingInt32(AdapterLuid.HighPart, "LuidHigh"));
	return STATUS_SUCCESS;
}

NTSTATUS IndirectDeviceContext::AddDisplay(const LUZVDA_DISPLAY_DESC& Desc, LUZVDA_ADD_OUT& Out)
{
	if (!m_Adapter)
	{
		return STATUS_DEVICE_NOT_READY;
	}

	uint32_t serial = luzvda::edid::serial_for(Desc.monitor_guid.bytes);
	IndirectMonitorContext* pContext = nullptr;

	{
		std::lock_guard<std::mutex> lg(g_Registry.lock);

		// Adding a display that is already there returns it, so a caller can retry safely
		if (auto* existing = g_Registry.FindByGuid(Desc.monitor_guid))
		{
			if (!existing->arrived)
			{
				return STATUS_DEVICE_BUSY;
			}
			if (std::memcmp(&existing->desc, &Desc, sizeof(Desc)) != 0)
			{
				return STATUS_OBJECT_NAME_COLLISION;
			}
			Out = {ToProtocol(existing->adapterLuid), existing->targetId, 0};
			return STATUS_SUCCESS;
		}

		// Windows tells monitors apart by their EDID, whose serial comes from the GUID
		if (g_Registry.FindBySerial(serial))
		{
			return STATUS_DUPLICATE_OBJECTID;
		}

		// The lowest free connector, so a display returns to the same one
		UINT connector = 0;
		while (connector < g_Registry.slotUsed.size() && g_Registry.slotUsed[connector])
		{
			connector++;
		}
		if (connector >= g_Registry.slotUsed.size())
		{
			return STATUS_TOO_MANY_NODES;
		}

		pContext = new IndirectMonitorContext(Desc, serial, connector);
		g_Registry.slotUsed[connector] = true;
		g_Registry.monitors.push_back(pContext);
		pContext->registered = true;
	}

	// Without the lock from here: IddCx calls back into the driver while it creates the monitor
	WDF_OBJECT_ATTRIBUTES Attr;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectMonitorContextWrapper);
	Attr.EvtCleanupCallback = [](WDFOBJECT Object)
	{
		auto* pWrapper = WdfObjectGet_IndirectMonitorContextWrapper(Object);
		if (pWrapper)
		{
			pWrapper->Cleanup();
		}
	};

	IDDCX_MONITOR_INFO MonitorInfo = {};
	MonitorInfo.Size = sizeof(MonitorInfo);
	MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
	MonitorInfo.ConnectorIndex = pContext->connector;

	MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
	MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
	MonitorInfo.MonitorDescription.DataSize = (UINT)pContext->edid.size();
	MonitorInfo.MonitorDescription.pData = pContext->edid.data();
	std::memcpy(&MonitorInfo.MonitorContainerId, Desc.monitor_guid.bytes, sizeof(GUID));

	IDARG_IN_MONITORCREATE MonitorCreate = {};
	MonitorCreate.ObjectAttributes = &Attr;
	MonitorCreate.pMonitorInfo = &MonitorInfo;

	IDARG_OUT_MONITORCREATE MonitorCreateOut;
	NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
	if (!NT_SUCCESS(Status))
	{
		{
			std::lock_guard<std::mutex> lg(g_Registry.lock);
			g_Registry.Unregister(pContext);
		}
		delete pContext;
		TraceLoggingWrite(g_Provider, "MonitorCreateFailed", TraceLoggingHexInt32(Status, "Status"));
		return Status;
	}

	// From here the monitor object owns the context and deletes it
	auto* pWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorCreateOut.MonitorObject);
	pWrapper->pContext = pContext;

	{
		std::lock_guard<std::mutex> lg(g_Registry.lock);
		pContext->monitor = MonitorCreateOut.MonitorObject;
	}

	// Tell the OS that the monitor has been plugged in
	IDARG_OUT_MONITORARRIVAL ArrivalOut;
	Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);
	if (!NT_SUCCESS(Status))
	{
		// Don't leave a half-made monitor that a retry would mistake for a working one. Deleting the
		// object deletes its context, through the cleanup callback.
		{
			std::lock_guard<std::mutex> lg(g_Registry.lock);
			g_Registry.Unregister(pContext);
		}
		WdfObjectDelete(MonitorCreateOut.MonitorObject);
		TraceLoggingWrite(g_Provider, "MonitorArrivalFailed", TraceLoggingHexInt32(Status, "Status"));
		return Status;
	}

	std::lock_guard<std::mutex> lg(g_Registry.lock);

	pContext->adapterLuid = ArrivalOut.OsAdapterLuid;
	pContext->targetId = ArrivalOut.OsTargetId;
	pContext->arrived = true;
	Out = {ToProtocol(ArrivalOut.OsAdapterLuid), ArrivalOut.OsTargetId, 0};

	const auto& preferred = Desc.modes[Desc.preferred_mode];
	TraceLoggingWrite(g_Provider, "DisplayAdded",
		TraceLoggingUInt32(pContext->connector, "Connector"),
		TraceLoggingUInt32(ArrivalOut.OsTargetId, "TargetId"),
		TraceLoggingUInt32(preferred.width, "Width"),
		TraceLoggingUInt32(preferred.height, "Height"),
		TraceLoggingUInt32(preferred.refresh_mhz, "RefreshMilliHz"),
		TraceLoggingBool((Desc.flags & LUZVDA_DISPLAY_HDR) != 0, "Hdr"));
	return STATUS_SUCCESS;
}

NTSTATUS IndirectDeviceContext::RemoveDisplay(const LUZVDA_GUID& Guid)
{
	IDDCX_MONITOR monitor = nullptr;
	{
		std::lock_guard<std::mutex> lg(g_Registry.lock);
		auto* pContext = g_Registry.FindByGuid(Guid);
		if (!pContext || !pContext->arrived)
		{
			return STATUS_NOT_FOUND;
		}
		monitor = pContext->monitor;
		g_Registry.Unregister(pContext);
	}

	// Outside the lock: departure tears the monitor down, which may delete its context
	NTSTATUS Status = IddCxMonitorDeparture(monitor);
	TraceLoggingWrite(g_Provider, "DisplayRemoved", TraceLoggingHexInt32(Status, "Status"));
	return Status;
}

NTSTATUS IndirectDeviceContext::UpdateModes(const LUZVDA_UPDATE_MODES& Update)
{
	auto list = luzvda::mode_list(Update.mode_count, Update.preferred_mode, Update.modes);

	IDDCX_MONITOR monitor = nullptr;
	{
		std::lock_guard<std::mutex> lg(g_Registry.lock);
		auto* pContext = g_Registry.FindByGuid(Update.monitor_guid);
		if (!pContext || !pContext->arrived)
		{
			return STATUS_NOT_FOUND;
		}
		pContext->targetModes = list.modes;
		monitor = pContext->monitor;
	}

	std::vector<IDDCX_TARGET_MODE2> TargetModes;
	for (const auto& m : list.modes)
	{
		TargetModes.push_back(CreateIddCxTargetMode2(m));
	}

	IDARG_IN_UPDATEMODES2 inArgs = {};
	inArgs.Reason = IDDCX_UPDATE_REASON_OTHER;
	inArgs.TargetModeCount = (UINT)TargetModes.size();
	inArgs.pTargetModes = TargetModes.data();

	NTSTATUS Status = IddCxMonitorUpdateModes2(monitor, &inArgs);
	TraceLoggingWrite(g_Provider, "ModesUpdated",
		TraceLoggingUInt32((UINT32)TargetModes.size(), "Count"),
		TraceLoggingHexInt32(Status, "Status"));
	return Status;
}

IndirectMonitorContext::IndirectMonitorContext(const LUZVDA_DISPLAY_DESC& Desc, uint32_t Serial, UINT Connector) :
	desc(Desc), serial(Serial), connector(Connector)
{
	luzvda::edid::input_t in;
	in.serial = Serial;
	in.name = luzvda::ascii_name(Desc.name, sizeof(Desc.name));
	in.width_mm = Desc.width_mm;
	in.height_mm = Desc.height_mm;
	std::copy(std::begin(Desc.primaries), std::end(Desc.primaries), in.primaries.begin());
	in.hdr = (Desc.flags & LUZVDA_DISPLAY_HDR) != 0;
	in.max_nits = Desc.max_luminance;
	in.max_full_frame_nits = Desc.max_full_frame_luminance;
	in.min_luminance = Desc.min_luminance;
	in.preferred = Desc.modes[Desc.preferred_mode];
	in.split_name = g_EdidSplitName;
	edid = luzvda::edid::build(in);

	auto list = luzvda::mode_list(Desc.mode_count, Desc.preferred_mode, Desc.modes);
	monitorModes = list.modes;
	monitorPreferred = list.preferred;
	targetModes = list.modes;
}

IndirectMonitorContext::~IndirectMonitorContext()
{
	m_ProcessingThread.reset();

	// Torn down without a remove request, e.g. with the adapter
	std::lock_guard<std::mutex> lg(g_Registry.lock);
	if (registered)
	{
		g_Registry.Unregister(this);
	}
}

void IndirectMonitorContext::AssignSwapChain(IDDCX_MONITOR MonitorObject, IDDCX_SWAPCHAIN SwapChain, const LUID& RenderAdapter, HANDLE NewFrameEvent)
{
	m_ProcessingThread.reset();

	auto Device = std::make_shared<Direct3DDevice>(RenderAdapter);
	if (FAILED(Device->Init()))
	{
		// It's important to delete the swap-chain if D3D initialization fails, so that the OS knows to generate a new
		// swap-chain and try again.
		WdfObjectDelete(SwapChain);
		return;
	}

	// Create a new swap-chain processing thread
	m_ProcessingThread.reset(new SwapChainProcessor(SwapChain, Device, NewFrameEvent));

	// The cursor is drawn by the capture side, not composed into the frames. The event is required but
	// never waited on; one per monitor, unnamed, closed with the monitor (SudoVDA leaked a named one
	// on every assignment).
	if (!m_CursorEvent.IsValid())
	{
		m_CursorEvent.Attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
	}
	if (!m_CursorEvent.IsValid())
	{
		return;
	}

	IDDCX_CURSOR_CAPS cursorInfo = {};
	cursorInfo.Size = sizeof(cursorInfo);
	cursorInfo.ColorXorCursorSupport = IDDCX_XOR_CURSOR_SUPPORT_FULL;
	cursorInfo.AlphaCursorSupport = true;
	cursorInfo.MaxX = 64;
	cursorInfo.MaxY = 64;

	IDARG_IN_SETUP_HWCURSOR hwCursor = {};
	hwCursor.CursorInfo = cursorInfo;
	hwCursor.hNewCursorDataAvailable = m_CursorEvent.Get();

	NTSTATUS Status = IddCxMonitorSetupHardwareCursor(MonitorObject, &hwCursor);
	if (!NT_SUCCESS(Status))
	{
		TraceLoggingWrite(g_Provider, "HardwareCursorFailed", TraceLoggingHexInt32(Status, "Status"));
	}
}

void IndirectMonitorContext::UnassignSwapChain()
{
	// Stop processing the last swap-chain
	m_ProcessingThread.reset();
}

#pragma endregion

#pragma region DDI Callbacks

_Use_decl_annotations_
NTSTATUS LuzVDAAdapterInitFinished(IDDCX_ADAPTER AdapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
	if (NT_SUCCESS(pInArgs->AdapterInitStatus))
	{
		std::lock_guard<std::mutex> lg(g_AdapterLock);
		if (g_PreferredAdapterPending)
		{
			IDARG_IN_ADAPTERSETRENDERADAPTER inArgs{g_PreferredAdapter};
			IddCxAdapterSetRenderAdapter(AdapterObject, &inArgs);
		}
	}

	return pInArgs->AdapterInitStatus;
}

_Use_decl_annotations_
NTSTATUS LuzVDAAdapterCommitModes2(
	IDDCX_ADAPTER AdapterObject,
	const IDARG_IN_COMMITMODES2* pInArgs
)
{
	UNREFERENCED_PARAMETER(AdapterObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS LuzVDAParseMonitorDescription2(
	const IDARG_IN_PARSEMONITORDESCRIPTION2* pInArgs,
	IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs
)
{
	// The EDIDs are the driver's own, so rather than parse one it finds the monitor by the serial it
	// wrote and reports that monitor's modes
	if (pInArgs->MonitorDescription.DataSize != 256 || !pInArgs->MonitorDescription.pData)
	{
		return STATUS_INVALID_PARAMETER;
	}

	const auto* edid = static_cast<const uint8_t*>(pInArgs->MonitorDescription.pData);
	uint32_t serial = edid[12] | (edid[13] << 8) | (edid[14] << 16) | ((uint32_t)edid[15] << 24);

	std::lock_guard<std::mutex> lg(g_Registry.lock);
	auto* pContext = g_Registry.FindBySerial(serial);
	if (!pContext)
	{
		return STATUS_NOT_FOUND;
	}

	const auto& modes = pContext->monitorModes;
	pOutArgs->MonitorModeBufferOutputCount = (UINT)modes.size();

	if (pInArgs->MonitorModeBufferInputCount < modes.size())
	{
		// Return success if there was no buffer, since the caller was only asking for a count of modes
		return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
	}

	for (size_t i = 0; i < modes.size(); i++)
	{
		pInArgs->pMonitorModes[i] = CreateIddCxMonitorMode2(modes[i], IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
	}
	pOutArgs->PreferredMonitorModeIdx = (UINT)pContext->monitorPreferred;

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS LuzVDAMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs, IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
	// Only for monitors without an EDID, which this driver never creates
	UNREFERENCED_PARAMETER(MonitorObject);

	static const LUZVDA_MODE Fallback = {1920, 1080, 60000};

	pOutArgs->DefaultMonitorModeBufferOutputCount = 1;
	pOutArgs->PreferredMonitorModeIdx = 0;

	if (pInArgs->DefaultMonitorModeBufferInputCount == 0)
	{
		return STATUS_SUCCESS;
	}

	IDDCX_MONITOR_MODE Mode = {};
	Mode.Size = sizeof(Mode);
	Mode.Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER;
	FillSignalInfo(Mode.MonitorVideoSignalInfo, Fallback, true);
	pInArgs->pDefaultMonitorModes[0] = Mode;

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS LuzVDAMonitorQueryModes2(IDDCX_MONITOR MonitorObject, const IDARG_IN_QUERYTARGETMODES2* pInArgs, IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
	auto* pWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
	if (!pWrapper || !pWrapper->pContext)
	{
		return STATUS_INVALID_PARAMETER;
	}

	std::vector<LUZVDA_MODE> modes;
	{
		std::lock_guard<std::mutex> lg(g_Registry.lock);
		modes = pWrapper->pContext->targetModes;
	}

	// Create a set of modes supported for frame processing and scan-out. The OS will report the available set of
	// modes for a given output as the intersection of monitor modes with target modes.
	pOutArgs->TargetModeBufferOutputCount = (UINT)modes.size();

	if (pInArgs->TargetModeBufferInputCount >= modes.size())
	{
		for (size_t i = 0; i < modes.size(); i++)
		{
			pInArgs->pTargetModes[i] = CreateIddCxTargetMode2(modes[i]);
		}
	}
	else if (pInArgs->TargetModeBufferInputCount != 0)
	{
		return STATUS_BUFFER_TOO_SMALL;
	}

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS LuzVDAMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
	auto* pWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
	if (!pWrapper || !pWrapper->pContext)
	{
		return STATUS_INVALID_PARAMETER;
	}

	{
		std::lock_guard<std::mutex> lg(g_AdapterLock);
		if (g_PreferredAdapterPending && !SameLuid(pInArgs->RenderAdapterLuid, g_PreferredAdapter))
		{
			// Ask for the preferred GPU again, a few times; after that take what the OS gives, so a GPU
			// that won't take the display can't have every swap-chain abandoned forever
			IDDCX_ADAPTER adapter = g_Adapter;
			if (adapter && g_AbandonedSwapChains < 3)
			{
				g_AbandonedSwapChains++;
				IDARG_IN_ADAPTERSETRENDERADAPTER inArgs{g_PreferredAdapter};
				IddCxAdapterSetRenderAdapter(adapter, &inArgs);
				return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
			}
			TraceLoggingWrite(g_Provider, "RenderAdapterNotTaken",
				TraceLoggingUInt32(pInArgs->RenderAdapterLuid.LowPart, "LuidLow"),
				TraceLoggingInt32(pInArgs->RenderAdapterLuid.HighPart, "LuidHigh"));
		}
		g_PreferredAdapterPending = false;
		g_AbandonedSwapChains = 0;
	}

	pWrapper->pContext->AssignSwapChain(MonitorObject, pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS LuzVDAMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
	auto* pWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
	if (pWrapper && pWrapper->pContext)
	{
		pWrapper->pContext->UnassignSwapChain();
	}
	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS LuzVDAAdapterQueryTargetInfo(
	IDDCX_ADAPTER AdapterObject,
	IDARG_IN_QUERYTARGET_INFO* pInArgs,
	IDARG_OUT_QUERYTARGET_INFO* pOutArgs
)
{
	UNREFERENCED_PARAMETER(AdapterObject);

	// HDR only for monitors whose screen can show it; wide color for all
	bool hdr = true;
	{
		std::lock_guard<std::mutex> lg(g_Registry.lock);
		if (auto* pContext = g_Registry.FindByConnector(pInArgs->ConnectorIndex))
		{
			hdr = (pContext->desc.flags & LUZVDA_DISPLAY_HDR) != 0;
		}
	}

	pOutArgs->TargetCaps = IDDCX_TARGET_CAPS_WIDE_COLOR_SPACE;
	if (hdr)
	{
		pOutArgs->TargetCaps |= IDDCX_TARGET_CAPS_HIGH_COLOR_SPACE;
	}
	pOutArgs->DitheringSupport.Rgb = g_HdrBits;

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS LuzVDAMonitorSetDefaultHdrMetadata(
	IDDCX_MONITOR MonitorObject,
	const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA* pInArgs
)
{
	UNREFERENCED_PARAMETER(MonitorObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS LuzVDAMonitorSetGammaRamp(
	IDDCX_MONITOR MonitorObject,
	const IDARG_IN_SET_GAMMARAMP* pInArgs
)
{
	UNREFERENCED_PARAMETER(MonitorObject);
	UNREFERENCED_PARAMETER(pInArgs);

	return STATUS_SUCCESS;
}

#pragma endregion

#pragma region Requests

// The request's input, copied out before anything is written: METHOD_BUFFERED gives input and output
// the same buffer
template <typename T>
static NTSTATUS ReadInput(WDFREQUEST Request, size_t InputBufferLength, T& Value)
{
	if (InputBufferLength < sizeof(T))
	{
		return STATUS_BUFFER_TOO_SMALL;
	}

	void* buffer = nullptr;
	NTSTATUS Status = WdfRequestRetrieveInputBuffer(Request, sizeof(T), &buffer, nullptr);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	std::memcpy(&Value, buffer, sizeof(T));
	return STATUS_SUCCESS;
}

template <typename T>
static NTSTATUS WriteOutput(WDFREQUEST Request, const T& Value, size_t& BytesReturned)
{
	void* buffer = nullptr;
	NTSTATUS Status = WdfRequestRetrieveOutputBuffer(Request, sizeof(T), &buffer, nullptr);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	std::memcpy(buffer, &Value, sizeof(T));
	BytesReturned = sizeof(T);
	return STATUS_SUCCESS;
}

static NTSTATUS StatusFor(luzvda::check_e Check)
{
	switch (Check)
	{
	case luzvda::check_e::ok:
		return STATUS_SUCCESS;
	case luzvda::check_e::too_small:
		return STATUS_BUFFER_TOO_SMALL;
	case luzvda::check_e::bad_struct_size:
		return STATUS_REVISION_MISMATCH;
	default:
		return STATUS_INVALID_PARAMETER;
	}
}

static void LogRejected(ULONG IoControlCode, luzvda::check_e Check)
{
	TraceLoggingWrite(g_Provider, "RequestRejected",
		TraceLoggingHexUInt32(IoControlCode, "Code"),
		TraceLoggingString(luzvda::describe(Check), "Reason"));
}

_Use_decl_annotations_
VOID LuzVDAIoDeviceControl(
	WDFDEVICE Device,
	WDFREQUEST Request,
	size_t OutputBufferLength,
	size_t InputBufferLength,
	ULONG IoControlCode
)
{
	UNREFERENCED_PARAMETER(OutputBufferLength);

	NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
	size_t bytesReturned = 0;

	auto* pDeviceWrapper = WdfObjectGet_IndirectDeviceContextWrapper(Device);
	auto* pDevice = pDeviceWrapper ? pDeviceWrapper->pContext : nullptr;

	switch (IoControlCode)
	{
	case IOCTL_LUZVDA_GET_VERSION: {
		LUZVDA_VERSION version = {
			LUZVDA_PROTOCOL_MAJOR,
			LUZVDA_PROTOCOL_MINOR,
			{LUZVDA_VERSION_MAJOR, LUZVDA_VERSION_MINOR, LUZVDA_VERSION_PATCH, LUZVDA_VERSION_BUILD},
		};
		Status = WriteOutput(Request, version, bytesReturned);
		break;
	}
	case IOCTL_LUZVDA_PING: {
		ResetWatchdog();
		Status = STATUS_SUCCESS;
		break;
	}
	case IOCTL_LUZVDA_GET_WATCHDOG: {
		LUZVDA_WATCHDOG watchdog = {g_WatchdogTimeout.load(), g_WatchdogCountdown.load()};
		Status = WriteOutput(Request, watchdog, bytesReturned);
		break;
	}
	case IOCTL_LUZVDA_ADD_DISPLAY: {
		ResetWatchdog();

		LUZVDA_DISPLAY_DESC desc;
		Status = ReadInput(Request, InputBufferLength, desc);
		if (!NT_SUCCESS(Status))
		{
			break;
		}

		auto check = luzvda::check(desc, InputBufferLength);
		if (check != luzvda::check_e::ok)
		{
			LogRejected(IoControlCode, check);
			Status = StatusFor(check);
			break;
		}

		if (!pDevice)
		{
			Status = STATUS_DEVICE_NOT_READY;
			break;
		}

		LUZVDA_ADD_OUT out = {};
		Status = pDevice->AddDisplay(desc, out);
		if (NT_SUCCESS(Status))
		{
			Status = WriteOutput(Request, out, bytesReturned);
		}
		break;
	}
	case IOCTL_LUZVDA_REMOVE_DISPLAY: {
		ResetWatchdog();

		LUZVDA_GUID guid;
		Status = ReadInput(Request, InputBufferLength, guid);
		if (NT_SUCCESS(Status))
		{
			Status = pDevice ? pDevice->RemoveDisplay(guid) : STATUS_DEVICE_NOT_READY;
		}
		break;
	}
	case IOCTL_LUZVDA_UPDATE_MODES: {
		ResetWatchdog();

		LUZVDA_UPDATE_MODES update;
		Status = ReadInput(Request, InputBufferLength, update);
		if (!NT_SUCCESS(Status))
		{
			break;
		}

		auto check = luzvda::check(update, InputBufferLength);
		if (check != luzvda::check_e::ok)
		{
			LogRejected(IoControlCode, check);
			Status = StatusFor(check);
			break;
		}

		Status = pDevice ? pDevice->UpdateModes(update) : STATUS_DEVICE_NOT_READY;
		break;
	}
	case IOCTL_LUZVDA_SET_RENDER_ADAPTER: {
		ResetWatchdog();

		LUZVDA_LUID luid;
		Status = ReadInput(Request, InputBufferLength, luid);
		if (NT_SUCCESS(Status))
		{
			LUID adapter = {luid.low, luid.high};
			Status = pDevice ? pDevice->SetRenderAdapter(adapter) : STATUS_DEVICE_NOT_READY;
		}
		break;
	}
	case IOCTL_LUZVDA_LIST_DISPLAYS: {
		LUZVDA_LIST list = {};
		{
			std::lock_guard<std::mutex> lg(g_Registry.lock);
			for (auto* m : g_Registry.monitors)
			{
				if (!m->arrived || list.count >= LUZVDA_MAX_DISPLAYS)
				{
					continue;
				}
				auto& entry = list.displays[list.count++];
				entry.monitor_guid = m->desc.monitor_guid;
				entry.adapter = ToProtocol(m->adapterLuid);
				entry.target_id = m->targetId;
				entry.connector = m->connector;
				entry.preferred_mode = m->monitorModes[m->monitorPreferred];
			}
		}
		Status = WriteOutput(Request, list, bytesReturned);
		break;
	}
	default:
		break;
	}

	WdfRequestCompleteWithInformation(Request, Status, bytesReturned);
}

#pragma endregion
