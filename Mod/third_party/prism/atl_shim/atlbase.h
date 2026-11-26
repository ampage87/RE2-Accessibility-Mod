// Minimal stand-in for ATL's <atlbase.h>, used only to build Prism (prism.dll) for the
// RE2 accessibility mod. Visual Studio Build Tools often has no ATL component; Prism uses
// just CComPtr, CComHeapPtr and _T from it, so these are provided here with the same
// behaviour (COM reference counting, CoTaskMemFree). build_prism.ps1 copies this file
// into Prism's source folder, which is on the include path ahead of the SDK.
#pragma once
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <tchar.h>
#include <memory>
#include <utility>

template <class T>
class CComPtr {
public:
    T* p = nullptr;
    CComPtr() noexcept = default;
    CComPtr(std::nullptr_t) noexcept {}
    CComPtr(T* lp) noexcept : p(lp) { if (p) p->AddRef(); }
    CComPtr(const CComPtr& o) noexcept : p(o.p) { if (p) p->AddRef(); }
    CComPtr(CComPtr&& o) noexcept : p(o.p) { o.p = nullptr; }
    ~CComPtr() { Release(); }
    CComPtr& operator=(const CComPtr& o) noexcept { if (this != std::addressof(o)) { if (o.p) o.p->AddRef(); Release(); p = o.p; } return *this; }
    CComPtr& operator=(CComPtr&& o) noexcept { if (this != std::addressof(o)) { Release(); p = o.p; o.p = nullptr; } return *this; }
    CComPtr& operator=(T* lp) noexcept { if (lp) lp->AddRef(); Release(); p = lp; return *this; }
    CComPtr& operator=(std::nullptr_t) noexcept { Release(); return *this; }
    operator T*() const noexcept { return p; }
    T& operator*() const noexcept { return *p; }
    T** operator&() noexcept { return &p; }          // like ATL: for out parameters; p is expected to be empty
    T* operator->() const noexcept { return p; }
    bool operator!() const noexcept { return p == nullptr; }
    void Release() noexcept { T* t = p; if (t) { p = nullptr; t->Release(); } }
    void Attach(T* lp) noexcept { Release(); p = lp; }   // takes ownership, no AddRef
    T* Detach() noexcept { T* t = p; p = nullptr; return t; }
    HRESULT CoCreateInstance(REFCLSID clsid, LPUNKNOWN outer = nullptr, DWORD ctx = CLSCTX_ALL) noexcept {
        Release();
        return ::CoCreateInstance(clsid, outer, ctx, __uuidof(T), reinterpret_cast<void**>(&p));
    }
    // true when both point at the same COM object (compared through IUnknown, as ATL does)
    bool IsEqualObject(IUnknown* other) const noexcept {
        if (!p && !other) return true;
        if (!p || !other) return false;
        IUnknown* a = nullptr; IUnknown* b = nullptr;
        p->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&a));
        other->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void**>(&b));
        bool same = a && a == b;
        if (a) a->Release();
        if (b) b->Release();
        return same;
    }
    template <class Q> HRESULT QueryInterface(Q** pp) const noexcept {
        return p->QueryInterface(__uuidof(Q), reinterpret_cast<void**>(pp));
    }
};

template <class T>
class CComHeapPtr {
public:
    T* m_pData = nullptr;
    CComHeapPtr() noexcept = default;
    explicit CComHeapPtr(T* d) noexcept : m_pData(d) {}
    CComHeapPtr(const CComHeapPtr&) = delete;
    CComHeapPtr& operator=(const CComHeapPtr&) = delete;
    ~CComHeapPtr() { Free(); }
    operator T*() const noexcept { return m_pData; }
    T** operator&() noexcept { return &m_pData; }
    T* operator->() const noexcept { return m_pData; }
    void Free() noexcept { if (m_pData) ::CoTaskMemFree(m_pData); m_pData = nullptr; }
    void Attach(T* d) noexcept { Free(); m_pData = d; }
    T* Detach() noexcept { T* t = m_pData; m_pData = nullptr; return t; }
};
