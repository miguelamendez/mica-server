#pragma once
#include <functional>
#include <string>
bool microphone_start(const std::string& path, std::string& error);
void microphone_stop();
bool shortcut_start(std::function<void()> callback, std::string& error);
void shortcut_stop();
long focused_application();
bool deliver_text(const std::string& text, bool type, long original_focus, std::string& error);
