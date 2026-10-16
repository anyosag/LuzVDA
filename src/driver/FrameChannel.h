/*++

Abstract:

	A display's frame channel (protocol 1.1): shared textures the swap-chain thread copies each frame
	into, and the shared header that describes them. Uses only Win32 and Direct3D 11, not IddCx, so
	it builds and is checked outside the driver too.

Environment:

	User Mode, UMDF

	SPDX-License-Identifier: MS-PL

--*/

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <string>

#include <luzvda/luzvda-ioctl.h>

namespace LuzVDA
{
	class FrameChannel
	{
	public:
		// What the last Publish did to the textures, for the driver to trace
		struct Change
		{
			enum Kind
			{
				None,
				Made,
				Failed,
			};

			Kind kind = None;
			HRESULT hr = S_OK;
			uint32_t generation = 0;
			uint32_t width = 0;
			uint32_t height = 0;
			uint32_t format = 0;
			uint32_t names = 0;
		};

		explicit FrameChannel(const LUZVDA_GUID& Guid);
		~FrameChannel();

		FrameChannel(const FrameChannel&) = delete;
		FrameChannel& operator=(const FrameChannel&) = delete;

		// Makes the header and the ready event on first use, and counts the caller as an active
		// consumer
		HRESULT Open(LUZVDA_FRAME_CHANNEL& Out);

		// From the swap-chain thread, for each frame Windows gives the driver
		void Publish(ID3D11Device* Device, ID3D11DeviceContext* Context, const LUID& Adapter, IDXGIResource* Surface, int64_t AcquiredQpc);

		// When the swap chain whose device is Device ends: its textures go with it
		void DropDevice(ID3D11Device* Device);

		Change TakeChange();

	private:
		struct Slot
		{
			Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
			Microsoft::WRL::ComPtr<IDXGIKeyedMutex> mutex;
			HANDLE handle = nullptr;
		};

		HRESULT MakeRing(ID3D11Device* Device, const LUID& Adapter, const D3D11_TEXTURE2D_DESC& Frame);
		void DropRing();

		std::mutex m_Lock;
		std::string m_Prefix;
		PSECURITY_DESCRIPTOR m_Security = nullptr;
		SECURITY_ATTRIBUTES m_Attributes{};
		HANDLE m_Mapping = nullptr;
		HANDLE m_Ready = nullptr;
		LUZVDA_FRAME_HEADER* m_Header = nullptr;
		uint32_t m_ReadyNamed = 0;

		std::array<Slot, LUZVDA_FRAME_SLOTS> m_Slots;
		ID3D11Device* m_RingDevice = nullptr;
		D3D11_TEXTURE2D_DESC m_RingDesc{};
		bool m_RingBroken = false;
		uint32_t m_Generation = 0;
		uint64_t m_FrameNumber = 0;

		Change m_Change;
	};
}
