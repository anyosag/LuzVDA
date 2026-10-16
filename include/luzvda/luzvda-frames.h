/*
 * LuzVDA frame channel helpers (protocol 1.1): the objects' names and the rules both sides follow,
 * and, on Windows, a consumer that takes the newest frame.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 LuzVDA contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
 * associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
 * OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * C++17. The consumer needs D3D11.1 and is left out where LUZVDA_FRAMES_NO_CONSUMER is defined.
 */
#ifndef LUZVDA_FRAMES_H
#define LUZVDA_FRAMES_H

#include "luzvda-ioctl.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace luzvda::frames {
  // Global\LuzVDA-<GUID>, the GUID as Windows writes it, in lower case
  inline std::string prefix_for(const LUZVDA_GUID &guid) {
    const uint8_t *b = guid.bytes;
    char out[LUZVDA_FRAME_PREFIX_BYTES];
    std::snprintf(out, sizeof(out), "Global\\LuzVDA-%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return out;
  }

  inline std::string header_name(const std::string &prefix) {
    return prefix + "-header";
  }

  inline std::string ready_name(const std::string &prefix) {
    return prefix + "-ready";
  }

  inline std::string slot_name(const std::string &prefix, uint32_t generation, uint32_t slot) {
    return prefix + "-g" + std::to_string(generation) + "-s" + std::to_string(slot);
  }

  inline uint64_t pack_latest(uint64_t frame, uint32_t slot) {
    return (frame << 8) | (slot & 0xFFu);
  }

  // Whether the consumer has written consumer_qpc recently enough for the driver to keep copying
  inline bool consumer_active(int64_t now_qpc, int64_t consumer_qpc, int64_t qpc_frequency) {
    if (consumer_qpc <= 0 || qpc_frequency <= 0) {
      return false;
    }
    int64_t age = now_qpc - consumer_qpc;
    return age < qpc_frequency * LUZVDA_FRAME_IDLE_MS / 1000;
  }

  // The slot the driver fills next: any but the newest, starting after it, whose keyed mutex
  // try_acquire(slot) takes without waiting. -1 when the consumer holds the only free one.
  template <class TryAcquire>
  int pick_slot(uint64_t latest, uint32_t slot_count, TryAcquire &&try_acquire) {
    bool have_latest = latest != 0;
    uint32_t newest = LUZVDA_FRAME_LATEST_SLOT(latest);
    uint32_t first = have_latest ? newest + 1 : 0;
    for (uint32_t i = 0; i < slot_count; ++i) {
      uint32_t slot = (first + i) % slot_count;
      if (have_latest && slot == newest) {
        continue;
      }
      if (try_acquire(slot)) {
        return (int) slot;
      }
    }
    return -1;
  }
}  // namespace luzvda::frames

#if defined(_WIN32) && !defined(LUZVDA_FRAMES_NO_CONSUMER)

  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <d3d11_1.h>
  #include <dxgi1_2.h>

namespace luzvda::frames {
  // Takes frames from one display's channel. Not thread-safe: use it from one thread.
  class consumer_t {
  public:
    enum class result_e {
      frame,  // out holds a frame, and its slot until release()
      timeout,  // no newer frame in time
      closed,  // the display is gone
      failed,  // see error()
    };

    struct frame_t {
      ID3D11Texture2D *texture = nullptr;
      uint64_t number = 0;
      // When Windows gave the driver the frame, when the driver queued its copy, and when the ready
      // event woke this consumer
      int64_t acquired_qpc = 0;
      int64_t copied_qpc = 0;
      int64_t ready_qpc = 0;
      uint32_t generation = 0;
      uint32_t width = 0;
      uint32_t height = 0;
      uint32_t dxgi_format = 0;
      uint32_t slot = 0;
    };

    consumer_t() = default;
    consumer_t(const consumer_t &) = delete;
    consumer_t &operator=(const consumer_t &) = delete;

    ~consumer_t() {
      close();
    }

    // Asks the driver for the display's channel through a handle to the LuzVDA device. Returns a
    // Win32 error, 0 on success.
    DWORD open(HANDLE driver, const LUZVDA_GUID &guid) {
      close();

      LUZVDA_FRAME_CHANNEL channel = {};
      DWORD returned = 0;
      if (!DeviceIoControl(driver, IOCTL_LUZVDA_OPEN_FRAME_CHANNEL, (void *) &guid, sizeof(guid), &channel, sizeof(channel), &returned, nullptr)) {
        return GetLastError();
      }
      if (returned < sizeof(channel) || channel.struct_size != sizeof(channel)) {
        return ERROR_REVISION_MISMATCH;
      }
      channel.prefix[sizeof(channel.prefix) - 1] = '\0';
      m_prefix = channel.prefix;
      m_owner_pid = channel.owner_pid;

      m_mapping = duplicate(channel.header_handle);
      m_header_by = m_mapping ? by_handle : by_name;
      if (!m_mapping) {
        m_mapping = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, header_name(m_prefix).c_str());
      }
      if (!m_mapping) {
        return GetLastError();
      }
      m_header = (LUZVDA_FRAME_HEADER *) MapViewOfFile(m_mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(LUZVDA_FRAME_HEADER));
      if (!m_header) {
        DWORD error = GetLastError();
        close();
        return error;
      }
      if (m_header->magic != LUZVDA_FRAME_MAGIC || m_header->version != LUZVDA_FRAME_VERSION) {
        close();
        return ERROR_REVISION_MISMATCH;
      }

      m_ready = duplicate(channel.ready_handle);
      if (!m_ready && (read32(&m_header->names) & LUZVDA_FRAME_NAMED_READY)) {
        m_ready = OpenEventA(SYNCHRONIZE, FALSE, ready_name(m_prefix).c_str());
      }
      if (!m_ready) {
        DWORD error = GetLastError();
        close();
        return error ? error : ERROR_ACCESS_DENIED;
      }

      heartbeat();
      return 0;
    }

    // The GPU the textures live on: create the device passed to use_device() on it
    LUID adapter() const {
      LUID luid = {};
      if (m_header) {
        luid.LowPart = m_header->adapter.low;
        luid.HighPart = m_header->adapter.high;
      }
      return luid;
    }

    bool has_textures() const {
      return m_header && read32(&m_header->generation) != 0;
    }

    void use_device(ID3D11Device1 *device) {
      drop_textures();
      if (m_device) {
        m_device->Release();
      }
      m_device = device;
      if (m_device) {
        m_device->AddRef();
      }
    }

    // Tells the driver someone still wants frames. next() does it too.
    void heartbeat() {
      if (m_header) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        InterlockedExchange64((volatile LONG64 *) &m_header->consumer_qpc, now.QuadPart);
      }
    }

    // Waits up to timeout_ms for a frame newer than the last one taken, and holds its slot
    result_e next(DWORD timeout_ms, frame_t &out) {
      if (!m_header || !m_ready) {
        m_error = E_HANDLE;
        return result_e::failed;
      }
      ULONGLONG deadline = GetTickCount64() + timeout_ms;
      for (bool first = true;; first = false) {
        if (!first && GetTickCount64() > deadline) {
          return result_e::timeout;
        }
        heartbeat();
        if (read32(&m_header->state) == LUZVDA_FRAME_CLOSED) {
          return result_e::closed;
        }

        ULONGLONG now = GetTickCount64();
        DWORD wait = now >= deadline ? 0 : (DWORD) std::min<ULONGLONG>(deadline - now, 250);
        DWORD woke = WaitForSingleObject(m_ready, wait);
        LARGE_INTEGER ready_qpc;
        QueryPerformanceCounter(&ready_qpc);
        if (woke == WAIT_FAILED) {
          m_error = HRESULT_FROM_WIN32(GetLastError());
          return result_e::failed;
        }

        uint64_t latest = read64(&m_header->latest);
        uint64_t number = LUZVDA_FRAME_LATEST_FRAME(latest);
        uint32_t generation = read32(&m_header->generation);
        if (latest == 0 || number <= m_last_frame || generation == 0) {
          continue;
        }

        if (generation != m_generation) {
          HRESULT hr = open_textures(generation);
          if (FAILED(hr)) {
            m_error = hr;
            return result_e::failed;
          }
        }

        uint32_t slot = LUZVDA_FRAME_LATEST_SLOT(latest);
        if (slot >= LUZVDA_FRAME_SLOTS || !m_slots[slot].mutex) {
          m_error = E_UNEXPECTED;
          return result_e::failed;
        }

        HRESULT hr = m_slots[slot].mutex->AcquireSync(0, 100);
        if (hr == (HRESULT) WAIT_TIMEOUT) {
          continue;
        }
        if (hr != S_OK) {
          // WAIT_ABANDONED: the driver's device went away holding it; its next textures replace these
          drop_textures();
          m_error = hr;
          if (hr == (HRESULT) WAIT_ABANDONED) {
            continue;
          }
          return result_e::failed;
        }

        const LUZVDA_FRAME_SLOT &entry = m_header->slots[slot];
        if (read32(&m_header->generation) != m_generation || entry.frame <= m_last_frame) {
          m_slots[slot].mutex->ReleaseSync(0);
          continue;
        }

        out.texture = m_slots[slot].texture;
        out.number = entry.frame;
        out.acquired_qpc = entry.acquired_qpc;
        out.copied_qpc = entry.copied_qpc;
        out.ready_qpc = woke == WAIT_OBJECT_0 ? ready_qpc.QuadPart : 0;
        out.generation = m_generation;
        out.width = m_width;
        out.height = m_height;
        out.dxgi_format = m_format;
        out.slot = slot;
        m_last_frame = entry.frame;
        m_held = (int) slot;
        return result_e::frame;
      }
    }

    // Gives the slot back to the driver; copy out of the texture first
    void release(frame_t &frame) {
      if (m_held >= 0 && m_slots[m_held].mutex) {
        m_slots[m_held].mutex->ReleaseSync(0);
      }
      m_held = -1;
      frame.texture = nullptr;
    }

    void close() {
      drop_textures();
      if (m_header) {
        UnmapViewOfFile(m_header);
        m_header = nullptr;
      }
      close_handle(m_mapping);
      close_handle(m_ready);
      close_handle(m_owner);
      if (m_device) {
        m_device->Release();
        m_device = nullptr;
      }
      m_last_frame = 0;
    }

    const LUZVDA_FRAME_HEADER *header() const {
      return m_header;
    }

    HRESULT error() const {
      return m_error;
    }

    // How the header and the textures were opened: "handle" (duplicated from the driver) or "name"
    const char *header_opened_by() const {
      return m_header_by == by_handle ? "handle" : "name";
    }

    const char *textures_opened_by() const {
      return m_textures_by == by_handle ? "handle" : "name";
    }

  private:
    enum opened_e {
      by_handle,
      by_name,
    };

    struct slot_t {
      ID3D11Texture2D *texture = nullptr;
      IDXGIKeyedMutex *mutex = nullptr;
    };

    static uint32_t read32(const uint32_t *value) {
      return (uint32_t) InterlockedCompareExchange((volatile LONG *) value, 0, 0);
    }

    static uint64_t read64(const uint64_t *value) {
      return (uint64_t) InterlockedCompareExchange64((volatile LONG64 *) value, 0, 0);
    }

    static void close_handle(HANDLE &handle) {
      if (handle) {
        CloseHandle(handle);
        handle = nullptr;
      }
    }

    static std::wstring widen(const std::string &text) {
      return std::wstring(text.begin(), text.end());
    }

    // The driver's handle, as a handle in this process; nullptr if this process may not take it
    HANDLE duplicate(uint64_t handle) {
      if (!handle) {
        return nullptr;
      }
      if (!m_owner) {
        m_owner = OpenProcess(PROCESS_DUP_HANDLE, FALSE, m_owner_pid);
        if (!m_owner) {
          return nullptr;
        }
      }
      HANDLE out = nullptr;
      if (!DuplicateHandle(m_owner, (HANDLE) (uintptr_t) handle, GetCurrentProcess(), &out, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return nullptr;
      }
      return out;
    }

    void drop_textures() {
      if (m_held >= 0 && m_slots[m_held].mutex) {
        m_slots[m_held].mutex->ReleaseSync(0);
      }
      m_held = -1;
      for (auto &slot : m_slots) {
        if (slot.mutex) {
          slot.mutex->Release();
          slot.mutex = nullptr;
        }
        if (slot.texture) {
          slot.texture->Release();
          slot.texture = nullptr;
        }
      }
      m_generation = 0;
    }

    HRESULT open_textures(uint32_t generation) {
      drop_textures();
      if (!m_device) {
        return E_HANDLE;
      }

      m_width = read32(&m_header->width);
      m_height = read32(&m_header->height);
      m_format = read32(&m_header->dxgi_format);
      uint32_t names = read32(&m_header->names);
      bool any_by_name = false;
      for (uint32_t i = 0; i < LUZVDA_FRAME_SLOTS; ++i) {
        HRESULT hr = E_FAIL;
        HANDLE handle = duplicate(read64(&m_header->slots[i].handle));
        if (handle) {
          hr = m_device->OpenSharedResource1(handle, __uuidof(ID3D11Texture2D), (void **) &m_slots[i].texture);
          CloseHandle(handle);
        }
        if (FAILED(hr) && (names & (1u << i))) {
          hr = m_device->OpenSharedResourceByName(widen(slot_name(m_prefix, generation, i)).c_str(), DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, __uuidof(ID3D11Texture2D), (void **) &m_slots[i].texture);
          any_by_name = any_by_name || SUCCEEDED(hr);
        }
        if (SUCCEEDED(hr)) {
          hr = m_slots[i].texture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) &m_slots[i].mutex);
        }
        if (FAILED(hr)) {
          drop_textures();
          return hr;
        }
      }

      // Made again while these were being opened: the next call opens the new ones
      if (read32(&m_header->generation) != generation) {
        drop_textures();
        return S_OK;
      }
      m_generation = generation;
      m_textures_by = any_by_name ? by_name : by_handle;
      return S_OK;
    }

    std::string m_prefix;
    DWORD m_owner_pid = 0;
    HANDLE m_owner = nullptr;
    HANDLE m_mapping = nullptr;
    HANDLE m_ready = nullptr;
    LUZVDA_FRAME_HEADER *m_header = nullptr;
    ID3D11Device1 *m_device = nullptr;
    slot_t m_slots[LUZVDA_FRAME_SLOTS];
    uint32_t m_generation = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_format = 0;
    uint64_t m_last_frame = 0;
    int m_held = -1;
    HRESULT m_error = S_OK;
    opened_e m_header_by = by_handle;
    opened_e m_textures_by = by_handle;
  };
}  // namespace luzvda::frames

#endif

#endif /* LUZVDA_FRAMES_H */
