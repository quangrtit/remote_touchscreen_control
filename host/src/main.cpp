#include "app/host_options.h"
#include "app/remote_touch_host.h"

#include <Windows.h>

#include <exception>
#include <iostream>

namespace {

HANDLE stop_event = nullptr;

BOOL WINAPI handle_console_event(const DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ||
        event == CTRL_CLOSE_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        if (stop_event) {
            SetEvent(stop_event);
        }
        return TRUE;
    }
    return FALSE;
}

void enable_dpi_awareness() noexcept {
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        (void)SetProcessDPIAware();
    }
}

}  // namespace

int main(const int argc, char* argv[]) {
    try {
        const auto options = remote_touch::app::parse_options(argc, argv);
        if (options.show_help) {
            std::cout << remote_touch::app::usage_text();
            return 0;
        }

        enable_dpi_awareness();
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_event || !SetConsoleCtrlHandler(handle_console_event, TRUE)) {
            throw std::runtime_error("Unable to install shutdown handler");
        }

        remote_touch::app::RemoteTouchHost host{options};
        host.start();
        (void)WaitForSingleObject(stop_event, INFINITE);
        host.stop();

        SetConsoleCtrlHandler(handle_console_event, FALSE);
        CloseHandle(stop_event);
        stop_event = nullptr;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RemoteTouchHost: " << error.what() << '\n'
                  << remote_touch::app::usage_text();
        if (stop_event) {
            CloseHandle(stop_event);
            stop_event = nullptr;
        }
        return 1;
    }
}
