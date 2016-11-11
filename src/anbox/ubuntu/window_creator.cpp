/*
 * Copyright (C) 2016 Simon Fels <morphis@gravedo.de>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "anbox/ubuntu/window_creator.h"
#include "anbox/ubuntu/window.h"
#include "anbox/ubuntu/keycode_converter.h"
#include "anbox/input/manager.h"
#include "anbox/input/device.h"
#include "anbox/logger.h"

#include <boost/throw_exception.hpp>

#include <sys/types.h>
#include <signal.h>

namespace anbox {
namespace ubuntu {
WindowCreator::WindowCreator(const std::shared_ptr<input::Manager> &input_manager) :
    graphics::WindowCreator(input_manager),
    input_manager_(input_manager),
    event_thread_running_(false),
    display_info_({1920, 1080}) {

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) < 0)
        BOOST_THROW_EXCEPTION(std::runtime_error("Failed to initialize SDL"));

    event_thread_ = std::thread(&WindowCreator::process_events, this);

    SDL_DisplayMode display_mode;
    // FIXME statically just check the first (primary) display for its mode;
    // once we get multi-monitor support we need to do this better.
    if (SDL_GetCurrentDisplayMode(0, &display_mode) == 0) {
        display_info_.horizontal_resolution = display_mode.w;
        display_info_.vertical_resolution = display_mode.h;
    }

    pointer_ = input_manager->create_device();
    pointer_->set_name("anbox-pointer");
    pointer_->set_driver_version(1);
    pointer_->set_input_id({ BUS_VIRTUAL, 2, 2, 2 });
    pointer_->set_physical_location("none");
    pointer_->set_key_bit(BTN_MOUSE);
    // NOTE: We don't use REL_X/REL_Y in reality but have to specify them here
    // to allow InputFlinger to detect we're a cursor device.
    pointer_->set_rel_bit(REL_X);
    pointer_->set_rel_bit(REL_Y);
    pointer_->set_rel_bit(REL_HWHEEL);
    pointer_->set_rel_bit(REL_WHEEL);
    pointer_->set_prop_bit(INPUT_PROP_POINTER);

    keyboard_ = input_manager->create_device();
    keyboard_->set_name("anbox-keyboard");
    keyboard_->set_driver_version(1);
    keyboard_->set_input_id({ BUS_VIRTUAL, 3, 3, 3 });
    keyboard_->set_physical_location("none");
    keyboard_->set_key_bit(BTN_MISC);
    keyboard_->set_key_bit(KEY_OK);
}

WindowCreator::~WindowCreator() {
    event_thread_running_ = false;
    event_thread_.join();
}

void WindowCreator::process_events() {
    event_thread_running_ = true;

    while(event_thread_running_) {
        SDL_Event event;
        while (SDL_WaitEventTimeout(&event, 100)) {
            switch (event.type) {
            case SDL_QUIT:
                // This is the best way to reliable terminate the whole application for now. It will
                // trigger a correct shutdown in the main part.
                ::kill(getpid(), SIGTERM);
                break;
            case SDL_WINDOWEVENT:
                for (auto &iter : windows_) {
                    if (iter.second->id() == event.window.windowID) {
                        iter.second->process_event(event);
                        break;
                    }
                }
                break;
            case SDL_MOUSEMOTION:
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
            case SDL_MOUSEWHEEL:
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                process_input_event(event);
                break;
            }
        }
    }
}

void WindowCreator::process_input_event(const SDL_Event &event) {
    std::vector<input::Event> mouse_events;
    std::vector<input::Event> keyboard_events;

    std::int32_t x = 0;
    std::int32_t y = 0;
    SDL_Window *window = nullptr;

    switch (event.type) {
    case SDL_MOUSEBUTTONDOWN:
        mouse_events.push_back({ EV_KEY, BTN_LEFT, 1 });
        mouse_events.push_back({ EV_SYN, SYN_REPORT, 0 });
        break;
    case SDL_MOUSEBUTTONUP:
        mouse_events.push_back({ EV_KEY, BTN_LEFT, 0 });
        mouse_events.push_back({ EV_SYN, SYN_REPORT, 0 });
        break;
    case SDL_MOUSEMOTION:
        // As we get only absolute coordindates relative to our window we have to
        // calculate the correct position based on the current focused window
        window = SDL_GetWindowFromID(event.window.windowID);
        if (!window)
            break;

        SDL_GetWindowPosition(window, &x, &y);
        x += event.motion.x;
        y += event.motion.y;

        // NOTE: Sending relative move events doesn't really work and we have changes
        // in libinputflinger to take ABS_X/ABS_Y instead for absolute position events.
        mouse_events.push_back({ EV_ABS, ABS_X, x });
        mouse_events.push_back({ EV_ABS, ABS_Y, y });
        // We're sending relative position updates here too but they will be only used
        // by the Android side EventHub/InputReader to determine if the cursor was
        // moved. They are not used to find out the exact position.
        mouse_events.push_back({ EV_REL, REL_X, event.motion.xrel });
        mouse_events.push_back({ EV_REL, REL_Y, event.motion.yrel });
        mouse_events.push_back({ EV_SYN, SYN_REPORT, 0 });
        break;
    case SDL_MOUSEWHEEL:
        mouse_events.push_back({ EV_REL, REL_WHEEL, static_cast<std::int32_t>(event.wheel.y) });
        break;
    case SDL_KEYDOWN: {
        const auto code = KeycodeConverter::convert(event.key.keysym.scancode);
        if (code == KEY_RESERVED)
            break;
        keyboard_events.push_back({ EV_KEY, code, 1 });
        break;
    }
    case SDL_KEYUP: {
        const auto code = KeycodeConverter::convert(event.key.keysym.scancode);
        if (code == KEY_RESERVED)
            break;
        keyboard_events.push_back({ EV_KEY, code, 0 });
        break;
    }
    default:
        break;
    }

    if (mouse_events.size() > 0)
        pointer_->send_events(mouse_events);

    if (keyboard_events.size() > 0)
        keyboard_->send_events(keyboard_events);
}

EGLNativeWindowType WindowCreator::create_window(int x, int y, int width, int height)
try {
    auto window = std::make_shared<Window>(x, y, width, height);
    if (not window)
        BOOST_THROW_EXCEPTION(std::bad_alloc());

    windows_.insert({window->native_window(), window});

    return window->native_window();
}
catch (std::exception &err) {
    DEBUG("Failed to create window: %s", err.what());
    return 0;
}

void WindowCreator::update_window(EGLNativeWindowType win, int x, int y, int width, int height) {
    auto iter = windows_.find(win);
    if (iter == windows_.end())
        return;

    iter->second->resize(width, height);
    iter->second->update_position(x, y);
}

void WindowCreator::destroy_window(EGLNativeWindowType win) {
    auto iter = windows_.find(win);
    if (iter == windows_.end())
        return;

    windows_.erase(iter);
}

WindowCreator::DisplayInfo WindowCreator::display_info() const {
    return display_info_;
}

EGLNativeDisplayType WindowCreator::native_display() const {
    return  0;
}
} // namespace bridge
} // namespace anbox
