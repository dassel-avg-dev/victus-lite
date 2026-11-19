#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <oleauto.h>
#include <wbemidl.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#endif

namespace hp {

struct Bstr {
    BSTR value = nullptr;
    explicit Bstr(const wchar_t* s) : value(SysAllocString(s)) {}
    ~Bstr() { if (value) SysFreeString(value); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    operator BSTR() const { return value; }
};

enum class PerformanceMode : uint8_t {
    Balanced    = 0x30,
    Performance = 0x31,
    Cool        = 0x50,
};

struct FanStatus {
    uint32_t fan1_rpm = 0;
    uint32_t fan2_rpm = 0;
};

struct BiosCallResult {
    bool ok = false;
    uint32_t bios_return_code = 0;
    std::vector<uint8_t> data;
    std::string error;
};

class WmiController {
public:
    WmiController() = default;
    ~WmiController() { close(); }

    WmiController(const WmiController&) = delete;
    WmiController& operator=(const WmiController&) = delete;

    bool init(std::string& out_error) {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            out_error = "CoInitializeEx failed: " + hex_hr(hr);
            return false;
        }
        com_initialized_ = SUCCEEDED(hr);

        hr = CoInitializeSecurity(
            nullptr, -1, nullptr, nullptr,
            RPC_C_AUTHN_LEVEL_DEFAULT,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr, EOAC_NONE, nullptr);
        if (FAILED(hr) && hr != RPC_E_TOO_LATE) {
            out_error = "CoInitializeSecurity failed: " + hex_hr(hr);
            return false;
        }

        hr = CoCreateInstance(
            CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
            IID_IWbemLocator, reinterpret_cast<void**>(&locator_));
        if (FAILED(hr)) {
            out_error = "CoCreateInstance(WbemLocator) failed: " + hex_hr(hr);
            return false;
        }

        hr = locator_->ConnectServer(
            Bstr(L"ROOT\\WMI"),
            nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services_);
        if (FAILED(hr)) {
            out_error = "ConnectServer(ROOT\\WMI) failed (run as Administrator): " + hex_hr(hr);
            return false;
        }

        hr = CoSetProxyBlanket(
            services_, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr, EOAC_NONE);
        if (FAILED(hr)) {
            out_error = "CoSetProxyBlanket failed: " + hex_hr(hr);
            return false;
        }

        // Locate the first active hpqBIntM instance path.
        IEnumWbemClassObject* enumerator = nullptr;
        hr = services_->CreateInstanceEnum(
            Bstr(L"hpqBIntM"),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, &enumerator);
        if (FAILED(hr)) {
            out_error = "hpqBIntM class not found (HP BIOS WMI unavailable): " + hex_hr(hr);
            return false;
        }

        IWbemClassObject* obj = nullptr;
        ULONG returned = 0;
        hr = enumerator->Next(WBEM_INFINITE, 1, &obj, &returned);
        enumerator->Release();

        if (FAILED(hr) || returned == 0 || !obj) {
            out_error = "No hpqBIntM instance found (run as Administrator on an HP OMEN/Victus).";
            return false;
        }

        VARIANT path_var;
        VariantInit(&path_var);
        hr = obj->Get(L"__RELPATH", 0, &path_var, nullptr, nullptr);
        obj->Release();

        if (FAILED(hr) || path_var.vt != VT_BSTR) {
            VariantClear(&path_var);
            out_error = "Failed to read hpqBIntM __RELPATH.";
            return false;
        }

        instance_path_ = path_var.bstrVal;
        VariantClear(&path_var);
        return true;
    }

    void close() {
        if (services_) {
            services_->Release();
            services_ = nullptr;
        }
        if (locator_) {
            locator_->Release();
            locator_ = nullptr;
        }
        if (com_initialized_) {
            CoUninitialize();
            com_initialized_ = false;
        }
    }

    BiosCallResult set_performance_mode(PerformanceMode mode) {
        // Command 0x20008, CommandType 0x1A: { 0xFF, thermal_policy_byte }
        const std::vector<uint8_t> payload = { 0xFF, static_cast<uint8_t>(mode) };
        return call_bios(0x20008, 0x1A, payload, L"hpqBIOSInt0");
    }

    BiosCallResult set_fan_max(bool enable) {
        // Command 0x20008, CommandType 0x27: { 0x01 } = Max, { 0x00 } = Auto
        const std::vector<uint8_t> payload = { static_cast<uint8_t>(enable ? 0x01 : 0x00) };
        return call_bios(0x20008, 0x27, payload, L"hpqBIOSInt0");
    }

    BiosCallResult set_fan_levels(uint8_t fan1_x100, uint8_t fan2_x100) {
        // Command 0x20008, CommandType 0x2E: { fan1_level, fan2_level } in 100-RPM units (0 = auto)
        const std::vector<uint8_t> payload = { fan1_x100, fan2_x100 };
        return call_bios(0x20008, 0x2E, payload, L"hpqBIOSInt0");
    }

    std::optional<FanStatus> get_fan_status(std::string& out_error) {
        // Command 0x20008, CommandType 0x2D: returns fan speeds divided by 100
        auto res = call_bios(0x20008, 0x2D, {}, L"hpqBIOSInt128");
        if (!res.ok) {
            out_error = res.error;
            return std::nullopt;
        }
        FanStatus status{};
        if (res.data.size() >= 1) status.fan1_rpm = static_cast<uint32_t>(res.data[0]) * 100u;
        if (res.data.size() >= 2) status.fan2_rpm = static_cast<uint32_t>(res.data[1]) * 100u;
        return status;
    }

    BiosCallResult call_bios(
        uint32_t command,
        uint32_t command_type,
        const std::vector<uint8_t>& payload,
        const wchar_t* method_name = L"hpqBIOSInt128")
    {
        BiosCallResult result{};
        if (!services_ || instance_path_.empty()) {
            result.error = "WMI controller not initialized.";
            return result;
        }

        IWbemClassObject* bintm_class = nullptr;
        HRESULT hr = services_->GetObject(Bstr(L"hpqBIntM"), 0, nullptr, &bintm_class, nullptr);
        if (FAILED(hr) || !bintm_class) {
            result.error = "GetObject(hpqBIntM) failed: " + hex_hr(hr);
            return result;
        }

        IWbemClassObject* in_params_def = nullptr;
        hr = bintm_class->GetMethod(method_name, 0, &in_params_def, nullptr);
        bintm_class->Release();
        if (FAILED(hr) || !in_params_def) {
            result.error = "GetMethod failed: " + hex_hr(hr);
            return result;
        }

        IWbemClassObject* in_params = nullptr;
        hr = in_params_def->SpawnInstance(0, &in_params);
        in_params_def->Release();
        if (FAILED(hr) || !in_params) {
            result.error = "SpawnInstance(in_params) failed: " + hex_hr(hr);
            return result;
        }

        IWbemClassObject* data_in_class = nullptr;
        hr = services_->GetObject(Bstr(L"hpqBDataIn"), 0, nullptr, &data_in_class, nullptr);
        if (FAILED(hr) || !data_in_class) {
            in_params->Release();
            result.error = "GetObject(hpqBDataIn) failed: " + hex_hr(hr);
            return result;
        }

        IWbemClassObject* data_in = nullptr;
        hr = data_in_class->SpawnInstance(0, &data_in);
        data_in_class->Release();
        if (FAILED(hr) || !data_in) {
            in_params->Release();
            result.error = "SpawnInstance(hpqBDataIn) failed: " + hex_hr(hr);
            return result;
        }

        // Populate hpqBDataIn fields: Sign ("SECU"), Command, CommandType, Size, hpqBData
        const std::vector<uint8_t> sign = { 'S', 'E', 'C', 'U' };
        put_byte_array(data_in, L"Sign", sign);
        put_uint32(data_in, L"Command", command);
        put_uint32(data_in, L"CommandType", command_type);
        put_uint32(data_in, L"Size", static_cast<uint32_t>(payload.size()));
        if (!payload.empty()) {
            put_byte_array(data_in, L"hpqBData", payload);
        }

        VARIANT v_in;
        VariantInit(&v_in);
        v_in.vt = VT_UNKNOWN;
        v_in.punkVal = data_in;
        data_in->AddRef();
        hr = in_params->Put(L"InData", 0, &v_in, 0);
        VariantClear(&v_in);
        data_in->Release();

        if (FAILED(hr)) {
            in_params->Release();
            result.error = "Failed to bind InData parameter: " + hex_hr(hr);
            return result;
        }

        IWbemClassObject* out_params = nullptr;
        hr = services_->ExecMethod(
            Bstr(instance_path_.c_str()),
            Bstr(method_name),
            0, nullptr, in_params, &out_params, nullptr);
        in_params->Release();

        if (FAILED(hr) || !out_params) {
            result.error = "ExecMethod failed: " + hex_hr(hr);
            return result;
        }

        VARIANT v_out;
        VariantInit(&v_out);
        hr = out_params->Get(L"OutData", 0, &v_out, nullptr, nullptr);
        out_params->Release();

        if (SUCCEEDED(hr) && v_out.vt == VT_UNKNOWN && v_out.punkVal) {
            IWbemClassObject* out_data = nullptr;
            if (SUCCEEDED(v_out.punkVal->QueryInterface(IID_IWbemClassObject, reinterpret_cast<void**>(&out_data))) && out_data) {
                VARIANT v_rc;
                VariantInit(&v_rc);
                if (SUCCEEDED(out_data->Get(L"rwReturnCode", 0, &v_rc, nullptr, nullptr))) {
                    result.bios_return_code = static_cast<uint32_t>(v_rc.ulVal);
                }
                VariantClear(&v_rc);

                VARIANT v_bytes;
                VariantInit(&v_bytes);
                if (SUCCEEDED(out_data->Get(L"Data", 0, &v_bytes, nullptr, nullptr)) &&
                    (v_bytes.vt == (VT_ARRAY | VT_UI1)) && v_bytes.parray) {
                    LONG lb = 0, ub = 0;
                    SafeArrayGetLBound(v_bytes.parray, 1, &lb);
                    SafeArrayGetUBound(v_bytes.parray, 1, &ub);
                    if (ub >= lb) {
                        uint8_t* raw = nullptr;
                        if (SUCCEEDED(SafeArrayAccessData(v_bytes.parray, reinterpret_cast<void**>(&raw)))) {
                            result.data.assign(raw, raw + (ub - lb + 1));
                            SafeArrayUnaccessData(v_bytes.parray);
                        }
                    }
                }
                VariantClear(&v_bytes);
                out_data->Release();
            }
        }
        VariantClear(&v_out);

        result.ok = (result.bios_return_code == 0);
        if (!result.ok) {
            result.error = "BIOS returned non-zero status: " + hex_u32(result.bios_return_code);
        }
        return result;
    }

private:
    static void put_uint32(IWbemClassObject* obj, const wchar_t* name, uint32_t value) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = static_cast<LONG>(value);
        obj->Put(name, 0, &v, CIM_UINT32);
        VariantClear(&v);
    }

    static void put_byte_array(IWbemClassObject* obj, const wchar_t* name, const std::vector<uint8_t>& bytes) {
        SAFEARRAYBOUND bound{};
        bound.lLbound = 0;
        bound.cElements = static_cast<ULONG>(bytes.size());
        SAFEARRAY* sa = SafeArrayCreate(VT_UI1, 1, &bound);
        if (!sa) return;

        uint8_t* raw = nullptr;
        if (SUCCEEDED(SafeArrayAccessData(sa, reinterpret_cast<void**>(&raw)))) {
            for (size_t i = 0; i < bytes.size(); ++i) raw[i] = bytes[i];
            SafeArrayUnaccessData(sa);
        }

        VARIANT v;
        VariantInit(&v);
        v.vt = VT_ARRAY | VT_UI1;
        v.parray = sa;
        obj->Put(name, 0, &v, CIM_UINT8 | CIM_FLAG_ARRAY);
        VariantClear(&v);
    }

    static std::string hex_hr(HRESULT hr) {
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
        return buf;
    }

    static std::string hex_u32(uint32_t v) {
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%08X", v);
        return buf;
    }

    IWbemLocator* locator_ = nullptr;
    IWbemServices* services_ = nullptr;
    std::wstring instance_path_;
    bool com_initialized_ = false;
};

} // namespace hp
