// cs-test's lab helper (tools/cs-test/README.md, "Lab"). It lets several games run at once, each from its own lab slot.
//
// - The game exits at start-up when a window of its class exists (1.6.1170 0x140640120). FindWindowA and FindWindowW
//   answer "none" for that class.
// - Every module asks the shell for Documents and local AppData through a few functions: the game and SKSE through
//   SHGetFolderPathA, CommonLib (Community Shaders' log, status and test files) through SHGetKnownFolderPath. Those
//   functions are detoured, so they answer with the slot's home (CS_TEST_LAB_HOME) for every module and thread.
//   INIs, plugins.txt, saves and the plugins' logs then stay in the slot.
// - It writes <home>\cs-test-lab.json when it has loaded, at kPostLoad and at kDataLoaded. cs-test starts the next game only
//   after that, because CommonLib's shared Address Library table fails when two games fill it at once.
//
// Plain Win32 and Detours, no CommonLib: it must not use the shared table itself, and it must load before every other
// plugin ("!" sorts first; SKSE loads plugins in directory order).

#include <Windows.h>
#include <ShlObj.h>
#include <KnownFolders.h>
#include <detours/detours.h>

#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace
{
	// SKSE's plugin API, as skse64's PluginAPI.h declares it (2.2.x).
	struct SKSEInterface
	{
		std::uint32_t skseVersion;
		std::uint32_t runtimeVersion;
		std::uint32_t editorVersion;
		std::uint32_t isEditor;
		void* (*QueryInterface)(std::uint32_t id);
		std::uint32_t (*GetPluginHandle)();
		std::uint32_t (*GetReleaseIndex)();
		const void* (*GetPluginInfo)(const char* name);
	};

	struct SKSEMessage
	{
		const char* sender;
		std::uint32_t type;
		std::uint32_t dataLen;
		void* data;
	};

	using EventCallback = void (*)(SKSEMessage* message);

	struct SKSEMessagingInterface
	{
		std::uint32_t interfaceVersion;
		bool (*RegisterListener)(std::uint32_t listener, const char* sender, EventCallback handler);
		bool (*Dispatch)(std::uint32_t sender, std::uint32_t messageType, void* data, std::uint32_t dataLen, const char* receiver);
		void* (*GetEventDispatcher)(std::uint32_t dispatcherId);
	};

	constexpr std::uint32_t kInterfaceMessaging = 5;
	constexpr std::uint32_t kMessagePostLoad = 0;
	constexpr std::uint32_t kMessageDataLoaded = 8;

	constexpr const char* kGameWindowClass = "Skyrim Special Edition";
	constexpr const wchar_t* kGameWindowClassW = L"Skyrim Special Edition";

	std::wstring g_home;
	std::wstring g_documents;
	std::wstring g_localAppData;
	std::wstring g_logPath;
	std::vector<std::string> g_hooked;
	std::atomic<long> g_answered{ 0 };

	decltype(&FindWindowA) g_findWindowA = nullptr;
	decltype(&FindWindowW) g_findWindowW = nullptr;
	decltype(&SHGetFolderPathA) g_getFolderPathA = nullptr;
	decltype(&SHGetFolderPathW) g_getFolderPathW = nullptr;
	decltype(&SHGetKnownFolderPath) g_getKnownFolderPath = nullptr;
	decltype(&SHGetSpecialFolderPathA) g_getSpecialFolderPathA = nullptr;
	decltype(&SHGetSpecialFolderPathW) g_getSpecialFolderPathW = nullptr;

	std::string Narrow(const std::wstring& a_text, UINT a_codePage = CP_UTF8)
	{
		if (a_text.empty())
			return {};
		const int size = WideCharToMultiByte(a_codePage, 0, a_text.c_str(), static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<std::size_t>(size), '\0');
		WideCharToMultiByte(a_codePage, 0, a_text.c_str(), static_cast<int>(a_text.size()), out.data(), size, nullptr, nullptr);
		return out;
	}

	void Log(const std::string& a_line)
	{
		if (g_logPath.empty())
			return;
		FILE* file = nullptr;
		if (_wfopen_s(&file, g_logPath.c_str(), L"ab") != 0 || !file)
			return;
		SYSTEMTIME now{};
		GetLocalTime(&now);
		std::fprintf(file, "[%02u:%02u:%02u.%03u] %s\r\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, a_line.c_str());
		std::fclose(file);
	}

	// A lab game that cannot be pointed at its home must not run on: it would read the user's INIs and write into their
	// saves. The reason goes into a file beside this plugin (the slot's own folder), where cs-test reads it.
	[[noreturn]] void Fatal(const std::string& a_reason)
	{
		wchar_t self[MAX_PATH]{};
		HMODULE module = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Fatal), &module);
		GetModuleFileNameW(module, self, MAX_PATH);
		std::wstring path(self);
		path = path.substr(0, path.find_last_of(L'\\') + 1) + L"CSTestLab-error.txt";
		FILE* file = nullptr;
		if (_wfopen_s(&file, path.c_str(), L"wb") == 0 && file) {
			std::fprintf(file, "%s\r\n", a_reason.c_str());
			std::fclose(file);
		}
		Log("fatal: " + a_reason);
		OutputDebugStringA(("CSTestLab: " + a_reason + "\n").c_str());
		TerminateProcess(GetCurrentProcess(), 3);
		for (;;) {}
	}

	std::string JsonText(const std::wstring& a_text)
	{
		std::string out;
		for (const char c : Narrow(a_text)) {
			if (c == '\\' || c == '"')
				out += '\\';
			out += c;
		}
		return out;
	}

	// <home>\cs-test-lab.json, written to a temporary file and renamed, so cs-test never reads half of it.
	void WriteMarker(const char* a_phase)
	{
		std::string hooks;
		for (const auto& name : g_hooked)
			hooks += (hooks.empty() ? "\"" : ", \"") + name + "\"";
		char text[2048];
		std::snprintf(text, sizeof(text), "{\"pid\": %lu, \"phase\": \"%s\", \"home\": \"%s\", \"hooks\": [%s], \"answered\": %ld}\n", GetCurrentProcessId(), a_phase,
			JsonText(g_home).c_str(), hooks.c_str(), g_answered.load());
		const std::wstring path = g_home + L"\\cs-test-lab.json";
		const std::wstring temp = path + L".tmp";
		FILE* file = nullptr;
		if (_wfopen_s(&file, temp.c_str(), L"wb") != 0 || !file)
			Fatal("could not write " + Narrow(temp));
		std::fputs(text, file);
		std::fclose(file);
		if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
			Fatal("could not rename " + Narrow(temp));
	}

	// The home's folder for a CSIDL, or null for the folders it leaves alone.
	const std::wstring* HomeFolderFor(int a_csidl)
	{
		switch (a_csidl & ~CSIDL_FLAG_MASK) {
		case CSIDL_PERSONAL:
			return &g_documents;
		case CSIDL_LOCAL_APPDATA:
			return &g_localAppData;
		default:
			return nullptr;
		}
	}

	const std::wstring* HomeFolderFor(REFKNOWNFOLDERID a_id)
	{
		if (IsEqualGUID(a_id, FOLDERID_Documents))
			return &g_documents;
		if (IsEqualGUID(a_id, FOLDERID_LocalAppData))
			return &g_localAppData;
		return nullptr;
	}

	// Which module asked, for the log: the first answers name their caller, so a run shows who reads the home.
	void Answered(const char* a_api, const std::wstring& a_folder, void* a_caller)
	{
		const long n = ++g_answered;
		if (n > 200)
			return;
		HMODULE module = nullptr;
		wchar_t name[MAX_PATH] = L"?";
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(a_caller), &module))
			GetModuleFileNameW(module, name, MAX_PATH);
		std::wstring caller(name);
		Log(std::string(a_api) + " -> " + Narrow(a_folder) + " for " + Narrow(caller.substr(caller.find_last_of(L'\\') + 1)));
	}

	HWND WINAPI FindWindowAHook(LPCSTR a_class, LPCSTR a_title)
	{
		if (a_class && !IS_INTRESOURCE(a_class) && std::strcmp(a_class, kGameWindowClass) == 0)
			return nullptr;
		return g_findWindowA(a_class, a_title);
	}

	HWND WINAPI FindWindowWHook(LPCWSTR a_class, LPCWSTR a_title)
	{
		if (a_class && !IS_INTRESOURCE(a_class) && std::wcscmp(a_class, kGameWindowClassW) == 0)
			return nullptr;
		return g_findWindowW(a_class, a_title);
	}

	HRESULT WINAPI SHGetFolderPathWHook(HWND a_owner, int a_csidl, HANDLE a_token, DWORD a_flags, LPWSTR a_path)
	{
		const auto* folder = HomeFolderFor(a_csidl);
		if (!folder)
			return g_getFolderPathW(a_owner, a_csidl, a_token, a_flags, a_path);
		if (folder->size() >= MAX_PATH)
			return E_FAIL;
		if (a_csidl & CSIDL_FLAG_CREATE)
			SHCreateDirectoryExW(nullptr, folder->c_str(), nullptr);
		wcscpy_s(a_path, MAX_PATH, folder->c_str());
		Answered("SHGetFolderPathW", *folder, _ReturnAddress());
		return S_OK;
	}

	HRESULT WINAPI SHGetFolderPathAHook(HWND a_owner, int a_csidl, HANDLE a_token, DWORD a_flags, LPSTR a_path)
	{
		const auto* folder = HomeFolderFor(a_csidl);
		if (!folder)
			return g_getFolderPathA(a_owner, a_csidl, a_token, a_flags, a_path);
		const auto text = Narrow(*folder, CP_ACP);
		if (text.size() >= MAX_PATH)
			return E_FAIL;
		if (a_csidl & CSIDL_FLAG_CREATE)
			SHCreateDirectoryExW(nullptr, folder->c_str(), nullptr);
		std::memcpy(a_path, text.c_str(), text.size() + 1);
		Answered("SHGetFolderPathA", *folder, _ReturnAddress());
		return S_OK;
	}

	HRESULT WINAPI SHGetKnownFolderPathHook(REFKNOWNFOLDERID a_id, DWORD a_flags, HANDLE a_token, PWSTR* a_path)
	{
		const auto* folder = HomeFolderFor(a_id);
		if (!folder)
			return g_getKnownFolderPath(a_id, a_flags, a_token, a_path);
		if (!a_path)
			return E_INVALIDARG;
		if (a_flags & KF_FLAG_CREATE)
			SHCreateDirectoryExW(nullptr, folder->c_str(), nullptr);
		const auto bytes = (folder->size() + 1) * sizeof(wchar_t);
		auto* out = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
		if (!out) {
			*a_path = nullptr;
			return E_OUTOFMEMORY;
		}
		std::memcpy(out, folder->c_str(), bytes);
		*a_path = out;
		Answered("SHGetKnownFolderPath", *folder, _ReturnAddress());
		return S_OK;
	}

	BOOL WINAPI SHGetSpecialFolderPathWHook(HWND a_owner, LPWSTR a_path, int a_csidl, BOOL a_create)
	{
		const auto* folder = HomeFolderFor(a_csidl);
		if (!folder)
			return g_getSpecialFolderPathW(a_owner, a_path, a_csidl, a_create);
		if (folder->size() >= MAX_PATH)
			return FALSE;
		if (a_create)
			SHCreateDirectoryExW(nullptr, folder->c_str(), nullptr);
		wcscpy_s(a_path, MAX_PATH, folder->c_str());
		Answered("SHGetSpecialFolderPathW", *folder, _ReturnAddress());
		return TRUE;
	}

	BOOL WINAPI SHGetSpecialFolderPathAHook(HWND a_owner, LPSTR a_path, int a_csidl, BOOL a_create)
	{
		const auto* folder = HomeFolderFor(a_csidl);
		if (!folder)
			return g_getSpecialFolderPathA(a_owner, a_path, a_csidl, a_create);
		const auto text = Narrow(*folder, CP_ACP);
		if (text.size() >= MAX_PATH)
			return FALSE;
		if (a_create)
			SHCreateDirectoryExW(nullptr, folder->c_str(), nullptr);
		std::memcpy(a_path, text.c_str(), text.size() + 1);
		Answered("SHGetSpecialFolderPathA", *folder, _ReturnAddress());
		return TRUE;
	}

	// The function every caller reaches: GetProcAddress follows shell32's forwarders (to windows.storage.dll), as the
	// loader does when it binds imports.
	template <class T>
	T Resolve(const wchar_t* a_module, const char* a_name)
	{
		HMODULE module = GetModuleHandleW(a_module);
		if (!module)
			module = LoadLibraryW(a_module);
		if (!module)
			Fatal("could not load " + Narrow(a_module));
		auto* function = reinterpret_cast<T>(GetProcAddress(module, a_name));
		if (!function)
			Fatal(std::string("no ") + a_name + " in " + Narrow(a_module));
		return function;
	}

	template <class T>
	void Attach(T& a_real, T a_hook, const wchar_t* a_module, const char* a_name)
	{
		a_real = Resolve<T>(a_module, a_name);
		const LONG error = DetourAttach(reinterpret_cast<PVOID*>(&a_real), reinterpret_cast<PVOID>(a_hook));
		if (error != NO_ERROR)
			Fatal(std::string("DetourAttach(") + a_name + ") failed: " + std::to_string(error));
		g_hooked.emplace_back(a_name);
	}

	void InstallHooks()
	{
		if (DetourTransactionBegin() != NO_ERROR)
			Fatal("DetourTransactionBegin failed");
		DetourUpdateThread(GetCurrentThread());
		Attach(g_findWindowA, &FindWindowAHook, L"user32.dll", "FindWindowA");
		Attach(g_findWindowW, &FindWindowWHook, L"user32.dll", "FindWindowW");
		Attach(g_getFolderPathA, &SHGetFolderPathAHook, L"shell32.dll", "SHGetFolderPathA");
		Attach(g_getFolderPathW, &SHGetFolderPathWHook, L"shell32.dll", "SHGetFolderPathW");
		Attach(g_getKnownFolderPath, &SHGetKnownFolderPathHook, L"shell32.dll", "SHGetKnownFolderPath");
		Attach(g_getSpecialFolderPathA, &SHGetSpecialFolderPathAHook, L"shell32.dll", "SHGetSpecialFolderPathA");
		Attach(g_getSpecialFolderPathW, &SHGetSpecialFolderPathWHook, L"shell32.dll", "SHGetSpecialFolderPathW");
		const LONG error = DetourTransactionCommit();
		if (error != NO_ERROR)
			Fatal("DetourTransactionCommit failed: " + std::to_string(error));
	}

	std::wstring Environment(const wchar_t* a_name)
	{
		const DWORD size = GetEnvironmentVariableW(a_name, nullptr, 0);
		if (!size)
			return {};
		std::wstring value(size, L'\0');
		const DWORD length = GetEnvironmentVariableW(a_name, value.data(), size);
		value.resize(length);
		return value;
	}

	void OnMessage(SKSEMessage* a_message)
	{
		// kPostLoad: every plugin's SKSEPlugin_Load has returned, so each CommonLib has filled its Address Library table.
		if (a_message && a_message->type == kMessagePostLoad) {
			WriteMarker("plugins-loaded");
			Log("plugins loaded");
		}
		if (a_message && a_message->type == kMessageDataLoaded) {
			WriteMarker("data-loaded");
			Log("data loaded; " + std::to_string(g_answered.load()) + " folder requests answered with the home so far");
		}
	}
}

extern "C"
{
	// skse64's SKSEPluginVersionData (kVersion 1).
	struct SKSEPluginVersionData
	{
		std::uint32_t dataVersion;
		std::uint32_t pluginVersion;
		char name[256];
		char author[256];
		char supportEmail[252];
		std::uint32_t versionIndependenceEx;
		std::uint32_t versionIndependence;
		std::uint32_t compatibleVersions[16];
		std::uint32_t seVersionRequired;
	};

	// No game addresses and no game structures: it runs on any runtime.
	__declspec(dllexport) constinit SKSEPluginVersionData SKSEPlugin_Version = {
		1,                // dataVersion
		1,                // pluginVersion
		"CSTestLab",      // name
		"cs-test",        // author
		"",               // supportEmail
		1 << 0,           // versionIndependenceEx: kVersionIndependentEx_NoStructUse
		1 << 1,           // versionIndependence: kVersionIndependent_Signatures
		{ 0 },            // compatibleVersions
		0,                // seVersionRequired
	};

	__declspec(dllexport) bool SKSEPlugin_Load(const SKSEInterface* a_skse)
	{
		g_home = Environment(L"CS_TEST_LAB_HOME");
		if (g_home.empty())
			Fatal("CS_TEST_LAB_HOME is not set: a lab slot is started by cs-test, which names its home");
		while (!g_home.empty() && (g_home.back() == L'\\' || g_home.back() == L'/'))
			g_home.pop_back();
		if (g_home.size() < 3 || g_home[1] != L':' || g_home[2] != L'\\')
			Fatal("CS_TEST_LAB_HOME is not an absolute path: " + Narrow(g_home));
		g_documents = g_home + L"\\Documents";
		g_localAppData = g_home + L"\\AppData\\Local";
		const std::wstring skse = g_documents + L"\\My Games\\Skyrim Special Edition\\SKSE";
		for (const std::wstring& folder : { skse, g_localAppData }) {
			const int result = SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
			if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS)
				Fatal("could not create " + Narrow(folder));
		}
		g_logPath = skse + L"\\CSTestLab.log";
		InstallHooks();
		std::string hooks;
		for (const auto& name : g_hooked)
			hooks += (hooks.empty() ? "" : ", ") + name;
		Log("CSTestLab in process " + std::to_string(GetCurrentProcessId()) + ": home " + Narrow(g_home) + "; detoured " + hooks);
		WriteMarker("loaded");
		auto* messaging = static_cast<SKSEMessagingInterface*>(a_skse->QueryInterface(kInterfaceMessaging));
		if (!messaging || !messaging->RegisterListener(a_skse->GetPluginHandle(), "SKSE", &OnMessage))
			Fatal("could not listen for SKSE's messages");
		return true;
	}
}
