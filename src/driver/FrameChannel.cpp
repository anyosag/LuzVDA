/*++

Abstract:

	A display's frame channel; see FrameChannel.h and the protocol header.

Environment:

	User Mode, UMDF

	SPDX-License-Identifier: MS-PL

--*/

#include "FrameChannel.h"

#include <sddl.h>

#include <algorithm>
#include <cstring>

#define LUZVDA_FRAMES_NO_CONSUMER
#include <luzvda/luzvda-frames.h>

using namespace LuzVDA;
using Microsoft::WRL::ComPtr;

namespace
{
	// Only SYSTEM (Apollo's service) and Administrators, as for the control device
	const wchar_t c_Sddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";

	// What IDXGIKeyedMutex::AcquireSync returns when the other device went away holding the mutex:
	// WAIT_ABANDONED, whose definition the driver build leaves out (WIN32_NO_STATUS)
	const HRESULT c_KeyedMutexAbandoned = 0x00000080L;

	std::wstring Widen(const std::string& Text)
	{
		return std::wstring(Text.begin(), Text.end());
	}

	int64_t Now()
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return now.QuadPart;
	}

	uint64_t Read64(const uint64_t* Value)
	{
		return (uint64_t)InterlockedCompareExchange64((volatile LONG64*)Value, 0, 0);
	}

	void Write64(uint64_t* Target, uint64_t Value)
	{
		InterlockedExchange64((volatile LONG64*)Target, (LONG64)Value);
	}

	void Write32(uint32_t* Target, uint32_t Value)
	{
		InterlockedExchange((volatile LONG*)Target, (LONG)Value);
	}
}

FrameChannel::FrameChannel(const LUZVDA_GUID& Guid) : m_Prefix(luzvda::frames::prefix_for(Guid))
{
	if (ConvertStringSecurityDescriptorToSecurityDescriptorW(c_Sddl, SDDL_REVISION_1, &m_Security, nullptr))
	{
		m_Attributes.nLength = sizeof(m_Attributes);
		m_Attributes.lpSecurityDescriptor = m_Security;
		m_Attributes.bInheritHandle = FALSE;
	}
}

FrameChannel::~FrameChannel()
{
	std::lock_guard<std::mutex> lg(m_Lock);
	DropRing();

	if (m_Header)
	{
		// A consumer holding the objects learns the display is gone
		Write32(&m_Header->state, LUZVDA_FRAME_CLOSED);
		if (m_Ready)
		{
			SetEvent(m_Ready);
		}
		UnmapViewOfFile(m_Header);
		m_Header = nullptr;
	}
	if (m_Mapping)
	{
		CloseHandle(m_Mapping);
	}
	if (m_Ready)
	{
		CloseHandle(m_Ready);
	}
	if (m_Security)
	{
		LocalFree(m_Security);
	}
}

HRESULT FrameChannel::Open(LUZVDA_FRAME_CHANNEL& Out)
{
	std::lock_guard<std::mutex> lg(m_Lock);
	if (!m_Security)
	{
		return E_ACCESSDENIED;
	}

	if (!m_Header)
	{
		// Named so that a consumer that can't duplicate the driver's handles can open it. The same
		// name may still be open from an earlier display with this GUID, if a consumer kept it:
		// then the counts carry on from there, so no texture name repeats.
		m_Mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &m_Attributes, PAGE_READWRITE, 0, sizeof(LUZVDA_FRAME_HEADER), Widen(luzvda::frames::header_name(m_Prefix)).c_str());
		bool existed = m_Mapping && GetLastError() == ERROR_ALREADY_EXISTS;
		if (!m_Mapping)
		{
			// Windows may not let this process name objects in the global namespace; a consumer can
			// still duplicate the handle
			m_Mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &m_Attributes, PAGE_READWRITE, 0, sizeof(LUZVDA_FRAME_HEADER), nullptr);
		}
		if (!m_Mapping)
		{
			return HRESULT_FROM_WIN32(GetLastError());
		}

		m_Header = (LUZVDA_FRAME_HEADER*)MapViewOfFile(m_Mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(LUZVDA_FRAME_HEADER));
		if (!m_Header)
		{
			HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
			CloseHandle(m_Mapping);
			m_Mapping = nullptr;
			return hr;
		}

		if (existed && m_Header->magic == LUZVDA_FRAME_MAGIC)
		{
			m_Generation = m_Header->generation;
			m_FrameNumber = LUZVDA_FRAME_LATEST_FRAME(Read64(&m_Header->latest));
		}
		std::memset(m_Header, 0, sizeof(*m_Header));
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		m_Header->magic = LUZVDA_FRAME_MAGIC;
		m_Header->version = LUZVDA_FRAME_VERSION;
		m_Header->slot_count = LUZVDA_FRAME_SLOTS;
		m_Header->qpc_frequency = frequency.QuadPart;
		m_Header->owner_pid = GetCurrentProcessId();
		m_Header->generation = m_Generation;
		Write32(&m_Header->state, LUZVDA_FRAME_OPEN);

		// Another program may already have an event by this name; then the event goes unnamed, and
		// only a consumer that duplicates the driver's handle uses it
		m_Ready = CreateEventW(&m_Attributes, FALSE, FALSE, Widen(luzvda::frames::ready_name(m_Prefix)).c_str());
		if (m_Ready && GetLastError() == ERROR_ALREADY_EXISTS)
		{
			CloseHandle(m_Ready);
			m_Ready = nullptr;
		}
		m_ReadyNamed = m_Ready ? LUZVDA_FRAME_NAMED_READY : 0;
		if (!m_Ready)
		{
			m_Ready = CreateEventW(&m_Attributes, FALSE, FALSE, nullptr);
		}
		if (!m_Ready)
		{
			return HRESULT_FROM_WIN32(GetLastError());
		}
		Write32(&m_Header->names, m_ReadyNamed);
	}

	Write64((uint64_t*)&m_Header->consumer_qpc, (uint64_t)Now());

	std::memset(&Out, 0, sizeof(Out));
	Out.struct_size = sizeof(Out);
	Out.owner_pid = GetCurrentProcessId();
	Out.header_handle = (uint64_t)(uintptr_t)m_Mapping;
	Out.ready_handle = (uint64_t)(uintptr_t)m_Ready;
	std::memcpy(Out.prefix, m_Prefix.c_str(), std::min(m_Prefix.size(), sizeof(Out.prefix) - 1));
	return S_OK;
}

void FrameChannel::Publish(ID3D11Device* Device, ID3D11DeviceContext* Context, const LUID& Adapter, IDXGIResource* Surface, int64_t AcquiredQpc)
{
	std::lock_guard<std::mutex> lg(m_Lock);
	if (!m_Header || !Device || !Context || !Surface)
	{
		return;
	}

	// Nobody has asked for frames lately: copy nothing, and keep the textures for when they do
	int64_t consumer = (int64_t)Read64((const uint64_t*)&m_Header->consumer_qpc);
	if (!luzvda::frames::consumer_active(Now(), consumer, m_Header->qpc_frequency))
	{
		return;
	}

	ComPtr<ID3D11Texture2D> frame;
	if (FAILED(Surface->QueryInterface(IID_PPV_ARGS(&frame))))
	{
		return;
	}
	D3D11_TEXTURE2D_DESC desc;
	frame->GetDesc(&desc);

	bool matches = m_RingDevice == Device && !m_RingBroken && m_RingDesc.Width == desc.Width && m_RingDesc.Height == desc.Height && m_RingDesc.Format == desc.Format;
	if (!matches)
	{
		HRESULT hr = MakeRing(Device, Adapter, desc);
		if (FAILED(hr))
		{
			Write32(&m_Header->last_error, (uint32_t)hr);
			m_Change = {Change::Failed, hr, m_Generation, desc.Width, desc.Height, (uint32_t)desc.Format, 0};
			return;
		}
	}

	int slot = luzvda::frames::pick_slot(Read64(&m_Header->latest), LUZVDA_FRAME_SLOTS, [&](uint32_t i) {
		HRESULT hr = m_Slots[i].mutex->AcquireSync(0, 0);
		if (hr == c_KeyedMutexAbandoned)
		{
			m_RingBroken = true;
		}
		return hr == S_OK;
	});
	if (slot < 0)
	{
		InterlockedIncrement64((volatile LONG64*)&m_Header->frames_dropped);
		return;
	}

	Context->CopySubresourceRegion(m_Slots[slot].texture.Get(), 0, 0, 0, 0, frame.Get(), 0, nullptr);

	LUZVDA_FRAME_SLOT& entry = m_Header->slots[slot];
	entry.frame = ++m_FrameNumber;
	entry.acquired_qpc = AcquiredQpc;
	entry.copied_qpc = Now();
	m_Slots[slot].mutex->ReleaseSync(0);

	Write64(&m_Header->latest, luzvda::frames::pack_latest(m_FrameNumber, (uint32_t)slot));
	InterlockedIncrement64((volatile LONG64*)&m_Header->frames_copied);
	SetEvent(m_Ready);
}

void FrameChannel::DropDevice(ID3D11Device* Device)
{
	std::lock_guard<std::mutex> lg(m_Lock);
	if (m_RingDevice == Device)
	{
		DropRing();
	}
}

FrameChannel::Change FrameChannel::TakeChange()
{
	std::lock_guard<std::mutex> lg(m_Lock);
	Change change = m_Change;
	m_Change = {};
	return change;
}

HRESULT FrameChannel::MakeRing(ID3D11Device* Device, const LUID& Adapter, const D3D11_TEXTURE2D_DESC& Frame)
{
	DropRing();

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = Frame.Width;
	desc.Height = Frame.Height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = Frame.Format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

	uint32_t generation = m_Generation + 1;
	uint32_t names = m_ReadyNamed;
	for (uint32_t i = 0; i < LUZVDA_FRAME_SLOTS; ++i)
	{
		Slot& slot = m_Slots[i];
		HRESULT hr = Device->CreateTexture2D(&desc, nullptr, &slot.texture);
		if (SUCCEEDED(hr))
		{
			hr = slot.texture.As(&slot.mutex);
		}
		ComPtr<IDXGIResource1> resource;
		if (SUCCEEDED(hr))
		{
			hr = slot.texture.As(&resource);
		}
		if (FAILED(hr))
		{
			DropRing();
			return hr;
		}

		// By name when the name is free (DXGI_ERROR_NAME_ALREADY_EXISTS otherwise); unnamed if not
		const DWORD access = DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE;
		hr = resource->CreateSharedHandle(&m_Attributes, access, Widen(luzvda::frames::slot_name(m_Prefix, generation, i)).c_str(), &slot.handle);
		if (SUCCEEDED(hr))
		{
			names |= 1u << i;
		}
		else
		{
			hr = resource->CreateSharedHandle(&m_Attributes, access, nullptr, &slot.handle);
		}
		if (FAILED(hr))
		{
			slot.handle = nullptr;
			DropRing();
			return hr;
		}
	}

	m_RingDevice = Device;
	m_RingDesc = desc;
	m_RingBroken = false;
	m_Generation = generation;

	// Everything about the textures first, then the generation that tells the consumer to open them
	m_Header->width = desc.Width;
	m_Header->height = desc.Height;
	m_Header->dxgi_format = (uint32_t)desc.Format;
	m_Header->adapter.low = Adapter.LowPart;
	m_Header->adapter.high = Adapter.HighPart;
	for (uint32_t i = 0; i < LUZVDA_FRAME_SLOTS; ++i)
	{
		m_Header->slots[i].frame = 0;
		m_Header->slots[i].handle = (uint64_t)(uintptr_t)m_Slots[i].handle;
	}
	Write32(&m_Header->names, names);
	Write64(&m_Header->latest, 0);
	Write32(&m_Header->last_error, 0);
	Write32(&m_Header->generation, generation);

	m_Change = {Change::Made, S_OK, generation, desc.Width, desc.Height, (uint32_t)desc.Format, names};
	return S_OK;
}

void FrameChannel::DropRing()
{
	for (auto& slot : m_Slots)
	{
		if (slot.handle)
		{
			CloseHandle(slot.handle);
			slot.handle = nullptr;
		}
		slot.mutex.Reset();
		slot.texture.Reset();
	}
	m_RingDevice = nullptr;
	m_RingDesc = {};
}
