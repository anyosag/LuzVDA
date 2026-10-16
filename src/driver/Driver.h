/*++

Copyright (c) Microsoft Corporation

Abstract:

	Types for the LuzVDA indirect display driver, based on Microsoft's IddSampleDriver.

Environment:

	User Mode, UMDF

--*/

#pragma once

#define NOMINMAX
#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <luzvda/luzvda-ioctl.h>

namespace Microsoft
{
	namespace WRL
	{
		namespace Wrappers
		{
			// Adds a wrapper for thread handles to the existing set of WRL handle wrapper classes
			typedef HandleT<HandleTraits::HANDLENullTraits> Thread;
		}
	}
}

namespace LuzVDA
{
	/// <summary>
	/// Manages the creation and lifetime of a Direct3D render device.
	/// </summary>
	struct Direct3DDevice
	{
		Direct3DDevice(LUID AdapterLuid);
		HRESULT Init();

		LUID AdapterLuid;
		Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
		Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
		Microsoft::WRL::ComPtr<ID3D11Device> Device;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
	};

	/// <summary>
	/// Manages a thread that consumes buffers from an indirect display swap-chain object.
	/// </summary>
	class SwapChainProcessor
	{
	public:
		SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent);
		~SwapChainProcessor();

	private:
		static DWORD CALLBACK RunThread(LPVOID Argument);

		void Run();
		void RunCore();

		IDDCX_SWAPCHAIN m_hSwapChain;
		std::shared_ptr<Direct3DDevice> m_Device;
		HANDLE m_hAvailableBufferEvent;
		Microsoft::WRL::Wrappers::Thread m_hThread;
		Microsoft::WRL::Wrappers::Event m_hTerminateEvent;
	};

	/// <summary>
	/// One virtual monitor: what the caller described, the EDID built from it, and its modes.
	/// Lives in the monitor registry from the add request until it is removed, and is deleted with the
	/// IddCx monitor object.
	/// </summary>
	class IndirectMonitorContext
	{
	public:
		IndirectMonitorContext(const LUZVDA_DISPLAY_DESC& Desc, uint32_t Serial, UINT Connector);
		~IndirectMonitorContext();

		void AssignSwapChain(IDDCX_MONITOR MonitorObject, IDDCX_SWAPCHAIN SwapChain, const LUID& RenderAdapter, HANDLE NewFrameEvent);
		void UnassignSwapChain();

		const LUZVDA_DISPLAY_DESC desc;
		const uint32_t serial;
		const UINT connector;
		std::array<uint8_t, 256> edid{};

		// The modes reported when the monitor arrived, and the target modes, which an update request
		// can replace. Windows offers modes that are in both. Guarded by the registry lock.
		std::vector<LUZVDA_MODE> monitorModes;
		size_t monitorPreferred = 0;
		std::vector<LUZVDA_MODE> targetModes;

		// Set once IddCx has the monitor; guarded by the registry lock
		IDDCX_MONITOR monitor = nullptr;
		LUID adapterLuid{};
		UINT targetId = 0;
		bool arrived = false;
		bool registered = false;

	private:
		std::unique_ptr<SwapChainProcessor> m_ProcessingThread;
		Microsoft::WRL::Wrappers::Event m_CursorEvent;
	};

	/// <summary>
	/// The root device and its IddCx adapter.
	/// </summary>
	class IndirectDeviceContext
	{
	public:
		IndirectDeviceContext(_In_ WDFDEVICE WdfDevice);
		virtual ~IndirectDeviceContext();

		void InitAdapter();
		IDDCX_ADAPTER Adapter() const { return m_Adapter; }

		NTSTATUS AddDisplay(const LUZVDA_DISPLAY_DESC& Desc, LUZVDA_ADD_OUT& Out);
		NTSTATUS RemoveDisplay(const LUZVDA_GUID& Guid);
		NTSTATUS UpdateModes(const LUZVDA_UPDATE_MODES& Update);
		NTSTATUS SetRenderAdapter(const LUID& AdapterLuid);

	protected:
		WDFDEVICE m_WdfDevice;
		IDDCX_ADAPTER m_Adapter = nullptr;
	};
}
