#include "pcsx2/PrecompiledHeader.h"

#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.ViewManagement.Core.h>
#include <winrt/Windows.Graphics.Display.Core.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.System.h>

#include <gamingdeviceinformation.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <sstream>

#include "fmt/core.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Exceptions.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"

#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/LogSink.h"
#include "pcsx2/GS.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/PAD/Host/PAD.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/VMManager.h"

#include "pcsx2/GameList.h"

#ifdef ENABLE_ACHIEVEMENTS
#include "pcsx2/Achievements.h"
#endif
#include "3rdparty/imgui/include/imgui.h"

using namespace winrt;

using namespace Windows;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Graphics::Display::Core;
using namespace Windows::Foundation::Numerics;
using namespace Windows::UI;
using namespace Windows::UI::Core;
using namespace Windows::UI::Composition;

static winrt::Windows::UI::Core::CoreWindow* s_corewind = NULL;
static std::mutex m_event_mutex;
static std::deque<std::function<void()>> m_event_queue;
static bool s_running = true;
static std::thread s_gamescanner_thread;

namespace WinRTHost
{
	static bool InitializeConfig();
	static std::optional<WindowInfo> GetPlatformWindowInfo();
	static void ProcessEventQueue();
} // namespace GSRunner

static std::unique_ptr<INISettingsInterface> s_settings_interface;
alignas(16) static SysMtgsThread s_mtgs_thread;

const IConsoleWriter* PatchesCon = &Console;
BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

bool WinRTHost::InitializeConfig()
{
	if (!EmuFolders::InitializeCriticalFolders())
		return false;

	const std::string path(Path::Combine(EmuFolders::Settings, "PCSX2.ini"));
	Console.WriteLn("Loading config from %s.", path.c_str());
	s_settings_interface = std::make_unique<INISettingsInterface>(std::move(path));
	Host::Internal::SetBaseSettingsLayer(s_settings_interface.get());

	if (!s_settings_interface->Load() || !VMManager::Internal::CheckSettingsVersion())
	{
		VMManager::SetDefaultSettings(*s_settings_interface, true, true, true, true, true);

		auto lock = Host::GetSettingsLock();
		if (!s_settings_interface->Save())
			Console.Error("Failed to save settings.");
	}

	VMManager::Internal::LoadStartupSettings();
	return true;
}

void Host::CommitBaseSettingChanges()
{
	auto lock = Host::GetSettingsLock();
	if (!s_settings_interface->Save())
		Console.Error("Failed to save settings.");
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	{
		auto lock = Host::GetSettingsLock();
		VMManager::SetDefaultSettings(*s_settings_interface.get(), folders, core, controllers, hotkeys, ui);
	}

	Host::CommitBaseSettingChanges();

	return true;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
	// nothing
}

std::optional<std::vector<u8>> Host::ReadResourceFile(const char* filename)
{
	const std::string path(Path::Combine(EmuFolders::Resources, filename));
	std::optional<std::vector<u8>> ret(FileSystem::ReadBinaryFile(path.c_str()));
	if (!ret.has_value())
		Console.Error("Failed to read resource file '%s'", filename);
	return ret;
}

std::optional<std::string> Host::ReadResourceFileToString(const char* filename)
{
	const std::string path(Path::Combine(EmuFolders::Resources, filename));
	std::optional<std::string> ret(FileSystem::ReadFileToString(path.c_str()));
	if (!ret.has_value())
		Console.Error("Failed to read resource file to string '%s'", filename);
	return ret;
}

std::optional<std::time_t> Host::GetResourceFileTimestamp(const char* filename)
{
	const std::string path(Path::Combine(EmuFolders::Resources, filename));
	FILESYSTEM_STAT_DATA sd;
	if (!FileSystem::StatFile(filename, &sd))
		return std::nullopt;

	return sd.ModificationTime;
}

void Host::ReportErrorAsync(const std::string_view& title, const std::string_view& message)
{
	if (!title.empty() && !message.empty())
	{
		Console.Error(
			"ReportErrorAsync: %.*s: %.*s", static_cast<int>(title.size()), title.data(), static_cast<int>(message.size()), message.data());
	}
	else if (!message.empty())
	{
		Console.Error("ReportErrorAsync: %.*s", static_cast<int>(message.size()), message.data());
	}
}

bool Host::ConfirmMessage(const std::string_view& title, const std::string_view& message)
{
	if (!title.empty() && !message.empty())
	{
		Console.Error(
			"ConfirmMessage: %.*s: %.*s", static_cast<int>(title.size()), title.data(), static_cast<int>(message.size()), message.data());
	}
	else if (!message.empty())
	{
		Console.Error("ConfirmMessage: %.*s", static_cast<int>(message.size()), message.data());
	}

	return true;
}

void Host::OpenURL(const std::string_view& url)
{
	winrt::Windows::Foundation::Uri m_uri { winrt::to_hstring(url) };
	auto asyncOperation = winrt::Windows::System::Launcher::LaunchUriAsync(m_uri);
	asyncOperation.Completed([](winrt::Windows::Foundation::IAsyncOperation<bool> const& sender,
								 winrt::Windows::Foundation::AsyncStatus const asyncStatus) {
		return;
	});
}

bool Host::CopyTextToClipboard(const std::string_view& text)
{
	return false;
}

void Host::BeginTextInput()
{
	winrt::Windows::UI::ViewManagement::Core::CoreInputView::GetForCurrentView().TryShowPrimaryView();
}

void Host::EndTextInput()
{
	winrt::Windows::UI::ViewManagement::Core::CoreInputView::GetForCurrentView().TryHide();
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	return WinRTHost::GetPlatformWindowInfo();
}

void Host::OnInputDeviceConnected(const std::string_view& identifier, const std::string_view& device_name)
{
	Host::AddKeyedOSDMessage(fmt::format("{} connected.", identifier), fmt::format("{} connected.", identifier), 5.0f);
}

void Host::OnInputDeviceDisconnected(const std::string_view& identifier)
{
	Host::AddKeyedOSDMessage(fmt::format("{} connected.", identifier), fmt::format("{} disconnected.", identifier), 5.0f);
}

void Host::SetRelativeMouseMode(bool enabled)
{
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	return WinRTHost::GetPlatformWindowInfo();
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
	VMManager::Internal::VSyncOnCPUThread;
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::OnVMStarting()
{
}

void Host::OnVMStarted()
{
}

void Host::OnVMDestroyed()
{
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
}

void Host::OnGameChanged(const std::string& disc_path, const std::string& elf_override, const std::string& game_serial,
	const std::string& game_name, u32 game_crc)
{
}

void Host::OnPerformanceMetricsUpdated()
{
}

void Host::OnSaveStateLoading(const std::string_view& filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view& filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view& filename)
{
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	std::unique_lock<std::mutex> lk(m_event_mutex);
	m_event_queue.push_back(function);
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
	s_gamescanner_thread = std::thread([invalidate_cache]() {
		GameList::Refresh(invalidate_cache, false);
	});
}

void Host::CancelGameListRefresh()
{
}

bool Host::IsFullscreen()
{
	return false;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::RequestExit(bool allow_confirm)
{
	s_running = false;
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	VMManager::Shutdown(allow_save_state && default_save_state);
}

#ifdef ENABLE_ACHIEVEMENTS
void Host::OnAchievementsRefreshed()
{
}
#endif

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view& str)
{
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return std::nullopt;
}

SysMtgsThread& GetMTGS()
{
	return s_mtgs_thread;
}

std::optional<WindowInfo> WinRTHost::GetPlatformWindowInfo()
{
	WindowInfo wi;

	if (s_corewind)
	{
		u32 width = 1920, height = 1080;
		float scale = 1.0;
		GAMING_DEVICE_MODEL_INFORMATION info = {};
		GetGamingDeviceModelInformation(&info);
		if (info.vendorId == GAMING_DEVICE_VENDOR_ID_MICROSOFT)
		{
			HdmiDisplayInformation hdi = HdmiDisplayInformation::GetForCurrentView();
			if (hdi)
			{
				width = hdi.GetCurrentDisplayMode().ResolutionWidthInRawPixels();
				height = hdi.GetCurrentDisplayMode().ResolutionHeightInRawPixels();
				// Our UI is based on 1080p, and we're adding a modifier to zoom in by 80%
				scale = ((float) width / 1920.0f) * 1.8f;
			}
		}

		wi.surface_width = width;
		wi.surface_height = height;
		wi.surface_scale = 1.0f;
		wi.type = WindowInfo::Type::Win32;
		wi.surface_handle = reinterpret_cast<void*>(winrt::get_abi(*s_corewind));
	}
	else
	{
		wi.type = WindowInfo::Type::Surfaceless;
	}

	return wi;
}

void Host::VSyncOnCPUThread()
{
	WinRTHost::ProcessEventQueue();
}

void WinRTHost::ProcessEventQueue() {
	if (!m_event_queue.empty())
	{
		std::unique_lock lk(m_event_mutex);
		while (!m_event_queue.empty())
		{
			m_event_queue.front()();
			m_event_queue.pop_front();
		}
	}
}

struct App : implements<App, IFrameworkViewSource, IFrameworkView>
{
	std::thread m_cpu_thread;
	winrt::hstring m_launchOnExit;
	std::string m_bootPath;

    IFrameworkView CreateView()
    {
        return *this;
    }

    void Initialize(CoreApplicationView const & v)
    {
		v.Activated({this, &App::OnActivate});

		namespace WGI = winrt::Windows::Gaming::Input;

		if (!WinRTHost::InitializeConfig())
		{
			Console.Error("Failed to initialize config.");
			return;
		}

		try
		{
			WGI::RawGameController::RawGameControllerAdded(
				[](auto&&, const WGI::RawGameController raw_game_controller) {
					m_event_queue.push_back([]() {
						InputManager::ReloadDevices();
					});
				});

			WGI::RawGameController::RawGameControllerRemoved(
				[](auto&&, const WGI::RawGameController raw_game_controller) {
					m_event_queue.push_back([]() {
						InputManager::ReloadDevices();
					});
				});
		}
		catch (winrt::hresult_error)
		{
		}
	}

	  void OnActivate(const winrt::Windows::ApplicationModel::Core::CoreApplicationView&,
		const winrt::Windows::ApplicationModel::Activation::IActivatedEventArgs& args)
	{
		std::stringstream filePath;

		if (args.Kind() == Windows::ApplicationModel::Activation::ActivationKind::Protocol)
		{
			auto protocolActivatedEventArgs{
				args.as<Windows::ApplicationModel::Activation::ProtocolActivatedEventArgs>()};
			auto query = protocolActivatedEventArgs.Uri().QueryParsed();

			for (uint32_t i = 0; i < query.Size(); i++)
			{
				auto arg = query.GetAt(i);

				// parse command line string
				if (arg.Name() == winrt::hstring(L"cmd"))
				{
					std::string argVal = winrt::to_string(arg.Value());

					// Strip the executable from the cmd argument
					if (argVal.rfind("xbsx2.exe", 0) == 0)
					{
						argVal = argVal.substr(10, argVal.length());
					}

					std::istringstream iss(argVal);
					std::string s;

					// Maintain slashes while reading the quotes
					while (iss >> std::quoted(s, '"', (char)0))
					{
						filePath << s;
					}
				}
				else if (arg.Name() == winrt::hstring(L"launchOnExit"))
				{
					// For if we want to return to a frontend
					m_launchOnExit = arg.Value();
				}
			}
		}

		std::string gamePath = filePath.str();
		if (!gamePath.empty() && gamePath != "")
		{
			std::unique_lock<std::mutex> lk(m_event_mutex);
			m_event_queue.push_back([gamePath]() {
				VMBootParameters params{};
				params.filename = gamePath;
				params.source_type = CDVD_SourceType::Iso;

				if (VMManager::HasValidVM()) {
					return;
				}

				if (!VMManager::Initialize(std::move(params))) {
					return;
				}

				VMManager::SetState(VMState::Running);

				GetMTGS().WaitForOpen();
				InputManager::ReloadDevices();
			});
		}
	}

	void Load(hstring const&)
	{
	}

	void Uninitialize()
	{
	}

	void Run()
	{
		CoreWindow window = CoreWindow::GetForCurrentThread();
		window.Activate();
		s_corewind = &window;

		auto navigation = winrt::Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
		navigation.BackRequested(
			[](const winrt::Windows::Foundation::IInspectable&,
				const winrt::Windows::UI::Core::BackRequestedEventArgs& args) { args.Handled(true); });

		VMManager::Internal::CPUThreadInitialize();

		WinRTHost::ProcessEventQueue();
		if (VMManager::GetState() != VMState::Running)
		{
			GameList::Refresh(false);
			ImGuiManager::InitializeFullscreenUI();

			GetMTGS().WaitForOpen();
		}

		window.Dispatcher().RunAsync(CoreDispatcherPriority::Normal, []() {
			Sleep(500);
			InputManager::ReloadDevices();
		});

		while (s_running)
		{
			window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);	

			if (VMManager::HasValidVM())
			{
				switch (VMManager::GetState())
				{
					case VMState::Initializing:
						pxFailRel("Shouldn't be in the starting state state");
						break;

					case VMState::Paused:
						InputManager::PollSources();
						WinRTHost::ProcessEventQueue();
						break;

					case VMState::Running:
						VMManager::Execute();
						break;

					case VMState::Resetting:
						VMManager::Reset();
						break;

					case VMState::Stopping:
						WinRTHost::ProcessEventQueue();
						return;

					default:
						break;
				}
			}
			else
			{
				WinRTHost::ProcessEventQueue();
				InputManager::PollSources();
			}

			Sleep(1);
		}

		if (!m_launchOnExit.empty())
		{
			winrt::Windows::Foundation::Uri m_uri{m_launchOnExit};
			auto asyncOperation = winrt::Windows::System::Launcher::LaunchUriAsync(m_uri);
			asyncOperation.Completed([](winrt::Windows::Foundation::IAsyncOperation<bool> const& sender,
										 winrt::Windows::Foundation::AsyncStatus const asyncStatus) {
				VMManager::Internal::CPUThreadShutdown();
				CoreApplication::Exit();
				return;
			});
		}
		else
		{
			VMManager::Internal::CPUThreadShutdown();
			CoreApplication::Exit();
		}
	}

	void SetWindow(CoreWindow const& window)
	{
		window.CharacterReceived({this, &App::OnKeyInput});
	}

	void OnKeyInput(const IInspectable&, const winrt::Windows::UI::Core::CharacterReceivedEventArgs& args) {
		if (ImGuiManager::WantsTextInput())
		{
			GetMTGS().RunOnGSThread([c = args.KeyCode()]() {
				if (!ImGui::GetCurrentContext())
					return;

				ImGui::GetIO().AddInputCharacter(c);
			});
		}
	}
};

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
	winrt::init_apartment();

	CoreApplication::Run(make<App>());

	winrt::uninit_apartment();
}